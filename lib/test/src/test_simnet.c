/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Pins the simnet foundation slice (lib/sim/src/simnet.c): a RAM-only,
 * single-node chain harness that mints blocks through the REAL consensus
 * code (connect_block) with no disk and no real PoW.
 *
 * The keystone loop:
 *   mint coinbase  →  coin becomes spendable  →  spend the (matured)
 *   coinbase  →  input consumed + new output present, all in the in-memory
 *   coins view.
 *
 * If any of these asserts fail it means the harness assembled a block the
 * real validator rejects — the fix is ALWAYS in the harness's block
 * construction, never in a consensus predicate.
 */

#include "test/test_helpers.h"

#include "sim/simnet.h"
#include "core/uint256.h"

#include <stdio.h>
#include <string.h>

#define SN_CHECK(name, expr) do {          \
    printf("%s... ", (name));              \
    if ((expr)) printf("OK\n");            \
    else { printf("FAIL\n"); failures++; } \
} while (0)

int test_simnet(void)
{
    printf("\n=== simnet in-memory chain harness ===\n");
    int failures = 0;

    struct simnet sim;
    SN_CHECK("simnet_init succeeds", simnet_init(&sim));
    SN_CHECK("fresh tip is the synthetic base (height 99)",
             simnet_tip_height(&sim) == 99);

    /* 1. Mint a coinbase block through real connect_block. */
    struct uint256 cb_txid;
    uint256_set_null(&cb_txid);
    bool minted = simnet_mint_coinbase(&sim, &cb_txid);
    SN_CHECK("mint coinbase drives block through connect_block", minted);
    SN_CHECK("tip advanced to height 100", simnet_tip_height(&sim) == 100);
    SN_CHECK("coinbase coin is spendable in the RAM coins view",
             simnet_coin_exists(&sim, &cb_txid));

    int64_t cb_value = 0;
    SN_CHECK("coinbase output value readable from coins view",
             simnet_coin_value(&sim, &cb_txid, 0, &cb_value));
    SN_CHECK("coinbase output value is the minted subsidy stub",
             cb_value == 1000000);

    /* 2. Spend the (now matured) coinbase to a chosen output. The harness
     *    mints the spending block at a height that satisfies the REAL
     *    coinbase-maturity predicate (>= 100 blocks). */
    struct uint256 spend_txid;
    uint256_set_null(&spend_txid);
    bool spent = simnet_spend(&sim, &cb_txid, 0, 900000, &spend_txid);
    SN_CHECK("spend the matured coinbase through connect_block", spent);

    /* Maturity forces the spend block to height 100 + COINBASE_MATURITY. */
    SN_CHECK("spend block minted at a mature height (>= 200)",
             simnet_tip_height(&sim) >= 200);

    /* 3. The coins view reflects the spend: input consumed, output present. */
    SN_CHECK("spent coinbase is consumed (no live output remains)",
             !simnet_coin_exists(&sim, &cb_txid));
    SN_CHECK("new spend output is present in the coins view",
             simnet_coin_exists(&sim, &spend_txid));

    int64_t spend_value = 0;
    SN_CHECK("spend output value readable from coins view",
             simnet_coin_value(&sim, &spend_txid, 0, &spend_value));
    SN_CHECK("spend output carries the chosen value", spend_value == 900000);

    /* 4. A second independent coinbase still mints cleanly on top. */
    struct uint256 cb2_txid;
    uint256_set_null(&cb2_txid);
    bool minted2 = simnet_mint_coinbase(&sim, &cb2_txid);
    SN_CHECK("a further coinbase mints on top of the spend block", minted2);
    SN_CHECK("second coinbase coin exists", simnet_coin_exists(&sim, &cb2_txid));

    /* 5. Negative path: spending an absent coin fails cleanly (no crash). */
    struct uint256 bogus;
    memset(bogus.data, 0xAB, 32);
    struct uint256 unused;
    SN_CHECK("spending an absent coin is rejected",
             !simnet_spend(&sim, &bogus, 0, 1, &unused));

    simnet_free(&sim);
    return failures;
}
