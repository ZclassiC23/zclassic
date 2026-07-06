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
    timeline_push_categories(result);
    json_push_kv(result, "events", &events);
    json_free(&events);
    return true;
}
