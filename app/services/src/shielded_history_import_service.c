/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * shielded_history_import_service — implementation. See the header for the
 * contract (completeness + atomicity are the whole point).
 *
 * The four chainstate iterators stream into anchor_kv / nullifier_kv inside ONE
 * BEGIN IMMEDIATE. Any anomaly aborts the whole transaction (rollback), so a
 * partial set can never flip a cursor to zero.
 *
 * one-result-type-ok:owner-gated-boot-import — this is a one-shot owner-gated
 * boot/import entry point (like the --importblockindex / node_db_import boot
 * modes), not a runtime saga step. Its public signature is deliberately
 * `bool + struct shielded_import_report` per the design spec
 * (docs/work/fast-sync-to-tip-plan-2026-07-16.md §4.4): the failure reason
 * travels with the failure via node.log [shielded_import] (every refusal path
 * LOG_FAIL/LOG_RETURNs the exact anomaly) AND via the per-pool report the
 * caller inspects; there is no zcl_result-returning runtime surface to thread. */
// one-result-type-ok:owner-gated-boot-import

#include "services/shielded_history_import_service.h"

#include "jobs/utxo_apply_anchors.h"
#include "jobs/utxo_apply_nullifiers.h"
#include "sapling/incremental_merkle_tree.h"
#include "storage/anchor_kv.h"
#include "storage/chainstate_legacy_reader.h"
#include "storage/nullifier_kv.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SHI_SUBSYS "shielded_import"
#define SHI_PROVENANCE_KEY SHIELDED_IMPORT_PROVENANCE_KEY

/* Streaming callback context — shared by the anchor + nullifier iterators. */
struct shi_ctx {
    sqlite3 *db;
    bool ok;                 /* set false on the first store failure */
    int pool;                /* ANCHOR_POOL_* / NULLIFIER_POOL_* for this pass */

    /* Height assignment: the best/tip anchor takes `tip_height` so
     * anchor_kv_latest_tree (ORDER BY height DESC) always returns the frontier
     * to append to; every other historical anchor takes a monotone
     * import-order height strictly below it (their exact block height is not
     * consensus-load-bearing — only presence + the tip frontier are). */
    int64_t tip_height;
    int64_t next_seq;        /* monotone import-order counter for non-tip rows */
    struct uint256 best_root;
    bool best_present;       /* is there a best-anchor pointer for this pool? */
    bool saw_best;           /* did we import a row equal to best_root? */

    int64_t nf_sentinel;     /* height stamped on imported nullifiers */
    int64_t count;
};

static bool shi_anchor_cb(const struct uint256 *root,
                          const struct incremental_merkle_tree *tree,
                          void *vctx)
{
    struct shi_ctx *c = vctx;
    if (!c || !c->ok)
        return false;

    int64_t height = c->next_seq++;
    if (c->best_present && uint256_eq(root, &c->best_root)) {
        height = c->tip_height;
        c->saw_best = true;
    }
    if (!anchor_kv_add_tree(c->db, c->pool, tree, height)) {
        c->ok = false;
        LOG_RETURN(false, SHI_SUBSYS,
                   "anchor_kv_add_tree failed pool=%d height=%lld",
                   c->pool, (long long)height);
    }
    c->count++;
    return true;
}

static bool shi_nullifier_cb(const uint8_t nf[32], void *vctx)
{
    struct shi_ctx *c = vctx;
    if (!c || !c->ok)
        return false;
    if (!nullifier_kv_add(c->db, nf, c->pool, c->nf_sentinel)) {
        c->ok = false;
        LOG_RETURN(false, SHI_SUBSYS, "nullifier_kv_add failed pool=%d",
                   c->pool);
    }
    c->count++;
    return true;
}

/* Read the three durable cursors, requiring both anchor pools share one
 * positive boundary and the nullifier marker be positive. Returns:
 *   1  -> proceed (anchor_boundary/nullifier_boundary filled, both positive)
 *   0  -> all three already zero (already complete; nothing to import)
 *  -1  -> anomaly (missing/negative/mixed) — caller must refuse */
static int shi_read_boundaries(sqlite3 *db, int64_t *anchor_boundary,
                               int64_t *nullifier_boundary)
{
    int64_t spr = -1, sap = -1, nf = -1;
    bool spr_f = false, sap_f = false, nf_f = false;
    if (!anchor_kv_activation_cursor(db, ANCHOR_POOL_SPROUT, &spr, &spr_f) ||
        !anchor_kv_activation_cursor(db, ANCHOR_POOL_SAPLING, &sap, &sap_f) ||
        !nullifier_kv_activation_cursor(db, &nf, &nf_f))
        LOG_RETURN(-1, SHI_SUBSYS, "cursor read failed");
    if (!spr_f || !sap_f || !nf_f)
        LOG_RETURN(-1, SHI_SUBSYS,
                   "cursor(s) absent spr=%d sap=%d nf=%d — history coverage "
                   "unknown; refusing", spr_f, sap_f, nf_f);
    if (spr == 0 && sap == 0 && nf == 0)
        return 0; /* already complete — not an error */
    if (spr <= 0 || sap <= 0 || nf <= 0)
        LOG_RETURN(-1, SHI_SUBSYS,
                   "cursor(s) not uniformly positive spr=%lld sap=%lld "
                   "nf=%lld — refusing (mixed/complete state)",
                   (long long)spr, (long long)sap, (long long)nf);
    if (spr != sap)
        LOG_RETURN(-1, SHI_SUBSYS,
                   "anchor pools disagree on boundary spr=%lld sap=%lld — "
                   "refusing", (long long)spr, (long long)sap);
    *anchor_boundary = sap;
    *nullifier_boundary = nf;
    return 1;
}

static void shi_rollback(sqlite3 *db)
{
    (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    progress_store_tx_unlock();
}

static bool shi_write_provenance(sqlite3 *db, const struct uint256 *best_block,
                                 const struct shielded_import_report *r)
{
    char best_hex[65];
    uint256_get_hex(best_block, best_hex);
    char buf[320];
    int n = snprintf(buf, sizeof(buf),
                     "source=zclassicd_chainstate;best_block=%s;tip_h=%lld;"
                     "sapling_anchors=%lld;sprout_anchors=%lld;"
                     "sapling_nf=%lld;sprout_nf=%lld;tip_anchor_bound=%d;"
                     "self_folded=false",
                     best_hex, (long long)r->anchor_boundary - 1,
                     (long long)r->sapling_anchors,
                     (long long)r->sprout_anchors,
                     (long long)r->sapling_nullifiers,
                     (long long)r->sprout_nullifiers,
                     r->tip_anchor_bound ? 1 : 0);
    if (n <= 0 || (size_t)n >= sizeof(buf))
        LOG_FAIL(SHI_SUBSYS, "provenance format failed");
    if (!progress_meta_set_in_tx(db, SHI_PROVENANCE_KEY, buf, (size_t)n))
        LOG_FAIL(SHI_SUBSYS, "provenance write failed");
    return true;
}

bool shielded_history_import_from_chainstate(
    sqlite3 *progress_db, const char *chainstate_src_path,
    int64_t expected_tip_height, const struct uint256 *expected_tip_sapling_root,
    struct shielded_import_report *out)
{
    struct shielded_import_report report = {0};
    if (out)
        *out = report;

    if (!progress_db || !chainstate_src_path || !expected_tip_sapling_root ||
        expected_tip_height < 0)
        LOG_FAIL(SHI_SUBSYS, "invalid args");

    /* Fork-safety, defense-in-depth: an all-zero expected root means the caller
     * failed to obtain the header-committed hashFinalSaplingRoot (e.g. reading a
     * blocks.sapling_root projection column that a header import left null). The
     * tip is far past Sapling activation, so its frontier root can never be
     * zero. Binding the imported frontier against zeros — then flipping the
     * activation cursors — would convert the SAFE wedge into an ACCEPT of an
     * unverified frontier. Refuse before touching the transaction. */
    if (uint256_is_null(expected_tip_sapling_root))
        LOG_FAIL(SHI_SUBSYS,
                 "expected tip Sapling root is all-zero — caller did not "
                 "obtain the header-committed hashFinalSaplingRoot; refusing to "
                 "bind the tip frontier against zeros (wedge left intact)");

    if (!anchor_kv_ensure_schema(progress_db) ||
        !nullifier_kv_ensure_schema(progress_db) ||
        !progress_meta_table_ensure(progress_db))
        LOG_FAIL(SHI_SUBSYS, "schema ensure failed");

    int64_t anchor_boundary = 0, nullifier_boundary = 0;
    int br = shi_read_boundaries(progress_db, &anchor_boundary,
                                 &nullifier_boundary);
    if (br < 0)
        return false;
    if (br == 0) {
        LOG_INFO(SHI_SUBSYS,
                 "cursors already zero — shielded history complete, nothing "
                 "to import");
        if (out) {
            out->committed = false;
            out->tip_anchor_bound = true;
        }
        return true;
    }
    report.anchor_boundary = anchor_boundary;
    report.nullifier_boundary = nullifier_boundary;

    /* Open the (already point-in-time / fixture) chainstate copy. */
    void *h = NULL;
    if (!chainstate_legacy_open(chainstate_src_path, &h) || !h)
        LOG_FAIL(SHI_SUBSYS, "chainstate_legacy_open(%s) failed",
                 chainstate_src_path);

    struct uint256 best_block;
    uint256_set_null(&best_block);
    (void)chainstate_legacy_get_best_block(h, &best_block); /* log/provenance */

    /* The chain-committed tip bind (§3): the chainstate best Sapling anchor
     * MUST equal the block header hashFinalSaplingRoot at the tip height. */
    struct uint256 best_sapling;
    if (!chainstate_legacy_get_best_sapling_anchor(h, &best_sapling)) {
        chainstate_legacy_close(h);
        LOG_FAIL(SHI_SUBSYS,
                 "chainstate has no best Sapling anchor — cannot bind tip");
    }
    if (!uint256_eq(&best_sapling, expected_tip_sapling_root)) {
        chainstate_legacy_close(h);
        LOG_FAIL(SHI_SUBSYS,
                 "tip bind FAILED: best Sapling anchor != expected header "
                 "hashFinalSaplingRoot at h=%lld — refusing",
                 (long long)expected_tip_height);
    }
    report.tip_anchor_bound = true;

    struct uint256 best_sprout;
    bool sprout_best_present =
        chainstate_legacy_get_best_sprout_anchor(h, &best_sprout);

    /* ── the ONE atomic transaction ── */
    progress_store_tx_lock();
    char *err = NULL;
    if (sqlite3_exec(progress_db, "BEGIN IMMEDIATE", NULL, NULL, &err)
            != SQLITE_OK) {
        LOG_WARN(SHI_SUBSYS, "BEGIN IMMEDIATE failed: %s",
                 err ? err : sqlite3_errmsg(progress_db));
        if (err) sqlite3_free(err);
        progress_store_tx_unlock();
        chainstate_legacy_close(h);
        return false;
    }
    if (err) { sqlite3_free(err); err = NULL; }

    int64_t nf_sentinel = anchor_boundary - 1; /* highest historical height */

    /* Sapling anchors. */
    struct shi_ctx sap = {
        .db = progress_db, .ok = true, .pool = ANCHOR_POOL_SAPLING,
        .tip_height = expected_tip_height, .next_seq = 0,
        .best_root = best_sapling, .best_present = true,
    };
    int64_t n = chainstate_legacy_iter_sapling_anchors(h, shi_anchor_cb, &sap);
    if (n < 0 || !sap.ok || !sap.saw_best) {
        shi_rollback(progress_db);
        chainstate_legacy_close(h);
        LOG_FAIL(SHI_SUBSYS,
                 "Sapling anchor import incomplete n=%lld ok=%d saw_tip=%d — "
                 "rolled back", (long long)n, sap.ok, sap.saw_best);
    }
    report.sapling_anchors = sap.count;

    /* Sprout anchors (best may be absent — a drained/empty pool). */
    struct shi_ctx spr = {
        .db = progress_db, .ok = true, .pool = ANCHOR_POOL_SPROUT,
        .tip_height = expected_tip_height, .next_seq = 0,
        .best_present = sprout_best_present,
    };
    if (sprout_best_present)
        spr.best_root = best_sprout;
    n = chainstate_legacy_iter_sprout_anchors(h, shi_anchor_cb, &spr);
    if (n < 0 || !spr.ok || (sprout_best_present && !spr.saw_best)) {
        shi_rollback(progress_db);
        chainstate_legacy_close(h);
        LOG_FAIL(SHI_SUBSYS,
                 "Sprout anchor import incomplete n=%lld ok=%d best=%d "
                 "saw_best=%d — rolled back", (long long)n, spr.ok,
                 sprout_best_present, spr.saw_best);
    }
    report.sprout_anchors = spr.count;

    /* Sapling nullifiers. */
    struct shi_ctx snf = {
        .db = progress_db, .ok = true, .pool = NULLIFIER_POOL_SAPLING,
        .nf_sentinel = nf_sentinel,
    };
    n = chainstate_legacy_iter_sapling_nullifiers(h, shi_nullifier_cb, &snf);
    if (n < 0 || !snf.ok) {
        shi_rollback(progress_db);
        chainstate_legacy_close(h);
        LOG_FAIL(SHI_SUBSYS, "Sapling nullifier import failed n=%lld — "
                             "rolled back", (long long)n);
    }
    report.sapling_nullifiers = snf.count;

    /* Sprout nullifiers. */
    struct shi_ctx pnf = {
        .db = progress_db, .ok = true, .pool = NULLIFIER_POOL_SPROUT,
        .nf_sentinel = nf_sentinel,
    };
    n = chainstate_legacy_iter_sprout_nullifiers(h, shi_nullifier_cb, &pnf);
    if (n < 0 || !pnf.ok) {
        shi_rollback(progress_db);
        chainstate_legacy_close(h);
        LOG_FAIL(SHI_SUBSYS, "Sprout nullifier import failed n=%lld — "
                             "rolled back", (long long)n);
    }
    report.sprout_nullifiers = pnf.count;

    /* We are done reading the chainstate; close it before the flip so a torn
     * source cannot silently reopen. */
    chainstate_legacy_close(h);
    h = NULL;

    /* Flip BOTH activation cursors to zero — only now, after the COMPLETE set
     * is staged in this transaction and the tip was bound. */
    if (!anchor_kv_publish_full_replay_complete_in_tx(progress_db,
                                                      anchor_boundary) ||
        !nullifier_kv_publish_full_replay_complete_in_tx(progress_db,
                                                         nullifier_boundary) ||
        !shielded_history_cancel_full_replay_in_tx(progress_db)) {
        shi_rollback(progress_db);
        LOG_FAIL(SHI_SUBSYS, "cursor publish failed — rolled back");
    }

    if (!shi_write_provenance(progress_db, &best_block, &report)) {
        shi_rollback(progress_db);
        LOG_FAIL(SHI_SUBSYS, "provenance failed — rolled back");
    }

    if (sqlite3_exec(progress_db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN(SHI_SUBSYS, "COMMIT failed: %s",
                 err ? err : sqlite3_errmsg(progress_db));
        if (err) sqlite3_free(err);
        shi_rollback(progress_db);
        return false;
    }
    if (err) sqlite3_free(err);
    progress_store_tx_unlock();

    report.committed = true;

    /* Both cursors are now durably zero: refresh the two permanent blockers so
     * the reducer resumes folding from the wedge height + 1. */
    utxo_apply_anchor_gap_blocker_refresh(progress_db);
    utxo_apply_nullifier_gap_blocker_refresh(progress_db);

    LOG_INFO(SHI_SUBSYS,
             "IMPORT COMPLETE: sapling_anchors=%lld sprout_anchors=%lld "
             "sapling_nf=%lld sprout_nf=%lld boundary=%lld — both cursors flipped "
             "to 0, shielded history published",
             (long long)report.sapling_anchors,
             (long long)report.sprout_anchors,
             (long long)report.sapling_nullifiers,
             (long long)report.sprout_nullifiers,
             (long long)anchor_boundary);

    if (out)
        *out = report;
    return true;
}
