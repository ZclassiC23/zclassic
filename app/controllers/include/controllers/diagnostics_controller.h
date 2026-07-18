/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Diagnostics controller — hosts read-only introspection RPC methods
 * for AI agents and power-dev users:
 *
 *   dumpstate <subsystem> [key]   — generic in-process state dump,
 *                                   dispatches to <subsystem>_dump_state_json
 *   statecatalog                  — machine-readable dumpstate catalog
 *   getnodelog <pattern> ...      — reverse-scan node.log with regex/level
 *   dbquery <sql> [limit]         — SELECT-only SQLite passthrough
 *
 * These primitives let native clients inspect runtime state without one
 * dedicated command per question. */

#ifndef ZCL_DIAGNOSTICS_CONTROLLER_H
#define ZCL_DIAGNOSTICS_CONTROLLER_H

#include <stddef.h>
#include <stdbool.h>

struct rpc_table;
struct main_state;
struct json_value;

/* Wire main_state for subsystems that look up chain state (block_index
 * dumps, lastboot, etc). Call once after main_state is initialized. */
void diagnostics_controller_set_state(struct main_state *ms,
                                      const char *datadir);

void register_diagnostics_rpc_commands(struct rpc_table *t);

/* Return the machine-readable catalog of `dumpstate` subsystems. Exposed so
 * the native CLI can serve `zclassic23 statecatalog`
 * directly without depending on a running RPC node. */
bool diag_rpc_statecatalog(const struct json_value *params, bool help,
                           struct json_value *result);

/* Fill `out` with a comma-separated list of every subsystem name accepted
 * by `dumpstate`. The list is
 * derived from the live g_dumpers registry, so adding a descriptor to
 * diagnostics_dumpers.def automatically propagates here. Truncates on
 * overflow; returns the unclamped length (snprintf-style). */
int diagnostics_subsystems_csv(char *out, size_t out_sz);

#endif /* ZCL_DIAGNOSTICS_CONTROLLER_H */
