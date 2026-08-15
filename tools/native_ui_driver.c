/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: drive one bounded physical event into a native presentation window. */

#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned int matching_windows(Display *display, Window window,
                                     const char *title, unsigned int depth,
                                     Window *first)
{
    unsigned int matches = 0;
    char *name = NULL;
    if (XFetchName(display, window, &name) && name) {
        if (strstr(name, title) != NULL) {
            if (*first == None)
                *first = window;
            matches++;
        }
        XFree(name);
    }
    if (depth > 0) {
        Window root = None, parent = None, *children = NULL;
        unsigned int child_count = 0;
        if (XQueryTree(display, window, &root, &parent,
                       &children, &child_count)) {
            for (unsigned int i = 0; i < child_count; i++)
                matches += matching_windows(display, children[i], title,
                                            depth - 1u, first);
            if (children)
                XFree(children);
        }
    }
    return matches;
}

static void sleep_20ms(void)
{
    struct timespec duration = {.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000};
    while (nanosleep(&duration, &duration) != 0) {}
}

static bool send_close(Display *display, Window window)
{
    Atom protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    Atom close_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    if (protocols == None || close_window == None)
        return false;
    XEvent event;
    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.display = display;
    event.xclient.window = window;
    event.xclient.message_type = protocols;
    event.xclient.format = 32;
    event.xclient.data.l[0] = (long)close_window;
    event.xclient.data.l[1] = CurrentTime;
    if (!XSendEvent(display, window, False, NoEventMask, &event))
        return false;
    (void)XSync(display, False);
    return true;
}

static bool send_key(Display *display, Window root, Window window,
                     KeySym key)
{
    KeyCode keycode = XKeysymToKeycode(display, key);
    if (keycode == 0)
        return false;
    XEvent event;
    memset(&event, 0, sizeof(event));
    event.xkey.type = KeyPress;
    event.xkey.display = display;
    event.xkey.window = window;
    event.xkey.root = root;
    event.xkey.subwindow = None;
    event.xkey.time = CurrentTime;
    event.xkey.x = 1;
    event.xkey.y = 1;
    event.xkey.x_root = 1;
    event.xkey.y_root = 1;
    event.xkey.same_screen = True;
    event.xkey.keycode = keycode;
    if (!XSendEvent(display, window, False, KeyPressMask, &event))
        return false;
    event.xkey.type = KeyRelease;
    if (!XSendEvent(display, window, False, KeyReleaseMask, &event))
        return false;
    (void)XSync(display, False);
    return true;
}

static XImage *capture_window(Display *display, Window window)
{
    XWindowAttributes attributes;
    if (window == None ||
        !XGetWindowAttributes(display, window, &attributes) ||
        attributes.width <= 0 || attributes.height <= 0)
        return NULL;
    return XGetImage(display, window, 0, 0,
                     (unsigned int)attributes.width,
                     (unsigned int)attributes.height,
                     AllPlanes, ZPixmap);
}

static bool image_differs(const XImage *before, const XImage *after)
{
    if (!before || !after || before->width != after->width ||
        before->height != after->height ||
        before->bytes_per_line != after->bytes_per_line)
        return false;
    size_t bytes = (size_t)before->bytes_per_line *
                   (size_t)before->height;
    return bytes > 0 && memcmp(before->data, after->data, bytes) != 0;
}

static int usage(const char *program)
{
    fprintf(stderr,
            "usage: %s --title=<substring> "
            "(--key=1|tab|enter|left|right|up|down|pagedown|pageup|escape "
            "| --expect-count=N) "
            "[--expect-pixels-change] [--timeout-ms=N]\n",
            program);
    return 2;
}

int main(int argc, char **argv)
{
    const char *title = NULL;
    const char *key = NULL;
    bool expect_count_set = false;
    unsigned int expected_count = 0;
    unsigned int timeout_ms = 5000;
    bool expect_pixels_change = false;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--title=", 8) == 0)
            title = argv[i] + 8;
        else if (strncmp(argv[i], "--key=", 6) == 0)
            key = argv[i] + 6;
        else if (strncmp(argv[i], "--expect-count=", 15) == 0) {
            char *end = NULL;
            unsigned long parsed = strtoul(argv[i] + 15, &end, 10);
            if (!end || *end || parsed > 1024)
                return usage(argv[0]);
            expect_count_set = true;
            expected_count = (unsigned int)parsed;
        }
        else if (strncmp(argv[i], "--timeout-ms=", 13) == 0) {
            char *end = NULL;
            unsigned long parsed = strtoul(argv[i] + 13, &end, 10);
            if (!end || *end || parsed < 20 || parsed > 30000)
                return usage(argv[0]);
            timeout_ms = (unsigned int)parsed;
        } else if (strcmp(argv[i], "--expect-pixels-change") == 0) {
            expect_pixels_change = true;
        } else {
            return usage(argv[0]);
        }
    }
    if (!title || !title[0] || (!!key == expect_count_set) ||
        (expect_pixels_change && !key))
        return usage(argv[0]);
    bool first_action = key && strcmp(key, "1") == 0;
    bool tab = key && strcmp(key, "tab") == 0;
    bool enter = key && strcmp(key, "enter") == 0;
    bool page_down = key && strcmp(key, "pagedown") == 0;
    bool page_up = key && strcmp(key, "pageup") == 0;
    bool left = key && strcmp(key, "left") == 0;
    bool right = key && strcmp(key, "right") == 0;
    bool up = key && strcmp(key, "up") == 0;
    bool down = key && strcmp(key, "down") == 0;
    bool close = key && strcmp(key, "escape") == 0;
    if (key && !first_action && !tab && !enter &&
        !page_down && !page_up && !left && !right && !up && !down && !close)
        return usage(argv[0]);

    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "native_ui_driver: DISPLAY cannot be opened\n");
        return 1;
    }
    Window root = DefaultRootWindow(display);
    Window window = None;
    unsigned int count = 0;
    unsigned int attempts = timeout_ms / 20u;
    for (unsigned int i = 0; i <= attempts; i++) {
        window = None;
        count = matching_windows(display, root, title, 4u, &window);
        if ((expect_count_set && count == expected_count) ||
            (!expect_count_set && window != None))
            break;
        if (i < attempts)
            sleep_20ms();
    }
    if (expect_count_set) {
        (void)XCloseDisplay(display);
        if (count != expected_count) {
            fprintf(stderr,
                    "native_ui_driver: expected %u matching window(s), saw %u: %s\n",
                    expected_count, count, title);
            return 1;
        }
        printf("{\"schema\":\"zcl.native_ui_window_count.v1\","
               "\"title\":\"%s\",\"count\":%u,\"matched\":true}\n",
               title, count);
        return 0;
    }
    KeySym key_sym = XK_Page_Up;
    if (first_action) key_sym = XK_1;
    else if (tab) key_sym = XK_Tab;
    else if (enter) key_sym = XK_Return;
    else if (left) key_sym = XK_Left;
    else if (right) key_sym = XK_Right;
    else if (up) key_sym = XK_Up;
    else if (down) key_sym = XK_Down;
    else if (page_down) key_sym = XK_Page_Down;
    XImage *before = expect_pixels_change
        ? capture_window(display, window) : NULL;
    bool sent = window != None && (!expect_pixels_change || before) && (close
        ? send_close(display, window)
        : send_key(display, root, window, key_sym));
    bool pixels_changed = !expect_pixels_change;
    if (sent && expect_pixels_change) {
        for (unsigned int i = 0; i <= attempts; i++) {
            XImage *after = capture_window(display, window);
            pixels_changed = image_differs(before, after);
            if (after) XDestroyImage(after);
            if (pixels_changed || i == attempts) break;
            sleep_20ms();
        }
    }
    if (before) XDestroyImage(before);
    (void)XCloseDisplay(display);
    if (!sent || !pixels_changed) {
        fprintf(stderr,
                "native_ui_driver: window/key delivery failed or pixels did not change: %s\n",
                title);
        return 1;
    }
    printf("{\"schema\":\"zcl.native_ui_physical_event.v1\","
           "\"title\":\"%s\",\"key\":\"%s\",\"window\":%lu,"
           "\"delivered\":true,\"pixels_changed\":%s}\n",
           title, key, (unsigned long)window,
           expect_pixels_change ? "true" : "false");
    return 0;
}
