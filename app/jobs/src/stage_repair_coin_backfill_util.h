/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stage_repair_coin_backfill_util — private contract between the backfill
 * orchestration TU (stage_repair_coin_backfill.c) and its read-only
 * progress.kv / observability helper TU (stage_repair_coin_backfill_util.c).
 * The split exists ONLY for the E1 file-size ceiling (the orchestration +
 * write transaction alone approach 800 lines); the helpers here are
 * read-only progress.kv accessors, key builders, and the direct refusal
 * paging + zcl_state snapshot state. NOT a public API — only the
 * stage_repair_coin_backfill*.c TUs include this. */

#ifndef ZCL_JOBS_STAGE_REPAIR_COIN_BACKFILL_UTIL_H
#define ZCL_JOBS_STAGE_REPAIR_COIN_BACKFILL_UTIL_H

#include "jobs/stage_repair_coin_backfill.h"

#include <stdint.h>

struct sqlite3;

/* Owner gate (G6): this repair MINTS consensus state, so it carries the
 * strictest gate — mirrors ZCL_REDUCER_VALUE_OVERFLOW_REPAIR_ACK in
 * utxo_apply_delta_repair.c. */
#define COIN_BACKFILL_ACK_ENV "ZCL_REDUCER_COIN_BACKFILL_ACK"

#define COIN_BACKFILL_MAX_ROUNDS 8

/* Refusal-class classification for the durable terminal-refusal marker
 * (block_index_loader_torn_gate.c condition (3)):
 *   RETRYABLE          — the hole may still resolve as the node operates
 *                        (node.db not wired, body not fetched, frontier cursor
 *                        not loaded, owner-ack gate, in-flight scan, window-
 *                        over-budget). NEVER persisted.
 *   TERMINAL           — the coin's creator provably cannot exist in the chain
 *                        we hold (corrupt txindex height, off active chain,
 *                        txid mismatch, vout/value/script out of range,
 *                        immature coinbase, metadata tear, round cap, proven
 *                        double-spend). Persist unconditionally. (scan_gap is
 *                        RETRYABLE — a missing body that resumes once fetched.)
 *   TERMINAL_IF_TXINDEX_COMPLETE — txindex_miss: terminal IFF the txindex is
 *                        COMPLETE (node.db tx_index_complete >= 3); otherwise
 *                        the creating tx may simply not be indexed yet (IBD). */
enum coin_backfill_terminal_class {
    COIN_BACKFILL_TC_RETRYABLE = 0,
    COIN_BACKFILL_TC_TERMINAL,
    COIN_BACKFILL_TC_TERMINAL_IF_TXINDEX_COMPLETE,
};

/* Persist coin_backfill's durable refusal of a TERMINAL (h, hole_hash) so the
 * boot-time torn-import gate (block_index_loader_torn_gate.c condition (3)) can
 * FIRE on a subsequent boot — the gate runs at BOOT, before coin_backfill ticks
 * this boot, so it can only read a durable row a PRIOR boot wrote. `refused_key`
 * is the EXACT key the gate reads (coin_backfill_key_h_hash("coin_backfill.
 * refused", h, hash)); mirrors the existing COIN_SCAN_SPENT_FOUND write (own
 * recursive-lock span, WARN-on-fail-refuse-anyway). RETRYABLE never persists;
 * TERMINAL_IF_TXINDEX_COMPLETE persists only when tx_index_complete >= 3 (so an
 * in-progress IBD txindex_miss is never marked terminal); TERMINAL persists
 * unconditionally. `value_class` is a short forensic token ("txindex_miss" /
 * "unprovable" / "round_cap"); the gate only needs key presence.
 * Call only OUTSIDE the enumerate/scan locked sections (it takes its own
 * lock). NOT consensus state — a diagnostic progress.kv meta marker. */
void coin_backfill_persist_terminal_refusal(
    struct sqlite3 *db, const struct coin_backfill_io *io,
    const char *refused_key, enum coin_backfill_terminal_class tc,
    const char *value_class);

static inline void coin_backfill_le32_put(uint8_t b[4], int32_t v)
{
    uint32_t u = (uint32_t)v;
    b[0] = (uint8_t)(u & 0xff);
    b[1] = (uint8_t)((u >> 8) & 0xff);
    b[2] = (uint8_t)((u >> 16) & 0xff);
    b[3] = (uint8_t)((u >> 24) & 0xff);
}

static inline int32_t coin_backfill_le32_get(const uint8_t b[4])
{
    return (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                     ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
}

const char *coin_backfill_status_name(enum coin_backfill_status st);
bool coin_backfill_owner_ack(void);
void coin_backfill_txid_hex(const uint8_t txid[32], char out[65]);

/* "<prefix>.<height>.<holehash-hex>" — matches reducer_repair_marker_key's
 * format when prefix == "reducer_frontier.script_replay_repair". */
bool coin_backfill_key_h_hash(char out[192], const char *prefix, int height,
                              const struct uint256 *hash);

/* Outpoint-keyed one-shot marker: the key carries ONLY the outpoint so a
 * re-lost coin is detected (MARKER_SEEN) at ANY future hole height; the
 * (H,holehash,round) forensics live in the VALUE (design §2 step 4). */
bool coin_backfill_outpoint_marker_key(char out[160], const uint8_t txid[32],
                                       uint32_t vout);

bool coin_backfill_meta_present(struct sqlite3 *db, const char *key,
                                bool *present);

/* STEP-2A pending-prevout HOLD signal (fixed progress_meta key
 * "script_validate.pending_prevout"): a NON-TERMINAL durable record published
 * by script_validate WHILE it HOLDS the cursor on a transient
 * prevout_unresolved — it stopped writing the terminal ok=0 row, but that row
 * was ALSO the TRIGGER that armed coin_backfill (G2 hole finder) and the boot
 * torn-import gate. For a genuinely torn pre-anchor coin, re-derivation alone
 * never re-inserts the missing coin — coin_backfill must — so this signal keeps
 * the torn-coins class's auto-terminating remedy owner alive without a terminal
 * reject. Only ONE height is held at a time, so a single fixed key suffices;
 * the writer DELETEs it the moment the held height advances (sv_unresolved_
 * clear). Value layout (72 bytes): height(LE32) | block_hash(32) |
 * first_failure_txid(32) | first_failure_vin(LE32). Each takes its own txn. */
bool coin_backfill_pending_prevout_set(struct sqlite3 *db, int height,
                                       const struct uint256 *block_hash,
                                       const struct uint256 *fail_txid,
                                       int fail_vin);
bool coin_backfill_pending_prevout_clear(struct sqlite3 *db);
bool coin_backfill_pending_prevout_get(struct sqlite3 *db, int *out_height,
                                       struct uint256 *out_block_hash,
                                       struct uint256 *out_txid, int *out_vin,
                                       bool *out_found);

/* Round counter value: i32 LE; absent == 0 rounds so far. */
bool coin_backfill_rounds_read(struct sqlite3 *db, const char *key,
                               int32_t *out);

/* G2 + boot torn-gate: lowest hole of EXACTLY `wanted_status` below the
 * script_validate cursor (the tear/repair consumers pass 'prevout_unresolved'),
 * plus the row's block_hash for hash binding. Scanning for the exact status
 * keeps a lower-height transient internal_error from masking a higher genuine
 * prevout_unresolved tear; internal_error/decode holes stay owned by the
 * separate replay path (stale_script_hole_unlocked). `wanted_status` must be
 * non-NULL. Caller holds the progress lock. */
bool find_lowest_prevout_unresolved_hole_unlocked(
    struct sqlite3 *db, int cursor, const char *wanted_status, int *out_height,
    char status_out[32], struct uint256 *hash_out, bool *hash_found);

/* Delta horizon: lowest L with BOTH a utxo_apply_log row AND a
 * utxo_apply_delta row at every height in [L..cursor-1]. Coins created at or
 * above this horizon are inside the reconstructible window — a miss there is
 * a live apply bug, refused to the operator (G7). Walks downward from
 * cursor-1; a capped (unbounded) walk reports -1 and the caller refuses
 * rather than guessing a permissive floor. Caller holds the progress lock. */
bool utxo_apply_log_contiguous_floor(struct sqlite3 *db, int cursor,
                                     int *out_floor);

/* Insert-tx step 1a: the hole row must still be present at H with the SAME
 * block_hash (the row is progress.kv state and survives reorgs — "row
 * present" alone is not binding). Caller holds the lock + open tx. */
bool coin_backfill_hole_row_matches_unlocked(struct sqlite3 *db, int height,
                                             const struct uint256 *hash,
                                             bool *match);

/* Direct operator paging (design B4): typed blocker + EV_OPERATOR_NEEDED
 * from this Job — paging never depends on condition-engine attempt
 * exhaustion. Once-latched per (H,holehash,status); the slot is claimed
 * only AFTER the blocker actually registered, so a failed emission
 * re-pages on the next tick instead of going silent until restart.
 * Returns true iff THIS call emitted (claimed the latch slot); false when
 * latch-suppressed or emission failed. Callers gate their own per-refusal
 * logging on it — the reconcile dry-run re-refuses every ~5 s tick
 * forever on the live default (owner ack unset), so any unlatched WARN
 * on the refusal path is a permanent log drip. */
bool coin_backfill_page_refusal(enum coin_backfill_status st, int h,
                                const struct uint256 *hole_hash,
                                const char *reason);

/* zcl_state snapshot + monotonic counters (dumped by
 * coin_backfill_dump_state_json). */
void coin_backfill_stats_note_call(void);
void coin_backfill_stats_note_rebind(void);
void coin_backfill_stats_note_repaired(int inserted);
void coin_backfill_publish_result(const struct coin_backfill_result *r);

/* Test hook (not part of the frozen public contract): clears the page
 * latches so refusal-paging tests run isolated. Pair with
 * blocker_reset_for_testing. */
void coin_backfill_reset_latches_for_testing(void);

#endif /* ZCL_JOBS_STAGE_REPAIR_COIN_BACKFILL_UTIL_H */
