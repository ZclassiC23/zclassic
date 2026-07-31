/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native handlers for the ZCODE package INSTALL lifecycle:
 *
 *   zcode package add plan     what installing <name_or_root> would do
 *   zcode package add commit   do exactly that plan, or refuse and say why
 *   zcode package rollback     re-activate the previous generation
 *
 * These handlers parse request JSON and render reply JSON. Every decision —
 * resolution, the dependency lock, verification, the confined build, the
 * atomic install, the generation log — belongs to the single lifecycle state
 * machine in app/services/src/package_lifecycle*.c, which is also the only
 * thing that touches disk. Nothing built or downloaded is ever loaded into
 * this process. */

#include "base/hex.h"
#include "command/native_command.h"

#include "json/json.h"
#include "platform/time_compat.h"
#include "services/package_lifecycle.h"

#include <stdio.h>
#include <string.h>

static const char *za_input_str(const struct json_value *input,
                                const char *key)
{
    const struct json_value *v = json_get(input, key);
    return v ? json_get_str(v) : NULL;
}

static const char *za_datadir(const struct zcl_command_request *request,
                              struct zcl_command_reply *reply,
                              const char *command)
{
    const char *dd = za_input_str(request->input, "datadir");
    if (dd && dd[0])
        return dd;
    dd = zcl_native_command_datadir();
    if (dd && dd[0])
        return dd;
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, "MISSING_DATADIR",
                           "normalize", false, false,
                           "no datadir given (input datadir or --datadir)",
                           command);
    return NULL;
}

/* The caller may pin the clock for deterministic testing; host clock
 * otherwise. Plan expiry is evaluated against this same value. */
static int64_t za_now(const struct zcl_command_request *request)
{
    const struct json_value *v = json_get(request->input, "now_unix");
    if (v)
        return json_get_int(v);
    return (int64_t)platform_time_wall_unix();
}

static void za_push_hex(struct json_value *obj, const char *key,
                        const uint8_t root[32])
{
    char hex[65];
    zcl_hex_encode(root, 32, hex);
    (void)json_push_kv_str(obj, key, hex);
}

/* One named rejection, rendered the same way from every leaf. The rule is
 * the machine-readable code; the message explains it. */
static void za_fail(struct zcl_command_reply *reply, const char *rule,
                    const char *message, const char *command)
{
    char code[80];
    size_t o = 0;
    for (size_t i = 0; rule && rule[i] && o + 1u < sizeof(code); i++) {
        char c = rule[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        else if (c == '-')
            c = '_';
        code[o++] = c;
    }
    code[o] = '\0';
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID,
                           code[0] ? code : "ADD_REFUSED", "execute", false,
                           true, message && message[0] ? message : rule,
                           command);
}

/* ── zcode package add plan ─────────────────────────────────────────── */

void zcl_native_handle_zcode_package_add_plan(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *command = "zcode.package.add.plan";
    const char *datadir = za_datadir(request, reply, command);
    if (!datadir)
        return;
    const char *target = za_input_str(request->input, "name_or_root");
    if (!target || !target[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_TARGET",
                               "normalize", false, false,
                               "name_or_root is required (a 64-hex package "
                               "root, or a publisher/package name)", command);
        return;
    }

    struct package_lifecycle_plan_report report;
    struct zcl_result r = package_lifecycle_plan(datadir, target,
                                                 za_now(request), &report);
    if (!r.ok) {
        za_fail(reply, report.rule[0] ? report.rule : "plan-failed",
                r.message, command);
        return;
    }

    za_push_hex(&reply->data, "plan_id", report.plan_id);
    za_push_hex(&reply->data, "target_root", report.plan.target_root);
    za_push_hex(&reply->data, "lock_root", report.plan.lock_root);
    (void)json_push_kv_bool(&reply->data, "ready", report.ready);
    (void)json_push_kv_int(&reply->data, "expires_unix",
                           report.plan.expires_unix);
    (void)json_push_kv_int(&reply->data, "step_count",
                           (int64_t)report.plan.step_count);
    if (report.rule[0]) {
        (void)json_push_kv_str(&reply->data, "rule", report.rule);
        (void)json_push_kv_str(&reply->data, "detail", report.detail);
    }

    struct json_value steps;
    json_init(&steps);
    json_set_array(&steps);
    for (size_t i = 0; i < report.plan.step_count; i++) {
        const struct vcs_package_plan_step *s = &report.plan.steps[i];
        struct json_value row;
        json_init(&row);
        json_set_object(&row);
        za_push_hex(&row, "root", s->root);
        (void)json_push_kv_str(&row, "name", s->name);
        (void)json_push_kv_str(&row, "semver", s->semver);
        (void)json_push_kv_str(&row, "license", s->license);
        (void)json_push_kv_int(&row, "depth", (int64_t)s->depth);
        (void)json_push_kv_str(
            &row, "state",
            vcs_package_lifecycle_state_string(
                (enum vcs_package_lifecycle_state)s->state));
        (void)json_push_kv_bool(&row, "complete", s->complete);
        (void)json_push_kv_bool(&row, "installed", s->installed);
        (void)json_push_kv_int(&row, "total_bytes", (int64_t)s->total_bytes);
        (void)json_push_kv_int(&row, "total_chunks",
                               (int64_t)s->total_chunks);
        (void)json_push_back(&steps, &row);
        json_free(&row);
    }
    (void)json_push_kv(&reply->data, "steps", &steps);
    json_free(&steps);

    (void)json_push_kv_str(
        &reply->data, "note",
        "a plan is a proposal, not an authorization: commit re-derives the "
        "dependency lock and refuses if it changed or if the plan expired. "
        "Steps are in build order (dependencies first, target last) and "
        "every one is pinned by its immutable package root");
}

/* ── zcode package add commit ───────────────────────────────────────── */

void zcl_native_handle_zcode_package_add_commit(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *command = "zcode.package.add.commit";
    const char *datadir = za_datadir(request, reply, command);
    if (!datadir)
        return;
    const char *hex = za_input_str(request->input, "plan_id");
    uint8_t plan_id[32];
    if (!hex || !zcl_hex_decode(hex, plan_id, 32)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "BAD_PLAN_ID",
                               "normalize", false, false,
                               "plan_id must be 64 hex chars (from "
                               "'zcode package add plan')",
                               hex ? hex : "(missing)");
        return;
    }

    struct package_lifecycle_commit_report report;
    struct zcl_result r = package_lifecycle_commit(datadir, plan_id,
                                                   za_now(request), &report);

    struct json_value steps;
    json_init(&steps);
    json_set_array(&steps);
    for (size_t i = 0; i < report.step_count; i++) {
        const struct package_lifecycle_step *s = &report.steps[i];
        struct json_value row;
        json_init(&row);
        json_set_object(&row);
        za_push_hex(&row, "root", s->root);
        (void)json_push_kv_str(&row, "name", s->name);
        (void)json_push_kv_str(&row, "semver", s->semver);
        (void)json_push_kv_int(&row, "depth", (int64_t)s->depth);
        (void)json_push_kv_str(
            &row, "state",
            vcs_package_lifecycle_state_string(
                (enum vcs_package_lifecycle_state)s->state));
        (void)json_push_kv_bool(&row, "already_installed",
                                s->already_installed);
        if (s->has_receipt)
            za_push_hex(&row, "build_receipt_id", s->receipt_id);
        if (s->rule[0]) {
            (void)json_push_kv_str(&row, "rule", s->rule);
            (void)json_push_kv_str(&row, "detail", s->detail);
        }
        (void)json_push_back(&steps, &row);
        json_free(&row);
    }

    if (!r.ok) {
        /* The per-step rows explain WHERE it stopped; the failure body
         * explains why. Nothing was installed past the failing step. */
        za_fail(reply, report.rule[0] ? report.rule : "commit-failed",
                r.message, command);
        json_free(&steps);
        return;
    }

    za_push_hex(&reply->data, "plan_id", report.plan_id);
    (void)json_push_kv_bool(&reply->data, "installed", report.installed);
    za_push_hex(&reply->data, "active_root", report.active_root);
    (void)json_push_kv_bool(&reply->data, "had_previous",
                            report.had_previous);
    if (report.had_previous)
        za_push_hex(&reply->data, "previous_root", report.previous_root);
    (void)json_push_kv_int(&reply->data, "step_count",
                           (int64_t)report.step_count);
    (void)json_push_kv(&reply->data, "steps", &steps);
    json_free(&steps);
    (void)json_push_kv_str(
        &reply->data, "note",
        "each package is a static archive plus headers under "
        "<datadir>/zcode/installed/<root>; the node never loads it. The "
        "previous generation stays on disk, so 'zcode package rollback' is "
        "immediate");
}

/* ── zcode package rollback ─────────────────────────────────────────── */

void zcl_native_handle_zcode_package_rollback(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *command = "zcode.package.rollback";
    const char *datadir = za_datadir(request, reply, command);
    if (!datadir)
        return;
    const char *name = za_input_str(request->input, "name");
    if (!name || !name[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_NAME",
                               "normalize", false, false,
                               "name is required (publisher/package)",
                               command);
        return;
    }

    struct package_lifecycle_rollback_report report;
    struct zcl_result r = package_lifecycle_rollback(datadir, name,
                                                     za_now(request),
                                                     &report);
    if (!r.ok) {
        za_fail(reply, report.rule[0] ? report.rule : "rollback-failed",
                r.message, command);
        return;
    }
    (void)json_push_kv_str(&reply->data, "name", report.name);
    za_push_hex(&reply->data, "from_root", report.from_root);
    za_push_hex(&reply->data, "to_root", report.to_root);
    (void)json_push_kv_int(&reply->data, "generation_count",
                           (int64_t)report.generation_count);
    (void)json_push_kv_str(
        &reply->data, "note",
        "rollback appends a new generation naming the older root — history "
        "is never rewritten and both installs remain on disk");
}
