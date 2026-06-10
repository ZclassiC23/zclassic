/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Diagnostics controller — the routing glue for the read-only
 * introspection RPC family used by AI agents and power-dev users.
 *
 * Focused diagnostics controller files own each read-only concern; this file
 * is the dispatch table that wires their RPC handlers into the rpc_table:
 *
 *   diagnostics_registry.c          dumpstate / zcl_state + g_dumpers[]
 *                                   + controller-level state ownership
 *   nodelog_controller.c            getnodelog
 *   dbquery_controller.c            dbquery
 *   probe_controller.c              probezclassicd
 *   mirror_status_controller.c      getmirrorstatus
 *
 * The public state-wiring + subsystem-CSV API (diagnostics_controller.h)
 * is implemented in diagnostics_registry.c, which owns g_diag. */

#include "controllers/diagnostics_controller.h"
#include "controllers/diagnostics_internal.h"

#include "rpc/server.h"

/* ── Registration ────────────────────────────────────────────────── */

void register_diagnostics_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "control", "dumpstate",     diag_rpc_dumpstate,     true },
        { "control", "getnodelog",    diag_rpc_getnodelog,    true },
        { "control", "dbquery",       diag_rpc_dbquery,       true },
        { "control", "probezclassicd", diag_rpc_probezclassicd, true },
        { "control", "getmirrorstatus", diag_rpc_getmirrorstatus, true },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_must_append(t, &cmds[i]);
}
