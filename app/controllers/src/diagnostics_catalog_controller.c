/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Machine-readable catalog for dumpstate / zcl_state. The source of truth for
 * available subsystems stays in diagnostics_registry.c; this file adds routing
 * metadata for agents without growing the registry dispatcher.
 */

#include "controllers/diagnostics_internal.h"
#include "controllers/strong_params.h"

#include "json/json.h"
#include "rpc/server.h"
#include "util/clientversion.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool diagnostics_catalog_is_stage(const char *name)
{
    static const char *const stages[] = {
        "header_admit", "validate_headers", "body_fetch", "body_persist",
        "script_validate", "proof_validate", "utxo_apply", "tip_finalize",
        "reducer_frontier",
    };
    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
        if (strcmp(name, stages[i]) == 0)
            return true;
    }
    return false;
}

static bool diagnostics_catalog_accepts_key(const char *name)
{
    return name && (strcmp(name, "block_index") == 0 ||
                    strcmp(name, "supervisor") == 0);
}

static const char *diagnostics_catalog_key_hint(const char *name)
{
    if (!name)
        return "";
    if (strcmp(name, "block_index") == 0)
        return "height or 64-char block hash";
    if (strcmp(name, "supervisor") == 0)
        return "child service name, or use subsystem=supervisor.<name>";
    return "";
}

static const char *diagnostics_catalog_cost(const char *name)
{
    if (!name)
        return "cheap";
    if (strcmp(name, "block_index") == 0 ||
        strcmp(name, "bundle_staleness") == 0 ||
        strstr(name, "_projection") != NULL)
        return "bounded_lookup";
    if (strcmp(name, "seal") == 0 ||
        strcmp(name, "progress") == 0 ||
        strcmp(name, "chain_evidence") == 0 ||
        strcmp(name, "chain_evidence_controller") == 0)
        return "projection_summary";
    return "cheap";
}

static const char *diagnostics_catalog_freshness(const char *name)
{
    if (!name)
        return "in_process_snapshot";
    if (strstr(name, "_projection") != NULL ||
        strcmp(name, "progress") == 0 ||
        strcmp(name, "seal") == 0 ||
        strcmp(name, "block_index") == 0)
        return "persisted_projection_snapshot";
    if (strcmp(name, "bundle_staleness") == 0)
        return "filesystem_snapshot";
    return "in_process_snapshot";
}

static const char *diagnostics_catalog_owner_shape(const char *name)
{
    if (!name)
        return "controller";
    if (diagnostics_catalog_is_stage(name) ||
        strcmp(name, "coin_backfill") == 0 ||
        strcmp(name, "refold") == 0)
        return "job";
    if (strstr(name, "_projection") != NULL ||
        strcmp(name, "progress") == 0 ||
        strcmp(name, "block_index") == 0)
        return "projection";
    if (strstr(name, "condition") != NULL ||
        strstr(name, "watchdog") != NULL ||
        strcmp(name, "blocker") == 0 ||
        strcmp(name, "canary_watch") == 0 ||
        strcmp(name, "sticky_escalator") == 0 ||
        strcmp(name, "validation_pack") == 0)
        return "condition";
    if (strcmp(name, "supervisor") == 0 ||
        strcmp(name, "long_op") == 0 ||
        strcmp(name, "service_state") == 0 ||
        strcmp(name, "health") == 0)
        return "runtime";
    return "service";
}

static const char *diagnostics_catalog_state_class(const char *name)
{
    if (!name)
        return "diagnostic";
    if (diagnostics_catalog_is_stage(name))
        return "reducer_stage";
    if (strstr(name, "_projection") != NULL)
        return "projection";
    if (strstr(name, "condition") != NULL ||
        strstr(name, "watchdog") != NULL ||
        strcmp(name, "blocker") == 0 ||
        strcmp(name, "sticky_escalator") == 0 ||
        strcmp(name, "canary_watch") == 0)
        return "condition_or_blocker";
    if (strcmp(name, "block_index") == 0 ||
        strcmp(name, "progress") == 0 ||
        strcmp(name, "seal") == 0)
        return "storage";
    if (strstr(name, "peer") != NULL ||
        strstr(name, "oracle") != NULL ||
        strcmp(name, "chain_evidence") == 0 ||
        strcmp(name, "chain_evidence_controller") == 0)
        return "chain_or_network";
    if (strcmp(name, "supervisor") == 0 ||
        strcmp(name, "health") == 0 ||
        strcmp(name, "service_state") == 0 ||
        strcmp(name, "long_op") == 0)
        return "runtime";
    return "diagnostic";
}

static const char *diagnostics_catalog_owner_file(const char *name)
{
    if (!name)
        return "app/controllers/src/diagnostics_registry.c";
    if (strcmp(name, "supervisor") == 0)
        return "lib/util/src/supervisor.c";
    if (strcmp(name, "blocker") == 0)
        return "lib/util/src/blocker.c";
    if (strcmp(name, "watchdog") == 0 ||
        strcmp(name, "condition_engine") == 0)
        return "lib/framework/src/condition.c";
    if (strcmp(name, "boot") == 0)
        return "app/services/src/chain_restore_boot_snapshot.c";
    if (strcmp(name, "service_state") == 0)
        return "lib/util/src/service_state.c";
    if (strcmp(name, "block_index") == 0 ||
        strcmp(name, "header_band") == 0)
        return "app/controllers/src/diagnostics_block_index.c";
    if (strcmp(name, "health") == 0)
        return "lib/health/src/heartbeat.c";
    if (strcmp(name, "explorer") == 0 ||
        strcmp(name, "bundle_staleness") == 0 ||
        strcmp(name, "chain_evidence") == 0 ||
        strcmp(name, "chain_evidence_controller") == 0)
        return "app/controllers/src/diagnostics_registry.c";
    if (strcmp(name, "oracle") == 0)
        return diagnostics_oracle_owner_file();
    if (strcmp(name, "header_probe") == 0)
        return "app/services/src/header_probe.c";
    if (strcmp(name, "utxo_parity") == 0)
        return "app/services/src/utxo_parity_service.c";
    if (strcmp(name, "legacy_mirror") == 0)
        return "app/services/src/legacy_mirror_sync_service.c";
    if (strcmp(name, "block_intake") == 0)
        return "app/controllers/src/block_intake_json.c";
    if (strcmp(name, "oracle_policy") == 0)
        return "app/services/src/oracle_policy.c";
    if (strcmp(name, "rolling_anchor") == 0)
        return "app/services/src/rolling_anchor_service.c";
    if (strcmp(name, "seal") == 0)
        return "app/services/src/seal_service.c";
    if (strcmp(name, "progress") == 0)
        return "lib/storage/src/progress_store.c";
    if (strcmp(name, "refold") == 0)
        return "app/jobs/src/refold_progress.c";
    if (strcmp(name, "reducer_frontier") == 0)
        return "app/jobs/src/reducer_frontier_dump.c";
    if (strcmp(name, "header_admit") == 0)
        return "app/jobs/src/header_admit_stage.c";
    if (strcmp(name, "validate_headers") == 0)
        return "app/jobs/src/validate_headers_stage.c";
    if (strcmp(name, "body_fetch") == 0)
        return "app/jobs/src/body_fetch_stage.c";
    if (strcmp(name, "body_persist") == 0)
        return "app/jobs/src/body_persist_stage.c";
    if (strcmp(name, "script_validate") == 0)
        return "app/jobs/src/script_validate_stage_dump.c";
    if (strcmp(name, "proof_validate") == 0)
        return "app/jobs/src/proof_validate_stage_dump.c";
    if (strcmp(name, "utxo_apply") == 0)
        return "app/jobs/src/utxo_apply_stage_dump.c";
    if (strcmp(name, "tip_finalize") == 0)
        return "app/jobs/src/tip_finalize_stage.c";
    if (strcmp(name, "coin_backfill") == 0)
        return "app/jobs/src/stage_repair_coin_backfill_util.c";
    if (strcmp(name, "quorum_oracle") == 0)
        return "app/services/src/quorum_oracle_service.c";
    if (strcmp(name, "peer_lifecycle") == 0)
        return "lib/net/src/peer_lifecycle.c";
    if (strcmp(name, "chain_advance_coordinator") == 0)
        return "app/services/src/block_source_policy_status.c";
    if (strcmp(name, "chain_tip_watchdog") == 0)
        return "app/services/src/chain_tip_watchdog.c";
    if (strcmp(name, "sticky_escalator") == 0)
        return "app/services/src/sticky_escalator.c";
    if (strcmp(name, "long_op") == 0)
        return "lib/util/src/long_op.c";
    if (strcmp(name, "ibd_throttle") == 0)
        return "app/services/src/ibd_throttle.c";
    if (strcmp(name, "mempool_limits") == 0)
        return "app/services/src/mempool_limits.c";
    if (strcmp(name, "block_pruning") == 0)
        return "app/services/src/block_pruning_service.c";
    if (strcmp(name, "crypto_registry") == 0)
        return "lib/crypto_registry/src/crypto_registry.c";
    if (strcmp(name, "mempool_projection") == 0)
        return "lib/storage/src/mempool_projection.c";
    if (strcmp(name, "peers_projection") == 0)
        return "lib/storage/src/peers_projection.c";
    if (strcmp(name, "utxo_projection") == 0)
        return "lib/storage/src/utxo_projection.c";
    if (strcmp(name, "znam_projection") == 0)
        return "lib/storage/src/znam_projection.c";
    if (strcmp(name, "wallet_projection") == 0)
        return "lib/storage/src/wallet_projection.c";
    if (strcmp(name, "contacts_projection") == 0)
        return "lib/storage/src/contacts_projection.c";
    if (strcmp(name, "onion_announcements_projection") == 0)
        return "lib/storage/src/onion_announcements_projection.c";
    if (strcmp(name, "hodl_history_projection") == 0)
        return "lib/storage/src/hodl_history_projection.c";
    if (strcmp(name, "block_index_projection") == 0)
        return "lib/storage/src/block_index_projection.c";
    if (strcmp(name, "validation_pack") == 0)
        return "app/services/src/invariant_sentinel.c";
    if (strcmp(name, "soak") == 0)
        return "app/services/src/soak_attestation_service.c";
    if (strcmp(name, "canary_watch") == 0)
        return "app/services/src/canary_sentinel_watch.c";
    if (strcmp(name, "bg_validation") == 0)
        return "app/services/src/bg_validation_dump.c";
    if (strcmp(name, "disk_monitor") == 0)
        return "app/services/src/disk_monitor.c";
    if (strcmp(name, "sync_monitor") == 0)
        return "app/services/src/sync_monitor.c";
    if (strcmp(name, "db_maintenance") == 0)
        return "app/services/src/db_maintenance.c";
    return "app/controllers/src/diagnostics_registry.c";
}

static const char *diagnostics_catalog_safety_level(const char *name)
{
    (void)name;
    return "read_only";
}

static const char *diagnostics_catalog_primary_test(const char *name)
{
    if (!name)
        return "lib/test/src/test_syncdiag_rpc.c";
    if (strcmp(name, "block_index") == 0 ||
        strcmp(name, "header_band") == 0)
        return "lib/test/src/test_block_index_integrity.c";
    if (strcmp(name, "reducer_frontier") == 0)
        return "lib/test/src/test_reducer_frontier.c";
    if (strcmp(name, "supervisor") == 0)
        return "lib/test/src/test_supervisor.c";
    if (strcmp(name, "condition_engine") == 0 ||
        strcmp(name, "watchdog") == 0)
        return "lib/test/src/test_condition_engine.c";
    if (strcmp(name, "chain_advance_coordinator") == 0)
        return "lib/test/src/test_chain_advance_coordinator.c";
    if (strcmp(name, "peer_lifecycle") == 0)
        return "lib/test/src/test_peer_lifecycle.c";
    if (strcmp(name, "mempool_projection") == 0)
        return "lib/test/src/test_mempool_projection.c";
    if (strcmp(name, "peers_projection") == 0)
        return "lib/test/src/test_peers_projection.c";
    if (strcmp(name, "utxo_projection") == 0)
        return "lib/test/src/test_utxo_projection.c";
    if (strcmp(name, "znam_projection") == 0)
        return "lib/test/src/test_znam_projection.c";
    if (strcmp(name, "wallet_projection") == 0)
        return "lib/test/src/test_wallet_projection.c";
    if (strcmp(name, "contacts_projection") == 0 ||
        strcmp(name, "onion_announcements_projection") == 0 ||
        strcmp(name, "hodl_history_projection") == 0)
        return "lib/test/src/test_small_projections.c";
    if (strcmp(name, "block_index_projection") == 0)
        return "lib/test/src/test_block_index_projection.c";
    if (diagnostics_catalog_is_stage(name))
        return "lib/test/src/test_stage.c";
    return "lib/test/src/test_syncdiag_rpc.c";
}

static void diagnostics_catalog_push_str(struct json_value *arr,
                                         const char *value)
{
    struct json_value v;
    json_init(&v);
    json_set_str(&v, value ? value : "");
    json_push_back(arr, &v);
    json_free(&v);
}

static void diagnostics_catalog_push_accepted_keys(struct json_value *obj,
                                                   const char *name,
                                                   const char *key_hint)
{
    struct json_value accepted, examples;
    json_init(&accepted);
    json_set_array(&accepted);
    json_init(&examples);
    json_set_array(&examples);

    if (key_hint && key_hint[0])
        diagnostics_catalog_push_str(&accepted, key_hint);
    if (name && strcmp(name, "block_index") == 0) {
        diagnostics_catalog_push_str(&examples, "3170000");
        diagnostics_catalog_push_str(&examples, "<64-char block hash>");
    } else if (name && strcmp(name, "supervisor") == 0) {
        diagnostics_catalog_push_str(&examples, "sync.watchdog");
        diagnostics_catalog_push_str(&examples, "net.outbound_floor");
    }

    json_push_kv(obj, "accepted_keys", &accepted);
    json_push_kv(obj, "key_examples", &examples);
    json_free(&accepted);
    json_free(&examples);
}

static void diagnostics_catalog_push_tests(struct json_value *obj,
                                           const char *name)
{
    struct json_value tests;
    json_init(&tests);
    json_set_array(&tests);
    diagnostics_catalog_push_str(&tests,
                                 "lib/test/src/test_syncdiag_rpc.c:statecatalog");
    diagnostics_catalog_push_str(&tests,
                                 diagnostics_catalog_primary_test(name));
    json_push_kv(obj, "tests", &tests);
    json_free(&tests);
}

static void diagnostics_catalog_push_drilldowns(struct json_value *obj,
                                                const char *name,
                                                const char *native,
                                                const char *mcp_args)
{
    struct json_value drilldowns;
    char mcp[224];

    json_init(&drilldowns);
    json_set_array(&drilldowns);
    diagnostics_catalog_push_str(&drilldowns, native);
    snprintf(mcp, sizeof(mcp), "zcl_state %s", mcp_args ? mcp_args : "{}");
    diagnostics_catalog_push_str(&drilldowns, mcp);
    if (name && strcmp(name, "supervisor") != 0) {
        char supervisor[224];
        snprintf(supervisor, sizeof(supervisor),
                 "zcl_state {\"subsystem\":\"supervisor.%s\"}", name);
        diagnostics_catalog_push_str(&drilldowns, supervisor);
    }
    json_push_kv(obj, "drilldowns", &drilldowns);
    json_free(&drilldowns);
}

static void diagnostics_catalog_push_entry(
    struct json_value *arr,
    const struct diagnostics_dump_entry *e)
{
    struct json_value obj;
    char native[192];
    char mcp_args[160];
    bool accepts_key = diagnostics_catalog_accepts_key(e ? e->name : NULL);
    const char *key_hint = diagnostics_catalog_key_hint(e ? e->name : NULL);

    if (!e)
        return;

    json_init(&obj);
    json_set_object(&obj);
    snprintf(native, sizeof(native), accepts_key
        ? "zclassic23 dumpstate %s <key>"
        : "zclassic23 dumpstate %s", e->name);
    snprintf(mcp_args, sizeof(mcp_args), accepts_key
        ? "{\"subsystem\":\"%s\",\"key\":\"%s\"}"
        : "{\"subsystem\":\"%s\"}", e->name, key_hint);

    json_push_kv_str(&obj, "name", e->name);
    json_push_kv_str(&obj, "subsystem", e->name);
    json_push_kv_str(&obj, "description", e->desc);
    json_push_kv_str(&obj, "schema", "subsystem-specific zcl_state JSON");
    json_push_kv_str(&obj, "state_class",
                     diagnostics_catalog_state_class(e->name));
    json_push_kv_str(&obj, "owner_shape",
                     diagnostics_catalog_owner_shape(e->name));
    json_push_kv_str(&obj, "owner_file",
                     diagnostics_catalog_owner_file(e->name));
    json_push_kv_str(&obj, "freshness",
                     diagnostics_catalog_freshness(e->name));
    json_push_kv_str(&obj, "cost", diagnostics_catalog_cost(e->name));
    json_push_kv_str(&obj, "safety_level",
                     diagnostics_catalog_safety_level(e->name));
    json_push_kv_bool(&obj, "accepts_key", accepts_key);
    json_push_kv_str(&obj, "key_hint", key_hint);
    json_push_kv_bool(&obj, "key_required", false);
    json_push_kv_str(&obj, "native_command", native);
    json_push_kv_str(&obj, "mcp_tool", "zcl_state");
    json_push_kv_str(&obj, "mcp_args", mcp_args);
    diagnostics_catalog_push_accepted_keys(&obj, e->name, key_hint);
    diagnostics_catalog_push_tests(&obj, e->name);
    diagnostics_catalog_push_drilldowns(&obj, e->name, native, mcp_args);
    json_push_back(arr, &obj);
    json_free(&obj);
}

bool diag_rpc_statecatalog(const struct json_value *params, bool help,
                           struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "statecatalog\n"
        "\nReturn the machine-readable catalog for dumpstate / zcl_state.\n"
        "\nResult: { schema, count, subsystems:[{ name, description, "
        "state_class, owner_shape, owner_file, freshness, cost, "
        "safety_level, accepted_keys, tests, drilldowns, ... }] }");

    struct json_value subsystems;
    json_set_object(result);
    json_push_kv_str(result, "schema", "zcl.state_catalog.v1");
    json_push_kv_str(result, "api_version", "v1");
    json_push_kv_str(result, "status", "ok");
    json_push_kv_str(result, "build_commit", zcl_build_commit());
    json_push_kv_str(result, "source", "diagnostics_registry.g_dumpers");
    json_push_kv_str(result, "default_native_command",
                     "zclassic23 dumpstate <subsystem> [key]");
    json_push_kv_str(result, "default_mcp_tool", "zcl_state");
    json_push_kv_str(result, "catalog_native_command",
                     "zclassic23 statecatalog");
    json_push_kv_str(result, "catalog_mcp_tool", "zcl_state_catalog");
    json_push_kv_int(result, "count",
                     (int64_t)diagnostics_dumper_count());

    json_init(&subsystems);
    json_set_array(&subsystems);
    for (size_t i = 0; i < diagnostics_dumper_count(); i++)
        diagnostics_catalog_push_entry(&subsystems,
                                       diagnostics_dumper_at(i));
    json_push_kv(result, "subsystems", &subsystems);
    json_free(&subsystems);
    return true;
}
