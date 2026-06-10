/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Chain State Validator: boot-time cross-check between coins_best_block and
 * active chain tip. */

// one-result-type-ok:single-boot-validation-result — E2 (one way out):
// the sole fallible entry point returns one domain type,
// struct boot_validation_result (carrying action + heights + coins hash).
// That is richer than zcl_result and is the domain contract its callers
// switch on; collapsing it to zcl_result would drop the decision
// payload. Failure context still travels via EV_BOOT_VALIDATION_FAILED /
// EV_RECOVERY_ACTION events emitted on every non-OK branch.

#include "services/chain_state_validator.h"
#include "validation/main_state.h"
#include "chain/chain.h"
#include "coins/coins_view.h"
#include "event/event.h"
#include "storage/progress_store.h"
#include "jobs/stage_helpers.h"
#include "jobs/tip_finalize_stage.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "util/log_macros.h"
#include "util/pprev_walk.h"

/* ActiveRecord-style validation for coins/chain agreement at boot.
 * Detects mismatch between coins_best_block and active chain tip,
 * returns the appropriate recovery action.
 * Emits EV_BOOT_VALIDATION_FAILED on mismatch. */

struct boot_validation_result validate_coins_chain_agreement(
    struct main_state *ms,
    struct coins_view_cache *cvtip,
    const char *datadir)
{
    struct boot_validation_result r = {0};

    struct block_index *chain_tip = active_chain_tip(&ms->chain_active);
    struct uint256 coins_best;
    coins_view_cache_get_best_block(cvtip, &coins_best);

    r.chain_height = chain_tip ? chain_tip->nHeight : 0;
    memcpy(&r.coins_hash, &coins_best, sizeof(r.coins_hash));
    r.coins_height = -1;

    /* Case 1: Chain at genesis or empty */
    if (!chain_tip || chain_tip->nHeight <= 0) {
        if (!uint256_is_null(&coins_best)) {
            /* coins_best_block is set but chain tip is at genesis.
             * Two scenarios:
             * a) Fresh LDB import — coins_best_block references a block
             *    that IS in our index but hasn't been set as chain tip yet.
             *    -> Reset chain to that block (BOOT_RECOVER_RESET_CHAIN).
             * b) Stale state after OOM — coins_best_block references a
             *    block NOT in our index (corrupt/partial state).
             *    -> Wipe and resync (BOOT_RECOVER_WIPE_WAIT). */
            struct block_index *coins_block = block_map_find(
                &ms->map_block_index, &coins_best);
            if (coins_block && coins_block->nHeight > 0) {
                printf("Chain at genesis but coins at h=%d — "
                       "restoring chain tip\n", coins_block->nHeight);
                event_emitf(EV_RECOVERY_ACTION, 0,
                    "action=reset_chain reason=coins_ahead_of_chain coins_h=%d",
                    coins_block->nHeight);
                r.action = BOOT_RECOVER_RESET_CHAIN;
                r.coins_height = coins_block->nHeight;
                memcpy(&r.coins_hash, &coins_best, sizeof(r.coins_hash));
            } else if (coins_block && coins_block->nHeight == 0) {
                /* Block exists in index but with nHeight=0 — this happens
                 * after LDB import when block index heights haven't been
                 * fully resolved yet. Walk pprev to find actual height. */
                struct block_index *walk = NULL;
                int walk_h = pprev_walk_depth(coins_block, 10000000,
                    "chain_state_validator.coins_resolve", &walk);
                if (walk_h < 0) walk_h = 0;
                if (walk_h > 0) {
                    printf("Post-import: coins_best_block resolved to h=%d "
                           "via pprev walk — restoring chain tip\n", walk_h);
                    event_emitf(EV_RECOVERY_ACTION, 0,
                        "action=reset_chain_pprev_walk reason=post_import coins_h=%d",
                        walk_h);
                    coins_block->nHeight = walk_h;
                    r.action = BOOT_RECOVER_RESET_CHAIN;
                    r.coins_height = walk_h;
                    memcpy(&r.coins_hash, &coins_best, sizeof(r.coins_hash));
                } else {
                    /* Can't resolve height — keep UTXOs, wait for P2P */
                    printf("Post-import: coins_best_block at h=0, no pprev "
                           "chain — will sync via P2P\n");
                    r.action = BOOT_OK;
                }
            } else {
                /* coins_best_block not in block map. Ask the recovery
                 * executor to move the cursor through CSR instead of
                 * mutating the coins view in the validator. */
                printf("Chain at genesis, coins_best_block not in index "
                       "— requesting genesis cursor reset\n");
                event_emitf(EV_RECOVERY_ACTION, 0,
                    "action=reset_coins_to_genesis reason=coins_not_in_index chain_h=%d",
                    chain_tip ? chain_tip->nHeight : 0);
                r.action = BOOT_RECOVER_RESET_COINS_TO_GENESIS;
                r.coins_height = 0;
            }
            return r;
        }
        r.action = BOOT_OK;
        return r;
    }

    /* Case 2: Coins DB empty but chain has blocks */
    if (uint256_is_null(&coins_best)) {
        /* Check if LevelDB chainstate exists for reimport */
        char cs_path[1024];
        snprintf(cs_path, sizeof(cs_path), "%s/chainstate", datadir);
        struct stat st;
        if (stat(cs_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            r.action = BOOT_RECOVER_REIMPORT;
        } else {
            r.action = BOOT_RECOVER_WIPE_WAIT;
        }
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
            "coins_empty chain_h=%d action=%s",
            r.chain_height,
            r.action == BOOT_RECOVER_REIMPORT ? "reimport" : "wipe_wait");
        return r;
    }

    /* Case 3: Coins and chain agree — all good */
    if (chain_tip->phashBlock &&
        uint256_cmp(chain_tip->phashBlock, &coins_best) == 0) {
        r.action = BOOT_OK;
        r.coins_height = r.chain_height;
        return r;
    }

    /* Case 3b (reducer-finalized tip is the coins authority): coins_best
     * disagrees with the active chain tip, BUT the active chain tip IS the
     * durable reducer-finalized tip (tip_finalize_log, in progress.kv). The runtime
     * coins authority is the coins_kv read view (g_coins_read_view, a
     * coins_view_kv over progress.kv, bound at config/src/boot.c). coins_kv is
     * authored in-txn with the tip_finalize cursor, so it IS at this finalized
     * tip by atomic co-commit; a stale/behind coins.db coins_best_block is
     * benign lagging materialization the reducer reconciles forward — NOT a
     * chain disagreement. Returning BOOT_OK here
     * stops the Case-4 RESET_CHAIN that discarded finalized progress on every
     * restart (observed: reset tip 3,135,248 -> stale coins 3,134,303, losing
     * the climb).
     *
     * STRICTLY guarded so it can never mask real corruption: AGREE only when the
     * active tip's (height,hash) BYTE-MATCHES the durable finalized tip (read via
     * the same accessor block_index_loader_rebuild uses to seed the tip, so the
     * lookahead/anchor convention is handled), AND coins.db is found behind-or-
     * equal. coins.db AHEAD of the finalized tip, or not in the index, falls
     * through to the Case-4 reset/wipe path unchanged. Read is safe here:
     * progress_store_open (boot.c:1581) precedes this validator (boot.c:2734). */
    if (chain_tip->phashBlock) {
        sqlite3 *pdb = progress_store_db();
        uint64_t fin_cursor = pdb
            ? stage_cursor_persisted(pdb, "tip_finalize", "chain_state_validator")
            : 0;
        if (fin_cursor > 0 && (int)fin_cursor - 1 == chain_tip->nHeight) {
            uint8_t fin_hash[32];
            if (tip_finalize_stage_finalized_tip_at(pdb, chain_tip->nHeight,
                                                    fin_hash) &&
                memcmp(fin_hash, chain_tip->phashBlock->data, 32) == 0) {
                struct block_index *cb = block_map_find(&ms->map_block_index,
                                                        &coins_best);
                if (cb && cb->nHeight <= chain_tip->nHeight) {
                    LOG_INFO("boot",
                             "[boot] coins_best (h=%d) behind reducer-finalized "
                             "tip (h=%d); active chain matches the finalized "
                             "authority — AGREE (coins reconciles forward, no "
                             "tip reset)",
                             cb->nHeight, chain_tip->nHeight);
                    r.action = BOOT_OK;
                    r.coins_height = r.chain_height;
                    return r;
                }
            }
        }
    }

    /* Case 4: Coins and chain disagree — find coins block in index */
    struct block_index *coins_block = block_map_find(
        &ms->map_block_index, &coins_best);

    if (coins_block) {
        r.coins_height = coins_block->nHeight;
        if (coins_block->nHeight <= chain_tip->nHeight) {
            /* Coins behind chain — reset chain to coins tip */
            r.action = BOOT_RECOVER_RESET_CHAIN;
        } else {
            /* Coins ahead of chain — unusual, wipe and resync */
            r.action = BOOT_RECOVER_WIPE_WAIT;
        }
    } else {
        /* Coins best block not in our index. This typically happens
         * when the node received P2P blocks past the last flat file
         * save, then crashed. The coins_best_block points to a block
         * received via P2P that wasn't saved to block_index.bin.
         *
         * If the chain tip is at a reasonable height (e.g. 3M+),
         * the missing hash is likely just a few blocks ahead of our
         * flat file. Reset coins to the chain tip instead of wiping the entire
         * UTXO set and reconnecting from genesis. Reducer activation will
         * unwind the few extra blocks and reconnect. */
        if (chain_tip && chain_tip->nHeight > 1000 &&
            chain_tip->phashBlock) {
            printf("Coins DB best block not in index — resetting "
                   "coins_best_block to chain tip h=%d (crash "
                   "recovery)\n", chain_tip->nHeight);
            r.action = BOOT_RECOVER_RESET_COINS_TO_CHAIN_TIP;
            r.coins_height = chain_tip->nHeight;
            memcpy(&r.coins_hash, chain_tip->phashBlock,
                   sizeof(r.coins_hash));
        } else {
            /* Chain at genesis or empty — wipe and resync */
            r.action = BOOT_RECOVER_WIPE_WAIT;
        }
    }

    event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
        "coins_chain_mismatch chain_h=%d coins_h=%d action=%s",
        r.chain_height, r.coins_height,
        r.action == BOOT_RECOVER_RESET_CHAIN ? "reset_chain" :
        r.action == BOOT_RECOVER_RESET_COINS_TO_CHAIN_TIP ? "reset_coins_to_chain_tip" :
        r.action == BOOT_RECOVER_RESET_COINS_TO_GENESIS ? "reset_coins_to_genesis" :
        r.action == BOOT_RECOVER_REIMPORT ? "reimport" : "wipe_wait");

    return r;
}
