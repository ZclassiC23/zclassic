/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: native software cards for renderer-neutral agent documents. */

#include "presentation/model_render.h"

#include "presentation/canvas.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct zcl_present_color PAPER = {0xfb, 0xfa, 0xf8};
static const struct zcl_present_color INK = {0x20, 0x20, 0x22};
static const struct zcl_present_color MUTED = {0x69, 0x65, 0x60};
static const struct zcl_present_color RULE = {0xdf, 0xd8, 0xcf};
static const struct zcl_present_color ORANGE = {0xc8, 0x70, 0x35};
static const struct zcl_present_color INFO = {0x32, 0x68, 0x91};
static const struct zcl_present_color GREEN = {0x28, 0x72, 0x4a};
static const struct zcl_present_color YELLOW = {0x9a, 0x6d, 0x18};
static const struct zcl_present_color RED = {0xa1, 0x37, 0x37};
static const struct zcl_present_color GREEN_BG = {0xe5, 0xf2, 0xe9};
static const struct zcl_present_color RED_BG = {0xf8, 0xe5, 0xe3};
static const struct zcl_present_color PANEL = {0xf3, 0xf0, 0xeb};

static bool render_error(char *error, size_t cap, const char *message)
{
    if (error && cap > 0) (void)snprintf(error, cap, "%s", message);
    return false;
}

static struct zcl_present_color status_color(uint16_t status)
{
    switch (status) {
    case ZCL_PRESENT_STATUS_INFO: return INFO;
    case ZCL_PRESENT_STATUS_GREEN: return GREEN;
    case ZCL_PRESENT_STATUS_YELLOW: return YELLOW;
    case ZCL_PRESENT_STATUS_RED: return RED;
    default: return MUTED;
    }
}

static void text_fit(struct zcl_present_canvas *canvas, int32_t x, int32_t y,
                     const char *text, uint32_t height, uint32_t max_width,
                     struct zcl_present_color color)
{
    size_t length = strlen(text);
    while (length > 0 && zcl_present_canvas_text_width(
               text, length, height) > max_width)
        length--;
    zcl_present_canvas_text(canvas, x, y, text, length, height, color);
    if (text[length]) {
        const char dots[] = "...";
        uint32_t used = zcl_present_canvas_text_width(text, length, height);
        uint32_t dots_width = zcl_present_canvas_text_width(dots, 3u, height);
        while (length > 0 && used + dots_width > max_width) {
            length--;
            used = zcl_present_canvas_text_width(text, length, height);
        }
        zcl_present_canvas_text(canvas, x + (int32_t)used, y,
                                dots, 3u, height, color);
    }
}

static int32_t render_progress(struct zcl_present_canvas *canvas,
                               const struct zcl_present_model_item_v1 *item,
                               int32_t y)
{
    text_fit(canvas, 42, y, item->label, 16u, 390u, INK);
    text_fit(canvas, 470, y, item->value, 14u, 205u, MUTED);
    zcl_present_canvas_fill_rect(canvas, 42, y + 28, 636u, 12u, RULE);
    uint32_t filled = item->denominator == 0 ? 0 :
        (uint32_t)((uint64_t)636u * item->numerator / item->denominator);
    zcl_present_canvas_fill_rect(canvas, 42, y + 28, filled, 12u,
                                 status_color(item->status));
    return y + 58;
}

static int32_t render_diff(struct zcl_present_canvas *canvas,
                           const struct zcl_present_model_item_v1 *item,
                           int32_t y)
{
    struct zcl_present_color bg = PAPER;
    struct zcl_present_color fg = INK;
    const char *prefix = "  ";
    if (item->kind == ZCL_PRESENT_ITEM_DIFF_ADD) {
        bg = GREEN_BG;
        fg = GREEN;
        prefix = "+ ";
    } else if (item->kind == ZCL_PRESENT_ITEM_DIFF_REMOVE) {
        bg = RED_BG;
        fg = RED;
        prefix = "- ";
    }
    zcl_present_canvas_fill_rect(canvas, 32, y, 656u, 27u, bg);
    zcl_present_canvas_text(canvas, 42, y + 5, prefix, 2u, 14u, fg);
    text_fit(canvas, 62, y + 5, item->value, 14u, 610u, fg);
    return y + 28;
}

static int32_t render_row(struct zcl_present_canvas *canvas,
                          const struct zcl_present_model_item_v1 *item,
                          int32_t y)
{
    struct zcl_present_color accent = status_color(item->status);
    zcl_present_canvas_fill_rect(canvas, 42, y + 3, 4u, 31u, accent);
    text_fit(canvas, 58, y + 2, item->label, 14u, 238u, MUTED);
    text_fit(canvas, 310, y + 2, item->value, 16u, 366u, INK);
    zcl_present_canvas_line(canvas, 42, y + 38, 678, y + 38, RULE);
    return y + 45;
}

static int32_t render_item(struct zcl_present_canvas *canvas,
                           const struct zcl_present_model_item_v1 *item,
                           int32_t y)
{
    if (item->kind == ZCL_PRESENT_ITEM_PROGRESS)
        return render_progress(canvas, item, y);
    if (item->kind == ZCL_PRESENT_ITEM_DIFF_CONTEXT ||
        item->kind == ZCL_PRESENT_ITEM_DIFF_ADD ||
        item->kind == ZCL_PRESENT_ITEM_DIFF_REMOVE)
        return render_diff(canvas, item, y);
    if (item->kind == ZCL_PRESENT_ITEM_TEXT) {
        text_fit(canvas, 42, y, item->value[0] ? item->value : item->label,
                 16u, 636u, INK);
        return y + 34;
    }
    return render_row(canvas, item, y);
}

static void render_actions(struct zcl_present_canvas *canvas,
                           const struct zcl_present_model_v1 *model)
{
    if (model->action_count == 0) return;
    int32_t y = ZCL_PRESENT_MODEL_ACTION_Y;
    uint32_t total_gap = ZCL_PRESENT_MODEL_ACTION_GAP *
                         (model->action_count - 1u);
    uint32_t width = (ZCL_PRESENT_MODEL_ACTION_WIDTH - total_gap) /
                     model->action_count;
    for (uint32_t i = 0; i < model->action_count; i++) {
        int32_t x = ZCL_PRESENT_MODEL_ACTION_X +
                    (int32_t)(i * (width + ZCL_PRESENT_MODEL_ACTION_GAP));
        bool decisive = model->actions[i].kind == ZCL_PRESENT_ACTION_CONFIRM ||
                        model->actions[i].kind == ZCL_PRESENT_ACTION_SUBMIT;
        struct zcl_present_color fill = decisive ? ORANGE : PANEL;
        struct zcl_present_color text = decisive ? PAPER : INK;
        zcl_present_canvas_fill_rect(canvas, x, y, width,
                                     ZCL_PRESENT_MODEL_ACTION_HEIGHT, fill);
        char numbered[64];
        (void)snprintf(numbered, sizeof(numbered), "%u  %s", i + 1u,
                       model->actions[i].label);
        uint32_t text_width = zcl_present_canvas_text_width(
            numbered, strlen(numbered), 15u);
        int32_t text_x = text_width < width
            ? x + (int32_t)(width - text_width) / 2 : x + 8;
        text_fit(canvas, text_x, y + 12, numbered, 15u, width - 16u, text);
    }
}

bool zcl_present_model_render_v1(const struct zcl_present_model_v1 *model,
                                 struct zcl_present_model_bitmap_v1 *bitmap,
                                 char *error, size_t error_cap)
{
    if (!bitmap)
        return render_error(error, error_cap,
                            "visual model bitmap output is missing");
    *bitmap = (struct zcl_present_model_bitmap_v1){0};
    if (!zcl_present_model_validate_v1(model, error, error_cap)) return false;
    uint8_t *pixels = malloc(ZCL_PRESENT_MODEL_BITMAP_BYTES); // raw-alloc-ok:standalone-presentation-package
    if (!pixels)
        return render_error(error, error_cap,
                            "visual model bitmap allocation failed");
    struct zcl_present_canvas canvas;
    if (!zcl_present_canvas_init(&canvas, pixels,
                                 ZCL_PRESENT_MODEL_BITMAP_BYTES,
                                 ZCL_PRESENT_MODEL_BITMAP_WIDTH,
                                 ZCL_PRESENT_MODEL_BITMAP_HEIGHT)) {
        free(pixels);
        return render_error(error, error_cap,
                            "visual model canvas initialization failed");
    }
    zcl_present_canvas_clear(&canvas, PAPER);
    zcl_present_canvas_fill_rect(&canvas, 0, 0, 12u, canvas.height, ORANGE);
    zcl_present_canvas_text(&canvas, 42, 28, "ZCLASSIC23", 10u, 14u, ORANGE);
    const char *kind = zcl_present_model_kind_name(model->kind);
    text_fit(&canvas, 520, 28, kind, 12u, 158u, MUTED);
    text_fit(&canvas, 42, 60, model->title, 30u, 636u, INK);
    if (model->summary[0])
        text_fit(&canvas, 42, 104, model->summary, 15u, 636u, MUTED);
    if (model->exact_root[0]) {
        char root_line[86];
        (void)snprintf(root_line, sizeof(root_line), "Exact root  %.16s...",
                       model->exact_root);
        text_fit(&canvas, 42, 134, root_line, 12u, 636u, MUTED);
    }
    zcl_present_canvas_line(&canvas, 42, 164, 678, 164, RULE);

    int32_t y = 184;
    uint32_t rendered = 0;
    for (uint32_t i = 0; i < model->item_count; i++) {
        if (y > 604) break;
        y = render_item(&canvas, &model->items[i], y);
        rendered++;
    }
    if (rendered < model->item_count) {
        char omitted[64];
        (void)snprintf(omitted, sizeof(omitted),
                       "%u more bounded item%s not shown",
                       model->item_count - rendered,
                       model->item_count - rendered == 1u ? "" : "s");
        text_fit(&canvas, 42, 614, omitted, 12u, 636u, MUTED);
    }
    render_actions(&canvas, model);
    bitmap->pixels = pixels;
    bitmap->width = canvas.width;
    bitmap->height = canvas.height;
    if (error && error_cap > 0) error[0] = '\0';
    return true;
}

void zcl_present_model_bitmap_free_v1(
    struct zcl_present_model_bitmap_v1 *bitmap)
{
    if (!bitmap) return;
    free(bitmap->pixels);
    *bitmap = (struct zcl_present_model_bitmap_v1){0};
}
