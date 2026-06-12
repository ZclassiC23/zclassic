/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Block Index Loader: block_index.bin flat cache, SQLite cache, LevelDB block
 * tree compatibility, and projection-backed boot rebuild.
 *
 * Background
 * ----------
 * This service isolates block-index I/O behind a clean boundary. The flat,
 * SQLite, and LevelDB loaders remain compatibility/fallback paths; the
 * reducer-aligned boot path rebuilds from block_index_projection through
 * load_block_index_from_projection().
 *
 * On-disk flat format (block_index.bin)
 * -------------------------------------
 *   [4B magic "ZCLI" = 0x5A434C49]
 *   [4B count (LE)]
 *   [count * 192B block_index_flat entries, height-sorted]
 *
 * Each entry is a packed struct containing hash, prev_hash, height,
 * PoW metadata, chain_work, and Sapling root.
 *
 * Integrity is verified by the sibling block_index_integrity service
 * (bii_verify) using a SHA3-256 sidecar file.
 */

#ifndef ZCL_SERVICES_BLOCK_INDEX_LOADER_H
#define ZCL_SERVICES_BLOCK_INDEX_LOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util/result.h"

/* Forward declarations — avoids pulling in heavy headers */
struct main_state;
struct node_db;
struct chain_params;
struct block_tree_db;
struct block_index_projection;
struct sqlite3;

/* ── Flat file (block_index.bin) ─────────────────────────── */

/* Save all block_index entries to a height-sorted flat file.
 * Creates <datadir>/block_index.bin.  Overwrites if present. */
void save_block_index_flat(const char *datadir, struct main_state *ms);

/* Load block_index.bin via mmap.  Returns true if >= 1 entry loaded.
 * Allocates a contiguous arena for all entries, links pprev by hash,
 * recomputes nChainWork/nChainTx from pprev chain. */
bool load_block_index_flat(const char *datadir, struct main_state *ms);

/* ── SQLite cache (block_index_cache table) ──────────────── */

/* Write all block_index entries to the block_index_cache table.
 * Commits in 50K-row batches for write throughput. */
void save_block_index_recent(struct node_db *ndb, struct main_state *ms);

/* Load block_index from the block_index_cache table.
 * Returns true if >= 1 entry loaded.  Requires >= 1000 cached rows. */
bool load_block_index_sqlite(struct node_db *ndb, struct main_state *ms);

/* ── LevelDB block tree ──────────────────────────────────── */

/* Load block_index from LevelDB block tree database.
 * Post-processes: recomputes nChainWork, nChainTx, skip links,
 * branch IDs, and failed-child propagation.
 * If btdb_open is false and no entries exist, inserts genesis.
 *
 * Returns ZCL_OK on success (including the empty-but-genesis-seeded case).
 * On failure the result self-describes the cause (Law 2, "one way out"):
 *   code -1  LevelDB block-tree deserialization failed (corruption)
 *   code -2  out of memory allocating the post-load sort array (io/resource)
 *   code -3  csr rejected the genesis tip commit (integrity)
 * The previous bare-bool true/false maps to .ok true/false. */
struct zcl_result load_block_index(struct main_state *ms,
                       const struct chain_params *params,
                       struct block_tree_db *btdb, bool btdb_open);

/* ── Shared internal helpers ─────────────────────────────────────── */

struct block_index;

/* Forward pass over a height-sorted block_index array: recompute
 * nChainWork, nChainTx, skip links, cached branch id, and failed-child
 * propagation from each entry's (already-linked) pprev. Shared by the
 * LevelDB loader (load_block_index) and the projection rebuild so both compute
 * the pointer-graph-derived fields identically. `sorted` must be height-ASC
 * ordered. */
void block_index_forward_pass(struct block_index **sorted, size_t count);

/* ── Projection-backed boot rebuild (event_log -> projection -> map) ───── */

/* Rebuild the in-memory block_index map purely from the
 * block_index_projection (the log-derived authoritative source), then
 * seed the active tip from the tip_finalize cursor in progress.kv.
 *
 * This is the reducer-aligned boot rebuild. It does NOT touch any
 * $HOME/.zclassic path.
 *
 * Sequence:
 *   1. block_index_projection_catch_up(bip) — drain the event log.
 *   2. block_index_projection_iterate — fold every disk_block_index into
 *      ms->map_block_index via chainstate_insert_block_index (fields per
 *      block_index_db.c, OMITTING the +1703 file-0 fixup since the
 *      projection's nDataPos is this node's own body_persist position).
 *   3. Link pprev via the carried hashPrev.
 *   4. Forward pass: recompute nChainWork, nChainTx, skip links, branch
 *      ids, failed-child propagation (mirrors load_block_index post-load).
 *   5. Seed the tip: read the tip_finalize cursor from `progress_db`
 *      (stage_cursor table), look up the finalized tip hash at cursor-1,
 *      find that block_index, set the authoritative tip + publish via
 *      chain_set_active_tip.
 *
 * Empty projection (cold datadir) → no entries folded, no tip set,
 * returns true (the node sits at genesis; fast_sync seeds it).
 *
 * `bip` is the open projection; `progress_db` is the progress.kv handle
 * (progress_store_db()). Both may be NULL — a NULL `bip` makes this a
 * no-op (returns true, empty map); a NULL `progress_db` skips the tip
 * seed (map rebuilt, no tip published).
 *
 * Returns true on success (including the empty case), false on a hard
 * fold/iterate error. */
bool load_block_index_from_projection(struct main_state *ms,
                                      const struct chain_params *params,
                                      struct block_index_projection *bip,
                                      struct sqlite3 *progress_db);

/* FORWARD-ONLY finalized-tip seed for the NORMAL boot path.
 *
 * After the normal loaders establish the active tip from the coins/UTXO
 * authority, this adopts the durable tip_finalize frontier (cursor-1) ONLY
 * when it is a strictly-higher, CONTIGUOUS forward extension of the current
 * chain — every intermediate block HAVE_DATA + script-valid + failure-free,
 * with the pprev walk landing pointer-equal on the current active tip.
 * Otherwise it is a no-op. It never rewinds the tip, never swaps a fork, and
 * never mutates the tip_finalize_log or any cursor (read only), so a
 * sparse/header-only frontier yields a no-op rather than a hole.
 *
 * Returns 1 = seeded forward, 0 = no-op, -1 = error.
 *
 * WIRED at config/src/boot_services.c right after staged_sync_supervisor_register
 * (which runs tip_finalize_stage_init → registers the chain-height authority and
 * seeds it from the coins-restore tip). That is the race-free window: the
 * authority is live (so active_chain_height reads the real coins tip, not the
 * genesis value seen earlier in boot) but the runtime services / reducer ingest
 * have not started yet. Safe to call anywhere: it no-ops unless the finalized
 * frontier is a small, strictly-higher, contiguous have-data + script-valid
 * extension landing on the current tip. */
int block_index_loader_seed_tip_from_finalized(struct main_state *ms,
                                               struct sqlite3 *progress_db);

/* DURABLE, crash-recoverable cold-import staged-sync seed.
 *
 * A cold LDB import (utxo_recovery_import_ldb, ldb_import_found branch) seeds
 * the coins set + CSR-commits the public tip to H but NEVER seeds the 8 staged
 * reducer stage logs, so reducer_frontier_compute_hstar stays at the compiled
 * checkpoint and the staged-sync reducer cannot advance past the import floor. The
 * import path writes a DURABLE anchor (cold_import_seed_anchor_height/hash) to
 * node_db state; this consumer re-derives the wedge EVERY boot from that
 * durable marker plus the live frontier, so a kill-9 mid-seed self-heals on the
 * next boot.
 *
 * Fires the TRUSTED seed (tip_finalize_stage_seed_anchor(H, hash, true) — the
 * same fast-sync trust model snapshot_apply.c uses) ONLY when ALL hold:
 *   (1) the durable anchor (H, hash) is present (a real ldb_import_found ran),
 *   (2) the live active tip is exactly that (H, hash) and H >= the SHA3
 *       checkpoint anchor (partial/rewound import is excluded),
 *   (3) reducer_frontier_compute_hstar(progress_db) is far below H (the
 *       genuinely-pinned signal — NOT the tip_finalize cursor, which the boot
 *       reconcile clamp force-stamps to H+1 on every cold-import boot).
 * It sets coins_applied_height = H+1 first (the utxo_apply cursor convention),
 * then seeds. Forward-only, deletes no rows, idempotent (the gate self-clears
 * once H* == H). Returns 1 = seeded, 0 = no-op, -1 = error.
 *
 * WIRED right after block_index_loader_seed_tip_from_finalized in boot_services.c. */
int block_index_loader_seed_stages_from_cold_import(struct main_state *ms,
                                                    struct node_db *ndb,
                                                    struct sqlite3 *progress_db);

/* Shared projection-rebuild front door for boot (see the .c for contract).
 * Folds block_index_projection into ms's map, accepts iff the folded map has
 * > min_entries nodes. `publish_tip`=true publishes the cursor tip (the
 * projection-as-authority -rebuildfromlog path); =false does a PURE map
 * rebuild with NO tip publish (the kill-9 fallback — the coins authority +
 * the guarded forward seed own the tip). Returns true on accept. */
bool boot_try_rebuild_block_index_from_projection(struct main_state *ms,
                                                  const struct chain_params *params,
                                                  size_t min_entries,
                                                  bool publish_tip);

/* Projection top-up for the NORMAL boot path (defect #10, task #29).
 *
 * The flat/LevelDB loaders only know the last boot-time save; every block
 * connected since then exists durably ONLY in the event log → projection.
 * This folds the projection over the loaded map RAISE-ONLY: applies
 * HAVE_DATA + nFile/nDataPos an entry lacks, raises nTx and the
 * BLOCK_VALID level, inserts entries the loaders never saw (with pprev
 * link + bounded chainwork), and recovers nTx from the block file for
 * legacy rows emitted with n_tx=0 (hash-bound read-back). It never
 * lowers a field, never copies FAILED bits, and refuses rows whose
 * height disagrees with the loaded entry (logged, counted).
 *
 * Call AFTER the legacy loaders and BEFORE boot's nChainTx propagation
 * pass, which turns the topped-up nTx into the connected nChainTx chain
 * the boot scan and active-chain rebuild key on. NULL projection → no-op
 * true. Returns false only on a hard projection iterate/OOM error. */
bool block_index_projection_topup_with(struct block_index_projection *bip,
                                       struct main_state *ms,
                                       const char *datadir);
bool block_index_projection_topup(struct main_state *ms,
                                  const char *datadir);

#endif /* ZCL_SERVICES_BLOCK_INDEX_LOADER_H */
