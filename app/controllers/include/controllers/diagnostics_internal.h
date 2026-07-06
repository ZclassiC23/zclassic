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
#include <stddef.h>

struct json_value;
struct main_state;

/* Wired controller-level state, owned by diagnostics_registry.c.
 * `diag_datadir()` returns "" until set_state() runs. */
const char *diag_datadir(void);
struct main_state *diag_main_state(void);

/* ── Fast-sync starter-pack (bundle) freshness — pure read-only helpers ──
 *
 * A fresh install seeds at the published bundle's utxo-seed-<h>.snapshot
 * height then P2P-fetches the gap to the network tip, so a bundle far below
 * the tip turns "seconds to tip" into minutes. These two helpers back the
 * `bundle_staleness` diagnostic (registered in g_dumpers) and are exported so
 * the unit test can exercise the scan + classification directly. They never
 * mint, mutate, or touch the live datadir — read-only signal only. */

/* Scan `datadir` for the highest utxo-seed-<digits>.snapshot (the same naming
 * boot_autodetect_bundle_snapshot selects; highest height wins). Returns the
 * seed height, or -1 if no matching snapshot exists. When non-NULL, *count is
 * set to the number of matching snapshots, and the winning basename is copied
 * into name[name_sz] (or "" if none). */
long long bundle_scan_seed_height(const char *datadir, int *count,
                                  char *name, size_t name_sz);

/* Freshness verdict for the published bundle relative to the network tip. */
enum bundle_freshness {
    BUNDLE_FRESH_UNKNOWN = 0, /* no bundle, or network tip not yet known */
    BUNDLE_FRESH_OK,          /* est. catch-up <= fresh threshold */
    BUNDLE_FRESH_AGING,       /* between fresh and re-mint thresholds */
    BUNDLE_FRESH_STALE,       /* est. catch-up > re-mint threshold */
};

/* Classify staleness. `seed_h` and `header_tip` are heights (>= 0), or < 0 if
 * unknown. Computes the block gap (clamped at 0 when the bundle is at/above our
 * tip) and the estimated fresh-install catch-up seconds, written to *gap_out
 * and *secs_out (each -1 when unknown). Pure arithmetic — no I/O. */
enum bundle_freshness bundle_classify(long long seed_h, long long header_tip,
                                      long long *gap_out, long long *secs_out);

/* RPC handlers, one per concern file. Signatures match rpc_handler_fn. */

/* diagnostics_registry.c */
bool diag_rpc_dumpstate(const struct json_value *params, bool help,
                        struct json_value *result);
bool diag_rpc_statecatalog(const struct json_value *params, bool help,
                           struct json_value *result);

/* The native chain-evidence dump, registered in g_dumpers. `out` must be
 * a fresh json_value; the function sets it to an object. */
bool diag_chain_evidence_dump_state_json(struct json_value *out,
                                         const char *key);
bool diag_block_index_dump_state_json(struct json_value *out,
                                      const char *key);
bool diag_header_band_dump_state_json(struct json_value *out,
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
