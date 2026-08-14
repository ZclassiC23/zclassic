/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: deterministic software renderer for bounded visual documents. */

#ifndef ZCL_PRESENTATION_MODEL_RENDER_H
#define ZCL_PRESENTATION_MODEL_RENDER_H

#include "presentation/model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZCL_PRESENT_MODEL_BITMAP_WIDTH 720u
#define ZCL_PRESENT_MODEL_BITMAP_HEIGHT 720u
#define ZCL_PRESENT_MODEL_BITMAP_BYTES \
    (ZCL_PRESENT_MODEL_BITMAP_WIDTH * ZCL_PRESENT_MODEL_BITMAP_HEIGHT * 3u)
#define ZCL_PRESENT_MODEL_ACTION_X 42u
#define ZCL_PRESENT_MODEL_ACTION_Y 650u
#define ZCL_PRESENT_MODEL_ACTION_WIDTH 636u
#define ZCL_PRESENT_MODEL_ACTION_HEIGHT 42u
#define ZCL_PRESENT_MODEL_ACTION_GAP 12u

struct zcl_present_model_bitmap_v1 {
    uint8_t *pixels;
    uint32_t width;
    uint32_t height;
};

/* Render only inert model data into an owned RGB bitmap. The renderer performs
 * no input, filesystem, process, node, wallet, package, or network operation. */
bool zcl_present_model_render_v1(const struct zcl_present_model_v1 *model,
                                 struct zcl_present_model_bitmap_v1 *bitmap,
                                 char *error, size_t error_cap);
void zcl_present_model_bitmap_free_v1(
    struct zcl_present_model_bitmap_v1 *bitmap);

#endif /* ZCL_PRESENTATION_MODEL_RENDER_H */
