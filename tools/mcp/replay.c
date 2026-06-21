/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP replay recorder implementation. */

#include "platform/time_compat.h"
#include "replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util/safe_alloc.h"

static struct mcp_replay_entry g_ring[MCP_REPLAY_RING_SIZE];
static size_t g_head = 0;    /* next write position */
static size_t g_count = 0;   /* total recorded (may exceed ring size) */

void mcp_replay_init(void)
{
    memset(g_ring, 0, sizeof(g_ring));
    g_head = 0;
    g_count = 0;
}

void mcp_replay_record(const char *tool, const char *args_json,
                        const char *response, uint64_t duration_us,
                        bool is_error)
{
    struct mcp_replay_entry *e = &g_ring[g_head % MCP_REPLAY_RING_SIZE];

    memset(e, 0, sizeof(*e));
    if (tool)
        snprintf(e->tool, sizeof(e->tool), "%s", tool);
    if (args_json)
        snprintf(e->args_json, sizeof(e->args_json), "%s", args_json);
    if (response)
        snprintf(e->response, sizeof(e->response), "%s", response);

    struct timespec ts;
    platform_time_realtime_timespec(&ts);
    e->timestamp_us = (uint64_t)ts.tv_sec * 1000000ULL +
                      (uint64_t)(ts.tv_nsec / 1000);
    e->duration_us = duration_us;
    e->is_error = is_error;
    e->used = true;

    g_head++;
    g_count++;
}

size_t mcp_replay_count(void)
{
    size_t n = g_count < MCP_REPLAY_RING_SIZE ? g_count : MCP_REPLAY_RING_SIZE;
    return n;
}

/* Escape a string for JSON output: \ and " */
static size_t json_escape(const char *src, char *dst, size_t cap)
{
    size_t pos = 0;
    for (const char *c = src; *c && pos + 6 < cap; c++) {
        switch (*c) {
        case '"':  dst[pos++] = '\\'; dst[pos++] = '"';  break;
        case '\\': dst[pos++] = '\\'; dst[pos++] = '\\'; break;
        case '\n': dst[pos++] = '\\'; dst[pos++] = 'n';  break;
        case '\r': dst[pos++] = '\\'; dst[pos++] = 'r';  break;
        case '\t': dst[pos++] = '\\'; dst[pos++] = 't';  break;
        default:
            if ((unsigned char)*c < 0x20) {
                pos += (size_t)snprintf(dst + pos, cap - pos,
                                         "\\u%04x", (unsigned)*c);
            } else {
                dst[pos++] = *c;
            }
            break;
        }
    }
    dst[pos] = '\0';
    return pos;
}

char *mcp_replay_dump(size_t count)
{
    size_t total = mcp_replay_count();
    if (count == 0 || count > total)
        count = total;

    /* Each entry can be up to ~6KB of JSON. Allocate generously. */
    size_t cap = 64 + count * (MCP_REPLAY_MAX_BODY + 2048);
    char *buf = zcl_malloc(cap, "mcp replay dump buf");
    if (!buf) return strdup("[]");

    size_t pos = 0;
    buf[pos++] = '[';

    /* Walk from oldest to newest within the requested window. */
    size_t start_offset = total - count;
    bool first = true;

    for (size_t i = 0; i < MCP_REPLAY_RING_SIZE && count > 0; i++) {
        /* Calculate ring index: oldest entry in the ring. */
        size_t ring_start;
        if (g_count <= MCP_REPLAY_RING_SIZE)
            ring_start = 0;
        else
            ring_start = g_head % MCP_REPLAY_RING_SIZE;

        size_t idx = (ring_start + i) % MCP_REPLAY_RING_SIZE;
        struct mcp_replay_entry *e = &g_ring[idx];
        if (!e->used) continue;

        if (start_offset > 0) {
            start_offset--;
            continue;
        }

        if (!first && pos + 1 < cap)
            buf[pos++] = ',';
        first = false;

        /* Escape args and response for safe JSON embedding. */
        char esc_args[2048];
        char esc_resp[MCP_REPLAY_MAX_BODY * 2];
        json_escape(e->args_json, esc_args, sizeof(esc_args));
        json_escape(e->response, esc_resp, sizeof(esc_resp));

        pos += (size_t)snprintf(buf + pos, cap - pos,
            "{\"tool\":\"%s\","
            "\"args\":\"%s\","
            "\"response\":\"%s\","
            "\"timestamp_us\":%llu,"
            "\"duration_us\":%llu,"
            "\"is_error\":%s}",
            e->tool,
            esc_args,
            esc_resp,
            (unsigned long long)e->timestamp_us,
            (unsigned long long)e->duration_us,
            e->is_error ? "true" : "false");

        count--;
    }

    if (pos + 2 < cap) {
        buf[pos++] = ']';
        buf[pos] = '\0';
    }

    return buf;
}
