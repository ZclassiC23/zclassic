/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * coins_kv — the reducer's canonical UTXO set as a `coins` table IN progress.kv.
 *
 * WHY (docs/work/tip-durability-collapse.md): the live coins set used to live in
 * a SEPARATE WAL database (utxo_projection.db), folded from an out-of-txn
 * event_log, while the stage cursor + inverse-delta + utxo_apply_log row commit
 * in progress.kv. Two WAL databases have NO atomic cross-commit (WAL has no
 * master journal) — a crash drifted the coins from the cursor and no forward
 * path could realign them (the entire tip-wedge class). Storing the coins HERE,
 * on the progress.kv handle, makes every mutation commit inside the SAME
 * stage_run_once BEGIN IMMEDIATE as the cursor: every effect of a block lands or
 * rolls back as one atomic unit. Mirrors the proven created_outputs_index.
 *
 * Every function operates on the passed progress.kv handle and therefore
 * participates in whatever transaction the caller already holds open. Schema +
 * serialisation mirror utxo_projection's `utxo` table column-for-column so the
 * SHA3 UTXO commitment matches utxo_projection's.
 *
 * Callers may invoke from any thread: every function prepares and finalizes
 * its statement within the call (no shared statement state). See coins_kv.c
 * for why a cached statement is forbidden on this cross-thread shared handle.
 */
#ifndef STORAGE_COINS_KV_H
#define STORAGE_COINS_KV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct sqlite3;
struct coins;
struct utxo_projection;

/* CREATE TABLE IF NOT EXISTS coins(...). Idempotent. */
bool coins_kv_ensure_schema(struct sqlite3 *db);

/* INSERT OR REPLACE one output. `script` may be NULL iff `script_len`==0. */
bool coins_kv_add(struct sqlite3 *db, const uint8_t txid[32], uint32_t vout,
                  int64_t value, int32_t height, bool is_coinbase,
                  const uint8_t *script, size_t script_len);

/* DELETE one output (spend). A missing row is not an error. */
bool coins_kv_spend(struct sqlite3 *db, const uint8_t txid[32], uint32_t vout);

/* ── Batched per-block apply (utxo_apply_stage.c apply_coins_kv hot loop) ──
 *
 * coins_kv_add / coins_kv_spend each prepare+step+finalize their own statement
 * per row. A block with thousands of outputs/inputs pays that prepare/finalize
 * cost thousands of times. These _many variants prepare ONE statement (a
 * STACK-LOCAL stmt — NOT a module-static cached statement; the coins_kv.c
 * statement-lifetime prohibition is specifically about cached statics shared
 * across threads, which these are not) and reset+bind+step it for every row in
 * the SAME (txid,vout) order the per-row helpers would have used. The emitted
 * SQL is byte-identical to coins_kv_add / coins_kv_spend, so the resulting
 * coins set — and therefore coins_kv_commitment — is bit-identical. The
 * stack-local statement is finalized before the call returns; nothing escapes.
 *
 * Caller MUST preserve the intra-block invariant (adds BEFORE spends) by
 * invoking coins_kv_add_many for the whole added[] array first, then
 * coins_kv_spend_many for the whole spent[] array — exactly as the per-row
 * loop did. `txids`/`scripts` buffers must outlive the call (SQLITE_STATIC
 * binds); they do (the caller owns the parsed block until after this returns).
 *
 * Returns false on the first row whose step fails (the partial writes roll back
 * with the caller's enclosing transaction); the index of any failing row is
 * logged. count==0 is a clean true no-op. */
struct coins_kv_add_row {
    const uint8_t *txid;        /* 32 bytes */
    uint32_t       vout;
    int64_t        value;
    int32_t        height;
    bool           is_coinbase;
    const uint8_t *script;      /* may be NULL iff script_len==0 */
    size_t         script_len;
};
struct coins_kv_spend_row {
    const uint8_t *txid;        /* 32 bytes */
    uint32_t       vout;
};
bool coins_kv_add_many(struct sqlite3 *db, const struct coins_kv_add_row *rows,
                       size_t count);
bool coins_kv_spend_many(struct sqlite3 *db,
                         const struct coins_kv_spend_row *rows, size_t count);

/* True iff (txid,vout) is currently live (unspent). */
bool coins_kv_exists(struct sqlite3 *db, const uint8_t txid[32], uint32_t vout);

/* Point-read one live output (txid,vout). Returns true iff the output is
 * currently live (unspent), filling value/script via the non-NULL out-pointers.
 * If `script_cap` is smaller than the stored script length the script is
 * truncated to `script_cap` bytes, but `*script_len_out` always reports the
 * true length. Mirrors utxo_projection_get exactly so script_validate's prevout
 * resolver returns the same result against either store. A spent or absent
 * output returns false. */
bool coins_kv_get(struct sqlite3 *db, const uint8_t txid[32], uint32_t vout,
                  int64_t *value_out, uint8_t *script_out, size_t script_cap,
                  size_t *script_len_out);

/* Count of live outputs. Returns -1 on error. */
int64_t coins_kv_count(struct sqlite3 *db);

/* ── RAW (overlay-bypassing) accessors ──────────────────────────────────
 *
 * The in-RAM bulk-fold overlay (storage/coins_ram.h) fronts coins_kv_get /
 * coins_kv_exists / coins_kv_get_coins / coins_kv_count when active. Those
 * shims must read THROUGH to the durable SQLite set on a cold miss WITHOUT
 * re-entering the overlay (infinite recursion). These _sqlite variants are the
 * raw SQLite implementations, called by the overlay's read-through path and by
 * the public shims when the overlay is inactive. App code should keep calling
 * the public coins_kv_* functions. */
bool    coins_kv_get_sqlite(struct sqlite3 *db, const uint8_t txid[32],
                            uint32_t vout, int64_t *value_out,
                            uint8_t *script_out, size_t script_cap,
                            size_t *script_len_out);
bool    coins_kv_exists_sqlite(struct sqlite3 *db, const uint8_t txid[32],
                               uint32_t vout);
bool    coins_kv_get_coins_sqlite(struct sqlite3 *db, const uint8_t txid[32],
                                  struct coins *out);
int64_t coins_kv_count_sqlite(struct sqlite3 *db);
bool    coins_kv_setinfo_sqlite(struct sqlite3 *db, int64_t *num_txs,
                                int64_t *num_txouts, int64_t *total_amount);
/* RAW batched write variants — the overlay's flush drains into the durable
 * SQLite set THROUGH these (the public _many shims would re-enter the overlay). */
bool    coins_kv_add_many_sqlite(struct sqlite3 *db,
                                 const struct coins_kv_add_row *rows,
                                 size_t count);
bool    coins_kv_spend_many_sqlite(struct sqlite3 *db,
                                   const struct coins_kv_spend_row *rows,
                                   size_t count);

/* Reconstruct a `struct coins` for `txid` from live rows (`out` is coins_init'd
 * by this call). Returns false (num_vout==0) if the txid has no live outputs.
 * Same two-pass shape as utxo_projection_get_coins / coins_view_sqlite. */
bool coins_kv_get_coins(struct sqlite3 *db, const uint8_t txid[32],
                        struct coins *out);

/* gettxoutsetinfo aggregate: distinct txids, total outputs, summed value.
 * Mirrors utxo_projection_setinfo exactly. Returns false on error. */
bool coins_kv_setinfo(struct sqlite3 *db, int64_t *num_txs,
                      int64_t *num_txouts, int64_t *total_amount);

/* SHA3-256 UTXO commitment over the coins set in canonical (txid,vout) order.
 * The serialisation equals utxo_projection_commitment's (the read-flip
 * relies on this matching the oracle gettxoutsetinfo commitment). Returns 0 on
 * success, -1 on error. */
int coins_kv_commitment(struct sqlite3 *db, uint8_t out[32]);

/* ── Per-boundary UTXO root table (the keystone's reproducibility anchor) ──
 *
 * The MMB leaf carries a per-height utxo_root, but ONLY at boundary heights
 * (height % MMR_COMMITMENT_INTERVAL == 0). Three independent paths build the
 * same leaf — the live connect path (tip_finalize_post_step), the catch-up
 * path (rpc_blockchain_mmb_catchup), and the leaf-store rebuild
 * (mmb_leaf_store_rebuild) — and all three MUST stamp the IDENTICAL boundary
 * utxo_root or their leaf hashes diverge and every prior FlyClient proof
 * breaks. The leaf store persists only 32-byte leaf HASHES, so the boundary
 * root is not recoverable from it. The live connect path computes
 * coins_kv_commitment ONCE at each boundary and records it here; catch-up and
 * rebuild READ it back, so no path has to re-fold the historical UTXO set.
 *
 * Keyed per height under "mmb_utxo_root:<height>" in progress_meta (opaque
 * 32-byte blob). _set writes inside the caller's own implicit txn (own-BEGIN);
 * _get returns false in *found when the boundary root has not been recorded
 * yet, in which case the caller uses the zero sentinel (the leaf hash is then
 * the pre-keystone value for that height and is back-filled on the next pass).
 */
bool coins_kv_boundary_root_set(struct sqlite3 *db, int32_t height,
                                const uint8_t utxo_root[32]);
bool coins_kv_boundary_root_get(struct sqlite3 *db, int32_t height,
                                uint8_t out_utxo_root[32], bool *found);

/* One-shot idempotent boot migration: if coins_kv is empty AND the projection
 * holds the live set, bulk-copy the projection's UTXOs into coins_kv (atomic)
 * so the read-flip has data on existing / snapshot-seeded datadirs. No-op (and
 * NOT marked done) when the projection is still empty, so a pre-snapshot-seed
 * call safely retries after the seed. Non-fatal: returns false on error and the
 * next boot retries. Safe to call before progress_store is open (returns true,
 * no-op). */
bool coins_kv_boot_rebuild_if_needed(struct sqlite3 *progress_db,
                                     struct utxo_projection *proj);

/* Seed coins_kv from node.db's `utxos` table right after a cold
 * LevelDB import (the projection-based boot rebuild sees an empty
 * projection on that boot and copies nothing). Idempotent via the
 * same migration stamp; no-op when coins_kv already has rows. */
bool coins_kv_seed_from_node_db(struct sqlite3 *progress_db,
                                const char *node_db_path);

/* ── coins_applied_height — the canonical contiguous applied-frontier ──
 *
 * A single progress_meta key recording the contiguous applied frontier of the
 * coins_kv UTXO set: it ALWAYS equals the utxo_apply stage cursor by
 * construction (see docs/work/sync-organism-map.md). The frontier is
 * co-committed inside the SAME transaction as every WRITE that moves the
 * utxo_apply cursor — never lagging it — unlike MAX(coins.height), which is only
 * the most-recent surviving coin's creation height and cannot see an interior
 * hole. The complete set of utxo_apply cursor writers, each co-writing this
 * frontier in its own transaction:
 *   (a) forward apply (utxo_apply_stage.c step_apply): successful applies only
 *       write frontier = cursor+1. Failed verdicts return JOB_BLOCKED with the
 *       cursor/frontier unchanged, so a later height cannot apply over a hole;
 *   (b) reorg unwind (utxo_apply_delta_reorg.c): pulls the frontier BACK to
 *       fork+1 (a PLAIN set — the decrease must not be blocked);
 *   (c) the poison_rewind cursor-mover (stage_repair_rewind.c
 *       stage_repair_header_solution_poison_rewind): forces the utxo_apply cursor
 *       DOWN to the frontier height and co-writes frontier = height. Its ok=1
 *       guard (success_checked_logs includes utxo_apply_log) proves no coins were
 *       mutated at/above that height, so frontier == coins is exact.
 * The ONE non-co-writing cursor-touching path is the one-shot snapshot bulk seed
 * coins_kv_boot_rebuild_if_needed: it bulk-copies the projection's UTXOs but does
 * NOT write this frontier directly — it relies on the snapshot anchor stamping
 * the utxo_apply cursor + the next-boot if-absent backfill below to seed the
 * frontier from that cursor, with a transient ABSENT="unknown" window in between
 * (a clean unknown, never 0-as-applied). This gives the self-heal a single
 * non-divergent coins-frontier input with a stage-name-independent name. The
 * value is stored as a stable little-endian int64 blob under this key. */
#define COINS_APPLIED_HEIGHT_KEY "coins_applied_height"

/* Write the applied frontier inside the caller's ALREADY-OPEN transaction
 * (forward = stage_run_once's BEGIN IMMEDIATE; reorg = the unwind's own BEGIN
 * IMMEDIATE; poison_rewind = the rewind's own BEGIN IMMEDIATE). Wraps
 * progress_meta_set_in_tx — NO inner BEGIN/COMMIT — so the height commits or
 * rolls back as ONE unit with the coin mutation + cursor. This is a PLAIN set
 * (allows decrease): both the reorg path (frontier BACK to fork+1) and the
 * poison_rewind path (frontier DOWN to height) legitimately decrease it, so a
 * monotonic-floor helper must NEVER be used here.
 * Returns false on failure so the caller can ROLLBACK. */
bool coins_kv_set_applied_height_in_tx(struct sqlite3 *db, int32_t height);

/* Read the applied frontier. *found is false (a clean "unknown") when the row
 * is absent — a fresh / un-synced datadir reports ABSENT, never 0-as-applied.
 * Returns false only on a hard read error (db NULL / malformed blob).
 *
 * THE coins-best derivation lives at
 * app/jobs/include/jobs/reducer_frontier.h reducer_frontier_derive_coins_best
 * (coins-best height = this value - 1; hash from the durable stage logs).
 * The node_state 'coins_best_block' key is a CACHE of that derivation. */
bool coins_kv_get_applied_height(struct sqlite3 *db, int32_t *out, bool *found);

/* One-shot migration stamp: set when coins_kv provably holds the live coin
 * set (fresh-sync forward population, projection bulk-copy, or
 * coins_kv_seed_from_node_db). Written by coins_kv_boot_rebuild.c / the
 * seed path; read by the proven-authority predicate below. */
#define COINS_KV_MIGRATION_COMPLETE_KEY "coins_kv_migration_complete"

/* THE proven-authority predicate — true iff coins_kv is the CANONICAL coin
 * store on this datadir (the wave-2 "canonical datadir" signal; the same
 * three rungs the L1 torn-anchor heal requires):
 *   (1) coins_applied_height present (durable applied frontier);
 *   (2) COINS_KV_MIGRATION_COMPLETE_KEY == 1 (the store provably holds the
 *       live set — a cursor-backfilled frontier alone is NOT proof);
 *   (3) coins_kv_count > 0.
 * On true, *out_applied (nullable) receives the applied frontier. Read
 * errors return false — degrading to the STRICTER legacy gates, never the
 * permissive derived path. SELECT-only. */
bool coins_kv_is_proven_authority(struct sqlite3 *db, int32_t *out_applied);

/* One-time idempotent boot backfill for existing datadirs that predate this
 * key: if coins_applied_height is ABSENT, seed it (in its OWN BEGIN IMMEDIATE —
 * the one allowed standalone txn, since it backfills a derived value that
 * already equals the durable cursor with NO coin mutation) from the trusted
 * utxo_apply stage cursor. NEVER seeds from MAX(coins.height). No-op once the
 * key exists. Returns false on error (next boot retries). */
bool coins_kv_backfill_applied_height_if_absent(struct sqlite3 *db);

/* Truncate coins_kv + clear the migration stamp + drop coins_applied_height so
 * a subsequent coins_kv_seed_from_node_db performs a FRESH copy. The reindex
 * epilogue (app/services/src/reindex_epilogue.c) calls this right after a full
 * from-genesis replay: the replayed node.db `utxos` mirror is the authority and
 * coins_kv is being rebuilt from it in the SAME boot, so the pre-reindex set
 * (which may carry rows the replay legitimately deleted) must be discarded
 * first — coins_kv_seed_from_node_db short-circuits when the migration stamp is
 * already set, so without clearing it the reseed would no-op over a stale set.
 *
 * ONE BEGIN IMMEDIATE: DELETE FROM coins; clear COINS_KV_MIGRATION_COMPLETE_KEY
 * + COINS_APPLIED_HEIGHT_KEY. The single legal standalone txn for a reindex
 * epilogue (boot single-writer, no stage threads running yet — same
 * justification as boot_index_clear_coins_state). Idempotent (a fresh store is
 * a no-op delete + absent-key clears). Returns false on error so the caller
 * aborts the epilogue and leaves the reindex sentinel pending for a retry. */
bool coins_kv_reset_for_reseed(struct sqlite3 *db);

/* ── ANCHOR-SET MINT writer (lib/storage/src/coins_kv_snapshot_write.c) ──
 *
 * Stream the LIVE coins_kv set to a UTXO snapshot sidecar in the exact
 * on-disk format the loader (lib/chain/src/utxo_snapshot_loader.c) reads and
 * the `--gen-utxo-snapshot` tool writes:
 *
 *   Header (104 bytes, little-endian): magic="ZCLUTXO\0", version u32=1,
 *     reserved u32, height u32, reserved u32, count u64, total_supply i64,
 *     anchor_block_hash[32], sha3_hash[32] (SHA3-256 over the body).
 *   Body: `count` records in canonical (txid,vout) order, each
 *     txid[32], vout u32, value i64, script_len u32, script[*], height u32,
 *     is_coinbase u8.
 *
 * The per-record encoding is the SAME single encoder as coins_kv_commitment
 * (utxo_commitment_sha3_write_record), so the body SHA3 written here equals
 * coins_kv_commitment(db) — and therefore comparable to the
 * compiled checkpoint root. The caller (the `-mint-anchor` driver) HARD-ASSERTS
 * the written header's sha3_hash + count against checkpoints.c before trusting
 * the artifact.
 *
 * Writes to `out_path` atomically (out_path.tmp then rename). `height` and
 * `anchor_block_hash` are stamped into the header for the loader's bind check.
 * On success fills *out_sha3 (32 bytes) and *out_count with what was written.
 * Returns true on success, false (and removes the temp file) on any error. */
bool coins_kv_snapshot_write(struct sqlite3 *db, const char *out_path,
                             int32_t height, const uint8_t anchor_block_hash[32],
                             uint8_t out_sha3[32], uint64_t *out_count,
                             int64_t *out_total_supply);

/* v2 writer: same as coins_kv_snapshot_write, but if `frontier_len` > 0 it
 * appends a Sapling commitment-tree frontier section
 * ([u32 frontier_len LE][blob]) after the UTXO records (still inside the body
 * SHA3 region) and stamps header version = 2. `frontier` must be the output of
 * incremental_tree_serialize for the tree AT `height` (the seed height). With
 * frontier_len == 0 this is byte-identical to the v1 writer. This lets a fresh
 * node seed from the snapshot WITHOUT the blocks/ dir: it deserializes + roots
 * the embedded frontier and verifies it against the PoW-proven
 * hashFinalSaplingRoot at the seed height, skipping the block-replay rebuild. */
bool coins_kv_snapshot_write_v2(struct sqlite3 *db, const char *out_path,
                                int32_t height,
                                const uint8_t anchor_block_hash[32],
                                const uint8_t *frontier, uint32_t frontier_len,
                                uint8_t out_sha3[32], uint64_t *out_count,
                                int64_t *out_total_supply);

#endif /* STORAGE_COINS_KV_H */
