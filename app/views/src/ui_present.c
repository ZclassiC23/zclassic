/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: detached same-binary launcher for reviewed local UI windows. */

#include "views/ui_present.h"

#include "json/json.h"
#include "platform/os_proc.h"
#include "presentation/model_render.h"
#include "presentation/presentation.h"
#include "presentation/zclassic_brand.h"
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

static bool ui_present_text(char *out, size_t cap,
                            const struct json_value *value,
                            bool required, char *error, size_t error_cap)
{
    const char *text = json_get_str(value);
    if (!text) {
        if (!required) {
            out[0] = '\0';
            return true;
        }
        ui_present_error(error, error_cap,
                         "required visual model text is missing");
        return false;
    }
    size_t length = strlen(text);
    if (length >= cap || (required && length == 0)) {
        ui_present_error(error, error_cap,
                         "visual model text is empty or oversized");
        return false;
    }
    memcpy(out, text, length + 1u);
    return true;
}

static bool ui_present_keys_allowed(const struct json_value *object,
                                    const char *const allowed[],
                                    size_t allowed_count,
                                    char *error, size_t error_cap)
{
    if (!object || object->type != JSON_OBJ) {
        ui_present_error(error, error_cap,
                         "visual model member must be an object");
        return false;
    }
    for (size_t i = 0; i < object->num_children; i++) {
        bool found = false;
        for (size_t j = 0; j < allowed_count; j++)
            found |= strcmp(object->keys[i], allowed[j]) == 0;
        if (!found) {
            ui_present_error(error, error_cap,
                             "visual model contains an unknown key");
            return false;
        }
    }
    return true;
}

static uint16_t ui_present_model_kind(const char *name)
{
    static const char *const names[] = {
        "", "qr", "status", "table", "progress", "chart", "timeline",
        "code-diff", "evidence-graph", "choice", "confirmation", "form",
        "canvas",
    };
    if (!name) return 0;
    for (uint16_t i = 1; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(name, names[i]) == 0) return i;
    return 0;
}

static uint16_t ui_present_item_kind(const char *name)
{
    static const char *const names[] = {
        "", "text", "key-value", "table-header", "table-row", "progress",
        "chart-point", "timeline-event", "diff-context", "diff-add",
        "diff-remove", "graph-node", "choice", "form-field",
    };
    if (!name) return 0;
    for (uint16_t i = 1; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(name, names[i]) == 0) return i;
    return 0;
}

static uint16_t ui_present_status(const char *name)
{
    static const char *const names[] = {
        "neutral", "info", "green", "yellow", "red",
    };
    if (!name || !name[0]) return ZCL_PRESENT_STATUS_NEUTRAL;
    for (uint16_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(name, names[i]) == 0) return i;
    return UINT16_MAX;
}

static uint16_t ui_present_action_kind(const char *name)
{
    static const char *const names[] = {
        "", "close", "copy", "select", "confirm", "cancel", "submit",
    };
    if (!name) return 0;
    for (uint16_t i = 1; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(name, names[i]) == 0) return i;
    return 0;
}

static bool ui_present_u32(const struct json_value *value, uint32_t *out,
                           uint32_t default_value,
                           char *error, size_t error_cap)
{
    if (!value) {
        *out = default_value;
        return true;
    }
    if (value->type != JSON_INT || value->val.i < 0 ||
        (uint64_t)value->val.i > UINT32_MAX) {
        ui_present_error(error, error_cap,
                         "visual model integer is out of range");
        return false;
    }
    *out = (uint32_t)value->val.i;
    return true;
}

static bool ui_present_parse_item(const struct json_value *value,
                                  struct zcl_present_model_item_v1 *item,
                                  char *error, size_t error_cap)
{
    static const char *const allowed[] = {
        "kind", "status", "id", "label", "value", "numerator",
        "denominator", "parent_index", "selected", "required", "read_only",
    };
    if (!ui_present_keys_allowed(value, allowed,
                                 sizeof(allowed) / sizeof(allowed[0]),
                                 error, error_cap))
        return false;
    memset(item, 0, sizeof(*item));
    item->parent_index = ZCL_PRESENT_MODEL_PARENT_NONE;
    item->kind = ui_present_item_kind(
        json_get_str(json_get(value, "kind")));
    item->status = ui_present_status(
        json_get_str(json_get(value, "status")));
    if (!item->kind) {
        ui_present_error(error, error_cap,
                         "visual model item kind is invalid");
        return false;
    }
    if (item->status == UINT16_MAX) {
        ui_present_error(error, error_cap,
                         "visual model item status is invalid");
        return false;
    }
    if (!ui_present_text(item->id, sizeof(item->id), json_get(value, "id"),
                         false, error, error_cap) ||
        !ui_present_text(item->label, sizeof(item->label),
                         json_get(value, "label"), false, error, error_cap) ||
        !ui_present_text(item->value, sizeof(item->value),
                         json_get(value, "value"), false, error, error_cap) ||
        !ui_present_u32(json_get(value, "numerator"), &item->numerator, 0,
                        error, error_cap) ||
        !ui_present_u32(json_get(value, "denominator"), &item->denominator, 0,
                        error, error_cap)) {
        return false;
    }
    uint32_t parent = 0;
    if (json_get(value, "parent_index")) {
        if (!ui_present_u32(json_get(value, "parent_index"), &parent, 0,
                            error, error_cap) || parent > UINT16_MAX)
            return false;
        item->parent_index = (uint16_t)parent;
    }
    static const struct { const char *name; uint16_t flag; } flags[] = {
        {"selected", ZCL_PRESENT_ITEM_SELECTED},
        {"required", ZCL_PRESENT_ITEM_REQUIRED},
        {"read_only", ZCL_PRESENT_ITEM_READ_ONLY},
    };
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        const struct json_value *flag = json_get(value, flags[i].name);
        if (flag && flag->type != JSON_BOOL) {
            ui_present_error(error, error_cap,
                             "visual model item flag must be boolean");
            return false;
        }
        if (flag && flag->val.b) item->flags |= flags[i].flag;
    }
    return true;
}

static bool ui_present_parse_action(
    const struct json_value *value,
    struct zcl_present_model_action_v1 *action,
    char *error, size_t error_cap)
{
    static const char *const allowed[] = {"kind", "id", "label"};
    if (!ui_present_keys_allowed(value, allowed,
                                 sizeof(allowed) / sizeof(allowed[0]),
                                 error, error_cap))
        return false;
    memset(action, 0, sizeof(*action));
    action->kind = ui_present_action_kind(
        json_get_str(json_get(value, "kind")));
    if (!action->kind ||
        !ui_present_text(action->id, sizeof(action->id),
                         json_get(value, "id"), true, error, error_cap) ||
        !ui_present_text(action->label, sizeof(action->label),
                         json_get(value, "label"), true, error, error_cap)) {
        if (error && error_cap > 0 && !error[0])
            ui_present_error(error, error_cap,
                             "visual model action kind is invalid");
        return false;
    }
    return true;
}

bool ui_present_model_from_json(const struct json_value *input,
                                struct zcl_present_model_v1 *out,
                                char *error, size_t error_cap)
{
    static const char *const allowed[] = {
        "kind", "request_id", "title", "summary", "exact_root",
        "items", "actions",
    };
    if (error && error_cap > 0) error[0] = '\0';
    if (!out || !ui_present_keys_allowed(
            input, allowed, sizeof(allowed) / sizeof(allowed[0]),
            error, error_cap))
        return false;
    uint16_t kind = ui_present_model_kind(
        json_get_str(json_get(input, "kind")));
    zcl_present_model_init_v1(out, (enum zcl_present_model_kind)kind);
    if (!kind ||
        !ui_present_text(out->request_id, sizeof(out->request_id),
                         json_get(input, "request_id"), true,
                         error, error_cap) ||
        !ui_present_text(out->title, sizeof(out->title),
                         json_get(input, "title"), true,
                         error, error_cap) ||
        !ui_present_text(out->summary, sizeof(out->summary),
                         json_get(input, "summary"), false,
                         error, error_cap) ||
        !ui_present_text(out->exact_root, sizeof(out->exact_root),
                         json_get(input, "exact_root"), false,
                         error, error_cap)) {
        if (error && error_cap > 0 && !error[0])
            ui_present_error(error, error_cap,
                             "visual model kind is invalid");
        return false;
    }
    const struct json_value *items = json_get(input, "items");
    if (items && (items->type != JSON_ARR ||
                  json_size(items) > ZCL_PRESENT_MODEL_ITEMS_MAX)) {
        ui_present_error(error, error_cap,
                         "visual model items are not a bounded array");
        return false;
    }
    out->item_count = items ? (uint32_t)json_size(items) : 0;
    for (uint32_t i = 0; i < out->item_count; i++)
        if (!ui_present_parse_item(json_at(items, i), &out->items[i],
                                   error, error_cap))
            return false;
    const struct json_value *actions = json_get(input, "actions");
    if (actions && (actions->type != JSON_ARR ||
                    json_size(actions) > ZCL_PRESENT_MODEL_ACTIONS_MAX)) {
        ui_present_error(error, error_cap,
                         "visual model actions are not a bounded array");
        return false;
    }
    out->action_count = actions ? (uint32_t)json_size(actions) : 0;
    for (uint32_t i = 0; i < out->action_count; i++)
        if (!ui_present_parse_action(json_at(actions, i), &out->actions[i],
                                     error, error_cap))
            return false;
    return zcl_present_model_validate_v1(out, error, error_cap);
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

struct zcl_result ui_present_model_launch(
    const struct zcl_present_model_v1 *model)
{
    uint8_t wire[ZCL_PRESENT_MODEL_WIRE_MAX];
    size_t wire_len = 0;
    char why[192];
    if (!zcl_present_model_encode_v1(model, wire, sizeof(wire), &wire_len,
                                     why, sizeof(why)))
        return ZCL_ERR(-1, "ui_present_model_launch: %s", why);

    char executable[PATH_MAX];
    if (!os_proc_exe_path(executable, sizeof(executable)))
        return ZCL_ERR(-errno,
                       "ui_present_model_launch: executable path unavailable: %s",
                       strerror(errno));
    const char *argv[] = {
        executable,
        "--ui-present-child=model",
        NULL,
    };
    return zcl_spawn_detached_input(argv, wire, wire_len, NULL);
}

static bool ui_present_model_show(const uint8_t *wire, size_t wire_len,
                                  char *error, size_t error_cap)
{
    struct zcl_present_model_v1 model;
    if (!zcl_present_model_decode_v1(wire, wire_len, &model,
                                     error, error_cap))
        return false; // raw-return-ok:model decoder supplied bounded error
    struct zcl_present_model_bitmap_v1 bitmap;
    if (!zcl_present_model_render_v1(&model, &bitmap, error, error_cap))
        return false; // raw-return-ok:model renderer supplied bounded error
    uint8_t icon[ZCL_PRESENT_ZCLASSIC_ICON_RGBA_BYTES];
    if (!zcl_present_zclassic_icon_rgba(icon, sizeof(icon))) {
        zcl_present_model_bitmap_free_v1(&bitmap);
        ui_present_error(error, error_cap,
                         "ZClassic presentation icon is unavailable");
        return false;
    }
    char title[ZCL_PRESENT_TITLE_MAX + 1u];
    (void)snprintf(title, sizeof(title), "ZClassic23 — %s",
                   model.title);
    struct zcl_present_window_v1 request = {
        .struct_size = sizeof(request),
        .abi_version = ZCL_PRESENT_ABI_V1,
        .title = title,
        .pixels = bitmap.pixels,
        .width = bitmap.width,
        .height = bitmap.height,
        .pixel_format = ZCL_PRESENT_RGB8,
        .icon_rgba = icon,
        .icon_width = ZCL_PRESENT_ZCLASSIC_ICON_WIDTH,
        .icon_height = ZCL_PRESENT_ZCLASSIC_ICON_HEIGHT,
        .copy_text = model.exact_root[0] ? model.exact_root : NULL,
    };
    bool shown = zcl_present_window_run_v1(&request, error, error_cap);
    zcl_present_model_bitmap_free_v1(&bitmap);
    return shown;
}

int ui_present_child_main(const char *kind)
{
    bool is_qr = kind && strcmp(kind, "qr") == 0;
    bool is_model = kind && strcmp(kind, "model") == 0;
    if (!is_qr && !is_model) {
        (void)fprintf(stderr, "Unsupported presentation kind.\n"); // obs-ok:detached-child-terminal-diagnostic
        return 2;
    }

    uint8_t raw[ZCL_PRESENT_MODEL_WIRE_MAX + 1u];
    size_t request_max = is_model ? ZCL_PRESENT_MODEL_WIRE_MAX
                                  : UI_PRESENT_REQUEST_MAX;
    size_t used = 0;
    while (used <= request_max) {
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
    if (used > request_max) {
        (void)fprintf(stderr, "Presentation request is oversized.\n"); // obs-ok:detached-child-terminal-diagnostic
        return 2;
    }

    char why[192];
    if (is_model) {
        if (!ui_present_model_show(raw, used, why, sizeof(why))) {
            (void)fprintf(stderr, "Presentation window failed: %s\n", why); // obs-ok:detached-child-terminal-diagnostic
            return 1;
        }
        return 0;
    }

    struct ui_present_qr_request request;
    memset(&request, 0, sizeof(request));
    if (!ui_present_qr_request_parse((const char *)raw, used, &request,
                                     why, sizeof(why))) {
        (void)fprintf(stderr, "Presentation request rejected: %s\n", why); // obs-ok:detached-child-terminal-diagnostic
        return 2;
    }
    if (!qr_popup_show(request.payload, request.title, why, sizeof(why))) {
        (void)fprintf(stderr, "Presentation window failed: %s\n", why); // obs-ok:detached-child-terminal-diagnostic
        return 1;
    }
    return 0;
}
