/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Pure coinbase-transaction shaping. Replays from inputs alone.
 * No clock, RNG, allocation, or I/O. */

#include "domain/consensus/coinbase.h"

#include "consensus/params.h"
#include "consensus/upgrades.h"
#include "core/amount.h"
#include "primitives/transaction.h"
#include "script/script.h"

#include <string.h>

/* The legacy `create_new_block` writes the 3-byte BIP34 height push
 * directly into `script.data` without checking the height's range; the
 * implicit invariant is that mainnet height fits in 24 bits. We make
 * that invariant explicit and reject heights that would overflow. */
#define DCB_HEIGHT_MAX 0x00FFFFFF /* 2^24 - 1 */

/* Write [push3, h0, h1, h2] into `out->data[0..3]`. Returns the number
 * of bytes written (always 4). Caller has already validated `height`. */
static size_t dcb_write_height_prefix(struct script *out, int n_height)
{
    out->data[0] = 0x03; /* push 3 bytes */
    out->data[1] = (uint8_t)(n_height & 0xff);
    out->data[2] = (uint8_t)((n_height >> 8) & 0xff);
    out->data[3] = (uint8_t)((n_height >> 16) & 0xff);
    return 4;
}

struct zcl_result domain_consensus_coinbase_script_sig_placeholder(
        int n_height,
        struct script *out)
{
    if (!out)
        return ZCL_ERR(DOMAIN_CONSENSUS_COINBASE_ERR_NULL_OUT,
                       "coinbase_script_sig_placeholder: null out at height %d",
                       n_height);
    if (n_height < 0)
        return ZCL_ERR(DOMAIN_CONSENSUS_COINBASE_ERR_NEG_HEIGHT,
                       "coinbase_script_sig_placeholder: negative height %d",
                       n_height);
    if (n_height > DCB_HEIGHT_MAX)
        return ZCL_ERR(DOMAIN_CONSENSUS_COINBASE_ERR_HEIGHT_RANGE,
                       "coinbase_script_sig_placeholder: height %d exceeds "
                       "BIP34 3-byte range", n_height);

    size_t n = dcb_write_height_prefix(out, n_height);
    out->data[n++] = (uint8_t)OP_0;
    out->size = (uint16_t)n;
    return ZCL_OK;
}

struct zcl_result domain_consensus_coinbase_script_sig_with_extra_nonce(
        int n_height,
        uint32_t extra_nonce,
        struct script *out)
{
    if (!out)
        return ZCL_ERR(DOMAIN_CONSENSUS_COINBASE_ERR_NULL_OUT,
                       "coinbase_script_sig_with_extra_nonce: null out at "
                       "height %d", n_height);
    if (n_height < 0)
        return ZCL_ERR(DOMAIN_CONSENSUS_COINBASE_ERR_NEG_HEIGHT,
                       "coinbase_script_sig_with_extra_nonce: negative "
                       "height %d", n_height);
    if (n_height > DCB_HEIGHT_MAX)
        return ZCL_ERR(DOMAIN_CONSENSUS_COINBASE_ERR_HEIGHT_RANGE,
                       "coinbase_script_sig_with_extra_nonce: height %d "
                       "exceeds BIP34 3-byte range", n_height);

    size_t n = dcb_write_height_prefix(out, n_height);

    uint8_t en_bytes[4];
    en_bytes[0] = (uint8_t)(extra_nonce & 0xff);
    en_bytes[1] = (uint8_t)((extra_nonce >> 8) & 0xff);
    en_bytes[2] = (uint8_t)((extra_nonce >> 16) & 0xff);
    en_bytes[3] = (uint8_t)((extra_nonce >> 24) & 0xff);

    /* Minimal little-endian length; a zero nonce keeps one zero byte to
     * match the legacy `increment_extra_nonce` semantics byte-for-byte. */
    int en_len = 4;
    while (en_len > 1 && en_bytes[en_len - 1] == 0)
        en_len--;

    out->data[n++] = (uint8_t)en_len;
    memcpy(out->data + n, en_bytes, (size_t)en_len);
    n += (size_t)en_len;
    out->size = (uint16_t)n;
    return ZCL_OK;
}

struct zcl_result domain_consensus_coinbase_build(
        const struct domain_consensus_coinbase_inputs *in,
        struct transaction *out_tx)
{
    if (!out_tx)
        return ZCL_ERR(DOMAIN_CONSENSUS_COINBASE_ERR_NULL_OUT,
                       "coinbase_build: null out_tx");
    if (!in)
        return ZCL_ERR(DOMAIN_CONSENSUS_COINBASE_ERR_NULL_PARAMS,
                       "coinbase_build: null inputs");
    if (!in->params)
        return ZCL_ERR(DOMAIN_CONSENSUS_COINBASE_ERR_NULL_PARAMS,
                       "coinbase_build: null consensus_params");
    if (!in->miner_script)
        return ZCL_ERR(DOMAIN_CONSENSUS_COINBASE_ERR_NULL_SCRIPT,
                       "coinbase_build: null miner_script");
    if (in->n_height < 0)
        return ZCL_ERR(DOMAIN_CONSENSUS_COINBASE_ERR_NEG_HEIGHT,
                       "coinbase_build: negative height %d", in->n_height);
    if (in->n_height > DCB_HEIGHT_MAX)
        return ZCL_ERR(DOMAIN_CONSENSUS_COINBASE_ERR_HEIGHT_RANGE,
                       "coinbase_build: height %d exceeds BIP34 3-byte range",
                       in->n_height);
    if (in->total_fees < 0)
        return ZCL_ERR(DOMAIN_CONSENSUS_COINBASE_ERR_NEG_FEES,
                       "coinbase_build: negative fees %lld",
                       (long long)in->total_fees);
    if (in->subsidy < 0)
        return ZCL_ERR(DOMAIN_CONSENSUS_COINBASE_ERR_NEG_SUBSIDY,
                       "coinbase_build: negative subsidy %lld",
                       (long long)in->subsidy);
    if (out_tx->num_vin != 1 || out_tx->num_vout != 1 ||
        !out_tx->vin || !out_tx->vout)
        return ZCL_ERR(DOMAIN_CONSENSUS_COINBASE_ERR_NOT_PREALLOC,
                       "coinbase_build: out_tx not preallocated (vin=%zu/%p, "
                       "vout=%zu/%p; need 1/non-null each)",
                       out_tx->num_vin, (const void *)out_tx->vin,
                       out_tx->num_vout, (const void *)out_tx->vout);

    /* --- vin[0]: null prevout + placeholder height-OP_0 scriptSig. --- */
    uint256_set_null(&out_tx->vin[0].prevout.hash);
    out_tx->vin[0].prevout.n = 0xffffffff;

    struct zcl_result rs = domain_consensus_coinbase_script_sig_placeholder(
            in->n_height, &out_tx->vin[0].script_sig);
    if (!rs.ok)
        return rs;

    /* --- vout[0]: miner script + subsidy+fees. --- */
    out_tx->vout[0].script_pub_key = *in->miner_script;
    out_tx->vout[0].value = in->subsidy + in->total_fees;

    /* --- Version per epoch. Match legacy `create_new_block` precisely:
     *     - Sapling active -> v4 / 0x892F2085, overwintered=true (set by
     *       primitives helpers via the version_group_id field).
     *     - else Overwinter active -> v3 / 0x03C48270.
     *     - else: leave version/group_id as initialised (legacy code
     *       falls through to whatever transaction_init() set). --- */
    if (consensus_network_upgrade_active(in->params, in->n_height,
                                         UPGRADE_SAPLING)) {
        out_tx->version          = SAPLING_TX_VERSION;
        out_tx->version_group_id = SAPLING_VERSION_GROUP_ID;
    } else if (consensus_network_upgrade_active(in->params, in->n_height,
                                                UPGRADE_OVERWINTER)) {
        out_tx->version          = OVERWINTER_TX_VERSION;
        out_tx->version_group_id = OVERWINTER_VERSION_GROUP_ID;
    }
    out_tx->expiry_height = 0;

    transaction_compute_hash(out_tx);
    return ZCL_OK;
}
