/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_consensus_parity — golden assertion that zclassic23's consensus
 * constants are EXACTLY zclassicd's (the canonical C++ ZClassic daemon).
 *
 * zclassic23 must be bit-for-bit consensus-compatible with zclassicd
 * (docs/CONSENSUS_PARITY_DOCTRINE.md). The values below are golden,
 * cross-referenced against the zclassicd source (src/chainparams.cpp,
 * src/consensus/upgrades.cpp). If a future change drifts a consensus
 * constant off these values, this test fails — by design. To change a
 * value you must change zclassicd FIRST and ship it network-wide; this
 * test is the tripwire that makes silent divergence impossible.
 *
 * Companion guard: tools/scripts/check_consensus_parity.sh (lint gate
 * E13) bans the *mechanism* class (miner-signaled versionbits / dynamic
 * Equihash override). This test pins the *values*.
 */

#include "test/test_helpers.h"
#include "chain/chainparams.h"
#include "consensus/params.h"
#include "consensus/upgrades.h"
#include "core/uint256.h"
#include "domain/consensus/tx_structural.h"

/* The generated empirical oversize-grandfather table (see
 * docs/CONSENSUS_PARITY_DOCTRINE.md "Empirical oversize grandfather"),
 * included directly so the golden pins below bind the artifact, not just
 * the predicate. consensus/consensus.h is NOT included (its unguarded
 * MAX_BLOCK_SIGOPS collides with validation/main_constants.h via
 * test_helpers.h); the 102000 cap is pinned as a literal below. */
#include "../../../domain/consensus/src/oversize_grandfather_table.inc"

#include <stdio.h>
#include <string.h>

int test_consensus_parity(void)
{
    int failures = 0;

    /* Pin to mainnet — the chain whose parity with zclassicd is load-bearing. */
    chain_params_select(CHAIN_MAIN);
    const struct chain_params *p = chain_params_get();
    const struct consensus_params *c = &p->consensus;

#define CHECK(desc, cond)                                                 \
    do {                                                                  \
        printf("consensus-parity: %s... ", (desc));                       \
        if (cond) printf("OK\n");                                         \
        else { printf("FAIL\n"); failures++; }                            \
    } while (0)

    /* ── Equihash defaults (zclassicd src/chainparams.cpp: N=200, K=9) ── */
    CHECK("nEquihashN == 200", p->nEquihashN == 200);
    CHECK("nEquihashK == 9",   p->nEquihashK == 9);

    /* ── Equihash per-epoch table (zclassicd consensus/upgrades.cpp).
     * Pre-Bubbles epochs use the chain default (200,9); Bubbles/DiffAdj/
     * Buttercup carry the consensus-active (192,7). The (0,0) sentinel
     * means "fall back to the chain default". ── */
    CHECK("EquihashUpgradeInfo[BASE_SPROUT]    == default(0,0)",
          EquihashUpgradeInfo[BASE_SPROUT].N == EQUIHASH_DEFAULT_PARAMS &&
          EquihashUpgradeInfo[BASE_SPROUT].K == EQUIHASH_DEFAULT_PARAMS);
    CHECK("EquihashUpgradeInfo[UPGRADE_OVERWINTER] == default(0,0)",
          EquihashUpgradeInfo[UPGRADE_OVERWINTER].N == EQUIHASH_DEFAULT_PARAMS &&
          EquihashUpgradeInfo[UPGRADE_OVERWINTER].K == EQUIHASH_DEFAULT_PARAMS);
    CHECK("EquihashUpgradeInfo[UPGRADE_SAPLING]    == default(0,0)",
          EquihashUpgradeInfo[UPGRADE_SAPLING].N == EQUIHASH_DEFAULT_PARAMS &&
          EquihashUpgradeInfo[UPGRADE_SAPLING].K == EQUIHASH_DEFAULT_PARAMS);
    CHECK("EquihashUpgradeInfo[UPGRADE_BUBBLES]   == (192,7)",
          EquihashUpgradeInfo[UPGRADE_BUBBLES].N == 192 &&
          EquihashUpgradeInfo[UPGRADE_BUBBLES].K == 7);
    CHECK("EquihashUpgradeInfo[UPGRADE_DIFFADJ]   == (192,7)",
          EquihashUpgradeInfo[UPGRADE_DIFFADJ].N == 192 &&
          EquihashUpgradeInfo[UPGRADE_DIFFADJ].K == 7);
    CHECK("EquihashUpgradeInfo[UPGRADE_BUTTERCUP] == (192,7)",
          EquihashUpgradeInfo[UPGRADE_BUTTERCUP].N == 192 &&
          EquihashUpgradeInfo[UPGRADE_BUTTERCUP].K == 7);

    /* ── Height-aware getters resolve the table by height: 200,9 before the
     * Bubbles fork (585318), 192,7 at and after. This is the single PoW
     * parameter switch zclassicd performs, and it is HEIGHT-keyed only. ── */
    CHECK("equihash_n(pre-Bubbles 585317) == 200",
          chain_params_equihash_n(p, 585317) == 200);
    CHECK("equihash_k(pre-Bubbles 585317) == 9",
          chain_params_equihash_k(p, 585317) == 9);
    CHECK("equihash_n(Bubbles 585318) == 192",
          chain_params_equihash_n(p, 585318) == 192);
    CHECK("equihash_k(Bubbles 585318) == 7",
          chain_params_equihash_k(p, 585318) == 7);
    CHECK("equihash_n(genesis 0) == 200", chain_params_equihash_n(p, 0) == 200);

    /* ── Network-upgrade activation heights (zclassicd src/chainparams.cpp).
     * These are the agreed flag-day schedule; both nodes must use IDENTICAL
     * heights or they fork. ── */
    CHECK("BASE_SPROUT activation == ALWAYS_ACTIVE(0)",
          c->vUpgrades[BASE_SPROUT].nActivationHeight == NETWORK_UPGRADE_ALWAYS_ACTIVE);
    CHECK("UPGRADE_TESTDUMMY activation == NO_ACTIVATION(-1)",
          c->vUpgrades[UPGRADE_TESTDUMMY].nActivationHeight == NETWORK_UPGRADE_NO_ACTIVATION);
    CHECK("UPGRADE_OVERWINTER activation == 476969",
          c->vUpgrades[UPGRADE_OVERWINTER].nActivationHeight == 476969);
    CHECK("UPGRADE_SAPLING activation == 476969",
          c->vUpgrades[UPGRADE_SAPLING].nActivationHeight == 476969);
    CHECK("UPGRADE_BUBBLES activation == 585318",
          c->vUpgrades[UPGRADE_BUBBLES].nActivationHeight == 585318);
    CHECK("UPGRADE_DIFFADJ activation == 585322",
          c->vUpgrades[UPGRADE_DIFFADJ].nActivationHeight == 585322);
    CHECK("UPGRADE_BUTTERCUP activation == 707000",
          c->vUpgrades[UPGRADE_BUTTERCUP].nActivationHeight == 707000);

    /* ── Per-upgrade protocol versions (zclassicd src/chainparams.cpp). ── */
    CHECK("BASE_SPROUT protocol == 170002",
          c->vUpgrades[BASE_SPROUT].nProtocolVersion == 170002);
    CHECK("UPGRADE_OVERWINTER protocol == 170005",
          c->vUpgrades[UPGRADE_OVERWINTER].nProtocolVersion == 170005);
    CHECK("UPGRADE_SAPLING protocol == 170007",
          c->vUpgrades[UPGRADE_SAPLING].nProtocolVersion == 170007);
    CHECK("UPGRADE_BUBBLES protocol == 170009",
          c->vUpgrades[UPGRADE_BUBBLES].nProtocolVersion == 170009);
    CHECK("UPGRADE_DIFFADJ protocol == 170010",
          c->vUpgrades[UPGRADE_DIFFADJ].nProtocolVersion == 170010);
    CHECK("UPGRADE_BUTTERCUP protocol == 170011",
          c->vUpgrades[UPGRADE_BUTTERCUP].nProtocolVersion == 170011);

    /* ── PoW retargeting constants (zclassicd src/chainparams.cpp). ── */
    CHECK("nPowAveragingWindow == 17", c->nPowAveragingWindow == 17);
    CHECK("nPowMaxAdjustDown == 32",   c->nPowMaxAdjustDown == 32);
    CHECK("nPowMaxAdjustUp == 16",     c->nPowMaxAdjustUp == 16);
    CHECK("nPreButtercupPowTargetSpacing == 150",
          c->nPreButtercupPowTargetSpacing == PRE_BUTTERCUP_POW_TARGET_SPACING);
    CHECK("nPostButtercupPowTargetSpacing == 75",
          c->nPostButtercupPowTargetSpacing == POST_BUTTERCUP_POW_TARGET_SPACING);

    /* ── powLimit + genesis hash (uint256 golden values). ── */
    {
        struct uint256 expect_powlimit, expect_genesis;
        uint256_set_hex(&expect_powlimit,
            "0007ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        uint256_set_hex(&expect_genesis,
            "0007104ccda289427919efc39dc9e4d499804b7bebc22df55f8b834301260602");
        CHECK("powLimit == 0007ffff…ffff",
              uint256_eq(&c->powLimit, &expect_powlimit));
        CHECK("hashGenesisBlock == 0007104ccda2…",
              uint256_eq(&c->hashGenesisBlock, &expect_genesis));
    }

    /* ── Empirical oversize grandfather (zclassicd LIVE-behavior parity).
     * zclassicd's text caps post-Sapling tx size at 102000 unconditionally
     * (src/consensus/consensus.h:27, main.cpp:1196-1200), but the canonical
     * chain carries exactly 413 post-Sapling txs above it (heights
     * 478544..1968856; complete empirical scan 2026-06-11, re-verified
     * per-entry against zclassicd by the generator). Running nodes accept
     * them only because validated blocks are never re-checked, and enforce
     * 102000 on every new block. The pins below freeze that ruleset. ── */
    {
        struct uint256 first, last;
        uint256_set_hex(&first,
            "e3eeb123a79945cc74e6107422b124dc130ddd4b61fe5c74087317c256c79700");
        uint256_set_hex(&last,
            "e28d84f48ac642252519ad7d1ca6009cc2eb0ad2c5aaf5d0761c9105518a1db9");

        CHECK("oversize grandfather: table count == 413",
              OVERSIZE_GRANDFATHER_COUNT == 413u);
        uint32_t max_sz = 0;
        bool sorted = true;
        for (size_t i = 0; i < OVERSIZE_GRANDFATHER_COUNT; i++) {
            if (g_oversize_grandfather[i].size > max_sz)
                max_sz = g_oversize_grandfather[i].size;
            if (i > 0 &&
                memcmp(g_oversize_grandfather[i - 1].txid,
                       g_oversize_grandfather[i].txid, 32) >= 0)
                sorted = false;
        }
        CHECK("oversize grandfather: max tx size == 1922197 (h=685036)",
              max_sz == 1922197u);
        CHECK("oversize grandfather: table strictly sorted (bsearch contract)",
              sorted);
        CHECK("oversize grandfather: first violation (e3eeb123 @478544, 125811) excused",
              domain_consensus_tx_oversize_grandfathered(&first, 125811));
        CHECK("oversize grandfather: last violation (e28d84f4 @1968856, 108629) excused",
              domain_consensus_tx_oversize_grandfathered(&last, 108629));
        CHECK("oversize grandfather: wrong size NOT excused (size-exact match)",
              !domain_consensus_tx_oversize_grandfathered(&first, 125810));
    }

#undef CHECK

    if (failures)
        printf("=== consensus-parity: %d golden constant(s) drifted from "
               "zclassicd — see docs/CONSENSUS_PARITY_DOCTRINE.md ===\n", failures);
    return failures;
}
