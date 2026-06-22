/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * script_validate_stage — implementation. See jobs/script_validate_stage.h.
 *
 * Consumes body_persist_log and replays script verification over block
 * bodies already proven readable by body_persist. It writes
 * script_validate_log plus its stage cursor in progress.kv. */

#include "platform/time_compat.h"
#include "jobs/script_validate_stage.h"
#include "jobs/stage_helpers.h"
#include "script_validate_log_store.h"

#include "chain/chain.h"
#include "chain/chainparams.h"
#include "consensus/upgrades.h"
#include "coins/coins.h"
#include "core/uint256.h"
#include "event/event.h"
#include "json/json.h"
#include "primitives/block.h"
#include "script/interpreter.h"
#include "script/script_error.h"
#include "script/script_flags.h"
#include "storage/disk_block_io.h"
#include "storage/event_log.h"
#include "storage/event_log_payloads.h"
#include "storage/event_log_singleton.h"
#include "jobs/block_header_emit.h"
#include "jobs/created_outputs_index.h"
#include "jobs/mint_skip_crypto.h"
#include "jobs/script_validate_contextual.h"
#include "script/script.h"
#include "storage/progress_store.h"
#include "storage/coins_kv.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/stage.h"
#include "util/util.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "validation/sighash.h"
#include "validation/tx_verifier.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STAGE_NAME "script_validate"

/* struct body_persist_row + the script_validate_log schema/migrations/read/
 * write helpers live in script_validate_log_store.c (pure sqlite kernel
 * helpers below the AR layer). */

struct validate_summary {
    int ok;
    int internal_error;       /* umbrella: block_decode OR prevout_unresolved */
    size_t tx_count;
    size_t input_count;
    struct uint256 first_failure_txid;
    int first_failure_vin;
    /* ScriptError from verify_script on a genuine script-invalid verdict
     * (SCRIPT_ERR_OK otherwise). Persisted so a bad-signature reject is
     * distinguishable from bad-pubkey / non-canonical-DER. */
    ScriptError first_failure_serror;
    /* Typed reason for the verdict ("" on a clean verify): the specific
     * status token, e.g. "block_decode_failed" or
     * "prevout_unresolved tx=<hex> vin=<n>". Drives the persisted status so
     * zcl_state answers "why is the pipeline stuck" without conflating
     * distinct causes under one "internal_error" bucket. */
    char reason[128];
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct main_state *g_ms = NULL;
static stage_t *g_stage = NULL;
static char g_datadir[2048] = {0};
static script_validate_reader_fn g_reader = NULL;
static void *g_reader_user = NULL;
static script_validate_prevout_fn g_prevout = NULL;
static void *g_prevout_user = NULL;

static _Atomic uint64_t g_verified_total = 0;
static _Atomic uint64_t g_script_invalid_total = 0;
static _Atomic uint64_t g_internal_error_total = 0;
static _Atomic uint64_t g_upstream_failed_total = 0;
static _Atomic uint64_t g_inputs_verified_total = 0;
static _Atomic uint64_t g_inputs_failed_total = 0;
static _Atomic int64_t  g_last_step_unix = 0;
static _Atomic int64_t  g_last_blocked_unix = 0;
static _Atomic int64_t  g_last_advance_height = -1;
static _Atomic uint64_t g_header_event_emit_total = 0;
static _Atomic uint64_t g_header_event_emit_fail_total = 0;

struct created_prevout_view {
    sqlite3 *db;
    int height;
    int frontier;
    bool have_frontier;
};

/* Production prevout resolver (P0 §2.1) — resolves an outpoint to its spent
 * output WITHOUT requiring -txindex, correct at the script_validate frontier
 * (which runs ahead of utxo_apply). Two layers, in order:
 *   (1) the forward creation index body_persist maintains — covers every coin
 *       created in a block this node has persisted (body_persist.cursor is
 *       strictly > script_validate.cursor, so the row is present before needed);
 *   (2) coins_kv (the canonical UTXO set in progress.kv) — covers pre-anchor
 *       coins on a fast-synced node (seeded from the snapshot at boot) and the
 *       reducer-applied live set; a spent output is DELETEd, so a hit here is a
 *       currently-live coin.
 * A genuine miss returns false; the caller then FAILS LOUD with the exact
 * outpoint (never silently passes). The verifier (verify_script) is unchanged. */
static bool created_index_prevout(const struct outpoint *prevout,
                                  struct tx_out *out, void *user)
{
    if (!prevout || !out)
        return false;

    int64_t value = 0;
    unsigned char script[MAX_SCRIPT_SIZE];
    size_t slen = 0;

    const struct created_prevout_view *view = user;
    sqlite3 *db = view && view->db ? view->db : progress_store_db();
    if (!db)
        return false;

    if (view && view->have_frontier) {
        int min_created = view->frontier <= view->height ? view->frontier : 0;
        int created_h = -1;
        if (created_outputs_index_get_bounded(
                db, prevout->hash.data, prevout->n, min_created,
                view->height, &value, script, sizeof(script), &slen,
                &created_h)) {
            if (slen > MAX_SCRIPT_SIZE)
                return false;
            out->value = value;
            script_set(&out->script_pub_key, script, slen);
            return true;
        }

        struct coins c;
        coins_init(&c);
        if (coins_kv_get_coins(db, prevout->hash.data, &c)) {
            bool usable = c.height < view->frontier &&
                          c.height <= view->height &&
                          prevout->n < c.num_vout &&
                          !tx_out_is_null(&c.vout[prevout->n]);
            if (usable) {
                const struct tx_out *src = &c.vout[prevout->n];
                size_t src_len = src->script_pub_key.size;
                if (src_len <= MAX_SCRIPT_SIZE) {
                    out->value = src->value;
                    script_set(&out->script_pub_key,
                               src->script_pub_key.data, src_len);
                    coins_free(&c);
                    return true;
                }
            }
            coins_free(&c);
        }
        return false;
    }

    if (created_outputs_index_get(db, prevout->hash.data, prevout->n,
                                  &value, script, sizeof(script), &slen)) {
        if (slen > MAX_SCRIPT_SIZE)
            return false;  /* never silently truncate a scriptPubKey */
        out->value = value;
        script_set(&out->script_pub_key, script, slen);
        return true;
    }

    if (db && coins_kv_get(db, prevout->hash.data, prevout->n,
                           &value, script, sizeof(script), &slen)) {
        if (slen > MAX_SCRIPT_SIZE)
            return false;
        out->value = value;
        script_set(&out->script_pub_key, script, slen);
        return true;
    }

    return false;
}

static void created_prevout_view_init(struct created_prevout_view *view,
                                      int height)
{
    memset(view, 0, sizeof(*view));
    view->db = progress_store_db();
    view->height = height;
    view->frontier = 0;
    view->have_frontier = false;
    if (!view->db)
        return;

    int32_t frontier = 0;
    bool found = false;
    if (coins_kv_get_applied_height(view->db, &frontier, &found) && found) {
        view->frontier = frontier;
        view->have_frontier = true;
    }
}

static void validate_summary_init(struct validate_summary *s)
{
    memset(s, 0, sizeof(*s));
    s->ok = 1;
    s->first_failure_vin = -1;
    s->first_failure_serror = SCRIPT_ERR_OK;
    s->reason[0] = '\0';
    uint256_set_null(&s->first_failure_txid);
}

static void validate_block_scripts_with_prevout(
    const struct block *blk,
    int height,
    bool count_counters,
    script_validate_prevout_fn override_prevout,
    void *override_user,
    struct validate_summary *out)
{
    validate_summary_init(out);
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

    struct created_prevout_view default_view;
    created_prevout_view_init(&default_view, height);
    script_validate_prevout_fn default_resolver =
        g_prevout ? g_prevout : created_index_prevout;
    void *default_user = g_prevout_user;
    if (default_resolver == created_index_prevout && !default_user)
        default_user = &default_view;

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
            script_validate_prevout_fn resolver =
                override_prevout ? override_prevout : default_resolver;
            void *resolver_user =
                override_prevout ? override_user : default_user;
            if (!resolver(&tx->vin[vi].prevout, &prev, resolver_user)) {
                out->ok = 0;
                out->internal_error = 1;
                if (out->first_failure_vin < 0) {
                    out->first_failure_txid = tx->hash;
                    out->first_failure_vin = (int)vi;
                }
                /* NOTE: this LABELS the prevout-unresolved cause distinctly;
                 * it does NOT change the accept/reject decision (still ok=0)
                 * and does NOT wire the prevout resolver — that is a separate
                 * P0. We only stop conflating it with block_decode_failed. */
                char txhex[65];
                uint256_get_hex(&tx->hash, txhex);
                snprintf(out->reason, sizeof(out->reason),
                         "prevout_unresolved tx=%s vin=%d", txhex, (int)vi);
                LOG_WARN("script_validate",
                         "[script_validate] prevout_unresolved height=%d "
                         "tx=%s vin=%d", height, txhex, (int)vi);
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
                if (count_counters)
                    atomic_fetch_add(&g_inputs_verified_total, 1);
            } else {
                if (count_counters)
                    atomic_fetch_add(&g_inputs_failed_total, 1);
                out->ok = 0;
                if (out->first_failure_vin < 0) {
                    out->first_failure_txid = tx->hash;
                    out->first_failure_vin = (int)vi;
                    out->first_failure_serror = serror;
                }
                /* Genuine script-verification failure — distinct from the
                 * internal/decode/prevout causes. Reason carries the
                 * ScriptError code AND its mapped string; status stays the
                 * stable "script_invalid" token. */
                char txhex[65];
                uint256_get_hex(&tx->hash, txhex);
                const char *estr = ScriptErrorString(serror);
                snprintf(out->reason, sizeof(out->reason),
                         "script_invalid tx=%s vin=%d err=%d (%s)",
                         txhex, (int)vi, (int)serror, estr);
                LOG_WARN("script_validate",
                         "[script_validate] script_invalid height=%d tx=%s "
                         "vin=%d serror=%d (%s)", height, txhex, (int)vi,
                         (int)serror, estr);
                return;
            }
        }
    }
}

static bool dry_run_block_impl(
    const struct block *blk,
    int height,
    script_validate_prevout_fn prevout,
    void *prevout_user,
    struct script_validate_dry_run_report *out)
{
    if (!blk || !out)
        LOG_FAIL("script_validate", "dry_run_block: bad input");

    struct validate_summary summary;
    validate_block_scripts_with_prevout(blk, height, false, prevout,
                                        prevout_user, &summary);

    memset(out, 0, sizeof(*out));
    out->ok = summary.ok != 0;
    out->internal_error = summary.internal_error != 0;
    out->tx_count = summary.tx_count;
    out->input_count = summary.input_count;
    out->first_failure_txid = summary.first_failure_txid;
    out->first_failure_vin = summary.first_failure_vin;
    out->first_failure_serror = summary.first_failure_serror;

    const char *status = "verified";
    if (!summary.ok && summary.internal_error) {
        status = (summary.reason[0] != '\0' &&
                  strncmp(summary.reason, "block_decode_failed", 19) == 0)
                     ? "block_decode_failed"
                     : "prevout_unresolved";
    } else if (!summary.ok) {
        status = "script_invalid";
    }
    snprintf(out->status, sizeof(out->status), "%s", status);
    return true;
}

bool script_validate_stage_dry_run_block(
    const struct block *blk,
    int height,
    struct script_validate_dry_run_report *out)
{
    return dry_run_block_impl(blk, height, NULL, NULL, out);
}

bool script_validate_stage_dry_run_block_with_prevout(
    const struct block *blk,
    int height,
    script_validate_prevout_fn prevout,
    void *prevout_user,
    struct script_validate_dry_run_report *out)
{
    if (!prevout)
        LOG_FAIL("script_validate", "dry_run_block_with_prevout: NULL resolver");
    return dry_run_block_impl(blk, height, prevout, prevout_user, out);
}

static job_result_t step_validate(struct stage_step_ctx *c)
{
    atomic_store(&g_last_step_unix, platform_time_wall_unix());

    struct main_state *ms = g_ms;
    if (!ms) return JOB_IDLE;
    sqlite3 *db = progress_store_db();
    if (!db) return JOB_IDLE;

    int next_h = (int)c->cursor_in;
    if (next_h < 0) return JOB_FATAL;

    uint64_t bp_cursor = stage_cursor_persisted(db, "body_persist",
                                               STAGE_NAME);
    if ((uint64_t)next_h >= bp_cursor) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    struct body_persist_row upstream;
    int found = script_validate_body_persist_log_at(db, next_h, &upstream);
    if (found < 0) return JOB_FATAL;
    if (found == 0) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    if (upstream.ok == 0) {
        if (!script_validate_log_insert(db, next_h, "upstream_failed", false,
                        0, 0, NULL, -1, SCRIPT_ERR_OK, NULL))
            return JOB_FATAL;
        atomic_fetch_add(&g_upstream_failed_total, 1);
        atomic_store(&g_last_advance_height, (int64_t)next_h);
        c->cursor_out = c->cursor_in + 1;
        return JOB_ADVANCED;
    }

    struct block_index *bi = active_chain_at(&ms->chain_active, next_h);
    if (!bi || !(bi->nStatus & BLOCK_HAVE_DATA)) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    struct block blk;
    block_init(&blk);
    if (!stage_read_block(&blk, bi, next_h, g_datadir, g_reader, g_reader_user)) {
        block_free(&blk);
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    /* #26 — at-tip contextual gate (script_validate_contextual.c): per-tx
     * contextual rules / finality / BIP34 before script verification. */
    bool ctx_internal = false;
    switch (script_validate_contextual_gate(ms, db, next_h, bi, &blk,
                                            &ctx_internal)) {
    case SV_CTX_PASS:
        break;
    case SV_CTX_WAIT_PARAMS:
        block_free(&blk);
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    case SV_CTX_REJECTED:
        block_free(&blk);
        if (ctx_internal)
            atomic_fetch_add(&g_internal_error_total, 1);
        atomic_store(&g_last_advance_height, (int64_t)next_h);
        c->cursor_out = c->cursor_in + 1;
        return JOB_ADVANCED;
    case SV_CTX_FATAL:
        block_free(&blk);
        return JOB_FATAL;
    }

    struct validate_summary summary;
    if (mint_skip_crypto_get()) {
        /* OFFLINE FAST-MINT pass-through (jobs/mint_skip_crypto.h): SKIP the
         * per-input ECDSA verify_script loop and synthesize the SAME "verified"
         * summary the clean path produces (the contextual gate above still ran;
         * the coin SET is unchanged — utxo_apply is the state transition). The
         * terminal SHA3==checkpoint hard-assert certifies it. Default OFF → a
         * normal boot never reaches this branch. */
        validate_summary_init(&summary);   /* ok=1, internal_error=0 */
        for (size_t ti = 0; ti < blk.num_vtx; ti++) {
            const struct transaction *tx = &blk.vtx[ti];
            summary.tx_count++;
            if (!transaction_is_coinbase(tx))
                summary.input_count += tx->num_vin;
        }
    } else {
        validate_block_scripts_with_prevout(&blk, next_h, true, NULL, NULL,
                                            &summary);
    }
    block_free(&blk);

    const char *status = "verified";
    bool ok = true;
    const struct uint256 *fail_txid = NULL;
    int fail_vin = -1;
    ScriptError fail_serror = SCRIPT_ERR_OK;
    if (!summary.ok && summary.internal_error) {
        /* Persist the TYPED cause token ("block_decode_failed" or
         * "prevout_unresolved"), not the generic "internal_error". The
         * txid/vin ride in the first_failure_* columns; dump_state composes
         * them back. g_internal_error_total is the umbrella counter. */
        status = (summary.reason[0] != '\0' &&
                  strncmp(summary.reason, "block_decode_failed", 19) == 0)
                     ? "block_decode_failed"
                     : "prevout_unresolved";
        ok = false;
        fail_txid = &summary.first_failure_txid;
        fail_vin = summary.first_failure_vin;
        atomic_fetch_add(&g_internal_error_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "script_validate %s height=%d source=%s reason=%s",
                    status, next_h, upstream.source,
                    summary.reason[0] ? summary.reason : status);
    } else if (!summary.ok) {
        status = "script_invalid";
        ok = false;
        fail_txid = &summary.first_failure_txid;
        fail_vin = summary.first_failure_vin;
        fail_serror = summary.first_failure_serror;
        atomic_fetch_add(&g_script_invalid_total, 1);
        /* Carry ScriptError code + mapped string + txid + vin into the reject
         * event so a bad-signature block is distinguishable downstream. */
        char ev_txhex[65];
        uint256_get_hex(fail_txid, ev_txhex);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "script_validate script_invalid height=%d tx=%s vin=%d "
                    "err=%d (%s) reason=%s",
                    next_h, ev_txhex, fail_vin, (int)fail_serror,
                    ScriptErrorString(fail_serror),
                    summary.reason[0] ? summary.reason : "script_invalid");
    } else {
        atomic_fetch_add(&g_verified_total, 1);
        /* Raise the in-memory validity level to BLOCK_VALID_SCRIPTS, which
         * tip_finalize.preconditions_ok requires before publication. This is
         * a validity LEVEL stored in the low BLOCK_VALID_MASK bits, not an
         * OR-able flag, so clear the mask before setting it. Re-emit
         * EV_BLOCK_HEADER so the projection persists the new nStatus across
         * restart. bi is the live in-memory entry from active_chain_at; blk
         * was freed above but bi is independent. */
        bi->nStatus = (bi->nStatus & ~(unsigned)BLOCK_VALID_MASK)
                      | BLOCK_VALID_SCRIPTS;
        block_index_emit_header_event(bi, "script_validate", &g_header_event_emit_total, &g_header_event_emit_fail_total);
    }

    if (!script_validate_log_insert(db, next_h, status, ok, summary.tx_count,
                    summary.input_count, fail_txid, fail_vin, fail_serror,
                    bi->phashBlock))
        return JOB_FATAL;

    atomic_store(&g_last_advance_height, (int64_t)next_h);
    c->cursor_out = c->cursor_in + 1;
    return JOB_ADVANCED;
}

bool script_validate_stage_init(struct main_state *ms)
{
    if (!ms) LOG_FAIL("script_validate", "init: NULL main_state");

    sqlite3 *db = progress_store_db();
    if (!db) LOG_FAIL("script_validate",
        "init: progress_store not open");

    pthread_mutex_lock(&g_lock);
    if (g_stage != NULL) {
        bool same = (g_ms == ms);
        pthread_mutex_unlock(&g_lock);
        if (!same)
            LOG_FAIL("script_validate",
                "init: already bound to a different main_state");
        return true;
    }

    if (!script_validate_log_ensure_schema(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    GetDataDir(true, g_datadir, sizeof(g_datadir));

    stage_t *s = stage_create(STAGE_NAME, step_validate, NULL);
    if (!s) {
        pthread_mutex_unlock(&g_lock);
        LOG_FAIL("script_validate", "init: stage_create failed");
    }

    g_ms = ms;
    g_stage = s;
    /* Wire the production prevout resolver (P0 §2.1) unless a caller (e.g. a
     * test) already installed one. */
    if (!g_prevout)
        g_prevout = created_index_prevout;
    pthread_mutex_unlock(&g_lock);

    LOG_INFO("script_validate", "[script_validate] stage initialised");
    return true;
}

STAGE_STEP_ONCE_SIMPLE(script_validate)

STAGE_DRAIN_IMPL(script_validate)

void script_validate_stage_shutdown(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_stage) {
        stage_destroy(g_stage);
        g_stage = NULL;
    }
    g_ms = NULL;
    g_datadir[0] = '\0';
    g_reader = NULL;
    g_reader_user = NULL;
    g_prevout = NULL;
    g_prevout_user = NULL;
    atomic_store(&g_verified_total, (uint64_t)0);
    atomic_store(&g_script_invalid_total, (uint64_t)0);
    atomic_store(&g_internal_error_total, (uint64_t)0);
    atomic_store(&g_upstream_failed_total, (uint64_t)0);
    script_validate_contextual_counters_reset();
    atomic_store(&g_inputs_verified_total, (uint64_t)0);
    atomic_store(&g_inputs_failed_total, (uint64_t)0);
    atomic_store(&g_last_step_unix, (int64_t)0);
    atomic_store(&g_last_blocked_unix, (int64_t)0);
    atomic_store(&g_last_advance_height, (int64_t)-1);
    atomic_store(&g_header_event_emit_total, (uint64_t)0);
    atomic_store(&g_header_event_emit_fail_total, (uint64_t)0);
    pthread_mutex_unlock(&g_lock);
}

void script_validate_stage_set_reader(script_validate_reader_fn fn,
                                      void *user)
{
    pthread_mutex_lock(&g_lock);
    g_reader = fn;
    g_reader_user = user;
    pthread_mutex_unlock(&g_lock);
}

void script_validate_stage_set_prevout_resolver(script_validate_prevout_fn fn,
                                                void *user)
{
    pthread_mutex_lock(&g_lock);
    g_prevout = fn;
    g_prevout_user = user;
    pthread_mutex_unlock(&g_lock);
}

uint64_t script_validate_stage_cursor(void)
{
    return g_stage ? stage_cursor(g_stage) : 0;
}

uint64_t script_validate_stage_verified_total(void)
{
    return atomic_load(&g_verified_total);
}

uint64_t script_validate_stage_script_invalid_total(void)
{
    return atomic_load(&g_script_invalid_total);
}

uint64_t script_validate_stage_internal_error_total(void)
{
    return atomic_load(&g_internal_error_total);
}

uint64_t script_validate_stage_upstream_failed_total(void)
{
    return atomic_load(&g_upstream_failed_total);
}

uint64_t script_validate_stage_inputs_verified_total(void)
{
    return atomic_load(&g_inputs_verified_total);
}

/* Emit {blocking_height, status, reason, txid, vin} for the lowest logged
 * height with ok=0 — i.e. the first row holding the pipeline back. Reads the
 * status + first_failure_* columns persisted by step_validate and composes
 * the full typed reason (e.g. "prevout_unresolved tx=<hex> vin=<n>"), so
 * zcl_state answers "why is the pipeline stuck" without a separate query.
 * No-op (emits blocking_height=-1) when nothing is blocking. Mirrors the
 * sqlite access already used by body_persist_log_at in this file. */
static void dump_blocking_failure(struct json_value *out, sqlite3 *db)
{
    if (!db) {
        json_push_kv_int(out, "blocking_height", -1);
        return;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT height, status, first_failure_txid, first_failure_vin, "
        "       first_failure_serror "
        "FROM script_validate_log WHERE ok = 0 "
        "ORDER BY height ASC LIMIT 1",
        -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("script_validate",
                 "[script_validate] dump blocking prepare failed: %s",
                 sqlite3_errmsg(db));
        json_push_kv_int(out, "blocking_height", -1);
        return;
    }
    int rc = sqlite3_step(st);  // raw-sql-ok:progress-kv-kernel-store
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        json_push_kv_int(out, "blocking_height", -1);
        return;
    }

    int64_t height = sqlite3_column_int64(st, 0);
    const unsigned char *status_c = sqlite3_column_text(st, 1);
    const char *status = status_c ? (const char *)status_c : "(unknown)";

    char txhex[65] = {0};
    const void *blob = sqlite3_column_blob(st, 2);
    int blob_len = sqlite3_column_bytes(st, 2);
    bool have_txid = (blob && blob_len == 32);
    if (have_txid) {
        struct uint256 txid;
        memcpy(txid.data, blob, 32);
        uint256_get_hex(&txid, txhex);
    }

    int vin = sqlite3_column_type(st, 3) == SQLITE_NULL
                  ? -1 : sqlite3_column_int(st, 3);

    /* ScriptError is persisted only for a script-invalid verdict (NULL on
     * decode/prevout/upstream rows). Map it back to its stable string. */
    bool have_serror = sqlite3_column_type(st, 4) != SQLITE_NULL;
    int serror_code = have_serror ? sqlite3_column_int(st, 4)
                                  : (int)SCRIPT_ERR_OK;
    const char *serror_str =
        have_serror ? ScriptErrorString((ScriptError)serror_code) : "";

    /* Compose the full typed reason from the persisted token + columns. */
    char reason[224];
    if (have_txid && vin >= 0 && have_serror)
        snprintf(reason, sizeof(reason), "%s tx=%s vin=%d err=%d (%s)",
                 status, txhex, vin, serror_code, serror_str);
    else if (have_txid && vin >= 0)
        snprintf(reason, sizeof(reason), "%s tx=%s vin=%d",
                 status, txhex, vin);
    else if (have_txid)
        snprintf(reason, sizeof(reason), "%s tx=%s", status, txhex);
    else
        snprintf(reason, sizeof(reason), "%s", status);

    json_push_kv_int(out, "blocking_height", height);
    json_push_kv_str(out, "blocking_status", status);
    json_push_kv_str(out, "blocking_reason", reason);
    json_push_kv_str(out, "blocking_txid", have_txid ? txhex : "");
    json_push_kv_int(out, "blocking_vin", vin);
    json_push_kv_int(out, "blocking_serror", have_serror ? serror_code : -1);
    json_push_kv_str(out, "blocking_serror_string", serror_str);
    sqlite3_finalize(st);
}

bool script_validate_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    sqlite3 *db = progress_store_db();
    int64_t now = platform_time_wall_unix();
    int64_t last = atomic_load(&g_last_step_unix);

    stage_dump_header(out, STAGE_NAME, g_stage);
    json_push_kv_int (out, "verified_total",
                      (int64_t)atomic_load(&g_verified_total));
    json_push_kv_int (out, "script_invalid_total",
                      (int64_t)atomic_load(&g_script_invalid_total));
    json_push_kv_int (out, "internal_error_total",
                      (int64_t)atomic_load(&g_internal_error_total));
    json_push_kv_int (out, "upstream_failed_total",
                      (int64_t)atomic_load(&g_upstream_failed_total));
    json_push_kv_int (out, "contextual_reject_total",
                      (int64_t)script_validate_contextual_reject_total());
    json_push_kv_int (out, "inputs_verified_total",
                      (int64_t)atomic_load(&g_inputs_verified_total));
    json_push_kv_int (out, "inputs_failed_total",
                      (int64_t)atomic_load(&g_inputs_failed_total));
    json_push_kv_int (out, "header_event_emit_total",
                      (int64_t)atomic_load(&g_header_event_emit_total));
    json_push_kv_int (out, "header_event_emit_fail_total",
                      (int64_t)atomic_load(&g_header_event_emit_fail_total));
    json_push_kv_int (out, "last_advance_height",
                      atomic_load(&g_last_advance_height));
    json_push_kv_int (out, "last_step_unix", last);
    json_push_kv_int (out, "last_step_age_seconds",
                      last > 0 ? now - last : -1);
    json_push_kv_int (out, "last_blocked_unix",
                      atomic_load(&g_last_blocked_unix));
    json_push_kv_int (out, "log_rows",
                      db ? stage_log_row_count(db, STAGE_NAME,
                                               "script_validate_log") : 0);
    /* "Why is the pipeline stuck": surface the lowest ok=0 row's typed
     * reason (status + txid + vin) so zcl_state pinpoints the blocker. */
    dump_blocking_failure(out, db);
    stage_dump_counters(out, g_stage);
    return true;
}
