/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * L7 LOCK-IN — shielded anchor-membership is a stub.
 *
 * Parity-audit round 2 (docs/work/parity-audit-round2-findings.md, L7):
 * coins_view_cache_have_joinsplit_requirements() is a structural stub that
 * ignores both arguments and returns true (coins_view.c:477-486). It is
 * called per-tx on the connect path (connect_block.c:477). zclassicd's
 * HaveShieldedRequirements rejects a JoinSplit/Spend whose anchor never
 * existed in chain history (GetSproutAnchorAt / GetSaplingAnchorAt,
 * coins.cpp:565-604). The zk-proof still binds the claimed root, but the
 * membership rule itself is entirely absent in zcl23: the sapling_anchors
 * table is created but never read to reject.
 *
 * THIS PIN ASSERTS THE CURRENT (LOOSENED) BEHAVIOR: a transaction carrying a
 * forged / never-existed JoinSplit anchor (and a forged Sapling spend anchor)
 * passes the requirements check today (returns true). When a real
 * anchor-membership implementation lands (parity-RESTORING per the doc; the
 * highest-priority real loosening), this assertion flips deliberately and the
 * forged-anchor tx must be rejected.
 */

#include "test/test_helpers.h"

#include "coins/coins_view.h"
#include "primitives/transaction.h"
#include "core/uint256.h"
#include "util/safe_alloc.h"

#include <stdio.h>
#include <string.h>

#define L7_CHECK(name, expr) do {                                  \
    printf("parity_lockin_anchor_membership: %s... ", (name));     \
    if ((expr)) printf("OK\n");                                    \
    else { printf("FAIL\n"); failures++; }                         \
} while (0)

int test_parity_lockin_anchor_membership(void)
{
    int failures = 0;

    /* A cache layered over a null backing view — the stub never consults it,
     * but we build it honestly so the call is exercised as the connect path
     * does. */
    struct coins_view_cache view;
    struct coins_view null_view;
    memset(&null_view, 0, sizeof(null_view));
    coins_view_cache_init(&view, &null_view);

    /* ---- PIN 1: a tx with a forged JoinSplit anchor is accepted ---- */
    {
        struct transaction tx;
        transaction_init(&tx);
        tx.num_joinsplit = 1;
        tx.v_joinsplit = zcl_calloc(1, sizeof(struct js_description),
                                    "l7_js");
        /* Arbitrary anchor that was never the root of any committed
         * Sprout note-commitment tree. */
        memset(tx.v_joinsplit[0].anchor.data, 0xDE, 32);

        bool ok = coins_view_cache_have_joinsplit_requirements(&view, &tx);
        L7_CHECK("L7 PIN: tx with forged JoinSplit anchor "
                 "-> requirements ACCEPTED today (stub returns true)",
                 ok == true);

        transaction_free(&tx);
    }

    /* ---- PIN 2: a tx with a forged Sapling spend anchor is accepted ---- */
    {
        struct transaction tx;
        transaction_init(&tx);
        tx.num_shielded_spend = 1;
        tx.v_shielded_spend = zcl_calloc(1, sizeof(struct spend_description),
                                         "l7_spend");
        memset(tx.v_shielded_spend[0].anchor.data, 0xAB, 32);

        bool ok = coins_view_cache_have_joinsplit_requirements(&view, &tx);
        L7_CHECK("L7 PIN: tx with forged Sapling spend anchor "
                 "-> requirements ACCEPTED today (stub returns true)",
                 ok == true);

        transaction_free(&tx);
    }

    /* ---- PIN 3: even a fully empty (no-shielded) tx returns true ---- *
     * Documents that the stub's verdict is independent of the tx: it never
     * inspects anchors at all. */
    {
        struct transaction tx;
        transaction_init(&tx);
        bool ok = coins_view_cache_have_joinsplit_requirements(&view, &tx);
        L7_CHECK("L7: empty tx -> requirements true (verdict ignores tx)",
                 ok == true);
        transaction_free(&tx);
    }

    coins_view_cache_free(&view);
    return failures;
}
