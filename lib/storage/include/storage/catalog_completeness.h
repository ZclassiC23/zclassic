/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * catalog_completeness — the READ-ONLY catalog reader.
 *
 * Enumerates every chain-data index/projection this process knows about,
 * reads each one's live cursor, and compares it against a caller-supplied
 * target height (H* — the reducer's provable served height,
 * reducer_frontier_provable_tip_cached()). The result is a per-index lag
 * table an operator surface can render directly: "op_return_index is 0
 * blocks behind, address_index is disabled, sprout_anchor is missing
 * 1,200,000 blocks of history".
 *
 * REPORT ONLY. This module never writes to any store it reads from (not
 * anchor_kv, not nullifier_kv, not progress.kv state, not node.db) and
 * never mutates a cursor. It is pure diagnostic composition over
 * already-shipped accessors.
 *
 * Design: a small static table of {name, get_cursor(void), always_on}
 * rows (catalog_completeness.c). Adding a new index to the catalog is one
 * row + one wrapper function — no dynamic registration API, no dependency
 * on boot order (every wrapper degrades to "unavailable" instead of
 * assuming its subsystem is already linked).
 *
 * Layering: this file lives in lib/storage/, but three of its rows are
 * backed by app/-layer accessors (op_return_index in app/models,
 * address_index in app/jobs, the explorer node.db projection in
 * app/models + app/controllers). Those symbols are reached via forward
 * declaration ONLY (see catalog_completeness.c) — never an #include of
 * controllers/models/services/views headers — so the lib/ → app/
 * direction check_lib_layering.sh enforces (HARD gate, see that script's
 * own "Fix option 2: forward declaration") stays clean.
 *
 * This lane does NOT register a diagnostics dumper or a condition; a
 * later lane wires catalog_completeness_snapshot() into
 * `zclassic23 ops state` / a typed blocker. This module is the engine,
 * not the surface. */

#ifndef STORAGE_CATALOG_COMPLETENESS_H
#define STORAGE_CATALOG_COMPLETENESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Sentinel a get_cursor() accessor returns when its index's subsystem is
 * not linked, not initialized, or explicitly disabled for this process
 * (e.g. -addressindex was not passed, or the shared progress.kv / node.db
 * singleton has not been wired yet — both routine, not error, states).
 * catalog_completeness_snapshot() turns this into a clean
 * {enabled=false, cursor=0, lag=0} row rather than a guess. Never a valid
 * real cursor value: every real cursor here is a block height (>= -1) or
 * an activation-derived proxy (>= 0), always far above INT64_MIN. */
#define CATALOG_CURSOR_UNAVAILABLE INT64_MIN

/* One row of the completeness report. */
struct catalog_index_status {
    const char *name;   /* stable row name, e.g. "op_return_index" */
    int64_t cursor;      /* this index's live cursor (0 when !enabled) */
    int64_t target;      /* the target_height passed to snapshot() (H*) */
    int64_t lag;         /* max(0, target - cursor); 0 when !enabled */
    bool always_on;      /* true = expected running on every node (a
                           * disabled always_on row is worth flagging;
                           * false = legitimately opt-in, e.g. address_index) */
    bool enabled;         /* false = subsystem not linked/initialized/opted
                           * in for this process right now */
};

/* Upper bound on registered indexes — a compile-time static table, not a
 * dynamic registry (see the header comment above). Sized generously above
 * the current row count so a future row never needs this constant bumped
 * blindly by a caller. */
#define CATALOG_COMPLETENESS_MAX_INDEXES 16

/* Fill out[0 .. min(registered_count, max)) with one row per registered
 * index, each cursor read LIVE via its accessor and compared against
 * target_height. Returns the number of rows written (0 if out is NULL or
 * max is 0 — logged, never a crash).
 *
 * Pure read: touches no store's write path, allocates nothing (out is
 * caller-owned), and is safe to call before boot completes or with a
 * process that never linked a given subsystem — every row degrades to
 * enabled=false instead of dereferencing an unready singleton. */
size_t catalog_completeness_snapshot(struct catalog_index_status *out,
                                     size_t max, int64_t target_height);

/* The largest lag among ENABLED rows in `rows[0..n)` (disabled rows carry
 * no lag signal and are skipped — an opted-out address_index is not
 * "behind"). Returns 0 when n == 0, rows is NULL, or every enabled row is
 * caught up. Every row.lag is already clamped to >= 0 by snapshot(), so
 * this is a simple max-reduce, not a re-derivation. */
int64_t catalog_completeness_worst_lag(const struct catalog_index_status *rows,
                                       size_t n);

#endif /* STORAGE_CATALOG_COMPLETENESS_H */
