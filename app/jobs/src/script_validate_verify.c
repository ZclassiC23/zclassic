/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * script_validate_verify — implementation. See jobs/script_validate_verify.h.
 *
 * Serial reference sweep + a pool-parallel sweep that reduces per-input results
 * back in original (tx, vin) order. The two are verdict-identical by
 * construction and pinned by test_validate_parallel_determinism. */

#include "jobs/script_validate_verify.h"
#include "script_validate_stage_internal.h"
#include "validate_work_pool.h"

#include "chain/chainparams.h"
#include "consensus/upgrades.h"
#include "core/uint256.h"
#include "primitives/transaction.h"
#include "script/interpreter.h"
#include "script/script.h"
#include "script/script_error.h"
#include "script/script_flags.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "validation/sighash.h"
#include "validation/tx_verifier.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void script_verify_summary_init(struct script_verify_summary *s)
{
    memset(s, 0, sizeof(*s));
    s->ok = 1;
    s->first_failure_vin = -1;
    s->first_failure_serror = SCRIPT_ERR_OK;
    s->reason[0] = '\0';
    uint256_set_null(&s->first_failure_txid);
}

/* ── shared verdict writers (byte-identical between serial + reduce) ──────── */

static void sv_write_prevout_unresolved(struct script_verify_summary *out,
                                        const struct uint256 *txid,
                                        unsigned int vi, int height)
{
    out->ok = 0;
    out->internal_error = 1;
    if (out->first_failure_vin < 0) {
        out->first_failure_txid = *txid;
        out->first_failure_vin = (int)vi;
    }
    char txhex[65];
    uint256_get_hex(txid, txhex);
    snprintf(out->reason, sizeof(out->reason),
             "prevout_unresolved tx=%s vin=%d", txhex, (int)vi);
    LOG_WARN("script_validate",
             "[script_validate] prevout_unresolved height=%d tx=%s vin=%d",
             height, txhex, (int)vi);
}

static void sv_write_script_invalid(struct script_verify_summary *out,
                                    const struct uint256 *txid,
                                    unsigned int vi, ScriptError serror,
                                    int height)
{
    out->ok = 0;
    if (out->first_failure_vin < 0) {
        out->first_failure_txid = *txid;
        out->first_failure_vin = (int)vi;
        out->first_failure_serror = serror;
    }
    char txhex[65];
    uint256_get_hex(txid, txhex);
    const char *estr = ScriptErrorString(serror);
    snprintf(out->reason, sizeof(out->reason),
             "script_invalid tx=%s vin=%d err=%d (%s)",
             txhex, (int)vi, (int)serror, estr);
    LOG_WARN("script_validate",
             "[script_validate] script_invalid height=%d tx=%s vin=%d "
             "serror=%d (%s)", height, txhex, (int)vi, (int)serror, estr);
}

/* Resolve the effective per-input resolver + user (override wins, else the
 * default, else the created-index resolver seeded with the per-block view). */
struct sv_resolvers {
    script_validate_prevout_fn default_resolver;
    void *default_user;
    script_validate_prevout_fn override_prevout;
    void *override_user;
};

static void sv_resolvers_init(struct sv_resolvers *r,
                              script_validate_prevout_fn resolver_default,
                              void *resolver_default_user,
                              script_validate_prevout_fn override_prevout,
                              void *override_user,
                              struct script_validate_created_prevout_view *view)
{
    r->default_resolver = resolver_default ? resolver_default
                                           : script_validate_created_index_prevout;
    r->default_user = resolver_default_user;
    if (r->default_resolver == script_validate_created_index_prevout &&
        !r->default_user)
        r->default_user = view;
    r->override_prevout = override_prevout;
    r->override_user = override_user;
}

static bool sv_resolve(const struct sv_resolvers *r,
                       const struct outpoint *prevout, struct tx_out *prev)
{
    script_validate_prevout_fn fn =
        r->override_prevout ? r->override_prevout : r->default_resolver;
    void *user = r->override_prevout ? r->override_user : r->default_user;
    return fn(prevout, prev, user);
}

/* ── serial reference sweep (the original in-order loop) ──────────────────── */

static void sv_serial(const struct block *blk, int height, uint32_t flags,
                      uint32_t branch_id, const struct sv_resolvers *r,
                      struct script_verify_summary *out)
{
    for (size_t ti = 0; ti < blk->num_vtx; ti++) {
        const struct transaction *tx = &blk->vtx[ti];
        out->tx_count++;
        if (transaction_is_coinbase(tx))
            continue;

        struct precomputed_tx_data txdata;
        precompute_tx_data(tx, &txdata);

        for (size_t vi = 0; vi < tx->num_vin; vi++) {
            struct tx_out prev;
            tx_out_set_null(&prev);
            if (!sv_resolve(r, &tx->vin[vi].prevout, &prev)) {
                sv_write_prevout_unresolved(out, &tx->hash, (unsigned)vi,
                                            height);
                return;
            }
            struct tx_sig_checker tsc;
            tx_sig_checker_init(&tsc, tx, (unsigned int)vi, prev.value,
                                branch_id, &txdata);
            struct sig_checker checker = tx_make_sig_checker(&tsc);
            ScriptError serror = SCRIPT_ERR_OK;
            out->input_count++;
            bool ok = verify_script(&tx->vin[vi].script_sig,
                                    &prev.script_pub_key, flags, &checker,
                                    branch_id, &serror);
            if (ok) {
                out->inputs_verified++;
            } else {
                out->inputs_failed++;
                sv_write_script_invalid(out, &tx->hash, (unsigned)vi, serror,
                                        height);
                return;
            }
        }
    }
}

/* ── pool-parallel sweep ──────────────────────────────────────────────────── */

struct sv_job {
    const struct transaction *tx;
    const struct precomputed_tx_data *txdata;
    struct tx_out prev;
    struct uint256 txid;
    unsigned int vi;
    int ti;                 /* owning tx index — reproduces serial tx_count */
    bool prevout_ok;        /* set in the serial build phase */
    /* result (only meaningful when prevout_ok) */
    bool verified;
    ScriptError serror;
};

struct sv_ctx {
    uint32_t flags;
    uint32_t branch_id;
};

static void sv_worker(void *j, void *u)
{
    struct sv_job *job = j;
    struct sv_ctx *ctx = u;
    if (!job->prevout_ok)
        return;
    struct tx_sig_checker tsc;
    tx_sig_checker_init(&tsc, job->tx, job->vi, job->prev.value,
                        ctx->branch_id, job->txdata);
    struct sig_checker checker = tx_make_sig_checker(&tsc);
    ScriptError serror = SCRIPT_ERR_OK;
    job->verified = verify_script(&job->tx->vin[job->vi].script_sig,
                                  &job->prev.script_pub_key, ctx->flags,
                                  &checker, ctx->branch_id, &serror);
    job->serror = serror;
}

/* The shared script-verify pool: lazily started, stopped at shutdown. */
static struct vwp_pool g_sv_pool;
static pthread_mutex_t g_sv_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_sv_pool_ready = false;
static bool g_sv_pool_failed = false;

static struct vwp_pool *sv_pool_get(void)
{
    pthread_mutex_lock(&g_sv_pool_lock);
    if (!g_sv_pool_ready && !g_sv_pool_failed) {
        if (vwp_pool_start(&g_sv_pool, sv_worker, 0))
            g_sv_pool_ready = true;
        else
            g_sv_pool_failed = true;
    }
    struct vwp_pool *p = g_sv_pool_ready ? &g_sv_pool : NULL;
    pthread_mutex_unlock(&g_sv_pool_lock);
    return p;
}

void script_verify_pool_shutdown(void)
{
    pthread_mutex_lock(&g_sv_pool_lock);
    if (g_sv_pool_ready) {
        vwp_pool_stop(&g_sv_pool);
        g_sv_pool_ready = false;
    }
    g_sv_pool_failed = false;
    pthread_mutex_unlock(&g_sv_pool_lock);
}

/* Returns false if it could not run in parallel (alloc/pool failure); the
 * caller then falls back to the verdict-identical serial sweep. */
static bool sv_parallel(const struct block *blk, int height, uint32_t flags,
                        uint32_t branch_id, const struct sv_resolvers *r,
                        struct script_verify_summary *out)
{
    size_t n_inputs = 0;
    for (size_t ti = 0; ti < blk->num_vtx; ti++) {
        const struct transaction *tx = &blk->vtx[ti];
        if (!transaction_is_coinbase(tx))
            n_inputs += tx->num_vin;
    }
    if (n_inputs == 0) {
        out->tx_count = blk->num_vtx;      /* no inputs: verdict trivially ok */
        return true;
    }

    struct vwp_pool *pool = sv_pool_get();
    if (!pool)
        return false;

    struct precomputed_tx_data *txdata_arr =
        zcl_calloc(blk->num_vtx, sizeof(*txdata_arr), "sv txdata");
    struct sv_job *jobs = zcl_calloc(n_inputs, sizeof(*jobs), "sv jobs");
    if (!txdata_arr || !jobs) {
        free(txdata_arr);
        free(jobs);
        return false;
    }

    /* Build phase — SERIAL: precompute per-tx data + resolve prevouts in
     * (tx, vin) order so any in-order resolver dependency is preserved. */
    size_t k = 0;
    for (size_t ti = 0; ti < blk->num_vtx; ti++) {
        const struct transaction *tx = &blk->vtx[ti];
        if (transaction_is_coinbase(tx))
            continue;
        precompute_tx_data(tx, &txdata_arr[ti]);
        for (size_t vi = 0; vi < tx->num_vin; vi++) {
            struct sv_job *job = &jobs[k++];
            job->tx = tx;
            job->txid = tx->hash;
            job->vi = (unsigned int)vi;
            job->ti = (int)ti;
            tx_out_set_null(&job->prev);
            if (sv_resolve(r, &tx->vin[vi].prevout, &job->prev)) {
                job->prevout_ok = true;
                job->txdata = &txdata_arr[ti];
            } else {
                job->prevout_ok = false;
            }
        }
    }

    struct sv_ctx ctx = { .flags = flags, .branch_id = branch_id };
    vwp_pool_run_batch(pool, jobs, sizeof(*jobs), (int)n_inputs, &ctx);

    /* Reduce phase — SERIAL, in original order: reproduce the serial verdict,
     * first-failure, and reached-input counts exactly. */
    bool stopped = false;
    for (size_t i = 0; i < n_inputs && !stopped; i++) {
        struct sv_job *job = &jobs[i];
        if (!job->prevout_ok) {
            out->tx_count = (size_t)job->ti + 1;
            sv_write_prevout_unresolved(out, &job->txid, job->vi, height);
            stopped = true;
            break;
        }
        out->input_count++;
        if (job->verified) {
            out->inputs_verified++;
            continue;
        }
        out->inputs_failed++;
        out->tx_count = (size_t)job->ti + 1;
        sv_write_script_invalid(out, &job->txid, job->vi, job->serror, height);
        stopped = true;
    }
    if (!stopped)
        out->tx_count = blk->num_vtx;  /* clean sweep touched every tx */

    free(txdata_arr);
    free(jobs);
    return true;
}

void script_verify_block(const struct block *blk, int height,
                         script_validate_prevout_fn resolver_default,
                         void *resolver_default_user,
                         script_validate_prevout_fn override_prevout,
                         void *override_user,
                         bool parallel,
                         struct script_verify_summary *out)
{
    script_verify_summary_init(out);
    if (!blk) {
        out->ok = 0;
        out->internal_error = 1;
        snprintf(out->reason, sizeof(out->reason), "block_decode_failed");
        LOG_WARN("script_validate",
                 "[script_validate] block_decode_failed height=%d "
                 "(null/undecodable block body)", height);
        return;
    }

    uint32_t flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;
    uint32_t branch_id = consensus_current_epoch_branch_id(
        height, &chain_params_get()->consensus);

    struct script_validate_created_prevout_view view;
    script_validate_created_prevout_view_init(&view, height);
    struct sv_resolvers r;
    sv_resolvers_init(&r, resolver_default, resolver_default_user,
                      override_prevout, override_user, &view);

    if (parallel && sv_parallel(blk, height, flags, branch_id, &r, out))
        return;
    sv_serial(blk, height, flags, branch_id, &r, out);
}
