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
 * SHA3 UTXO commitment is byte-identical.
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

/* True iff (txid,vout) is currently live (unspent). */
bool coins_kv_exists(struct sqlite3 *db, const uint8_t txid[32], uint32_t vout);

/* Point-read one live output (txid,vout). Returns true iff the output is
 * currently live (unspent), filling value/script via the non-NULL out-pointers.
 * If `script_cap` is smaller than the stored script length the script is
 * truncated to `script_cap` bytes, but `*script_len_out` always reports the
 * true length. Mirrors utxo_projection_get exactly so script_validate's prevout
 * resolver is a verbatim swap. A spent or absent output returns false. */
bool coins_kv_get(struct sqlite3 *db, const uint8_t txid[32], uint32_t vout,
                  int64_t *value_out, uint8_t *script_out, size_t script_cap,
                  size_t *script_len_out);

/* Count of live outputs. Returns -1 on error. */
int64_t coins_kv_count(struct sqlite3 *db);

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
 * BYTE-IDENTICAL serialisation to utxo_projection_commitment (the read-flip
 * relies on this matching the oracle gettxoutsetinfo commitment). Returns 0 on
 * success, -1 on error. */
int coins_kv_commitment(struct sqlite3 *db, uint8_t out[32]);

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
 * construction (see docs/work/self-healing-reducer-plan.md). The frontier is
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
 * Returns false only on a hard read error (db NULL / malformed blob). */
bool coins_kv_get_applied_height(struct sqlite3 *db, int32_t *out, bool *found);

/* One-time idempotent boot backfill for existing datadirs that predate this
 * key: if coins_applied_height is ABSENT, seed it (in its OWN BEGIN IMMEDIATE —
 * the one allowed standalone txn, since it backfills a derived value that
 * already equals the durable cursor with NO coin mutation) from the trusted
 * utxo_apply stage cursor. NEVER seeds from MAX(coins.height). No-op once the
 * key exists. Returns false on error (next boot retries). */
bool coins_kv_backfill_applied_height_if_absent(struct sqlite3 *db);

#endif /* STORAGE_COINS_KV_H */
