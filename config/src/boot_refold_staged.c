/* boot_refold_staged.c — the -refold-staged reset, extracted from boot.c to
 * hold the E1 file-size ceiling. Resets the staged reducer's durable derived
 * state to genesis so the staged pipeline re-folds forward over on-disk block
 * BODIES. Contract declared in config/boot.h. */
#include "config/boot.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>          /* EXIT_FAILURE */
#include <string.h>
#include <unistd.h>          /* _exit */
#include <sqlite3.h>

#include "models/database.h"
#include "storage/progress_store.h"
#include "storage/coins_kv.h"
#include "config/boot_internal.h"        /* boot_index_clear_coins_state */
#include "jobs/reducer_frontier.h"       /* progress_meta_delete_in_tx,
                                          * REDUCER_TRUSTED_BASE_*_KEY,
                                          * REDUCER_FRONTIER_TRUSTED_ANCHOR,
                                          * progress_store_tx_lock/unlock */
#include "jobs/stage_repair_internal.h"  /* stage_repair_force_stage_cursor */
#include "jobs/refold_progress.h"        /* refold_progress_boot_init,
                                          * refold_progress_mark_started_from_anchor */
#include "jobs/tip_finalize_stage.h"     /* tip_finalize_stage_seed_anchor */
#include "chain/checkpoints.h"           /* get_sha3_utxo_checkpoint */
#include "event/event.h"                 /* event_emitf, EV_BOOT_VALIDATION_FAILED */
#include "services/block_index_loader.h" /* block_index_loader_torn_import_detect (B2 1c) */
#include "validation/main_state.h"       /* struct main_state */
#include "validation/chainstate.h"       /* active_chain_height */
#include "util/log_macros.h"

/* Declared in app/services/src/utxo_recovery_internal.h (src-private); forward
 * declare for the -refold-staged seed-provenance clear. */
void utxo_recovery_clear_cold_import_seed(struct node_db *ndb);

void boot_refold_staged_init(bool refold_staged)
{
    (void)refold_progress_boot_init(progress_store_db(), refold_staged);
}

void boot_refold_staged_reset(struct node_db *ndb)
{
    sqlite3 *rpdb = progress_store_db();
    if (!rpdb) {
        fprintf(stderr, "[boot] -refold-staged: progress store not open; skip\n");
        return;
    }

    /* Neutralize the cold-import seed provenance so the later stage init
     * (block_index_loader_seed_stages_from_cold_import) does not re-stamp the
     * trusted anchor forward to the checkpoint. */
    utxo_recovery_clear_cold_import_seed(ndb);
    (void)node_db_state_set(ndb, "leveldb_utxo_migrated", NULL, 0);

    /* Phase 1 (own transactions): truncate the coin set — do NOT reseed from
     * the wrong-fork node.db mirror; clear the node.db mirror + commitment
     * keys + header_admit_log + sapling tree. */
    (void)coins_kv_reset_for_reseed(rpdb);
    (void)boot_index_clear_coins_state(ndb);
    (void)node_db_exec(ndb, "DELETE FROM header_admit_log");
    (void)node_db_state_set(ndb, "sapling_tree", NULL, 0);
    (void)node_db_state_set(ndb, "sapling_tree_rescan_height", NULL, 0);

    /* Phase 2 (one progress.kv transaction): clear the reducer-derived tables,
     * force the 8 stage cursors to genesis, drop the trusted-base declaration.
     * stage_repair_force_stage_cursor REQUIRES the caller hold the progress tx
     * lock + an open transaction. */
    static const char *const k_refold_tables[] = {
        "validate_headers_log", "body_fetch_log", "body_persist_log",
        "script_validate_log", "proof_validate_log", "utxo_apply_log",
        "tip_finalize_log", "utxo_apply_delta", "nullifiers",
        "created_outputs",
    };
    static const char *const k_refold_stages[] = {
        "header_admit", "validate_headers", "body_fetch", "body_persist",
        "script_validate", "proof_validate", "utxo_apply", "tip_finalize",
    };
    bool refold_ok = true;
    char *refold_err = NULL;
    progress_store_tx_lock();
    if (sqlite3_exec(rpdb, "BEGIN IMMEDIATE", NULL, NULL, &refold_err)
        != SQLITE_OK)
        refold_ok = false;
    for (size_t i = 0;
         refold_ok && i < sizeof(k_refold_tables) / sizeof(k_refold_tables[0]);
         i++) {
        char dsql[96];
        snprintf(dsql, sizeof dsql, "DELETE FROM %s", k_refold_tables[i]);
        if (sqlite3_exec(rpdb, dsql, NULL, NULL, &refold_err) != SQLITE_OK) {
            /* A table may not exist yet on a given datadir — tolerate "no such
             * table", fail loud on any other error. */
            if (refold_err && strstr(refold_err, "no such table")) {
                sqlite3_free(refold_err);
                refold_err = NULL;
            } else {
                refold_ok = false;
            }
        }
    }
    for (size_t i = 0;
         refold_ok && i < sizeof(k_refold_stages) / sizeof(k_refold_stages[0]);
         i++)
        if (!stage_repair_force_stage_cursor(rpdb, k_refold_stages[i], 0))
            refold_ok = false;
    if (refold_ok && !coins_kv_set_applied_height_in_tx(rpdb, 0))
        refold_ok = false;
    if (refold_ok) {
        (void)progress_meta_delete_in_tx(rpdb, REDUCER_TRUSTED_BASE_HEIGHT_KEY);
        (void)progress_meta_delete_in_tx(rpdb, REDUCER_TRUSTED_BASE_HASH_KEY);
    }
    if (refold_ok && sqlite3_exec(rpdb, "COMMIT", NULL, NULL, &refold_err)
        != SQLITE_OK)
        refold_ok = false;
    if (!refold_ok)
        sqlite3_exec(rpdb, "ROLLBACK", NULL, NULL, NULL);
    if (refold_err)
        sqlite3_free(refold_err);
    progress_store_tx_unlock();

    fprintf(stderr,
            "[boot] -refold-staged: staged reducer reset to genesis %s; "
            "the staged pipeline re-folds forward over on-disk bodies "
            "(H* climbs as the logs fill)\n",
            refold_ok ? "OK" : "FAILED");
}

/* B2 — the -refold-from-anchor reset (config/boot.h). Sibling of
 * boot_refold_staged_reset EXCEPT:
 *   (i)   coins_kv is FULL-reset (coins_kv_reset_for_reseed) — a height-bounded
 *         DELETE cannot restore spent-below-anchor coins, so we re-seed the
 *         WHOLE SHA3-verified anchor set from node.db's `utxos` mirror.
 *   (ii)  HARD-ASSERT the re-seeded set: coins_kv_commitment == the compiled
 *         checkpoint sha3_hash AND coins_kv_count == checkpoint utxo_count.
 *         On mismatch this FATALs (rolls back the cursor txn, pages
 *         EV_BOOT_VALIDATION_FAILED, _exit(EXIT_FAILURE)) — never advances on an
 *         unproven anchor set.
 *   (iii) the 8 stage cursors are forced to the ANCHOR (3,056,758), NOT genesis,
 *         coins_applied_height = anchor+1 (the utxo_apply next-height cursor
 *         convention) so the fold resumes AT the anchor.
 *   (iv)  tip_finalize_stage_seed_anchor(anchor, checkpoint block_hash, true) so
 *         the finalize served-tip prefix starts AT the anchor.
 *
 * The fold then climbs anchor -> active tip over on-disk BODIES, running the
 * REAL script_validate/proof_validate/utxo_apply/tip_finalize stages. Marks
 * refold_from_anchor (progress.kv) so the L0 floor HOLDS at the anchor (not 0)
 * and the below-anchor self-repair is suspended until utxo_apply reaches the
 * resume target. */
void boot_refold_from_anchor_reset(struct node_db *ndb)
{
    sqlite3 *rpdb = progress_store_db();
    if (!rpdb) {
        fprintf(stderr,
                "[boot] -refold-from-anchor: progress store not open; skip\n");
        return;
    }

    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    if (!cp) {
        fprintf(stderr, "FATAL: -refold-from-anchor: no compiled SHA3 UTXO "
                "checkpoint — cannot anchor the refold\n");
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "refold_from_anchor no_compiled_checkpoint");
        _exit(EXIT_FAILURE);
    }
    const int32_t anchor = cp->height;

    /* Neutralize the cold-import seed provenance so the later stage init
     * (block_index_loader_seed_stages_from_cold_import) does not re-stamp the
     * trusted anchor forward to the checkpoint. */
    utxo_recovery_clear_cold_import_seed(ndb);
    (void)node_db_state_set(ndb, "leveldb_utxo_migrated", NULL, 0);

    /* Phase 1 (own transactions): FULL coins_kv reset, then RE-SEED the anchor
     * set from node.db's `utxos` mirror (the SHA3-verified cold-import set).
     * coins_kv_reset_for_reseed clears the migration stamp so the seed copies
     * fresh. Clear the node.db mirror commitment keys + header_admit_log +
     * sapling tree exactly like the from-genesis reset. */
    bool reseed_ok = coins_kv_reset_for_reseed(rpdb);
    if (reseed_ok)
        reseed_ok = coins_kv_seed_from_node_db(
            rpdb, sqlite3_db_filename(ndb->db, "main"));
    (void)boot_index_clear_coins_state(ndb);
    (void)node_db_exec(ndb, "DELETE FROM header_admit_log");
    (void)node_db_state_set(ndb, "sapling_tree", NULL, 0);
    (void)node_db_state_set(ndb, "sapling_tree_rescan_height", NULL, 0);

    /* (ii) HARD-ASSERT the re-seeded anchor set against the compiled checkpoint
     * BEFORE arming any cursor. A height-correct/state-wrong set must FATAL
     * here — never advance the fold on an unproven base. (pattern from
     * utxo_recovery_restore.c:298-322.) */
    uint8_t got_root[32] = {0};
    int crc = reseed_ok ? coins_kv_commitment(rpdb, got_root) : -1;
    int64_t got_count = reseed_ok ? coins_kv_count(rpdb) : -1;
    bool anchor_proven = reseed_ok && crc == 0 &&
                         memcmp(got_root, cp->sha3_hash, 32) == 0 &&
                         got_count == (int64_t)cp->utxo_count;
    if (!anchor_proven) {
        fprintf(stderr,
                "FATAL: -refold-from-anchor: re-seeded anchor set FAILED the "
                "SHA3/count check (count=%lld want=%llu, commitment_match=%d, "
                "reseed_ok=%d) — refusing to fold from an unproven anchor\n",
                (long long)got_count, (unsigned long long)cp->utxo_count,
                (crc == 0 && memcmp(got_root, cp->sha3_hash, 32) == 0) ? 1 : 0,
                reseed_ok ? 1 : 0);
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "check=refold_from_anchor anchor_h=%d reseed anchor set "
                    "mismatch (count=%lld want=%llu) — the node.db `utxos` "
                    "mirror does not reproduce the compiled checkpoint; ACTION: "
                    "wipe + re-import with the two-step cold-sync recipe",
                    anchor, (long long)got_count,
                    (unsigned long long)cp->utxo_count);
        _exit(EXIT_FAILURE);
    }

    /* Phase 2 (one progress.kv transaction): clear the reducer-derived tables,
     * force the 8 stage cursors to the ANCHOR (NOT genesis), set the applied
     * frontier to anchor+1, drop the trusted-base declaration.
     * stage_repair_force_stage_cursor REQUIRES the caller hold the progress tx
     * lock + an open transaction. */
    static const char *const k_refold_tables[] = {
        "validate_headers_log", "body_fetch_log", "body_persist_log",
        "script_validate_log", "proof_validate_log", "utxo_apply_log",
        "tip_finalize_log", "utxo_apply_delta", "nullifiers",
        "created_outputs",
    };
    static const char *const k_refold_stages[] = {
        "header_admit", "validate_headers", "body_fetch", "body_persist",
        "script_validate", "proof_validate", "utxo_apply", "tip_finalize",
    };
    bool refold_ok = true;
    char *refold_err = NULL;
    progress_store_tx_lock();
    if (sqlite3_exec(rpdb, "BEGIN IMMEDIATE", NULL, NULL, &refold_err)
        != SQLITE_OK)
        refold_ok = false;
    for (size_t i = 0;
         refold_ok && i < sizeof(k_refold_tables) / sizeof(k_refold_tables[0]);
         i++) {
        char dsql[96];
        snprintf(dsql, sizeof dsql, "DELETE FROM %s", k_refold_tables[i]);
        if (sqlite3_exec(rpdb, dsql, NULL, NULL, &refold_err) != SQLITE_OK) {
            if (refold_err && strstr(refold_err, "no such table")) {
                sqlite3_free(refold_err);
                refold_err = NULL;
            } else {
                refold_ok = false;
            }
        }
    }
    for (size_t i = 0;
         refold_ok && i < sizeof(k_refold_stages) / sizeof(k_refold_stages[0]);
         i++)
        if (!stage_repair_force_stage_cursor(rpdb, k_refold_stages[i], anchor))
            refold_ok = false;
    /* utxo_apply's next-height cursor convention: applied-through `anchor`
     * means the frontier value is anchor+1. */
    if (refold_ok && !coins_kv_set_applied_height_in_tx(rpdb, anchor + 1))
        refold_ok = false;
    if (refold_ok) {
        (void)progress_meta_delete_in_tx(rpdb, REDUCER_TRUSTED_BASE_HEIGHT_KEY);
        (void)progress_meta_delete_in_tx(rpdb, REDUCER_TRUSTED_BASE_HASH_KEY);
    }
    if (refold_ok && sqlite3_exec(rpdb, "COMMIT", NULL, NULL, &refold_err)
        != SQLITE_OK)
        refold_ok = false;
    if (!refold_ok)
        sqlite3_exec(rpdb, "ROLLBACK", NULL, NULL, NULL);
    if (refold_err)
        sqlite3_free(refold_err);
    progress_store_tx_unlock();

    if (!refold_ok) {
        fprintf(stderr,
                "FATAL: -refold-from-anchor: failed to force the 8 stage "
                "cursors to the anchor (h=%d) — refusing to start a half-armed "
                "fold\n", anchor);
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "check=refold_from_anchor anchor_h=%d cursor arm failed",
                    anchor);
        _exit(EXIT_FAILURE);
    }

    /* (iv) Seed the tip_finalize served-tip prefix AT the anchor (trusted seed —
     * the SHA3-verified base, same trust model snapshot_apply.c uses). */
    if (!tip_finalize_stage_seed_anchor(anchor, cp->block_hash, true))
        LOG_WARN("boot", "[boot] -refold-from-anchor: tip_finalize anchor seed "
                 "returned false (stage not yet wired?) — the runtime authority "
                 "re-seeds from the coins tip");

    fprintf(stderr,
            "[boot] -refold-from-anchor: anchor coin set RE-SEEDED + verified "
            "(count=%llu, SHA3 OK) at h=%d; the 8 stage cursors forced to the "
            "anchor; the staged pipeline re-folds forward over on-disk bodies "
            "(H* climbs from the anchor as the logs fill)\n",
            (unsigned long long)cp->utxo_count, anchor);
}

/* B2 1c — the boot torn-import AUTO-ARM. Consults the PURE detect predicate
 * (block_index_loader_torn_import_detect — no side-effects) and, on a detected
 * tear, ARMS a from-anchor refold (reset 1a + mark 1b) so the node REPAIRS by
 * re-folding the proven anchor set forward instead of dead-ending at an operator
 * page. Returns true iff a from-anchor refold is (or is already) armed — the
 * caller then SKIPS block_index_loader_seed_stages_from_cold_import.
 *
 * IDEMPOTENT: if a from-anchor refold is already in progress (e.g. the explicit
 * -refold-from-anchor flag armed it at boot.c, or a prior crashed boot left the
 * durable key), it returns true WITHOUT re-resetting.
 *
 * FAIL-LOUD: boot_refold_from_anchor_reset _exit()s (FATAL) if the re-seeded
 * anchor set fails the SHA3/count assert — a genuinely-unrecoverable tear thus
 * stops here. When this returns FALSE (no tear detected), the caller proceeds to
 * the existing seed path, whose torn-import gate
 * (block_index_loader_torn_import_gate_fires) remains the EV_OPERATOR_NEEDED +
 * BLOCKER_PERMANENT fallback.
 *
 * NORMAL-BOOT INVARIANT: gated at the call site on ctx->refold_from_anchor; a
 * normal boot never calls this, so it is byte-identical to today. */
bool boot_refold_from_anchor_arm_if_torn(struct main_state *ms,
                                         struct node_db *ndb,
                                         struct sqlite3 *progress_db)
{
    if (!ms || !ndb || !progress_db)
        return false;

    /* Already armed (explicit flag at boot.c, or a mid-fold restart): the
     * durable from-anchor signal is live — do NOT re-reset, just take over. */
    if (refold_from_anchor_active())
        return true;

    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    if (!cp)
        return false;  /* no checkpoint to anchor against — defer to the gate */
    const int32_t checkpoint = cp->height;

    int32_t hole_h = -1;
    int32_t ceiling = -1;
    if (!block_index_loader_torn_import_detect(ms, progress_db, checkpoint,
                                               &hole_h, &ceiling))
        return false;  /* no durable tear → caller runs the normal seed path */

    LOG_WARN("boot",
             "[boot] from-anchor auto-arm: torn cold-import detected (durable "
             "unresolved prevout at h=%d, frontier=%d) — arming a from-anchor "
             "refold (re-seed the SHA3 anchor set + fold forward) instead of "
             "halting; the anchor set is HARD-ASSERTED next (FATAL on mismatch)",
             (int)hole_h, (int)ceiling);

    /* Reset FATALs internally if the re-seeded anchor set fails the SHA3/count
     * assert (a genuinely-unrecoverable tear). On return the anchor set is
     * PROVEN, so mark the from-anchor signal + resume target (the active tip the
     * fold climbs to). */
    boot_refold_from_anchor_reset(ndb);
    int32_t resume_target = (int32_t)active_chain_height(&ms->chain_active);
    (void)refold_progress_mark_started_from_anchor(progress_db, resume_target);
    return true;
}
