/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: drive one bounded physical event into a native presentation window. */

#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static Window find_window(Display *display, Window window,
                          const char *title, unsigned int depth)
{
    Window found = None;
    if (depth > 0) {
        Window root = None, parent = None, *children = NULL;
        unsigned int child_count = 0;
        if (XQueryTree(display, window, &root, &parent,
                       &children, &child_count)) {
            for (unsigned int i = 0; i < child_count && found == None; i++)
                found = find_window(display, children[i], title, depth - 1u);
            if (children)
                XFree(children);
        }
    }
    if (found != None)
        return found;
    char *name = NULL;
    if (XFetchName(display, window, &name) && name) {
        bool match = strstr(name, title) != NULL;
        XFree(name);
        if (match)
            return window;
    }
    return found;
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

static int usage(const char *program)
{
    fprintf(stderr,
            "usage: %s --title=<substring> "
            "--key=1|pagedown|pageup|escape [--timeout-ms=N]\n",
            program);
    return 2;
}

int main(int argc, char **argv)
{
    const char *title = NULL;
    const char *key = NULL;
    unsigned int timeout_ms = 5000;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--title=", 8) == 0)
            title = argv[i] + 8;
        else if (strncmp(argv[i], "--key=", 6) == 0)
            key = argv[i] + 6;
        else if (strncmp(argv[i], "--timeout-ms=", 13) == 0) {
            char *end = NULL;
            unsigned long parsed = strtoul(argv[i] + 13, &end, 10);
            if (!end || *end || parsed < 20 || parsed > 30000)
                return usage(argv[0]);
            timeout_ms = (unsigned int)parsed;
        } else {
            return usage(argv[0]);
        }
    }
    if (!title || !title[0] || !key)
        return usage(argv[0]);
    bool first_action = strcmp(key, "1") == 0;
    bool page_down = strcmp(key, "pagedown") == 0;
    bool page_up = strcmp(key, "pageup") == 0;
    bool close = strcmp(key, "escape") == 0;
    if (!first_action && !page_down && !page_up && !close)
        return usage(argv[0]);

    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "native_ui_driver: DISPLAY cannot be opened\n");
        return 1;
    }
    Window root = DefaultRootWindow(display);
    Window window = None;
    unsigned int attempts = timeout_ms / 20u;
    for (unsigned int i = 0; i <= attempts && window == None; i++) {
        window = find_window(display, root, title, 4u);
        if (window == None)
            sleep_20ms();
    }
    KeySym key_sym = first_action ? XK_1
        : (page_down ? XK_Page_Down : XK_Page_Up);
    bool sent = window != None && (close
        ? send_close(display, window)
        : send_key(display, root, window, key_sym));
    (void)XCloseDisplay(display);
    if (!sent) {
        fprintf(stderr, "native_ui_driver: window/key delivery failed: %s\n",
                title);
        return 1;
    }
    printf("{\"schema\":\"zcl.native_ui_physical_event.v1\","
           "\"title\":\"%s\",\"key\":\"%s\",\"window\":%lu,"
           "\"delivered\":true}\n",
           title, key, (unsigned long)window);
    return 0;
}
