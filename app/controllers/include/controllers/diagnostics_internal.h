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

typedef bool (*diagnostics_dump_fn)(struct json_value *out, const char *key);

struct diagnostics_dump_entry {
    const char *name;
    diagnostics_dump_fn fn;
    const char *desc;
};

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

/* The "bundle_staleness" g_dumpers[] entry itself; implementation lives in
 * diagnostics_registry_bundle.c alongside the two helpers above. */
bool bundle_staleness_dump_state_json(struct json_value *out,
                                      const char *key);

/* RPC handlers, one per concern file. Signatures match rpc_handler_fn. */

/* diagnostics_registry.c / diagnostics_dispatch.c.
 *
 * diag_rpc_dumpstate is a hot-swap TRAMPOLINE (resident, in
 * diagnostics_dispatch.c): it acquire-loads an atomic provider and delegates
 * to it when a dev generation .so has installed one, else calls the resident
 * built-in below (which owns g_dumpers[], in the swap-eligible
 * diagnostics_registry.c). See docs/work/HOTSWAP.md. */
bool diag_rpc_dumpstate(const struct json_value *params, bool help,
                        struct json_value *result);
bool diag_rpc_dumpstate_builtin(const struct json_value *params, bool help,
                                struct json_value *result);

/* Matches diag_rpc_dumpstate_builtin — the swap unit for `dumpstate`. */
typedef bool (*diag_dumpstate_fn)(const struct json_value *params, bool help,
                                  struct json_value *result);

#ifdef ZCL_DEV_BUILD
/* DEV-ONLY: atomically re-point the resident `dumpstate` provider at `fn`
 * (release store; the trampoline reads it with an acquire load). Resident in
 * diagnostics_dispatch.c so a generation .so reaches it as an undefined symbol
 * bound to the executable's copy. Returns false on a NULL fn. */
bool diag_dumpstate_replace(diag_dumpstate_fn fn);
#endif
bool diag_rpc_statecatalog(const struct json_value *params, bool help,
                           struct json_value *result);
size_t diagnostics_dumper_count(void);
const struct diagnostics_dump_entry *diagnostics_dumper_at(size_t idx);
const char *diagnostics_oracle_owner_file(void);

/* The native chain-evidence dump, registered in g_dumpers. `out` must be
 * a fresh json_value; the function sets it to an object. */
bool diag_chain_evidence_dump_state_json(struct json_value *out,
                                         const char *key);
bool diag_block_index_dump_state_json(struct json_value *out,
                                      const char *key);
bool diag_header_band_dump_state_json(struct json_value *out,
                                      const char *key);
bool sapling_checkpoint_dump_state_json(struct json_value *out,
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

/* selfbacktrace — dump a live backtrace for every registered thread of the
 * RUNNING node into <datadir>/backtrace-<ts>.log and return { path,
 * thread_count }. Backs the ops.debug.backtrace native command. */
bool diag_rpc_selfbacktrace(const struct json_value *params, bool help,
                            struct json_value *result);

/* diagnostics_health_rollup.c — unhealthy-only rollup, registered in
 * g_dumpers as subsystem "unhealthy". Walks every OTHER dumper's `_health`
 * key (see the file header for the { ok, reason } convention) and reports
 * only the ones with ok == false. */
bool unhealthy_dump_state_json(struct json_value *out, const char *key);

#endif /* ZCL_DIAGNOSTICS_INTERNAL_H */
