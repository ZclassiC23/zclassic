/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP replay recorder — ring buffer of last 100 request/response pairs.
 * Hooks into mcp_router_dispatch for zero-config recording.
 *
 * zcl_replay_dump: dump the ring buffer as a JSON array.
 * zcl_replay_exec: re-execute a previously recorded request. */

#ifndef ZCL_MCP_REPLAY_H
#define ZCL_MCP_REPLAY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define MCP_REPLAY_RING_SIZE 100
#define MCP_REPLAY_MAX_BODY  4096   /* truncated beyond this */

struct mcp_replay_entry {
    char tool[64];
    char args_json[1024];
    char response[MCP_REPLAY_MAX_BODY];
    uint64_t timestamp_us;
    uint64_t duration_us;
    bool is_error;
    bool used;
};

/* Initialize the replay ring (idempotent). */
void mcp_replay_init(void);

/* Record a request/response pair. Called from mcp_router_dispatch hook. */
void mcp_replay_record(const char *tool, const char *args_json,
                        const char *response, uint64_t duration_us,
                        bool is_error);

/* Dump all recorded entries as a JSON array string (malloc'd, caller frees).
 * Entries are ordered oldest-to-newest.  If count > 0, return at most count
 * entries from the end.  count == 0 means all. */
char *mcp_replay_dump(size_t count);

/* Return the number of recorded entries. */
size_t mcp_replay_count(void);

/* Clear the ring buffer. */
void mcp_replay_clear(void);

#endif /* ZCL_MCP_REPLAY_H */
