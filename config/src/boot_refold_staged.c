/* boot_refold_staged.c — the -refold-staged reset, in its own file separate
 * from boot.c so each file keeps one focused responsibility. Resets the staged
 * reducer's durable derived state to genesis so the staged pipeline re-folds
 * forward over on-disk block BODIES. Contract declared in config/boot.h. */
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
#include "jobs/mint_fold_ceiling.h"      /* mint_fold_ceiling_set (-mint-anchor) */
#include "jobs/mint_skip_crypto.h"       /* mint_skip_crypto_set (-mint-anchor-fast) */
#include "jobs/refold_progress.h"        /* refold_progress_boot_init,
                                          * refold_progress_mark_started,
                                          * refold_progress_mark_started_from_anchor */
#include "jobs/tip_finalize_stage.h"     /* tip_finalize_stage_seed_anchor */
#include "chain/checkpoints.h"           /* get_sha3_utxo_checkpoint */
#include "chain/utxo_snapshot_loader.h"  /* uss_open/uss_iter/uss_close (mint) */
#include "event/event.h"                 /* event_emitf, EV_BOOT_VALIDATION_FAILED */
#include "services/block_index_loader.h" /* block_index_loader_torn_import_detect (B2 1c) */
#include "validation/main_state.h"       /* struct main_state */
#include "validation/chainstate.h"       /* active_chain_height, active_chain_at */
#include "chain/chain.h"                  /* struct block_index (hashBlock) */
#include "controllers/sync_controller.h" /* sapling_tree_rebuild (re-seed tree) */
#include "util/util.h"                   /* GetDataDir */
#include "util/blocker.h"                 /* refold.body_gap named blocker */
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

/* ── B2 anchor-SET SOURCE: the minted snapshot ──────────────────────────────
 *
 * Derive the anchor-snapshot path the SAME way the -mint-anchor driver wrote it
 * (boot_mint_anchor.c): $ZCL_MINT_ANCHOR_OUT, else <datadir>/utxo-anchor.snapshot,
 * where <datadir> is node.db's directory. */
static bool mint_snapshot_path(struct node_db *ndb, char *buf, size_t cap)
{
    const char *env_out = getenv("ZCL_MINT_ANCHOR_OUT");
    if (env_out && env_out[0]) {
        int n = snprintf(buf, cap, "%s", env_out);
        return n > 0 && (size_t)n < cap;
    }
    const char *dbpath = sqlite3_db_filename(ndb->db, "main");
    if (!dbpath || !dbpath[0]) return false;
    /* Strip the trailing "/node.db" to get the datadir. */
    char dir[1024];
    int n = snprintf(dir, sizeof(dir), "%s", dbpath);
    if (n <= 0 || (size_t)n >= sizeof(dir)) return false;
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0'; else snprintf(dir, sizeof(dir), ".");
    int m = snprintf(buf, cap, "%s/utxo-anchor.snapshot", dir);
    return m > 0 && (size_t)m < cap;
}

/* Read-only probe: is a SHA3-verified anchor snapshot reachable for THIS
 * checkpoint? True iff a file exists at mint_snapshot_path AND uss_open verifies
 * its full body SHA3 against cp->sha3_hash (verify_full_sha3=true binds the
 * compiled checkpoint root) AND hdr.count == cp->utxo_count. This is the EXACT
 * "usable verified snapshot" predicate boot_anchor_seed_from_snapshot
 * (lines below) and boot_load_verify_snapshot_eligible apply before they trust a
 * snapshot — factored here so the from-anchor AUTO-ARM can DECLINE (instead of
 * falling into the node.db reseed + FATAL) when no verified snapshot exists. No
 * coins_kv mutation: mmaps read-only, closes immediately. */
static bool anchor_snapshot_verified_reachable(struct node_db *ndb,
                                               const struct sha3_utxo_checkpoint *cp)
{
    if (!ndb || !cp)
        return false;
    char path[1100];
    if (!mint_snapshot_path(ndb, path, sizeof(path)))
        return false;
    char err[256] = {0};
    struct uss_header hdr;
    struct uss_handle *h = uss_open(path, /*verify_full_sha3=*/true,
                                    cp->sha3_hash, &hdr, err, sizeof(err));
    if (!h)
        return false;  /* absent OR header/body SHA3 mismatch → not reachable */
    bool ok = (hdr.count == cp->utxo_count);
    uss_close(h);
    return ok;
}

/* uss_iter callback: insert one snapshot record into coins_kv. The caller holds
 * the progress.kv handle in an open BEGIN IMMEDIATE so every coins_kv_add
 * commits atomically with the rest. Stops the iteration (returns false) on a
 * coins_kv_add failure so the caller can ROLLBACK. */
struct mint_load_ctx {
    sqlite3 *pdb;
    uint64_t inserted;
    bool     failed;
};
static bool mint_load_record_cb(const struct uss_record *r, void *vctx)
{
    struct mint_load_ctx *c = vctx;
    if (!coins_kv_add(c->pdb, r->txid, r->vout, r->value,
                      (int32_t)r->height, r->is_coinbase != 0,
                      r->script, (size_t)r->script_len)) {
        c->failed = true;
        return false;
    }
    c->inserted++;
    return true;
}

/* Re-seed coins_kv from the MINTED, SHA3-committed anchor snapshot (the artifact
 * the -mint-anchor ceremony produced). uss_open verifies the body SHA3 AND binds
 * it to the compiled checkpoint root (expected_sha3 = cp->sha3_hash), so a loaded
 * set is ALREADY proven against the checkpoint before a single coin lands.
 * Returns true iff the snapshot was present, verified, and fully loaded into
 * coins_kv; false (with *present telling whether a file existed at all) when no
 * snapshot is available or it failed verification — the caller then falls back to
 * the node.db re-seed (which the hard-assert still guards). */
static bool boot_anchor_seed_from_snapshot(struct node_db *ndb, sqlite3 *rpdb,
                                           const struct sha3_utxo_checkpoint *cp,
                                           bool *present)
{
    if (present) *present = false;
    char path[1100];
    if (!mint_snapshot_path(ndb, path, sizeof(path)))
        return false;

    char err[256] = {0};
    struct uss_header hdr;
    struct uss_handle *h = uss_open(path, /*verify_full_sha3=*/true,
                                    cp->sha3_hash, &hdr, err, sizeof(err));
    if (!h)
        return false;  /* absent or failed verify → fall back to node.db reseed */
    if (present) *present = true;

    if (hdr.count != cp->utxo_count) {
        LOG_WARN("boot", "[boot] -refold-from-anchor: snapshot %s count=%llu != "
                 "checkpoint %llu — ignoring the artifact", path,
                 (unsigned long long)hdr.count,
                 (unsigned long long)cp->utxo_count);
        uss_close(h);
        return false;
    }

    /* Bulk-load under ONE transaction so a crash mid-load rolls back cleanly. */
    if (!coins_kv_reset_for_reseed(rpdb)) {
        uss_close(h);
        return false;
    }
    char *terr = NULL;
    if (sqlite3_exec(rpdb, "BEGIN IMMEDIATE", NULL, NULL, &terr) != SQLITE_OK) {
        if (terr) sqlite3_free(terr);
        uss_close(h);
        return false;
    }
    struct mint_load_ctx lc = { .pdb = rpdb };
    int64_t emitted = uss_iter(h, mint_load_record_cb, &lc);
    bool ok = !lc.failed && emitted == (int64_t)hdr.count;
    /* Stamp the migration-complete key inside the SAME txn (parity with the
     * node.db reseed path): coins_kv provably holds the live anchor set. */
    if (ok) {
        uint8_t one = 1;
        if (!progress_meta_set_in_tx(rpdb, COINS_KV_MIGRATION_COMPLETE_KEY,
                                     &one, 1))
            ok = false;
    }
    if (ok && sqlite3_exec(rpdb, "COMMIT", NULL, NULL, &terr) != SQLITE_OK)
        ok = false;
    if (!ok)
        sqlite3_exec(rpdb, "ROLLBACK", NULL, NULL, NULL);
    if (terr) sqlite3_free(terr);
    uss_close(h);

    if (ok)
        fprintf(stderr,
                "[boot] -refold-from-anchor: loaded %llu coins from the MINTED "
                "snapshot %s (SHA3 verified vs the compiled checkpoint)\n",
                (unsigned long long)lc.inserted, path);
    return ok;
}

/* B2 — the -refold-from-anchor reset (config/boot.h). Sibling of
 * boot_refold_staged_reset EXCEPT:
 *   (i)   coins_kv is FULL-reset (coins_kv_reset_for_reseed) — a height-bounded
 *         DELETE cannot restore spent-below-anchor coins, so we re-seed the
 *         WHOLE SHA3-verified anchor set. The source is the MINTED snapshot
 *         artifact when present (verified vs the compiled checkpoint by the
 *         loader); otherwise the existing node.db `utxos` re-seed fallback (the
 *         operator-paged path — the hard-assert below still guards it).
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
     * set. PREFERRED SOURCE = the MINTED snapshot artifact (the -mint-anchor
     * ceremony's verified anchor UTXO set): the loader SHA3-verifies it AND
     * binds it to the compiled checkpoint before any coin lands, so it
     * reproduces the anchor exactly even on a contaminated datadir (§0d: the
     * node.db `utxos` mirror is the contaminated TIP, NOT the anchor — re-seeding
     * from it yields 1,344,574 ≠ 1,354,771). FALLBACK (no snapshot present) =
     * the existing node.db `utxos` re-seed, which the hard-assert below still
     * guards (FATALs if it doesn't reproduce the checkpoint — the operator-paged
     * path). Clear the node.db mirror commitment keys + header_admit_log +
     * sapling tree exactly like the from-genesis reset. */
    bool snap_present = false;
    bool reseed_ok =
        boot_anchor_seed_from_snapshot(ndb, rpdb, cp, &snap_present);
    if (!reseed_ok) {
        if (snap_present)
            LOG_WARN("boot", "[boot] -refold-from-anchor: minted snapshot "
                     "present but unusable — falling back to the node.db reseed "
                     "(hard-assert will FATAL if it is the contaminated tip)");
        reseed_ok = coins_kv_reset_for_reseed(rpdb);
        if (reseed_ok)
            reseed_ok = coins_kv_seed_from_node_db(
                rpdb, sqlite3_db_filename(ndb->db, "main"));
    }
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

/* -load-snapshot-at-own-height=PATH (config/boot.h). EXPLICIT-ONLY recovery
 * loader. Sibling of boot_refold_from_anchor_reset EXCEPT:
 *   (i)   the snapshot is SELF-verified — uss_open(path, verify_full_sha3=true,
 *         expected_sha3=NULL) recomputes the body SHA3 and compares it to the
 *         file's OWN hdr.sha3_hash; it is NOT bound to the compiled checkpoint.
 *         We additionally require uss_iter to consume EXACTLY hdr.count records
 *         (records_parsed == hdr.count). Either check failing → LOG_FAIL +
 *         FATAL-refuse; NO coin is ever stamped on a self-inconsistent file.
 *   (ii)  the fold resumes at the snapshot's OWN header height (hdr.height),
 *         NOT the compiled anchor — so a snapshot taken ABOVE the compiled
 *         checkpoint seeds at its real height and the fold climbs from there.
 *   (iii) the 8 stage cursors are forced to hdr.height, coins_applied_height =
 *         hdr.height+1 (utxo_apply next-height convention), tip_finalize seeded
 *         AT hdr.height with hdr.anchor_block_hash (trusted seed: the
 *         self-SHA3-verified base).
 * The staged pipeline then folds FORWARD over on-disk BODIES from hdr.height to
 * the active tip running the REAL script/proof/utxo_apply/tip_finalize stages.
 *
 * GATED: only called when ctx->load_snapshot_at_own_height is non-NULL (an
 * operator-supplied path); a normal boot never reaches it. */

/* FIX 3 seam (see boot.h). PURE: no side effects. */
bool boot_snapshot_anchor_hash_matches(const unsigned char *index_block_hash,
                                       const unsigned char *snapshot_anchor_hash)
{
    if (!index_block_hash || !snapshot_anchor_hash)
        return false;
    return memcmp(index_block_hash, snapshot_anchor_hash, 32) == 0;
}

void boot_load_snapshot_at_own_height_reset(struct node_db *ndb,
                                            const char *path,
                                            struct main_state *ms)
{
    sqlite3 *rpdb = progress_store_db();
    if (!rpdb) {
        fprintf(stderr, "FATAL: -load-snapshot-at-own-height: progress store not "
                "open; cannot seed\n");
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "load_snapshot_at_own_height progress_store_not_open");
        _exit(EXIT_FAILURE);
    }
    if (!path || !path[0]) {
        fprintf(stderr, "FATAL: -load-snapshot-at-own-height: empty path\n");
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "load_snapshot_at_own_height empty_path");
        _exit(EXIT_FAILURE);
    }

    /* (i) Open + SELF-verify the snapshot against its OWN header SHA3 (NOT the
     * compiled checkpoint). expected_sha3=NULL skips the checkpoint binding;
     * verify_full_sha3=true still recomputes the whole body and compares it to
     * hdr.sha3_hash, so a corrupt/forged body is rejected before any coin lands. */
    char err[256] = {0};
    struct uss_header hdr;
    struct uss_handle *h = uss_open(path, /*verify_full_sha3=*/true,
                                    /*expected_sha3=*/NULL, &hdr, err, sizeof(err));
    if (!h) {
        fprintf(stderr, "FATAL: -load-snapshot-at-own-height: uss_open(%s) failed "
                "(self-SHA3 verify): %s — REFUSING to seed\n", path, err);
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "load_snapshot_at_own_height uss_open_failed path=%s err=%s",
                    path, err);
        _exit(EXIT_FAILURE);
    }
    const int32_t seed_h = (int32_t)hdr.height;

    /* (ii) CONSENSUS CROSS-CHECK — bind the snapshot to THIS node's PoW-proven
     * header chain. The self-SHA3 verify above proves the file is internally
     * consistent but binds it to NOTHING on the real chain: a self-consistent
     * FORGED snapshot at an arbitrary height would otherwise seed coins_kv.
     * Require the snapshot's hdr.anchor_block_hash to byte-equal the in-memory
     * active-chain block hash at seed_h (both internal little-endian; the
     * snapshot writer fed cp->block_hash, same representation as
     * block_index.hashBlock). The prior block-index load (`--importblockindex`
     * is a documented prerequisite) populates ms->chain_active up to the tip, so
     * the slot at seed_h is present on the supported recovery path. FATAL on a
     * mismatch — never stamp a coin against a forged anchor. */
    if (ms) {
        const struct block_index *bi = active_chain_at(&ms->chain_active, seed_h);
        if (!bi && ms->pindex_best_header &&
            ms->pindex_best_header->nHeight >= seed_h) {
            /* seed_h is above the coins-best active-chain WINDOW but within
             * this node's PoW-proven header chain. The block_index map + the
             * on-disk bodies span to the header tip (pindex_best_header); only
             * the VISIBLE active window was pinned to coins-best by the boot
             * restore (utxo_recovery_restore_chain_tip -> csr_commit_tip).
             * Widen the window forward to the header tip with the SAME
             * primitive the reducer uses for its lookahead: it fills chain[]
             * by walking pprev and NEVER publishes finalized authority, so the
             * consensus anchor-hash cross-check below reads the real
             * PoW-proven block_index at seed_h. A pprev gap leaves the slot
             * NULL and the FATAL below still fires (never bind against a
             * missing/forged anchor). The loader's header docstring assumed
             * `--importblockindex` supplies this slot; it does NOT (header
             * import sets only pindex_best_header, never c->chain[]), so the
             * window is extended here. */
            active_chain_extend_window(&ms->chain_active, ms->pindex_best_header);
            bi = active_chain_at(&ms->chain_active, seed_h);
        }
        if (!bi) {
            fprintf(stderr,
                    "FATAL: -load-snapshot-at-own-height: the in-memory active "
                    "chain has NO block at the snapshot height h=%d — cannot "
                    "consensus-bind the snapshot's anchor hash (header tip h=%d). "
                    "REFUSING to seed.\n",
                    seed_h,
                    ms->pindex_best_header ? ms->pindex_best_header->nHeight : -1);
            event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                        "load_snapshot_at_own_height no_index_block seed_h=%d",
                        seed_h);
            uss_close(h);
            _exit(EXIT_FAILURE);
        }
        if (!boot_snapshot_anchor_hash_matches(bi->hashBlock.data,
                                               hdr.anchor_block_hash)) {
            fprintf(stderr,
                    "FATAL: -load-snapshot-at-own-height: snapshot anchor hash "
                    "DOES NOT MATCH this node's PoW-proven header at h=%d — the "
                    "snapshot is for a DIFFERENT (or forged) chain. REFUSING to "
                    "seed coins_kv.\n",
                    seed_h);
            event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                        "load_snapshot_at_own_height anchor_hash_mismatch "
                        "seed_h=%d", seed_h);
            uss_close(h);
            _exit(EXIT_FAILURE);
        }
        fprintf(stderr,
                "[boot] -load-snapshot-at-own-height: anchor hash MATCHES the "
                "in-binary PoW header at h=%d — snapshot is consensus-bound to "
                "this chain\n", seed_h);
    } else {
        LOG_WARN("boot", "[boot] -load-snapshot-at-own-height: no main_state "
                 "passed (unit-test path?) — SKIPPING the anchor-hash consensus "
                 "cross-check; the loaded set is NOT bound to a PoW header");
    }

    /* Neutralize the cold-import seed provenance so the later stage init
     * (block_index_loader_seed_stages_from_cold_import) does not re-stamp the
     * trusted anchor forward to the checkpoint. */
    utxo_recovery_clear_cold_import_seed(ndb);
    (void)node_db_state_set(ndb, "leveldb_utxo_migrated", NULL, 0);

    /* Phase 1 (own transactions): FULL coins_kv reset, then RE-SEED from the
     * snapshot. Bulk-load under ONE transaction so a crash mid-load rolls back. */
    if (!coins_kv_reset_for_reseed(rpdb)) {
        fprintf(stderr, "FATAL: -load-snapshot-at-own-height: "
                "coins_kv_reset_for_reseed failed\n");
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "load_snapshot_at_own_height coins_kv_reset_failed");
        uss_close(h);
        _exit(EXIT_FAILURE);
    }
    char *terr = NULL;
    if (sqlite3_exec(rpdb, "BEGIN IMMEDIATE", NULL, NULL, &terr) != SQLITE_OK) {
        fprintf(stderr, "FATAL: -load-snapshot-at-own-height: BEGIN failed: %s\n",
                terr ? terr : "(no msg)");
        if (terr) sqlite3_free(terr);
        uss_close(h);
        _exit(EXIT_FAILURE);
    }
    struct mint_load_ctx lc = { .pdb = rpdb };
    int64_t emitted = uss_iter(h, mint_load_record_cb, &lc);
    /* (i) the ONLY trust gate: body SHA3 already matched in uss_open; here we
     * require uss_iter to have consumed EXACTLY hdr.count records (records_parsed
     * == hdr.count) with no insert failure. */
    bool load_ok = !lc.failed && emitted == (int64_t)hdr.count &&
                   lc.inserted == hdr.count;
    if (load_ok) {
        uint8_t one = 1;
        if (!progress_meta_set_in_tx(rpdb, COINS_KV_MIGRATION_COMPLETE_KEY,
                                     &one, 1))
            load_ok = false;
    }
    if (load_ok && sqlite3_exec(rpdb, "COMMIT", NULL, NULL, &terr) != SQLITE_OK)
        load_ok = false;
    if (!load_ok)
        sqlite3_exec(rpdb, "ROLLBACK", NULL, NULL, NULL);
    if (terr) sqlite3_free(terr);
    uss_close(h);

    if (!load_ok) {
        fprintf(stderr, "FATAL: -load-snapshot-at-own-height: load FAILED "
                "(inserted=%llu emitted=%lld want count=%llu, insert_failed=%d) "
                "— REFUSING to seed\n",
                (unsigned long long)lc.inserted, (long long)emitted,
                (unsigned long long)hdr.count, lc.failed ? 1 : 0);
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "load_snapshot_at_own_height load_mismatch inserted=%llu "
                    "want=%llu", (unsigned long long)lc.inserted,
                    (unsigned long long)hdr.count);
        _exit(EXIT_FAILURE);
    }
    fprintf(stderr,
            "[boot] -load-snapshot-at-own-height: SELF-verified snapshot %s "
            "(body SHA3 OK, height=%d, count=%llu) — seeded coins_kv\n",
            path, seed_h, (unsigned long long)hdr.count);

    (void)boot_index_clear_coins_state(ndb);

    /* SAPLING COMMITMENT-TREE RE-SEED at the seed height.
     *
     * Why this is needed: this loader keeps body_persist/validate_headers at the
     * on-disk header/body tip (so db_tip stays HIGH) while it re-seeds coins_kv
     * at seed_h. The Sapling note-commitment tree (node_state["sapling_tree"])
     * is consumed by the wallet witness machinery + the MMR controllers
     * (node_db_catchup_service.c, wallet_rescan_controller_witness.c,
     * blockchain_controller_mmr.c). The OLD behavior here BLINDLY cleared
     * node_state["sapling_tree"] to NULL — but the catchup service resumes its
     * tree from db_tip+1 deserializing exactly that blob, so a NULL blob at a
     * HIGH db_tip would silently rebuild the tree as if EMPTY below db_tip:
     * a wrong (truncated) commitment tree, breaking wallet note witnesses and
     * the MMR root. (NOTE: this re-seed does NOT unblock proof_validate —
     * proof_validate is stateless w.r.t. this tree; it reads each shielded
     * spend's OWN embedded anchor. See the blocker note returned by the task.)
     *
     * Correct re-seed: clear ALL stale resume markers together, then drive the
     * dedicated, consensus-validating rebuilder (sync_controller_sapling_tree.c
     * sapling_tree_rebuild) which replays note commitments from a SHA3-verified
     * flat-file checkpoint (<datadir>/sapling_tree_ckpt.dat) — or from Sapling
     * activation when no checkpoint is present — validating each per-height root
     * against hashFinalSaplingRoot, and re-persists node_state["sapling_tree"]
     * coherent with the chain tip. It is bounded by the flat-file checkpoint
     * (no unconditional 2.6M-block replay) and is a no-op below Sapling
     * activation. Requires the in-memory active chain (ms); when absent (unit
     * tests) we fall back to the old NULL-clear so the catchup path still owns
     * the rebuild. */
    (void)node_db_state_set(ndb, "sapling_tree", NULL, 0);
    (void)node_db_state_set(ndb, "sapling_tree_rescan_height", NULL, 0);
    (void)node_db_state_set(ndb, "sapling_tree_rebuild_height", NULL, 0);
    if (ms) {
        char datadir[1024] = {0};
        GetDataDir(true, datadir, sizeof(datadir));
        int appended = sapling_tree_rebuild(ndb, &ms->chain_active, datadir);
        if (appended < 0) {
            LOG_WARN("boot", "[boot] -load-snapshot-at-own-height: "
                     "sapling_tree_rebuild failed (returned %d) — wallet note "
                     "witnesses / MMR may be stale until the catchup service "
                     "rebuilds; the consensus coins-fold is unaffected",
                     appended);
        } else {
            fprintf(stderr,
                    "[boot] -load-snapshot-at-own-height: Sapling commitment "
                    "tree REBUILT + re-persisted (%d commitments) coherent with "
                    "the chain tip\n", appended);
        }
    } else {
        LOG_WARN("boot", "[boot] -load-snapshot-at-own-height: no main_state — "
                 "cleared sapling_tree to NULL; the catchup service rebuilds it "
                 "from genesis on next run");
    }

    /* Phase 2 — TWO-TIER stage reset (one progress.kv transaction).
     *
     * SOUNDNESS / why two tiers (the keystone of this loader): the snapshot
     * re-seeds ONLY the UTXO set (coins_kv) at seed_h. Two of the eight stages
     * (header_admit, validate_headers) validate ONLY PoW/Equihash from the
     * in-memory header chain and NEVER read coins; two more (body_fetch,
     * body_persist) only observe on-disk block bodies + build the forward
     * created_outputs creation index — also coins-INDEPENDENT. Their work
     * ABOVE seed_h is therefore still valid after the re-seed and MUST be
     * preserved, because:
     *   (a) rewinding validate_headers forces a P2P header RE-SYNC that, against
     *       a confused getheaders locator, collapses pindex_best_header from the
     *       on-disk header tip down to the seed — capping the WHOLE fold at the
     *       seed height (observed: header tip pinned, H* frozen one block past
     *       the seed). Keeping validate_headers at the on-disk tip keeps
     *       pindex_best_header at the real tip, so active_chain_extend_window
     *       supplies the lookahead all the way up and the coins-fold climbs over
     *       ON-DISK bodies with NO peers.
     *   (b) created_outputs (body_persist's forward creation index) is exactly
     *       what script_validate's prevout resolver reads for coins created in
     *       blocks ABOVE seed_h — deleting it would make every such prevout
     *       unresolvable.
     * The other FOUR stages (script_validate, proof_validate, utxo_apply,
     * tip_finalize) DO read/derive from the coins set, so their rows above the
     * old (contaminated) seed are stale: force them DOWN to seed_h and clear
     * their derived tables so they re-run against the freshly seeded coins.
     *
     * Coins-INDEPENDENT stages: preserve at MAX(current, seed_h) — never rewind
     * below the seed, never below where they already are. Their log tables and
     * created_outputs are KEPT. */
    static const char *const k_keep_stages[] = {
        "header_admit", "validate_headers", "body_fetch", "body_persist",
    };
    /* Coins-DEPENDENT stages: force to seed_h; their derived tables are cleared
     * (created_outputs is deliberately NOT here — it is body_persist's index). */
    static const char *const k_coins_stages[] = {
        "script_validate", "proof_validate", "utxo_apply", "tip_finalize",
    };
    static const char *const k_coins_tables[] = {
        "script_validate_log", "proof_validate_log", "utxo_apply_log",
        "tip_finalize_log", "utxo_apply_delta", "nullifiers",
    };
    bool refold_ok = true;
    char *refold_err = NULL;
    progress_store_tx_lock();
    if (sqlite3_exec(rpdb, "BEGIN IMMEDIATE", NULL, NULL, &refold_err)
        != SQLITE_OK)
        refold_ok = false;
    for (size_t i = 0;
         refold_ok && i < sizeof(k_coins_tables) / sizeof(k_coins_tables[0]);
         i++) {
        char dsql[96];
        snprintf(dsql, sizeof dsql, "DELETE FROM %s", k_coins_tables[i]);
        if (sqlite3_exec(rpdb, dsql, NULL, NULL, &refold_err) != SQLITE_OK) {
            if (refold_err && strstr(refold_err, "no such table")) {
                sqlite3_free(refold_err);
                refold_err = NULL;
            } else {
                refold_ok = false;
            }
        }
    }
    /* Coins-independent stages: keep their on-disk progress (only raise to
     * seed_h if somehow behind it — never rewind the validated header/body
     * frontier below where it already stands). Read the durable cursor with a
     * direct query (we already hold the progress tx lock + an open tx). */
    for (size_t i = 0;
         refold_ok && i < sizeof(k_keep_stages) / sizeof(k_keep_stages[0]);
         i++) {
        int cur = -1;
        sqlite3_stmt *cst = NULL;
        if (sqlite3_prepare_v2(rpdb,
                "SELECT cursor FROM stage_cursor WHERE name = ?",
                -1, &cst, NULL) == SQLITE_OK) {
            sqlite3_bind_text(cst, 1, k_keep_stages[i], -1, SQLITE_STATIC);
            if (sqlite3_step(cst) == SQLITE_ROW)  // raw-sql-ok:progress-kv-kernel-store
                cur = sqlite3_column_int(cst, 0);
        }
        if (cst)
            sqlite3_finalize(cst);
        int target = (cur > seed_h) ? cur : seed_h;
        if (!stage_repair_force_stage_cursor(rpdb, k_keep_stages[i], target))
            refold_ok = false;
    }
    /* Coins-dependent stages: force DOWN to the seed to re-fold against the
     * freshly seeded coins. */
    for (size_t i = 0;
         refold_ok && i < sizeof(k_coins_stages) / sizeof(k_coins_stages[0]);
         i++)
        if (!stage_repair_force_stage_cursor(rpdb, k_coins_stages[i], seed_h))
            refold_ok = false;
    if (refold_ok && !coins_kv_set_applied_height_in_tx(rpdb, seed_h + 1))
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
                "FATAL: -load-snapshot-at-own-height: failed to force the 8 stage "
                "cursors to h=%d — refusing to start a half-armed fold\n", seed_h);
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "load_snapshot_at_own_height seed_h=%d cursor arm failed",
                    seed_h);
        _exit(EXIT_FAILURE);
    }

    /* (iii) Seed the tip_finalize served-tip prefix AT hdr.height with the
     * snapshot's own anchor_block_hash (trusted seed — the self-SHA3-verified
     * base). */
    if (!tip_finalize_stage_seed_anchor(seed_h, hdr.anchor_block_hash, true))
        LOG_WARN("boot", "[boot] -load-snapshot-at-own-height: tip_finalize anchor "
                 "seed returned false (stage not yet wired?) — the runtime "
                 "authority re-seeds from the coins tip");

    fprintf(stderr,
            "[boot] -load-snapshot-at-own-height: coin set RE-SEEDED + "
            "self-verified (count=%llu, body SHA3 OK) at h=%d; coins-dependent "
            "stages (script_validate/proof_validate/utxo_apply/tip_finalize) "
            "forced to h=%d, coins-independent stages "
            "(header_admit/validate_headers/body_fetch/body_persist) KEPT at the "
            "on-disk header/body tip; the staged pipeline re-folds the coins "
            "delta forward over ON-DISK bodies (H* climbs from h=%d, no P2P "
            "header re-sync needed)\n",
            (unsigned long long)hdr.count, seed_h, seed_h, seed_h);
}

/* -load-verify-boot eligibility probe (config/boot.h). Pure: decides whether a
 * NORMAL boot should route the verified-snapshot load+anchor-fold instead of the
 * cold-import seed. Reuses mint_snapshot_path + uss_open (the EXISTING loader's
 * SHA3 verification — never reimplemented). See boot.h for the full contract.
 *
 * SAFETY (load-bearing): a snapshot whose recomputed SHA3 != the compiled
 * checkpoint makes uss_open return NULL (header expected_sha3 memcmp OR full-body
 * SHA3 memcmp in the loader) → this returns FALSE → the caller runs the proven
 * path and NO bad set is ever routed toward coins_kv. A healthy coins_kv (proven
 * authority) also returns FALSE → an already-seeded node is NEVER reset. */
bool boot_load_verify_snapshot_eligible(struct node_db *ndb,
                                        struct sqlite3 *progress_db)
{
    if (!ndb || !progress_db)
        return false;

    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    if (!cp)
        return false;  /* no compiled checkpoint to anchor against */

    /* (3) NEVER reset a healthy node: if coins_kv already provably holds the
     * live coin set, the load-verify route must not fire (it FULL-resets
     * coins_kv). Probe this BEFORE touching the file so a synced node short-
     * circuits cheaply. */
    if (coins_kv_is_proven_authority(progress_db, NULL))
        return false;

    char path[1100];
    if (!mint_snapshot_path(ndb, path, sizeof(path)))
        return false;

    /* (1)+(2) Present + verified: uss_open binds expected_sha3 = cp->sha3_hash
     * AND recomputes the full body SHA3 (verify_full_sha3=true). A NULL return
     * means absent OR header-magic/version/expected-sha3 mismatch OR body-SHA3
     * mismatch — every "no usable baked snapshot" case. Read-only mmap; closed
     * immediately (no coins_kv mutation in this probe). */
    char err[256] = {0};
    struct uss_header hdr;
    struct uss_handle *h = uss_open(path, /*verify_full_sha3=*/true,
                                    cp->sha3_hash, &hdr, err, sizeof(err));
    if (!h)
        return false;  /* absent or failed SHA3/header verify → safe fallback */

    bool count_ok = (hdr.count == cp->utxo_count);
    uss_close(h);
    if (!count_ok) {
        LOG_WARN("boot", "[boot] -load-verify-boot: snapshot %s count=%llu != "
                 "checkpoint %llu — ignoring the artifact, running the proven "
                 "boot path", path, (unsigned long long)hdr.count,
                 (unsigned long long)cp->utxo_count);
        return false;
    }

    LOG_INFO("boot", "[boot] -load-verify-boot: verified baked anchor snapshot "
             "present (%s, SHA3 == compiled checkpoint, count=%llu) and coins_kv "
             "is not yet the proven authority — routing the LOAD+VERIFY anchor "
             "seed + anchor->tip delta fold", path,
             (unsigned long long)cp->utxo_count);
    return true;
}

/* CUTOVER DEFECT 2 — body-span contiguity gate. See config/boot.h for the
 * contract. PURE except for the optional named-blocker raise on a gap: it scans
 * the active-chain block_index over (anchor_height, resume_target] and verifies
 * each slot has BLOCK_HAVE_DATA. The from-anchor fold replays on-disk BODIES
 * across this span; a missing/pruned body would pin utxo_apply at that height
 * (the prevout_unresolved wedge relocated). Gate BEFORE arming so the failure is
 * a NAMED blocker the operator/peer-fetch path can act on, never a silent stall. */
bool boot_refold_body_span_contiguous(struct main_state *ms,
                                      int32_t anchor_height,
                                      int32_t resume_target,
                                      int32_t *out_first_missing,
                                      bool raise_blocker)
{
    if (out_first_missing)
        *out_first_missing = -1;
    if (!ms)
        return false;   /* cannot prove contiguity without the chain */
    if (resume_target <= anchor_height)
        return true;    /* empty span — nothing to fold, trivially contiguous */

    for (int32_t h = anchor_height + 1; h <= resume_target; h++) {
        const struct block_index *bi = active_chain_at(&ms->chain_active, h);
        if (!bi || !(bi->nStatus & BLOCK_HAVE_DATA)) {
            if (out_first_missing)
                *out_first_missing = h;
            if (raise_blocker) {
                char reason[BLOCKER_REASON_MAX];
                snprintf(reason, sizeof(reason),
                         "from-anchor fold span (%d..%d] has a missing/pruned "
                         "block body at height=%d (%s); refusing to arm the "
                         "fold — utxo_apply would pin here. ACTION: fetch the "
                         "body (peer/disk) so the span is contiguous, then retry",
                         anchor_height, resume_target, h,
                         bi ? "BLOCK_HAVE_DATA clear" : "no block_index slot");
                struct blocker_record r;
                if (blocker_init(&r, "refold.body_gap", "boot",
                                 BLOCKER_DEPENDENCY, reason)) {
                    r.escape_deadline_secs = 0;   /* no auto-escape: needs a body */
                    r.retry_budget = -1;
                    (void)blocker_set(&r);
                }
                fprintf(stderr,
                        "[boot] -refold-from-anchor: BODY-SPAN GAP at height=%d "
                        "in (%d..%d] — NOT arming the fold; raised named blocker "
                        "refold.body_gap (fill the body, then retry)\n",
                        h, anchor_height, resume_target);
            }
            return false;
        }
    }
    /* Whole span contiguous: clear any stale gap blocker from a prior boot so a
     * re-armed fold over a now-filled span starts clean. */
    if (raise_blocker)
        blocker_clear("refold.body_gap");
    return true;
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
 * DEFAULT SELF-HEAL: this is now called UNCONDITIONALLY on every boot from the
 * single seed-vs-anchor decision site (config/src/boot_services.c). It is safe
 * to run flag-free because the PURE detect predicate only fires on a DURABLY
 * proven tear, and the reset it triggers can only ever stamp the SHA3-verified
 * anchor set (or FATAL) — never an unproven one. A HEALTHY (untorn) datadir
 * returns false here and takes the normal seed path unchanged. */
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

    /* SAFETY (load-bearing): the AUTO-ARM must NEVER route a torn datadir into
     * the node.db `utxos` reseed + the hard-assert FATAL. boot_refold_from_anchor_reset
     * (the explicit -refold-from-anchor flag path) DELIBERATELY allows that
     * node.db fallback as the operator-paged path (its contract). But the
     * auto-arm runs by DEFAULT on a torn boot, so if no SHA3-verified snapshot is
     * reachable it must DECLINE — return false WITHOUT resetting — so the caller
     * (boot_services.c) falls through to the normal cold-import seed, whose torn
     * gate (block_index_loader_torn_import_gate_fires) raises the honest
     * EV_OPERATOR_NEEDED + seed.torn_import PERMANENT blocker. Without this the
     * default auto-arm would turn the current honest operator_needed halt into a
     * _exit(EXIT_FAILURE) on a torn-but-no-snapshot datadir — a regression. */
    if (!anchor_snapshot_verified_reachable(ndb, cp)) {
        LOG_WARN("boot", "[boot] from-anchor auto-arm: torn cold-import detected "
                 "at h=%d (frontier=%d) but NO SHA3-verified anchor snapshot is "
                 "reachable — DECLINING the auto-arm (would otherwise fall into "
                 "the contaminated node.db reseed + FATAL); deferring to the "
                 "honest operator_needed halt (block_index_loader_torn_import_"
                 "gate_fires)", (int)hole_h, (int)ceiling);
        return false;
    }

    /* CUTOVER DEFECT 2 — body-span gate BEFORE arming. The from-anchor fold
     * replays on-disk BODIES over (anchor, resume_target]; a missing/pruned body
     * in that span pins utxo_apply mid-fold (the prevout_unresolved wedge,
     * relocated to the gap height). Capture resume_target from the SAME
     * active_chain_height read the post-reset mark uses, then verify every body
     * is present. On a gap: raise the NAMED blocker refold.body_gap and DECLINE
     * the arm (return false) so the caller defers to the normal seed path / a
     * peer-fetch fills the body — never a silent stall mid-fold. */
    int32_t resume_target = (int32_t)active_chain_height(&ms->chain_active);
    int32_t first_missing = -1;
    if (!boot_refold_body_span_contiguous(ms, cp->height, resume_target,
                                          &first_missing, /*raise_blocker=*/true)) {
        LOG_WARN("boot", "[boot] from-anchor auto-arm: torn cold-import detected "
                 "at h=%d (frontier=%d) but the fold span (%d..%d] has a missing "
                 "block body at h=%d — DECLINING the auto-arm and raising "
                 "refold.body_gap (would otherwise pin utxo_apply mid-fold)",
                 (int)hole_h, (int)ceiling, (int)checkpoint, (int)resume_target,
                 (int)first_missing);
        return false;
    }

    LOG_WARN("boot",
             "[boot] from-anchor auto-arm: torn cold-import detected (durable "
             "unresolved prevout at h=%d, frontier=%d) — arming a from-anchor "
             "refold (re-seed the SHA3 anchor set + fold forward) instead of "
             "halting; the anchor set is HARD-ASSERTED next (FATAL on mismatch)",
             (int)hole_h, (int)ceiling);

    /* Reset FATALs internally if the re-seeded anchor set fails the SHA3/count
     * assert (a genuinely-unrecoverable tear). On return the anchor set is
     * PROVEN, so mark the from-anchor signal + resume target (the active tip the
     * fold climbs to). The verified-snapshot reachability was just confirmed
     * above, so the reset takes the snapshot reseed path (never the node.db
     * fallback). */
    boot_refold_from_anchor_reset(ndb);
    (void)refold_progress_mark_started_from_anchor(progress_db, resume_target);
    return true;
}

/* -mint-anchor (config/boot.h). The ANCHOR-SET MINT boot-time reset:
 *   (1) reset the staged reducer to GENESIS exactly like -refold-staged
 *       (boot_refold_staged_reset truncates coins_kv, forces the 8 cursors to
 *       0, clears the *_log rows + node.db mirror commitment keys) so the fold
 *       re-derives every coin from the on-disk BODIES, never the borrowed
 *       node.db `utxos` mirror;
 *   (2) cap the fold at the compiled SHA3 UTXO checkpoint anchor — header_admit
 *       refuses to admit above the ceiling, so the eight-stage pipeline
 *       converges AT the anchor instead of folding to the tip;
 *   (3) mark refold_in_progress (progress.kv) so the L0 frontier floor drops to
 *       0 and the below-anchor self-repair is suspended while the fold re-walks
 *       the frozen prefix (the from-genesis refold semantics).
 * The driver boot_mint_anchor_run() (config/src/boot_mint_anchor.c) then drives
 * the fold to the anchor and writes + hard-asserts the snapshot. */
void boot_mint_anchor_reset(struct node_db *ndb, bool fast)
{
    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    if (!cp) {
        fprintf(stderr, "FATAL: -mint-anchor: no compiled SHA3 UTXO checkpoint "
                "— nothing to mint toward\n");
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "mint_anchor no_compiled_checkpoint");
        _exit(EXIT_FAILURE);
    }

    /* (1) genesis reset — identical machinery to -refold-staged. */
    boot_refold_staged_reset(ndb);

    /* (2) cap the fold AT the anchor (inclusive). header_admit stops here. */
    mint_fold_ceiling_set(cp->height);

    /* (2b) OFFLINE FAST-MINT (-mint-anchor-fast): flip the process-global
     * crypto pass-through so script_validate/proof_validate skip their per-block
     * ECDSA/Groth16 verification while folding genesis..anchor. The state
     * transition (utxo_apply) is untouched, so the minted coins_kv set is
     * IDENTICAL to the full-validated fold; boot_mint_anchor_run still
     * HARD-ASSERTS SHA3==checkpoint + count before publishing, so a wrong set
     * can never be written. `fast` is true ONLY when the caller already gated on
     * ctx->mint_anchor (config/src/boot.c) — this TU is the lone caller of
     * mint_skip_crypto_set, so a normal boot can never arm the pass-through. */
    if (fast) {
        mint_skip_crypto_set(true);
        fprintf(stderr,
                "[boot] -mint-anchor-fast: OFFLINE FAST-MINT — script_validate/"
                "proof_validate crypto PASS-THROUGH for the genesis..%d fold; the "
                "state fold is unchanged and the SHA3==checkpoint hard-assert "
                "still certifies the minted set (FATAL on mismatch)\n",
                cp->height);
    }

    /* (3) suspend the below-anchor self-repair while the frozen prefix re-folds
     * (the from-genesis refold L0 floor = 0). */
    (void)refold_progress_mark_started(progress_store_db());

    fprintf(stderr,
            "[boot] -mint-anchor: reset to genesis; fold CAPPED at the SHA3 "
            "checkpoint anchor h=%d (want count=%llu); the staged pipeline folds "
            "genesis..anchor over on-disk bodies, then the mint writes + asserts "
            "the snapshot\n",
            cp->height, (unsigned long long)cp->utxo_count);
}
