/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * core.status.brief body — split out of status_native_handlers.c (E1 file
 * size ceiling) so it can grow its own field list without pushing the other
 * read compositions over the 800-line cap.
 *
 * A compact FLAT projection of the sync/serving fields an operator or AI
 * checks on every call — H*, header tip, gap, peer-best height, sync state,
 * healthy/serving/rss, peer count, the dominant typed blocker + its age, and
 * the active self-heal condition count — instead of piping the ~15KB
 * core.status body through grep/tr. Unknown fields are `null`, never a
 * fabricated zero (status_push_int_if_known / status_push_bool_if_known).
 * The ONE flat source both the one-line brief render and the `field=`
 * selector read — see docs/NATIVE_COMMAND_INTERFACE.md "CLI UX contract".
 */

#include "controllers/status_native_handlers.h"
#include "controllers/native_handler_body.h"
#include "controllers/status_native_helpers.h"

#include "json/json.h"
#include "mcp/rpc_client.h"
#include "util/log_macros.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

char *zcl_native_status_brief_body(const struct json_value *args,
                                   struct zcl_native_body_err *err)
{
    (void)args;
    char *h  = mcp_node_rpc("getblockcount", NULL);
    char *ci = mcp_node_rpc("getblockchaininfo", NULL);
    char *s  = mcp_node_rpc("syncstate", NULL);
    char *hc = mcp_node_rpc("healthcheck", NULL);
    char *p  = mcp_node_rpc("getpeerinfo", NULL);
    char *bl = mcp_node_rpc("dumpstate", "[\"blocker\"]");
    char *ce = mcp_node_rpc("dumpstate", "[\"condition_engine\"]");

    struct json_value height_j, chain_j, sync_j, health_j, peers_j;
    bool height_ok = status_parse_rpc_json(&height_j, h, JSON_INT) &&
                     height_j.val.i >= 0 && height_j.val.i <= INT_MAX;
    bool chain_ok = status_parse_rpc_json(&chain_j, ci, JSON_OBJ);
    const struct json_value *header_value =
        chain_ok ? json_get(&chain_j, "best_header_height") : NULL;
    bool header_ok = header_value && header_value->type == JSON_INT &&
                     header_value->val.i >= 0 &&
                     header_value->val.i <= INT_MAX;
    bool sync_ok = status_parse_rpc_json(&sync_j, s, JSON_OBJ);
    bool health_ok = status_parse_rpc_json(&health_j, hc, JSON_OBJ);
    bool peers_ok = status_parse_rpc_json(&peers_j, p, JSON_ARR) &&
                    status_peer_array_is_valid(&peers_j);

    int64_t served_height = height_ok ? height_j.val.i : 0;
    int64_t header_height = header_ok ? header_value->val.i : 0;
    bool gap_known = height_ok && header_ok;
    int64_t gap = gap_known && header_height > served_height
                      ? header_height - served_height : 0;

    const struct json_value *healthy_value =
        health_ok ? json_get(&health_j, "healthy") : NULL;
    bool healthy_known = healthy_value && healthy_value->type == JSON_BOOL;
    const struct json_value *serving_value =
        health_ok ? json_get(&health_j, "serving") : NULL;
    bool serving_known = serving_value && serving_value->type == JSON_BOOL;
    const struct json_value *checks =
        health_ok ? json_get(&health_j, "checks") : NULL;
    const struct json_value *tage_value =
        checks && checks->type == JSON_OBJ
            ? json_get(checks, "tip_advance_age_seconds") : NULL;
    bool tage_known = tage_value && tage_value->type == JSON_INT;
    const struct json_value *rss_value =
        health_ok ? json_get(&health_j, "memory_rss_mb") : NULL;
    bool rss_known = rss_value && rss_value->type == JSON_INT &&
                     rss_value->val.i >= 0;

    int64_t peer_count = peers_ok ? (int64_t)json_size(&peers_j) : 0;
    struct peer_survey survey = {0};
    if (peers_ok) status_peer_survey(&peers_j, &survey);
    const char *sync_state =
        sync_ok ? status_json_str(&sync_j, "state", "unknown") : "unknown";

    struct json_value blocker_summary, blocker_dominant, blocker_error;
    json_init(&blocker_summary);
    json_init(&blocker_dominant);
    json_init(&blocker_error);
    bool blockers_ok = status_build_blocker_summary(
        bl, false, &blocker_summary, &blocker_dominant, &blocker_error);
    const char *primary_blocker = "unknown";
    bool blocker_age_known = false;
    int64_t blocker_age_s = 0;
    if (blockers_ok) {
        primary_blocker = blocker_dominant.type == JSON_OBJ
            ? status_json_str(&blocker_dominant, "id", "none") : "none";
        if (blocker_dominant.type == JSON_OBJ) {
            const struct json_value *age_us =
                json_get(&blocker_dominant, "age_us");
            if (age_us && age_us->type == JSON_INT && age_us->val.i >= 0) {
                blocker_age_known = true;
                blocker_age_s = age_us->val.i / 1000000;
            }
        }
    }

    struct json_value ce_j;
    bool ce_ok = status_parse_rpc_json(&ce_j, ce, JSON_OBJ);
    const struct json_value *ce_state = ce_ok ? json_get(&ce_j, "state")
                                              : NULL;
    const struct json_value *active_conditions_value =
        ce_state && ce_state->type == JSON_OBJ
            ? json_get(ce_state, "active_count") : NULL;
    bool active_conditions_known = active_conditions_value &&
                                   active_conditions_value->type == JSON_INT &&
                                   active_conditions_value->val.i >= 0;

    struct json_value root;
    json_init(&root);
    json_set_object(&root);
    status_push_int_if_known(&root, "hstar", height_ok, served_height);
    status_push_int_if_known(&root, "header_height", header_ok,
                             header_height);
    status_push_int_if_known(&root, "gap", gap_known, gap);
    status_push_int_if_known(&root, "peer_best", survey.max_height_known,
                             survey.max_height);
    json_push_kv_str(&root, "sync_state", sync_state);
    status_push_bool_if_known(&root, "serving", serving_known,
                              serving_known ? serving_value->val.b : false);
    status_push_bool_if_known(&root, "healthy", healthy_known,
                              healthy_known ? healthy_value->val.b : false);
    status_push_int_if_known(&root, "peer_count", peers_ok, peer_count);
    json_push_kv_str(&root, "primary_blocker", primary_blocker);
    status_push_int_if_known(&root, "blocker_age_s", blocker_age_known,
                             blocker_age_s);
    status_push_int_if_known(&root, "active_conditions",
                             active_conditions_known,
                             active_conditions_known
                                 ? active_conditions_value->val.i : 0);
    status_push_int_if_known(&root, "rss_mb", rss_known,
                             rss_known ? rss_value->val.i : 0);
    status_push_int_if_known(&root, "tip_advance_age_seconds", tage_known,
                             tage_known ? tage_value->val.i : 0);

    char *out = zcl_json_value_to_body(&root, "status_brief_body");
    json_free(&root);
    if (ce_ok) json_free(&ce_j);
    json_free(&blocker_error);
    json_free(&blocker_dominant);
    json_free(&blocker_summary);
    json_free(&peers_j);
    json_free(&health_j);
    json_free(&sync_j);
    json_free(&chain_j);
    json_free(&height_j);
    free(h); free(ci); free(s); free(hc); free(p); free(bl); free(ce);
    if (!out) {
        err->status = ZCL_NATIVE_BODY_INTERNAL;
        snprintf(err->message, sizeof(err->message),
                 "malloc failed for %s", "status brief response");
        LOG_NULL("mcp.ops", "malloc failed for %s", "status brief response");
    }
    return out;
}
