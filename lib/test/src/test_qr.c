/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "encoding/qr.h"
#include "command/native_command.h"
#include "json/json.h"
#include "presentation/canvas.h"
#include "presentation/model.h"
#include "presentation/model_render.h"
#include "presentation/model_text.h"
#include "presentation/presentation.h"
#include "presentation/zclassic_brand.h"
#include "views/qr_popup.h"
#include "views/ui_present.h"
#include "vcs/zcode_work_node.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int qr_failures;

#define QR_CHECK(name, condition) do {                                      \
    printf("  %-58s ", (name));                                             \
    if (condition) printf("PASS\n");                                       \
    else { printf("FAIL\n"); qr_failures++; }                              \
} while (0)

static void qr_reproduction_facts(struct json_value *facts,
                                  const char *action, const char *candidate,
                                  const char *event, const char *receipt,
                                  const char *state)
{
    json_init(facts); json_set_object(facts);
    json_push_kv_str(facts, "schema", "zcl.build_fabric_action_state.v1");
    json_push_kv_bool(facts, "found", true);
    json_push_kv_bool(facts, "event_root_rederived", true);
    json_push_kv_str(facts, "action_id", action);
    json_push_kv_str(facts, "candidate_root", candidate);
    json_push_kv_str(facts, "event_root", event);
    json_push_kv_str(facts, "receipt_root", receipt);
    json_push_kv_str(facts, "state", state);
}

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

    struct zcl_present_model_v1 deposit_model;
    QR_CHECK("deposit payload becomes one bounded QR visual model",
             zcl_present_model_qr_from_payload_v1(
                 "zclassic:t1QRNativeC23?label=phone&amount=0.01000000",
                 "ignored fixture title", &deposit_model,
                 why, sizeof(why)));
    struct qr_popup_card deposit_card;
    QR_CHECK("ZCL URI composes as a branded deposit card",
             qr_popup_card_render(&deposit_model, &deposit_card,
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

    struct zcl_present_model_v1 generic_model;
    QR_CHECK("generic payload becomes the same closed QR model shape",
             zcl_present_model_qr_from_payload_v1(
                 "generic metadata", "Metadata", &generic_model,
                 why, sizeof(why)));
    struct qr_popup_card generic_card;
    QR_CHECK("non-payment text stays explicitly generic",
             qr_popup_card_render(&generic_model, &generic_card,
                                  why, sizeof(why)) &&
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
    present.abi_version = ZCL_PRESENT_ABI_V1;
    struct zcl_present_window_event_v1 bounded_event;
    QR_CHECK("native action keys remain bounded to four",
             !zcl_present_window_run_actions_v1(
                 &present, ZCL_PRESENT_WINDOW_ACTIONS_MAX + 1u,
                 NULL, NULL, &bounded_event, why, sizeof(why)));
    uint32_t clicked_action = UINT32_MAX;
    QR_CHECK("native confirmation click selects the first exact action",
             zcl_present_window_action_at_v1(
                 720, 720, 720, 720, 100, 670, 2, &clicked_action) &&
             clicked_action == 0);
    QR_CHECK("resized confirmation click selects the second exact action",
             zcl_present_window_action_at_v1(
                 720, 720, 1440, 1440, 1100, 1340, 2,
                 &clicked_action) && clicked_action == 1);
    QR_CHECK("action gap and letterbox clicks return no decision",
             !zcl_present_window_action_at_v1(
                 720, 720, 720, 720, 360, 670, 2,
                 &clicked_action) &&
             !zcl_present_window_action_at_v1(
                 720, 720, 1000, 720, 100, 670, 2,
                 &clicked_action));
    struct zcl_present_model_v1 rejected_qr;
    QR_CHECK("shared QR model rejects an empty payload",
             !zcl_present_model_qr_from_payload_v1(
                 "", "Empty", &rejected_qr, why, sizeof(why)));
    char oversized_qr[ZCL_QR_MAX_PAYLOAD + 2u];
    memset(oversized_qr, 'x', sizeof(oversized_qr) - 1u);
    oversized_qr[sizeof(oversized_qr) - 1u] = '\0';
    QR_CHECK("shared QR model rejects oversized bytes",
             !zcl_present_model_qr_from_payload_v1(
                 oversized_qr, "Oversized", &rejected_qr,
                 why, sizeof(why)));
    char max_qr[ZCL_QR_MAX_PAYLOAD + 1u];
    memset(max_qr, 'q', sizeof(max_qr) - 1u);
    max_qr[sizeof(max_qr) - 1u] = '\0';
    char recovered_qr[ZCL_PRESENT_MODEL_QR_PAYLOAD_MAX + 1u];
    QR_CHECK("maximum QR bytes use all ordered model chunks",
             zcl_present_model_qr_from_payload_v1(
                 max_qr, "Maximum", &rejected_qr, why, sizeof(why)) &&
             rejected_qr.item_count == ZCL_PRESENT_MODEL_QR_CHUNKS_MAX &&
             zcl_present_model_qr_payload_v1(
                 &rejected_qr, recovered_qr, why, sizeof(why)) &&
             strcmp(recovered_qr, max_qr) == 0);
    char qr_text[ZCL_PRESENT_MODEL_TEXT_MAX];
    size_t qr_text_len = 0;
    uint32_t qr_text_pages = 0;
    QR_CHECK("QR text companion pages the exact model payload chunks",
             zcl_present_model_text_page_v1(
                 &rejected_qr, 0, qr_text, sizeof(qr_text), &qr_text_len,
                 &qr_text_pages, why, sizeof(why)) &&
             qr_text_pages == ZCL_PRESENT_MODEL_QR_CHUNKS_MAX &&
             qr_text_len < sizeof(qr_text) &&
             strstr(qr_text, "page: 1/8") != NULL &&
             strstr(qr_text, "payload-bytes: 1-256 of 2048") != NULL &&
             strstr(qr_text, rejected_qr.items[0].value) != NULL &&
             zcl_present_model_text_page_v1(
                 &rejected_qr, ZCL_PRESENT_MODEL_QR_CHUNKS_MAX - 1u,
                 qr_text, sizeof(qr_text), &qr_text_len, &qr_text_pages,
                 why, sizeof(why)) &&
             strstr(qr_text, "payload-bytes: 1793-2048 of 2048") != NULL &&
             !zcl_present_model_text_page_v1(
                 &rejected_qr, ZCL_PRESENT_MODEL_QR_CHUNKS_MAX,
                 qr_text, sizeof(qr_text), &qr_text_len, &qr_text_pages,
                 why, sizeof(why)));
    rejected_qr.items[1].id[0] = 'x';
    QR_CHECK("reordered QR payload chunks fail closed",
             !zcl_present_model_validate_v1(
                 &rejected_qr, why, sizeof(why)));
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
    char visual_text[ZCL_PRESENT_MODEL_TEXT_MAX];
    char visual_text_again[ZCL_PRESENT_MODEL_TEXT_MAX];
    size_t visual_text_len = 0, visual_text_len_again = 0;
    uint32_t visual_text_pages = 0, visual_text_pages_again = 0;
    bool visual_text_ok = zcl_present_model_text_page_v1(
        &visual, 0, visual_text, sizeof(visual_text), &visual_text_len,
        &visual_text_pages, why, sizeof(why));
    bool visual_text_again_ok = zcl_present_model_text_page_v1(
        &visual, 0, visual_text_again, sizeof(visual_text_again),
        &visual_text_len_again, &visual_text_pages_again,
        why, sizeof(why));
    QR_CHECK("same model produces one deterministic text companion",
             visual_text_ok && visual_text_again_ok &&
             visual_text_pages == 1u && visual_text_pages_again == 1u &&
             visual_text_len == visual_text_len_again &&
             strcmp(visual_text, visual_text_again) == 0 &&
             strstr(visual_text, "kind: progress") != NULL &&
             strstr(visual_text, "progress: 7/10") != NULL &&
             strstr(visual_text, "action 1: close") != NULL &&
             strstr(visual_text, "authority: display-only") != NULL);

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

    struct json_value status_facts, health_facts, health_checks;
    struct json_value backup_facts, work_facts;
    json_init(&status_facts); json_set_object(&status_facts);
    json_push_kv_int(&status_facts, "provable_tip", 3216084);
    json_push_kv_bool(&status_facts, "provable_tip_published", true);
    json_push_kv_bool(&status_facts, "sync_gap_known", true);
    json_push_kv_int(&status_facts, "sync_gap", 0);
    json_push_kv_int(&status_facts, "peers", 6);
    json_init(&health_facts); json_set_object(&health_facts);
    json_init(&health_checks); json_set_object(&health_checks);
    json_push_kv_bool(&health_checks, "tor_enabled", true);
    json_push_kv_bool(&health_checks, "onion_service_ready", true);
    json_push_kv_str(&health_checks, "onion_address", "fixture.onion");
    json_push_kv(&health_facts, "checks", &health_checks);
    json_free(&health_checks);
    json_init(&backup_facts); json_set_object(&backup_facts);
    json_push_kv_int(&backup_facts, "total_runs", 3);
    json_push_kv_int(&backup_facts, "total_failures", 0);
    json_push_kv_int(&backup_facts, "last_run_unix", 1234);
    json_push_kv_str(&backup_facts, "last_error", "");
    json_init(&work_facts); json_set_object(&work_facts);
    json_push_kv_bool(&work_facts, "enabled", true);
    json_push_kv_int(&work_facts, "worker_capacity", 4);
    json_push_kv_int(&work_facts, "worker_active", 1);
    json_push_kv_int(&work_facts, "worker_available", 3);
    struct zcl_present_model_v1 status_model;
    QR_CHECK("canonical status facts build one closed native model",
             zcl_native_presentation_status_model_from_facts(
                 &status_facts, &health_facts, &backup_facts, &work_facts,
                 &status_model, why, sizeof(why)) &&
             status_model.kind == ZCL_PRESENT_MODEL_STATUS_CARD &&
             status_model.item_count == 6);
    QR_CHECK("status model labels fact authority and preserves capacity",
             strncmp(status_model.items[0].label, "NODE FACT - ", 12) == 0 &&
             strcmp(status_model.items[0].value, "3216084") == 0 &&
             strcmp(status_model.items[5].value,
                    "3 available / 4 total (1 active)") == 0);
    QR_CHECK("dark canonical sources stay unavailable, never false-disabled",
             zcl_native_presentation_status_model_from_facts(
                 &status_facts, NULL, &backup_facts, NULL,
                 &status_model, why, sizeof(why)) &&
             strcmp(status_model.items[3].value, "unavailable") == 0 &&
             strcmp(status_model.items[5].value, "unavailable") == 0);
    json_free(&work_facts);
    json_free(&backup_facts);
    json_free(&health_facts);
    json_free(&status_facts);

    struct json_value corpus_facts;
    json_init(&corpus_facts); json_set_object(&corpus_facts);
    json_push_kv_bool(&corpus_facts, "projection_ready", true);
    json_push_kv_str(&corpus_facts, "checkpoint_root",
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
    json_push_kv_int(&corpus_facts, "admitted_production_loc", 201600);
    json_push_kv_int(&corpus_facts, "admitted_test_loc", 390954);
    json_push_kv_int(&corpus_facts, "durably_hosted_loc", 0);
    json_push_kv_int(&corpus_facts, "unique_semantic_units", 434817);
    json_push_kv_int(&corpus_facts, "packages_admitted", 50);
    json_push_kv_int(&corpus_facts, "packages_excluded", 18);
    json_push_kv_str(&corpus_facts, "progress_stage", "below_50m");
    json_push_kv_str(&corpus_facts, "blocker",
                     "verified lower bound is 592554 LOC");
    struct zcl_present_model_v1 corpus_model;
    QR_CHECK("canonical corpus status builds one exact native instrument",
             zcl_native_presentation_corpus_model_from_facts(
                 &corpus_facts, &corpus_model, why, sizeof(why)) &&
             corpus_model.kind == ZCL_PRESENT_MODEL_STATUS_CARD &&
             strcmp(corpus_model.title, "10 Million Exact C23") == 0 &&
             strcmp(corpus_model.exact_root,
                "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd")
                == 0 &&
             corpus_model.item_count == 10);
    bool corpus_package_exact = false;
    bool corpus_used_honest = false;
    bool corpus_velocity_honest = false;
    for (uint32_t i = 0; i < corpus_model.item_count; i++) {
        const struct zcl_present_model_item_v1 *item =
            &corpus_model.items[i];
        corpus_package_exact |= strcmp(item->id, "packages") == 0 &&
                                strcmp(item->value, "50 packages") == 0;
        corpus_used_honest |= strcmp(item->id, "used-loc") == 0 &&
            strcmp(item->value, "unavailable (not checkpoint-bound)") == 0 &&
            item->status == ZCL_PRESENT_STATUS_YELLOW;
        corpus_velocity_honest |= strcmp(item->id, "velocity") == 0 &&
            strcmp(item->value,
                   "unavailable (previous checkpoint not bound)") == 0 &&
            item->status == ZCL_PRESENT_STATUS_YELLOW;
    }
    QR_CHECK("corpus instrument preserves exact package and exclusion facts",
             corpus_package_exact &&
             strcmp(corpus_model.items[6].value,
                    "18 entries; reason LOC unavailable") == 0);
    QR_CHECK("corpus instrument never fabricates used LOC or velocity",
             corpus_used_honest && corpus_velocity_honest);
    json_free(&corpus_facts);

    struct json_value corpus_request_input;
    json_init(&corpus_request_input); json_set_object(&corpus_request_input);
    json_push_kv_str(&corpus_request_input, "output", "text");
    struct zcl_command_request corpus_request = {
        .input = &corpus_request_input,
    };
    struct zcl_command_reply corpus_reply;
    zcl_command_reply_init(&corpus_reply,
                           "zcl.app_presentation_corpus.v1");
    zcl_native_handle_presentation_corpus(&corpus_request, &corpus_reply);
    const char *corpus_text =
        json_get_str(json_get(&corpus_reply.data, "plain_text"));
    QR_CHECK("typed corpus instrument is headless and display-only end to end",
             corpus_reply.status == ZCL_COMMAND_STATUS_PASSED &&
             !json_get_bool(json_get(&corpus_reply.data, "launched")) &&
             corpus_text && strstr(corpus_text, "10 Million Exact C23") &&
             strstr(corpus_text, "CORPUS FACT - Admitted production") &&
             strstr(corpus_text, "value: unavailable") &&
             strcmp(json_get_str(json_get(&corpus_reply.data, "authority")),
                    "display-only") == 0);
    zcl_command_reply_free(&corpus_reply);
    json_free(&corpus_request_input);

    static const uint8_t code_before[] =
        "#include \"presentation/model.h\"\n"
        "int exact_value(void) {\n"
        "    return 1;\n"
        "}\n";
    static const uint8_t code_after[] =
        "#include \"presentation/model.h\"\n"
        "int exact_value(void) {\n"
        "    return 2;\n"
        "}\n";
    char root_a[65], root_b[65], tree_root[65];
    memset(root_a, 'a', 64u); root_a[64] = '\0';
    memset(root_b, 'b', 64u); root_b[64] = '\0';
    memset(tree_root, 'c', 64u); tree_root[64] = '\0';
    struct zcl_present_model_v1 code_model;
    QR_CHECK("exact C facts build a provenance-labeled code-change model",
             zcl_native_presentation_code_change_model_from_facts(
                 code_before, sizeof(code_before) - 1u,
                 code_after, sizeof(code_after) - 1u,
                 "tools/command/native_qr_command.c", "return two",
                 "returned one", "returns two", root_a, root_b, tree_root,
                 &code_model, why, sizeof(why)) &&
             code_model.kind == ZCL_PRESENT_MODEL_CODE_DIFF &&
             strcmp(code_model.exact_root, tree_root) == 0 &&
             strncmp(code_model.items[0].label, "AGENT SUMMARY - ", 16) == 0 &&
             strncmp(code_model.items[3].label, "LOCAL OBSERVATION - ", 20) == 0);
    bool caught_remove = false, caught_add = false, caught_include = false;
    for (uint32_t i = 0; i < code_model.item_count; i++) {
        caught_remove |= code_model.items[i].kind ==
                         ZCL_PRESENT_ITEM_DIFF_REMOVE &&
                         strcmp(code_model.items[i].value, "    return 1;") == 0;
        caught_add |= code_model.items[i].kind == ZCL_PRESENT_ITEM_DIFF_ADD &&
                      strcmp(code_model.items[i].value, "    return 2;") == 0;
        caught_include |= strcmp(code_model.items[i].id, "dependencies") == 0 &&
                          strcmp(code_model.items[i].value,
                                 "presentation/model.h") == 0;
    }
    QR_CHECK("code-change diff catches the semantic mutant in exact bytes",
             caught_remove && caught_add);
    QR_CHECK("candidate dependency row comes from exact include bytes",
             caught_include);
    QR_CHECK("unchanged candidate bytes cannot masquerade as a code change",
             !zcl_native_presentation_code_change_model_from_facts(
                 code_before, sizeof(code_before) - 1u,
                 code_before, sizeof(code_before) - 1u,
                 "tools/command/native_qr_command.c", "return two",
                 "returned one", "returns two", root_a, root_a, tree_root,
                 &code_model, why, sizeof(why)));

    struct json_value publication_plan, publication_release;
    struct json_value publication_package;
    json_init(&publication_plan); json_set_object(&publication_plan);
    json_push_kv_bool(&publication_plan, "valid", true);
    json_push_kv_bool(&publication_plan, "ready_to_commit", true);
    json_push_kv_str(&publication_plan, "plan_token", root_a);
    json_init(&publication_release); json_set_object(&publication_release);
    json_push_kv_str(&publication_release, "name", "stranger/hello-c23");
    json_push_kv_str(&publication_release, "semver", "1.0.0");
    json_push_kv_str(&publication_release, "license", "Apache-2.0");
    json_push_kv(&publication_plan, "release", &publication_release);
    json_free(&publication_release);
    json_init(&publication_package); json_set_object(&publication_package);
    json_push_kv_str(&publication_package, "package_root", root_b);
    json_push_kv_int(&publication_package, "files", 3);
    json_push_kv_int(&publication_package, "bytes", 4096);
    json_push_kv_int(&publication_package, "chunks", 3);
    json_push_kv_bool(&publication_package, "chunks_checked", true);
    json_push_kv(&publication_plan, "package", &publication_package);
    json_free(&publication_package);
    struct zcl_present_model_v1 publication_model;
    QR_CHECK("canonical package plan builds exact inert confirmation",
             zcl_native_presentation_publication_confirm_model_from_plan(
                 &publication_plan, &publication_model,
                 why, sizeof(why)) &&
             publication_model.kind == ZCL_PRESENT_MODEL_CONFIRMATION &&
             strcmp(publication_model.exact_root, root_a) == 0 &&
             publication_model.item_count == 13 &&
             publication_model.action_count == 2 &&
             publication_model.actions[0].kind ==
                 ZCL_PRESENT_ACTION_CONFIRM &&
             publication_model.actions[1].kind ==
                 ZCL_PRESENT_ACTION_CANCEL);
    QR_CHECK("confirmation chrome and effect text are ZClassic23-authored",
             strcmp(publication_model.actions[0].label,
                    "Confirm exact local commit") == 0 &&
             strcmp(publication_model.actions[1].label,
                    "Cancel - make no change") == 0 &&
             strncmp(publication_model.items[0].label,
                     "LOCAL OBSERVATION - ", 20) == 0 &&
             strstr(publication_model.summary, "HUMAN DECISION - ") != NULL);
    QR_CHECK("confirmation names every later publication evidence boundary",
             strcmp(publication_model.items[7].value,
                    "Pending this exact decision") == 0 &&
             strcmp(publication_model.items[8].value,
                    "Not started - separate commit required") == 0 &&
             strcmp(publication_model.items[9].value, "Not observed") == 0 &&
             strcmp(publication_model.items[10].value, "Not observed") == 0 &&
             strcmp(publication_model.items[11].value, "Not observed") == 0 &&
             strcmp(publication_model.items[12].value, "Not observed") == 0);
    json_free(&publication_plan);
    json_init(&publication_plan); json_set_object(&publication_plan);
    QR_CHECK("agent facts alone cannot fabricate a ready confirmation",
             !zcl_native_presentation_publication_confirm_model_from_plan(
                 &publication_plan, &publication_model,
                 why, sizeof(why)));
    json_free(&publication_plan);

    struct json_value reproduction_facts;
    qr_reproduction_facts(&reproduction_facts, root_a, tree_root, root_b,
                          "", "RUNNING");
    struct zcl_present_model_v1 reproduction_model;
    QR_CHECK("canonical running event builds six fixed progress stages",
             zcl_native_presentation_reproduction_model_from_facts(
                 &reproduction_facts, &reproduction_model,
                 why, sizeof(why)) &&
             reproduction_model.kind == ZCL_PRESENT_MODEL_PROGRESS &&
             reproduction_model.item_count == 6 &&
             reproduction_model.items[2].numerator == 1 &&
             reproduction_model.items[3].numerator == 0);
    char running_request_id[ZCL_PRESENT_MODEL_ID_MAX + 1u];
    (void)snprintf(running_request_id, sizeof(running_request_id), "%s",
                   reproduction_model.request_id);
    json_free(&reproduction_facts);
    qr_reproduction_facts(&reproduction_facts, root_a, tree_root, root_b,
                          root_b, "REPRODUCED");
    QR_CHECK("matching evidence updates the same action-bound window",
             zcl_native_presentation_reproduction_model_from_facts(
                 &reproduction_facts, &reproduction_model,
                 why, sizeof(why)) &&
             strcmp(reproduction_model.request_id, running_request_id) == 0 &&
             reproduction_model.items[5].numerator == 1 &&
             reproduction_model.items[5].status == ZCL_PRESENT_STATUS_GREEN);
    json_free(&reproduction_facts);
    qr_reproduction_facts(&reproduction_facts, root_a, tree_root, root_b,
                          root_b, "REMOTE_RED");
    QR_CHECK("remote mismatch stays a named red output refusal",
             zcl_native_presentation_reproduction_model_from_facts(
                 &reproduction_facts, &reproduction_model,
                 why, sizeof(why)) &&
             reproduction_model.items[3].status == ZCL_PRESENT_STATUS_RED &&
             strstr(reproduction_model.summary, "named refusal") != NULL);
    json_free(&reproduction_facts);

    struct json_value work_dump;
    json_init(&work_dump);
    vcs_zcode_work_node_set_global(NULL);
    QR_CHECK("package-worker diagnostic reports exact disabled capacity",
             vcs_zcode_work_node_dump_state_json(&work_dump, NULL) &&
             !json_get_bool(json_get(&work_dump, "enabled")) &&
             json_get_int(json_get(&work_dump, "worker_capacity")) == 0 &&
             json_get_int(json_get(&work_dump, "worker_available")) == 0);
    json_free(&work_dump);

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

    struct zcl_present_model_v1 long_table;
    zcl_present_model_init_v1(&long_table, ZCL_PRESENT_MODEL_TABLE);
    (void)snprintf(long_table.request_id, sizeof(long_table.request_id),
                   "bounded-table-64");
    (void)snprintf(long_table.title, sizeof(long_table.title),
                   "Every bounded row is reachable");
    long_table.item_count = ZCL_PRESENT_MODEL_ITEMS_MAX;
    for (uint32_t i = 0; i < long_table.item_count; i++) {
        long_table.items[i].kind = ZCL_PRESENT_ITEM_TABLE_ROW;
        long_table.items[i].parent_index = ZCL_PRESENT_MODEL_PARENT_NONE;
        (void)snprintf(long_table.items[i].id,
                       sizeof(long_table.items[i].id), "row-%u", i + 1u);
        (void)snprintf(long_table.items[i].label,
                       sizeof(long_table.items[i].label), "Owner %u", i + 1u);
        (void)snprintf(long_table.items[i].value,
                       sizeof(long_table.items[i].value), "Exact value %u",
                       i + 1u);
    }
    uint32_t table_pages = 0;
    QR_CHECK("maximum bounded table partitions into eight exact pages",
             zcl_present_model_page_count_v1(
                 &long_table, &table_pages, why, sizeof(why)) &&
             table_pages == 8u);
    char table_text[ZCL_PRESENT_MODEL_TEXT_MAX];
    size_t table_text_len = 0;
    uint32_t table_text_pages = 0;
    QR_CHECK("maximum table text export is bounded and fully paged",
             zcl_present_model_text_page_v1(
                 &long_table, 63u, table_text, sizeof(table_text),
                 &table_text_len, &table_text_pages, why, sizeof(why)) &&
             table_text_pages == 64u &&
             table_text_len < sizeof(table_text) &&
             strstr(table_text, "id: row-64") != NULL &&
             !zcl_present_model_text_page_v1(
                 &long_table, table_text_pages, table_text,
                 sizeof(table_text), &table_text_len, &table_text_pages,
                 why, sizeof(why)));
    struct zcl_present_model_bitmap_v1 first_page, last_page;
    bool first_page_ok = zcl_present_model_render_page_v1(
        &long_table, 0, &first_page, why, sizeof(why));
    bool last_page_ok = zcl_present_model_render_page_v1(
        &long_table, table_pages - 1u, &last_page, why, sizeof(why));
    QR_CHECK("first and last bounded table pages render distinct pixels",
             first_page_ok && last_page_ok &&
             memcmp(first_page.pixels, last_page.pixels,
                    ZCL_PRESENT_MODEL_BITMAP_BYTES) != 0);
    QR_CHECK("page past the exact model bound fails closed",
             !zcl_present_model_render_page_v1(
                 &long_table, table_pages, &visual_bitmap,
                 why, sizeof(why)));
    zcl_present_model_bitmap_free_v1(&first_page);
    zcl_present_model_bitmap_free_v1(&last_page);
    uint32_t next_page = UINT32_MAX;
    QR_CHECK("keyboard page movement advances and clamps deterministically",
             zcl_present_window_page_step_v1(
                 0, table_pages, 1, &next_page) && next_page == 1u &&
             zcl_present_window_page_step_v1(
                 table_pages - 1u, table_pages, 1, &next_page) &&
             next_page == table_pages - 1u &&
             zcl_present_window_page_step_v1(
                 0, table_pages, -1, &next_page) && next_page == 0u);

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
