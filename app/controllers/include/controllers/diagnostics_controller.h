/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Diagnostics controller — hosts read-only introspection RPC methods
 * for AI agents and power-dev users:
 *
 *   dumpstate <subsystem> [key]   — generic in-process state dump,
 *                                   dispatches to <subsystem>_dump_state_json
 *   getnodelog <pattern> ...      — reverse-scan node.log with regex/level
 *   dbquery <sql> [limit]         — SELECT-only SQLite passthrough
 *
 * These primitives let an MCP client (Claude Code) inspect runtime
 * state without one dedicated tool per question. */

#ifndef ZCL_DIAGNOSTICS_CONTROLLER_H
#define ZCL_DIAGNOSTICS_CONTROLLER_H

#include <stddef.h>

struct rpc_table;
struct main_state;

/* Wire main_state for subsystems that look up chain state (block_index
 * dumps, lastboot, etc). Call once after main_state is initialized. */
void diagnostics_controller_set_state(struct main_state *ms,
                                      const char *datadir);

/* Accessor for the wired main_state used by state dumpers and other
 * cross-controller consumers. Returns NULL until set_state() is called. */
struct main_state *diagnostics_controller_get_state(void);

void register_diagnostics_rpc_commands(struct rpc_table *t);

/* Fill `out` with a comma-separated list of every subsystem name accepted
 * by `dumpstate` (and hence by the MCP `zcl_state` tool). The list is
 * derived from the live g_dumpers registry, so adding a subsystem in
 * diagnostics_controller.c automatically propagates here. Truncates on
 * overflow; returns the unclamped length (snprintf-style). */
int diagnostics_subsystems_csv(char *out, size_t out_sz);

#endif /* ZCL_DIAGNOSTICS_CONTROLLER_H */
