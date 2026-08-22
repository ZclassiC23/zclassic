/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Read-only inner-loop guide for coding agents (`code.guide`).
 *
 * THE INVARIANT OF THIS FILE: the handler reads no checkout, datadir, wallet,
 * compiler, or test state. It renders the currently honest edit/proof loop
 * and refuses extra input keys. */

#include "command/native_command.h"

#include "json/json.h"
#include "util/log_macros.h"

#include <string.h>

#define CG_TAG "native.code.guide"

void zcl_native_handle_code_guide(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!reply)
        return;
    if (!request || !request->input || request->input->type != JSON_OBJ ||
        request->input->num_children != 0) {
        LOG_ERROR(CG_TAG, "BAD_CODE_GUIDE_INPUT: code guide accepts no "
                          "input keys");
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID,
                               "BAD_CODE_GUIDE_INPUT", "validate", false,
                               false, "code guide accepts no input keys",
                               "code.guide");
        return;
    }
    bool ok = json_push_kv_str(
            &reply->data, "mission",
            "Edit the smallest owned surface and prove it without touching "
            "the live node.") &&
        json_push_kv_str(
            &reply->data, "next_action",
            "Ask code impact for the changed file, then run only those "
            "focused tests.") &&
        json_push_kv_str(
            &reply->data, "start_command",
            "z23 code impact --input='{\"path\":\"<file.c>\"}'") &&
        json_push_kv_str(
            &reply->data, "tests_command",
            "z23 code tests --input='{\"path\":\"<file.c>\"}'") &&
        json_push_kv_str(
            &reply->data, "proof_command",
            "make -j\"$(nproc)\" t-fast ONLY=<group from code tests>") &&
        json_push_kv_str(&reply->data, "lint_command", "make lint-fast") &&
        json_push_kv_str(&reply->data, "push_command", "make pre-push-ci") &&
        json_push_kv_str(
            &reply->data, "never",
            "Do not run full make lint on an ordinary slice, do not invoke "
            "test_zcl, do not omit --datadir when a leaf accepts datadir, "
            "do not stash unrelated dirty work, do not restart or deploy.") &&
        json_push_kv_str(
            &reply->data, "umbrella_lint",
            "make lint is the full umbrella. Run it only when the impact "
            "rule names a gate that lint-fast excludes.") &&
        json_push_kv_str(
            &reply->data, "product_guide",
            "z23 zcode guide") &&
        json_push_kv_str(
            &reply->data, "continue_rule",
            "Follow next_action. Discover exact keys with "
            "z23 discover schema <leaf>.") &&
        json_push_kv_str(&reply->data, "docs", "docs/DEVELOPING.md");
    if (!ok) {
        LOG_ERROR(CG_TAG, "CODE_GUIDE_OUTPUT: the develop guide could not "
                          "be rendered");
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "CODE_GUIDE_OUTPUT",
                               "render", false, false,
                               "the develop guide could not be rendered",
                               "code.guide");
    }
}
