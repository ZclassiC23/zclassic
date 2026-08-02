/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Standalone libzclpresentation example and cross-platform link proof. */

#include "presentation/presentation.h"
#include "presentation/zclassic_brand.h"

#include <stdint.h>

int main(void)
{
    uint8_t pixels[128u * 128u * 3u];
    for (uint32_t y = 0; y < 128u; y++) {
        for (uint32_t x = 0; x < 128u; x++) {
            size_t at = ((size_t)y * 128u + x) * 3u;
            bool orange = ((x / 16u) + (y / 16u)) % 2u == 0;
            pixels[at] = orange ? 0xc8 : 0xff;
            pixels[at + 1u] = orange ? 0x70 : 0xff;
            pixels[at + 2u] = orange ? 0x35 : 0xff;
        }
    }
    uint8_t icon[ZCL_PRESENT_ZCLASSIC_ICON_RGBA_BYTES];
    if (!zcl_present_zclassic_icon_rgba(icon, sizeof(icon))) return 2;
    struct zcl_present_window_v1 request = {
        .struct_size = sizeof(request),
        .abi_version = ZCL_PRESENT_ABI_V1,
        .title = "libzclpresentation — C copies, Esc closes",
        .pixels = pixels,
        .width = 128u,
        .height = 128u,
        .pixel_format = ZCL_PRESENT_RGB8,
        .icon_rgba = icon,
        .icon_width = ZCL_PRESENT_ZCLASSIC_ICON_WIDTH,
        .icon_height = ZCL_PRESENT_ZCLASSIC_ICON_HEIGHT,
        .copy_text = "libzclpresentation",
    };
    char error[192];
    return zcl_present_window_run_v1(&request, error, sizeof(error)) ? 0 : 1;
}
