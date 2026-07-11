/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Pure signature-operation counting. Replays from (tx, prevouts, flags).
 * No clock, RNG, allocation, or I/O. Mirrors zclassicd
 * src/main.cpp::GetLegacySigOpCount + GetP2SHSigOpCount. */

#include "domain/consensus/sigops.h"

#include "primitives/transaction.h"
#include "script/script.h"
#include "script/script_flags.h"

struct zcl_result domain_consensus_tx_legacy_sig_op_count(
        const struct transaction *tx,
        uint32_t flags,
        uint64_t *out_count)
{
    if (!tx)
        return ZCL_ERR(DOMAIN_CONSENSUS_SIGOPS_ERR_NULL_TX,
                       "legacy_sig_op_count: null tx");
    if (!out_count)
        return ZCL_ERR(DOMAIN_CONSENSUS_SIGOPS_ERR_NULL_OUT,
                       "legacy_sig_op_count: null out_count");

    uint64_t n = 0;
    for (size_t i = 0; i < tx->num_vin; i++)
        n += script_get_sig_op_count(&tx->vin[i].script_sig, flags, false);
    for (size_t i = 0; i < tx->num_vout; i++)
        n += script_get_sig_op_count(&tx->vout[i].script_pub_key, flags, false);

    *out_count = n;
    return ZCL_OK;
}

struct zcl_result domain_consensus_tx_p2sh_sig_op_count(
        const struct transaction *tx,
        const struct tx_out *const *prevouts,
        uint32_t flags,
        uint64_t *out_count)
{
    if (!tx)
        return ZCL_ERR(DOMAIN_CONSENSUS_SIGOPS_ERR_NULL_TX,
                       "p2sh_sig_op_count: null tx");
    if (!out_count)
        return ZCL_ERR(DOMAIN_CONSENSUS_SIGOPS_ERR_NULL_OUT,
                       "p2sh_sig_op_count: null out_count");

    /* P2SH flag gates the entire counter — pre-P2SH blocks historically
     * had no P2SH concept. Mirrors zclassicd's early return. */
    if ((flags & SCRIPT_VERIFY_P2SH) == 0) {
        *out_count = 0;
        return ZCL_OK;
    }
    /* Coinbase has no real inputs. */
    if (transaction_is_coinbase(tx)) {
        *out_count = 0;
        return ZCL_OK;
    }
    /* If we actually have non-coinbase inputs and P2SH is on, the
     * caller MUST supply the prevouts array. We deliberately do not
     * silently treat a NULL array as "all missing" — that would mask
     * a caller bug. */
    if (tx->num_vin > 0 && !prevouts)
        return ZCL_ERR(DOMAIN_CONSENSUS_SIGOPS_ERR_NULL_PREVOUTS,
                       "p2sh_sig_op_count: null prevouts with %zu vin",
                       tx->num_vin);

    uint64_t n = 0;
    for (size_t i = 0; i < tx->num_vin; i++) {
        const struct tx_out *prevout = prevouts[i];
        if (!prevout)
            continue;  /* Missing-prevout reported elsewhere in connect_block. */
        if (!script_is_pay_to_script_hash(&prevout->script_pub_key))
            continue;
        n += script_get_sig_op_count_p2sh(&prevout->script_pub_key,
                                           &tx->vin[i].script_sig, flags);
    }

    *out_count = n;
    return ZCL_OK;
}
