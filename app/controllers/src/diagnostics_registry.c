/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Diagnostics registry: the `g_dumpers[]` table plus the `dumpstate` /
 * `zcl_state` dispatcher. Adding a subsystem is one registry line plus one
 * dump function in the owning module.
 *
 * It also owns controller-level state (main_state + datadir) shared across
 * the diagnostics controller family.
 */

#include "platform/time_compat.h"
#include "controllers/agent_copy_prove_controller.h"
#include "controllers/agent_test_controller.h"
#include "controllers/block_intake_json.h"
#include "controllers/diagnostics_controller.h"
#include "controllers/diagnostics_internal.h"

#include "validation/main_state.h"
#include "validation/contextual_check_tx.h"
#include "validation/process_block.h"
#include "chain/chain.h"
#include "core/uint256.h"
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
#include "services/block_pruning_service.h"
#include "services/chain_evidence_authority_service.h"
#include "services/gap_fill_service.h"
#include "services/wallet_backup_service.h"
#include "services/consensus_reject_index.h"
#include "services/block_index_integrity.h"
#include "services/utxo_mirror_sync_service.h"
#include "services/mirror_divergence_locator.h"
#include "services/nullifier_backfill_service.h"
#include "services/consensus_state_publication_cas.h"
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
#include "services/sticky_escalator.h"
#include "services/invariant_sentinel.h"
#include "framework/condition.h"
#include "conditions/reducer_drive_watchdog.h"
#include "storage/block_index_projection.h"
#include "storage/mempool_projection.h"
#include "storage/peers_projection.h"
#include "storage/progress_store.h"
#include "config/boot.h"
#include "storage/small_projections.h"
#include "storage/utxo_projection.h"
#include "storage/znam_projection.h"
#include "storage/wallet_projection.h"
#include "crypto_registry/crypto_registry.h"
#include "hotswap/hotswap.h"
#include "services/ibd_throttle.h"
#include "services/mempool_limits.h"
#include "health/heartbeat.h"
#include "models/database.h"
#include "models/principal.h"
#include "models/auth_challenge.h"
#include "config/runtime.h"
#include "net/peer_lifecycle.h"
#include "net/https_server.h"
#include "net/tor_integration.h"
#include "util/log_macros.h"
#include "util/long_op.h"
#include "util/service_state.h"
#include "util/supervisor.h"
#include "util/blocker.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* ── Controller-level state ─────────────────────────────────────────
 *
 * The boot-populated main_state + datadir (diagnostics_controller_set_state /
 * diag_datadir / diag_main_state) moved to the RESIDENT diagnostics_dispatch.c.
 * This TU is Tier-1 hot-swap eligible (config/hotswap_eligible.def), so it must
 * hold NO mutable file-scope statics: a recompiled generation .so would get its
 * own zero-initialized copy and lose the live boot state. The dumpers here reach
 * that state through the resident accessors declared in diagnostics_internal.h. */

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
    json_push_kv_int(&obj, "missing_heights", v.missing_heights);
    json_push_kv_int(&obj, "first_missing_height", v.first_missing_height);
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
    (void)chain_evidence_drain_pending_tip(&authority);
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

/* ── explorer (clearnet HTTPS frontend) dump ───────────────────────
 *
 * One-call diagnosis of why the clearnet block-explorer is or isn't
 * serving. The HTTPS listener only binds when a TLS cert/key exist at
 * <datadir>/ssl/{fullchain,privkey}.pem at boot (see
 * boot_https_explorer_start in config/src/boot_frontend_services.c); a
 * missing cert silently leaves the node onion-only. This dumper surfaces
 * that condition without grepping node.log + ss + openssl by hand.
 *
 * Lives here because it needs diag_datadir; access()-only existence checks,
 * no sqlite and no cert parsing. */
static bool explorer_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;
    json_set_object(out);

    json_push_kv_bool(out, "https_started", https_server_is_running());
    json_push_kv_int(out, "https_port", (int64_t)https_server_port());
    json_push_kv_bool(out, "https_deferred", https_deferred_pending());

    const char *datadir = diag_datadir();
    char cert_path[1100], key_path[1100];
    snprintf(cert_path, sizeof(cert_path), "%s/ssl/fullchain.pem",
             datadir ? datadir : "");
    snprintf(key_path, sizeof(key_path), "%s/ssl/privkey.pem",
             datadir ? datadir : "");
    bool cert_present = (datadir && datadir[0] && access(cert_path, R_OK) == 0);
    bool key_present  = (datadir && datadir[0] && access(key_path, R_OK) == 0);
    json_push_kv_str(out, "cert_path", cert_path);
    json_push_kv_bool(out, "cert_present", cert_present);
    json_push_kv_bool(out, "key_present", key_present);

    json_push_kv_bool(out, "onion_enabled", tor_integration_is_enabled());
    const char *onion = tor_integration_get_onion_address();
    json_push_kv_str(out, "onion_address", onion ? onion : "");
    return true;
}

/* bundle_scan_seed_height / bundle_classify / bundle_staleness_dump_state_json
 * live in diagnostics_registry_bundle.c, a separate translation unit sized
 * per check-file-size-ceiling; decl in diagnostics_internal.h. Same
 * precedent as sapling_checkpoint_dump_state_json in
 * diagnostics_sapling_checkpoint.c. */

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

static const struct diagnostics_dump_entry g_dumpers[] = {
    { "supervisor", supervisor_dump_state_json,
                    "root supervisor: registered liveness contracts, ticks_run, stall_fires, deadlines" },
    { "blocker",    blocker_dump_state_json,
                    "typed blocker registry: active blockers by class {permanent,transient,dependency,resource}, "
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
    { "block_index", diag_block_index_dump_state_json,
                     "block_index entry by height or hash (in `key`)" },
    { "health",      health_dump_state_json,
                     "unified heartbeat ring: registered subsystems, ages, stall fires" },
    { "explorer",    explorer_dump_state_json,
                     "clearnet block-explorer frontend: https_started/port/deferred, "
                     "TLS cert_path + cert_present/key_present at <datadir>/ssl, "
                     "onion_enabled + onion_address. Diagnoses why the explorer is "
                     "or isn't serving on clearnet" },
    { "bundle_staleness", bundle_staleness_dump_state_json,
                     "fast-sync starter-pack freshness: seed height, block_index presence, network-tip gap, catch-up estimate, remint recommendation" },
    { "oracle",      zclassicd_oracle_dump_state_json,
                     "zclassicd oracle: drift-probe stats + RPC config" },
    { "header_probe", header_probe_dump_state_json,
                     "header probe: bulk header pull from co-located zclassicd via JSON-RPC" },
    { "header_band", diag_header_band_dump_state_json,
                     "header band backfill: derived island root, current getheaders anchor, remaining span, and blocker state" },
    { "utxo_parity", utxo_parity_dump_state_json,
                     "standing UTXO-set parity vs reference commitment at the finalized frontier: checks/matches/mismatches, finalized_frontier, last_checked_height, source name+exact flag" },
    { "legacy_mirror", legacy_mirror_sync_dump_state_json,
                     "legacy mirror: always-on lockstep catch-up from co-located zclassicd" },
    { "block_intake", controller_block_intake_dump_state_json,
                     "P2P block intake queue: async catch-up worker state, depth/capacity, backpressure saturation, accepted/retryable/rejected/drop counters" },
    { "oracle_policy", oracle_policy_dump_state_json,
                     "oracle policy: disagreement state machine (NORMAL / HALTED / PANIC)" },
    { "rolling_anchor", rolling_anchor_dump_state_json,
                     "rolling SHA3 anchor extension: runtime windows past compile-time prefix" },
    { "seal", seal_dump_state_json,
                     "seal ring (last 4): candidate/ratified state seals — height, coins_sha3, "
                     "utxo_count, supply, block_hash, ratified, sealed_at, self_sha3 valid" },
    { "progress",    progress_store_dump_state_json,
                     "progress.kv: open/path/stage_cursor row count" },
    { "mint_preflight", boot_mint_anchor_preflight_dump_state_json,
                     "last -mint-anchor producer preflight run (run_all): "
                     "have_report, all_ok, per-check {name,ok,why,remedy} — "
                     "datadir_lock_acquirable, "
                     "legacy_block_index_covers_anchor, "
                     "bodies_present_sampled, disk_headroom, "
                     "no_leftover_interrupted_run_artifacts, "
                     "fold_inram_memory_estimate" },
    { "refold",      refold_progress_dump_state_json,
                     "refold/mint mode: in_progress + from_anchor cached flags, compiled trusted_anchor, "
                     "durable progress.kv keys (durable_in_progress/from_anchor), from_anchor_target_tip" },
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
    { "reducer_drive", reducer_drive_dump_state_json,
                     "synchronous reducer/mint drive: active, label, age_us, "
                     "watchdog_threshold_secs, last_watchdog_fire_unix, "
                     "utxo_apply_cursor vs. the lagging coins_applied_height "
                     "(the -fold-inram batching gap)" },
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
    { "sticky_escalator", sticky_escalator_dump_state_json,
                     "always-terminating remedy ladder (sticky S2): armed state, "
                     "current rung, per-rung dispatch counts, witness window, "
                     "re-arm cooldown, ladder cycles, non-latching page count" },
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
    { "sapling_checkpoint", sapling_checkpoint_dump_state_json,
                     "flat-file Sapling note-commitment tree cache: periodic write "
                     "height/count/fails + boot load outcome (absent/loaded_verified/discarded)" },
    { "agent_copy_prove", agent_copy_prove_dump_state_json,
                     "copy-prove run launched via agentcopyprove: queued/running/done state, verdict, "
                     "h_star before/after, tip_regression, copy_path (key = the run's slug)" },
    { "agent_test", agent_test_dump_state_json,
                     "allowlisted test run launched via agenttest: queued/running/done state, "
                     "verdict (PASS/FAIL/NO_MATCH/ERROR), exit_code, output tail "
                     "(key = \"<kind>-<name>\", kind = test_group|scenario)" },
    { "unhealthy",   unhealthy_dump_state_json,
                     "unhealthy-only rollup: all_ok/checked/reporting + unhealthy subsystems (name+reason) from `_health`" },
    { "hotswap",     hotswap_dump_state_json,
                     "DEV-ONLY in-process hot-swap loader: availability + per-generation {gen, so_path, loaded_at, replaced_count, ok}; 'unavailable' in release" },
    { "gap_fill",    gap_fill_dump_state_json,
                     "IBD gap-fill worker: running, pass/enqueue/idle/corrupt-walk + timeout-sweep counters, last tip/window bounds" },
    { "wallet_backup", wallet_backup_dump_state_json,
                     "periodic wallet-table backup thread: running, total_runs/failures, last run time/size/key_count/duration/path/error" },
    { "consensus_reject_index", consensus_reject_index_dump_state_json,
                     "hash-keyed ring of recent consensus tx/block rejections: running, total/count/capacity, recent {hash,kind,dos,ts_us,reason}" },
    { "block_index_integrity", block_index_integrity_dump_state_json,
                     "block_index.bin sidecar integrity: last verify verdict + recovery action, degraded/unsafe_override, reason, heights_repaired" },
    { "utxo_mirror_sync", utxo_mirror_sync_dump_state_json,
                     "node.db `utxos` explorer mirror worker: state, rebuilds_run, rows_written, last_mirror_height, last_frontier, last_pass/error_unix" },
    { "mirror_divergence", mirror_divergence_dump_state_json,
                     "mirror hash-disagreement bisect locator: last_locate_unix, last_first_div, probes_last_run, divergence_latched, pending record" },
    { "nullifier_backfill", nullifier_backfill_dump_state_json,
                     "owner-gated C-3 nullifier gap backfill: durable activation/resume cursors, derived status, gap_blocker_active" },
    { "publication_cas", consensus_state_publication_cas_dump_state_json,
                     "contained consensus-state publication CAS: latest decision (ADMIT/typed refusal), "
                     "bound artifact/chain/source/epoch digests, bundle H/hash, expected frontier H/hash, decision digest" },
    { "principals", principal_dump_state_json,
                     "multi-user-server identity registry: count + public projection of each principal {address, role, status, key_kind, last_login, has_znam}" },
    { "auth",       auth_challenge_dump_state_json,
                     "auth login-challenge nonce store: db_open + pending (unconsumed) single-use challenge count" },
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

size_t diagnostics_dumper_count(void) { return sizeof(g_dumpers) / sizeof(g_dumpers[0]); }

const struct diagnostics_dump_entry *diagnostics_dumper_at(size_t idx)
{
    return idx < diagnostics_dumper_count() ? &g_dumpers[idx] : NULL;
}
const char *diagnostics_oracle_owner_file(void) { return "app/services/src/zclassicd_oracle_service.c"; }

static void diagnostics_dumpstate_help(struct json_value *result)
{
    char known[4096];
    diagnostics_subsystems_csv(known, sizeof(known));

    char text[8192];
    snprintf(text, sizeof(text),
             "dumpstate <subsystem> [key]\n"
             "\nDump in-process state for a subsystem. Known subsystems:\n"
             "  %s\n"
             "\nResult: { subsystem, description, captured_at, state: {...} }",
             known);
    json_set_str(result, text);
}

static bool diagnostics_dumpstate_unknown(struct json_value *result,
                                          const char *sub)
{
    char known[4096];
    diagnostics_subsystems_csv(known, sizeof(known));

    char msg[8192];
    snprintf(msg, sizeof(msg),
             "dumpstate: unknown subsystem '%s'; known_subsystems=%s",
             sub ? sub : "", known);
    json_set_str(result, msg);
    LOG_FAIL("diag", "%s", msg);
}

bool diag_rpc_dumpstate_builtin(const struct json_value *params, bool help,
                                struct json_value *result)
{
    if (help) {
        diagnostics_dumpstate_help(result);
        return true;
    }

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

    const struct diagnostics_dump_entry *e = NULL;
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
        return diagnostics_dumpstate_unknown(result, sub);
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

/* ── Hot-swap generation entrypoint ─────────────────────────────────
 *
 * This TU (the g_dumpers[] table + the dumpers defined here) is Tier-1
 * hot-swap eligible (config/hotswap_eligible.def). Under a generation .so build
 * (-DZCL_HOTSWAP_GEN) the macro emits zcl_hotswap_gen_init, which re-points the
 * resident `dumpstate` provider at THIS TU's freshly-compiled
 * diag_rpc_dumpstate_builtin (and its recompiled g_dumpers[] + dumpers).
 * diag_dumpstate_replace lives in the resident diagnostics_dispatch.c, so it
 * binds to the executable's copy and mutates the provider the live read path
 * reads; the boot-populated main_state/datadir are reached through the resident
 * accessors, never a stale .so-local copy. In the node build and in release the
 * macro expands to nothing (no trailing semicolon by design). */
ZCL_HOTSWAP_EXPORT_PROVIDER(
    diag_dumpstate_replace(diag_rpc_dumpstate_builtin))
