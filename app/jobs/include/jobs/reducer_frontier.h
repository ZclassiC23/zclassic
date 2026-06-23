/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * reducer_frontier — the L0 authority: compute H* (the deepest
 * provably-consistent height) and served_floor from durable progress.kv
 * state. Read-only; no writes, no transactions of its own.
 *
 * H* is the single number every reconciliation gates on. It is computed
 * once at boot/condition-check time from the durable stage logs and the
 * coins-applied frontier — never read from a drifted in-RAM int or a
 * served-tip that can advance past a torn prefix. L1 (flag/cursor reset)
 * and L2 (coin rewind) both clamp to this value rather than re-deriving
 * a private frontier, so all three layers agree on one boundary.
 *
 * The contract:
 *   - [0, H*] is a provably-consistent prefix: every success-checked log
 *     shows a contiguous ok=1 run up to H*, the validate_headers and
 *     script_validate hashes agree where both are present, and the coins
 *     frontier does not contradict it.
 *   - [H*+1, ...] has SOME defect (a hole, an ok=0 row, or a hash split).
 *   - H* >= TRUSTED_ANCHOR (the SHA3 UTXO checkpoint height) ALWAYS — the
 *     algorithm never rewinds across the irreversible finality floor.
 *   - served_floor = MAX(tip_finalize_log.height WHERE ok=1), reported
 *     SEPARATELY so L1 can HOLD a tip finalized above H* (a T10 torn
 *     view) rather than re-serving unstable blocks.
 */

#ifndef ZCL_JOBS_REDUCER_FRONTIER_H
#define ZCL_JOBS_REDUCER_FRONTIER_H

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>

struct json_value;

/* The compiled-in SHA3 UTXO checkpoint height — the irreversible floor H*
 * may never fall below. Mirrors get_sha3_utxo_checkpoint()->height; exposed
 * as a named constant so callers and tests can assert the clamp without
 * pulling in lib/chain. Verified equal to the live checkpoint by the
 * regression test. */
#define REDUCER_FRONTIER_TRUSTED_ANCHOR ((int32_t)3056758)

/* The FLOOR H* and the L1 reconcile operate at. On a NORMAL boot this is the
 * compiled SHA3 checkpoint anchor (REDUCER_FRONTIER_TRUSTED_ANCHOR via
 * reducer_frontier_compiled_anchor() / the test override) — H* never rewinds
 * across finality and a below-anchor cursor is a defect the heal fixes.
 *
 * EXCEPTION — a from-genesis staged refold (jobs/refold_progress.h): while
 * refold_in_progress() is true the fold is legitimately re-walking the frozen
 * prefix from genesis, so the floor drops to 0 — below-anchor H* is REPORTED
 * (not clamped up) and a below-anchor cursor is NOT flagged as a defect. This
 * changes only the H*-reporting floor + whether the self-repair runs; it
 * changes NO validation rule. Returns 0 during a refold, else the compiled
 * anchor. Cheap (atomic cache read + at most the compiled-anchor read). */
int32_t reducer_frontier_floor(void);

/* The PROVABLE TIP cache (H*), served to EXTERNAL consumers (getblockcount,
 * P2P version.start_height, zcl_status chain.height, explorer tip, getblock
 * confirmations). It is a single cached atomic refreshed ONCE per finalized
 * advance and ONCE per reorg rewind — never per RPC (compute_hstar is O(n)).
 *
 * Internal sync-window callers (header-admit window, snapshot lookahead, the
 * watchdog/reconcile/lag detectors, MMR catchup) keep reading
 * active_chain_height() — they legitimately need the lookahead/window tip,
 * which can sit ABOVE H* by the pipeline depth. This cache is the SEPARATE,
 * provable-prefix number, equal to the real tip at steady state and LOWER
 * mid-fold or after a reorg.
 *
 * Init value is REDUCER_FRONTIER_TRUSTED_ANCHOR (the irreversible finality
 * floor) — a sane >= anchor fallback before the first refresh. The getter is
 * a plain lock-free atomic load; never takes progress_store_tx_lock(). */
int32_t reducer_frontier_provable_tip_cached(void);

/* Refresh the cached provable tip (H*). Stores `hstar` verbatim — the caller
 * passes the value it just computed via reducer_frontier_compute_hstar (which
 * already clamps to the finality floor), so this never re-clamps and faithfully
 * mirrors a reorg-LOWERED H*. Called ONLY at the two chokepoints under
 * progress_store_tx_lock: the finalize advance (tip_finalize_stage.c) and the
 * reorg-rewind (rewind_cursor_if_active_chain_reorged). Cross-thread-safe
 * (atomic store); the getter is the matching atomic load. */
void reducer_frontier_provable_tip_set(int32_t hstar);

/* Reset the cached provable tip to the finality anchor — mirrors the
 * tip_finalize g_last_advance_height reset on shutdown / test_reset so a stale
 * high value from a prior run/test group cannot leak into a fresh boot. */
void reducer_frontier_provable_tip_reset(void);

/* Durable trusted-base declaration (progress_meta, 8-byte LE height blob +
 * 32-byte hash blob — the coins_applied_height storage convention). Written
 * RAISE-ONLY by the cold-import/snapshot seed (tip_finalize_anchor.c);
 * read by reducer_trusted_anchor as an anchor candidate vetted by the same
 * reducer_anchor_candidate_ok gate as a tip_finalize_log 'anchor' row.
 *
 * WHY a meta key and not (only) the anchor row: the anchor row at H is
 * pipeline-owned — the very first forward step REPLACES it with the
 * 'finalized' row for the H→H+1 transition (log_insert is INSERT OR
 * REPLACE, and the finalized row at H is the only durable source of
 * hash(H+1) for the boot resolver, so the replacement is correct). A
 * cold-import datadir whose ONLY anchor row was the seed's then starves
 * reducer_trusted_anchor back to the compiled checkpoint, the frontier
 * walk reads the legitimately log-less import region as an 88k hole, and
 * the I4.3 sweep HOLD-wedges an otherwise healthy at-tip node (copy-proven
 * 2026-06-12, run 3). A trust DECLARATION must not live in a row the
 * pipeline consumes. */
#define REDUCER_TRUSTED_BASE_HEIGHT_KEY "reducer_trusted_base_height"
#define REDUCER_TRUSTED_BASE_HASH_KEY   "reducer_trusted_base_hash"

/* Compute H* (deepest provably-consistent height) and served_floor from
 * durable state.
 *
 * Called under progress_store_tx_lock with read-only access to progress_db
 * (progress.kv: stage_cursor, progress_meta, *_log tables) and the coins
 * store (whose applied-frontier == progress_meta['coins_applied_height']).
 *
 * Returns false on a DB read error; true on success (both out params are
 * always set on success). Sets *hstar to >= REDUCER_FRONTIER_TRUSTED_ANCHOR
 * (never below the SHA3 checkpoint). Sets *served_floor to
 * MAX(tip_finalize_log.height WHERE ok=1), or 0 if no ok=1 rows exist.
 *
 * PURE SELECT-only — issues no INSERT/UPDATE/DELETE and opens no
 * transaction of its own. The caller MUST already hold
 * progress_store_tx_lock() so the durable snapshot is consistent. */
bool reducer_frontier_compute_hstar(
    sqlite3 *progress_db,           /* progress.kv handle (lock held by caller) */
    int32_t *hstar,                 /* OUT: deepest provably-consistent height */
    int32_t *served_floor           /* OUT: MAX(tip_finalize ok=1 height), or 0 */
);

/* The contiguous ok=1 prefix of ONE success-checked *_log above the trusted
 * anchor — the same per-log frontier reducer_frontier_compute_hstar MIN-folds.
 * The single DRY reader callers use to derive a per-stage frontier instead of
 * duplicating the anchor/cursor/contiguity SQL:
 *   - validate_headers_log -> the validated HEADER frontier (Invariant A: a tip
 *     is committable only at or below this height).
 *   - utxo_apply_log -> the APPLIED frontier (the coin-tear test compares
 *     coins_applied against THIS, not the tip_finalize-pinned global MIN H*,
 *     so legitimate pipeline depth is never misread as a tear).
 *
 * Reads the trusted anchor + the named stage cursor internally and acquires
 * progress_store_tx_lock() itself (recursive; safe whether or not the caller
 * already holds it). SELECT-only — no writes, no transaction of its own.
 *
 * NOTE: O(cursor - anchor) rows streamed in ONE ranged scan (single
 * prepare; ~0.04 us/height vs 2.3 us/height for the old per-height probe).
 * Still call off the hot reducer-tick path — at boot, condition-check,
 * reconcile, or commit-prepare; periodic callers should memoize via
 * reducer_frontier_log_frontier_above below.
 *
 * Returns false on a DB read error (*out_h is then meaningless); true on
 * success with *out_h in [anchor, cursor-1]. */
bool reducer_frontier_log_frontier(
    sqlite3 *progress_db,           /* progress.kv handle */
    const char *log_table,          /* e.g. "validate_headers_log" */
    const char *cursor_name,        /* e.g. "validate_headers" */
    int32_t *out_h                  /* OUT: contiguous ok=1 prefix height */
);

/* Delta variant of reducer_frontier_log_frontier for callers that already
 * PROVED contiguity up to `verified_floor` on an earlier pass (e.g. the
 * 60 s invariant sweep memoizing its last clean frontier): extends the
 * contiguous ok=1 run from `verified_floor` upward in ONE ranged scan,
 * skipping the trusted-anchor read/re-validation entirely. O(rows above
 * the floor), not O(cursor - anchor) — the cost no longer grows with
 * uptime.
 *
 * CALLER CONTRACT: `verified_floor` MUST be a height this same log was
 * previously verified contiguous-ok=1 through (or the trusted anchor),
 * under a cursor that has NOT rewound since (a cursor rewind means an
 * unwind deleted rows — invalidate the memo and re-walk via
 * reducer_frontier_log_frontier). Acquires progress_store_tx_lock()
 * itself (recursive). SELECT-only. Returns false on a DB read error;
 * true with *out_h >= verified_floor otherwise. */
bool reducer_frontier_log_frontier_above(
    sqlite3 *progress_db,           /* progress.kv handle */
    const char *log_table,          /* e.g. "utxo_apply_log" */
    const char *cursor_name,        /* e.g. "utxo_apply" */
    int32_t verified_floor,         /* previously verified ok=1 height */
    int32_t *out_h                  /* OUT: contiguous ok=1 prefix height */
);

/* Hash recorded by ONE stage log at `height` (e.g. validate_headers_log.hash).
 * The Invariant A restore clamp uses it to derive the frontier tip hash when
 * the in-RAM pprev chain is torn between the frontier and a restore candidate
 * (log-as-truth: the stage's own recorded hash names the frontier block).
 *
 * *found=false on an absent row or a NULL hash (cold-import prefix) — that is
 * success, not an error. Returns false only on a real DB read error. Acquires
 * progress_store_tx_lock() itself (recursive; safe whether or not the caller
 * already holds it). SELECT-only. */
bool reducer_frontier_log_hash_at(
    sqlite3 *progress_db,           /* progress.kv handle */
    const char *log_table,          /* e.g. "validate_headers_log" */
    const char *hash_col,           /* e.g. "hash" */
    int32_t height,
    uint8_t out[32],                /* OUT: the 32-byte hash when *found */
    bool *found                     /* OUT: row + non-NULL 32B hash present */
);

/* MIN(height) over a stage log — the oldest height the log still covers.
 * The stage logs are rolling WINDOWS (rows below the window get pruned /
 * were never written under an anchor seed), so a height BELOW this floor
 * is "log-unknown": the log cannot refute it, only fail to vouch for it.
 * The Invariant A clamp uses this to fail OPEN (index authority only) for
 * candidates beneath the window instead of clamping an 86K-block rollback
 * to the compiled anchor (the 2026-06-11 copy-prove regression).
 *
 * *found=false when the table is empty — success, not an error. Returns
 * false only on a real DB read error. Acquires progress_store_tx_lock()
 * itself (recursive). SELECT-only. */
bool reducer_frontier_log_coverage_floor(
    sqlite3 *progress_db,           /* progress.kv handle */
    const char *log_table,          /* e.g. "validate_headers_log" */
    int32_t *out_lo,                /* OUT: MIN(height) when *found */
    bool *found                     /* OUT: table has at least one row */
);

/* Derive the coins-best fact from progress.kv's own co-committed state.
 * THE derivation (wave 2 of docs/work/canonical-frontier-derived-state-plan.md
 * step 6): the answer is computed from coins_applied_height — co-committed in
 * the SAME BEGIN IMMEDIATE as every coin mutation + utxo_apply cursor move
 * (storage/coins_kv.h) — never from the separately-written node_state
 * 'coins_best_block' key, which is a CACHE that can drift and is never
 * consulted here.
 *
 * Height: *out_height = coins_applied_height - 1.
 * Hash, two durable-log witnesses (SELECT-only):
 *   1. tip_finalize_log read CONVENTION-AWARE (tip_finalize_stage_block_hash_at,
 *      jobs/tip_finalize_stage.h): a finalized ok=1 row at h-1 carries the
 *      LOOKAHEAD hash(h); an anchor seed row at h carries the block's own
 *      hash(h). (The raw row AT h carries hash(h+1) for finalized rows —
 *      reading it as "hash at h" is the inconsistent authority-pair shape
 *      the 2026-06-11 splice forensic banned.)
 *   2. validate_headers_log.hash at that height (own-hash by construction) —
 *      covers the <=1-block pipeline window where utxo_apply leads
 *      tip_finalize (Invariant B bound); the Invariant A trust root.
 * When BOTH witnesses resolve they must byte-agree; a cross-log mismatch is
 * a throttled WARN with *hash_found=false (don't guess between durable logs).
 * Neither resolving => *hash_found=false; either way the HEIGHT stays
 * authoritative (callers may resolve height->hash via their own block index,
 * never the reverse).
 *
 * *found requires the PROVEN-AUTHORITY predicate (coins_kv_is_proven_authority,
 * storage/coins_kv.h): coins_applied_height present AND the coins_kv migration
 * stamp set AND a non-empty set — the same three rungs the L1 torn-anchor heal
 * demands. *found=false (success, not error) on pre-migration / virgin
 * datadirs and on authority-proof read errors — callers then fall back to
 * their stricter labeled legacy gates. Returns false only on a real DB read
 * error in the hash rungs. SELECT-only; acquires progress_store_tx_lock()
 * itself (recursive). */
bool reducer_frontier_derive_coins_best(
    sqlite3 *progress_db,     /* progress.kv handle */
    int32_t *out_height,      /* OUT: coins_applied_height - 1 */
    uint8_t  out_hash[32],    /* OUT: hash at that height when *hash_found */
    bool    *hash_found,      /* OUT: hash resolved from a durable log */
    bool    *found);          /* OUT: coins_applied_height present */

/* Convenience form over the SINGLETON progress store (progress_store_db()):
 * one cheap point-read at each decision point — derive, don't cache.
 * Returns true iff the store is open AND coins_applied_height is present
 * (the canonical-datadir signal: every legacy node_state/mirror anchor is
 * a mere cache). false => legacy datadir / store closed / read error — the
 * caller keeps its labeled legacy fallback. out_hash and out_hash_found
 * may be NULL when only the height is needed. */
bool reducer_frontier_derive_coins_best_now(
    int32_t *out_height,      /* OUT: coins_applied_height - 1 */
    uint8_t  out_hash[32],    /* OUT (nullable): hash when *out_hash_found */
    bool    *out_hash_found); /* OUT (nullable): hash from a durable log */

/* `zcl_state subsystem=reducer_frontier`: read-only snapshot of the L0
 * reducer authority. Reports H*, served_floor, raw stage cursors,
 * success-checked log frontiers, coins_applied_height, and first
 * validate_headers failure. Never writes progress.kv. */
bool reducer_frontier_dump_state_json(struct json_value *out,
                                      const char *key);

#endif /* ZCL_JOBS_REDUCER_FRONTIER_H */
