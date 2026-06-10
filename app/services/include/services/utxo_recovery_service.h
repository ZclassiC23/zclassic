/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * UTXO Recovery Service — boot-time UTXO wipe, import, restore, and
 * integrity operations, all gated through recovery_policy.
 *
 * Background
 * ----------
 * Every destructive UTXO operation (wipe, import, restore) is policy-gated to
 * prevent a repeat of the 2026-04-10 incident where 1.3M UTXOs were wiped by
 * an unguarded recovery path.
 *
 * All functions take explicit parameters — no globals.
 */

#ifndef ZCL_SERVICES_UTXO_RECOVERY_SERVICE_H
#define ZCL_SERVICES_UTXO_RECOVERY_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include "core/uint256.h"
#include "services/chain_state_validator.h"
#include "util/result.h"

/* Forward declarations */
struct sqlite3;
struct main_state;
struct coins_view_sqlite;
struct coins_view_cache;
struct node_db;
struct chain_params;
struct block_index;
struct chain_activation_controller;
struct db_service;

/* ── Context for recovery operations ───────────────────────── */

struct utxo_recovery_ctx {
    struct main_state *state;
    struct coins_view_sqlite *coins_sqlite;
    struct coins_view_cache *coins_tip;
    struct node_db *ndb;
    const char *datadir;
    const struct chain_params *params;
    struct chain_activation_controller *activation_ctl;
    struct db_service *db_service;   /* NULL if not started */
};

/* ── Policy-gated UTXO wipe ───────────────────────────────── */

/* Wipe the UTXO set after checking recovery_policy.
 * Returns ZCL_OK if the wipe was allowed and executed; a policy ZCL_ERR
 * (code -41) if recovery_policy refused; a persistence ZCL_ERR (code -42)
 * if node_db_wipe_utxos failed to delete the rows.
 * `reason` is a grep-able tag, e.g. "boot.reimport_utxos_flag".
 * This is the gate that would have saved the 1.3M UTXOs on
 * 2026-04-10. Do not bypass. */
struct zcl_result utxo_recovery_wipe(struct node_db *ndb, const char *reason);

/* ── Auto-reimport flag ───────────────────────────────────── */

/* Prepare for reimport: clear migration flag (the actual UTXO wipe
 * happens at the start of utxo_recovery_import_ldb). Returns ZCL_OK on
 * success, or a persistence ZCL_ERR if clearing the flag failed. */
struct zcl_result utxo_recovery_prepare_reimport(struct node_db *ndb);

/* ── LDB→SQLite UTXO import ──────────────────────────────── */

struct utxo_import_result {
    struct zcl_result status; /* rich status for service-result discipline */
    bool imported;          /* UTXOs were successfully imported */
    bool skip_activate;     /* caller should skip reducer activation */
    int height;             /* discovered import height */
    uint64_t utxo_count;    /* number of UTXOs imported */
    char anchor_reason[64]; /* activation anchor reason, if set */
};

/* Import UTXOs from LevelDB chainstate to SQLite.
 * Handles: LOCK file copy, policy-gated wipe, SHA3 verification,
 * chain tip / anchor creation. */
struct utxo_import_result utxo_recovery_import_ldb(
    struct utxo_recovery_ctx *ctx);

/* ── Chain tip restoration ───────────────────────────────── */

struct chain_restore_result {
    struct zcl_result status; /* rich status for service-result discipline */
    bool restored;          /* chain tip was successfully restored */
    bool skip_activate;     /* caller should skip reducer activation */
    int restored_height;    /* restored tip height, -1 if none */
    struct uint256 restored_hash; /* restored tip hash, null if none */
    char anchor_reason[64]; /* activation anchor reason, if set */
};

/* Restore chain tip from coins DB best block hash.
 * Creates placeholder anchor if coins_best_block is ahead of index.
 * Falls back to fast_rebuild_chainstate if coins DB is empty. */
struct chain_restore_result utxo_recovery_restore_chain_tip(
    struct utxo_recovery_ctx *ctx,
    struct block_index *scan_fallback);

/* ── Validation recovery execution ───────────────────────── */

struct recovery_exec_result {
    struct zcl_result status; /* rich status for recovery execution */
    bool skip_activate;     /* caller should skip reducer activation */
    bool recovered;         /* a recovery action was taken */
};

enum utxo_count_check_level {
    UTXO_COUNT_CHECK_OK = 0,
    UTXO_COUNT_CHECK_INFO_STALE_REFERENCE,
    UTXO_COUNT_CHECK_WARNING,
    UTXO_COUNT_CHECK_CRITICAL
};

struct utxo_count_check_result {
    enum utxo_count_check_level level;
    int blocks_past_checkpoint;
    double pct_delta;
};

struct utxo_count_check_result utxo_recovery_classify_count_check(
    int tip_height,
    int checkpoint_height,
    uint64_t checkpoint_count,
    uint64_t actual_count);

bool utxo_recovery_xor_mismatch_is_corruption_candidate(
    uint64_t saved_count,
    uint64_t computed_count);

/* Execute recovery based on validate_coins_chain_agreement result.
 * Handles REIMPORT, WIPE_WAIT, RESET_CHAIN, and BOOT_OK integrity
 * checks (stale genesis wipe, UTXO count sanity, XOR commitment). */
struct recovery_exec_result utxo_recovery_execute(
    struct utxo_recovery_ctx *ctx,
    struct boot_validation_result *vr);

/* Boot preflight: if coins_best_block is stale but the durable sync
 * projection names a later consensus-backed tip, advance the coins cursor
 * after applying the same bounded one-block overshoot guard used by the
 * normal UTXO rewind path. Returns true only when a repair was made. */
bool utxo_recovery_repair_stale_cursor_from_sync_projection(
    struct node_db *ndb);

/* L1 torn-legacy-coins boot recovery (the §3 dual-store tear).
 *
 * Fires ONLY from the boot gate AFTER coins_view_sqlite_open() returned false
 * with the torn-legacy shape: node.db `utxos` has rows but `coins_best_block`
 * is UNSET (the crash lost the legacy mirror's lazy batch + tip anchor, while
 * the tear-PROOF reducer authority coins_kv committed every block atomically).
 *
 * Recovery is gated on coins_kv being the PROVEN-healthy authority:
 *   (1) coins_kv_migration_complete == 1  (read-flip done; coins_kv is sole),
 *   (2) coins_kv_count(progress_db) > 0   (it actually holds the live set),
 *   (3) coins_kv_get_applied_height found  (it has a durable applied frontier).
 * If ANY predicate fails, the function returns false and the caller's FATAL is
 * preserved unchanged (no safety gate is weakened).
 *
 * On the proven-healthy path it re-seeds `coins_best_block` to the block hash
 * at `MAX(height) FROM utxos` — the height the LEGACY mirror actually reaches,
 * NEVER the (further-ahead) coins_kv frontier — so the SHA3 snapshot served to
 * peers stays self-consistent with its committed anchor. The chosen height's
 * block must be consensus-backed on disk (a node.db `blocks` row with
 * status>=3); otherwise the function refuses (returns false → FATAL preserved).
 *
 * Reset-safe: it only WRITES the anchor (never deletes a *_log row, never
 * lowers coins_kv, never resets the tip). progress_db is the live
 * progress_store_db() handle (already open at the gate). Returns true ONLY
 * when the anchor was durably written and a retry of coins_view_sqlite_open
 * is warranted; false otherwise (caller must FATAL). */
bool utxo_recovery_heal_torn_legacy_coins_anchor(
    struct node_db *ndb,
    struct sqlite3 *progress_db,
    const char *datadir);

/* ── UTXO cleanup ────────────────────────────────────────── */

/* Delete UTXOs with height above chain tip.
 * SAFETY: only a single-block overshoot of <= UTXO_BOOT_REWIND_MAX_ROWS (32)
 * rows is auto-healable; a larger proposed wipe is refused (tip is likely
 * wrong — investigate block_index/coins drift instead).
 * Returns count of UTXOs deleted (0 if refused or none found).
 * NOTE: this is also the single heal mechanism reused by the continuous
 * orphan_utxo_above_tip Condition; the boot.c one-shot caller is a
 * boot-ordering requirement (runs before the Condition engine registers). */
int utxo_recovery_clean_above_tip(struct node_db *ndb,
                                   struct main_state *state);

/* ── Shielded value backfill ─────────────────────────────── */

/* Backfill sprout_value/sapling_value into SQLite blocks table
 * from block files on disk. Idempotent — skips already-populated.
 * On success returns ZCL_OK and, if out_updated != NULL, writes the
 * count of blocks updated. On failure returns a non-ok zcl_result
 * (-50/-51 = invalid args, -52 = write/persistence failure) and leaves
 * *out_updated untouched. */
struct zcl_result utxo_recovery_backfill_shielded(struct node_db *ndb,
                                     struct db_service *dbsvc,
                                     struct main_state *state,
                                     const char *datadir,
                                     int *out_updated);

#endif /* ZCL_SERVICES_UTXO_RECOVERY_SERVICE_H */
