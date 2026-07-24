/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Resume-candidate selection and fail-closed accounting for the Sapling
 * tree rebuild.
 *
 * sync_controller_sapling_tree.c owns the replay LOOP. This file owns the
 * three decisions that loop delegates:
 *   - where the replay may start (the anchor_kv frontier candidate),
 *   - what a block the replay could not fold costs (skip accounting),
 *   - how a fail-closed outcome is classified for operators.
 * Declarations live in sync_controller_internal.h. */

#include "sync_controller_internal.h"

#include "chain/chain.h"
#include "core/uint256.h"
#include "sapling/incremental_merkle_tree.h"
#include "storage/anchor_kv.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/log_macros.h"
#include "validation/chainstate.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

bool sapling_rebuild_header_root_known(const struct block_index *bi)
{
    static const uint8_t zeros32[32] = {0};

    return bi && memcmp(bi->hashFinalSaplingRoot.data, zeros32, 32) != 0;
}

/* Raise (or clear) the fail-closed blocker family the boot-time "Sapling
 * tree root MISMATCH" path drives sapling_tree_rebuild() through — every
 * fail-closed reason from this function (root mismatch included) shares
 * one blocker id so operators see a single named signal instead of raw
 * log lines.
 *
 * Classification of a root mismatch depends on whether the walk could even
 * have produced the header root:
 *   - skips TOLERATED below the mismatch height ⇒ commitments are
 *     KNOWN-MISSING (their bodies are not on disk), so the rebuilt tree
 *     could not match no matter how healthy the derived state is. That is a
 *     body-availability DEPENDENCY — the cure is the anchor_kv frontier
 *     seed above or a body backfill, never operator corruption triage.
 *     This is the structural shape of a cure-seeded datadir, where bodies
 *     exist only above the cure anchor.
 *   - ZERO tolerated skips ⇒ the walk folded every in-range body and STILL
 *     disagreed with the header-committed root: a genuine derived-state
 *     disagreement, PERMANENT (unchanged).
 * Every other reason (serialize/persist plumbing) stays TRANSIENT. */
void sapling_tree_rebuild_raise_fail_blocker(
        const char *fail_reason, int fail_height, int total_commitments,
        int mismatches, const struct sapling_rebuild_skip_tally *skips)
{
    bool is_root_mismatch = fail_reason &&
        (strstr(fail_reason, "sapling_root_mismatch") != NULL ||
         strcmp(fail_reason, "tip_missing_sapling_root") == 0);
    bool body_gap = is_root_mismatch && skips && skips->total > 0;
    char reason[BLOCKER_REASON_MAX];
    if (body_gap)
        snprintf(reason, sizeof(reason),
                "sapling_tree_rebuild fail-closed reason=%s height=%d "
                "commitments=%d mismatches=%d body_gap=%d span=[%d..%d] "
                "classes=%s: bodies absent below the mismatch height, "
                "seed anchor_kv or backfill bodies",
                fail_reason ? fail_reason : "unknown", fail_height,
                total_commitments, mismatches, skips->total,
                skips->first_height, skips->last_height,
                skips->classes[0] ? skips->classes : "unknown");
    else
        snprintf(reason, sizeof(reason),
                "sapling_tree_rebuild fail-closed reason=%s height=%d "
                "commitments=%d mismatches=%d",
                fail_reason ? fail_reason : "unknown", fail_height,
                total_commitments, mismatches);
    enum blocker_class cls = body_gap ? BLOCKER_DEPENDENCY
                           : is_root_mismatch ? BLOCKER_PERMANENT
                                              : BLOCKER_TRANSIENT;
    struct blocker_record rec;
    if (blocker_init(&rec, "sapling_tree_rebuild.fail_closed",
                     "sync.sapling_tree_rebuild", cls, reason))
        blocker_set(&rec);
}

/* Per-class typed accounting for a block the replay could not fold. The old
 * code had four SILENT `continue`s (no index / no body / unmappable file /
 * data-position past the mmap / undeserializable) — a dropped block's shielded
 * commitments then vanished with ZERO accounting, surfacing only ~100k blocks
 * later as an opaque tip-root mismatch. This makes every skip a named,
 * counted event at the EXACT height, so a skipped shielded-output block is
 * never a silent gap.
 *
 * Returns true when the caller MUST fail-closed: when the rebuild endpoint is
 * the coins-applied frontier, every in-range block has by construction been
 * APPLIED (body on disk, data position valid), so a skip there is a real local
 * defect, not a legitimate header-only tail — name it and stop AT the block.
 * When the endpoint is the header tip (legacy/no coins frontier), a header-only
 * tail block genuinely has no body to fold; the skip is TOLERATED (counted +
 * throttled-logged), and the denser per-block root check below still catches
 * any skip that actually dropped commitments, at its exact height. */
bool sapling_rebuild_account_skip(const char *reason_tag, int h,
                                  bool fatal, int *counter,
                                  int *first_skip_h, int *last_skip_h)
{
    (*counter)++;
    if (*first_skip_h < 0)
        *first_skip_h = h;
    *last_skip_h = h;
    /* Throttle: log the first of each class, then every 512th, so a wide
     * header-only tail cannot spam node.log while a lone defect is still
     * always surfaced. */
    if (fatal || *counter == 1 || (*counter % 512) == 0)
        LOG_WARN("sapling_tree_rebuild",
                 "shielded verify: block h=%d skipped — reason=%s "
                 "(class_count=%d)%s", h, reason_tag, *counter,
                 fatal ? " [fail-closed: endpoint is coins-applied frontier, "
                         "every in-range block must have a foldable body]"
                       : " [tolerated: header-tip endpoint]");
    return fatal;
}

/* Resume candidate (0): the CANONICAL Sapling frontier ledger.
 *
 * app/jobs/src/utxo_apply_anchors.c:fold_sapling is the SINGLE writer of
 * anchor_kv's sapling_anchors table, and it fail-closed verifies
 * incremental_tree_root(tree) == blk->header.hashFinalSaplingRoot on every
 * shielded-commitment block it applies. Its stored frontier is therefore
 * header-bound by construction. The node_state["sapling_tree"] blob and the
 * flat-file checkpoint are SECOND copies of that same fact, and second copies
 * drift: on a cure-seeded datadir both lose their header binding at boot and
 * the replay-from-activation they fall back to is structurally impossible,
 * because the block bodies below the cure anchor are simply not on disk.
 * Reading the canonical ledger is the only resume that can succeed there —
 * per docs/ARCHITECTURE_NORTH_STAR.md, heal by reading the canonical copy,
 * never by replaying a clone.
 *
 * Lock order (docs: LOCK-ORDER LAW): anchor_kv lives in the coins/consensus
 * store, which the reducer drive owns while it holds coins_kv. This runs on a
 * background thread, so it must never take progress_store_tx_lock or the
 * reducer's csr->lock. progress_store_open_reader() returns an INDEPENDENT
 * READONLY SQLite connection to the same file (WAL permits concurrent
 * readers, 25 ms busy timeout); the seed is read once, the connection is
 * closed immediately, and NO lock is held across the multi-minute walk.
 *
 * Fail-closed binding: an anchor row carries no block hash of its own, so
 * sapling_ckpt_verify_binding is called with a NULL checkpoint hash — the
 * reorg gate is skipped and the (stronger) root gate does the work: the
 * frontier's own root MUST equal the header chain's hashFinalSaplingRoot at
 * that height. Anything else — unknown header root, mismatch, height above
 * the rebuild endpoint — is refused and the caller falls through to the
 * existing candidates. */
bool sapling_rebuild_anchor_seed(const struct active_chain *chain,
                                 int chain_tip, int sapling_height,
                                 struct incremental_merkle_tree *tree_out,
                                 int64_t *height_out)
{
    if (!chain || !tree_out || !height_out)
        return false;

    sqlite3 *rdb = progress_store_open_reader();
    if (!rdb)
        return false;  /* store not open (fixture/legacy boot) — not an error */

    struct incremental_merkle_tree t;
    sapling_tree_init(&t);
    int64_t h = -1;
    enum anchor_kv_lookup_result lr =
        anchor_kv_latest_tree(rdb, ANCHOR_POOL_SAPLING, &t, NULL, &h);
    sqlite3_close(rdb);

    if (lr != ANCHOR_KV_FOUND)
        return false;
    if (h <= (int64_t)sapling_height)
        return false;  /* nothing above activation to resume from */
    if (h > (int64_t)chain_tip) {
        LOG_WARN("sapling_tree_rebuild",
                 "sapling_tree_rebuild: refusing anchor_kv frontier h=%lld "
                 "(above rebuild endpoint %d)", (long long)h, chain_tip);
        return false;
    }

    const struct block_index *abi = active_chain_at(chain, (int)h);
    bool root_known = sapling_rebuild_header_root_known(abi);
    struct uint256 seed_root;
    incremental_tree_root(&t, &seed_root);
    enum sapling_ckpt_verdict v = sapling_ckpt_verify_binding(
        h, &seed_root, NULL, (int64_t)chain_tip, NULL, false,
        root_known ? &abi->hashFinalSaplingRoot : NULL, root_known);
    if (v != SAPLING_CKPT_OK) {
        LOG_WARN("sapling_tree_rebuild",
                 "sapling_tree_rebuild: refusing anchor_kv frontier h=%lld "
                 "(%s)", (long long)h, sapling_ckpt_verdict_str(v));
        return false;
    }

    *tree_out = t;
    *height_out = h;
    return true;
}

