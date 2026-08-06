/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: detached same-binary launcher for reviewed local UI windows. */

#include "views/ui_present.h"

#include "json/json.h"
#include "platform/os_proc.h"
#include "util/spawn.h"
#include "views/qr_popup.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void ui_present_error(char *error, size_t cap, const char *message)
{
    if (error && cap > 0) (void)snprintf(error, cap, "%s", message);
}

bool ui_present_qr_request_parse(const char *raw, size_t raw_len,
                                 struct ui_present_qr_request *out,
                                 char *error, size_t error_cap)
{
    if (!raw || !out || raw_len == 0 || raw_len > UI_PRESENT_REQUEST_MAX) {
        ui_present_error(error, error_cap,
                         "presentation request is empty or oversized");
        return false;
    }

    struct json_value request;
    json_init(&request);
    if (!json_read(&request, raw, raw_len) || request.type != JSON_OBJ) {
        json_free(&request);
        ui_present_error(error, error_cap,
                         "presentation request is not a JSON object");
        return false;
    }

    const char *payload = json_get_str(json_get(&request, "payload"));
    const char *title = json_get_str(json_get(&request, "title"));
    if (!payload || !payload[0]) {
        json_free(&request);
        ui_present_error(error, error_cap,
                         "presentation payload must be a non-empty string");
        return false;
    }
    size_t payload_len = strnlen(payload, ZCL_QR_MAX_PAYLOAD + 1u);
    if (payload_len > ZCL_QR_MAX_PAYLOAD) {
        json_free(&request);
        ui_present_error(error, error_cap,
                         "presentation payload exceeds the QR limit");
        return false;
    }
    size_t title_len = title ? strnlen(title, UI_PRESENT_TITLE_MAX + 1u) : 0;
    if (title_len > UI_PRESENT_TITLE_MAX) {
        json_free(&request);
        ui_present_error(error, error_cap,
                         "presentation title exceeds the title limit");
        return false;
    }

    memcpy(out->payload, payload, payload_len + 1u);
    if (title_len > 0) memcpy(out->title, title, title_len);
    out->title[title_len] = '\0';
    json_free(&request);
    if (error && error_cap > 0) error[0] = '\0';
    return true;
}

struct zcl_result ui_present_qr_launch(const char *payload,
                                        const char *title)
{
    if (!payload || !payload[0])
        return ZCL_ERR(-1, "ui_present_qr_launch: empty payload");

    struct json_value request;
    json_init(&request);
    json_set_object(&request);
    bool built = json_push_kv_str(&request, "payload", payload);
    if (built && title && title[0])
        built = json_push_kv_str(&request, "title", title);
    if (!built) {
        json_free(&request);
        return ZCL_ERR(-1, "ui_present_qr_launch: request encoding failed");
    }

    char wire[UI_PRESENT_REQUEST_MAX + 1u];
    size_t wire_len = json_write(&request, wire, sizeof(wire));
    json_free(&request);
    if (wire_len >= sizeof(wire))
        return ZCL_ERR(-1,
                       "ui_present_qr_launch: encoded request exceeds %u bytes",
                       UI_PRESENT_REQUEST_MAX);

    char executable[PATH_MAX];
    if (!os_proc_exe_path(executable, sizeof(executable)))
        return ZCL_ERR(-errno,
                       "ui_present_qr_launch: executable path unavailable: %s",
                       strerror(errno));

    const char *argv[] = {
        executable,
        "--ui-present-child=qr",
        NULL,
    };
    return zcl_spawn_detached_input(argv, wire, wire_len, NULL);
}

int ui_present_child_main(const char *kind)
{
    if (!kind || strcmp(kind, "qr") != 0) {
        (void)fprintf(stderr, "Unsupported presentation kind.\n"); // obs-ok:detached-child-terminal-diagnostic
        return 2;
    }

    char raw[UI_PRESENT_REQUEST_MAX + 1u];
    size_t used = 0;
    while (used < sizeof(raw)) {
        ssize_t nr = read(STDIN_FILENO, raw + used, sizeof(raw) - used);
        if (nr > 0) {
            used += (size_t)nr;
            continue;
        }
        if (nr == 0) break;
        if (errno == EINTR) continue;
        (void)fprintf(stderr, "Could not read presentation request.\n"); // obs-ok:detached-child-terminal-diagnostic
        return 2;
    }
    if (used > UI_PRESENT_REQUEST_MAX) {
        (void)fprintf(stderr, "Presentation request is oversized.\n"); // obs-ok:detached-child-terminal-diagnostic
        return 2;
    }

    struct ui_present_qr_request request;
    memset(&request, 0, sizeof(request));
    char why[192];
    if (!ui_present_qr_request_parse(raw, used, &request, why, sizeof(why))) {
        (void)fprintf(stderr, "Presentation request rejected: %s\n", why); // obs-ok:detached-child-terminal-diagnostic
        return 2;
    }
    if (!qr_popup_show(request.payload, request.title, why, sizeof(why))) {
        (void)fprintf(stderr, "Presentation window failed: %s\n", why); // obs-ok:detached-child-terminal-diagnostic
        return 1;
    }
    return 0;
}
