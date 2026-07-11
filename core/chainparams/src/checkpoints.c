/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Thin lib/ adapter over the pure
 * domain/consensus/checkpoints.{c,h} primitives. Holds the single
 * clock read (`platform_time_wall_time_t`) plus the NULL-arg
 * logging policy. All matching/lookup logic is in the domain layer. */

#include "platform/time_compat.h"
#include "chain/checkpoints.h"
#include "domain/consensus/checkpoints.h"
#include "util/log_macros.h"
#include <stddef.h>

int checkpoints_get_total_blocks_estimate(const struct checkpoint_data *data)
{
    return domain_consensus_checkpoints_total_blocks_estimate(data);
}

double checkpoints_guess_verification_progress(
    const struct checkpoint_data *data,
    const struct block_index *pindex, bool fSigchecks)
{
    /* Legacy contract: NULL pindex -> 0.0. The domain primitive can't
     * see "pindex was NULL" — that's a wrapper concern, so we keep
     * the early-return here and read the clock only when there's
     * actual work to do. */
    if (pindex == NULL)
        return 0.0;

    int64_t now = platform_time_wall_time_t();
    return domain_consensus_checkpoints_progress_at_now(
            data,
            (uint64_t)pindex->nChainTx,
            block_index_get_time(pindex),
            now,
            fSigchecks);
}

/* ── Enforcement helpers ───────────────────────────────────
 *
 * Thin wrappers — the matching logic lives in the domain layer.
 * The lib/ wrapper preserves the legacy LOG_FAIL / LOG_ERR
 * observability on NULL arguments. */

bool checkpoints_hash_at_height(const struct checkpoint_data *data,
                                 int height,
                                 struct uint256 *out_hash)
{
    if (!data || !out_hash)
        LOG_FAIL("checkpoints", "hash_at_height: NULL argument (data=%p, out_hash=%p)",
                 (const void *)data, (const void *)out_hash);
    return domain_consensus_checkpoints_hash_at_height(data, height, out_hash);
}

int checkpoints_last_height(const struct checkpoint_data *data)
{
    if (!data || data->nEntries == 0)
        LOG_ERR("checkpoints", "last_height: no checkpoint data (data=%p)", (const void *)data);
    return domain_consensus_checkpoints_last_height(data);
}

bool checkpoints_validate_header(const struct checkpoint_data *data,
                                  int height,
                                  const struct uint256 *hash)
{
    return domain_consensus_checkpoints_validate_header(data, height, hash);
}

/* ── SHA3 UTXO checkpoint ──────────────────────────────── */
/* Verified bit-for-bit against zclassicd (ZclassicCommunity/zclassic)
 * at height 3,056,758 on 2026-03-26.
 *
 * Verification method:
 *   1. Both nodes at height 3,056,758, same bestblockhash
 *   2. gettxoutsetinfo: txouts=1,354,771, total=10364138.33747381 ZCL
 *   3. Confirmed PERFECT MATCH at height 3,056,763 (zero delta)
 *   4. SHA3-256 computed over all UTXOs in (txid,vout) canonical order
 *      including full scriptPubKey data
 *
 * A new node reaching this height MUST produce the same SHA3 hash.
 * If not, its UTXO set is corrupted and cannot be trusted. */

static const struct sha3_utxo_checkpoint g_sha3_checkpoint = {
    .height = 3056758,
    .block_hash = {
        /* 000002979090fba9da6cdc140d050245c1b637480609510922662407855bd653 */
        0x53, 0xd6, 0x5b, 0x85, 0x07, 0x24, 0x66, 0x22,
        0x09, 0x51, 0x09, 0x06, 0x48, 0x37, 0xb6, 0xc1,
        0x45, 0x02, 0x05, 0x0d, 0x14, 0xdc, 0x6c, 0xda,
        0xa9, 0xfb, 0x90, 0x90, 0x97, 0x02, 0x00, 0x00,
    },
    .sha3_hash = {
        /* 00e95dbd54a791a51433d68127f9975a3b1d6f8e9002b109647343ba0c83c3e0 */
        0x00, 0xe9, 0x5d, 0xbd, 0x54, 0xa7, 0x91, 0xa5,
        0x14, 0x33, 0xd6, 0x81, 0x27, 0xf9, 0x97, 0x5a,
        0x3b, 0x1d, 0x6f, 0x8e, 0x90, 0x02, 0xb1, 0x09,
        0x64, 0x73, 0x43, 0xba, 0x0c, 0x83, 0xc3, 0xe0,
    },
    .utxo_count = 1354771,
    .total_supply = 1036413833747381LL,  /* 10364138.33747381 ZCL */
};

/* Test-only override (NULL in production). See checkpoints.h. */
static const struct sha3_utxo_checkpoint *g_sha3_checkpoint_test_override = NULL;

const struct sha3_utxo_checkpoint *get_sha3_utxo_checkpoint(void)
{
    if (g_sha3_checkpoint_test_override)
        return g_sha3_checkpoint_test_override;
    return &g_sha3_checkpoint;
}

void checkpoints_set_sha3_override_for_test(const struct sha3_utxo_checkpoint *cp)
{
    g_sha3_checkpoint_test_override = cp;
}

void checkpoints_reset_sha3_override_for_test(void)
{
    g_sha3_checkpoint_test_override = NULL;
}
