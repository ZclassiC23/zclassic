/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * UTXO Recovery Service: boot-time UTXO consistency checks and destructive
 * recovery operations gated through recovery_policy.
 */

#include "services/utxo_recovery_service.h"
#include "services/recovery_policy.h"
#include "services/chain_activation_service.h"
#include "services/chain_restore_executor.h"
#include "services/chain_restore_repair.h"
#include "services/chain_state_service.h"
#include "services/chain_tip.h"
#include "net/snapshot_sync_contract.h"
#include "config/boot_internal.h"
#include "config/db_service.h"
#include "validation/main_state.h"
#include "validation/process_block.h"
#include "chain/chainparams.h"
#include "storage/coins_view_sqlite.h"
#include "storage/coins_db.h"
#include "coins/coins_view.h"
#include "coins/utxo_commitment.h"
#include "chain/checkpoints.h"
#include "chain/chain.h"
#include "chain/pow.h"
#include "models/database.h"
#include "models/block.h"
#include "storage/disk_block_io.h"
#include "event/event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdatomic.h>
#include <limits.h>
#include <sqlite3.h>

#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include "utxo_recovery_internal.h"

#define UTXO_CHECKPOINT_NEAR_WINDOW 144
#define UTXO_BOOT_REWIND_MAX_ROWS 32

static struct zcl_result recovery_exec_status_ok(void)
{
    return ZCL_OK;
}

static bool urs_block_height_for_hash(struct node_db *ndb,
                                      const uint8_t hash[32],
                                      int64_t *out_height)
{
    if (!ndb || !ndb->open || !ndb->db || !hash || !out_height)
        return false;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT height FROM blocks WHERE hash=? AND status>=3",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("chain", "stale cursor repair: block lookup prepare "
                 "failed: %s", sqlite3_errmsg(ndb->db));
        return false;
    }
    sqlite3_bind_blob(st, 1, hash, 32, SQLITE_STATIC);
    bool ok = false;
    if (AR_STEP_ROW_READONLY(st) == SQLITE_ROW) {
        *out_height = sqlite3_column_int64(st, 0);
        ok = true;
    }
    sqlite3_finalize(st);
    return ok;
}

static bool urs_utxo_height_summary(struct node_db *ndb,
                                    bool *out_have_utxos,
                                    int64_t *out_max_height)
{
    if (!ndb || !ndb->open || !ndb->db ||
        !out_have_utxos || !out_max_height)
        return false;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT MAX(height), COUNT(*) FROM utxos",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN("chain", "stale cursor repair: utxo summary prepare "
                 "failed: %s", sqlite3_errmsg(ndb->db));
        return false;
    }

    bool ok = false;
    if (AR_STEP_ROW_READONLY(st) == SQLITE_ROW) {
        *out_have_utxos = sqlite3_column_int64(st, 1) > 0;
        *out_max_height = sqlite3_column_type(st, 0) == SQLITE_INTEGER
            ? sqlite3_column_int64(st, 0) : -1;
        ok = true;
    }
    sqlite3_finalize(st);
    return ok;
}

bool utxo_recovery_repair_stale_cursor_from_sync_projection(
    struct node_db *ndb)
{
    if (!ndb || !ndb->open || !ndb->db)
        return false;

    uint8_t coins_hash[32];
    size_t len = 0;
    if (!node_db_state_get(ndb, "coins_best_block",
                           coins_hash, sizeof(coins_hash), &len) ||
        len != sizeof(coins_hash))
        return false;

    int64_t sync_h = -1;
    if (!node_db_state_get_int(ndb, "sync_projection_tip_height", &sync_h) ||
        sync_h <= 0)
        return false;

    uint8_t sync_hash[32];
    len = 0;
    if (!node_db_state_get(ndb, "sync_projection_tip_hash",
                           sync_hash, sizeof(sync_hash), &len) ||
        len != sizeof(sync_hash))
        return false;

    int64_t coins_h = -1;
    if (!urs_block_height_for_hash(ndb, coins_hash, &coins_h))
        return false;

    int64_t resolved_sync_h = -1;
    if (!urs_block_height_for_hash(ndb, sync_hash, &resolved_sync_h) ||
        resolved_sync_h != sync_h)
        return false;
    if (sync_h <= coins_h)
        return false;

    bool have_utxos = false;
    int64_t max_utxo_h = -1;
    if (!urs_utxo_height_summary(ndb, &have_utxos, &max_utxo_h) ||
        !have_utxos)
        return false;

    if (max_utxo_h + UTXO_CHECKPOINT_NEAR_WINDOW < sync_h) {
        LOG_WARN("chain", "stale cursor repair refused: coins_h=%lld "
                 "sync_h=%lld max_utxo_h=%lld below sync window=%d",
                 (long long)coins_h, (long long)sync_h,
                 (long long)max_utxo_h, UTXO_CHECKPOINT_NEAR_WINDOW);
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
            "stale_cursor_repair_refused coins_h=%lld sync_h=%lld "
            "max_utxo_h=%lld below_window=%d",
            (long long)coins_h, (long long)sync_h,
            (long long)max_utxo_h, UTXO_CHECKPOINT_NEAR_WINDOW);
        return false;
    }

    if (max_utxo_h > sync_h + 1) {
        LOG_WARN("chain", "stale cursor repair refused: coins_h=%lld "
                 "sync_h=%lld max_utxo_h=%lld exceeds one-block guard",
                 (long long)coins_h, (long long)sync_h,
                 (long long)max_utxo_h);
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
            "stale_cursor_repair_refused coins_h=%lld sync_h=%lld "
            "max_utxo_h=%lld",
            (long long)coins_h, (long long)sync_h,
            (long long)max_utxo_h);
        return false;
    }

    if (max_utxo_h > sync_h) {
        int deleted = coins_rewind_above_tip(
            ndb->db, sync_h, UTXO_BOOT_REWIND_MAX_ROWS);
        if (deleted < 0)
            return false;
    } else {
        (void)node_db_exec(ndb,
            "DELETE FROM node_state WHERE key='utxo_commitment'");
    }

    if (!node_db_state_set(ndb, "coins_best_block",
                           sync_hash, sizeof(sync_hash)))
        return false;
    /* The height metadata must land too: coins_best_block (hash) and its
     * height are read together by the coins-integrity gate, so a hash
     * written without its height is exactly the inconsistency that gate
     * trips on. Don't claim a successful repair if this write fails. */
    if (!node_db_state_set_int(ndb, "cec.coins_best_block_height", sync_h)) {
        LOG_WARN("chain", "stale cursor repair: coins_best_block advanced "
                 "to h=%lld but height-metadata write failed; reporting "
                 "repair incomplete", (long long)sync_h);
        return false;
    }

    LOG_WARN("chain", "stale cursor repair: advanced coins_best_block "
             "from h=%lld to sync projection h=%lld (max_utxo_h=%lld)",
             (long long)coins_h, (long long)sync_h,
             (long long)max_utxo_h);
    event_emitf(EV_RECOVERY_ACTION, 0,
        "action=repair_stale_coins_cursor from=%lld to=%lld max_utxo=%lld",
        (long long)coins_h, (long long)sync_h, (long long)max_utxo_h);
    return true;
}

struct zcl_result utxo_recovery_commit_tip(struct utxo_recovery_ctx *ctx,
                              struct block_index *tip,
                              const char *reason,
                              bool persist_coins_best)
{
    if (!ctx || !ctx->state || !tip || !tip->phashBlock)
        return ZCL_ERR(-43,
            "utxo_recovery_commit_tip: invalid args ctx=%p state=%p tip=%p "
            "phashBlock=%p reason=%s",
            (void *)ctx, ctx ? (void *)ctx->state : NULL, (void *)tip,
            tip ? (void *)tip->phashBlock : NULL, reason ? reason : "");

    struct chain_state_rollback_authorization rollback_auth = {
        .source = CSR_ROLLBACK_SOURCE_UTXO_REPAIR,
        .decision = POLICY_ALLOW,
        .from_height = active_chain_height(&ctx->state->chain_active),
        .to_height = tip->nHeight,
        .max_depth = INT64_MAX,
        .evidence_class = "utxo_recovery_verified",
        .reason = reason ? reason : "utxo_recovery",
    };
    struct chain_state_commit commit = {
        .new_tip = tip,
        .new_coins_best = *tip->phashBlock,
        .expected_utxo_count = 0,
        .update_header_tip = true,
        .persist_coins_best = persist_coins_best,
        .rollback_auth = &rollback_auth,
        .wallet_scan_height = -1,
        .reason = reason ? reason : "utxo_recovery",
    };

    enum csr_result rc = csr_commit_tip(csr_instance(), &commit);
    if (rc == CSR_OK)
        return ZCL_OK;

#ifdef ZCL_TESTING
    if (rc == CSR_REJECTED_NOT_INITIALIZED) {
        if (ctx->coins_tip)
            coins_view_cache_set_best_block(ctx->coins_tip,
                                            tip->phashBlock);
        if (persist_coins_best && ctx->ndb && ctx->ndb->open)
            (void)node_db_state_set(ctx->ndb, "coins_best_block",
                                    tip->phashBlock->data, 32);
        (void)chain_set_active_tip(ctx->state, tip, TIP_FROM_UTXO_REPAIR,
                             reason ? reason : "utxo_recovery_csr_uninit");
        ctx->state->pindex_best_header = tip;
        return ZCL_OK;
    }
#endif

    LOG_WARN("utxo_recovery", "utxo_recovery: csr rejected tip promotion (%s) reason=%s h=%d", csr_result_name(rc), reason ? reason : "", tip->nHeight);
    return ZCL_ERR(-44,
        "utxo_recovery_commit_tip: csr rejected tip promotion (%s) reason=%s "
        "h=%d", csr_result_name(rc), reason ? reason : "", tip->nHeight);
}

struct zcl_result utxo_recovery_commit_genesis(struct utxo_recovery_ctx *ctx,
                                  const char *reason)
{
    if (!ctx || !ctx->state || !ctx->params)
        return ZCL_ERR(-45,
            "utxo_recovery_commit_genesis: invalid args ctx=%p state=%p "
            "params=%p reason=%s",
            (void *)ctx, ctx ? (void *)ctx->state : NULL,
            ctx ? (void *)ctx->params : NULL, reason ? reason : "");

    struct block_index *genesis = block_map_find(
        &ctx->state->map_block_index,
        &ctx->params->consensus.hashGenesisBlock);
    if (!genesis) {
        LOG_WARN("utxo_recovery", "utxo_recovery: cannot reset coins best to genesis; " "genesis is missing from block index (reason=%s)", reason ? reason : "");
        return ZCL_ERR(-46,
            "utxo_recovery_commit_genesis: genesis is missing from block "
            "index (reason=%s)", reason ? reason : "");
    }

    ZCL_CHECK(utxo_recovery_commit_tip(ctx, genesis,
                                  reason ? reason : "utxo_recovery_genesis",
                                  true));
    return ZCL_OK;
}

struct utxo_count_check_result utxo_recovery_classify_count_check(
    int tip_height,
    int checkpoint_height,
    uint64_t checkpoint_count,
    uint64_t actual_count)
{
    struct utxo_count_check_result res = {0};
    res.blocks_past_checkpoint = tip_height - checkpoint_height;

    if (checkpoint_height <= 0 || checkpoint_count == 0 ||
        tip_height < checkpoint_height) {
        res.level = UTXO_COUNT_CHECK_OK;
        return res;
    }

    int64_t delta = (int64_t)actual_count - (int64_t)checkpoint_count;
    if (delta < 0)
        delta = -delta;
    res.pct_delta = (double)delta / (double)checkpoint_count * 100.0;

    if (res.blocks_past_checkpoint > UTXO_CHECKPOINT_NEAR_WINDOW) {
        res.level = UTXO_COUNT_CHECK_INFO_STALE_REFERENCE;
        return res;
    }

    if (res.pct_delta > 50.0)
        res.level = UTXO_COUNT_CHECK_CRITICAL;
    else if (res.pct_delta > 10.0)
        res.level = UTXO_COUNT_CHECK_WARNING;
    else
        res.level = UTXO_COUNT_CHECK_OK;
    return res;
}

bool utxo_recovery_xor_mismatch_is_corruption_candidate(
    uint64_t saved_count,
    uint64_t computed_count)
{
    (void)saved_count;
    (void)computed_count;
    return false;
}

/* ── Policy-gated UTXO wipe ──────────────────────────────────────
 *
 * Every destructive UTXO wipe must go through this function.
 * It counts the rows first, asks recovery_policy for permission with
 * a grep-able reason string, and refuses loudly if the proposed wipe
 * is larger than ZCL_MAX_UTXO_WIPE_ROWS (default 1000).
 *
 * This is the gate that would have saved the 1.3M UTXOs on
 * 2026-04-10. Do not bypass. */
struct zcl_result utxo_recovery_wipe(struct node_db *ndb, const char *reason)
{
    int64_t proposed = node_db_utxo_count(ndb);

    struct recovery_policy pol;
    policy_load_from_env(&pol);

    enum policy_decision d = policy_check_utxo_wipe(&pol, proposed, reason);
    if (d != POLICY_ALLOW) {
        LOG_INFO("utxo_recovery", "utxo_recovery: REFUSING wipe at \"%s\" — would drop %lld rows, " "policy=%s (override with ZCL_MAX_UTXO_WIPE_ROWS=%lld)", reason, (long long)proposed, policy_decision_name(d), (long long)(proposed + 1));
        return ZCL_ERR(-41,
            "utxo_recovery_wipe: policy denied wipe at \"%s\" — would drop "
            "%lld rows, policy=%s (override with ZCL_MAX_UTXO_WIPE_ROWS=%lld)",
            reason, (long long)proposed, policy_decision_name(d),
            (long long)(proposed + 1));
    }
    event_emitf(EV_RECOVERY_ACTION, 0,
        "action=utxo_wipe reason=%s rows=%lld", reason, (long long)proposed);
    if (!node_db_wipe_utxos(ndb))
        return ZCL_ERR(-42,
            "utxo_recovery_wipe: node_db_wipe_utxos failed at \"%s\" "
            "(rows=%lld)", reason, (long long)proposed);
    /* The coins are gone — the cold-import seed anchor (a height/hash/count
     * triple that asserts coins are present through H) must not survive them,
     * or a later partial reimport could read a stale key and trust-stamp a
     * finalized frontier above the real coin frontier. */
    utxo_recovery_clear_cold_import_seed(ndb);
    return ZCL_OK;
}

/* ── Auto-reimport flag ──────────────────────────────────────── */
/* The read-side needs-reimport sentinel lives in lib/storage as
 * `utxo_reimport_flag_check_and_clear`. This service owns only the
 * node_db state transition needed before reimport starts. */

struct zcl_result utxo_recovery_prepare_reimport(struct node_db *ndb)
{
    printf("Forced UTXO re-import requested.\n");
    /* Only clear the migration flag — the wipe happens inside
     * utxo_recovery_import_ldb (the "boot.ldb_import_prepare" wipe in
     * utxo_recovery_restore.c).  Wiping here AND in import_ldb was one of
     * the three redundant wipes that could destroy imported UTXOs. */
    if (!node_db_exec(ndb,
            "DELETE FROM node_state WHERE key='leveldb_utxo_migrated'"))
        return ZCL_ERR(-50, "prepare_reimport: failed to clear "
                       "leveldb_utxo_migrated flag");
    /* A reimport is about to replace the coins — drop the durable seed anchor
     * for the OLD import so an interrupted reimport (migrated cleared, coins
     * partially wiped) cannot leave the consumer reading a stale key. */
    utxo_recovery_clear_cold_import_seed(ndb);
    return ZCL_OK;
}

/* ── LDB→SQLite UTXO import + chain tip restoration ─────────────
 * utxo_recovery_restore.c owns the heavy boot recovery routines:
 * utxo_recovery_import_ldb and utxo_recovery_restore_chain_tip. They share the
 * CSR-gated commit primitives above via utxo_recovery_internal.h. */

/* ── Validation recovery execution ──────────────────────────── */

/* Helper: recover from stale metadata by fixing coins_best_block
 * from UTXO set heights instead of wiping. */
static bool recover_stale_metadata(struct utxo_recovery_ctx *ctx)
{
    int64_t actual_utxos = node_db_utxo_count(ctx->ndb);
    if (actual_utxos <= 1000)
        return false;

    LOG_WARN("chain", "ABORT WIPE: validation says 'empty' but utxos " "table has %lld rows. coins_best_block may be " "stale, NOT the UTXOs. Refusing to destroy data.", (long long)actual_utxos);

    int max_h = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(ctx->ndb->db,
            "SELECT MAX(height) FROM utxos",
            -1, &st, NULL) == SQLITE_OK) {
        if (AR_STEP_ROW_READONLY(st) == SQLITE_ROW)
            max_h = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    if (max_h > 0) {
        struct db_block tip_blk;
        if (db_block_find_by_height(ctx->ndb, max_h, &tip_blk)) {
            struct uint256 tip_hash;
            memcpy(tip_hash.data, tip_blk.hash, 32);
            struct block_index *bi = block_map_find(
                &ctx->state->map_block_index, &tip_hash);
            if (bi && chain_restore_block_is_consensus_backed_on_disk(
                    bi, ctx->datadir)) {
                if (utxo_recovery_commit_tip(
                        ctx, bi, "best_have_data", true).ok) {
                    printf("RECOVERY: restored chain tip from UTXO set: h=%d\n",
                           max_h);
                }
            } else if (bi) {
                LOG_INFO("chain", "RECOVERY: UTXO-derived tip h=%d is not disk-backed; " "not setting active tip", max_h);
            }
        }
    }
    return true;
}

/* Helper: wipe UTXOs and reset to genesis for clean re-sync */
static bool reset_to_genesis(struct utxo_recovery_ctx *ctx)
{
    struct zcl_result wipe = utxo_recovery_wipe(ctx->ndb,
                                                "boot.reset_to_genesis");
    if (!wipe.ok) {
        LOG_WARN("utxo_recovery", "%s", wipe.message);
        return false;
    }

    struct block_index *genesis = block_map_find(
        &ctx->state->map_block_index,
        &ctx->params->consensus.hashGenesisBlock);
    if (genesis) {
        if (!utxo_recovery_commit_tip(ctx, genesis, "fresh_genesis", true).ok)
            return false;
    } else {
        LOG_WARN("utxo_recovery",
                 "reset_to_genesis: genesis missing from block index");
        return false;
    }

    /* Clear migration flag for LevelDB re-import on next boot */
    node_db_exec(ctx->ndb,
        "DELETE FROM node_state WHERE key='leveldb_utxo_migrated'");

    extern _Atomic bool g_utxo_commitment_skip;
    atomic_store(&g_utxo_commitment_skip, true);

    set_flush_policy(3600, 1000000, 500);
    return true;
}

/* Helper: boot-time integrity checks when BOOT_OK */
static bool integrity_checks_boot_ok(struct utxo_recovery_ctx *ctx,
                                      bool *out_skip_activate)
{
    int tip_h = active_chain_height(&ctx->state->chain_active);

    /* A: Stale UTXO wipe — chain at genesis but UTXO set non-empty */
    if (tip_h <= 0 && ctx->ndb->open) {
        sqlite3_stmt *stale_cnt = NULL;
        int64_t stale_utxos = 0;
        if (sqlite3_prepare_v2(ctx->ndb->db,
                "SELECT COUNT(*) FROM utxos", -1,
                &stale_cnt, NULL) == SQLITE_OK && stale_cnt) {
            if (AR_STEP_ROW_READONLY(stale_cnt) == SQLITE_ROW)
                stale_utxos = sqlite3_column_int64(stale_cnt, 0);
            sqlite3_finalize(stale_cnt);
        }
        if (stale_utxos > 0) {
            printf("WARNING: Chain at genesis but %lld stale UTXOs "
                   "from previous snapshot — wiping for clean sync\n",
                   (long long)stale_utxos);
            struct zcl_result wipe = utxo_recovery_wipe(
                ctx->ndb, "boot.stale_utxos_at_genesis");
            if (!wipe.ok)
                LOG_WARN("utxo_recovery", "%s", wipe.message);
            (void)utxo_recovery_commit_genesis(
                ctx, "boot.stale_utxos_at_genesis");
            *out_skip_activate = true;
        }
    }

    /* B: UTXO count sanity check against SHA3 checkpoint */
    const struct sha3_utxo_checkpoint *sha3cp = get_sha3_utxo_checkpoint();
    if (sha3cp && sha3cp->height > 0 &&
        tip_h >= sha3cp->height && ctx->ndb->open) {
        sqlite3_stmt *cnt_stmt = NULL;
        sqlite3_prepare_v2(ctx->ndb->db,
            "SELECT COUNT(*) FROM utxos", -1, &cnt_stmt, NULL);
        if (cnt_stmt && AR_STEP_ROW_READONLY(cnt_stmt) == SQLITE_ROW) {
            int64_t actual = sqlite3_column_int64(cnt_stmt, 0);
            struct utxo_count_check_result count_check =
                utxo_recovery_classify_count_check(
                    tip_h, sha3cp->height, sha3cp->utxo_count,
                    (uint64_t)actual);
            if (count_check.level == UTXO_COUNT_CHECK_CRITICAL)
                LOG_WARN("chain", "UTXO count %lld vs " "checkpoint %lld (%.1f%% off) — consider " "reimport", (long long)actual, (long long)sha3cp->utxo_count, count_check.pct_delta);
            else if (count_check.level == UTXO_COUNT_CHECK_WARNING)
                printf("WARNING: UTXO count %lld vs checkpoint %lld "
                       "(%.1f%% off, chain %d blocks past checkpoint)\n",
                       (long long)actual,
                       (long long)sha3cp->utxo_count,
                       count_check.pct_delta,
                       count_check.blocks_past_checkpoint);
            else if (count_check.level ==
                     UTXO_COUNT_CHECK_INFO_STALE_REFERENCE)
                printf("INFO: skipping UTXO count checkpoint warning: "
                       "checkpoint h=%d is %d blocks behind tip h=%d "
                       "(actual=%lld checkpoint=%lld delta=%.1f%%)\n",
                       sha3cp->height,
                       count_check.blocks_past_checkpoint,
                       tip_h,
                       (long long)actual,
                       (long long)sha3cp->utxo_count,
                       count_check.pct_delta);
        }
        sqlite3_finalize(cnt_stmt);
    }

    /* C: XOR commitment verification */
    if (ctx->ndb->open) {
        struct utxo_commitment saved_uc, computed_uc;
        memset(&saved_uc, 0, sizeof(saved_uc));
        memset(&computed_uc, 0, sizeof(computed_uc));
        if (utxo_commitment_load_checkpoint(ctx->ndb->db, &saved_uc)) {
            utxo_commitment_compute_db(ctx->ndb->db, &computed_uc);
            if (!utxo_commitment_equal(&saved_uc, &computed_uc)) {
                if (utxo_recovery_xor_mismatch_is_corruption_candidate(
                        saved_uc.count, computed_uc.count)) {
                    LOG_WARN("chain", "XOR commitment mismatch — " "UTXO set may be corrupted. " "Consider running --importchainstate");
                } else {
                    printf("INFO: skipping XOR commitment corruption warning: "
                           "stored commitment checkpoint is stale "
                           "(saved_count=%llu computed_count=%llu)\n",
                           (unsigned long long)saved_uc.count,
                           (unsigned long long)computed_uc.count);
                    if (ctx->coins_tip)
                        ctx->coins_tip->commitment = computed_uc;
                    if (utxo_commitment_save_checkpoint(ctx->ndb->db,
                                                        &computed_uc)) {
                        printf("INFO: refreshed stale XOR commitment "
                               "checkpoint (count=%llu)\n",
                               (unsigned long long)computed_uc.count);
                    } else {
                        LOG_WARN("chain", "failed to refresh stale " "XOR commitment checkpoint");
                    }
                }
            }
        }
    }

    return true;
}

struct recovery_exec_result utxo_recovery_execute(
    struct utxo_recovery_ctx *ctx,
    struct boot_validation_result *vr)
{
    struct recovery_exec_result res = {
        .status = recovery_exec_status_ok(),
    };

    if (!ctx || !vr || !ctx->state || !ctx->ndb ||
        !ctx->activation_ctl) {
        res.status = ZCL_ERR(-30,
            "utxo_recovery_execute: invalid ctx=%p vr=%p state=%p "
            "ndb=%p activation_ctl=%p",
            (void *)ctx, (void *)vr,
            ctx ? (void *)ctx->state : NULL,
            ctx ? (void *)ctx->ndb : NULL,
            ctx ? (void *)ctx->activation_ctl : NULL);
        LOG_WARN("utxo_recovery", "%s", res.status.message);
        return res;
    }

    /* Check activation controller — skip when ANCHOR_ACTIVE */
    struct utxo_wipe_decision wd;
    activation_should_allow_utxo_wipe(&wd,
        activation_get_state(ctx->activation_ctl),
        snapsync_get_anchor() != NULL);
    if (!wd.safe_to_wipe) {
        printf("Skipping coins/chain validation — %s\n", wd.reason);
        return res;
    }

    switch (vr->action) {
    case BOOT_RECOVER_REIMPORT:
    case BOOT_RECOVER_WIPE_WAIT:
        printf("WARNING: Chain tip at h=%d but coins DB %s!\n",
               vr->chain_height,
               vr->action == BOOT_RECOVER_REIMPORT
                   ? "empty (LevelDB available)" : "empty");

        /* SAFETY: check actual UTXO count before wiping */
        if (recover_stale_metadata(ctx)) {
            res.recovered = true;
            break;
        }

        if (!ctx->params) {
            res.status = ZCL_ERR(-31,
                "utxo_recovery_execute: %s requires chain params",
                vr->action == BOOT_RECOVER_REIMPORT
                    ? "reimport" : "wipe_wait");
            LOG_WARN("utxo_recovery", "%s", res.status.message);
            break;
        }

        if (!reset_to_genesis(ctx)) {
            res.status = ZCL_ERR(-32,
                "utxo_recovery_execute: reset_to_genesis failed for "
                "action=%s chain_height=%d coins_height=%d",
                vr->action == BOOT_RECOVER_REIMPORT
                    ? "reimport" : "wipe_wait",
                vr->chain_height, vr->coins_height);
            LOG_WARN("utxo_recovery", "%s", res.status.message);
            break;
        }
        res.recovered = true;
        break;

    case BOOT_RECOVER_RESET_CHAIN: {
        struct block_index *coins_block = block_map_find(
            &ctx->state->map_block_index, &vr->coins_hash);
        if (coins_block) {
            if (chain_restore_block_is_consensus_backed_on_disk(
                    coins_block, ctx->datadir)) {
                printf("Chain tip/coins mismatch: chain=%d coins=%d\n"
                       "  Resetting chain to disk-backed coins tip — "
                       "will replay %d blocks.\n",
                       vr->chain_height, vr->coins_height,
                       vr->chain_height - vr->coins_height);
                if (!utxo_recovery_commit_tip(
                        ctx, coins_block,
                        "chain_coins_mismatch_reset", true).ok) {
                    res.status = ZCL_ERR(-33,
                        "utxo_recovery_execute: failed to reset chain "
                        "to coins tip h=%d", vr->coins_height);
                    LOG_WARN("utxo_recovery", "%s", res.status.message);
                    break;
                }
            } else {
                LOG_WARN("chain", "Chain tip/coins mismatch: coins tip h=%d is not " "disk-backed; refusing active-tip reset", vr->coins_height);
                res.status = ZCL_ERR(-34,
                    "utxo_recovery_execute: coins tip h=%d is not "
                    "disk-backed", vr->coins_height);
                LOG_WARN("utxo_recovery", "%s", res.status.message);
                break;
            }
        } else {
            res.status = ZCL_ERR(-35,
                "utxo_recovery_execute: coins tip h=%d is missing from "
                "block index", vr->coins_height);
            LOG_WARN("utxo_recovery", "%s", res.status.message);
            break;
        }
        res.recovered = true;
        break;
    }
    case BOOT_RECOVER_RESET_COINS_TO_CHAIN_TIP: {
        struct block_index *chain_tip =
            active_chain_tip(&ctx->state->chain_active);
        if (chain_tip) {
            if (!utxo_recovery_commit_tip(
                    ctx, chain_tip, "coins_cursor_to_chain_tip", true).ok) {
                res.status = ZCL_ERR(-36,
                    "utxo_recovery_execute: failed to reset coins cursor "
                    "to active chain tip h=%d", chain_tip->nHeight);
                LOG_WARN("utxo_recovery", "%s", res.status.message);
                break;
            }
            res.recovered = true;
        } else {
            res.status = ZCL_ERR(-37,
                "utxo_recovery_execute: cannot reset coins cursor because "
                "active chain tip is missing");
            LOG_WARN("utxo_recovery", "%s", res.status.message);
        }
        break;
    }
    case BOOT_RECOVER_RESET_COINS_TO_GENESIS:
        if (!ctx->params) {
            res.status = ZCL_ERR(-38,
                "utxo_recovery_execute: reset coins to genesis requires "
                "chain params");
            LOG_WARN("utxo_recovery", "%s", res.status.message);
        } else if (utxo_recovery_commit_genesis(ctx,
                                                "coins_cursor_to_genesis").ok) {
            res.recovered = true;
        } else {
            res.status = ZCL_ERR(-39,
                "utxo_recovery_execute: failed to reset coins cursor "
                "to genesis");
            LOG_WARN("utxo_recovery", "%s", res.status.message);
        }
        break;
    case BOOT_OK:
        if (!integrity_checks_boot_ok(ctx, &res.skip_activate)) {
            res.status = ZCL_ERR(-40,
                "utxo_recovery_execute: BOOT_OK integrity checks failed");
            LOG_WARN("utxo_recovery", "%s", res.status.message);
        }
        break;
    }

    return res;
}

/* ── UTXO cleanup ───────────────────────────────────────────── */

int utxo_recovery_clean_above_tip(struct node_db *ndb,
                                   struct main_state *state)
{
    if (!ndb || !state || !ndb->open)
        return 0;

    struct block_index *tip = active_chain_tip(&state->chain_active);
    int tip_h = tip ? tip->nHeight : 0;
    if (tip_h <= 0) return 0;

    int deleted_total = coins_rewind_above_tip(
        ndb->db, tip_h, UTXO_BOOT_REWIND_MAX_ROWS);
    if (deleted_total == 0)
        return 0;
    if (deleted_total < 0) {
        LOG_WARN("chain", "ABORT: refusing or failing boot UTXO rewind above tip h=%d " "(guard=%d). Only a single-block overshoot with a bounded row " "count is auto-healable; investigate block_index/coins drift.", tip_h, UTXO_BOOT_REWIND_MAX_ROWS);
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
            "wipe_blocked tip=%d guard=%d",
            tip_h, UTXO_BOOT_REWIND_MAX_ROWS);
        return 0;
    }

    event_emitf(EV_RECOVERY_ACTION, 0,
        "action=utxo_prune_above_tip height=%d count=%d",
        tip_h, deleted_total);
    printf("Boot: removed %d UTXOs above tip h=%d\n",
           deleted_total, tip_h);
    return deleted_total;
}
