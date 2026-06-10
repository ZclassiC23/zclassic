/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * proof_validate_stage — implementation. See jobs/proof_validate_stage.h.
 *
 * Consumes script_validate_log and replays shielded proof verification
 * over block bodies that have already passed script_validate. It writes
 * proof_validate_log plus its stage cursor in progress.kv. */

#include "platform/time_compat.h"
#include "jobs/proof_validate_stage.h"
#include "jobs/stage_helpers.h"
#include "proof_validate_log_store.h"

#include "chain/chain.h"
#include "chain/chainparams.h"
#include "consensus/upgrades.h"
#include "crypto/ed25519.h"
#include "core/uint256.h"
#include "event/event.h"
#include "json/json.h"
#include "primitives/block.h"
#include "sapling/bn254.h"
#include "sapling/params_init.h"
#include "sapling/sapling.h"
#include "sapling/sapling_prover.h"
#include "sapling/sprout.h"
#include "script/script.h"
#include "script/sighashtype.h"
#include "storage/disk_block_io.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/stage.h"
#include "util/util.h"
#include "validation/main_state.h"
#include "validation/sighash.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STAGE_NAME "proof_validate"

/* struct script_validate_row + the proof_validate_log schema/read/write
 * helpers live in proof_validate_log_store.c (pure sqlite kernel helpers
 * below the AR layer). */

struct validate_summary {
    int ok;
    int internal_error;
    size_t sapling_spends_total;
    size_t sapling_outputs_total;
    size_t sprout_joinsplits_total;
    struct uint256 first_failure_txid;
    const char *first_failure_proof_type;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct main_state *g_ms = NULL;
static stage_t *g_stage = NULL;
static char g_datadir[2048] = {0};
static proof_validate_reader_fn g_reader = NULL;
static void *g_reader_user = NULL;
static proof_validate_tx_verify_fn g_tx_verifier = NULL;
static void *g_tx_verifier_user = NULL;

static _Atomic uint64_t g_verified_total = 0;
static _Atomic uint64_t g_proof_invalid_total = 0;
static _Atomic uint64_t g_internal_error_total = 0;
static _Atomic uint64_t g_upstream_failed_total = 0;
static _Atomic uint64_t g_sapling_spends_verified_total = 0;
static _Atomic uint64_t g_sapling_spends_failed_total = 0;
static _Atomic uint64_t g_sapling_outputs_verified_total = 0;
static _Atomic uint64_t g_sapling_outputs_failed_total = 0;
static _Atomic uint64_t g_sprout_groth16_verified_total = 0;
static _Atomic uint64_t g_sprout_groth16_failed_total = 0;
static _Atomic uint64_t g_sprout_phgr13_verified_total = 0;
static _Atomic uint64_t g_sprout_phgr13_failed_total = 0;
static _Atomic uint64_t g_binding_sig_verified_total = 0;
static _Atomic uint64_t g_binding_sig_failed_total = 0;
static _Atomic int64_t  g_last_step_unix = 0;
static _Atomic int64_t  g_last_blocked_unix = 0;
static _Atomic int64_t  g_last_advance_height = -1;

static void tx_report_init(struct proof_validate_tx_report *r)
{
    memset(r, 0, sizeof(*r));
    r->ok = true;
}

static bool tx_has_shielded_proofs(const struct transaction *tx)
{
    return tx && (tx->num_joinsplit > 0 ||
                  tx->num_shielded_spend > 0 ||
                  tx->num_shielded_output > 0);
}

static bool default_verify_tx(const struct transaction *tx, int height,
                              struct proof_validate_tx_report *out,
                              void *user)
{
    (void)user;
    tx_report_init(out);
    if (!tx) {
        out->ok = false;
        out->internal_error = true;
        out->first_failure_proof_type = "internal";
        return true;
    }

    if (!tx_has_shielded_proofs(tx))
        return true;

    if (!sapling_params_loaded()) {
        out->ok = false;
        out->internal_error = true;
        out->first_failure_proof_type = "params_not_loaded";
        return true;
    }

    struct uint256 sighash;
    uint256_set_null(&sighash);
    uint32_t branch_id = consensus_current_epoch_branch_id(
        height, &chain_params_get()->consensus);
    struct script empty_script;
    empty_script.size = 0;
    struct sighash_type ht = { .raw = SIGHASH_ALL };
    if (!signature_hash(&empty_script, tx, NOT_AN_INPUT, ht, 0,
                        branch_id, NULL, &sighash)) {
        out->ok = false;
        out->internal_error = true;
        out->first_failure_proof_type = "sighash";
        return true;
    }

    if (tx->num_joinsplit > 0 &&
        !ed25519_verify(tx->joinsplit_sig, sighash.data, 32,
                        tx->joinsplit_pubkey.data)) {
        out->ok = false;
        out->first_failure_proof_type = "joinsplit_sig";
        return true;
    }

    if (tx->num_shielded_spend > 0 || tx->num_shielded_output > 0) {
        void *sctx = zclassic_sapling_verification_ctx_init();
        if (!sctx) {
            out->ok = false;
            out->internal_error = true;
            out->first_failure_proof_type = "sapling_ctx";
            return true;
        }

        for (size_t i = 0; i < tx->num_shielded_spend; i++) {
            const struct spend_description *sd = &tx->v_shielded_spend[i];
            out->sapling_spends_total++;
            if (!zclassic_sapling_check_spend(
                    sctx, sd->cv.data, sd->anchor.data,
                    sd->nullifier.data, sd->rk.data,
                    sd->zkproof, sd->spend_auth_sig, sighash.data)) {
                zclassic_sapling_verification_ctx_free(sctx);
                out->ok = false;
                out->first_failure_proof_type = "sapling_spend";
                return true;
            }
        }

        for (size_t i = 0; i < tx->num_shielded_output; i++) {
            const struct output_description *od = &tx->v_shielded_output[i];
            out->sapling_outputs_total++;
            if (!zclassic_sapling_check_output(
                    sctx, od->cv.data, od->cm.data,
                    od->ephemeral_key.data, od->zkproof)) {
                zclassic_sapling_verification_ctx_free(sctx);
                out->ok = false;
                out->first_failure_proof_type = "sapling_output";
                return true;
            }
        }

        if (!zclassic_sapling_final_check(sctx, tx->value_balance,
                                          tx->binding_sig, sighash.data)) {
            zclassic_sapling_verification_ctx_free(sctx);
            out->ok = false;
            out->first_failure_proof_type = "binding_sig";
            return true;
        }
        zclassic_sapling_verification_ctx_free(sctx);
    }

    for (size_t i = 0; i < tx->num_joinsplit; i++) {
        const struct js_description *js = &tx->v_joinsplit[i];
        out->sprout_joinsplits_total++;
        uint8_t h_sig[32];
        sprout_h_sig(js->random_seed.data, js->nullifiers[0].data,
                     js->nullifiers[1].data, tx->joinsplit_pubkey.data,
                     h_sig);

        bool ok;
        if (js->use_groth) {
            ok = sprout_verify_groth16(js->proof, js->anchor.data, h_sig,
                    js->macs[0].data, js->macs[1].data,
                    js->nullifiers[0].data, js->nullifiers[1].data,
                    js->commitments[0].data, js->commitments[1].data,
                    (uint64_t)js->vpub_old, (uint64_t)js->vpub_new);
            if (!ok) {
                out->ok = false;
                out->first_failure_proof_type = "sprout_groth16";
                return true;
            }
        } else {
            ok = sprout_verify_phgr13(js->proof, js->anchor.data, h_sig,
                    js->macs[0].data, js->macs[1].data,
                    js->nullifiers[0].data, js->nullifiers[1].data,
                    js->commitments[0].data, js->commitments[1].data,
                    (uint64_t)js->vpub_old, (uint64_t)js->vpub_new);
            if (!ok) {
                out->ok = false;
                out->first_failure_proof_type = "sprout_phgr13";
                return true;
            }
        }
    }

    return true;
}

static void validate_summary_init(struct validate_summary *s)
{
    memset(s, 0, sizeof(*s));
    s->ok = 1;
    uint256_set_null(&s->first_failure_txid);
}

static void add_success_counters(const struct transaction *tx,
                                 const struct proof_validate_tx_report *r)
{
    atomic_fetch_add(&g_sapling_spends_verified_total,
                     (uint64_t)r->sapling_spends_total);
    atomic_fetch_add(&g_sapling_outputs_verified_total,
                     (uint64_t)r->sapling_outputs_total);
    for (size_t i = 0; tx && i < tx->num_joinsplit; i++) {
        if (tx->v_joinsplit[i].use_groth)
            atomic_fetch_add(&g_sprout_groth16_verified_total, 1);
        else
            atomic_fetch_add(&g_sprout_phgr13_verified_total, 1);
    }
    if (tx && (tx->num_shielded_spend > 0 ||
               tx->num_shielded_output > 0))
        atomic_fetch_add(&g_binding_sig_verified_total, 1);
}

static void add_failure_counter(const char *type)
{
    if (!type) return;
    if (strcmp(type, "sapling_spend") == 0)
        atomic_fetch_add(&g_sapling_spends_failed_total, 1);
    else if (strcmp(type, "sapling_output") == 0)
        atomic_fetch_add(&g_sapling_outputs_failed_total, 1);
    else if (strcmp(type, "sprout_groth16") == 0)
        atomic_fetch_add(&g_sprout_groth16_failed_total, 1);
    else if (strcmp(type, "sprout_phgr13") == 0)
        atomic_fetch_add(&g_sprout_phgr13_failed_total, 1);
    else if (strcmp(type, "binding_sig") == 0)
        atomic_fetch_add(&g_binding_sig_failed_total, 1);
}

static void validate_block_proofs(const struct block *blk, int height,
                                  struct validate_summary *out)
{
    validate_summary_init(out);
    if (!blk) {
        out->ok = 0;
        out->internal_error = 1;
        return;
    }

    for (size_t ti = 0; ti < blk->num_vtx; ti++) {
        const struct transaction *tx = &blk->vtx[ti];
        proof_validate_tx_verify_fn verifier =
            g_tx_verifier ? g_tx_verifier : default_verify_tx;
        struct proof_validate_tx_report r;
        if (!verifier(tx, height, &r, g_tx_verifier_user)) {
            out->ok = 0;
            out->internal_error = 1;
            out->first_failure_txid = tx->hash;
            out->first_failure_proof_type = "internal";
            return;
        }

        out->sapling_spends_total += r.sapling_spends_total;
        out->sapling_outputs_total += r.sapling_outputs_total;
        out->sprout_joinsplits_total += r.sprout_joinsplits_total;
        if (r.ok) {
            add_success_counters(tx, &r);
            continue;
        }

        out->ok = 0;
        out->internal_error = r.internal_error ? 1 : 0;
        out->first_failure_txid = tx->hash;
        out->first_failure_proof_type = r.first_failure_proof_type
            ? r.first_failure_proof_type : "internal";
        add_failure_counter(out->first_failure_proof_type);
        return;
    }
}

static bool block_has_shielded_proofs(const struct block *blk)
{
    if (!blk)
        return false;
    for (size_t i = 0; i < blk->num_vtx; i++) {
        if (tx_has_shielded_proofs(&blk->vtx[i]))
            return true;
    }
    return false;
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

    uint64_t sv_cursor = stage_cursor_persisted(db, "script_validate",
                                               STAGE_NAME);
    if ((uint64_t)next_h >= sv_cursor) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    struct script_validate_row upstream;
    int found = proof_validate_script_validate_log_at(db, next_h, &upstream);
    if (found < 0) return JOB_FATAL;
    if (found == 0) {
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    if (upstream.ok == 0) {
        if (!proof_validate_log_insert(db, next_h, "upstream_failed", false,
                        0, 0, 0, NULL, NULL))
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
    proof_validate_reader_fn reader = g_reader ? g_reader
                                               : stage_default_block_reader;
    if (!reader(&blk, bi, g_datadir, g_reader_user)) {
        block_free(&blk);
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    if (!g_tx_verifier && block_has_shielded_proofs(&blk) &&
        !sapling_params_loaded()) {
        block_free(&blk);
        atomic_store(&g_last_blocked_unix, platform_time_wall_unix());
        return JOB_IDLE;
    }

    struct validate_summary summary;
    validate_block_proofs(&blk, next_h, &summary);
    block_free(&blk);

    const char *status = "verified";
    bool ok = true;
    const struct uint256 *fail_txid = NULL;
    const char *fail_type = NULL;
    char fail_txid_hex[65] = {0};
    if (!summary.ok) {
        uint256_get_hex(&summary.first_failure_txid, fail_txid_hex);
        fail_txid = &summary.first_failure_txid;
        fail_type = summary.first_failure_proof_type;
    }
    if (!summary.ok && summary.internal_error) {
        status = "internal_error";
        ok = false;
        atomic_fetch_add(&g_internal_error_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "proof_validate internal_error height=%d type=%s txid=%s",
                    next_h, fail_type ? fail_type : "unknown", fail_txid_hex);
    } else if (!summary.ok) {
        status = "proof_invalid";
        ok = false;
        atomic_fetch_add(&g_proof_invalid_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0,
                    "proof_validate proof_invalid height=%d type=%s txid=%s",
                    next_h, fail_type ? fail_type : "unknown", fail_txid_hex);
    } else {
        atomic_fetch_add(&g_verified_total, 1);
    }

    if (!proof_validate_log_insert(db, next_h, status, ok,
                    summary.sapling_spends_total,
                    summary.sapling_outputs_total,
                    summary.sprout_joinsplits_total, fail_txid, fail_type))
        return JOB_FATAL;

    atomic_store(&g_last_advance_height, (int64_t)next_h);
    c->cursor_out = c->cursor_in + 1;
    return JOB_ADVANCED;
}

bool proof_validate_stage_init(struct main_state *ms)
{
    if (!ms) LOG_FAIL("proof_validate", "init: NULL main_state");

    sqlite3 *db = progress_store_db();
    if (!db) LOG_FAIL("proof_validate", "init: progress_store not open");

    pthread_mutex_lock(&g_lock);
    if (g_stage != NULL) {
        bool same = (g_ms == ms);
        pthread_mutex_unlock(&g_lock);
        if (!same)
            LOG_FAIL("proof_validate",
                "init: already bound to a different main_state");
        return true;
    }

    if (!proof_validate_log_ensure_schema(db)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    GetDataDir(true, g_datadir, sizeof(g_datadir));

    stage_t *s = stage_create(STAGE_NAME, step_validate, NULL);
    if (!s) {
        pthread_mutex_unlock(&g_lock);
        LOG_FAIL("proof_validate", "init: stage_create failed");
    }

    g_ms = ms;
    g_stage = s;
    pthread_mutex_unlock(&g_lock);

    LOG_INFO("proof_validate", "[proof_validate] stage initialised");
    return true;
}

STAGE_STEP_ONCE_SIMPLE(proof_validate)

STAGE_DRAIN_IMPL(proof_validate)

void proof_validate_stage_shutdown(void)
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
    g_tx_verifier = NULL;
    g_tx_verifier_user = NULL;
    atomic_store(&g_verified_total, (uint64_t)0);
    atomic_store(&g_proof_invalid_total, (uint64_t)0);
    atomic_store(&g_internal_error_total, (uint64_t)0);
    atomic_store(&g_upstream_failed_total, (uint64_t)0);
    atomic_store(&g_sapling_spends_verified_total, (uint64_t)0);
    atomic_store(&g_sapling_spends_failed_total, (uint64_t)0);
    atomic_store(&g_sapling_outputs_verified_total, (uint64_t)0);
    atomic_store(&g_sapling_outputs_failed_total, (uint64_t)0);
    atomic_store(&g_sprout_groth16_verified_total, (uint64_t)0);
    atomic_store(&g_sprout_groth16_failed_total, (uint64_t)0);
    atomic_store(&g_sprout_phgr13_verified_total, (uint64_t)0);
    atomic_store(&g_sprout_phgr13_failed_total, (uint64_t)0);
    atomic_store(&g_binding_sig_verified_total, (uint64_t)0);
    atomic_store(&g_binding_sig_failed_total, (uint64_t)0);
    atomic_store(&g_last_step_unix, (int64_t)0);
    atomic_store(&g_last_blocked_unix, (int64_t)0);
    atomic_store(&g_last_advance_height, (int64_t)-1);
    pthread_mutex_unlock(&g_lock);
}

void proof_validate_stage_set_reader(proof_validate_reader_fn fn, void *user)
{
    pthread_mutex_lock(&g_lock);
    g_reader = fn;
    g_reader_user = user;
    pthread_mutex_unlock(&g_lock);
}

void proof_validate_stage_set_tx_verifier(proof_validate_tx_verify_fn fn,
                                          void *user)
{
    pthread_mutex_lock(&g_lock);
    g_tx_verifier = fn;
    g_tx_verifier_user = user;
    pthread_mutex_unlock(&g_lock);
}

uint64_t proof_validate_stage_cursor(void)
{
    return g_stage ? stage_cursor(g_stage) : 0;
}

uint64_t proof_validate_stage_verified_total(void)
{
    return atomic_load(&g_verified_total);
}

uint64_t proof_validate_stage_proof_invalid_total(void)
{
    return atomic_load(&g_proof_invalid_total);
}

uint64_t proof_validate_stage_internal_error_total(void)
{
    return atomic_load(&g_internal_error_total);
}

uint64_t proof_validate_stage_upstream_failed_total(void)
{
    return atomic_load(&g_upstream_failed_total);
}

uint64_t proof_validate_stage_sapling_spends_verified_total(void)
{
    return atomic_load(&g_sapling_spends_verified_total);
}

uint64_t proof_validate_stage_sapling_spends_failed_total(void)
{
    return atomic_load(&g_sapling_spends_failed_total);
}

uint64_t proof_validate_stage_sapling_outputs_verified_total(void)
{
    return atomic_load(&g_sapling_outputs_verified_total);
}

uint64_t proof_validate_stage_sapling_outputs_failed_total(void)
{
    return atomic_load(&g_sapling_outputs_failed_total);
}

uint64_t proof_validate_stage_sprout_groth16_verified_total(void)
{
    return atomic_load(&g_sprout_groth16_verified_total);
}

uint64_t proof_validate_stage_sprout_groth16_failed_total(void)
{
    return atomic_load(&g_sprout_groth16_failed_total);
}

uint64_t proof_validate_stage_sprout_phgr13_verified_total(void)
{
    return atomic_load(&g_sprout_phgr13_verified_total);
}

uint64_t proof_validate_stage_sprout_phgr13_failed_total(void)
{
    return atomic_load(&g_sprout_phgr13_failed_total);
}

uint64_t proof_validate_stage_binding_sig_verified_total(void)
{
    return atomic_load(&g_binding_sig_verified_total);
}

uint64_t proof_validate_stage_binding_sig_failed_total(void)
{
    return atomic_load(&g_binding_sig_failed_total);
}

/* Surface the lowest ok=0 row (status/proof_type/txid) into `out`, mirroring
 * the validate_headers_report failure-summary query convention. No-op if the
 * db is unavailable or there is no failing row. Takes its own tx lock since
 * dump_state runs outside any stage txn. */
static void dump_first_failure(struct json_value *out, sqlite3 *db)
{
    if (!db) return;
    progress_store_tx_lock();
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT height, COALESCE(status,''), "
        "       COALESCE(first_failure_proof_type,''), first_failure_txid "
        "  FROM proof_validate_log WHERE ok=0 "
        " ORDER BY height ASC LIMIT 1",
        -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {  // raw-sql-ok:progress-kv-kernel-store
        json_push_kv_int(out, "first_failure_height",
                         sqlite3_column_int64(st, 0));
        const unsigned char *status = sqlite3_column_text(st, 1);
        const unsigned char *ptype = sqlite3_column_text(st, 2);
        json_push_kv_str(out, "first_failure_status",
                         status ? (const char *)status : "");
        json_push_kv_str(out, "first_failure_proof_type",
                         ptype ? (const char *)ptype : "");
        const void *blob = sqlite3_column_blob(st, 3);
        char hex[65] = {0};
        if (blob && sqlite3_column_bytes(st, 3) == 32) {
            struct uint256 t;
            memcpy(t.data, blob, 32);
            uint256_get_hex(&t, hex);
        }
        json_push_kv_str(out, "first_failure_txid", hex);
    }
    sqlite3_finalize(st);
    progress_store_tx_unlock();
}

bool proof_validate_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    sqlite3 *db = progress_store_db();
    int64_t now = platform_time_wall_unix();
    int64_t last = atomic_load(&g_last_step_unix);

    json_push_kv_bool(out, "initialised", g_stage != NULL);
    json_push_kv_str (out, "stage_name", STAGE_NAME);
    json_push_kv_int (out, "cursor",
                      (int64_t)(g_stage ? stage_cursor(g_stage) : 0));
    json_push_kv_int (out, "verified_total",
                      (int64_t)atomic_load(&g_verified_total));
    json_push_kv_int (out, "proof_invalid_total",
                      (int64_t)atomic_load(&g_proof_invalid_total));
    json_push_kv_int (out, "internal_error_total",
                      (int64_t)atomic_load(&g_internal_error_total));
    json_push_kv_int (out, "upstream_failed_total",
                      (int64_t)atomic_load(&g_upstream_failed_total));
    json_push_kv_int (out, "sapling_spends_verified_total",
                      (int64_t)atomic_load(&g_sapling_spends_verified_total));
    json_push_kv_int (out, "sapling_spends_failed_total",
                      (int64_t)atomic_load(&g_sapling_spends_failed_total));
    json_push_kv_int (out, "sapling_outputs_verified_total",
                      (int64_t)atomic_load(&g_sapling_outputs_verified_total));
    json_push_kv_int (out, "sapling_outputs_failed_total",
                      (int64_t)atomic_load(&g_sapling_outputs_failed_total));
    json_push_kv_int (out, "sprout_groth16_verified_total",
                      (int64_t)atomic_load(&g_sprout_groth16_verified_total));
    json_push_kv_int (out, "sprout_groth16_failed_total",
                      (int64_t)atomic_load(&g_sprout_groth16_failed_total));
    json_push_kv_int (out, "sprout_phgr13_verified_total",
                      (int64_t)atomic_load(&g_sprout_phgr13_verified_total));
    json_push_kv_int (out, "sprout_phgr13_failed_total",
                      (int64_t)atomic_load(&g_sprout_phgr13_failed_total));
    json_push_kv_int (out, "binding_sig_verified_total",
                      (int64_t)atomic_load(&g_binding_sig_verified_total));
    json_push_kv_int (out, "binding_sig_failed_total",
                      (int64_t)atomic_load(&g_binding_sig_failed_total));
    json_push_kv_int (out, "last_advance_height",
                      atomic_load(&g_last_advance_height));
    json_push_kv_int (out, "last_step_unix", last);
    json_push_kv_int (out, "last_step_age_seconds",
                      last > 0 ? now - last : -1);
    json_push_kv_int (out, "last_blocked_unix",
                      atomic_load(&g_last_blocked_unix));
    json_push_kv_int (out, "log_rows",
                      db ? stage_log_row_count(db, STAGE_NAME,
                                               "proof_validate_log") : 0);
    dump_first_failure(out, db);
    stage_dump_counters(out, g_stage);
    return true;
}
