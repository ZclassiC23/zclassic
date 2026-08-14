/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: typed local command for native QR presentation. */

#include "command/native_command.h"
#include "encoding/qr.h"
#include "json/json.h"
#include "platform/time_compat.h"
#include "presentation/presentation.h"
#include "util/log_macros.h"
#include "views/ui_present.h"

#include <string.h>

#define NQR_TAG "native.qr"

static void nqr_fail(struct zcl_command_reply *reply, const char *code,
                     const char *message)
{
    LOG_ERROR(NQR_TAG, "%s: %s", code, message);
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
        ZCL_COMMAND_EXIT_FAILED, code, "display", false, false, message,
        "app.qr.show");
}

void zcl_native_handle_qr_show(const struct zcl_command_request *request,
                               struct zcl_command_reply *reply)
{
    const char *payload = json_get_str(json_get(request->input, "payload"));
    const char *title = json_get_str(json_get(request->input, "title"));
    if (!payload || !payload[0]) {
        nqr_fail(reply, "MISSING_PAYLOAD", "payload must be a non-empty string");
        return;
    }
    size_t payload_len = strnlen(payload, ZCL_QR_MAX_PAYLOAD + 1u);
    if (payload_len > ZCL_QR_MAX_PAYLOAD) {
        nqr_fail(reply, "PAYLOAD_TOO_LARGE", "payload exceeds 2048 bytes");
        return;
    }
    if (title && strnlen(title, 81u) > 80u) {
        nqr_fail(reply, "TITLE_TOO_LARGE", "title exceeds 80 bytes");
        return;
    }
    struct zcl_result launched = ui_present_qr_launch(payload, title);
    if (!launched.ok) {
        nqr_fail(reply, "QR_LAUNCH_FAILED", launched.message);
        return;
    }
    (void)json_push_kv_bool(&reply->data, "launched", true);
    (void)json_push_kv_str(&reply->data, "presentation_kind", "qr");
    (void)json_push_kv_int(&reply->data, "payload_bytes",
                           (int64_t)payload_len);
    (void)json_push_kv_str(&reply->data, "backend",
                           zcl_present_backend_name());
    (void)json_push_kv_str(&reply->data, "platform",
                           zcl_present_platform_name());
}

static void np_fail(struct zcl_command_reply *reply, const char *code,
                    const char *message)
{
    LOG_ERROR("native.presentation", "%s: %s", code, message);
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
        ZCL_COMMAND_EXIT_FAILED, code, "display", false, false, message,
        "app.presentation.show");
}

static bool np_noninteractive_kind(uint16_t kind)
{
    return kind == ZCL_PRESENT_MODEL_STATUS_CARD ||
           kind == ZCL_PRESENT_MODEL_TABLE ||
           kind == ZCL_PRESENT_MODEL_PROGRESS ||
           kind == ZCL_PRESENT_MODEL_CHART ||
           kind == ZCL_PRESENT_MODEL_TIMELINE ||
           kind == ZCL_PRESENT_MODEL_CODE_DIFF ||
           kind == ZCL_PRESENT_MODEL_EVIDENCE_GRAPH;
}

void zcl_native_handle_presentation_show(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    struct zcl_present_model_v1 model;
    char why[192];
    if (!ui_present_model_from_json(request->input, &model,
                                    why, sizeof(why))) {
        np_fail(reply, "INVALID_VISUAL_MODEL", why);
        return;
    }
    if (!np_noninteractive_kind(model.kind) || model.action_count != 0) {
        np_fail(reply, "INTERACTION_REQUIRES_RESIDENT_HOST",
                "this checkpoint launches only non-interactive status, table, "
                "progress, chart, timeline, code-diff, and evidence-graph views");
        return;
    }
    int64_t started_us = platform_time_monotonic_us();
    struct zcl_result launched = ui_present_model_launch(&model);
    int64_t handoff_us = platform_time_monotonic_us() - started_us;
    if (!launched.ok) {
        np_fail(reply, "PRESENTATION_LAUNCH_FAILED", launched.message);
        return;
    }
    (void)json_push_kv_bool(&reply->data, "launched", true);
    (void)json_push_kv_str(&reply->data, "presentation_kind",
                           zcl_present_model_kind_name(model.kind));
    (void)json_push_kv_str(&reply->data, "request_id", model.request_id);
    (void)json_push_kv_int(&reply->data, "item_count", model.item_count);
    (void)json_push_kv_int(&reply->data, "launch_handoff_us", handoff_us);
    (void)json_push_kv_bool(&reply->data, "warm_host", false);
    (void)json_push_kv_bool(&reply->data, "event_return", false);
    (void)json_push_kv_str(&reply->data, "authority", "display-only");
    (void)json_push_kv_str(&reply->data, "backend",
                           zcl_present_backend_name());
    (void)json_push_kv_str(&reply->data, "platform",
                           zcl_present_platform_name());
}
