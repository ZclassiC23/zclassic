/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * stage_repair — reducer-stage repair helpers used by Conditions.
 *
 * These helpers operate on progress.kv, the reducer's durable cursor/log
 * store. They intentionally live with Jobs rather than Conditions so each
 * Condition stays a small detect/remedy/witness file. */

#ifndef ZCL_JOBS_STAGE_REPAIR_H
#define ZCL_JOBS_STAGE_REPAIR_H

#include "core/uint256.h"
#include "primitives/block.h"

#include <stdbool.h>

struct sqlite3;
struct main_state;

enum stage_repair_header_solution_poison {
    STAGE_REPAIR_POISON_NONE = 0,
    STAGE_REPAIR_POISON_VALIDATE_SOLUTIONLESS,
    STAGE_REPAIR_POISON_VALIDATE_HASH_MISMATCH,
    STAGE_REPAIR_POISON_DOWNSTREAM_STALE,
};

enum stage_repair_tipfin_refused_reason {
    STAGE_REPAIR_TIPFIN_REFUSED_NONE = 0,
    STAGE_REPAIR_TIPFIN_REFUSED_G1_COIN_UNKNOWN = 1,
    STAGE_REPAIR_TIPFIN_REFUSED_G2_EVIDENCE_ROW = 2,
    STAGE_REPAIR_TIPFIN_REFUSED_G2_ROW_PRESENT = 3,
    STAGE_REPAIR_TIPFIN_REFUSED_G3_MISSING_EVIDENCE = 4,
    STAGE_REPAIR_TIPFIN_REFUSED_G4_AT_SERVED_FLOOR = 5,
    STAGE_REPAIR_TIPFIN_REFUSED_G5_BINDER_MISSING = 6,
    STAGE_REPAIR_TIPFIN_REFUSED_G6_IN_TX_RECHECK = 7,
    STAGE_REPAIR_TIPFIN_REFUSED_G7_MARKER_SEEN = 8,
    STAGE_REPAIR_TIPFIN_REFUSED_HSTAR_RANGE = 9,
};

enum stage_repair_tipfin_refused_log {
    STAGE_REPAIR_TIPFIN_LOG_UNKNOWN = 0,
    STAGE_REPAIR_TIPFIN_LOG_VALIDATE_HEADERS = 1,
    STAGE_REPAIR_TIPFIN_LOG_SCRIPT_VALIDATE = 2,
    STAGE_REPAIR_TIPFIN_LOG_VALIDATE_SCRIPT_SPLIT = 3,
    STAGE_REPAIR_TIPFIN_LOG_BODY_PERSIST = 4,
    STAGE_REPAIR_TIPFIN_LOG_PROOF_VALIDATE = 5,
    STAGE_REPAIR_TIPFIN_LOG_UTXO_APPLY = 6,
    STAGE_REPAIR_TIPFIN_LOG_TIP_FINALIZE = 7,
};

struct stage_repair_header_solution_result {
    bool repaired;
    int target_height;
    int deleted_rows;
    int rewound_cursors;
    enum stage_repair_header_solution_poison mode;
};

struct stage_repair_body_fetch_gap {
    bool ready;
    bool body_observed;
    int target_height;
    int validate_cursor;
    int body_fetch_cursor;
};

enum stage_repair_header_solution_poison
stage_repair_header_solution_poison_mode(struct sqlite3 *db, int height);

bool stage_repair_header_solution_repairable_validate_frontier(
    struct sqlite3 *db,
    int *out_height);

bool stage_repair_header_solution_save(struct sqlite3 *db,
                                       int height,
                                       const struct uint256 *hash,
                                       const struct block_header *header);

bool stage_repair_header_solution_load(struct sqlite3 *db,
                                       int height,
                                       const struct uint256 *expected_hash,
                                       struct block_header *out);

/* Returns true iff a header-solution row is present at `height` AND — when
 * `expected_hash != NULL` — its stored hash equals expected_hash, i.e. the
 * CORRECT solution for the canonical block at that height is present. Pass NULL
 * for a presence-only check (any row that round-trips). Hash-aware callers pass
 * active_chain_at(height)->phashBlock so a STALE wrong-block row (e.g. an
 * earlier off-by-N save) does NOT count as available — otherwise the backfill /
 * self-heal paths would skip a height whose stored solution validate_headers
 * (whose load IS hash-checked) keeps rejecting, wedging the tip. */
bool stage_repair_header_solution_available(struct sqlite3 *db, int height,
                                            const struct uint256 *expected_hash);

bool stage_repair_header_solution_poison_rewind(
    struct sqlite3 *db,
    int height,
    int active_tip_height,
    struct stage_repair_header_solution_result *out);

bool stage_repair_body_fetch_missing_have_data_candidate(
    struct sqlite3 *db,
    int height,
    struct stage_repair_body_fetch_gap *out);

bool stage_repair_body_fetch_missing_have_data_frontier_candidate(
    struct sqlite3 *db,
    struct stage_repair_body_fetch_gap *out);

bool stage_repair_body_fetch_observed(struct sqlite3 *db, int height);

struct stage_reconcile_result {
    bool clamped;   /* the tip_finalize cursor was strictly above floor and moved */
    int  floor;     /* coins_best + 1 */
};

struct stage_reducer_frontier_reconcile_result {
    bool repaired;
    bool clamped_tip_finalize;
    bool refused_coin_tear;
    bool refused_coin_unknown;
    bool coins_applied_found;
    int hstar;
    int served_floor;
    int coins_applied_height;
    bool clamped_validate_headers;
    int validate_headers_cursor_before;
    int validate_headers_cursor_after;
    bool clamped_body_fetch;
    int body_fetch_cursor_before;
    int body_fetch_cursor_after;
    bool clamped_body_persist;
    int body_persist_cursor_before;
    int body_persist_cursor_after;
    int tip_finalize_cursor_before;
    int tip_finalize_cursor_after;
    int sweep_top;
    int lowest_have_data_cleared;
    int lowest_validate_headers_refill_hole;
    int lowest_validate_headers_hash_split;
    int lowest_body_fetch_refill_hole;
    int lowest_body_persist_refill_hole;
    int scripts_set;
    int have_data_set;
    int have_data_cleared;
    int failed_mask_cleared;
    int header_events_emitted;
    bool value_overflow_repair_attempted;
    bool value_overflow_repaired;
    bool value_overflow_repair_owner_refused;
    bool value_overflow_repair_marker_seen;
    bool value_overflow_repair_genuinely_invalid;
    int value_overflow_repair_height;
    int value_overflow_cursor_before;
    int value_overflow_cursor_after;
    bool stale_script_repair_attempted;
    bool stale_script_repaired;
    bool stale_script_repair_marker_seen;
    bool stale_script_repair_genuinely_invalid;
    int stale_script_repair_height;
    int stale_script_cursor_before;
    int stale_script_cursor_after;
    int stale_script_backfill_first;
    int stale_script_backfill_last;
    int stale_script_utxo_cursor_before;
    int stale_script_tip_cursor_before;
    bool coin_backfill_attempted;
    int coin_backfill_status; /* enum coin_backfill_status */
    int coin_backfill_hole_height;
    int coin_backfill_unresolved;
    int coin_backfill_inserted;
    int coin_backfill_scan_next;
    bool coin_backfill_owner_refused;
    bool coin_backfill_genuinely_invalid;
    int lowest_script_validate_refill_hole;
    int lowest_proof_validate_refill_hole;
    bool clamped_script_validate;
    bool clamped_proof_validate;
    int script_validate_cursor_before;
    int script_validate_cursor_after;
    int proof_validate_cursor_before;
    int proof_validate_cursor_after;
    bool pre_refusal_unapplied_clamp;
    int tipfin_backfill_height;
    int tipfin_backfill_count;
    bool tipfin_backfill_marker_seen;
    /* enum stage_repair_tipfin_refused_reason code; 0 =
     * STAGE_REPAIR_TIPFIN_REFUSED_NONE. The refusal WARN names the guard in
     * text; *_height/log expose the precise evidence boundary to Conditions
     * without parsing node.log. */
    int tipfin_backfill_refused_reason;
    int tipfin_backfill_refused_height;
    int tipfin_backfill_refused_log;
    /* Non-canonical row purge (stage_repair_reducer_frontier_purge.c):
     * stage-log rows whose stored hash doesn't match the canonical block
     * at their height — relabel/reorg residue. found counts rows judged
     * stale (dry-run too); purged counts actual deletions (apply only). */
    int noncanonical_found;
    int noncanonical_purged;
    int lowest_noncanonical;
    /* Stale reorg-residue tip_finalize verdict REPLACEMENT (FIX-A,
     * stage_repair_reducer_frontier_purge.c): an ok=0 skip-status
     * tip_finalize_log row (reorg_detected / utxo_count_diverged residue)
     * left at a height already covered by coins (h <= coins_applied-1) and
     * re-evidenced upstream (header_admit_log present at h) caps H* below the
     * coins frontier, manufacturing a FALSE coin-tear refusal even though a
     * contiguous column above it is fully refillable. The replacement writes
     * a fresh ok=1 'finalize_backfill' verdict (row REPLACED in place — never
     * deleted, served_floor preserved) so the row stops capping H*; the
     * existing header_admit-keyed refill + tip_finalize clamp then re-derive
     * the column. found counts rows judged stale (dry-run too); replaced
     * counts in-place rewrites (apply only). */
    int reorg_residue_tipfin_found;
    int reorg_residue_tipfin_replaced;
    int lowest_reorg_residue_tipfin;
};

/* Reconcile a reducer cursor/coins desync that wedges the chain.
 *
 * After an unclean restart (kill-9 + WAL) the durable tip_finalize cursor can
 * sit AHEAD of the durably-applied coins tip (`coins_best`). tip_finalize then
 * idles ("cursor says done") and never re-finalizes, so the connect gate
 * rejects every block at coins_best+1 with "block-not-finalized-by-reducer".
 *
 * This reconciles ONLY the tip_finalize cursor to the stronger of
 * `coins_best + 1` and durable served finality
 * (`MAX(tip_finalize_log.ok=1) + 1`). The second guard is mandatory because a
 * stale coins_best scan can lag rows already served to RPC; boot must never
 * lower the public tip below those finalized rows. Upstream logs/cursors are
 * left untouched, so any re-finalize pass still has its evidence.
 *
 * SAFETY (proven in test_stage_reducer_unwedge):
 *   - It NEVER deletes any *_log row, and it never writes the tip_finalize
 *     cursor below the Tier-2 public-tip authority
 *     (`SELECT MAX(height) FROM tip_finalize_log WHERE ok=1`) plus one. The
 *     public tip must not regress below served finality.
 *   - It touches ONLY the tip_finalize cursor — no upstream cursor or log — so
 *     the re-finalize cannot self-stall.
 *   - No-op (clamped=false) when the tip_finalize cursor already equals the
 *     target; refuses (returns true, clamped=false) when `coins_best < 0`
 *     (no durable anchor to floor on).
 *
 * Must run at boot AFTER coins_best is durable and BEFORE the stages init (so
 * they load the clamped cursor). Single transaction. */
bool stage_reconcile_clamp_tip_finalize_to_floor(
    struct sqlite3 *db,
    int coins_best,
    struct stage_reconcile_result *out);

const char *stage_repair_tipfin_refused_reason_label(int reason);
const char *stage_repair_tipfin_refused_log_label(int log);
bool stage_repair_tipfin_refusal_is_pending_forward(
    const struct stage_reducer_frontier_reconcile_result *rr);

/* L1 reducer-frontier reconcile: repair block_index mirror flags from
 * hash-bound durable reducer logs, rewind validate_headers/body_fetch/
 * body_persist to the lowest missing admitted/validated/body row or cleared
 * HAVE_DATA hole so forward-only stages can refill the gap, then clamp
 * tip_finalize to the coin-capped H*+1 floor so tip_finalize can replay
 * forward. This is a flag/cursor repair only: it never deletes log rows and
 * never mutates coins. If coins_applied_height is absent or present above H*,
 * the helper refuses (unknown/L2 coin-tear domain). */
bool stage_reducer_frontier_reconcile_light_needed(
    struct sqlite3 *db,
    struct main_state *ms,
    struct stage_reducer_frontier_reconcile_result *out);

bool stage_reducer_frontier_reconcile_light(
    struct sqlite3 *db,
    struct main_state *ms,
    struct stage_reducer_frontier_reconcile_result *out);

#ifdef ZCL_TESTING
/* Test-only: drop the dry-run detect memo so the next reconcile re-sweeps. Call
 * between fixtures that close+reopen progress.kv (see the definition comment). */
void stage_reducer_frontier_reset_detect_memo_for_testing(void);
#endif

#endif /* ZCL_JOBS_STAGE_REPAIR_H */
