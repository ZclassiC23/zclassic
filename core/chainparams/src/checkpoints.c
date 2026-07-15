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
/* Re-baked 2026-07-15 after the original mint captured a CORRUPT utxos
 * projection at height 3,056,758 (the ceremony had no height assert, so an
 * off-by-one projection state was committed): the old sha3/count/supply were
 * wrong for this height. The block_hash was already correct and is unchanged.
 *
 * Corrected provenance — two independent re-derivations from the preserved
 * FULL-validation producer at height 3,056,758 agree bit-for-bit:
 *   height   = 3,056,758, bestblockhash 000002979090fba9…855bd653
 *   sha3     = 5817f0ec66738db6989cf881cf37b2148d07b978fd69e5a334855b4991ac5f85
 *   txouts   = 1,354,769   (the old 1,354,771 was the h=3,056,759 count)
 *   supply   = 10364137.94674881 ZCL
 * SHA3-256 is computed over all UTXOs in (txid,vout) canonical order including
 * full scriptPubKey data.
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
        /* 5817f0ec66738db6989cf881cf37b2148d07b978fd69e5a334855b4991ac5f85 */
        0x58, 0x17, 0xf0, 0xec, 0x66, 0x73, 0x8d, 0xb6,
        0x98, 0x9c, 0xf8, 0x81, 0xcf, 0x37, 0xb2, 0x14,
        0x8d, 0x07, 0xb9, 0x78, 0xfd, 0x69, 0xe5, 0xa3,
        0x34, 0x85, 0x5b, 0x49, 0x91, 0xac, 0x5f, 0x85,
    },
    .utxo_count = 1354769,
    .total_supply = 1036413794674881LL,  /* 10364137.94674881 ZCL */
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
