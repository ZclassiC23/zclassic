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

/* Round counter value: i32 LE; absent == 0 rounds so far. */
bool coin_backfill_rounds_read(struct sqlite3 *db, const char *key,
                               int32_t *out);

/* G2: lowest repairable hole below the script_validate cursor — same status
 * triplet as stale_script_hole_unlocked (single-frontier discipline), plus
 * the row's block_hash for hash binding. Caller holds the progress lock. */
bool find_lowest_prevout_unresolved_hole_unlocked(
    struct sqlite3 *db, int cursor, int *out_height, char status_out[32],
    struct uint256 *hash_out, bool *hash_found);

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
 * exhaustion. Once-latched per (H,holehash,status). */
void coin_backfill_page_refusal(enum coin_backfill_status st, int h,
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
