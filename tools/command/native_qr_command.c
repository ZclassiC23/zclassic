/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: typed local command for native QR presentation. */

#include "command/native_command.h"
#include "encoding/qr.h"
#include "json/json.h"
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
