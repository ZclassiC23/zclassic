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

/* ── ROM state checkpoint (the "shielded ROM keystone") ────────────────
 * The COMPLETE-state extension of the transparent-only g_sha3_checkpoint
 * above: same height and coins commitment, PLUS the full shielded fold —
 * combined Sprout+Sapling anchor history digest, both commitment-tree
 * frontier roots, and the combined nullifier history digest — folded into
 * the single rom_state_root (see checkpoints.h for the exact preimages).
 *
 * Provenance: derived from the independent from-genesis fold of the real
 * chain (producer bundle consensus-state-bundle-3056758.sqlite), and
 * re-derived from the bundle's raw rows by tools/rom_two_builder_compare
 * (`--rom-root` prints the folded rom_state_root; the two-bundle mode
 * re-computes every section digest from raw rows and asserts them against
 * the manifest). The block_hash/utxo_root/utxo_count/total_supply fields
 * are byte-identical to g_sha3_checkpoint (checked by the parity golden
 * test). Hex strings below are in INTERNAL byte order (the byte order the
 * digests are computed and stored in — the same convention as
 * g_sha3_checkpoint.sha3_hash; block_hash's display form is
 * 000002979090fba9da6cdc140d050245c1b637480609510922662407855bd653).
 *
 * ADMISSIBILITY: SATISFIED 2026-07-18 — the two-builder gate PASSED: a
 * second independent from-genesis fold (the mint3 datadir, ~23.6h fold)
 * reproduced every value below byte-identically (tools/rom_two_builder_compare
 * run over both producer bundles; rom_state_root identical from both). */
static const struct rom_state_checkpoint g_rom_state_checkpoint = {
    .height = 3056758,
    .block_hash = {
        /* 53d65b8507246622095109064837b6c14502050d14dc6cdaa9fb909097020000 */
        0x53, 0xd6, 0x5b, 0x85, 0x07, 0x24, 0x66, 0x22,
        0x09, 0x51, 0x09, 0x06, 0x48, 0x37, 0xb6, 0xc1,
        0x45, 0x02, 0x05, 0x0d, 0x14, 0xdc, 0x6c, 0xda,
        0xa9, 0xfb, 0x90, 0x90, 0x97, 0x02, 0x00, 0x00,
    },
    .utxo_root = {
        /* 5817f0ec66738db6989cf881cf37b2148d07b978fd69e5a334855b4991ac5f85 */
        0x58, 0x17, 0xf0, 0xec, 0x66, 0x73, 0x8d, 0xb6,
        0x98, 0x9c, 0xf8, 0x81, 0xcf, 0x37, 0xb2, 0x14,
        0x8d, 0x07, 0xb9, 0x78, 0xfd, 0x69, 0xe5, 0xa3,
        0x34, 0x85, 0x5b, 0x49, 0x91, 0xac, 0x5f, 0x85,
    },
    .utxo_count = 1354769,
    .total_supply = 1036413794674881LL,  /* 10364137.94674881 ZCL */
    .anchor_digest = {
        /* 1545575b79d24965eba46d16d10634fc79732c44f4a0b1099c2107f642f1285d */
        0x15, 0x45, 0x57, 0x5b, 0x79, 0xd2, 0x49, 0x65,
        0xeb, 0xa4, 0x6d, 0x16, 0xd1, 0x06, 0x34, 0xfc,
        0x79, 0x73, 0x2c, 0x44, 0xf4, 0xa0, 0xb1, 0x09,
        0x9c, 0x21, 0x07, 0xf6, 0x42, 0xf1, 0x28, 0x5d,
    },
    .anchor_count = 631645,
    .sprout_frontier_root = {
        /* fcc3621e37d169757a6e1763dbdbf5adeb9b01c824ae2f7200e980c0fe8a77f6 */
        0xfc, 0xc3, 0x62, 0x1e, 0x37, 0xd1, 0x69, 0x75,
        0x7a, 0x6e, 0x17, 0x63, 0xdb, 0xdb, 0xf5, 0xad,
        0xeb, 0x9b, 0x01, 0xc8, 0x24, 0xae, 0x2f, 0x72,
        0x00, 0xe9, 0x80, 0xc0, 0xfe, 0x8a, 0x77, 0xf6,
    },
    .sprout_frontier_height = 2124937,
    .sapling_frontier_root = {
        /* dfa46bc0c31b584a6b4f7b55c9056d9e3c57c493c4acd8303a7c7d350ff20a1a */
        0xdf, 0xa4, 0x6b, 0xc0, 0xc3, 0x1b, 0x58, 0x4a,
        0x6b, 0x4f, 0x7b, 0x55, 0xc9, 0x05, 0x6d, 0x9e,
        0x3c, 0x57, 0xc4, 0x93, 0xc4, 0xac, 0xd8, 0x30,
        0x3a, 0x7c, 0x7d, 0x35, 0x0f, 0xf2, 0x0a, 0x1a,
    },
    .sapling_frontier_height = 3056742,
    .nullifier_digest = {
        /* 347515c1849fb8964194b1fcbc87299dc738c8c727edf7093e27a5d9220f055d */
        0x34, 0x75, 0x15, 0xc1, 0x84, 0x9f, 0xb8, 0x96,
        0x41, 0x94, 0xb1, 0xfc, 0xbc, 0x87, 0x29, 0x9d,
        0xc7, 0x38, 0xc8, 0xc7, 0x27, 0xed, 0xf7, 0x09,
        0x3e, 0x27, 0xa5, 0xd9, 0x22, 0x0f, 0x05, 0x5d,
    },
    .nullifier_count = 1495126,
    .rom_state_root = {
        /* c33ddee7f4eda1b355b0df88fd9fed8cae2d889a4ed5c83920c1140e59247957 */
        0xc3, 0x3d, 0xde, 0xe7, 0xf4, 0xed, 0xa1, 0xb3,
        0x55, 0xb0, 0xdf, 0x88, 0xfd, 0x9f, 0xed, 0x8c,
        0xae, 0x2d, 0x88, 0x9a, 0x4e, 0xd5, 0xc8, 0x39,
        0x20, 0xc1, 0x14, 0x0e, 0x59, 0x24, 0x79, 0x57,
    },
};

/* Test-only override (NULL in production). See checkpoints.h. */
static const struct rom_state_checkpoint *g_rom_state_checkpoint_test_override = NULL;

const struct rom_state_checkpoint *get_rom_state_checkpoint(void)
{
    if (g_rom_state_checkpoint_test_override)
        return g_rom_state_checkpoint_test_override;
    return &g_rom_state_checkpoint;
}

void checkpoints_set_rom_state_override_for_test(
    const struct rom_state_checkpoint *cp)
{
    g_rom_state_checkpoint_test_override = cp;
}

void checkpoints_reset_rom_state_override_for_test(void)
{
    g_rom_state_checkpoint_test_override = NULL;
}
