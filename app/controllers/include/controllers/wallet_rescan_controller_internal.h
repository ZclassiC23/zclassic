/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Internal shared includes + wallet_ctx accessor for the wallet rescan
 * RPC controller. Included by wallet_rescan_controller*.c only. */

#ifndef ZCL_CONTROLLERS_WALLET_RESCAN_CONTROLLER_INTERNAL_H
#define ZCL_CONTROLLERS_WALLET_RESCAN_CONTROLLER_INTERNAL_H

#include "platform/time_compat.h"
#include "controllers/wallet_rescan_controller.h"
#include "controllers/rpc_chainstate_guard.h"
#include "controllers/wallet_helpers.h"
#include "controllers/strong_params.h"
#include "wallet/wallet.h"
#include "wallet/sapling_keys.h"
#include "chain/chainparams.h"
#include "encoding/utilmoneystr.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "keys/key_io.h"
#include "sapling/fast_scan.h"
#include <stdatomic.h>
#include "script/standard.h"
#include "support/cleanse.h"
#include "core/utiltime.h"
#include "core/random.h"
#include "validation/main_state.h"
#include "validation/sighash.h"
#include "validation/sync_evidence_policy.h"
#include "validation/txmempool.h"
#include "wallet/wallet_sqlite.h"
#include "net/connman.h"
#include "sapling/sapling.h"
#include "sapling/fr.h"
#include "sapling/incremental_merkle_tree.h"
#include "consensus/upgrades.h"
#include "models/database.h"
#include "models/block.h"
#include "models/utxo.h"
#include "models/wallet_key.h"
#include "models/wallet_tx.h"
#include "models/mempool_entry.h"
#include "models/peer.h"
#include "controllers/sync_controller.h"
#include "controllers/wallet_scan.h"
#include "models/chain_snapshot.h"
#include "controllers/legacy_import.h"
#include "core/serialize.h"
#include "coins/coins.h"
#include "coins/coins_view.h"
#include "views/wallet_view.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

static inline struct wallet_rpc_context *wallet_ctx(void)
{
    return wallet_rpc_context_current();
}

/* wallet_rescan_controller_coins.c — coinanalysis */
bool rpc_coinanalysis(const struct json_value *params, bool help,
                      struct json_value *result);

/* wallet_rescan_controller_witness.c — rescanwitnesses */
bool rescan_result_consensus_valid(const struct uint256 *our_root,
                                   const struct uint256 *header_root,
                                   int witness_mismatches);
bool rpc_rescanwitnesses(const struct json_value *params, bool help,
                         struct json_value *result);

#endif /* ZCL_CONTROLLERS_WALLET_RESCAN_CONTROLLER_INTERNAL_H */
