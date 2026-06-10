/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Event log and sync state machine RPCs.
 * Canonical source for: eventlog, syncstate */

#include "controllers/event_controller.h"
#include "controllers/strong_params.h"
#include "config/boot.h"
#include "services/node_health_service.h"
#include "services/bg_validation_service.h"
#include "services/block_index_integrity.h"
#include "services/block_source_policy.h"
#include "services/chain_evidence_authority_service.h"
#include "services/chain_state_service.h"
#include "services/legacy_mirror_sync_service.h"
#include "event/event.h"
#include "sync/sync_state.h"
#include "json/json.h"
#include "rpc/server.h"
#include "config/runtime.h"
#include <stdlib.h>
#include <string.h>
#include "util/clientversion.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

static void push_chain_evidence_health_json(struct json_value *checks)
{
    struct chain_evidence_controller cec;
    struct chain_evidence_controller_view view;
    struct json_value ce = {0};

    chain_evidence_controller_init(&cec, app_runtime_node_db(),
                                   csr_instance());
    chain_evidence_controller_snapshot(&cec, &view);

    json_set_object(&ce);
    json_push_kv_str(&ce, "state",
                     chain_evidence_controller_state_name(view.state));
    json_push_kv_str(&ce, "publish_state",
                     chain_evidence_publish_state_name(view.publish_state));
    json_push_kv_str(&ce, "active_tip_source_class",
                     chain_evidence_source_class_name(
                         view.active_tip_source_class));
    json_push_kv_int(&ce, "active_tip",
                     (int64_t)view.active_tip_height);
    json_push_kv_int(&ce, "header_tip",
                     (int64_t)view.header_tip_height);
    json_push_kv_int(&ce, "persisted_active_tip",
                     (int64_t)view.persisted_active_tip_height);
    json_push_kv_int(&ce, "utxo_max_height",
                     (int64_t)view.utxo_max_height);
    json_push_kv_int(&ce, "coins_best_block_height",
                     (int64_t)view.coins_best_block_height);
    json_push_kv_int(&ce, "csr_sqlite_max_height",
                     (int64_t)view.sqlite_max_height);
    json_push_kv_bool(&ce, "missing_active_tip_evidence",
                      view.missing_active_tip_evidence);
    json_push_kv_bool(&ce, "publish_state_not_local",
                      view.publish_state_not_local);
    json_push_kv_bool(&ce, "active_tip_hash_mismatch",
                      view.active_tip_hash_mismatch);
    json_push_kv_bool(&ce, "csr_cursor_mismatch",
                      view.csr_cursor_mismatch);
    json_push_kv_bool(&ce, "repaired_active_tip_evidence",
                      view.repaired_active_tip_evidence);
    if (view.health_reason[0])
        json_push_kv_str(&ce, "health_reason", view.health_reason);
    if (view.contradiction_reason[0])
        json_push_kv_str(&ce, "contradiction_reason",
                         view.contradiction_reason);
    json_push_kv(checks, "chain_evidence", &ce);
    json_free(&ce);
}

static bool rpc_eventlog(const struct json_value *params, bool help,
                         struct json_value *result)
{
    RPC_HELP(help, result,
        "eventlog ( count )\n"
        "\nReturn recent events from the system event log.\n"
        "Every P2P message, state transition, block validation,\n"
        "and error is captured in a lock-free ring buffer.\n"
        "\nArguments:\n"
        "1. count     (numeric, optional, default=200) Number of events\n"
        "\nResult:\n"
        "  { \"sync_state\": \"...\", \"events\": [...] }\n");

    int count = 200;
    if (params && params->type == JSON_ARR && params->num_children > 0) {
        const struct json_value *v = &params->children[0];
        if (v->type == JSON_INT) count = (int)v->val.i;
        else if (v->type == JSON_REAL) count = (int)v->val.d;
    }
    if (count < 1) count = 1;
    if (count > 65536) count = 65536;

    size_t buf_size = (size_t)count * 256 + 256;
    if (buf_size > 16 * 1024 * 1024) buf_size = 16 * 1024 * 1024;
    char *buf = zcl_malloc(buf_size, "eventlog json buf");
    if (!buf) {
        json_set_str(result, "out of memory");
        return false;
    }

    size_t w = 0;
    w += (size_t)snprintf(buf + w, 256, "{\"sync_state\":\"%s\",\"events\":",
                           sync_state_name(sync_get_state()));
    w += event_dump_json(buf + w, buf_size - w, (size_t)count);
    if (w + 1 < buf_size) buf[w++] = '}';
    buf[w] = '\0';

    json_read(result, buf, w);
    free(buf);
    return true;
}

/* Last N chain.reorg_* events from the ring buffer. Same shape as
 * eventlog, but filtered to the reorg family — exposes the on-the-wire
 * EV_REORG_START / EV_REORG_DISCONNECT_FAILED / EV_REORG_RECOVERY_COMPLETE
 * stream without parsing all 200+ general events client-side. */
static bool rpc_getreorghistory(const struct json_value *params, bool help,
                                 struct json_value *result)
{
    RPC_HELP(help, result,
        "getreorghistory ( count )\n"
        "\nReturn recent chain.reorg_* events from the system event log.\n"
        "\nArguments:\n"
        "1. count     (numeric, optional, default=50) Max events\n"
        "\nResult:\n"
        "  { \"sync_state\": \"...\", \"reorgs\": [...] }\n");

    int count = 50;
    if (params && params->type == JSON_ARR && params->num_children > 0) {
        const struct json_value *v = &params->children[0];
        if (v->type == JSON_INT) count = (int)v->val.i;
        else if (v->type == JSON_REAL) count = (int)v->val.d;
    }
    if (count < 1) count = 1;
    if (count > 1024) count = 1024;

    size_t buf_size = (size_t)count * 256 + 256;
    char *buf = zcl_malloc(buf_size, "reorghistory json buf");
    if (!buf) {
        json_set_str(result, "out of memory");
        return false;
    }

    size_t w = 0;
    w += (size_t)snprintf(buf + w, 256, "{\"sync_state\":\"%s\",\"reorgs\":",
                           sync_state_name(sync_get_state()));
    w += event_dump_json_filtered(buf + w, buf_size - w,
                                   (size_t)count, "chain.reorg_");
    if (w + 1 < buf_size) buf[w++] = '}';
    buf[w] = '\0';

    json_read(result, buf, w);
    free(buf);
    return true;
}

static bool rpc_syncstate(const struct json_value *params, bool help,
                          struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "syncstate\n"
        "\nReturn the current sync state machine state.\n"
        "\nResult:\n"
        "  { \"state\": \"...\", \"state_id\": N }\n");

    json_set_object(result);
    json_push_kv_str(result, "state", sync_state_name(sync_get_state()));
    json_push_kv_int(result, "state_id", (int64_t)sync_get_state());
    json_push_kv_bool(result, "utxo_replay_active",
                      atomic_load(&g_utxo_replay_active));
    json_push_kv_int(result, "utxo_replay_height",
                     (int64_t)atomic_load(&g_utxo_replay_height));

    struct bii_recovery_status bii;
    bii_get_recovery_status(&bii);
    struct json_value bi = {0};
    json_set_object(&bi);
    json_push_kv_str(&bi, "verdict", bii_verdict_name(bii.verdict));
    json_push_kv_str(&bi, "action", bii_recovery_action_name(bii.action));
    json_push_kv_bool(&bi, "degraded", bii.degraded);
    json_push_kv_bool(&bi, "unsafe_override", bii.unsafe_override);
    json_push_kv_int(&bi, "last_check_unix", bii.unix_time);
    if (bii.reason[0])
        json_push_kv_str(&bi, "reason", bii.reason);
    json_push_kv(result, "block_index_integrity", &bi);
    json_free(&bi);
    return true;
}

static bool rpc_healthcheck(const struct json_value *params, bool help,
                             struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "healthcheck\n"
        "\nReturn node health status — single pass/fail for monitoring.\n"
        "\nResult:\n"
        "  { \"healthy\": true/false, \"build_commit\": \"...\",\n"
        "    \"sync_state\": \"...\", \"checks\": { ... } }\n");

    json_set_object(result);

    struct node_health_snapshot health;
    node_health_collect(&health, NULL, NULL);
    json_push_kv_str(result, "build_commit", zcl_build_commit());
    json_push_kv_str(result, "sync_state", sync_state_name(health.sync_state));

    /* Individual health checks */
    struct json_value checks = {0};
    json_set_object(&checks);

    json_push_kv_bool(&checks, "synced", health.synced);
    json_push_kv_bool(&checks, "has_peers", health.has_peers);
    json_push_kv_bool(&checks, "tor_enabled", health.tor_enabled);
    json_push_kv_bool(&checks, "tor_ready", health.tor_ready);
    json_push_kv_bool(&checks, "onion_service_ready",
                      health.onion_service_ready);
    json_push_kv_bool(&checks, "tip_stale", health.tip_stale);
    json_push_kv_bool(&checks, "queue_backed_up", health.queue_backed_up);
    json_push_kv_int(&checks, "peer_count", (int64_t)health.peer_count);
    json_push_kv_int(&checks, "tip_lag", (int64_t)health.tip_lag);
    /* Prime Directive: health = network_tip minus the reducer log head. */
    json_push_kv_int(&checks, "log_head", (int64_t)health.log_head);
    json_push_kv_int(&checks, "log_head_gap", (int64_t)health.log_head_gap);
    json_push_kv_int(&checks, "error_total", health.error_total);
    json_push_kv_int(&checks, "last_error_age_seconds",
                     health.last_error_age_seconds);
    json_push_kv_bool(&checks, "last_error_recent",
                      health.last_error_recent);
    if (health.last_error_type[0])
        json_push_kv_str(&checks, "last_error_type",
                         health.last_error_type);
    if (health.last_error[0])
        json_push_kv_str(&checks, "last_error", health.last_error);
    if (health.onion_address[0])
        json_push_kv_str(&checks, "onion_address", health.onion_address);
    if (health.degraded_reason[0])
        json_push_kv_str(&checks, "degraded_reason", health.degraded_reason);
    json_push_kv_bool(&checks, "operator_needed", health.operator_needed);
    if (health.operator_needed && health.operator_needed_detail[0])
        json_push_kv_str(&checks, "operator_needed_detail",
                         health.operator_needed_detail);
    struct bii_recovery_status bii;
    bii_get_recovery_status(&bii);
    if (bii.degraded)
        json_push_kv_str(&checks, "block_index_integrity",
                         bii_recovery_action_name(bii.action));
    push_chain_evidence_health_json(&checks);
    {
        struct cac_decision d;
        struct json_value ca = {0};

        block_source_policy_get_status(&d);
        json_set_object(&ca);
        json_push_kv_str(&ca, "authority", "local_consensus_validation");
        json_push_kv_str(&ca, "decision",
                         cac_decision_result_name(d.result));
        json_push_kv_str(&ca, "selected_source",
                         cac_source_name(d.selected_source));
        json_push_kv_str(&ca, "selected_source_trust",
                         cac_source_trust_name(d.selected_source));
        json_push_kv_bool(&ca, "activation_allowed", d.activation_allowed);
        json_push_kv_bool(&ca, "mirror_fallback_allowed",
                          d.mirror_fallback_allowed);
        json_push_kv_int(&ca, "local_height", d.local_height);
        json_push_kv_int(&ca, "best_header_height", d.best_header_height);
        json_push_kv_int(&ca, "target_height", d.target_height);
        json_push_kv_int(&ca, "projection_height",
                         d.projection_height);
        json_push_kv_int(&ca, "projection_lag", d.projection_lag);
        json_push_kv_bool(&ca, "projection_deferred",
                          d.projection_deferred);
        json_push_kv_str(&ca, "projection_state", d.projection_state);
        json_push_kv_int(&ca, "projection_deferred_total",
                         d.projection_deferred_total);
        json_push_kv_int(&ca, "last_projection_deferred_height",
                         d.last_projection_deferred_height);
        json_push_kv_int(&ca, "last_projection_deferred_time",
                         d.last_projection_deferred_time);
        json_push_kv_str(&ca, "last_projection_deferred_reason",
                         d.last_projection_deferred_reason);
        if (d.selected_source > CAC_SOURCE_NONE &&
            d.selected_source < CAC_SOURCE_NUM) {
            const struct cac_source_status *s = &d.sources[d.selected_source];
            json_push_kv_bool(&ca, "selected_source_selectable",
                              s->selectable);
            json_push_kv_str(&ca, "selected_source_selection_blocker",
                             s->selection_reason);
            json_push_kv_int(&ca, "selected_source_score_base",
                             s->score_base);
            json_push_kv_int(&ca, "selected_source_score_health",
                             s->score_health);
            json_push_kv_int(&ca, "selected_source_score_height",
                             s->score_height);
            json_push_kv_int(&ca, "selected_source_score_authorized",
                             s->score_authorized);
            json_push_kv_int(&ca,
                             "selected_source_score_target_lag_penalty",
                             s->score_target_lag_penalty);
            json_push_kv_int(&ca, "selected_source_score_failure_penalty",
                             s->score_failure_penalty);
            json_push_kv_int(&ca,
                             "selected_source_score_mirror_gate_penalty",
                             s->score_mirror_gate_penalty);
        }
        json_push_kv_str(&ca, "reason", d.reason);
        json_push_kv_str(&ca, "blocker", d.blocker);
        {
            struct json_value dump = {0};
            if (block_source_policy_dump_state_json(&dump, NULL)) {
                const struct json_value *has_last =
                    json_get(&dump, "has_last_decision");
                const struct json_value *last =
                    json_get(&dump, "last_decision");
                const struct json_value *sources =
                    json_get(&dump, "sources");
                const struct json_value *initialized =
                    json_get(&dump, "initialized");
                const struct json_value *has_connman =
                    json_get(&dump, "has_connman");
                const struct json_value *has_main_state =
                    json_get(&dump, "has_main_state");
                const struct json_value *has_node_db =
                    json_get(&dump, "has_node_db");
                if (initialized)
                    json_push_kv(&ca, "initialized", initialized);
                if (has_connman)
                    json_push_kv(&ca, "has_connman", has_connman);
                if (has_main_state)
                    json_push_kv(&ca, "has_main_state", has_main_state);
                if (has_node_db)
                    json_push_kv(&ca, "has_node_db", has_node_db);
                if (has_last)
                    json_push_kv(&ca, "has_last_decision", has_last);
                if (last)
                    json_push_kv(&ca, "last_decision", last);
                if (sources)
                    json_push_kv(&ca, "sources", sources);
            }
            json_free(&dump);
        }
        json_push_kv(&checks, "chain_advance", &ca);
        json_free(&ca);
    }
    {
        struct legacy_mirror_sync_stats ms;
        legacy_mirror_sync_stats_snapshot(&ms);
        json_push_kv_str(result, "consensus_authority",
                         "local_consensus_validation");
        json_push_kv_str(result, "candidate_source", "legacy_advisory");
        json_push_kv_str(result, "candidate_trust", ms.candidate_trust);
        json_push_kv_int(result, "candidate_lag", ms.lag);
        json_push_kv_str(result, "candidate_blocker",
                         ms.activation_blocker_reason[0] ? ms.activation_blocker_reason
                                                  : ms.last_blocker_id);
        json_push_kv_str(result, "mirror_activation_blocker",
                         ms.activation_blocker_reason);
        json_push_kv_str(result, "mirror_last_blocker_code",
                         ms.last_blocker_id);
        json_push_kv_int(result, "mirror_blockers_total",
                         ms.blockers_total);
        json_push_kv_int(result, "mirror_unsafe_overrides_total",
                         ms.unsafe_overrides_total);
        json_push_kv_int(result, "mirror_stalls_total",
                         ms.stalls_total);
        json_push_kv_int(result, "mirror_overrides_total",
                         ms.overrides_total);
        json_push_kv_bool(result, "mirror_last_override_safe",
                          ms.last_override_safe);
        json_push_kv_str(result, "mirror_last_override_reason",
                         ms.last_override_reason);
        json_push_kv_str(result, "mirror_last_override_scope",
                         ms.last_override_scope);
    }
    json_push_kv_int(&checks, "memory_rss_mb", health.memory_rss_mb);
    json_push_kv_int(&checks, "uptime_seconds", health.uptime_seconds);
    json_push_kv_int(&checks, "tip_advance_age_seconds",
                     health.tip_advance_age_seconds);

    json_push_kv_bool(result, "healthy", health.healthy);
    json_push_kv(result, "checks", &checks);
    json_free(&checks);

    return true;
}

static bool rpc_validationstatus(const struct json_value *params, bool help,
                                 struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "validationstatus\n"
        "\nReturn background full validation progress.\n"
        "\nAfter fast sync (FlyClient + SHA3), the node verifies every\n"
        "historical signature, zk-SNARK proof, and Equihash solution\n"
        "in a background thread using parallel script verification.\n"
        "\nResult:\n"
        "  { \"state\": \"...\", \"verified_height\": N, \"chain_height\": N,\n"
        "    \"percent\": N.N, \"sigs_verified\": N, \"proofs_verified\": N,\n"
        "    \"blocks_per_sec\": N }\n");

    json_set_object(result);

    if (!g_bg_validation) {
        json_push_kv_str(result, "state", "not_initialized");
        return true;
    }

    struct bg_validation_progress p = bg_validation_get_progress(g_bg_validation);
    json_push_kv_str(result, "state",
                     bg_validation_state_name((enum bg_validation_state)p.state));
    json_push_kv_int(result, "verified_height", (int64_t)p.verified_height);
    json_push_kv_int(result, "chain_height", (int64_t)p.chain_height);

    double pct = 0.0;
    if (p.chain_height > 0)
        pct = 100.0 * (double)(p.verified_height + 1) / (double)(p.chain_height + 1);
    /* Format as fixed-point integer (10x percent) for JSON compatibility */
    json_push_kv_int(result, "percent_x10", (int64_t)(pct * 10.0));

    json_push_kv_int(result, "sigs_verified", p.sigs_verified);
    json_push_kv_int(result, "proofs_verified", p.proofs_verified);
    json_push_kv_int(result, "blocks_per_sec", p.blocks_per_sec);

    return true;
}

static bool rpc_resetvalidation(const struct json_value *params, bool help,
                                struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "resetvalidation\n"
        "\nReset background validation and re-verify all blocks from genesis.\n"
        "\nResult:\n"
        "  { \"reset\": true }\n");

    json_set_object(result);
    if (g_bg_validation) {
        bg_validation_reset(g_bg_validation);
        json_push_kv_bool(result, "reset", true);
    } else {
        json_push_kv_bool(result, "reset", false);
        json_push_kv_str(result, "error", "bg_validation not initialized");
    }
    return true;
}

void register_event_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "control", "eventlog",          rpc_eventlog,          true },
        { "control", "getreorghistory",   rpc_getreorghistory,   true },
        { "control", "syncstate",         rpc_syncstate,         true },
        { "control", "healthcheck",       rpc_healthcheck,       true },
        { "control", "validationstatus",  rpc_validationstatus,  true },
        { "control", "resetvalidation",   rpc_resetvalidation,   true },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_must_append(t, &cmds[i]);
}
