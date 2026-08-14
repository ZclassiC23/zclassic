/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "encoding/qr.h"
#include "json/json.h"
#include "presentation/canvas.h"
#include "presentation/model.h"
#include "presentation/model_render.h"
#include "presentation/presentation.h"
#include "presentation/zclassic_brand.h"
#include "views/qr_popup.h"
#include "views/ui_present.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int qr_failures;

#define QR_CHECK(name, condition) do {                                      \
    printf("  %-58s ", (name));                                             \
    if (condition) printf("PASS\n");                                       \
    else { printf("FAIL\n"); qr_failures++; }                              \
} while (0)

static bool finder_matches(const struct qr_matrix *matrix, uint32_t ox,
                           uint32_t oy)
{
    for (uint32_t y = 0; y < 7; y++) {
        for (uint32_t x = 0; x < 7; x++) {
            bool expected = x == 0 || x == 6 || y == 0 || y == 6 ||
                            (x >= 2 && x <= 4 && y >= 2 && y <= 4);
            bool actual = (matrix->modules[(size_t)(oy + y) * matrix->width +
                                           ox + x] & 1u) != 0;
            if (actual != expected) return false;
        }
    }
    return true;
}

int test_qr(void)
{
    printf("\n=== qr ===\n");
    qr_failures = 0;
    QR_CHECK("native QR backend is compiled", qr_matrix_backend_available());
    if (!qr_matrix_backend_available()) return qr_failures;

    char why[128];
    struct qr_matrix first;
    struct qr_matrix second;
    bool encoded = qr_matrix_encode(
        "zclassic:t1QRNativeC23?amount=0.01000000", &first,
        why, sizeof(why));
    QR_CHECK("payment URI encodes", encoded);
    if (!encoded) return qr_failures;
    QR_CHECK("matrix has a standards-shaped version width",
             first.width >= 21u && first.width <= 177u &&
             (first.width - 21u) % 4u == 0u);
    QR_CHECK("top-left finder pattern is exact", finder_matches(&first, 0, 0));
    QR_CHECK("top-right finder pattern is exact",
             finder_matches(&first, first.width - 7u, 0));
    QR_CHECK("bottom-left finder pattern is exact",
             finder_matches(&first, 0, first.width - 7u));

    bool encoded_again = qr_matrix_encode(
        "zclassic:t1QRNativeC23?amount=0.01000000", &second,
        why, sizeof(why));
    QR_CHECK("same payload encodes deterministically",
             encoded_again && second.width == first.width &&
             memcmp(second.modules, first.modules,
                    (size_t)first.width * first.width) == 0);

    uint8_t *pixels = NULL;
    uint32_t side = 0;
    bool rendered = qr_matrix_render_rgb(&first, 3, ZCL_QR_QUIET_MODULES,
                                         &pixels, &side, why, sizeof(why));
    QR_CHECK("RGB renderer succeeds", rendered);
    QR_CHECK("RGB renderer uses exact integer dimensions",
             rendered && side == (first.width + 8u) * 3u);
    QR_CHECK("quiet-zone corner is white",
             rendered && pixels[0] == 0xff && pixels[1] == 0xff &&
             pixels[2] == 0xff);
    size_t dark = ((size_t)ZCL_QR_QUIET_MODULES * 3u * side +
                   ZCL_QR_QUIET_MODULES * 3u) * 3u;
    QR_CHECK("first finder module renders black",
             rendered && pixels[dark] == 0 && pixels[dark + 1] == 0 &&
             pixels[dark + 2] == 0);
    free(pixels);

    char oversized[ZCL_QR_MAX_PAYLOAD + 2u];
    memset(oversized, 'x', sizeof(oversized));
    oversized[sizeof(oversized) - 1u] = '\0';
    struct qr_matrix rejected;
    QR_CHECK("empty payload is rejected",
             !qr_matrix_encode("", &rejected, why, sizeof(why)));
    QR_CHECK("oversized payload is rejected",
             !qr_matrix_encode(oversized, &rejected, why, sizeof(why)));

    struct ui_present_qr_request request;
    static const char wire[] =
        "{\"payload\":\"zclassic:t1stdin?amount=0.01\","
        "\"title\":\"Deposit\"}";
    QR_CHECK("presentation stdin request parses",
             ui_present_qr_request_parse(wire, sizeof(wire) - 1u, &request,
                                         why, sizeof(why)));
    QR_CHECK("presentation payload survives JSON framing",
             strcmp(request.payload,
                    "zclassic:t1stdin?amount=0.01") == 0);
    QR_CHECK("presentation title survives JSON framing",
             strcmp(request.title, "Deposit") == 0);
    QR_CHECK("malformed presentation request is rejected",
             !ui_present_qr_request_parse("not-json", 8u, &request,
                                          why, sizeof(why)));
    QR_CHECK("empty presentation request is rejected",
             !ui_present_qr_request_parse("", 0, &request,
                                          why, sizeof(why)));

    uint8_t icon[ZCL_PRESENT_ZCLASSIC_ICON_RGBA_BYTES];
    QR_CHECK("canonical ZClassic window icon expands",
             zcl_present_zclassic_icon_rgba(icon, sizeof(icon)));
    bool saw_orange = false;
    bool saw_transparent = false;
    for (size_t i = 0; i < sizeof(icon); i += 4u) {
        if (icon[i] == 0xc8 && icon[i + 1u] == 0x70 &&
            icon[i + 2u] == 0x35 && icon[i + 3u] == 0xff)
            saw_orange = true;
        if (icon[i + 3u] == 0) saw_transparent = true;
    }
    QR_CHECK("canonical icon preserves brand color and transparency",
             saw_orange && saw_transparent);

    uint8_t canvas_pixels[32u * 32u * 3u];
    struct zcl_present_canvas canvas;
    QR_CHECK("reusable RGB canvas initializes",
             zcl_present_canvas_init(&canvas, canvas_pixels,
                                     sizeof(canvas_pixels), 32u, 32u));
    const struct zcl_present_color canvas_white = {0xff, 0xff, 0xff};
    const struct zcl_present_color canvas_orange = {0xc8, 0x70, 0x35};
    zcl_present_canvas_clear(&canvas, canvas_white);
    zcl_present_canvas_fill_rect(&canvas, -4, -4, 8u, 8u, canvas_orange);
    QR_CHECK("canvas primitives clip safely at the upper-left edge",
             canvas_pixels[0] == 0xc8 && canvas_pixels[1] == 0x70 &&
             canvas_pixels[2] == 0x35 &&
             canvas_pixels[((size_t)5u * 32u + 5u) * 3u] == 0xff);
    zcl_present_canvas_text(&canvas, 8, 8, "Aa", 2u, 16u, canvas_orange);
    bool saw_antialias = false;
    for (size_t i = 0; i < sizeof(canvas_pixels); i++) {
        if (canvas_pixels[i] != 0xff && canvas_pixels[i] != 0xc8 &&
            canvas_pixels[i] != 0x70 && canvas_pixels[i] != 0x35) {
            saw_antialias = true;
            break;
        }
    }
    QR_CHECK("embedded Basic Latin text is antialiased", saw_antialias);
    uint32_t balance_width =
        zcl_present_canvas_text_width("balance", 7u, 16u);
    QR_CHECK("proportional canvas text metrics are deterministic",
             balance_width > 40u && balance_width < 80u &&
             balance_width ==
                 zcl_present_canvas_text_width("balance", 7u, 16u));

    struct qr_popup_card deposit_card;
    QR_CHECK("ZCL URI composes as a branded deposit card",
             qr_popup_card_render(
                 "zclassic:t1QRNativeC23?label=phone&amount=0.01000000",
                 "ignored fixture title", &deposit_card,
                 why, sizeof(why)));
    QR_CHECK("deposit card identifies exact address and amount",
             deposit_card.is_deposit &&
             strcmp(deposit_card.address, "t1QRNativeC23") == 0 &&
             strcmp(deposit_card.amount, "0.01000000") == 0);
    QR_CHECK("deposit card has stable presentation dimensions",
             deposit_card.pixels &&
             deposit_card.width == ZCL_QR_POPUP_CARD_WIDTH &&
             deposit_card.height == ZCL_QR_POPUP_CARD_HEIGHT);
    bool card_has_orange = false;
    for (size_t i = 0; deposit_card.pixels &&
         i < ZCL_QR_POPUP_CARD_BYTES; i += 3u) {
        if (deposit_card.pixels[i] == 0xc8 &&
            deposit_card.pixels[i + 1u] == 0x70 &&
            deposit_card.pixels[i + 2u] == 0x35) {
            card_has_orange = true;
            break;
        }
    }
    QR_CHECK("deposit card carries ZClassic orange branding in pixels",
             card_has_orange);
    qr_popup_card_free(&deposit_card);

    struct qr_popup_card generic_card;
    QR_CHECK("non-payment text stays explicitly generic",
             qr_popup_card_render("generic metadata", "Metadata",
                                  &generic_card, why, sizeof(why)) &&
             !generic_card.is_deposit &&
             strcmp(generic_card.address, "generic metadata") == 0);
    qr_popup_card_free(&generic_card);

    static const uint8_t tiny_rgb[] = {
        0xff, 0xff, 0xff, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0xff, 0xff, 0xff,
    };
    struct zcl_present_window_v1 present = {
        .struct_size = sizeof(present),
        .abi_version = ZCL_PRESENT_ABI_V1,
        .title = "Presentation validation fixture",
        .pixels = tiny_rgb,
        .width = 2,
        .height = 2,
        .pixel_format = ZCL_PRESENT_RGB8,
        .icon_rgba = icon,
        .icon_width = ZCL_PRESENT_ZCLASSIC_ICON_WIDTH,
        .icon_height = ZCL_PRESENT_ZCLASSIC_ICON_HEIGHT,
        .copy_text = "fixture",
    };
    QR_CHECK("portable presentation request validates",
             zcl_present_window_validate_v1(&present, why, sizeof(why)));
    present.abi_version++;
    QR_CHECK("presentation ABI mismatch fails closed",
             !zcl_present_window_validate_v1(&present, why, sizeof(why)));
    QR_CHECK("presentation backend is the pinned software backend",
             strcmp(zcl_present_backend_name(), "rgfw-1.8.1-software") == 0);
    QR_CHECK("presentation uses stable desktop application identity",
             strcmp(ZCL_PRESENT_APPLICATION_ID,
                    "org.zclassic.ZClassic23") == 0);

    struct zcl_present_model_v1 visual;
    zcl_present_model_init_v1(&visual, ZCL_PRESENT_MODEL_PROGRESS);
    (void)snprintf(visual.request_id, sizeof(visual.request_id),
                   "reproduce-42");
    (void)snprintf(visual.title, sizeof(visual.title),
                   "Independent reproduction");
    (void)snprintf(visual.summary, sizeof(visual.summary),
                   "Builder two is reproducing the exact candidate bytes.");
    visual.item_count = 1;
    visual.items[0].kind = ZCL_PRESENT_ITEM_PROGRESS;
    visual.items[0].status = ZCL_PRESENT_STATUS_INFO;
    visual.items[0].parent_index = ZCL_PRESENT_MODEL_PARENT_NONE;
    visual.items[0].numerator = 7;
    visual.items[0].denominator = 10;
    (void)snprintf(visual.items[0].id, sizeof(visual.items[0].id),
                   "builder-two");
    (void)snprintf(visual.items[0].label, sizeof(visual.items[0].label),
                   "Builder two");
    (void)snprintf(visual.items[0].value, sizeof(visual.items[0].value),
                   "Compiling");
    visual.action_count = 1;
    visual.actions[0].kind = ZCL_PRESENT_ACTION_CLOSE;
    (void)snprintf(visual.actions[0].id, sizeof(visual.actions[0].id),
                   "close");
    (void)snprintf(visual.actions[0].label,
                   sizeof(visual.actions[0].label), "Close");
    QR_CHECK("renderer-neutral progress model validates",
             zcl_present_model_validate_v1(&visual, why, sizeof(why)));

    uint8_t model_wire[ZCL_PRESENT_MODEL_WIRE_MAX];
    size_t model_wire_len = 0;
    struct zcl_present_model_v1 decoded;
    QR_CHECK("visual model encodes without structure padding",
             zcl_present_model_encode_v1(
                 &visual, model_wire, sizeof(model_wire), &model_wire_len,
                 why, sizeof(why)) && model_wire_len > 0);
    QR_CHECK("visual model round-trips exactly",
             zcl_present_model_decode_v1(
                 model_wire, model_wire_len, &decoded, why, sizeof(why)) &&
             decoded.kind == visual.kind &&
             decoded.item_count == 1 &&
             decoded.items[0].numerator == 7 &&
             decoded.items[0].denominator == 10 &&
             strcmp(decoded.items[0].value, "Compiling") == 0);
    QR_CHECK("visual model rejects trailing wire bytes",
             model_wire_len + 1u < sizeof(model_wire) &&
             !zcl_present_model_decode_v1(
                 model_wire, model_wire_len + 1u, &decoded,
                 why, sizeof(why)));

    struct zcl_present_model_v1 confirmation;
    zcl_present_model_init_v1(&confirmation,
                              ZCL_PRESENT_MODEL_CONFIRMATION);
    (void)snprintf(confirmation.request_id,
                   sizeof(confirmation.request_id), "publish-7");
    (void)snprintf(confirmation.title, sizeof(confirmation.title),
                   "Publish exact candidate?");
    memset(confirmation.exact_root, 'a', ZCL_PRESENT_MODEL_ROOT_MAX);
    confirmation.exact_root[ZCL_PRESENT_MODEL_ROOT_MAX] = '\0';
    confirmation.action_count = 2;
    confirmation.actions[0].kind = ZCL_PRESENT_ACTION_CONFIRM;
    (void)snprintf(confirmation.actions[0].id,
                   sizeof(confirmation.actions[0].id), "confirm");
    (void)snprintf(confirmation.actions[0].label,
                   sizeof(confirmation.actions[0].label), "Publish");
    confirmation.actions[1].kind = ZCL_PRESENT_ACTION_CANCEL;
    (void)snprintf(confirmation.actions[1].id,
                   sizeof(confirmation.actions[1].id), "cancel");
    (void)snprintf(confirmation.actions[1].label,
                   sizeof(confirmation.actions[1].label), "Cancel");
    QR_CHECK("exact confirmation binds a root and two explicit actions",
             zcl_present_model_validate_v1(
                 &confirmation, why, sizeof(why)));
    confirmation.exact_root[0] = '\0';
    QR_CHECK("rootless publication confirmation fails closed",
             !zcl_present_model_validate_v1(
                 &confirmation, why, sizeof(why)));

    struct zcl_present_model_bitmap_v1 visual_bitmap;
    QR_CHECK("renderer-neutral progress card becomes native RGB pixels",
             zcl_present_model_render_v1(
                 &visual, &visual_bitmap, why, sizeof(why)) &&
             visual_bitmap.pixels &&
             visual_bitmap.width == ZCL_PRESENT_MODEL_BITMAP_WIDTH &&
             visual_bitmap.height == ZCL_PRESENT_MODEL_BITMAP_HEIGHT);
    bool visual_has_orange = false;
    bool visual_has_info = false;
    for (size_t i = 0; visual_bitmap.pixels &&
         i < ZCL_PRESENT_MODEL_BITMAP_BYTES; i += 3u) {
        visual_has_orange |= visual_bitmap.pixels[i] == 0xc8 &&
            visual_bitmap.pixels[i + 1u] == 0x70 &&
            visual_bitmap.pixels[i + 2u] == 0x35;
        visual_has_info |= visual_bitmap.pixels[i] == 0x32 &&
            visual_bitmap.pixels[i + 1u] == 0x68 &&
            visual_bitmap.pixels[i + 2u] == 0x91;
    }
    QR_CHECK("native model pixels preserve brand and semantic status",
             visual_has_orange && visual_has_info);
    zcl_present_model_bitmap_free_v1(&visual_bitmap);

    static const char model_json[] =
        "{\"kind\":\"code-diff\",\"request_id\":\"diff-1\","
        "\"title\":\"Exact candidate diff\","
        "\"summary\":\"One candidate-owned line changed.\","
        "\"items\":[{\"kind\":\"diff-remove\",\"value\":\"return 0;\"},"
        "{\"kind\":\"diff-add\",\"status\":\"green\","
        "\"value\":\"return verified;\"}]}";
    struct json_value visual_json;
    json_init(&visual_json);
    QR_CHECK("typed native visual JSON parses",
             json_read(&visual_json, model_json, sizeof(model_json) - 1u));
    struct zcl_present_model_v1 json_model;
    bool visual_json_ok = ui_present_model_from_json(
        &visual_json, &json_model, why, sizeof(why));
    if (!visual_json_ok) printf("  visual JSON diagnostic: %s\n", why);
    QR_CHECK("closed visual JSON becomes the renderer-neutral model",
             visual_json_ok &&
             json_model.kind == ZCL_PRESENT_MODEL_CODE_DIFF &&
             json_model.item_count == 2 &&
             json_model.items[1].kind == ZCL_PRESENT_ITEM_DIFF_ADD);
    json_free(&visual_json);

    static const char smuggled_json[] =
        "{\"kind\":\"status\",\"request_id\":\"bad-1\","
        "\"title\":\"Bad\",\"items\":[{\"kind\":\"text\","
        "\"value\":\"x\",\"command\":\"/bin/sh\"}]}";
    json_init(&visual_json);
    QR_CHECK("unknown visual item key fixture parses as JSON",
             json_read(&visual_json, smuggled_json,
                       sizeof(smuggled_json) - 1u));
    QR_CHECK("visual model rejects command smuggling",
             !ui_present_model_from_json(&visual_json, &json_model,
                                         why, sizeof(why)));
    json_free(&visual_json);

    qr_matrix_free(&second);
    qr_matrix_free(&first);
    printf("=== qr: %d failure(s) ===\n", qr_failures);
    return qr_failures;
}
