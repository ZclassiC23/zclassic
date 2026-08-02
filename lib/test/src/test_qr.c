/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "encoding/qr.h"
#include "presentation/presentation.h"
#include "presentation/zclassic_brand.h"
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

    qr_matrix_free(&second);
    qr_matrix_free(&first);
    printf("=== qr: %d failure(s) ===\n", qr_failures);
    return qr_failures;
}
