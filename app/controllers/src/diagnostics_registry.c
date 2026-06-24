/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Diagnostics registry — the `g_dumpers[]` table and the `dumpstate` /
 * `zcl_state` dispatcher. A single declarative registry that maps a
 * subsystem name to its `*_dump_state_json` function, so adding a new
 * introspectable subsystem is one line here plus one dump function in
 * the owning module (see CLAUDE.md "Adding state introspection").
 *
 * It also owns the controller-level state (main_state + datadir) shared
 * across the diagnostics controller family, because the block_index dump
 * here is the primary consumer of main_state.
 */

#include "platform/time_compat.h"
#include "controllers/diagnostics_controller.h"
#include "controllers/diagnostics_internal.h"

#include "views/format_helpers.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "validation/contextual_check_tx.h"
#include "validation/verify_queue.h"
#include "chain/chain.h"
#include "core/uint256.h"
#include "core/arith_uint256.h"
#include "json/json.h"
#include "rpc/server.h"
#include "controllers/explorer_internal.h"
#include "controllers/strong_params.h"
#include "services/chain_restore_boot_snapshot.h"
#include "services/block_source_policy.h"
#include "services/zclassicd_oracle_service.h"
#include "services/header_probe.h"
#include "services/utxo_parity_service.h"
#include "services/soak_attestation_service.h"
#include "services/canary_sentinel_watch.h"
#include "services/bg_validation_service.h"
#include "services/disk_monitor.h"
#include "services/sync_monitor.h"
#include "services/db_maintenance.h"
#include "services/legacy_mirror_sync_service.h"
#include "services/oracle_policy.h"
#include "services/quorum_oracle_service.h"
#include "services/rolling_anchor_service.h"
#include "services/seal_service.h"
#include "services/block_index_integrity.h"
#include "services/block_pruning_service.h"
#include "services/chain_evidence_authority_service.h"
#include "services/utxo_recovery_service.h"
#include "sync/sync_planner.h"
#include "jobs/header_admit_stage.h"
#include "jobs/reducer_frontier.h"
#include "jobs/validate_headers_stage.h"
#include "services/node_health_service.h"
#include "jobs/body_fetch_stage.h"
#include "jobs/body_persist_stage.h"
#include "jobs/script_validate_stage.h"
#include "jobs/proof_validate_stage.h"
#include "jobs/utxo_apply_stage.h"
#include "jobs/tip_finalize_stage.h"
#include "jobs/stage_repair_coin_backfill.h"
#include "jobs/refold_progress.h"
#include "services/chain_tip_watchdog.h"
#include "services/invariant_sentinel.h"
#include "framework/condition.h"
#include "storage/block_index_projection.h"
#include "storage/mempool_projection.h"
#include "storage/peers_projection.h"
#include "storage/progress_store.h"
#include "storage/small_projections.h"
#include "storage/utxo_projection.h"
#include "storage/znam_projection.h"
#include "storage/wallet_projection.h"
#include "crypto_registry/crypto_registry.h"
#include "services/ibd_throttle.h"
#include "services/mempool_limits.h"
#include "health/heartbeat.h"
#include "models/database.h"
#include "config/runtime.h"
#include "net/peer_lifecycle.h"
#include "util/log_macros.h"
#include "util/long_op.h"
#include "util/service_state.h"
#include "util/supervisor.h"
#include "util/blocker.h"

#include <stdint.h>
#include <string.h>

/* ── Controller-level state ─────────────────────────────────────── */

static struct {
    struct main_state *main_state;
    char datadir[1024];
} g_diag = {0};

void diagnostics_controller_set_state(struct main_state *ms,
                                      const char *datadir)
{
    g_diag.main_state = ms;
    if (datadir) {
        snprintf(g_diag.datadir, sizeof(g_diag.datadir), "%s", datadir);
    }
}

const char *diag_datadir(void)
{
    return g_diag.datadir;
}

/* ── block_index dump ─────────────────────────────────────────────
 *
 * Lives here (not lib/chain) because the lookup needs main_state, which
 * is an app/validation layering concern. Decodes nStatus flags by name
 * so callers don't have to remember the bit positions.
 */

static void push_block_status_flags(struct json_value *arr, unsigned nStatus)
{
    /* Lower 3 bits = validity level (enum); the rest are flag bits. */
    static const struct { unsigned mask; const char *name; } flags[] = {
        { BLOCK_HAVE_DATA,    "BLOCK_HAVE_DATA" },
        { BLOCK_HAVE_UNDO,    "BLOCK_HAVE_UNDO" },
        { BLOCK_FAILED_VALID, "BLOCK_FAILED_VALID" },
        { BLOCK_FAILED_CHILD, "BLOCK_FAILED_CHILD" },
    };
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        if (nStatus & flags[i].mask) {
            struct json_value v = {0};
            json_set_str(&v, flags[i].name);
            json_push_back(arr, &v);
            json_free(&v);
        }
    }
}

static const char *block_validity_level_name(unsigned nStatus)
{
    switch (nStatus & BLOCK_VALID_MASK) {
        case BLOCK_VALID_UNKNOWN:      return "BLOCK_VALID_UNKNOWN";
        case BLOCK_VALID_HEADER:       return "BLOCK_VALID_HEADER";
        case BLOCK_VALID_TREE:         return "BLOCK_VALID_TREE";
        case BLOCK_VALID_TRANSACTIONS: return "BLOCK_VALID_TRANSACTIONS";
        case BLOCK_VALID_CHAIN:        return "BLOCK_VALID_CHAIN";
        case BLOCK_VALID_SCRIPTS:      return "BLOCK_VALID_SCRIPTS";
    }
    return "UNKNOWN";
}

static void push_block_ref(struct json_value *out, const char *prefix,
                           const struct block_index *bi)
{
    char key[64];
    snprintf(key, sizeof(key), "%s_height", prefix);
    json_push_kv_int(out, key, bi ? bi->nHeight : -1);

    snprintf(key, sizeof(key), "%s_hash", prefix);
    if (bi && bi->phashBlock) {
        char hex[65];
        uint256_get_hex(bi->phashBlock, hex);
        json_push_kv_str(out, key, hex);
    } else {
        json_push_kv_str(out, key, "");
    }
}

static bool header_band_dump_state_json(struct json_value *out,
                                        const char *key)
{
    (void)key;
    if (!out)
        return false;
    json_set_object(out);

    struct main_state *ms = g_diag.main_state;
    bool blocker = blocker_exists(HEADER_BAND_BLOCKER_ID);
    json_push_kv_bool(out, "has_main_state", ms != NULL);
    json_push_kv_bool(out, "blocker_recorded", blocker);

    if (!ms) {
        json_push_kv_str(out, "state", blocker ? "blocker_without_state"
                                                : "unknown_no_state");
        json_push_kv_int(out, "remaining_headers", -1);
        return true;
    }

    struct active_chain *chain = &ms->chain_active;
    struct block_index *tip = active_chain_tip(chain);
    const struct block_index *island_root =
        tip ? utxo_recovery_block_ancestry_break(tip) : NULL;
    struct block_index *anchor =
        blocker ? syncsvc_header_band_backfill_anchor(chain) : NULL;

    push_block_ref(out, "active_tip", tip);
    push_block_ref(out, "island_root", island_root);
    push_block_ref(out, "backfill_anchor", anchor);
    json_push_kv_bool(out, "band_open", island_root != NULL);

    int remaining = -1;
    if (island_root && anchor && island_root->nHeight > anchor->nHeight)
        remaining = island_root->nHeight - anchor->nHeight;
    else if (!island_root)
        remaining = 0;
    json_push_kv_int(out, "remaining_headers", remaining);

    const char *state = "healthy";
    if (blocker && island_root && anchor)
        state = "backfilling";
    else if (blocker && island_root)
        state = "blocked_no_anchor";
    else if (blocker && !island_root)
        state = "closing_or_stale_blocker";
    else if (!blocker && island_root)
        state = "derived_band_without_blocker";
    json_push_kv_str(out, "state", state);
    return true;
}

static struct block_index *find_block_index_by_key(struct main_state *ms,
                                                    const char *key)
{
    if (!ms || !key || !key[0]) return NULL;

    /* Numeric → height lookup via active_chain. */
    bool is_num = true;
    for (const char *c = key; *c; c++) {
        if (*c < '0' || *c > '9') { is_num = false; break; }
    }
    if (is_num) {
        int height = atoi(key);
        return active_chain_at(&ms->chain_active, height);
    }

    /* Hex → hash lookup via block_map. */
    if (!zcl_is_hex_string(key, 64)) return NULL;
    struct uint256 h;
    uint256_set_hex(&h, key);
    return block_map_find(&ms->map_block_index, &h);
}

static bool block_index_dump_state_json(struct json_value *out, const char *key)
{
    if (!out) return false;
    struct block_index *bi = find_block_index_by_key(g_diag.main_state, key);
    json_set_object(out);
    {
        struct bii_recovery_status status;
        struct json_value integrity = {0};
        bii_get_recovery_status(&status);
        json_set_object(&integrity);
        json_push_kv_str(&integrity, "verdict",
                         bii_verdict_name(status.verdict));
        json_push_kv_str(&integrity, "action",
                         bii_recovery_action_name(status.action));
        json_push_kv_bool(&integrity, "degraded", status.degraded);
        json_push_kv_bool(&integrity, "unsafe_override",
                          status.unsafe_override);
        json_push_kv_int(&integrity, "last_check_unix", status.unix_time);
        if (status.reason[0])
            json_push_kv_str(&integrity, "reason", status.reason);
        json_push_kv(out, "integrity", &integrity);
        json_free(&integrity);
    }
    if (!bi) {
        json_push_kv_bool(out, "found", false);
        json_push_kv_str(out, "key", key ? key : "");
        return true;
    }

    /* Snapshot every field we report under cs_main and release before any
     * JSON formatting. A concurrent reorg/restore writer mutates these
     * block_index fields (nStatus/nFile/nDataPos via
     * stage_repair_reducer_frontier.c, pprev/nChainWork via reorg) while
     * holding cs_main, so a lock-free read here can tear the multi-word
     * nChainWork or report a half-updated field set. cs_main is the inner
     * lock relative to the reducer's coins_kv/progress_store tx lock (that
     * lock is always taken first); taking ONLY cs_main here, and never
     * coins_kv while holding it, keeps the established order and cannot form
     * the ABBA cycle the lock-order law guards against. */
    struct {
        int64_t nHeight, nVersion, nTime, nBits, nChainTx, nTx;
        int64_t nFile, nDataPos, nUndoPos, nSequenceId, nStatus;
        bool have_hash, have_prev_hash, on_chain;
        char hash[65], hash_prev[65], chain_work[65];
    } snap = {0};

    if (g_diag.main_state)
        zcl_mutex_lock(&g_diag.main_state->cs_main);
    snap.nHeight = (int64_t)bi->nHeight;
    snap.nVersion = (int64_t)bi->nVersion;
    snap.nTime = (int64_t)bi->nTime;
    snap.nBits = (int64_t)bi->nBits;
    snap.nChainTx = (int64_t)bi->nChainTx;
    snap.nTx = (int64_t)bi->nTx;
    snap.nFile = (int64_t)bi->nFile;
    snap.nDataPos = (int64_t)bi->nDataPos;
    snap.nUndoPos = (int64_t)bi->nUndoPos;
    snap.nSequenceId = (int64_t)bi->nSequenceId;
    snap.nStatus = (int64_t)bi->nStatus;
    if (bi->phashBlock) {
        uint256_get_hex(bi->phashBlock, snap.hash);
        snap.have_hash = true;
    }
    if (bi->pprev && bi->pprev->phashBlock) {
        uint256_get_hex(bi->pprev->phashBlock, snap.hash_prev);
        snap.have_prev_hash = true;
    }
    arith_uint256_get_hex(&bi->nChainWork, snap.chain_work);
    if (g_diag.main_state) {
        struct block_index *at = active_chain_at(
            &g_diag.main_state->chain_active, bi->nHeight);
        snap.on_chain = (at == bi);
        zcl_mutex_unlock(&g_diag.main_state->cs_main);
    }

    json_push_kv_bool(out, "found", true);
    json_push_kv_int(out, "nHeight", snap.nHeight);
    json_push_kv_int(out, "nVersion", snap.nVersion);
    json_push_kv_int(out, "nTime", snap.nTime);
    json_push_kv_int(out, "nBits", snap.nBits);
    json_push_kv_int(out, "nChainTx", snap.nChainTx);
    json_push_kv_int(out, "nTx", snap.nTx);
    json_push_kv_int(out, "nFile", snap.nFile);
    json_push_kv_int(out, "nDataPos", snap.nDataPos);
    json_push_kv_int(out, "nUndoPos", snap.nUndoPos);
    json_push_kv_int(out, "nSequenceId", snap.nSequenceId);
    json_push_kv_int(out, "nStatus_raw", snap.nStatus);
    json_push_kv_str(out, "nStatus_validity",
                     block_validity_level_name((unsigned)snap.nStatus));
    {
        struct json_value flags_arr = {0};
        json_set_array(&flags_arr);
        push_block_status_flags(&flags_arr, (unsigned)snap.nStatus);
        json_push_kv(out, "nStatus_flags", &flags_arr);
        json_free(&flags_arr);
    }
    json_push_kv_str(out, "hash", snap.have_hash ? snap.hash : "");
    json_push_kv_str(out, "hash_prev",
                     snap.have_prev_hash ? snap.hash_prev : "");
    json_push_kv_str(out, "nChainWork", snap.chain_work);
    json_push_kv_bool(out, "on_active_chain", snap.on_chain);
    return true;
}

static void push_evidence_record_json(struct json_value *out, const char *key,
                                      const struct chain_evidence_record *e)
{
    struct json_value obj = {0};
    json_set_object(&obj);
    json_push_kv_str(&obj, "source_class",
                      chain_evidence_source_class_name(
                          e ? e->source_class : CEC_SOURCE_CLASS_UNKNOWN));
    json_push_kv_str(&obj, "publish_state",
                      chain_evidence_publish_state_name(
                          e ? e->publish_state
                            : CEC_PUBLISH_NOT_PUBLISHABLE));
    json_push_kv_bool(&obj, "header_ancestry_linked",
                      e && e->header_ancestry_linked);
    json_push_kv_bool(&obj, "chainwork_recomputed",
                      e && e->chainwork_recomputed);
    json_push_kv_bool(&obj, "nakamoto_selected_best_work",
                      e && e->nakamoto_selected_best_work);
    json_push_kv_bool(&obj, "block_bytes_hash_checked",
                      e && e->block_bytes_hash_checked);
    json_push_kv_bool(&obj, "utxo_sha3_verified",
                      e && e->utxo_sha3_verified);
    json_push_kv_bool(&obj, "mmb_flyclient_proof_verified",
                      e && e->mmb_flyclient_proof_verified);
    json_push_kv_bool(&obj, "chunk_hash_coverage_verified",
                      e && e->chunk_hash_coverage_verified);
    json_push_kv_bool(&obj, "full_validation_complete",
                      e && e->full_validation_complete);
    json_push_kv(out, key, &obj);
    json_free(&obj);
}

static void push_hash_json(struct json_value *out, const char *key,
                           bool present, const struct uint256 *hash)
{
    char hex[65] = {0};
    if (present && hash)
        uint256_get_hex(hash, hex);
    json_push_kv_str(out, key, present ? hex : "");
}

static void push_explorer_index_state_json(struct json_value *out,
                                           struct node_db *ndb)
{
    struct json_value obj = {0};

    json_set_object(&obj);
    if (!ndb || !ndb->open || !ndb->db) {
        json_push_kv_str(&obj, "state", "unknown");
        json_push_kv_str(&obj, "reason", "database unavailable");
        json_push_kv(out, "explorer_index_state", &obj);
        json_free(&obj);
        return;
    }

    struct explorer_history_validation v;
    int64_t height = sql_query_i64(ndb->db,
        "SELECT COALESCE(MAX(height),0) FROM blocks");
    explorer_validate_block_history(ndb->db, height, &v);
    json_push_kv_str(&obj, "state", v.usable ? "complete" : "degraded");
    json_push_kv_str(&obj, "reason", v.reason);
    json_push_kv_int(&obj, "height", v.max_height);
    json_push_kv_int(&obj, "blocks", v.block_rows);
    json_push_kv_int(&obj, "transactions", v.tx_rows);
    json_push_kv_int(&obj, "tx_outputs", v.tx_output_rows);
    json_push_kv_int(&obj, "integrity_receipts", v.integrity_rows);
    json_push_kv(out, "explorer_index_state", &obj);
    json_free(&obj);
}

bool diag_chain_evidence_dump_state_json(struct json_value *out,
                                         const char *key)
{
    (void)key;
    if (!out)
        return false;
    struct chain_evidence_controller authority;
    struct chain_evidence_controller_view view;

    json_set_object(out);
    chain_evidence_controller_init(&authority, app_runtime_node_db(), csr_instance());
    chain_evidence_controller_snapshot(&authority, &view);

    json_push_kv_str(out, "sync_state",
                     chain_evidence_controller_state_name(view.state));
    json_push_kv_str(out, "publish_state",
                     chain_evidence_publish_state_name(view.publish_state));
    json_push_kv_str(out, "active_tip_source_class",
                     chain_evidence_source_class_name(
                         view.active_tip_source_class));
    json_push_kv_int(out, "active_tip",
                     (int64_t)view.active_tip_height);
    json_push_kv_int(out, "header_tip",
                     (int64_t)view.header_tip_height);
    json_push_kv_int(out, "persisted_active_tip",
                     (int64_t)view.persisted_active_tip_height);
    json_push_kv_int(out, "snapshot_anchor",
                     (int64_t)view.snapshot_anchor_height);
    json_push_kv_int(out, "utxo_max_height",
                     (int64_t)view.utxo_max_height);
    json_push_kv_int(out, "coins_best_block_height",
                     (int64_t)view.coins_best_block_height);
    json_push_kv_int(out, "csr_sqlite_max_height",
                     (int64_t)view.sqlite_max_height);
    push_hash_json(out, "active_tip_hash", view.has_active_tip_hash,
                   &view.active_tip_hash);
    push_hash_json(out, "header_tip_hash", view.has_header_tip_hash,
                   &view.header_tip_hash);
    push_hash_json(out, "persisted_active_tip_hash",
                   view.has_persisted_active_tip_hash,
                   &view.persisted_active_tip_hash);
    push_hash_json(out, "coins_best_block_hash",
                   view.has_coins_best_block_hash,
                   &view.coins_best_block_hash);
    json_push_kv_bool(out, "missing_active_tip_evidence",
                      view.missing_active_tip_evidence);
    json_push_kv_bool(out, "publish_state_not_local",
                      view.publish_state_not_local);
    json_push_kv_bool(out, "active_tip_hash_mismatch",
                      view.active_tip_hash_mismatch);
    json_push_kv_bool(out, "csr_cursor_mismatch",
                      view.csr_cursor_mismatch);
    json_push_kv_bool(out, "repaired_active_tip_evidence",
                      view.repaired_active_tip_evidence);
    json_push_kv_str(out, "health_reason", view.health_reason);
    push_evidence_record_json(out, "block_index_evidence_state",
                              &view.block_index_evidence_state);
    push_evidence_record_json(out, "active_tip_evidence",
                              &view.active_tip_evidence);
    push_evidence_record_json(out, "snapshot_evidence",
                              &view.snapshot_evidence);
    push_evidence_record_json(out, "header_chain_evidence",
                              &view.header_chain_evidence);
    json_push_kv_int(out, "deferred_proof_validation_below",
                     (int64_t)g_deferred_proof_validation_below_height);
    json_push_kv_int(out, "background_validation_height",
                     (int64_t)view.background_validation_height);
    json_push_kv_str(out, "contradiction_reason",
                     view.contradiction_reason);
    push_explorer_index_state_json(out, app_runtime_node_db());
    return true;
}

/* ── RPC: getmirrorstatus ──────────────────────────────────────────
 *
 * Backs the `zcl_mirror_status` MCP tool: the legacy_mirror
 * drift-detection introspection surface.
 */
bool diag_rpc_getmirrorstatus(const struct json_value *params, bool help,
                              struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "getmirrorstatus\n"
        "\nReturn legacy mirror sync status.\n"
        "\nResult: zclassic23_height/hash, zclassicd_height/hash, lag, "
        "reachable, mirror_running, last_catchup, last_error, "
        "headers_added, blocks_applied.");

    json_set_object(result);
    return legacy_mirror_sync_dump_state_json(result, NULL);
}

/* ── RPC: dumpstate <subsystem> [key] ────────────────────────────── */

typedef bool (*dump_fn)(struct json_value *out, const char *key);

struct dump_entry {
    const char *name;
    dump_fn     fn;
    const char *desc;
};

static const struct dump_entry g_dumpers[] = {
    { "supervisor", supervisor_dump_state_json,
                    "root supervisor: registered liveness contracts, "
                    "ticks_run, stall_fires, deadlines" },
    { "blocker",    blocker_dump_state_json,
                    "typed blocker registry: active blockers by class "
                    "{permanent,transient,dependency,resource}, "
                    "deadlines, escape actions, fire counts" },
    { "watchdog",    condition_engine_dump_state_json,
                     "compat alias for condition_engine status" },
    { "chain_evidence", diag_chain_evidence_dump_state_json,
                     "native chain evidence: tips, cursors, evidence flags, reconciliation reason" },
    { "chain_evidence_controller", diag_chain_evidence_dump_state_json,
                     "native chain evidence controller: tips, snapshot anchor, evidence, contradiction reason" },
    { "boot",        chain_restore_dump_state_json,
                     "last boot's integrity check + nbits-backfill counters" },
    { "service_state", service_state_dump_state_json,
                     "canonical runtime operational mode "
                     "(boot/restore/reconcile/degraded_serving/syncing/"
                     "healthy/repairing) + last transition reason" },
    { "block_index", block_index_dump_state_json,
                     "block_index entry by height or hash (in `key`)" },
    { "health",      health_dump_state_json,
                     "unified heartbeat ring: registered subsystems, ages, stall fires" },
    { "oracle",      zclassicd_oracle_dump_state_json,
                     "zclassicd oracle: drift-probe stats + RPC config" },
    { "header_probe", header_probe_dump_state_json,
                     "header probe: bulk header pull from co-located zclassicd via JSON-RPC" },
    { "header_band", header_band_dump_state_json,
                     "header band backfill: derived island root, current getheaders anchor, remaining span, and blocker state" },
    { "utxo_parity", utxo_parity_dump_state_json,
                     "standing UTXO-set parity vs reference commitment at the finalized frontier: checks/matches/mismatches, finalized_frontier, last_checked_height, source name+exact flag" },
    { "legacy_mirror", legacy_mirror_sync_dump_state_json,
                     "legacy mirror: always-on lockstep catch-up from co-located zclassicd" },
    { "oracle_policy", oracle_policy_dump_state_json,
                     "oracle policy: disagreement state machine (NORMAL / HALTED / PANIC)" },
    { "rolling_anchor", rolling_anchor_dump_state_json,
                     "rolling SHA3 anchor extension: runtime windows past compile-time prefix" },
    { "seal", seal_dump_state_json,
                     "seal ring (last 4): candidate/ratified state seals — height, "
                     "coins_sha3, utxo_count, supply, block_hash, ratified, sealed_at, "
                     "self_sha3 valid" },
    { "progress",    progress_store_dump_state_json,
                     "progress.kv: open/path/stage_cursor row count" },
    { "refold",      refold_progress_dump_state_json,
                     "refold/mint mode: in_progress + from_anchor cached "
                     "flags, compiled trusted_anchor, durable progress.kv keys "
                     "(durable_in_progress/from_anchor), from_anchor_target_tip" },
    { "reducer_frontier", reducer_frontier_dump_state_json,
                     "reducer L0 authority: H*, served_floor, stage cursors, "
                     "success-checked frontiers, coins frontier, first "
                     "validate_headers blocker + repair owner" },
    { "header_admit", header_admit_stage_dump_state_json,
                     "header_admit stage: cursor, counters, last admit" },
    { "validate_headers", validate_headers_stage_dump_state_json,
                     "validate_headers stage: cursor, pool stats, pass/fail counters" },
    { "body_fetch", body_fetch_stage_dump_state_json,
                     "body_fetch stage: cursor, observed/skipped counters, last advance" },
    { "body_persist", body_persist_dump_state_json,
                     "body_persist stage: cursor, verification counters, log rows" },
    { "script_validate", script_validate_dump_state_json,
                     "script_validate stage: cursor, script counters, log rows" },
    { "proof_validate", proof_validate_dump_state_json,
                     "proof_validate stage: cursor, proof counters, log rows" },
    { "utxo_apply", utxo_apply_dump_state_json,
                     "utxo_apply stage: cursor, UTXO delta counters, log rows" },
    { "tip_finalize", tip_finalize_dump_state_json,
                     "tip_finalize stage: cursor, finalize counters, log rows" },
    { "coin_backfill", coin_backfill_dump_state_json,
                     "frontier coin backfill: last result, scan cursor, refusal latches" },
    { "quorum_oracle", quorum_oracle_dump_state_json,
                     "multi-source quorum oracle: per-source vote stats + last verdict" },
    { "peer_lifecycle", peer_lifecycle_dump_state_json,
                     "P2P peer lifecycle attempts, handshakes, timeouts, and rejects by address/source" },
    { "chain_advance_coordinator", block_source_policy_dump_state_json,
                     "canonical chain-advance source scoring: P2P, snapshot, local import, mirror fallback" },
    { "chain_tip_watchdog", chain_tip_watchdog_dump_state_json,
                     "tip-stuck overlord: highest_tip, age_secs since last advance, escalation level + fire counts" },
    { "condition_engine", condition_engine_dump_state_json,
                     "self-heal engine: registered conditions with active/cleared status, attempts, thresholds" },
    { "long_op",     long_op_dump_state_json,
                     "active long-operation scopes (>600s code paths) that gate STATE_STUCK watchdog suppression" },
    { "ibd_throttle", ibd_throttle_dump_state_json,
                     "IBD throttle: token-bucket state, acquired/blocked counts, total wait time" },
    { "mempool_limits", mempool_limits_dump_state_json,
                     "mempool limits: enforce/expire call counts, evicted/expired totals, last-run summary" },
    { "block_pruning", block_pruning_dump_state_json,
                     "block pruning service: files/blocks pruned, bytes reclaimed, lowest height with data" },
    { "crypto_registry", crypto_registry_dump_state_json,
                     "registered crypto schemes, statuses, implementations, and kind counts" },
    { "mempool_projection", mempool_projection_dump_state_json,
                     "mempool projection over EV_TX_ADMIT_MEMPOOL / EV_TX_REMOVE_MEMPOOL" },
    { "peers_projection", peers_projection_dump_state_json,
                     "peers projection over EV_PEER_OBSERVED / EV_PEER_DROPPED" },
    { "utxo_projection", utxo_projection_dump_state_json,
                     "utxo_projection: open/path/last_consumed_offset, "
                     "utxo_count, events_consumed_total, emit/consume counters, "
                     "REPLACE collisions, last_catch_up_ms. UTXO set derived "
                     "from the event_log." },
    { "znam_projection", znam_projection_dump_state_json,
                     "znam projection: name_count, addr/text counts, "
                     "events_consumed_total, per-event-type counters, emit/fail "
                     "counters, last_consumed_offset, last_catch_up_ms." },
    { "wallet_projection", wallet_projection_dump_state_json,
                     "wallet view projection: public-only "
                     "address/tx/UTXO/note counts, total value, cursor, "
                     "and emit counters." },
    { "contacts_projection", contacts_projection_dump_state_json,
                     "contacts projection: count, cursor, "
                     "consume counters, emit counters, catch_up timing." },
    { "onion_announcements_projection", onion_ann_projection_dump_state_json,
                     "onion announcements projection: count, cursor, "
                     "consume counters, emit counters, catch_up timing." },
    { "hodl_history_projection", hodl_history_projection_dump_state_json,
                     "HODL history projection: count, cursor, "
                     "consume counters, emit counters, catch_up timing." },
    { "block_index_projection", block_index_projection_dump_state_json,
                     "block_index_projection: cursor, entry count, "
                     "events consumed, replace collisions, last catch_up_ms" },
    { "validation_pack", invariant_sentinel_dump_state_json,
                     "fail-loud validation pack: HOLD latch, per-connect "
                     "linkage + coinbase-label checks, authority-pair "
                     "self-check, window sweep, commitment audit, seed "
                     "gate, mirror divergence locator" },
    { "soak",           soak_dump_state_json,
                     "soak attestation log: lines_written, last_ts, "
                     "last_healthy, rotations, write_failures, file_bytes" },
    { "canary_watch",   canary_watch_dump_state_json,
                     "replay-canary sentinel watch: verdict_dir, last_scan, "
                     "files_seen, per-kind verdict+reason+ts, fail latch, "
                     "condition_active" },
    { "bg_validation", bg_validation_dump_state_json,
                     "background historical-proof re-verification: state, "
                     "verified/chain height, sigs+proofs verified, "
                     "blocks_per_sec, script-verify skips" },
    { "disk_monitor", disk_monitor_dump_state_json,
                     "free-space watchdog: running, level (ok/low/critical), "
                     "last free bytes + poll time, warn/refuse thresholds, "
                     "datadir" },
    { "sync_monitor", sync_monitor_dump_state_json,
                     "sync watchdog: recovery counters + last recovery detail, "
                     "local header-refill recovery sub-state" },
    { "db_maintenance", db_maintenance_dump_state_json,
                     "WAL/ANALYZE/VACUUM worker: last-run times + durations, "
                     "total runs/failures, last error" },
    { "verify_engine", verify_engine_dump_state_json,
                     "parallel-verify engine (ADDITIVE, not on the consensus "
                     "hot path): wired flag, would_be_workers, and lifetime "
                     "batch/job counters (batches_submitted, serial/parallel "
                     "split, jobs_processed)" },
};

int diagnostics_subsystems_csv(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return 0;
    out[0] = '\0';
    size_t pos = 0;
    int unclamped = 0;
    for (size_t i = 0; i < sizeof(g_dumpers) / sizeof(g_dumpers[0]); i++) {
        int n = snprintf(out + pos, pos < out_sz ? out_sz - pos : 0,
                         "%s%s", i ? "," : "", g_dumpers[i].name);
        unclamped += n;
        if (n > 0 && (size_t)n < out_sz - pos) pos += (size_t)n;
        else pos = out_sz - 1;
    }
    return unclamped;
}

bool diag_rpc_dumpstate(const struct json_value *params, bool help,
                        struct json_value *result)
{
    RPC_HELP(help, result,
        "dumpstate <subsystem> [key]\n"
        "\nDump in-process state for a subsystem. Subsystems:\n"
        "  watchdog     — sync watchdog status + stats\n"
        "  chain_advance_coordinator — source scoring + fallback policy\n"
        "  boot         — last boot's integrity + backfill counters\n"
        "  block_index  — block_index entry (key = height or hex hash)\n"
        "\nResult: { subsystem, captured_at, state: {...} }");

    const char *sub = json_get_str(json_at(params, 0));
    const struct json_value *key_val = json_at(params, 1);
    /* Accept either string or int for the key; numeric height callers
     * often pass `3091000` rather than `"3091000"`. */
    char key_buf[64] = {0};
    const char *key = NULL;
    if (key_val) {
        if (key_val->type == JSON_INT) {
            snprintf(key_buf, sizeof(key_buf), "%lld",
                     (long long)json_get_int(key_val));
            key = key_buf;
        } else if (key_val->type == JSON_STR) {
            key = json_get_str(key_val);
        }
    }

    if (!sub || !sub[0]) {
        json_set_str(result, "dumpstate: missing subsystem");
        LOG_FAIL("diag", "dumpstate: missing subsystem");
    }

    const struct dump_entry *e = NULL;
    const char *domain_key = NULL;
    for (size_t i = 0; i < sizeof(g_dumpers) / sizeof(g_dumpers[0]); i++) {
        if (strcmp(g_dumpers[i].name, sub) == 0) {
            e = &g_dumpers[i];
            break;
        }
    }
    if (!e && strncmp(sub, "supervisor.", strlen("supervisor.")) == 0) {
        e = &g_dumpers[0];
        domain_key = sub + strlen("supervisor.");
    }
    if (!e) {
        json_set_str(result, "dumpstate: unknown subsystem");
        LOG_FAIL("diag",
                 "dumpstate: unknown subsystem '%s' (try watchdog/boot/block_index)",
                 sub);
    }

    json_set_object(result);
    json_push_kv_str(result, "subsystem", domain_key ? sub : e->name);
    json_push_kv_str(result, "description", e->desc);
    json_push_kv_int(result, "captured_at", (int64_t)platform_time_wall_time_t());

    struct json_value state = {0};
    json_set_object(&state);
    bool ok = e->fn(&state, domain_key ? domain_key : key);
    if (!ok) {
        json_free(&state);
        json_set_str(result, "dumpstate: dump function returned false");
        LOG_FAIL("diag", "dumpstate: %s dump function returned false", sub);
    }
    json_push_kv(result, "state", &state);
    json_free(&state);
    return true;
}
