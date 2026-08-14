/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: bounded cross-platform native bitmap presentation capability. */

#ifndef ZCL_PRESENTATION_PRESENTATION_H
#define ZCL_PRESENTATION_PRESENTATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZCL_PRESENT_ABI_V1 1u
#define ZCL_PRESENT_APPLICATION_ID "org.zclassic.ZClassic23"
#define ZCL_PRESENT_TITLE_MAX 127u
#define ZCL_PRESENT_COPY_TEXT_MAX 4096u
#define ZCL_PRESENT_DIMENSION_MAX 2048u
#define ZCL_PRESENT_WINDOW_ACTIONS_MAX 4u
#define ZCL_PRESENT_WINDOW_PAGES_MAX 16u

enum zcl_present_pixel_format {
    ZCL_PRESENT_RGB8 = 3,
    ZCL_PRESENT_RGBA8 = 4,
};

/* Pointer-only inputs are borrowed for the duration of the blocking call.
 * Pixels must be tightly packed, row-major, and exactly width*height*channels
 * bytes. The reviewed host decides whether an untrusted App/ZCode request may
 * receive this local-human-output capability; this API grants no process,
 * network, wallet, or filesystem authority. */
struct zcl_present_window_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    const char *title;
    const uint8_t *pixels;
    uint32_t width;
    uint32_t height;
    enum zcl_present_pixel_format pixel_format;
    const uint8_t *icon_rgba;
    uint32_t icon_width;
    uint32_t icon_height;
    const char *copy_text;
};

/* A fixed, bounded sequence of inert bitmaps shown in one native window.
 * Page selection is local display state only: it grants no capability and
 * produces no software-authority event. */
struct zcl_present_window_pages_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    const struct zcl_present_window_v1 *pages;
    uint32_t page_count;
};

enum zcl_present_window_outcome {
    ZCL_PRESENT_WINDOW_DISMISSED = 1,
    ZCL_PRESENT_WINDOW_ACTION = 2,
};

struct zcl_present_window_event_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t outcome;
    uint32_t action_index;
};

/* Called after the native window and software surface exist and the first
 * bitmap has been blitted. The callback belongs to the reviewed host, never
 * to the inert visual document or fetched code. */
typedef void (*zcl_present_window_ready_fn)(void *context);

/* Pure validation, suitable for package hosts before they cross the native UI
 * boundary. `error` is always a bounded human-readable explanation on false. */
bool zcl_present_window_validate_v1(
    const struct zcl_present_window_v1 *request,
    char *error, size_t error_cap);

/* Open one native, resizable software-rendered window and block until the user
 * closes it. Resizing preserves aspect ratio; Escape/Q close and C copies
 * copy_text when it is present. */
bool zcl_present_window_run_v1(
    const struct zcl_present_window_v1 *request,
    char *error, size_t error_cap);

/* Interactive host variant. Number keys 1..action_count return only a bounded
 * zero-based action index; Escape/Q/window-close return DISMISSED. Labels and
 * authority remain outside this backend. */
bool zcl_present_window_run_actions_v1(
    const struct zcl_present_window_v1 *request,
    uint32_t action_count,
    zcl_present_window_ready_fn ready,
    void *ready_context,
    struct zcl_present_window_event_v1 *event,
    char *error, size_t error_cap);

/* Multi-page host variant. PgUp/PgDn, arrows, Home/End, and the mouse wheel
 * select among already-bounded bitmaps. Numbered actions retain their exact
 * meaning on every page. */
bool zcl_present_window_run_pages_actions_v1(
    const struct zcl_present_window_pages_v1 *request,
    uint32_t action_count,
    zcl_present_window_ready_fn ready,
    void *ready_context,
    struct zcl_present_window_event_v1 *event,
    char *error, size_t error_cap);

/* Pure clamped page transition used by the backend and sensitivity tests. */
bool zcl_present_window_page_step_v1(
    uint32_t current_page, uint32_t page_count, int32_t delta,
    uint32_t *next_page);

/* Deterministic hit test for the standard renderer-neutral model action row.
 * Window pixels are aspect-fit, so letterboxing and resize are accounted for
 * before an action index is returned. */
bool zcl_present_window_action_at_v1(
    uint32_t source_width, uint32_t source_height,
    int32_t target_width, int32_t target_height,
    int32_t mouse_x, int32_t mouse_y, uint32_t action_count,
    uint32_t *action_index);

/* Stable diagnostic labels; neither string implies graphics acceleration. */
const char *zcl_present_backend_name(void);
const char *zcl_present_platform_name(void);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_PRESENTATION_PRESENTATION_H */
