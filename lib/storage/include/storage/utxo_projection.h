/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_projection — rebuildable UTXO read model over the append-only
 * event_log.
 *
 * The projection is a rebuildable SQLite UTXO set derived from
 * EV_UTXO_ADD / EV_UTXO_SPEND events. The reducer path owns UTXO authorship;
 * test binaries can flip the author to prove the legacy emitters stay silent
 * under stage authority.
 *
 * Threading
 * ----------
 * Single open handle per process (mirrors peers_projection / event_log
 * conventions). `_get` / `_count` / `_commitment` are reentrant reads;
 * `_catch_up` serialises internally via a SQLite IMMEDIATE txn.
 *
 * See `docs/FRAMEWORK.md` for the current reducer authority model. */

#ifndef ZCL_STORAGE_UTXO_PROJECTION_H
#define ZCL_STORAGE_UTXO_PROJECTION_H

#include "storage/event_log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct utxo_projection utxo_projection_t;

/* Single-writer authority over the UTXO projection.
 *
 *   UTXO_AUTHOR_STAGE (the production default) — `utxo_apply_stage` authors
 *       the events from its own validated delta and the legacy emitters
 *       become no-ops, so there is exactly one writer to the projection.
 *   UTXO_AUTHOR_LEGACY — `update_coins()` authors EV_UTXO_* events. This is
 *       the test-only emitter path; tests flip the author here to prove the
 *       legacy emitters stay silent under stage authority.
 *
 * Both emit paths read this flag atomically and only the matching author
 * writes. */
typedef enum {
    UTXO_AUTHOR_LEGACY = 0,
    UTXO_AUTHOR_STAGE  = 1
} utxo_author_t;

utxo_author_t utxo_projection_get_author(void);

#ifdef ZCL_TESTING
void utxo_projection_test_set_author(utxo_author_t who);
#endif

/* Open or create the projection at `projection_path`. Replays from the
 * `last_consumed_offset` stored in the projection's own metadata table
 * (idempotent across crashes). Returns NULL on unrecoverable error
 * (schema mismatch, sqlite open failure). The returned handle is also
 * published as the process-global accessor (see `utxo_projection_get_global`). */
utxo_projection_t *utxo_projection_open(const char *projection_path,
                                        event_log_t *log);

/* Graceful close: PRAGMA wal_checkpoint(TRUNCATE), sqlite3_close.
 * NULL-safe. */
void utxo_projection_close(utxo_projection_t *p);

/* Consume new events from the event log starting at the projection's
 * own last_consumed_offset. Idempotent — replaying the same byte range
 * twice produces the same UTXO set. Returns the new
 * last_consumed_offset on success, or `UINT64_MAX` on error. */
uint64_t utxo_projection_catch_up(utxo_projection_t *p);

/* One-time anchor-seed: bulk-copy the full legacy coins.db `utxos` table
 * into the projection so SHA3(projection)==SHA3(coins.db) and counts match.
 * A projection opened before anchor seeding only holds the tail deltas emitted
 * since its event log was wired; it lacks the historical set that predates
 * projection emission.
 * Clears any existing rows, then copies the full authoritative set in one
 * IMMEDIATE txn, and stamps the cursor to the current event-log head so a
 * later catch_up does NOT re-fold the tail deltas already captured by the
 * snapshot — ongoing catch_up then keeps the projection tracking coins.db.
 * Idempotent/one-time: refuses (returns -1) if already anchor-seeded.
 * Returns the number of UTXOs seeded, or -1 on error/refusal. */
struct sqlite3;
int64_t utxo_projection_seed_from_legacy(utxo_projection_t *p,
                                         struct sqlite3 *legacy_db);

/* Cold-start anchor-seed for fast_sync: identical to seed_from_legacy but
 * sources the verified SHA3 UTXO snapshot from the fast_sync staging table
 * (SNAPSYNC_STAGING_TABLE = "snapshot_staging_utxos") instead of the legacy
 * `utxos` table. Used when a fresh datadir is seeded by FlyClient + SHA3
 * snapshot rather than by a live coins.db: lands the snapshot directly in the
 * authoritative projection so a subsequent boot_rebuild_from_log restores
 * tip+UTXO purely from the log/projection with no legacy sibling read.
 * Clears any existing rows, copies the full set in one IMMEDIATE txn, stamps
 * last_consumed_offset to the current event-log head + anchor_seeded=1.
 * Idempotent/one-time: refuses (returns -1) if already anchor-seeded.
 * Returns the number of UTXOs seeded, or -1 on error/refusal. */
int64_t utxo_projection_seed_from_snapshot(utxo_projection_t *p,
                                           struct sqlite3 *staging_db);

/* Best-effort operator mirror: replace the rebuildable projection's live UTXO
 * rows from the authoritative progress.kv `coins` table, then stamp the
 * projection cursor to the current event-log head. This is non-consensus and
 * intended for post-repair reseeds after coins_kv has already been repaired.
 * Returns false on copy errors; callers must not use failure to roll back a
 * consensus repair. */
bool utxo_projection_reseed_from_coins_kv(utxo_projection_t *p,
                                          struct sqlite3 *progress_db);

/* Lookup a UTXO by (txid, vout). Returns true if present; fills
 * value/script if the corresponding out-pointer is non-NULL. If the
 * caller's `script_cap` is smaller than the stored script length, the
 * script is truncated to `script_cap` bytes — but `*script_len_out`
 * always reports the true length. */
bool utxo_projection_get(utxo_projection_t *p,
                         const uint8_t txid[32], uint32_t vout,
                         int64_t *value_out,
                         uint8_t *script_out, size_t script_cap,
                         size_t *script_len_out);

/* B4: reconstruct a full `struct coins` (all live outputs of a txid)
 * from the projection — the read primitive behind the projection-backed
 * coins_view (see coins_view_projection.h). `out` is coins_init'd by
 * this call; on success it owns an allocated vout array the caller must
 * coins_free(). Returns false (num_vout==0) if the txid has no live
 * outputs. `version` is set to 1 to match coins_view_sqlite exactly. */
struct coins;
bool utxo_projection_get_coins(utxo_projection_t *p,
                               const uint8_t txid[32], struct coins *out);

/* Total live UTXOs in the projection. O(SELECT COUNT(*)) — acceptable
 * given we run with WITHOUT ROWID and SQLite caches the count. */
uint64_t utxo_projection_count(utxo_projection_t *p);

/* The projection's on-disk db path (for a one-shot read-only ATTACH bulk copy,
 * e.g. the coins_kv boot migration). NULL if unavailable. */
const char *utxo_projection_path(const utxo_projection_t *p);

/* gettxoutsetinfo aggregate over the projection: distinct txids, total UTXO
 * outputs, summed value (zatoshi). Byte-compatible with the legacy coins.db
 * query so the projection-backed RPC matches exactly. Returns false if the
 * projection is unavailable; out params are optional (NULL to skip). */
bool utxo_projection_setinfo(utxo_projection_t *p, int64_t *num_txs,
                             int64_t *num_txouts, int64_t *total_amount);

/* SHA3-256 over every (txid|vout_le|value_le|script_len_le|script|
 * height_le|is_coinbase) UTXO in ORDER BY txid, vout. Matches the
 * canonical serialisation in `lib/coins/src/utxo_commitment.c` so audits can
 * compare the projection commitment to the legacy coins.db commitment.
 * Returns 0 on success. */
int utxo_projection_commitment(utxo_projection_t *p, uint8_t out[32]);

/* Process-global accessor for the projection handle published by the
 * boot wiring (`config/src/boot.c`). Returns NULL if not yet opened or
 * already closed. Safe to call from any thread. */
utxo_projection_t *utxo_projection_get_global(void);

/* Process-global setter for the event log used by the projection emission
 * path in `update_coins.c`. NULL log disables emission (the legacy
 * SQLite write still happens). Mirrors peers_projection wiring. */
void utxo_projection_set_event_log(event_log_t *log);
event_log_t *utxo_projection_event_log(void);

/* Projection emission helpers used by `lib/validation/src/update_coins.c`.
 * Both increment `g_utxo_event_emit_*_total` counters internally so the
 * dump_state_json output shows how many events have been authored.
 *
 * `script_bytes` may be NULL iff `script_len == 0`. */
bool utxo_projection_emit_add(const uint8_t txid[32], uint32_t vout,
                              int64_t value, uint32_t height,
                              bool is_coinbase,
                              const uint8_t *script_bytes,
                              uint32_t script_len);
bool utxo_projection_emit_spend(const uint8_t txid[32], uint32_t vout);

/* For zcl_state subsystem=utxo_projection (CLAUDE.md convention).
 * `out` is initialized by the caller; this function also calls
 * json_set_object(out) defensively. `key` is unused. */
struct json_value;
bool utxo_projection_dump_state_json(struct json_value *out, const char *key);

#endif /* ZCL_STORAGE_UTXO_PROJECTION_H */
