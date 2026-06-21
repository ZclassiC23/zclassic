/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Internal seam shared across the diagnostics controller family. Focused
 * controller files (registry, nodelog, dbquery, probe) share two things:
 *
 *   - the controller-level state (`main_state` + `datadir`), owned by
 *     diagnostics_registry.c and reachable here via accessors;
 *   - each file's RPC handler prototypes, so the routing table in
 *     diagnostics_controller.c can register them.
 *
 * This header is internal to app/controllers; it is not part of the
 * public diagnostics_controller.h API. */

#ifndef ZCL_DIAGNOSTICS_INTERNAL_H
#define ZCL_DIAGNOSTICS_INTERNAL_H

#include <stdbool.h>

struct json_value;
struct main_state;

/* Wired controller-level state, owned by diagnostics_registry.c.
 * `diag_datadir()` returns "" until set_state() runs. */
const char *diag_datadir(void);

/* RPC handlers, one per concern file. Signatures match rpc_handler_fn. */

/* diagnostics_registry.c */
bool diag_rpc_dumpstate(const struct json_value *params, bool help,
                        struct json_value *result);

/* The native chain-evidence dump, registered in g_dumpers. `out` must be
 * a fresh json_value; the function sets it to an object. */
bool diag_chain_evidence_dump_state_json(struct json_value *out,
                                         const char *key);

/* nodelog_controller.c */
bool diag_rpc_getnodelog(const struct json_value *params, bool help,
                         struct json_value *result);

/* dbquery_controller.c */
bool diag_rpc_dbquery(const struct json_value *params, bool help,
                      struct json_value *result);

/* probe_controller.c */
bool diag_rpc_probezclassicd(const struct json_value *params, bool help,
                             struct json_value *result);

/* getmirrorstatus remains as the legacy mirror monitor; the old per-table
 * comparison RPC surfaces are gone. */
bool diag_rpc_getmirrorstatus(const struct json_value *params, bool help,
                              struct json_value *result);

#endif /* ZCL_DIAGNOSTICS_INTERNAL_H */
