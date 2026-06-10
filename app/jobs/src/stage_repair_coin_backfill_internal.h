/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stage_repair_coin_backfill_internal — contract between the backfill
 * orchestration TU (stage_repair_coin_backfill.c) and the chain-bound
 * no-spend scan TU (stage_repair_coin_backfill_scan.c). Not a public API. */

#ifndef ZCL_JOBS_STAGE_REPAIR_COIN_BACKFILL_INTERNAL_H
#define ZCL_JOBS_STAGE_REPAIR_COIN_BACKFILL_INTERNAL_H

#include "jobs/stage_repair_coin_backfill.h"

enum coin_backfill_scan_verdict {
    COIN_SCAN_IN_PROGRESS,
    COIN_SCAN_CLEAN,
    COIN_SCAN_SPENT_FOUND,
    COIN_SCAN_GAP,
    COIN_SCAN_CHAIN_REBOUND, /* prev-link mismatch on resume/start: restarted from floor */
};

/* Chunked, resumable, chain-bound no-spend scan (guard G9).
 *
 * Persisted record key: coin_backfill.scan.<H>.<holehash> in progress_meta;
 * payload [next_height i32 LE][frontier_at_start i32 LE]
 *         [last_scanned_hash 32B][set_digest 32B]
 * (+ [top_hash 32B] and a CLEAN flag once complete). Every chunk — start,
 * resume, and mid-chunk alike — requires blk.hashPrevBlock to match the
 * persisted lineage hash before processing, seeded at floor-1; the terminal
 * walk through [frontier..H] must end at the hole row's block hash. */
enum coin_backfill_scan_verdict coin_backfill_scan_step(
    struct sqlite3 *db, struct main_state *ms, const struct coin_backfill_io *io,
    int hole_height, const struct uint256 *hole_hash,
    const struct coin_backfill_outpoint *set, size_t n,
    int floor_height, int top_height, int frontier_at_start,
    int max_blocks, int64_t max_wall_ms,
    int *out_next_height, int *out_spent_height, uint8_t out_spender_txid[32]);

/* ── Persisted scan-record helpers (shared with the orchestration TU) ──
 *
 * Transaction contract: coin_backfill_scan_step (and the load helper)
 * persist/read the record via progress_meta own-tx helpers (internally
 * locked BEGIN IMMEDIATE..COMMIT) — call them OUTSIDE any open progress.kv
 * transaction. Only the insert tx (orchestration TU) uses the _in_tx
 * variants, deleting the record via coin_backfill_scan_record_key +
 * progress_meta_delete_in_tx.
 *
 * Wire layout of the progress_meta blob (all integers i32 LE):
 *   [ 0.. 3] next_height        — next height to scan; always <= frontier
 *                                 while in progress (the terminal walk
 *                                 [frontier..H] is never checkpointed
 *                                 mid-walk, so top_hash is recoverable
 *                                 from the lineage on every resume)
 *   [ 4.. 7] frontier_at_start  — coins_applied_height when the scan began
 *   [ 8..39] last_scanned_hash  — running prev-link lineage = hash(next-1)
 *   [40..71] set_digest         — SHA3-256 of the sorted (txid,vout) set
 * and, only once the terminal linkage through H has been proven:
 *   [72..103] top_hash          — active-chain hash at frontier_at_start-1
 *   [104]     clean flag (0x01)
 * Any other length / flag value is malformed and treated as absent
 * (scan restarts from floor). */
#define COIN_BACKFILL_SCAN_REC_BASE_LEN  72
#define COIN_BACKFILL_SCAN_REC_CLEAN_LEN 105

struct coin_backfill_scan_record {
    int32_t next_height;
    int32_t frontier_at_start;
    uint8_t last_scanned_hash[32];
    uint8_t set_digest[32];
    bool    clean;        /* terminal linkage through H proven */
    uint8_t top_hash[32]; /* valid iff clean */
};

/* Builds "coin_backfill.scan.<H>.<holehash>". False (logged) on overflow. */
bool coin_backfill_scan_record_key(char key[192], int hole_height,
                                   const struct uint256 *hole_hash);

/* Load + decode the persisted record. *found=false when absent. A malformed
 * payload returns true with *found=false (and a WARN) so callers restart
 * from floor instead of erroring. False only on infrastructure failure. */
bool coin_backfill_scan_record_load(struct sqlite3 *db, int hole_height,
                                    const struct uint256 *hole_hash,
                                    struct coin_backfill_scan_record *rec,
                                    bool *found);

/* SHA3-256 over the set sorted by (memcmp(txid), vout); each entry
 * contributes txid(32) || vout(u32 LE). False (logged) on NULL/empty or
 * n > COIN_BACKFILL_MAX_OUTPOINTS. */
bool coin_backfill_scan_set_digest(const struct coin_backfill_outpoint *set,
                                   size_t n, uint8_t out_digest[32]);

#endif /* ZCL_JOBS_STAGE_REPAIR_COIN_BACKFILL_INTERNAL_H */
