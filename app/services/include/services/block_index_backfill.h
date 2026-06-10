/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Block Index Backfill — one-time historical-chain seed for the
 * block_index_projection durable store.
 *
 * Why
 * ---
 * block_index_projection (lib/storage) materializes the chain index from
 * EV_BLOCK_HEADER events in the append-only event_log. Before the historical
 * backfill it only saw the tail of the chain — every block this node
 * connected since the projection was wired emits an event, but the
 * millions of historical headers the legacy loaders imported from
 * zclassicd's LevelDB were never replayed into the log. The projection is
 * therefore a partial mirror, not yet a complete durable store.
 *
 * This service closes that gap: it walks the canonical active chain
 * (the most-work chain the boot loaders already linked into
 * ms->map_block_index + ms->chain_active) from genesis to tip and emits
 * one EV_BLOCK_HEADER per block, which block_index_projection_catch_up
 * folds idempotently (INSERT OR REPLACE on hash). After it runs once the
 * projection holds the entire canonical history and can stand alone.
 *
 * Canonical-order source
 * ----------------------
 * NOT raw LevelDB iteration — that carries stale forks at shared heights.
 * By boot time the legacy loaders have built a fully-linked
 * map_block_index whose active_chain tip is the most-work chain. We walk
 * active_chain_tip(&ms->chain_active) back via ->pprev into height-ASC
 * order and emit each, so only the canonical lineage is seeded.
 *
 * nSolution correctness
 * ---------------------
 * The in-memory `struct block_index` does NOT retain nSolution (every
 * loader — flat, SQLite, and LevelDB — sets it NULL to save RAM). The
 * emitted EV_BLOCK_HEADER must carry the Equihash solution so the header
 * can be PoW-re-verified later, so for any block whose in-memory entry
 * lacks a solution we read it back from the legacy LevelDB
 * disk_block_index (block_tree_db_read_block_index) per canonical hash
 * just before emitting.
 *
 * One-time gating
 * ---------------
 * Gated SOLELY on the durable projection_meta flag
 * "block_index_backfilled" (block_index_projection_backfill_done /
 * _mark_backfilled). NEVER on a row count — the live projection already
 * carries tail rows, so a count guard would brick a never-backfilled
 * store. A second call after the flag is set is a no-op. This is NOT a
 * mandatory live-boot step; call it from the -rebuildfromlog opt-in path
 * or an explicit invocation. The default live boot is unchanged.
 */

#ifndef ZCL_SERVICES_BLOCK_INDEX_BACKFILL_H
#define ZCL_SERVICES_BLOCK_INDEX_BACKFILL_H

#include "util/result.h"

#include <stdbool.h>
#include <stdint.h>

struct main_state;
struct block_index_projection;
struct block_tree_db;

/* Result of a backfill attempt. */
struct block_index_backfill_result {
    bool     ran;          /* true if this call performed the backfill */
    bool     already_done; /* true if the durable flag was already set */
    uint64_t emitted;      /* EV_BLOCK_HEADER events appended this call */
    uint64_t walked;       /* canonical blocks walked (tip..genesis) */
    uint64_t solution_disk_reads; /* headers whose nSolution came from LDB */
};

/* Walk the canonical active chain and emit one EV_BLOCK_HEADER per block
 * into the shared event_log (via block_index_emit_header_event, which
 * uses event_log_singleton()), then drain into the projection and set the
 * one-time durable flag.
 *
 * `ms`    — boot-linked chain state; the walk source is
 *           active_chain_tip(&ms->chain_active) back via ->pprev.
 * `bip`   — the open block_index_projection (gates + folds). If NULL or
 *           the flag is already set, this is a no-op (returns true).
 * `btdb`  — legacy LevelDB block tree, the nSolution fallback source for
 *           in-memory entries that don't retain a solution. May be NULL
 *           (then headers whose in-mem entry lacks a solution emit with an
 *           empty solution — acceptable only for synthetic chains that
 *           carry nSolution in RAM; the live path passes g_active_block_tree).
 * `out`   — optional; filled with per-call counters. May be NULL.
 *
 * Returns ZCL_OK on success (including the already-done / unwired no-ops),
 * or a non-ok zcl_result carrying the failure reason on a hard error
 * (emit/fold/mark failure). On a hard error the durable flag is NOT set,
 * so a later call retries (the fold is idempotent). */
struct zcl_result block_index_backfill_canonical_chain(
        struct main_state *ms,
        struct block_index_projection *bip,
        struct block_tree_db *btdb,
        struct block_index_backfill_result *out);

#endif /* ZCL_SERVICES_BLOCK_INDEX_BACKFILL_H */
