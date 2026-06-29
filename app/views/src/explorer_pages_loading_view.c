/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Explorer pages loading VIEW: the auto-refreshing "Loading Token Data..."
 * placeholder served by the tokens page while its background cache warms. */

#include "views/explorer_pages_loading_view.h"
#include "controllers/explorer_internal.h"

#include <stddef.h>
#include <stdint.h>

size_t explorer_view_tokens_loading(uint8_t *r, size_t max)
{
    size_t off = 0;
    APPEND(off, r, max,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta http-equiv='refresh' content='3'>"
        "<link rel='stylesheet' href='/explorer/style.css'>"
        "</head><body>" EXPLORER_NAV
        "<div style='text-align:center;margin:80px 0'>"
        "<h1 style='font-size:32px;color:#ff99ff'>Loading Token Data...</h1>"
        "<p style='font-size:18px;color:#888'>Scanning SQLite for ZSLP tokens.</p>"
        "</div>" EXPLORER_FOOTER);
    return off;
}
