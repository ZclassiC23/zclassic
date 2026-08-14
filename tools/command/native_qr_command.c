/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: typed local command for native QR presentation. */

#include "command/native_command.h"
#include "encoding/qr.h"
#include "json/json.h"
#include "platform/time_compat.h"
#include "presentation/presentation.h"
#include "util/log_macros.h"
#include "views/ui_present.h"
#include "views/ui_present_host.h"

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
    int64_t started_us = platform_time_monotonic_us();
    struct ui_present_host_result host;
    struct zcl_result launched = ui_present_host_submit_qr(
        payload, title, &host);
    bool cold_fallback = false;
    if (!launched.ok) {
        launched = ui_present_qr_launch(payload, title);
        cold_fallback = launched.ok;
    }
    int64_t handoff_us = platform_time_monotonic_us() - started_us;
    if (!launched.ok) {
        nqr_fail(reply, "QR_LAUNCH_FAILED", launched.message);
        return;
    }
    (void)json_push_kv_bool(&reply->data, "launched", true);
    (void)json_push_kv_str(&reply->data, "presentation_kind", "qr");
    (void)json_push_kv_int(&reply->data, "payload_bytes",
                           (int64_t)payload_len);
    (void)json_push_kv_int(&reply->data, "launch_handoff_us", handoff_us);
    (void)json_push_kv_bool(&reply->data, "resident_host",
                            !cold_fallback && host.resident_host);
    (void)json_push_kv_bool(&reply->data, "host_reused",
                            !cold_fallback && host.host_reused);
    (void)json_push_kv_bool(&reply->data, "view_replaced", false);
    (void)json_push_kv_int(&reply->data, "window_ready_us",
                           cold_fallback ? -1 : host.ready_us);
    (void)json_push_kv_str(&reply->data, "authority", "display-only");
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

static bool np_supported_kind(uint16_t kind)
{
    return kind == ZCL_PRESENT_MODEL_STATUS_CARD ||
           kind == ZCL_PRESENT_MODEL_TABLE ||
           kind == ZCL_PRESENT_MODEL_PROGRESS ||
           kind == ZCL_PRESENT_MODEL_CHART ||
           kind == ZCL_PRESENT_MODEL_TIMELINE ||
           kind == ZCL_PRESENT_MODEL_CODE_DIFF ||
           kind == ZCL_PRESENT_MODEL_EVIDENCE_GRAPH ||
           kind == ZCL_PRESENT_MODEL_CHOICE ||
           kind == ZCL_PRESENT_MODEL_CONFIRMATION ||
           kind == ZCL_PRESENT_MODEL_FORM;
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
    if (!np_supported_kind(model.kind)) {
        np_fail(reply, "UNSUPPORTED_PRESENTATION_KIND",
                "use app.qr.show for QR; raw canvas documents are not admitted");
        return;
    }
    bool wait_for_event = model.action_count > 0;
    int64_t started_us = platform_time_monotonic_us();
    struct ui_present_host_result host;
    struct zcl_result launched = ui_present_host_submit(
        &model, wait_for_event, &host);
    bool cold_fallback = false;
    if (!launched.ok && !wait_for_event) {
        launched = ui_present_model_launch(&model);
        cold_fallback = launched.ok;
    }
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
    (void)json_push_kv_bool(&reply->data, "resident_host",
                            !cold_fallback && host.resident_host);
    (void)json_push_kv_bool(&reply->data, "host_reused",
                            !cold_fallback && host.host_reused);
    (void)json_push_kv_bool(&reply->data, "view_replaced",
                            !cold_fallback && host.view_replaced);
    (void)json_push_kv_int(&reply->data, "window_ready_us",
                           cold_fallback ? -1 : host.ready_us);
    (void)json_push_kv_bool(&reply->data, "event_return",
                            !cold_fallback && host.event_received);
    if (!cold_fallback && host.event_received) {
        bool action = host.action_index < model.action_count;
        (void)json_push_kv_str(&reply->data, "event",
                               action ? "action" : "dismissed");
        if (action) {
            (void)json_push_kv_str(&reply->data, "action_id",
                model.actions[host.action_index].id);
            (void)json_push_kv_int(&reply->data, "action_index",
                                   host.action_index);
        }
        if (model.exact_root[0])
            (void)json_push_kv_str(&reply->data, "exact_root",
                                   model.exact_root);
    }
    (void)json_push_kv_str(&reply->data, "authority", "display-only");
    (void)json_push_kv_str(&reply->data, "backend",
                           zcl_present_backend_name());
    (void)json_push_kv_str(&reply->data, "platform",
                           zcl_present_platform_name());
}
