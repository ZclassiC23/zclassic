/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Shared Commons join posture for toolchain, offered, and guide. */

#include "command/native_zcode_join.h"

#include "base/log_macros.h"
#include "json/json.h"
#include "util/util.h"
#include "vcs/zcode_work_node.h"

#include <string.h>

bool zcl_zcode_join_posture_fill(struct zcl_zcode_join_posture *out)
{
    if (!out)
        LOG_FAIL("zcode.join", "fill: out is NULL");
    memset(out, 0, sizeof(*out));
    struct json_value work;
    json_init(&work);
    bool dumped = vcs_zcode_work_node_dump_state_json(&work, NULL);
    bool worker_enabled = dumped && json_get_bool(json_get(&work, "enabled"));
    json_free(&work);
    out->package_hosting = GetBoolArg("-packagehost", false);
    out->build_worker = worker_enabled || GetBoolArg("-buildworker", false);
    out->joined = out->package_hosting && out->build_worker;
    out->join_flags = ZCL_ZCODE_JOIN_FLAGS;
    out->hosting_requirement = ZCL_ZCODE_HOSTING_REQUIREMENT;
    out->offline_next_command =
        "restart this node with " ZCL_ZCODE_JOIN_FLAGS
        ", then z23 zcode package offered";
    return true;
}

bool zcl_zcode_join_posture_push_json(
    struct json_value *data, const struct zcl_zcode_join_posture *join)
{
    if (!data || !join)
        LOG_FAIL("zcode.join", "push_json: data or join is NULL");
    if (!json_push_kv_str(data, "join_flags", join->join_flags) ||
        !json_push_kv_bool(data, "package_hosting", join->package_hosting) ||
        !json_push_kv_bool(data, "build_worker", join->build_worker) ||
        !json_push_kv_bool(data, "joined", join->joined) ||
        !json_push_kv_str(data, "hosting_requirement",
                          join->hosting_requirement))
        LOG_FAIL("zcode.join", "push_json: reply object refused join fields");
    return true;
}
