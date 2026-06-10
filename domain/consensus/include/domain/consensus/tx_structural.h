/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * domain/consensus/tx_structural.h — pure structural transaction checks.
 *
 * Context-FREE invariants on a single `struct transaction`:
 *
 *   - version / version-group-id / expiry-height bounds
 *   - vin/vout/joinsplit/shielded counts (existence + emptiness)
 *   - serialized-size cap (MAX_TX_SIZE_AFTER_SAPLING)
 *   - vout value range (>=0, <=MAX_MONEY) and running total
 *   - Sapling valueBalance range / nonzero-without-shielded-io rule
 *   - JoinSplit vpub_old/vpub_new range, both-nonzero, running totals
 *   - duplicate transparent inputs (outpoint == outpoint)
 *   - duplicate JoinSplit nullifiers (intra- and inter-joinsplit)
 *   - duplicate Sapling spend nullifiers
 *   - coinbase restrictions (no joinsplits/shielded, scriptSig 2..100 bytes)
 *   - non-coinbase: no null prevouts
 *
 * What's NOT here (deliberately left in lib/validation/):
 *   - signature, JoinSplit, or Sapling proof verification (crypto)
 *   - UTXO existence / spentness (state)
 *   - contextual locktime against block height (state)
 *   - metrics / event emission (side effects)
 *
 * Pure: no clock, no RNG, no I/O, no UTXO reads. Replays from `tx` alone.
 * The only call that allocates is the primitives helper
 * `transaction_serialize_size`, which uses a local scratch stream and
 * releases it before returning — observable purity is preserved.
 *
 * Layering: depends only on primitives/transaction.h, core/amount.h,
 * core/uint256.h, consensus/consensus.h, util/result.h. No callbacks
 * into lib/ services, no chainparams (the constants here are version-
 * level consensus, not runtime-mutable).
 */

#ifndef ZCL_DOMAIN_CONSENSUS_TX_STRUCTURAL_H
#define ZCL_DOMAIN_CONSENSUS_TX_STRUCTURAL_H

#include <stdbool.h>
#include <stdint.h>

#include "util/result.h"

struct transaction;

/* On a structural rejection we hand the caller the *exact* legacy
 * (reason, dos) pair so the wrapper can populate `validation_state`
 * byte-identically. The reject_reason is a pointer into a static
 * string literal (never owned, never freed). */
struct domain_consensus_tx_structural_failure {
    const char *reject_reason;  /* e.g. "bad-txns-vin-empty"; static literal */
    int         dos;            /* legacy DoS score */
};

/* Run all context-free structural checks against `tx`.
 *
 * On success returns ZCL_OK; if `out_failure` is non-NULL it is
 * zeroed (reject_reason=NULL, dos=0).
 *
 * On the first structural failure returns a non-ok zcl_result whose
 * .code is one of `enum domain_consensus_tx_structural_err` and
 * whose .message is a printf-formatted explanation. If `out_failure`
 * is non-NULL its `reject_reason` and `dos` fields are populated so
 * the caller can mirror the legacy validation_state semantics
 * verbatim.
 *
 * Returns DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_NULL_TX if `tx` is NULL.
 */
struct zcl_result domain_consensus_check_transaction_structural(
        const struct transaction *tx,
        struct domain_consensus_tx_structural_failure *out_failure);

/* Error codes used by domain/consensus/tx_structural.{c,h}. Stable
 * across builds; new codes are appended. Returned via zcl_result.code.
 *
 * Codes 1201..1230 cover the 1:1 mapping to the legacy reject_reason
 * tags. The contract-only code 1200 is for the NULL-tx pre-condition. */
enum domain_consensus_tx_structural_err {
    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_NULL_TX                     = 1200,

    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_VERSION_TOO_LOW              = 1201,
    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_OVERWINTER_VERSION_TOO_LOW   = 1202,
    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_VERSION_GROUP_ID             = 1203,
    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_EXPIRY_HEIGHT_TOO_HIGH       = 1204,

    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_VIN_EMPTY                    = 1205,
    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_VOUT_EMPTY                   = 1206,
    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_OVERSIZE                     = 1207,

    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_VOUT_NEGATIVE                = 1208,
    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_VOUT_TOOLARGE                = 1209,
    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_VOUT_TOTAL_TOOLARGE          = 1210,

    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_VALUEBALANCE_NONZERO         = 1211,
    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_VALUEBALANCE_TOOLARGE        = 1212,

    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_VPUB_OLD_NEGATIVE            = 1213,
    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_VPUB_NEW_NEGATIVE            = 1214,
    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_VPUB_OLD_TOOLARGE            = 1215,
    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_VPUB_NEW_TOOLARGE            = 1216,
    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_VPUBS_BOTH_NONZERO           = 1217,
    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_TXINTOTAL_TOOLARGE           = 1218,

    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_INPUTS_DUPLICATE             = 1219,
    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_JS_NULLIFIERS_DUPLICATE      = 1220,
    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_SPEND_NULLIFIERS_DUPLICATE   = 1221,

    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_CB_HAS_JOINSPLITS             = 1222,
    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_CB_HAS_SPEND_DESC             = 1223,
    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_CB_HAS_OUTPUT_DESC            = 1224,
    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_CB_LENGTH                     = 1225,
    DOMAIN_CONSENSUS_TX_STRUCTURAL_ERR_PREVOUT_NULL                  = 1226,
};

#endif /* ZCL_DOMAIN_CONSENSUS_TX_STRUCTURAL_H */
