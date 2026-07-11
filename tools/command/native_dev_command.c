/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native dev-lane command executors (contract §3, §8, §12; see
 * config/commands/dev.def, tools/command/native_command.h). This file is
 * part of COMMAND_SRCS (Makefile), which links into BOTH the release and dev
 * binaries — so every dev-only body here is compiled out entirely in a
 * release build (`#ifndef ZCL_DEV_BUILD` stub), matching the existing
 * tools/dev/devloop_cli.c::run_focused() idiom. A release binary never links
 * the process-exec / dev-activation code below.
 *
 * dev.vcs.revert — "one-command source+binary revert": restores the
 * checkout's tracked files to a prior ZVCS commit (vcs_revert(), lib/vcs/)
 * and, when relink_generation is requested, activates the reverted commit's
 * bound binary generation so the RUNNING node matches the reverted source.
 * lib/vcs/ itself stays git-free and process-spawn-free (the ZVCS
 * sovereignty gate, check-vcs-no-git); the relink activator that closes the
 * binary half lives here, entirely outside lib/vcs/. */

#include "command/native_command.h"

#include "kernel/command_registry.h"
#include "json/json.h"
#include "vcs/vcs.h"

#include "devloop.h"

#include "encoding/utilstrencodings.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef ZCL_DEV_BUILD
/* ── relink activator seam ──────────────────────────────────────────
 * This wave (3.3) ships a SHELL FALLBACK: activation rebuilds the binary
 * from the just-reverted source tree and redeploys it via the same fixed
 * argv devloop's transactional-reload path uses
 * (tools/dev/devloop_cycle.c: `make agent-deploy-fast`) — never a shell
 * string, never touching lib/vcs/. It cannot yet tell a full
 * binary-generation hash apart from a bare hotswap .so hash (that
 * classification belongs to the not-yet-built generation store / native
 * dev_activation engine), so it always issues a full rebuild+redeploy from
 * the now-reverted source tree: always a safe way to activate ANY
 * generation, just not the minimal one for a hotswap-only generation.
 *
 * dev_vcs_revert_relink_ops() is the ONE seam every caller goes through —
 * swapping the shell fallback for the native
 * dev_activation_activate_generation() engine (once it lands) touches only
 * this function's body. */
static bool dev_vcs_shell_fallback_activate(const uint8_t gen_sha256[32],
                                            void *ctx)
{
    (void)ctx;
    (void)gen_sha256; /* vcs_revert() already skips an all-zero hash; the
                       * shell fallback rebuilds from source regardless of
                       * which non-zero generation is bound. */
    const char *root = getenv("ZCL_DEV_SOURCE_ROOT");
    if (!root || !root[0])
        root = ".";
    const char *argv[] = {
        "make", "--no-print-directory", "agent-deploy-fast", NULL
    };
    struct zcl_devloop_process_result result;
    if (!zcl_devloop_process_run(root, argv, 900000, &result))
        return false;
    return result.exit_code == 0 && !result.timed_out &&
           result.term_signal == 0;
}

static struct vcs_revert_relink_ops dev_vcs_revert_relink_ops(void)
{
    return (struct vcs_revert_relink_ops){
        .activate_generation = dev_vcs_shell_fallback_activate,
        .ctx = NULL,
    };
}
#endif /* ZCL_DEV_BUILD */

void zcl_native_handle_dev_vcs_revert(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
#ifndef ZCL_DEV_BUILD
    (void)request;
    zcl_command_reply_fail(
        reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
        "DEV_BUILD_REQUIRED", "dispatch", false, false,
        "one-command source+binary revert requires a dev build",
        "make dev-bin, or zclassic23-dev");
#else
    if (!reply)
        return;
    if (!request || !request->input) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "BAD_REQUEST",
                               "normalize", false, false,
                               "missing request input", "");
        return;
    }

    const char *to_hex = json_get_str(json_get(request->input, "to"));
    bool relink_generation =
        json_get_bool(json_get(request->input, "relink_generation"));

    uint8_t target[32];
    if (!to_hex || strlen(to_hex) != 64 || !IsHex(to_hex) ||
        ParseHex(to_hex, target, sizeof(target)) != sizeof(target)) {
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
            "BAD_COMMIT_ID", "normalize", false, false,
            "'to' must be a 64-char hex ZVCS commit id",
            to_hex ? to_hex : "");
        return;
    }

    const char *root = (request->context && request->context->source_root &&
                        request->context->source_root[0])
                           ? request->context->source_root
                           : ".";
    struct vcs_repo *r = vcs_open(root);
    if (!r) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "VCS_OPEN_FAILED",
                               "execute", false, false,
                               "could not open the ZVCS repo at source_root",
                               root);
        return;
    }

    struct vcs_revert_relink_ops ops = dev_vcs_revert_relink_ops();
    uint8_t new_commit[32] = {0};
    int rc = vcs_revert(r, target, relink_generation ? &ops : NULL,
                        new_commit);
    vcs_close(r);

    /* Only VCS_OK / VCS_EPARTIAL actually write out_new_commit (vcs_revert
     * forwards VCS_REFUSED / VCS_ERR before the forward commit lands), so
     * the hex form is computed lazily per-branch below, never over an
     * unwritten buffer. */
    char new_hex[65];
    static const char hexd[] = "0123456789abcdef";

    switch (rc) {
    case VCS_OK:
        for (int i = 0; i < 32; i++) {
            new_hex[2 * i] = hexd[(new_commit[i] >> 4) & 0xf];
            new_hex[2 * i + 1] = hexd[new_commit[i] & 0xf];
        }
        new_hex[64] = '\0';
        (void)json_push_kv_str(&reply->data, "to", to_hex);
        (void)json_push_kv_str(&reply->data, "forward_commit", new_hex);
        (void)json_push_kv_bool(&reply->data, "relink_generation",
                                relink_generation);
        (void)json_push_kv_str(&reply->data, "status", "reverted");
        return;
    case VCS_EPARTIAL:
        for (int i = 0; i < 32; i++) {
            new_hex[2 * i] = hexd[(new_commit[i] >> 4) & 0xf];
            new_hex[2 * i + 1] = hexd[new_commit[i] & 0xf];
        }
        new_hex[64] = '\0';
        (void)json_push_kv_str(&reply->data, "to", to_hex);
        (void)json_push_kv_str(&reply->data, "forward_commit", new_hex);
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
            "RELINK_ACTIVATION_FAILED", "execute", true, true,
            "source revert + forward commit landed (append-only, never "
            "undone), but binary-generation activation failed",
            new_hex);
        return;
    case VCS_REFUSED:
        (void)json_push_kv_str(&reply->data, "to", to_hex);
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_BLOCKED,
            "SEALED_PATH_REFUSED", "execute", false, true,
            "revert would change a sealed path; run the owner-gated "
            "unseal ritual first",
            to_hex);
        return;
    default:
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "REVERT_FAILED",
                               "execute", false, false,
                               "vcs_revert failed (bad commit id or a "
                               "worktree I/O error)",
                               to_hex);
        return;
    }
#endif
}
