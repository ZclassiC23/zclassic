/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Semantic event timeline RPC for AI/operator diagnostics. */

#include "controllers/event_timeline_controller.h"
#include "controllers/strong_params.h"

#include "event/event.h"
#include "json/json.h"
#include "util/clientversion.h"
#include "util/safe_alloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct timeline_category {
    const char *name;
    const char *prefix;
    const char *description;
};

static const struct timeline_category k_timeline_categories[] = {
    { "all",        "",             "all retained structured events" },
    { "tcp",        "tcp.",         "TCP connection lifecycle" },
    { "peer",       "peer.",        "peer handshakes, state, bans, and floor breaches" },
    { "message",    "msg.",         "P2P message send/receive/deserialization events" },
    { "sync",       "sync.",        "sync FSM, headers, block requests, and stale-tip events" },
    { "snapshot",   "snapsync.",    "snapshot receive/verify progress" },
    { "chain",      "chain.",       "chain tip and chain-advance decisions" },
    { "validation", "val.",         "block validation and reducer validation events" },
    { "condition",  "condition.",   "self-heal condition lifecycle and operator-needed pages" },
    { "oracle",     "oracle.",      "zclassicd oracle agreement/disagreement and halt signals" },
    { "mirror",     "mirror.",      "mirror consensus and lag-SLO signals" },
    { "boot",       "boot.",        "boot, block-index, and startup recovery events" },
    { "db",         "db.",          "database transaction and maintenance events" },
    { "wallet",     "wallet.",      "wallet key, transaction, UTXO, and backup events" },
    { "mempool",    "mempool.",     "mempool eviction and expiry events" },
    { "disk",       "disk.",        "disk pressure monitor events" },
    { "mcp",        "mcp.",         "MCP request telemetry" },
    { "net",        "net.",         "network backpressure telemetry" },
};

static const struct timeline_category *timeline_category_find(const char *name)
{
    const char *want = (name && name[0]) ? name : "all";
    for (size_t i = 0; i < sizeof(k_timeline_categories) /
                            sizeof(k_timeline_categories[0]); i++) {
        if (strcmp(k_timeline_categories[i].name, want) == 0)
            return &k_timeline_categories[i];
    }
    return NULL;
}

static void timeline_push_categories(struct json_value *out)
{
    struct json_value arr;
    json_init(&arr);
    json_set_array(&arr);
    for (size_t i = 0; i < sizeof(k_timeline_categories) /
                            sizeof(k_timeline_categories[0]); i++) {
        struct json_value obj;
        json_init(&obj);
        json_set_object(&obj);
        json_push_kv_str(&obj, "name", k_timeline_categories[i].name);
        json_push_kv_str(&obj, "type_prefix", k_timeline_categories[i].prefix);
        json_push_kv_str(&obj, "description",
                         k_timeline_categories[i].description);
        json_push_back(&arr, &obj);
        json_free(&obj);
    }
    json_push_kv(out, "available_categories", &arr);
    json_free(&arr);
}

static void timeline_push_str(struct json_value *arr, const char *s)
{
    struct json_value v;
    json_init(&v);
    json_set_str(&v, s ? s : "");
    json_push_back(arr, &v);
    json_free(&v);
}

static const char *timeline_param_category(const struct json_value *params)
{
    if (!params)
        return "all";
    if (params->type == JSON_OBJ) {
        const char *s = json_get_str(json_get(params, "category"));
        return (s && s[0]) ? s : "all";
    }
    if (params->type == JSON_ARR && json_size(params) > 0) {
        const struct json_value *v = json_at(params, 0);
        if (v && v->type == JSON_STR) {
            const char *s = json_get_str(v);
            return (s && s[0]) ? s : "all";
        }
    }
    return "all";
}

static int timeline_param_count(const struct json_value *params)
{
    int count = 50;
    if (!params)
        return count;
    if (params->type == JSON_OBJ) {
        const struct json_value *v = json_get(params, "count");
        if (v && v->type == JSON_INT)
            count = (int)json_get_int(v);
        else if (v && v->type == JSON_REAL)
            count = (int)v->val.d;
    } else if (params->type == JSON_ARR && json_size(params) > 0) {
        const struct json_value *v0 = json_at(params, 0);
        const struct json_value *v1 = json_size(params) > 1 ?
            json_at(params, 1) : NULL;
        const struct json_value *v = (v0 && v0->type == JSON_STR) ? v1 : v0;
        if (v && v->type == JSON_INT)
            count = (int)json_get_int(v);
        else if (v && v->type == JSON_REAL)
            count = (int)v->val.d;
    }
    if (count < 1) count = 1;
    if (count > 1000) count = 1000;
    return count;
}

static bool timeline_type_is_problem(const char *type)
{
    if (!type)
        return false;
    return strstr(type, "fail") ||
           strstr(type, "reject") ||
           strstr(type, "stale") ||
           strstr(type, "breach") ||
           strstr(type, "panic") ||
           strstr(type, "halt") ||
           strstr(type, "timeout") ||
           strstr(type, "corrupt") ||
           strstr(type, "critical") ||
           strstr(type, "low") ||
           strstr(type, "drift") ||
           strstr(type, "misbehave") ||
           strstr(type, "banned") ||
           strstr(type, "disconnect") ||
           strstr(type, "backpressure") ||
           strstr(type, "operator_needed");
}

static void timeline_push_type_counts(struct json_value *out,
                                      const struct json_value *events,
                                      int64_t *problem_count,
                                      const char **dominant_type,
                                      int64_t *dominant_count)
{
    struct {
        char type[64];
        int64_t count;
        bool problem;
    } counts[64] = {0};
    size_t used = 0;
    int64_t overflow = 0;

    if (problem_count)
        *problem_count = 0;
    if (dominant_type)
        *dominant_type = "";
    if (dominant_count)
        *dominant_count = 0;

    size_t n = events && events->type == JSON_ARR ? json_size(events) : 0;
    for (size_t i = 0; i < n; i++) {
        const struct json_value *ev = json_at(events, i);
        const char *type = json_get_str(json_get(ev, "type"));
        bool problem = timeline_type_is_problem(type);
        if (problem && problem_count)
            (*problem_count)++;

        size_t slot = used;
        for (size_t j = 0; j < used; j++) {
            if (strcmp(counts[j].type, type) == 0) {
                slot = j;
                break;
            }
        }
        if (slot == used) {
            if (used >= sizeof(counts) / sizeof(counts[0])) {
                overflow++;
                continue;
            }
            snprintf(counts[slot].type, sizeof(counts[slot].type), "%s", type);
            counts[slot].problem = problem;
            used++;
        }
        counts[slot].count++;
        if (dominant_count && counts[slot].count > *dominant_count) {
            *dominant_count = counts[slot].count;
            if (dominant_type)
                *dominant_type = counts[slot].type;
        }
    }

    struct json_value arr;
    json_init(&arr);
    json_set_array(&arr);
    for (size_t i = 0; i < used; i++) {
        struct json_value obj;
        json_init(&obj);
        json_set_object(&obj);
        json_push_kv_str(&obj, "type", counts[i].type);
        json_push_kv_int(&obj, "count", counts[i].count);
        json_push_kv_bool(&obj, "problem", counts[i].problem);
        json_push_back(&arr, &obj);
        json_free(&obj);
    }
    json_push_kv(out, "type_counts", &arr);
    json_free(&arr);
    json_push_kv_int(out, "type_count_overflow", overflow);
}

static void timeline_push_peer_counts(struct json_value *out,
                                      const struct json_value *events,
                                      int64_t *unique_peers)
{
    struct {
        int64_t peer;
        int64_t count;
    } peers[64] = {0};
    size_t used = 0;
    int64_t overflow = 0;

    if (unique_peers)
        *unique_peers = 0;

    size_t n = events && events->type == JSON_ARR ? json_size(events) : 0;
    for (size_t i = 0; i < n; i++) {
        const struct json_value *ev = json_at(events, i);
        int64_t peer = json_get_int(json_get(ev, "peer"));
        size_t slot = used;
        for (size_t j = 0; j < used; j++) {
            if (peers[j].peer == peer) {
                slot = j;
                break;
            }
        }
        if (slot == used) {
            if (used >= sizeof(peers) / sizeof(peers[0])) {
                overflow++;
                continue;
            }
            peers[slot].peer = peer;
            used++;
        }
        peers[slot].count++;
    }
    if (unique_peers)
        *unique_peers = (int64_t)used;

    struct json_value arr;
    json_init(&arr);
    json_set_array(&arr);
    for (size_t i = 0; i < used; i++) {
        struct json_value obj;
        json_init(&obj);
        json_set_object(&obj);
        json_push_kv_int(&obj, "peer", peers[i].peer);
        json_push_kv_int(&obj, "count", peers[i].count);
        json_push_back(&arr, &obj);
        json_free(&obj);
    }
    json_push_kv(out, "peer_counts", &arr);
    json_free(&arr);
    json_push_kv_int(out, "peer_count_overflow", overflow);
}

static const char *timeline_category_next_action(const char *category,
                                                 int64_t problem_count)
{
    if (problem_count > 0)
        return "inspect_problem_drilldowns_before_raw_logs";
    if (category && strcmp(category, "sync") == 0)
        return "compare_sync_timeline_to_reducer_frontier_and_supervisor";
    if (category && strcmp(category, "peer") == 0)
        return "inspect_peer_lifecycle_if_connections_churn";
    if (category && strcmp(category, "all") == 0)
        return "switch_to_a_specific_category_before_deep_diagnosis";
    return "continue_with_category_drilldowns_if_the_timeline_is_suspicious";
}

static void timeline_push_drilldowns(struct json_value *out,
                                     const struct timeline_category *cat,
                                     int64_t problem_count)
{
    struct json_value arr;
    const char *name = cat ? cat->name : "all";

    json_init(&arr);
    json_set_array(&arr);
    if (strcmp(name, "sync") == 0) {
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"reducer_frontier\"}");
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"chain_advance_coordinator\"}");
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"supervisor.sync.watchdog\"}");
        timeline_push_str(&arr, "zcl_node_log pattern=\"sync|stale|lag|blocker\" max_lines=80");
    } else if (strcmp(name, "peer") == 0) {
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"peer_lifecycle\"}");
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"peers_projection\"}");
        timeline_push_str(&arr, "zcl_node_log pattern=\"peer|handshake|disconnect\" max_lines=80");
    } else if (strcmp(name, "chain") == 0) {
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"chain_evidence\"}");
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"chain_advance_coordinator\"}");
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"reducer_frontier\"}");
    } else if (strcmp(name, "validation") == 0) {
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"validation_pack\"}");
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"reducer_frontier\"}");
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"block_index\"}");
    } else if (strcmp(name, "condition") == 0) {
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"condition_engine\"}");
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"blocker\"}");
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"supervisor\"}");
    } else if (strcmp(name, "oracle") == 0 || strcmp(name, "mirror") == 0) {
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"oracle\"}");
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"legacy_mirror\"}");
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"quorum_oracle\"}");
    } else if (strcmp(name, "boot") == 0) {
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"boot\"}");
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"service_state\"}");
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"block_index\"}");
    } else if (strcmp(name, "db") == 0) {
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"db_maintenance\"}");
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"progress\"}");
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"disk_monitor\"}");
    } else if (strcmp(name, "wallet") == 0) {
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"wallet_projection\"}");
    } else if (strcmp(name, "disk") == 0) {
        timeline_push_str(&arr, "zcl_state {\"subsystem\":\"disk_monitor\"}");
    } else if (strcmp(name, "mcp") == 0) {
        timeline_push_str(&arr, "zcl_agent_interface");
        timeline_push_str(&arr, "zcl_agent_ops");
    } else {
        timeline_push_str(&arr, "zcl_state_catalog");
        timeline_push_str(&arr, "zcl_timeline category=\"sync\" count=50");
        timeline_push_str(&arr, "zcl_timeline category=\"peer\" count=50");
    }
    if (problem_count > 0)
        timeline_push_str(&arr, "zcl_node_log pattern=\"fail|reject|stale|breach|panic|halt|timeout|corrupt\" max_lines=120");
    json_push_kv(out, "recommended_drilldowns", &arr);
    json_free(&arr);
}

static void timeline_push_semantic_summary(struct json_value *out,
                                           const struct json_value *events,
                                           const struct timeline_category *cat)
{
    struct json_value summary;
    int64_t event_count = events && events->type == JSON_ARR
        ? (int64_t)json_size(events) : 0;
    int64_t first_seq = -1, last_seq = -1, first_ts = 0, last_ts = 0;
    int64_t problem_count = 0, unique_peers = 0, dominant_count = 0;
    const char *dominant_type = "";

    if (event_count > 0) {
        const struct json_value *first = json_at(events, 0);
        const struct json_value *last = json_at(events,
                                               (size_t)event_count - 1u);
        first_seq = json_get_int(json_get(first, "seq"));
        last_seq = json_get_int(json_get(last, "seq"));
        first_ts = json_get_int(json_get(first, "ts"));
        last_ts = json_get_int(json_get(last, "ts"));
    }

    timeline_push_type_counts(out, events, &problem_count, &dominant_type,
                              &dominant_count);
    timeline_push_peer_counts(out, events, &unique_peers);

    json_init(&summary);
    json_set_object(&summary);
    json_push_kv_int(&summary, "event_count", event_count);
    json_push_kv_int(&summary, "first_seq", first_seq);
    json_push_kv_int(&summary, "last_seq", last_seq);
    json_push_kv_int(&summary, "first_ts", first_ts);
    json_push_kv_int(&summary, "last_ts", last_ts);
    json_push_kv_int(&summary, "problem_event_count", problem_count);
    json_push_kv_int(&summary, "unique_peer_count", unique_peers);
    json_push_kv_str(&summary, "dominant_type", dominant_type);
    json_push_kv_int(&summary, "dominant_type_count", dominant_count);
    json_push_kv_bool(&summary, "has_problem_events", problem_count > 0);
    json_push_kv_str(&summary, "safe_next_action",
                     timeline_category_next_action(cat ? cat->name : "all",
                                                   problem_count));
    json_push_kv(out, "semantic_summary", &summary);
    json_free(&summary);
    json_push_kv_str(out, "safe_next_action",
                     timeline_category_next_action(cat ? cat->name : "all",
                                                   problem_count));
    timeline_push_drilldowns(out, cat, problem_count);
}

bool rpc_timeline(const struct json_value *params, bool help,
                  struct json_value *result)
{
    RPC_HELP(help, result,
        "timeline ( category count )\n"
        "\nReturn a versioned semantic timeline over the in-memory structured\n"
        "event ring. Categories are mapped to event type prefixes server-side\n"
        "so agents do not need jq/string filters.\n"
        "\nArguments:\n"
        "1. category  (string, optional, default=all) all|peer|sync|chain|...\n"
        "2. count     (numeric, optional, default=50, max=1000)\n"
        "\nResult:\n"
        "  { \"schema\":\"zcl.timeline.v1\", \"events\":[...] }\n");

    const char *category_name = timeline_param_category(params);
    int count = timeline_param_count(params);
    const struct timeline_category *cat =
        timeline_category_find(category_name);

    json_set_object(result);
    json_push_kv_str(result, "schema", "zcl.timeline.v1");
    json_push_kv_str(result, "api_version", "v1");
    json_push_kv_str(result, "build_commit", zcl_build_commit());
    json_push_kv_str(result, "source", "event_ring");
    json_push_kv_int(result, "head_seq",
                     (int64_t)event_log_head_sequence());
    json_push_kv_int(result, "retention_events", EVENT_LOG_SIZE);
    json_push_kv_str(result, "cursor_field", "events[].seq");
    json_push_kv_str(result, "native_command",
                     "zclassic23 timeline <category> <count>");
    json_push_kv_str(result, "mcp_tool", "zcl_timeline");

    if (!cat) {
        json_push_kv_str(result, "status", "error");
        json_push_kv_str(result, "error", "unknown_timeline_category");
        json_push_kv_str(result, "category_requested", category_name);
        timeline_push_categories(result);
        return true;
    }

    size_t buf_size = (size_t)count * 320u + 1024u;
    if (buf_size > 16u * 1024u * 1024u)
        buf_size = 16u * 1024u * 1024u;
    char *buf = zcl_malloc(buf_size, "timeline events json buf");
    if (!buf) {
        json_push_kv_str(result, "status", "error");
        json_push_kv_str(result, "error", "out_of_memory");
        return false;
    }

    size_t w = event_dump_json_filtered(buf, buf_size, (size_t)count,
                                        cat->prefix);
    struct json_value events;
    json_init(&events);
    bool parsed = w > 0 && json_read(&events, buf, w);
    free(buf);
    if (!parsed || events.type != JSON_ARR) {
        json_free(&events);
        json_push_kv_str(result, "status", "error");
        json_push_kv_str(result, "error", "event_timeline_parse_failed");
        return false;
    }

    json_push_kv_str(result, "status", "ok");
    json_push_kv_str(result, "category", cat->name);
    json_push_kv_str(result, "type_prefix", cat->prefix);
    json_push_kv_str(result, "description", cat->description);
    json_push_kv_int(result, "count_requested", count);
    json_push_kv_int(result, "count_returned", (int64_t)json_size(&events));
    timeline_push_semantic_summary(result, &events, cat);
    timeline_push_categories(result);
    json_push_kv(result, "events", &events);
    json_free(&events);
    return true;
}
