/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: QR-specific renderer over the reusable native presentation ABI. */

#include "views/qr_popup.h"

#include "encoding/qr.h"
#include "presentation/presentation.h"
#include "presentation/zclassic_brand.h"

#include <stdio.h>
#include <stdlib.h>

bool qr_popup_show(const char *payload, const char *title,
                   char *error, size_t error_cap)
{
    struct qr_matrix matrix;
    if (!qr_matrix_encode(payload, &matrix, error, error_cap))
        return false; // raw-return-ok:encoder supplied bounded caller error

    uint32_t full_modules = matrix.width + 2u * ZCL_QR_QUIET_MODULES;
    uint32_t scale = 480u / full_modules;
    if (scale < 2u) scale = 2u;
    if (scale > 12u) scale = 12u;

    uint8_t *pixels = NULL;
    uint32_t side = 0;
    if (!qr_matrix_render_rgb(&matrix, scale, ZCL_QR_QUIET_MODULES,
                              &pixels, &side, error, error_cap)) {
        qr_matrix_free(&matrix);
        return false;
    }

    uint8_t icon[ZCL_PRESENT_ZCLASSIC_ICON_RGBA_BYTES];
    if (!zcl_present_zclassic_icon_rgba(icon, sizeof(icon))) {
        free(pixels);
        qr_matrix_free(&matrix);
        if (error && error_cap > 0)
            (void)snprintf(error, error_cap,
                           "ZClassic presentation icon is unavailable");
        return false;
    }

    char window_title[ZCL_PRESENT_TITLE_MAX + 1u];
    const char *base_title = title && title[0] ? title : "ZClassic23 deposit";
    (void)snprintf(window_title, sizeof(window_title),
                   "%s — C copies, Esc closes", base_title);
    struct zcl_present_window_v1 request = {
        .struct_size = sizeof(request),
        .abi_version = ZCL_PRESENT_ABI_V1,
        .title = window_title,
        .pixels = pixels,
        .width = side,
        .height = side,
        .pixel_format = ZCL_PRESENT_RGB8,
        .icon_rgba = icon,
        .icon_width = ZCL_PRESENT_ZCLASSIC_ICON_WIDTH,
        .icon_height = ZCL_PRESENT_ZCLASSIC_ICON_HEIGHT,
        .copy_text = payload,
    };
    bool shown = zcl_present_window_run_v1(&request, error, error_cap);
    free(pixels);
    qr_matrix_free(&matrix);
    return shown;
}
