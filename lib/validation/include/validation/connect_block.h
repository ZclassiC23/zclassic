/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_VALIDATION_CONNECT_BLOCK_H
#define ZCL_VALIDATION_CONNECT_BLOCK_H

#include "chain/chain.h"
#include "chain/chainparams.h"
#include "coins/coins_view.h"
#include "coins/undo.h"
#include "consensus/validation.h"
#include "primitives/block.h"
#include "sapling/incremental_merkle_tree.h"
#include <stdbool.h>

/* Set the Sapling commitment tree for connect_block to update and verify.
 * Must be called before connect_block. Set to NULL for just_check mode. */
void connect_block_set_sapling_tree(struct incremental_merkle_tree *tree);

/* Connect `block` at `pindex` against `view`, applying its inputs/outputs.
 * just_check — when true, run full validation (all script/proof/conservation
 *   checks and the coins-view updates against the scratch `view`) but commit
 *   NO persistent state: connect_block returns as soon as validation succeeds
 *   without writing the best-block/tip. Used for dry-run checks of a candidate
 *   before committing. When false, the block is connected for real and its
 *   state changes are applied. Returns false (with `state` populated) on any
 *   validation failure or internal error. */
bool connect_block(const struct block *block,
                   struct validation_state *state,
                   struct block_index *pindex,
                   struct coins_view_cache *view,
                   const struct chain_params *params,
                   bool just_check);

bool disconnect_block(const struct block *block,
                      struct validation_state *state,
                      struct block_index *pindex,
                      struct coins_view_cache *view,
                      const struct block_undo *blockundo);

#endif
