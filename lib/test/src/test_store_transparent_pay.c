/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * A store order paid TRANSPARENTLY is credited, and only by its own payment.
 *
 * The store could only ever be paid shielded: the order address was always a
 * z-address and the merchant credited it only by decrypting the payment's
 * memo. A build with no Sapling proving backend cannot make a shielded SPEND
 * at all, so every purchase on such a build was refused. A transparent
 * payment needs no proof.
 *
 * A transparent output has nowhere to carry a memo, so the ORDER BIND is the
 * one-time address itself. That is only as tight as the memo bind while the
 * address belongs to exactly one order, so the money assertions here are the
 * ones that keep it tight — a payment must not be credited to an order it was
 * not made for, and value that is not yet confirmed, not a payment, or not
 * enough must not unlock anything.
 *
 * On the code before this group existed, the FIRST case fails: the merchant
 * reconciles a transparent order by looking for a Sapling note that does not
 * exist, finds nothing, and the order stays PENDING forever.
 *
 * Deterministic and in-process: one temporary datadir, hand-written confirmed
 * UTXOs, no network and no clock dependence. */

#include "test/test_core.h"

#include "chain/chainparams.h"
#include "config/runtime.h"
#include "controllers/store_controller.h"
#include "controllers/wallet_helpers.h"
#include "controllers/zslp_controller.h"
#include "models/block.h"
#include "models/store.h"
#include "models/wallet_tx.h"
#include "script/standard.h"
#include "services/store_buyer.h"

#include <stdio.h>
#include <string.h>

/* The customer address tokens are minted to. Mainnet form, because the
 * runner selects CHAIN_MAIN and that is what the store's validator takes. */
#define TSTP_CUSTOMER "t1YRBXKYLhrb4X8sTkBeRysAzBTMMHpUXrn"

/* Confirmation ceiling the merchant measures against is tip - 3. */
#define TSTP_TIP 100

static bool tstp_open(const char *datadir, struct node_db *ndb)
{
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/node.db", datadir);
    memset(ndb, 0, sizeof(*ndb));
    return node_db_open(ndb, path);
}

/* A tip block so the payment scan has a confirmation depth to measure. */
static bool tstp_seed_tip(struct node_db *ndb, int height)
{
    struct db_block blk;
    static uint8_t solution[] = { 0x51, 0x52 };

    memset(&blk, 0, sizeof(blk));
    memset(blk.hash, 0x11, sizeof(blk.hash));
    memset(blk.prev_hash, 0x22, sizeof(blk.prev_hash));
    memset(blk.merkle_root, 0x33, sizeof(blk.merkle_root));
    memset(blk.chain_work, 0x44, sizeof(blk.chain_work));
    blk.height = height;
    blk.time = 123456789u;
    blk.bits = 0x1d00ffffu;
    blk.status = 3;
    blk.solution = solution;
    blk.solution_len = sizeof(solution);
    return db_block_save(ndb, &blk);
}

/* Build the t-address for `hash160` AND hand back the same 20 bytes, so the
 * address the order binds to and the UTXO the wallet holds cannot drift
 * apart in the fixture the way they could if either were hand-written. */
static bool tstp_taddr_for(uint8_t tag, char *addr_out, size_t addr_max,
                           uint8_t hash160_out[20])
{
    struct tx_destination dest;

    memset(hash160_out, tag, 20);
    memset(&dest, 0, sizeof(dest));
    dest.type = DEST_KEY_ID;
    memcpy(dest.id.key.id.data, hash160_out, 20);
    return wallet_encode_destination(&dest, addr_out, addr_max);
}

/* A confirmed, unspent, non-coinbase transparent output the wallet holds at
 * `hash160` — what a real buyer's payment looks like once it is in a block
 * and the wallet has scanned it. */
static bool tstp_seed_utxo(struct node_db *ndb, const uint8_t hash160[20],
                           int64_t value, int height, bool is_coinbase,
                           uint8_t txid_tag)
{
    struct db_wallet_utxo u;
    static uint8_t script[] = { 0x76, 0xa9, 0x14 };

    memset(&u, 0, sizeof(u));
    memset(u.txid, txid_tag, sizeof(u.txid));
    u.vout = 0;
    u.value = value;
    memcpy(u.address_hash, hash160, 20);
    u.script = script;
    u.script_len = sizeof(script);
    u.height = height;
    u.is_coinbase = is_coinbase;
    return db_wallet_utxo_save(ndb, &u);
}

/* Write a PENDING order bound to `payment_addr`. Returns its id, 0 on
 * failure. Deliberately writes the row rather than going through the HTTP
 * order route: this group is about what the merchant CREDITS, and the order
 * route's CSRF + proof-of-work admission is already proven elsewhere. */
static int64_t tstp_place_order(struct node_db *ndb, int64_t product_id,
                                const char *payment_addr, int64_t amount)
{
    struct db_store_order order;

    memset(&order, 0, sizeof(order));
    order.product_id = product_id;
    (void)snprintf(order.customer_addr, sizeof(order.customer_addr), "%s",
                   TSTP_CUSTOMER);
    (void)snprintf(order.payment_addr, sizeof(order.payment_addr), "%s",
                   payment_addr);
    order.amount_zatoshi = amount;
    order.status = STORE_ORDER_PENDING;
    if (!db_store_order_save(ndb, &order))
        return 0;
    return order.id;
}

static int tstp_order_status(const char *datadir, int64_t order_id)
{
    struct node_db ndb;
    struct db_store_order_view view;
    int status = -1;

    if (!tstp_open(datadir, &ndb))
        return -1;
    if (db_store_order_find_view(&ndb, order_id, &view))
        status = view.status;
    node_db_close(&ndb);
    return status;
}

int test_store_transparent_pay(void)
{
    int failures = 0;
    char datadir[256];
    struct node_db ndb;
    int64_t product_id = 0;
    int64_t price = 0;

    printf("\n=== store transparent payment ===\n");

    test_make_tmpdir(datadir, sizeof(datadir), "store_transparent_pay",
                     "main");

    /* Warm the store so its schema exists and the demo catalog is seeded —
     * the reconcile joins orders to a product for its token id, so an order
     * against no product is never scanned. */
    {
        struct store_buyer_offer offers[8];
        size_t n = 0;
        if (!store_buyer_catalog(datadir, offers, 8, &n).ok || n == 0) {
            printf("store_transparent_pay: FAIL (no catalog to order from)\n");
            return failures + 1;
        }
        product_id = offers[0].product_id;
        price = offers[0].price_zatoshi;
    }

    if (!tstp_open(datadir, &ndb)) {
        printf("store_transparent_pay: FAIL (could not open node.db)\n");
        return failures + 1;
    }
    if (!tstp_seed_tip(&ndb, TSTP_TIP)) {
        printf("store_transparent_pay: FAIL (could not seed a tip)\n");
        node_db_close(&ndb);
        return failures + 1;
    }

    /* ── 1. A confirmed transparent payment credits its order ───────────
     *
     * THE regression this group holds. Before transparent orders existed the
     * merchant reconciled every order by hunting for a Sapling note, so this
     * order stayed PENDING no matter how much transparent value arrived. */
    printf("store_transparent_pay: a confirmed t-payment credits the order"
           "... ");
    {
        char addr[128] = "";
        uint8_t h160[20];
        int64_t order_id = 0;
        bool ok = tstp_taddr_for(0xA1, addr, sizeof(addr), h160);

        if (ok) {
            order_id = tstp_place_order(&ndb, product_id, addr, price);
            ok = order_id > 0;
        }
        /* Confirmed: height 50 is well under the tip-3 ceiling of 97. */
        ok = ok && tstp_seed_utxo(&ndb, h160, price, 50, false, 0xE1);
        node_db_close(&ndb);

        if (ok) {
            store_process_payments(datadir);
            ok = tstp_order_status(datadir, order_id) == STORE_ORDER_SENT;
        }
        if (ok) {
            printf("OK\n");
        } else {
            printf("FAIL\n");
            failures++;
        }
        if (!tstp_open(datadir, &ndb)) {
            printf("store_transparent_pay: FAIL (could not reopen node.db)\n");
            return failures + 1;
        }
    }

    /* ── 2. A payment to ANOTHER order's address does not credit this one ─
     *
     * The money assertion. The one-time address IS the order bind, so an
     * order whose own address was never paid must stay unpaid even while the
     * wallet is holding a perfectly good confirmed payment for a different
     * order. */
    printf("store_transparent_pay: another order's payment does not credit"
           "... ");
    {
        char paid_addr[128] = "", unpaid_addr[128] = "";
        uint8_t paid_h160[20], unpaid_h160[20];
        int64_t unpaid_order = 0;
        bool ok = tstp_taddr_for(0xB2, paid_addr, sizeof(paid_addr),
                                 paid_h160);
        ok = ok && tstp_taddr_for(0xB3, unpaid_addr, sizeof(unpaid_addr),
                                  unpaid_h160);
        if (ok) {
            unpaid_order = tstp_place_order(&ndb, product_id, unpaid_addr,
                                            price);
            ok = unpaid_order > 0;
        }
        /* Real, confirmed, sufficient — but paid to a DIFFERENT address. */
        ok = ok && tstp_seed_utxo(&ndb, paid_h160, price * 10, 50, false,
                                  0xE2);
        node_db_close(&ndb);

        if (ok) {
            store_process_payments(datadir);
            ok = tstp_order_status(datadir, unpaid_order) ==
                 STORE_ORDER_PENDING;
        }
        if (ok) {
            printf("OK\n");
        } else {
            printf("FAIL\n");
            failures++;
        }
        if (!tstp_open(datadir, &ndb)) {
            printf("store_transparent_pay: FAIL (could not reopen node.db)\n");
            return failures + 1;
        }
    }

    /* ── 3. Value too shallow to be confirmed does not credit ───────────
     *
     * The reorg control: the merchant mints tokens it cannot take back, so
     * it must not act on value that a short reorg could still remove. */
    printf("store_transparent_pay: an unconfirmed payment does not credit"
           "... ");
    {
        char addr[128] = "";
        uint8_t h160[20];
        int64_t order_id = 0;
        bool ok = tstp_taddr_for(0xC4, addr, sizeof(addr), h160);

        if (ok) {
            order_id = tstp_place_order(&ndb, product_id, addr, price);
            ok = order_id > 0;
        }
        /* Height 99 is above the tip-3 ceiling of 97 — one confirmation. */
        ok = ok && tstp_seed_utxo(&ndb, h160, price, 99, false, 0xE3);
        node_db_close(&ndb);

        if (ok) {
            store_process_payments(datadir);
            ok = tstp_order_status(datadir, order_id) == STORE_ORDER_PENDING;
        }
        if (ok) {
            printf("OK\n");
        } else {
            printf("FAIL\n");
            failures++;
        }
        if (!tstp_open(datadir, &ndb)) {
            printf("store_transparent_pay: FAIL (could not reopen node.db)\n");
            return failures + 1;
        }
    }

    /* ── 4. Block subsidy at the order address is not a payment ─────────
     *
     * A coinbase output crediting the order address is the merchant paying
     * itself, not a buyer paying. Counting it would hand out the goods for
     * free to whoever asked for an order while a block was being mined. */
    printf("store_transparent_pay: coinbase at the order address is not a "
           "payment... ");
    {
        char addr[128] = "";
        uint8_t h160[20];
        int64_t order_id = 0;
        bool ok = tstp_taddr_for(0xD5, addr, sizeof(addr), h160);

        if (ok) {
            order_id = tstp_place_order(&ndb, product_id, addr, price);
            ok = order_id > 0;
        }
        ok = ok && tstp_seed_utxo(&ndb, h160, price * 5, 50, true, 0xE4);
        node_db_close(&ndb);

        if (ok) {
            store_process_payments(datadir);
            ok = tstp_order_status(datadir, order_id) == STORE_ORDER_PENDING;
        }
        if (ok) {
            printf("OK\n");
        } else {
            printf("FAIL\n");
            failures++;
        }
        if (!tstp_open(datadir, &ndb)) {
            printf("store_transparent_pay: FAIL (could not reopen node.db)\n");
            return failures + 1;
        }
    }

    /* ── 5. Underpaying does not credit ─────────────────────────────── */
    printf("store_transparent_pay: an underpayment does not credit... ");
    {
        char addr[128] = "";
        uint8_t h160[20];
        int64_t order_id = 0;
        bool ok = tstp_taddr_for(0xE6, addr, sizeof(addr), h160);

        if (ok) {
            order_id = tstp_place_order(&ndb, product_id, addr, price);
            ok = order_id > 0;
        }
        ok = ok && price > 1;
        ok = ok && tstp_seed_utxo(&ndb, h160, price - 1, 50, false, 0xE5);
        node_db_close(&ndb);

        if (ok) {
            store_process_payments(datadir);
            ok = tstp_order_status(datadir, order_id) == STORE_ORDER_PENDING;
        }
        if (ok) {
            printf("OK\n");
        } else {
            printf("FAIL\n");
            failures++;
        }
        if (!tstp_open(datadir, &ndb)) {
            printf("store_transparent_pay: FAIL (could not reopen node.db)\n");
            return failures + 1;
        }
    }

    /* ── 6. Crediting is idempotent across a re-run ─────────────────────
     *
     * The reconcile runs every 30 s forever. An order it already credited
     * must not be credited a second time, and a restart mid-flight must land
     * on the same answer — the durable order status is what makes a crashed
     * merchant resume rather than double-deliver. */
    printf("store_transparent_pay: re-running the reconcile does not "
           "re-credit... ");
    {
        char addr[128] = "";
        uint8_t h160[20];
        int64_t order_id = 0;
        bool ok = tstp_taddr_for(0xF7, addr, sizeof(addr), h160);

        if (ok) {
            order_id = tstp_place_order(&ndb, product_id, addr, price);
            ok = order_id > 0;
        }
        ok = ok && tstp_seed_utxo(&ndb, h160, price, 50, false, 0xE6);
        node_db_close(&ndb);

        if (ok) {
            store_process_payments(datadir);
            ok = tstp_order_status(datadir, order_id) == STORE_ORDER_SENT;
        }
        if (ok) {
            uint64_t after_first =
                zslp_balance(datadir, "", TSTP_CUSTOMER);
            /* Whatever the first pass credited, a second pass must leave
             * alone: the order is no longer PENDING, so it is not scanned. */
            store_process_payments(datadir);
            ok = tstp_order_status(datadir, order_id) == STORE_ORDER_SENT &&
                 zslp_balance(datadir, "", TSTP_CUSTOMER) == after_first;
        }
        if (ok) {
            printf("OK\n");
        } else {
            printf("FAIL\n");
            failures++;
        }
        if (!tstp_open(datadir, &ndb)) {
            printf("store_transparent_pay: FAIL (could not reopen node.db)\n");
            return failures + 1;
        }
    }

    node_db_close(&ndb);
    return failures;
}
