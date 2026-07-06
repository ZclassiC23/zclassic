/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Event log and sync state machine RPCs.
 * Canonical source for: eventlog, syncstate */

#include "controllers/event_controller.h"
#include "controllers/agent_controller.h"
#include "controllers/event_healthcheck_controller.h"
#include "controllers/event_timeline_controller.h"
#include "controllers/strong_params.h"
#include "api_controller_internal.h"
#include "event_agent_summary.h"
#include "config/boot.h"
#include "services/bg_validation_service.h"
#include "services/block_index_integrity.h"
#include "services/chain_state_service.h"
#include "event/event.h"
#include "jobs/reducer_frontier.h"
#include "sync/sync_state.h"
#include "json/json.h"
#include "rpc/server.h"
#include "config/runtime.h"
#include <stdlib.h>
#include <string.h>
#include "util/log_macros.h"
#include "util/safe_alloc.h"

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

static bool rpc_api_index(const struct json_value *params, bool help,
                          struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "api\n"
        "\nReturn the versioned zclassic23 API discovery document. This is the\n"
        "same JSON body served by GET /api and GET /api/v1, without HTTP\n"
        "headers, so native clients can start with `zclassic23 api` instead\n"
        "of a shell helper or curl.\n"
        "\nResult:\n"
        "  { \"schema\":\"zcl.rest_index.v1\", \"base_path\":\"/api/v1\", "
        "\"first_call\":\"/api/v1/agent\" }\n");

    const char *body = api_rest_index_body_json();
    if (!json_read(result, body, strlen(body))) {
        json_set_object(result);
        json_push_kv_str(result, "schema", "zcl.rest_error.v1");
        json_push_kv_str(result, "error", "api_index_parse_failed");
        return false;
    }
    return true;
}

static bool rpc_milestone_status(const struct json_value *params, bool help,
                                 struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "milestone\n"
        "\nReturn node-computed ASCII and JSON progress toward the next "
        "version milestone.\n"
        "\nResult:\n"
        "  { \"schema\":\"zcl.milestone_status.v1\", "
        "\"milestone\":\"v1 MVP\", \"mvp_readiness_score\":4, "
        "\"ascii\":{\"goals\":\"goals [#####-----] 4/8 ...\"} }\n");

    api_milestone_status_json(result);
    return true;
}

static bool rpc_refold_status(const struct json_value *params, bool help,
                              struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "refold\n"
        "\nReturn read-only self-verified UTXO anchor rebuild readiness. "
        "This checks whether zclassic23 has a locally rebuilt UTXO anchor "
        "that can replace the borrowed snapshot seed. Internal boot flag: "
        "-refold-from-anchor.\n"
        "\nResult:\n"
        "  { \"schema\":\"zcl.refold_status.v1\", "
        "\"ready_for_refold\":false, "
        "\"primary_blocker\":\"missing_verified_anchor_snapshot\" }\n");

    api_refold_status_json(result);
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
        { "control", "timeline",          rpc_timeline,          true },
        { "control", "api",               rpc_api_index,         true },
        { "control", "apiindex",          rpc_api_index,         true },
        { "control", "agent",             rpc_agent_summary,     true },
        { "control", "summary",           rpc_agent_summary,     true },
        { "control", "operatorsummary",   rpc_agent_summary,     true },
        { "control", "agentmap",          rpc_agent_map,         true },
        { "control", "agentlanes",        rpc_agent_lanes,       true },
        { "control", "agentliveness",     rpc_agent_liveness,    true },
        { "control", "agentimpact",       rpc_agent_impact,      true },
        { "control", "agentcontracts",    rpc_agent_contracts,   true },
        { "control", "agentbuild",        rpc_agent_build,       true },
        { "control", "anchorstatus",      rpc_agent_anchor_status, true },
        { "control", "agentinterface",    rpc_agent_interface,   true },
        { "control", "agentops",          rpc_agent_ops,         true },
        { "control", "agentdiagnose",     rpc_agent_diagnose,    true },
        { "control", "agentdeployguard",  rpc_agent_deploy_guard, true },
        { "control", "milestone",         rpc_milestone_status,  true },
        { "control", "mvpstatus",         rpc_milestone_status,  true },
        { "control", "refold",            rpc_refold_status,     true },
        { "control", "refoldstatus",      rpc_refold_status,     true },
        { "control", "getreorghistory",   rpc_getreorghistory,   true },
        { "control", "syncstate",         rpc_syncstate,         true },
        { "control", "healthcheck",       rpc_healthcheck,       true },
        { "control", "validationstatus",  rpc_validationstatus,  true },
        { "control", "resetvalidation",   rpc_resetvalidation,   true },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_must_append(t, &cmds[i]);
}
