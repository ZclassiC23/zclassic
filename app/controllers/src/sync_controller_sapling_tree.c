/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Sapling tree rebuild support for the sync controller.
 *
 * This file owns the long-running replay that rebuilds the persisted
 * Sapling commitment tree from block files. */

#include "controllers/sync_controller.h"
#include "sync_controller_internal.h"

#include "chain/chain.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "core/utiltime.h"
#include "jobs/reducer_frontier.h"
#include "models/database.h"
#include "primitives/block.h"
#include "sapling/incremental_merkle_tree.h"
#include "util/boot_progress.h"
#include "util/log_macros.h"
#include "validation/chainstate.h"

#include <string.h>
#include <sys/mman.h>

static bool sapling_header_root_known(const struct block_index *bi)
{
    static const uint8_t zeros32[32] = {0};

    return bi && memcmp(bi->hashFinalSaplingRoot.data, zeros32, 32) != 0;
}

int sapling_tree_rebuild(struct node_db *ndb,
                         const struct active_chain *chain,
                         const char *datadir)
{
    if (!ndb || !chain || !datadir)
        LOG_ERR("sync", "sapling_tree_rebuild: invalid args (ndb=%p, chain=%p, datadir=%p)",
                (void *)ndb, (void *)chain, (void *)datadir);

    int header_tip = active_chain_height(chain);
    int sapling_height = 476969; /* ZClassic Sapling activation */

    /* Resolve the rebuild endpoint from coins-applied state, NOT the
     * pre-fold header tip. On a wedged node the active/header tip can run
     * far ahead of the durable coins frontier (active tip << header tip);
     * the persisted hashFinalSaplingRoot above the applied frontier may be
     * absent, so verifying the rebuilt tree against the HEADER tip would
     * FATAL on `tip_missing_sapling_root` before the forward fold even runs.
     * Cap the endpoint to coins-best (coins_applied_height - 1, the durable
     * applied frontier) when that is present and lower than the header tip:
     * coins-best is by construction a block the node has APPLIED, so its body
     * is on disk (BLOCK_HAVE_DATA) and its hashFinalSaplingRoot is known, so
     * the final tip-root check has a real block to verify against. When the
     * coins frontier is ABSENT (a fresh/legacy datadir, or a unit test with no
     * progress store) the header tip is kept unchanged — the per-block replay
     * already skips header-only (non-HAVE_DATA) blocks (see :BLOCK_HAVE_DATA
     * check below), so the existing legacy-resume behavior is preserved. */
    int chain_tip = header_tip;
    {
        int32_t coins_best = -1;
        if (reducer_frontier_derive_coins_best_now(&coins_best, NULL, NULL)
            && coins_best >= 0 && coins_best < chain_tip) {
            LOG_INFO("sapling_tree_rebuild",
                     "sapling_tree_rebuild: capping endpoint to coins-applied "
                     "height %d (header tip %d)", coins_best, header_tip);
            chain_tip = coins_best;
        }
    }

    if (chain_tip < sapling_height) return 0;

    struct incremental_merkle_tree tree;
    sapling_tree_init(&tree);
    int total_commitments = 0;
    int mismatches = 0;
    int start_height = sapling_height;
    const char *fail_reason = NULL;
    int fail_height = -1;

    /* Try to resume from a persisted checkpoint to avoid replaying
     * 2.6M blocks on every crash recovery. Two candidates, most
     * authoritative first:
     *   (1) Flat-file checkpoint at <datadir>/sapling_tree_ckpt.dat
     *       flushed every 10K blocks, SHA3-verified.
     *   (2) node_state["sapling_tree"] - flushed every 100K blocks,
     *       SQLite-backed, legacy path.
     * The flat-file path short-circuits the node_state path on a hit;
     * a miss falls through to the original node_state probe. */
    int64_t ckpt_h = 0;
    {
        char ckpt_path[512];
        int n = snprintf(ckpt_path, sizeof(ckpt_path),
                         "%s/sapling_tree_ckpt.dat", datadir);
        if (n > 0 && (size_t)n < sizeof(ckpt_path)) {
            int64_t flat_h = 0;
            struct incremental_merkle_tree ff_tree;
            sapling_tree_init(&ff_tree);
            if (sapling_tree_load_checkpoint(&ff_tree, &flat_h, ckpt_path)
                && flat_h > sapling_height && flat_h <= chain_tip) {
                const struct block_index *ckpt_bi =
                    active_chain_at(chain, (int)flat_h);
                bool root_match = true;
                if (sapling_header_root_known(ckpt_bi)) {
                    struct uint256 ffr;
                    incremental_tree_root(&ff_tree, &ffr);
                    root_match = memcmp(ffr.data,
                        ckpt_bi->hashFinalSaplingRoot.data, 32) == 0;
                } else {
                    root_match = false;
                    LOG_WARN("sapling_tree_rebuild",
                             "sapling_tree_rebuild: refusing unverified "
                             "flat-file checkpoint h=%lld (missing "
                             "hashFinalSaplingRoot)",
                             (long long)flat_h);
                }
                if (root_match) {
                    tree = ff_tree;
                    start_height = (int)flat_h + 1;
                    total_commitments =
                        (int)incremental_tree_size(&tree);
                    ckpt_h = flat_h;
                    LOG_INFO("sapling_tree_rebuild", "sapling_tree_rebuild: resuming " "from flat-file checkpoint h=%lld " "(%d commitments,)", (long long)flat_h, total_commitments);
                    fflush(stderr);
                }
            }
        }
    }

    if (ckpt_h == 0
        && node_db_state_get_int(ndb, "sapling_tree_rebuild_height", &ckpt_h)
        && ckpt_h > sapling_height && ckpt_h <= chain_tip) {
        uint8_t tbuf[8192];
        size_t tlen = 0;
        if (node_db_state_get(ndb, "sapling_tree", tbuf, sizeof(tbuf), &tlen)
            && tlen > 0) {
            struct byte_stream ts;
            stream_init_from_data(&ts, tbuf, tlen);
            if (incremental_tree_deserialize(&tree, &ts)) {
                const struct block_index *ckpt_bi =
                    active_chain_at(chain, (int)ckpt_h);
                if (sapling_header_root_known(ckpt_bi)) {
                    struct uint256 ckpt_root;
                    incremental_tree_root(&tree, &ckpt_root);
                    if (memcmp(ckpt_root.data,
                               ckpt_bi->hashFinalSaplingRoot.data,
                               32) == 0) {
                        start_height = (int)ckpt_h + 1;
                        total_commitments =
                            (int)incremental_tree_size(&tree);
                        LOG_INFO("sapling_tree_rebuild", "sapling_tree_rebuild: resuming " "from checkpoint h=%d (%d commitments)", (int)ckpt_h, total_commitments);
                        fflush(stderr);
                    } else {
                        LOG_WARN("sapling_tree_rebuild",
                                 "sapling_tree_rebuild: checkpoint h=%d "
                                 "root mismatch; replaying from activation",
                                 (int)ckpt_h);
                        sapling_tree_init(&tree);
                    }
                } else {
                    LOG_WARN("sapling_tree_rebuild",
                             "sapling_tree_rebuild: refusing unverified "
                             "node_state checkpoint h=%d (missing "
                             "hashFinalSaplingRoot)",
                             (int)ckpt_h);
                    sapling_tree_init(&tree);
                }
            }
        }
    }

    LOG_INFO("sapling_tree_rebuild", "sapling_tree_rebuild: replaying h=%d..%d", start_height, chain_tip);
    fflush(stderr);

    int64_t t_replay_start = GetTimeMillis();
    int cached_file = -1;
    uint8_t *cached_data = NULL;
    size_t cached_size = 0;

    for (int h = start_height; h <= chain_tip; h++) {
        if ((h % 100) == 0)
            boot_progress_tick("sapling_tree_rebuild");
        const struct block_index *bi = active_chain_at(chain, h);
        if (!bi) continue;
        if (!(bi->nStatus & BLOCK_HAVE_DATA)) continue;

        if (bi->nFile != cached_file) {
            if (cached_data) munmap(cached_data, cached_size);
            cached_data = sync_controller_mmap_block_file(datadir, bi->nFile,
                                                          &cached_size);
            cached_file = cached_data ? bi->nFile : -1;
            if (!cached_data) continue;
        }

        if (bi->nDataPos >= cached_size) continue;

        struct block blk;
        block_init(&blk);
        size_t remaining = cached_size - bi->nDataPos;
        struct byte_stream s;
        stream_init_from_data(&s, cached_data + bi->nDataPos, remaining);
        if (!block_deserialize(&blk, &s)) {
            block_free(&blk);
            continue;
        }

        for (size_t i = 0; i < blk.num_vtx; i++) {
            const struct transaction *tx = &blk.vtx[i];
            for (size_t j = 0; j < tx->num_shielded_output; j++) {
                incremental_tree_append(&tree,
                    &tx->v_shielded_output[j].cm);
                total_commitments++;
            }
        }

        bool is_checkpoint = ((h - sapling_height) % 100000 == 0 &&
                              h > sapling_height);
        if (is_checkpoint) {
            struct uint256 computed;
            incremental_tree_root(&tree, &computed);
            if (!sapling_header_root_known(bi)) {
                fail_reason = "checkpoint_missing_sapling_root";
                fail_height = h;
            } else if (memcmp(computed.data,
                       blk.header.hashFinalSaplingRoot.data, 32) != 0) {
                mismatches++;
                fail_reason = "checkpoint_sapling_root_mismatch";
                fail_height = h;
            }
        }

        block_free(&blk);

        if (fail_reason)
            goto fail;

        if (is_checkpoint) {
            LOG_WARN("sapling_tree_rebuild", "  sapling_tree_rebuild: h=%d/%d " "commitments=%d mismatches=%d", h, chain_tip, total_commitments, mismatches);
            fflush(stderr);

            struct byte_stream ts;
            stream_init(&ts, 4096);
            if (!incremental_tree_serialize(&tree, &ts)) {
                stream_free(&ts);
                fail_reason = "serialize_checkpoint_tree_failed";
                fail_height = h;
                goto fail;
            }
            if (!node_db_state_set(ndb, "sapling_tree", ts.data, ts.size)) {
                stream_free(&ts);
                fail_reason = "persist_checkpoint_tree_failed";
                fail_height = h;
                goto fail;
            }
            if (!node_db_state_set_int(ndb, "sapling_tree_rebuild_height",
                                       (int64_t)h)) {
                stream_free(&ts);
                fail_reason = "persist_checkpoint_height_failed";
                fail_height = h;
                goto fail;
            }
            stream_free(&ts);
        }
    }

    if (cached_data) {
        munmap(cached_data, cached_size);
        cached_data = NULL;
        cached_size = 0;
    }

    /* Verify against the RESOLVED endpoint (the coins-applied frontier, or the
     * header tip when no coins frontier exists), not active_chain_tip() which
     * is always the header tip — using the header tip on a wedged node would
     * compare the rebuilt tree against a block above the applied frontier whose
     * hashFinalSaplingRoot may be absent and FATAL on `tip_missing_sapling_root`
     * before the fold runs. When the frontier is absent, active_chain_at(chain,
     * chain_tip) == active_chain_tip(chain), so the legacy path is unchanged. */
    const struct block_index *tip = active_chain_at(chain, chain_tip);
    struct uint256 final_root;
    incremental_tree_root(&tree, &final_root);
    bool tip_root_known = sapling_header_root_known(tip);
    bool match = tip_root_known && memcmp(final_root.data,
                               tip->hashFinalSaplingRoot.data, 32) == 0;

    char root_hex[65];
    uint256_get_hex(&final_root, root_hex);
    int64_t replay_ms = GetTimeMillis() - t_replay_start;
    int replayed_blocks = (chain_tip >= start_height)
                          ? (chain_tip - start_height + 1)
                          : 0;
    LOG_INFO("sapling_tree_rebuild", "sapling_tree_rebuild: replayed %d blocks in %lld ms", replayed_blocks, (long long)replay_ms);
    LOG_WARN("sapling_tree_rebuild", "sapling_tree_rebuild: DONE commitments=%d " "mismatches=%d root=%s match=%s", total_commitments, mismatches, root_hex, match ? "YES" : "NO");
    fflush(stderr);

    if (!match) {
        fail_reason = tip_root_known ? "tip_sapling_root_mismatch"
                                     : "tip_missing_sapling_root";
        fail_height = tip ? tip->nHeight : -1;
        goto fail;
    }

    {
        struct byte_stream ts;
        stream_init(&ts, 4096);
        if (!incremental_tree_serialize(&tree, &ts)) {
            stream_free(&ts);
            fail_reason = "serialize_final_tree_failed";
            fail_height = chain_tip;
            goto fail;
        }
        if (!node_db_state_set(ndb, "sapling_tree", ts.data, ts.size)) {
            stream_free(&ts);
            fail_reason = "persist_final_tree_failed";
            fail_height = chain_tip;
            goto fail;
        }
        if (!node_db_state_set_int(ndb, "sapling_tree_rebuild_height",
                                   (int64_t)chain_tip)) {
            stream_free(&ts);
            fail_reason = "persist_final_height_failed";
            fail_height = chain_tip;
            goto fail;
        }
        stream_free(&ts);
    }

    (void)tree;
    return total_commitments;

fail:
    if (cached_data) munmap(cached_data, cached_size);
    LOG_ERR("sapling_tree_rebuild",
            "sapling_tree_rebuild: fail-closed reason=%s height=%d "
            "commitments=%d mismatches=%d",
            fail_reason ? fail_reason : "unknown", fail_height,
            total_commitments, mismatches);
}
