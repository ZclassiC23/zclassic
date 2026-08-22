/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Agent-readable local GCC toolchain capsule identity. */

#include "command/native_command.h"

#include "base/hex.h"
#include "json/json.h"
#include "util/spawn.h"
#include "vcs/build_action.h"

#include <stdio.h>
#include <string.h>

static bool ztc_first_line(const char *const argv[], char *out, size_t cap)
{
    if (!argv || !out || cap == 0) return false;
    if (zcl_spawn_capture(argv, out, cap, 10000) != 0 || !out[0])
        return false;
    out[strcspn(out, "\r\n")] = '\0';
    return out[0] != '\0';
}

void zcl_native_handle_zcode_toolchain_show(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply) return;
    if (request->input && request->input->type == JSON_OBJ &&
        request->input->num_children != 0) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
            "BAD_TOOLCHAIN_SHOW_INPUT", "status", false, false,
            "zcode work toolchain accepts no input keys",
            "zcode.work.toolchain");
        return;
    }
    struct vcs_toolchain_capsule_v1 capsule;
    uint8_t root[32];
    if (!vcs_toolchain_capsule_v1_capture_gcc(&capsule) ||
        !vcs_toolchain_capsule_v1_root(&capsule, root)) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_FAILED,
            "TOOLCHAIN_CAPTURE_FAILED", "status", false, false,
            "the fixed GCC toolchain capsule could not be captured",
            "zcode.work.toolchain");
        return;
    }
    char capsule_hex[65], machine[256], full_version[256], as_version[512];
    zcl_hex_encode(root, 32, capsule_hex);
    machine[0] = '\0';
    full_version[0] = '\0';
    as_version[0] = '\0';
    {
        const char *const cc_machine[] = { VCS_BUILD_COMPILER_V1, "-dumpmachine",
                                           NULL };
        const char *const cc_full[] = { VCS_BUILD_COMPILER_V1, "-dumpfullversion",
                                        NULL };
        const char *const as_ver[] = { "/usr/bin/as", "--version", NULL };
        (void)ztc_first_line(cc_machine, machine, sizeof(machine));
        (void)ztc_first_line(cc_full, full_version, sizeof(full_version));
        (void)ztc_first_line(as_ver, as_version, sizeof(as_version));
    }
    (void)json_push_kv_str(&reply->data, "schema", "zcl.zcode_toolchain_show.v1");
    (void)json_push_kv_str(&reply->data, "capsule_root", capsule_hex);
    (void)json_push_kv_str(&reply->data, "target", capsule.target[0]
                                                     ? capsule.target
                                                     : VCS_BUILD_TARGET_V1);
    (void)json_push_kv_str(&reply->data, "compiler", VCS_BUILD_COMPILER_V1);
    if (machine[0])
        (void)json_push_kv_str(&reply->data, "dumpmachine", machine);
    if (full_version[0])
        (void)json_push_kv_str(&reply->data, "dumpfullversion", full_version);
    if (as_version[0])
        (void)json_push_kv_str(&reply->data, "assembler_version", as_version);
    (void)json_push_kv_str(
        &reply->data, "next_action",
        "Compare capsule_root with zcode work toolchain on the proving node. "
        "Independent compile evidence needs the same capsule.");
    (void)json_push_kv_str(&reply->data, "next_safe_command",
                           "zcode work toolchain");
}
