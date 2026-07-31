/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native handlers for the `metaverse.agent.*` leaves: parse, authorize the
 * shape of the input, call ONE service, render. No broker logic lives here —
 * the confinement, the grant, and the receipt chain are all in
 * lib/session/agent_broker.h, and this file only reads what the broker
 * recorded.
 *
 * Both leaves are read-only and create nothing, including the directory they
 * are pointed at. A `dir` that does not exist is a named refusal, never a
 * side effect.
 */

#include "command/native_command.h"

#include "base/log_macros.h"
#include "json/json.h"
#include "kernel/command_registry.h"
#include "services/metaverse_agent_service.h"

#include <stddef.h>
#include <stdint.h>

#define MV_TAG "native.metaverse"

/* The rendered documents are bounded by the leaves' ZCL_COMMAND_LIST_BUDGET;
 * this buffer is the slack above it so a document that would exceed the budget
 * is reported as too large rather than silently truncated. */
#define MV_DOC_MAX 16384

static void mv_fail(struct zcl_command_reply *reply,
                    enum zcl_command_exit exit_code, const char *code,
                    const char *message, const char *evidence)
{
    enum zcl_command_status status =
        exit_code == ZCL_COMMAND_EXIT_BLOCKED ? ZCL_COMMAND_STATUS_BLOCKED
                                              : ZCL_COMMAND_STATUS_FAILED;
    zcl_command_reply_fail(reply, status, exit_code, code, "handle", false,
                           false, message, evidence ? evidence : "");
}

/* Map the service's refusal onto the command error contract. The service's own
 * message becomes the evidence, so the operator sees which rule was broken and
 * not just the directory they typed. */
static void mv_fail_result(struct zcl_command_reply *reply,
                           const struct zcl_result *r)
{
    switch (r->code) {
    case MVS_ERR_NOT_A_DIR:
        mv_fail(reply, ZCL_COMMAND_EXIT_FAILED, "NOT_A_DIR",
                "no such broker directory — run `zclassic23 "
                "--metaverse-broker --broker-dir=DIR` first, or point --dir at "
                "the directory a broker already used",
                r->message);
        return;
    case MVS_ERR_RENDER_FAILED:
        mv_fail(reply, ZCL_COMMAND_EXIT_FAILED, "RENDER_FAILED",
                "the document did not fit this leaf's output budget; lower "
                "--limit", r->message);
        return;
    default:
        mv_fail(reply, ZCL_COMMAND_EXIT_INVALID, "BAD_ARGS",
                "dir must be a non-empty absolute path", r->message);
        return;
    }
}

/* Read and shape-check the shared `dir` input. Returns NULL after failing the
 * reply when it is absent. */
static const char *mv_dir(const struct zcl_command_request *request,
                          struct zcl_command_reply *reply)
{
    const char *dir = json_get_str(json_get(request->input, "dir"));
    if (!dir || !dir[0]) {
        mv_fail(reply, ZCL_COMMAND_EXIT_INVALID, "MISSING_DIR",
                "dir is required: the broker directory to read", "dir");
        return NULL;
    }
    return dir;
}

/* Fold a rendered JSON document into reply->data. The service already produced
 * valid JSON; a parse failure here would mean the service and this file
 * disagree, which is an internal fault worth naming rather than papering. */
static bool mv_emit(struct zcl_command_reply *reply, const char *doc,
                    size_t len)
{
    struct json_value v;
    json_init(&v);
    if (!json_read(&v, doc, len)) {
        json_free(&v);
        mv_fail(reply, ZCL_COMMAND_EXIT_FAILED, "INTERNAL",
                "the broker document did not re-parse as JSON", "");
        LOG_FAIL(MV_TAG, "service produced %zu bytes that do not parse", len);
    }
    json_copy(&reply->data, &v);
    json_free(&v);
    return true;
}

/* ── metaverse.agent.status ─────────────────────────────────────────────── */
void zcl_native_handle_metaverse_agent_status(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    const char *dir = mv_dir(request, reply);
    if (!dir)
        return;

    static char doc[MV_DOC_MAX];
    size_t n = 0;
    struct zcl_result r =
        metaverse_agent_service_status(dir, doc, sizeof(doc), &n);
    if (!r.ok) {
        mv_fail_result(reply, &r);
        return;
    }
    (void)mv_emit(reply, doc, n);
}

/* ── metaverse.agent.audit ──────────────────────────────────────────────── */
void zcl_native_handle_metaverse_agent_audit(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    const char *dir = mv_dir(request, reply);
    if (!dir)
        return;

    int64_t limit = json_get_int(json_get(request->input, "limit"));
    if (limit < 0 || limit > 200) {
        mv_fail(reply, ZCL_COMMAND_EXIT_INVALID, "BAD_LIMIT",
                "limit must be between 0 (default) and 200", "limit");
        return;
    }

    static char doc[MV_DOC_MAX];
    size_t n = 0;
    struct zcl_result r =
        metaverse_agent_service_audit(dir, (size_t)limit, doc, sizeof(doc), &n);
    if (!r.ok) {
        mv_fail_result(reply, &r);
        return;
    }
    (void)mv_emit(reply, doc, n);
}
