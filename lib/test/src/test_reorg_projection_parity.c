/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Reorg projection-parity PROOF — proves disconnect-side UTXO projection
 * events keep the projection coherent through a reorg.
 *
 * The utxo_projection is a SQLite UTXO set derived purely from the
 * append-only EV_UTXO_ADD / EV_UTXO_SPEND event stream. The forward path
 * (update_coins_with_undo) emits those events. Before the fix, disconnect_block
 * emitted NOTHING, so on a chain REORG the projection kept stale coins from the
 * abandoned branch while the legacy coins.db unwound.
 *
 * This test drives the projection through a real reorg and asserts it
 * CONVERGES to the same coin set as a direct build of the winning branch:
 *
 *   RUN 1 (reorg path): connect branch A (with a spend) → disconnect
 *     branch A → connect heavier branch B. All via the REAL consensus
 *     functions (update_coins_with_undo / disconnect_block), each emitting
 *     into event log L1, consumed by projection P1.
 *
 *   RUN 2 (direct path): connect ONLY branch B from the same fork point,
 *     emitting into L2, consumed by P2. P2 never sees branch A.
 *
 *   ASSERT: commitment(P1) == commitment(P2) and count(P1) == count(P2),
 *     AND every branch-A-only outpoint is ABSENT from P1.
 *
 * Without the disconnect-side emission this FAILS: P1 retains branch-A
 * coins (stale ADDs that were never countered by a SPEND), so its
 * commitment and count diverge from the directly-built P2. With the fix,
 * disconnect_block emits the inverse events (restored input -> ADD,
 * erased output -> SPEND), P1 unwinds, and the two projections converge.
 *
 * This is projection-vs-projection (SHA3 commitment over each projection's
 * derived UTXO set), so no legacy coins.db is involved.
 *
 * Block / tx / undo builders are patterned on (and intentionally mirror)
 * test_reorg_parity.c so the two proofs exercise the same chain shape:
 * the coins.db byte-exact unwind there, the event-derived projection
 * unwind here. Builders are duplicated minimally (statics there) to keep
 * this file self-contained; the chain topology is identical on purpose.
 */

#include "test/test_helpers.h"

#include "validation/connect_block.h"
#include "validation/update_coins.h"
#include "coins/coins_view.h"
#include "coins/coins.h"
#include "coins/undo.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "core/uint256.h"
#include "script/script.h"
#include "bloom/merkle.h"

#include "storage/event_log.h"
#include "storage/utxo_projection.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "util/safe_alloc.h"

#define RPP_CHECK(name, expr) do {         \
    printf("reorg_projection_parity: %s... ", (name)); \
    if ((expr)) printf("OK\n");            \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* ── Temp-dir helpers (mirror test_utxo_projection.c) ────────────── */

static void rpp_tmpdir(char *buf, size_t n, const char *tag)
{
    snprintf(buf, n, "./test-tmp/reorg_proj_parity_%d_%s", (int)getpid(), tag);
}

static int rpp_mkdir_p(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

/* ── Builders (mirror test_reorg_parity.c) ───────────────────────── */

static struct transaction make_coinbase_seeded(int height, uint8_t seed)
{
    struct transaction tx;
    memset(&tx, 0, sizeof(tx));
    tx.version = 1;
    tx.num_vin = 1;
    tx.vin = zcl_calloc(1, sizeof(struct tx_in), "rpp_cb_vin");

    uint8_t sig[6];
    sig[0] = 4;
    sig[1] = (uint8_t)(height & 0xFF);
    sig[2] = (uint8_t)((height >> 8) & 0xFF);
    sig[3] = (uint8_t)((height >> 16) & 0xFF);
    sig[4] = (uint8_t)((height >> 24) & 0xFF);
    sig[5] = seed;
    script_set(&tx.vin[0].script_sig, sig, 6);

    uint256_set_null(&tx.vin[0].prevout.hash);
    tx.vin[0].prevout.n = 0xFFFFFFFF;
    tx.vin[0].sequence = 0xFFFFFFFF;

    tx.num_vout = 1;
    tx.vout = zcl_calloc(1, sizeof(struct tx_out), "rpp_cb_vout");
    tx.vout[0].value = 1000000000LL;
    uint8_t pk[] = {0x76, 0xa9, 0x14};
    script_set(&tx.vout[0].script_pub_key, pk, 3);

    transaction_compute_hash(&tx);
    return tx;
}

static struct transaction make_spend(const struct uint256 *prev_txid,
                                     uint32_t prev_n, int64_t value,
                                     uint8_t seed)
{
    struct transaction tx;
    memset(&tx, 0, sizeof(tx));
    tx.version = 1;
    tx.num_vin = 1;
    tx.vin = zcl_calloc(1, sizeof(struct tx_in), "rpp_sp_vin");
    tx.vin[0].prevout.hash = *prev_txid;
    tx.vin[0].prevout.n = prev_n;
    uint8_t sig[2] = {0x48, seed};
    script_set(&tx.vin[0].script_sig, sig, 2);
    tx.vin[0].sequence = 0xFFFFFFFF;

    tx.num_vout = 1;
    tx.vout = zcl_calloc(1, sizeof(struct tx_out), "rpp_sp_vout");
    tx.vout[0].value = value;
    uint8_t pk[4] = {0x76, 0xa9, 0x14, seed};
    script_set(&tx.vout[0].script_pub_key, pk, 4);

    transaction_compute_hash(&tx);
    return tx;
}

static void free_block(struct block *blk)
{
    for (size_t i = 0; i < blk->num_vtx; i++) {
        free(blk->vtx[i].vin);
        free(blk->vtx[i].vout);
    }
    free(blk->vtx);
    blk->vtx = NULL;
    blk->num_vtx = 0;
}

static void make_block_txs(struct block *blk, int height,
                           const struct uint256 *prev_hash,
                           uint8_t seed,
                           struct transaction *txs, size_t ntx)
{
    memset(blk, 0, sizeof(*blk));
    blk->num_vtx = ntx;
    blk->vtx = zcl_calloc(ntx, sizeof(struct transaction), "rpp_blk_vtx");
    struct uint256 *leaf_hashes =
        zcl_calloc(ntx, sizeof(struct uint256), "rpp_blk_leaves");
    for (size_t i = 0; i < ntx; i++) {
        blk->vtx[i] = txs[i];
        leaf_hashes[i] = txs[i].hash;
    }
    blk->header.nVersion = 4;
    if (prev_hash)
        blk->header.hashPrevBlock = *prev_hash;
    blk->header.nTime = 1000000 + (uint32_t)height * 150 + seed;
    blk->header.hashMerkleRoot = compute_merkle_root(leaf_hashes, ntx);
    free(leaf_hashes);
}

/* Connect every tx in a block via update_coins_with_undo, accumulating
 * undo into the block_undo so disconnect_block can reverse it. Coinbase
 * (vtx[0]) connects with plain update_coins (no inputs). Both paths emit
 * projection events into whatever event log is currently wired global. */
static bool connect_block_with_undo(struct block *blk, int height,
                                    struct coins_view_cache *cache,
                                    struct block_undo *bu)
{
    block_undo_init(bu);
    if (blk->num_vtx > 1)
        block_undo_alloc(bu, blk->num_vtx - 1);

    update_coins(&blk->vtx[0], cache, height);

    for (size_t i = 1; i < blk->num_vtx; i++) {
        struct tx_undo txundo;
        memset(&txundo, 0, sizeof(txundo));
        if (!update_coins_with_undo(&blk->vtx[i], cache, &txundo, height)) {
            printf("[connect h=%d tx=%zu spend failed] ", height, i);
            return false;
        }
        bu->vtxundo[i - 1] = txundo;
    }
    return true;
}

/* ── Test implementation ─────────────────────────────────────────── */

int test_reorg_projection_parity(void);
int test_reorg_projection_parity(void)
{
    printf("\n=== reorg projection-parity test "
           "(projection through reorg) ===\n");
    int failures = 0;

    rpp_mkdir_p("./test-tmp");
    char dir[256];
    rpp_tmpdir(dir, sizeof(dir), "main");
    rpp_mkdir_p(dir);

    char log1_path[512], proj1_path[512], log2_path[512], proj2_path[512];
    snprintf(log1_path,  sizeof(log1_path),  "%s/events1.log",      dir);
    snprintf(proj1_path, sizeof(proj1_path), "%s/utxo_proj1.db",    dir);
    snprintf(log2_path,  sizeof(log2_path),  "%s/events2.log",      dir);
    snprintf(proj2_path, sizeof(proj2_path), "%s/utxo_proj2.db",    dir);

    /* ── Build the chain topology once (identical to test_reorg_parity).
     * The same blocks are replayed against each run's global event log. */

    /* Shared genesis fork point (height 0). */
    struct transaction g_cb = make_coinbase_seeded(0, 0x00);
    struct block genesis;
    struct uint256 g_hash;
    struct transaction g_txs[1] = { g_cb };
    make_block_txs(&genesis, 0, NULL, 0x00, g_txs, 1);
    block_header_get_hash(&genesis.header, &g_hash);
    struct uint256 g_cb_hash = genesis.vtx[0].hash;

    /* Branch A: H+1..H+3, A2 spends A1's coinbase output. */
    struct block a_blk[4];
    struct uint256 a_hash[4];
    a_hash[0] = g_hash;
    {
        struct transaction txs[1] = { make_coinbase_seeded(1, 0x10) };
        make_block_txs(&a_blk[1], 1, &a_hash[0], 0x10, txs, 1);
        block_header_get_hash(&a_blk[1].header, &a_hash[1]);
    }
    struct uint256 a1_cb_hash = a_blk[1].vtx[0].hash;
    {
        struct transaction cb = make_coinbase_seeded(2, 0x11);
        struct transaction sp = make_spend(&a1_cb_hash, 0, 900000000LL, 0x1A);
        struct transaction txs[2] = { cb, sp };
        make_block_txs(&a_blk[2], 2, &a_hash[1], 0x11, txs, 2);
        block_header_get_hash(&a_blk[2].header, &a_hash[2]);
    }
    {
        struct transaction txs[1] = { make_coinbase_seeded(3, 0x12) };
        make_block_txs(&a_blk[3], 3, &a_hash[2], 0x12, txs, 1);
        block_header_get_hash(&a_blk[3].header, &a_hash[3]);
    }

    /* Branch B: H+1..H+4 (heavier), B2 spends the SHARED genesis output. */
    struct block b_blk[5];
    struct uint256 b_hash[5];
    b_hash[0] = g_hash;
    {
        struct transaction txs[1] = { make_coinbase_seeded(1, 0x20) };
        make_block_txs(&b_blk[1], 1, &b_hash[0], 0x20, txs, 1);
        block_header_get_hash(&b_blk[1].header, &b_hash[1]);
    }
    {
        struct transaction cb = make_coinbase_seeded(2, 0x21);
        struct transaction sp = make_spend(&g_cb_hash, 0, 950000000LL, 0x2B);
        struct transaction txs[2] = { cb, sp };
        make_block_txs(&b_blk[2], 2, &b_hash[1], 0x21, txs, 2);
        block_header_get_hash(&b_blk[2].header, &b_hash[2]);
    }
    {
        struct transaction txs[1] = { make_coinbase_seeded(3, 0x22) };
        make_block_txs(&b_blk[3], 3, &b_hash[2], 0x22, txs, 1);
        block_header_get_hash(&b_blk[3].header, &b_hash[3]);
    }
    {
        struct transaction txs[1] = { make_coinbase_seeded(4, 0x23) };
        make_block_txs(&b_blk[4], 4, &b_hash[3], 0x23, txs, 1);
        block_header_get_hash(&b_blk[4].header, &b_hash[4]);
    }

    /* block_index records needed by disconnect_block (height + pprev). */
    struct block_index gi;
    block_index_init(&gi);
    gi.nHeight = 0;
    gi.phashBlock = &a_hash[0];

    struct block_index a_idx[4];
    for (int h = 1; h <= 3; h++) {
        block_index_init(&a_idx[h]);
        a_idx[h].nHeight = h;
        a_idx[h].phashBlock = &a_hash[h];
        a_idx[h].pprev = (h == 1) ? &gi : &a_idx[h - 1];
    }

    uint8_t c1[32] = {0}, c2[32] = {0};
    uint64_t count1 = 0, count2 = 0;
    bool have_c1 = false, have_c2 = false;

    /* ── RUN 1 (reorg path): A → disconnect A → B, into log L1 / P1 ── */
    {
        event_log_t *l1 = event_log_open(log1_path);
        RPP_CHECK("run1: open event log L1", l1 != NULL);
        utxo_projection_t *p1 =
            l1 ? utxo_projection_open(proj1_path, l1) : NULL;
        RPP_CHECK("run1: open projection P1", p1 != NULL);

        if (l1 && p1) {
            /* Wire the PROCESS-GLOBAL log: every update_coins /
             * disconnect_block projection emit now lands in L1. */
            utxo_projection_set_event_log(l1);

            struct coins_view_cache v;
            struct coins_view nv;
            memset(&nv, 0, sizeof(nv));
            coins_view_cache_init(&v, &nv);
            update_coins(&genesis.vtx[0], &v, 0);

            /* Connect branch A. */
            struct block_undo a_undo[4];
            bool built_a = true;
            for (int h = 1; h <= 3 && built_a; h++)
                built_a = connect_block_with_undo(&a_blk[h], h, &v, &a_undo[h]);
            RPP_CHECK("run1: branch A connects (with spend in A2)", built_a);

            /* Disconnect branch A (reverse order) — the FIX emits inverse
             * events here so P1 can unwind. */
            bool unwound = true;
            for (int h = 3; h >= 1 && unwound; h--) {
                struct validation_state vs;
                validation_state_init(&vs);
                unwound = disconnect_block(&a_blk[h], &vs, &a_idx[h],
                                           &v, &a_undo[h]);
            }
            RPP_CHECK("run1: branch A fully disconnects to fork point",
                      unwound);
            for (int h = 1; h <= 3; h++)
                block_undo_free(&a_undo[h]);

            /* Connect heavier branch B onto the unwound view. */
            struct block_undo b_undo[5];
            bool reapplied = true;
            for (int h = 1; h <= 4 && reapplied; h++)
                reapplied = connect_block_with_undo(&b_blk[h], h, &v,
                                                    &b_undo[h]);
            RPP_CHECK("run1: branch B connects after reorg", reapplied);
            for (int h = 1; h <= 4; h++)
                block_undo_free(&b_undo[h]);

            coins_view_cache_free(&v);

            /* Materialise all emitted events into P1's queryable table. */
            uint64_t off = utxo_projection_catch_up(p1);
            RPP_CHECK("run1: P1 catch_up", off != UINT64_MAX);

            count1 = utxo_projection_count(p1);
            have_c1 = (utxo_projection_commitment(p1, c1) == 0);
            RPP_CHECK("run1: P1 commitment computed", have_c1);

            /* NO LEAK: branch-A-only outpoints must be ABSENT from P1.
             * A1/A3 coinbases and A2's spend output were created on A and
             * never re-created by B; the disconnect emits must have spent
             * them out of the projection. */
            bool a1_gone = !utxo_projection_get(p1, a1_cb_hash.data, 0,
                                                NULL, NULL, 0, NULL);
            bool a2cb_gone = !utxo_projection_get(p1, a_blk[2].vtx[0].hash.data,
                                                  0, NULL, NULL, 0, NULL);
            bool a2sp_gone = !utxo_projection_get(p1, a_blk[2].vtx[1].hash.data,
                                                  0, NULL, NULL, 0, NULL);
            bool a3_gone = !utxo_projection_get(p1, a_blk[3].vtx[0].hash.data,
                                                0, NULL, NULL, 0, NULL);
            RPP_CHECK("run1: A1 coinbase absent from P1", a1_gone);
            RPP_CHECK("run1: A2 coinbase absent from P1", a2cb_gone);
            RPP_CHECK("run1: A2 spend output absent from P1", a2sp_gone);
            RPP_CHECK("run1: A3 coinbase absent from P1", a3_gone);
        }

        if (p1) utxo_projection_close(p1);
        if (l1) event_log_close(l1);
        utxo_projection_set_event_log(NULL);
    }

    /* ── RUN 2 (direct path): B only, into log L2 / P2 ──────────────── */
    {
        event_log_t *l2 = event_log_open(log2_path);
        RPP_CHECK("run2: open event log L2", l2 != NULL);
        utxo_projection_t *p2 =
            l2 ? utxo_projection_open(proj2_path, l2) : NULL;
        RPP_CHECK("run2: open projection P2", p2 != NULL);

        if (l2 && p2) {
            /* RE-set the process-global log to the SECOND independent log
             * (mandatory: the global was wired to L1 in run 1). */
            utxo_projection_set_event_log(l2);

            struct coins_view_cache v;
            struct coins_view nv;
            memset(&nv, 0, sizeof(nv));
            coins_view_cache_init(&v, &nv);
            update_coins(&genesis.vtx[0], &v, 0);

            struct block_undo b_undo[5];
            bool direct = true;
            for (int h = 1; h <= 4 && direct; h++)
                direct = connect_block_with_undo(&b_blk[h], h, &v, &b_undo[h]);
            RPP_CHECK("run2: branch B builds directly from fork point",
                      direct);
            for (int h = 1; h <= 4; h++)
                block_undo_free(&b_undo[h]);

            coins_view_cache_free(&v);

            uint64_t off = utxo_projection_catch_up(p2);
            RPP_CHECK("run2: P2 catch_up", off != UINT64_MAX);

            count2 = utxo_projection_count(p2);
            have_c2 = (utxo_projection_commitment(p2, c2) == 0);
            RPP_CHECK("run2: P2 commitment computed", have_c2);
        }

        if (p2) utxo_projection_close(p2);
        if (l2) event_log_close(l2);
        utxo_projection_set_event_log(NULL);
    }

    /* ── THE PROOF: the reorged projection equals the direct build ──── */

    bool count_eq = (count1 == count2);
    bool cmt_eq = have_c1 && have_c2 && (memcmp(c1, c2, 32) == 0);

    if (!count_eq || !cmt_eq) {
        printf("[divergence] reorged projection != direct build: "
               "count1=%" PRIu64 " count2=%" PRIu64 " commitment_match=%d "
               "(without the disconnect-side projection emission, branch-A coins "
               "would remain stale in P1)\n",
               count1, count2, cmt_eq ? 1 : 0);
    }

    printf("[values] count1=%" PRIu64 " count2=%" PRIu64
           " commitment_match=%d\n", count1, count2, cmt_eq ? 1 : 0);
    RPP_CHECK("PROOF: reorged P1 count == direct P2 count", count_eq);
    RPP_CHECK("PROOF: reorged P1 commitment == direct P2 commitment "
              "(byte-exact UTXO set via projection events)", cmt_eq);

    /* ── Cleanup ────────────────────────────────────────────────────── */
    free_block(&genesis);
    for (int h = 1; h <= 3; h++)
        free_block(&a_blk[h]);
    for (int h = 1; h <= 4; h++)
        free_block(&b_blk[h]);

    test_cleanup_tmpdir(dir);

    printf("=== reorg projection-parity: %d failures ===\n", failures);
    return failures;
}
