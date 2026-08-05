/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: portable RGFW software-window backend behind the bounded ABI. */

#include "presentation/presentation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RGFW is private implementation detail. These switches keep graphics on the
 * CPU, avoid OpenGL, and on Linux dynamically load the system X11 API instead
 * of adding a link-time dependency. Windows uses Win32; macOS uses Cocoa via
 * the Objective-C runtime from ordinary C. */
#define RGFW_IMPLEMENTATION
#define RGFW_NO_API
#define RGFW_NO_IOKIT
#if defined(__APPLE__) && defined(__clang__)
/* RGFW has two legacy numeric `_MSC_VER` probes. Do not define that macro on
 * Apple: current SDK headers use its presence to select actual MSVC syntax. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wundef"
#elif !defined(_MSC_VER)
/* RGFW probes the MSVC version numerically instead of with defined(). */
#define _MSC_VER 0
#define ZCL_PRESENT_UNDEF_MSC_VER
#endif
#if defined(__linux__)
#define RGFW_USE_XDL
#define RGFW_NO_X11_CURSOR
/* The presentation surface displays a software bitmap and never changes a
 * monitor mode, reads XRandR DPI metadata, or captures raw-input deltas.
 * Keep those optional X11 extensions out of the compile closure: ordinary
 * node/test builds must not depend on host Xrandr/XInput2 development
 * headers merely because a read-only popup exists. Core Xlib is loaded at
 * runtime by the vendored XDL layer. */
#define RGFW_NO_DPI
#define RGFW_NO_XINPUT2
#define RGFW_NO_X11_XI_PRELOAD
#define XDL_NO_GLX
#define XDL_NO_XRANDR
#endif
#include "../../../vendor/rgfw/RGFW.h"
#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic pop
#elif defined(ZCL_PRESENT_UNDEF_MSC_VER)
#undef _MSC_VER
#undef ZCL_PRESENT_UNDEF_MSC_VER
#endif

static bool present_error(char *error, size_t cap, const char *message)
{
    if (error && cap > 0)
        (void)snprintf(error, cap, "%s", message);
    return false;
}

static bool bounded_text(const char *text, size_t max)
{
    if (!text) return true;
    size_t n = 0;
    while (n <= max && text[n]) n++;
    return n <= max;
}

bool zcl_present_window_validate_v1(
    const struct zcl_present_window_v1 *request,
    char *error, size_t error_cap)
{
    if (!request || request->struct_size != sizeof(*request) ||
        request->abi_version != ZCL_PRESENT_ABI_V1)
        return present_error(error, error_cap,
                             "presentation ABI/structure mismatch");
    if (!bounded_text(request->title, ZCL_PRESENT_TITLE_MAX))
        return present_error(error, error_cap,
                             "presentation title is oversized");
    if (!bounded_text(request->copy_text, ZCL_PRESENT_COPY_TEXT_MAX))
        return present_error(error, error_cap,
                             "presentation clipboard text is oversized");
    if (!request->pixels || request->width == 0 || request->height == 0 ||
        request->width > ZCL_PRESENT_DIMENSION_MAX ||
        request->height > ZCL_PRESENT_DIMENSION_MAX)
        return present_error(error, error_cap,
                             "presentation bitmap dimensions are invalid");
    if (request->pixel_format != ZCL_PRESENT_RGB8 &&
        request->pixel_format != ZCL_PRESENT_RGBA8)
        return present_error(error, error_cap,
                             "presentation pixel format is unsupported");
    uint64_t pixel_bytes = (uint64_t)request->width * request->height *
                           (uint32_t)request->pixel_format;
    if (pixel_bytes == 0 || pixel_bytes > SIZE_MAX)
        return present_error(error, error_cap,
                             "presentation bitmap size overflows");
    bool any_icon = request->icon_rgba || request->icon_width ||
                    request->icon_height;
    bool complete_icon = request->icon_rgba && request->icon_width > 0 &&
                         request->icon_height > 0 &&
                         request->icon_width <= 256u &&
                         request->icon_height <= 256u;
    if (any_icon && !complete_icon)
        return present_error(error, error_cap,
                             "presentation icon is incomplete or oversized");
    if (error && error_cap > 0) error[0] = '\0';
    return true;
}

const char *zcl_present_backend_name(void)
{
    return "rgfw-1.8.1-software";
}

const char *zcl_present_platform_name(void)
{
#if defined(_WIN32)
    return "win32";
#elif defined(__APPLE__)
    return "cocoa";
#elif defined(__linux__)
    return "x11-dynamic";
#else
    return "unsupported";
#endif
}

#if defined(__linux__)
static void present_set_linux_desktop_identity(RGFW_window *window)
{
    Display *display = (Display *)RGFW_getDisplay_X11();
    if (!display || !window || !window->src.window) return;

    const unsigned char *app_id =
        (const unsigned char *)ZCL_PRESENT_APPLICATION_ID;
    int app_id_len = (int)strlen(ZCL_PRESENT_APPLICATION_ID);
    Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
    static const char *const identity_properties[] = {
        "_KDE_NET_WM_DESKTOP_FILE",
        "_GTK_APPLICATION_ID",
    };
    for (size_t i = 0; i < sizeof(identity_properties) /
                            sizeof(identity_properties[0]); i++) {
        Atom property = XInternAtom(display, identity_properties[i], False);
        (void)XChangeProperty(display, window->src.window, property, utf8, 8,
                              PropModeReplace, app_id, app_id_len);
    }
    unsigned long process_id = (unsigned long)getpid();
    Atom pid_property = XInternAtom(display, "_NET_WM_PID", False);
    Atom cardinal = XInternAtom(display, "CARDINAL", False);
    (void)XChangeProperty(display, window->src.window, pid_property, cardinal,
                          32, PropModeReplace,
                          (const unsigned char *)&process_id, 1);
    XFlush(display);
}
#endif

static bool present_scale_bitmap(const struct zcl_present_window_v1 *request,
                                 i32 target_width, i32 target_height,
                                 uint8_t **out)
{
    *out = NULL;
    if (target_width <= 0 || target_height <= 0 ||
        target_width > 4096 || target_height > 4096)
        return false;
    uint32_t channels = (uint32_t)request->pixel_format;
    uint64_t bytes = (uint64_t)(uint32_t)target_width *
                     (uint32_t)target_height * channels;
    if (bytes == 0 || bytes > SIZE_MAX) return false;
    uint8_t *pixels = malloc((size_t)bytes); // raw-alloc-ok:standalone-presentation-package
    if (!pixels) return false;
    for (uint64_t i = 0; i < bytes; i += channels) {
        pixels[i] = 0x20;
        pixels[i + 1u] = 0x20;
        pixels[i + 2u] = 0x22;
        if (channels == 4u) pixels[i + 3u] = 0xff;
    }

    uint32_t draw_width = (uint32_t)target_width;
    uint32_t draw_height = (uint32_t)((uint64_t)draw_width *
                                      request->height / request->width);
    if (draw_height > (uint32_t)target_height) {
        draw_height = (uint32_t)target_height;
        draw_width = (uint32_t)((uint64_t)draw_height *
                                request->width / request->height);
    }
    if (draw_width == 0 || draw_height == 0) {
        free(pixels);
        return false;
    }
    uint32_t x0 = ((uint32_t)target_width - draw_width) / 2u;
    uint32_t y0 = ((uint32_t)target_height - draw_height) / 2u;
    for (uint32_t y = 0; y < draw_height; y++) {
        uint32_t source_y = (uint32_t)((uint64_t)y * request->height /
                                       draw_height);
        for (uint32_t x = 0; x < draw_width; x++) {
            uint32_t source_x = (uint32_t)((uint64_t)x * request->width /
                                           draw_width);
            size_t source = ((size_t)source_y * request->width + source_x) *
                            channels;
            size_t target = ((size_t)(y0 + y) * (uint32_t)target_width +
                             x0 + x) * channels;
            memcpy(pixels + target, request->pixels + source, channels);
        }
    }
    *out = pixels;
    return true;
}

bool zcl_present_window_run_v1(
    const struct zcl_present_window_v1 *request,
    char *error, size_t error_cap)
{
    if (!zcl_present_window_validate_v1(request, error, error_cap))
        return false;
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__linux__)
    return present_error(error, error_cap,
                         "native presentation is unsupported on this platform");
#else
#if defined(__linux__)
    const char *display = getenv("DISPLAY");
    if (!display || !display[0])
        return present_error(error, error_cap,
                             "cannot open the desktop display (DISPLAY is unset)");
#endif
    const char *title = request->title && request->title[0]
        ? request->title : "ZClassic23";
    /* Keep the native window identity stable across QR, graph, Metaverse, and
     * ZCode content. Linux desktop shells match this WM_CLASS to the bundled
     * desktop entry; Win32 uses it as the grouping class. */
    RGFW_setClassName(ZCL_PRESENT_APPLICATION_ID);
    RGFW_setXInstName(ZCL_PRESENT_APPLICATION_ID);
    RGFW_windowFlags flags = (RGFW_windowFlags)(
        RGFW_windowCenter | RGFW_windowFocusOnShow);
    RGFW_window *window = RGFW_createWindow(
        title, 0, 0, (i32)request->width, (i32)request->height, flags);
    if (!window)
        return present_error(error, error_cap,
                             "native window creation failed");
    RGFW_window_setMinSize(window, 260, 300);

#if defined(__linux__)
    present_set_linux_desktop_identity(window);
#endif

    if (request->icon_rgba) {
        (void)RGFW_window_setIcon(
            window, (u8 *)(uintptr_t)request->icon_rgba,
            (i32)request->icon_width, (i32)request->icon_height,
            RGFW_formatRGBA8);
    }
    RGFW_surface *surface = RGFW_window_createSurface(
        window, (u8 *)(uintptr_t)request->pixels,
        (i32)request->width, (i32)request->height,
        request->pixel_format == ZCL_PRESENT_RGB8
            ? RGFW_formatRGB8 : RGFW_formatRGBA8);
    if (!surface) {
        RGFW_window_close(window);
        return present_error(error, error_cap,
                             "native bitmap surface creation failed");
    }

    RGFW_window_setExitKey(window, RGFW_escape);
    RGFW_window_blitSurface(window, surface);
    uint8_t *scaled_pixels = NULL;
    while (!RGFW_window_shouldClose(window)) {
        RGFW_event event;
        bool saw_event = false;
        while (RGFW_window_checkEvent(window, &event)) {
            saw_event = true;
            if (event.type == RGFW_windowResized) {
                i32 resized_width = 0;
                i32 resized_height = 0;
                (void)RGFW_window_getSize(window, &resized_width,
                                          &resized_height);
                uint8_t *replacement_pixels = NULL;
                if (present_scale_bitmap(request, resized_width,
                                         resized_height,
                                         &replacement_pixels)) {
                    RGFW_surface *replacement = RGFW_window_createSurface(
                        window, replacement_pixels, resized_width,
                        resized_height,
                        request->pixel_format == ZCL_PRESENT_RGB8
                            ? RGFW_formatRGB8 : RGFW_formatRGBA8);
                    if (replacement) {
                        RGFW_surface_free(surface);
                        free(scaled_pixels);
                        scaled_pixels = replacement_pixels;
                        surface = replacement;
                    } else {
                        free(replacement_pixels);
                    }
                }
                RGFW_window_blitSurface(window, surface);
            } else if (event.type == RGFW_windowRefresh) {
                RGFW_window_blitSurface(window, surface);
            }
            if (event.type != RGFW_keyPressed) continue;
            if (event.key.value == RGFW_q)
                RGFW_window_setShouldClose(window, RGFW_TRUE);
            if (event.key.value == RGFW_c && request->copy_text) {
                size_t copy_len = strlen(request->copy_text);
                RGFW_writeClipboard(request->copy_text, (u32)copy_len);
            }
        }
        if (!saw_event) RGFW_waitForEvent(100);
    }
    RGFW_surface_free(surface);
    free(scaled_pixels);
    RGFW_window_close(window);
    if (error && error_cap > 0) error[0] = '\0';
    return true;
#endif
}
