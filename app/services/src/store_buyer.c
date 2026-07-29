/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Store BUYER service — see services/store_buyer.h for what this is and why
 * it drives the shipped store surfaces rather than shortcutting past them. */

#include "services/store_buyer.h"

#include "controllers/store_controller.h"
#include "controllers/sovereignty_controller.h"
#include "chain/chainparams.h"
#include "config/runtime.h"
#include "crypto/sha3.h"
#include "encoding/utilstrencodings.h"
#include "controllers/wallet_shielded_controller.h"
#include "models/store.h"
#include "net/puzzle.h"
#include "sapling/sapling_prover.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "wallet/wallet.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SB_TAG "store_buyer"

/* One store HTTP exchange fits in this. The gated-download response is the
 * largest: STORE_BLOB_INLINE_MAX bytes of payload plus headers, and the
 * dynhost buffer the live path uses is 64 KiB, so anything the real store
 * can answer with fits here too. Heap, not stack — it is 96 KiB. */
enum { SB_RESP_MAX = 96 * 1024 };

/* ── status vocabulary ──────────────────────────────────────────────── */

const char *store_buyer_status_code(enum store_buyer_status st)
{
    switch (st) {
    case STORE_BUYER_OK:                    return "OK";
    case STORE_BUYER_ERR_ARGS:              return "INVALID_ARGS";
    case STORE_BUYER_ERR_MAINNET_REFUSED:   return "MAINNET_REFUSED";
    case STORE_BUYER_ERR_DB:                return "NODE_DB_UNAVAILABLE";
    case STORE_BUYER_ERR_UNKNOWN_PRODUCT:   return "UNKNOWN_PRODUCT";
    case STORE_BUYER_ERR_ORDER_CREATE_FAILED: return "ORDER_CREATE_FAILED";
    case STORE_BUYER_ERR_UNKNOWN_PURCHASE:  return "UNKNOWN_PURCHASE";
    case STORE_BUYER_ERR_ALREADY_PAID:      return "ALREADY_PAID";
    case STORE_BUYER_ERR_PROVER_UNAVAILABLE: return "PROVER_UNAVAILABLE";
    case STORE_BUYER_ERR_SPEND_REFUSED:     return "SPEND_REFUSED";
    case STORE_BUYER_ERR_INSUFFICIENT_FUNDS: return "INSUFFICIENT_FUNDS";
    case STORE_BUYER_ERR_PAYMENT_NOT_CONFIRMED: return "PAYMENT_NOT_CONFIRMED";
    case STORE_BUYER_ERR_DELIVERY_FAILED:   return "DELIVERY_FAILED";
    case STORE_BUYER_ERR_HASH_MISMATCH:     return "HASH_MISMATCH";
    case STORE_BUYER_ERR_WRITE_FAILED:      return "WRITE_FAILED";
    case STORE_BUYER_ERR_INTERNAL:          return "INTERNAL";
    }
    return "UNKNOWN";
}

const char *store_buyer_status_message(enum store_buyer_status st)
{
    switch (st) {
    case STORE_BUYER_OK:
        return "ok";
    case STORE_BUYER_ERR_ARGS:
        return "the request is missing a required value or one is out of range";
    case STORE_BUYER_ERR_MAINNET_REFUSED:
        return "scripted store purchases are refused on mainnet; "
               "run this on regtest or testnet";
    case STORE_BUYER_ERR_DB:
        return "the node database is not open";
    case STORE_BUYER_ERR_UNKNOWN_PRODUCT:
        return "no active product with that id";
    case STORE_BUYER_ERR_ORDER_CREATE_FAILED:
        return "the store refused to create the order";
    case STORE_BUYER_ERR_UNKNOWN_PURCHASE:
        return "no purchase with that id";
    case STORE_BUYER_ERR_ALREADY_PAID:
        return "a payment for this purchase was already submitted";
    case STORE_BUYER_ERR_PROVER_UNAVAILABLE:
        return "this build has no Sapling proving backend, so it cannot "
               "send a shielded payment";
    case STORE_BUYER_ERR_SPEND_REFUSED:
        return "the node refuses to spend from this tip";
    case STORE_BUYER_ERR_INSUFFICIENT_FUNDS:
        return "the wallet cannot cover the order amount";
    case STORE_BUYER_ERR_PAYMENT_NOT_CONFIRMED:
        return "the merchant has not credited this order yet";
    case STORE_BUYER_ERR_DELIVERY_FAILED:
        return "the store did not return the purchased bytes";
    case STORE_BUYER_ERR_HASH_MISMATCH:
        return "the delivered bytes do not match the product content hash; "
               "nothing was written";
    case STORE_BUYER_ERR_WRITE_FAILED:
        return "the verified bytes could not be written to the output path";
    case STORE_BUYER_ERR_INTERNAL:
        return "the node ran out of working memory for this purchase step";
    }
    return "unknown store buyer status";
}

bool store_buyer_status_is_terminal(enum store_buyer_status st)
{
    switch (st) {
    /* Stuck: retrying the same purchase cannot change the answer without
     * an operator changing something first. */
    case STORE_BUYER_ERR_HASH_MISMATCH:
    case STORE_BUYER_ERR_DELIVERY_FAILED:
    case STORE_BUYER_ERR_UNKNOWN_PRODUCT:
        return true;
    /* Everything else is a "not yet" or an operator-fixable refusal
     * (no prover, no funds, guard closed, not confirmed). Those must NOT
     * bury the row in FAILED — the purchase is still resumable, and a
     * paid-but-uncollected row losing its stage is exactly the outcome
     * this service exists to prevent. */
    default:
        return false;
    }
}

/* ── shared preconditions ───────────────────────────────────────────── */

/* Refuse mainnet. This is the whole-service guard: a scripted buyer that
 * mints orders and submits shielded sends is a development and proof surface,
 * and the cost of it running against the real chain is real value leaving a
 * wallet with no human in the loop. Checked before any order is created and
 * again before any payment is prepared, so neither entry point can be
 * reached on mainnet through a caller that skipped the other. */
static bool store_buyer_network_allows(void)
{
    const struct chain_params *cp = chain_params_get();
    return cp && strcmp(cp->strNetworkID, "main") != 0;
}

/* Open the node database that backs `datadir`'s store. A runtime reopen, not
 * a boot ceremony — the same call the store's own request path makes. */
static bool sb_open_db(const char *datadir, struct node_db *ndb,
                       const char *tag)
{
    char db_path[1024];
    if (!datadir || !*datadir || !ndb)
        return false;
    (void)snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    memset(ndb, 0, sizeof(*ndb));
    return node_db_open_runtime(ndb, db_path, tag);
}

/* Load one purchase row. Returns the typed reason on failure. */
static enum store_buyer_status sb_load_purchase(struct node_db *ndb,
                                                int64_t purchase_id,
                                                struct db_store_purchase *out)
{
    if (purchase_id <= 0)
        return STORE_BUYER_ERR_ARGS;
    if (!db_store_purchase_find(ndb, purchase_id, out))
        return STORE_BUYER_ERR_UNKNOWN_PURCHASE;
    return STORE_BUYER_OK;
}

/* ── catalog ────────────────────────────────────────────────────────── */

enum store_buyer_status store_buyer_catalog(const char *datadir,
                                            struct store_buyer_offer *out,
                                            size_t max, size_t *n_out)
{
    struct node_db ndb;
    struct db_store_product *rows;
    int count;

    if (n_out)
        *n_out = 0;
    if (!datadir || !out || max == 0 || !n_out)
        return STORE_BUYER_ERR_ARGS;
    if (!sb_open_db(datadir, &ndb, "store_buyer.catalog"))
        return STORE_BUYER_ERR_DB;

    rows = zcl_calloc(max, sizeof(*rows), "store_buyer_catalog_rows");
    if (!rows) {
        node_db_close(&ndb);
        LOG_RETURN(STORE_BUYER_ERR_INTERNAL, SB_TAG,
                   "catalog: could not allocate %zu product rows", max);
    }

    count = db_store_product_list_active(&ndb, rows, max);
    for (int i = 0; i < count; i++) {
        /* list_active does not read content_hash; re-read the row by id so
         * the buyer learns whether there is a file to collect at all. That
         * is the difference between "buy this" and "buy this and get an
         * HTML page", and it decides whether a collect can ever succeed. */
        struct db_store_product full;
        bool have_full = db_store_product_find_active(&ndb, rows[i].id, &full);
        out[i].product_id = rows[i].id;
        (void)snprintf(out[i].name, sizeof(out[i].name), "%s", rows[i].name);
        (void)snprintf(out[i].token_id, sizeof(out[i].token_id), "%s",
                       rows[i].token_id);
        out[i].price_zatoshi = rows[i].price_zatoshi;
        out[i].tokens_per_purchase = rows[i].tokens_per_purchase;
        out[i].has_content = have_full && full.has_content;
    }
    free(rows);
    node_db_close(&ndb);
    *n_out = (size_t)(count > 0 ? count : 0);
    return STORE_BUYER_OK;
}

/* ── order-create: the browser's job, done by a program ─────────────── */

/* Read the value of `attr='...'` out of an already-fetched page. */
static bool sb_scrape_attr(const char *page, const char *attr,
                           char *out, size_t out_size)
{
    char needle[64];
    const char *p, *end;
    size_t len;

    (void)snprintf(needle, sizeof(needle), "%s='", attr);
    p = strstr(page, needle);
    if (!p)
        return false;
    p += strlen(needle);
    end = strchr(p, '\'');
    if (!end)
        return false;
    len = (size_t)(end - p);
    if (len >= out_size)
        return false;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

/* Fetch the product page once and take from it BOTH things the order form
 * carries: the CSRF token and the live proof-of-work challenge. One fetch,
 * because each render issues a fresh challenge — reading the two from
 * different pages would submit a nonce for a seed the server has moved past.
 *
 * The puzzle is solved with puzzle_solve_random, the same primitive
 * store_pow_verify_and_claim checks against, from a random start (a search
 * from zero is a pure function of the challenge, so two orders for one
 * product inside one second would produce the same nonce and the store's
 * single-use ring would refuse the second).
 *
 * Note what is deliberately NOT done here: the difficulty ramp is not reset
 * and the gate is not bypassed. A scripted buyer pays the same admission
 * cost as a browser, which is the point of the gate. */
static bool sb_solve_order_form(const char *datadir, int64_t product_id,
                                char *csrf, size_t csrf_max,
                                char *pow_ts, size_t ts_max,
                                char *pow_nonce, size_t nonce_max)
{
    uint8_t *page;
    char path[64];
    char seed_hex[65], token_hex[65], ts_str[32], bits_str[16];
    uint8_t seed[32], token[32];
    uint64_t nonce = 0;
    int64_t ts;
    int bits;
    size_t n;
    bool ok = false;

    page = zcl_malloc(SB_RESP_MAX, "store_buyer_product_page");
    if (!page)
        return false;

    (void)snprintf(path, sizeof(path), "/store/product/%lld",
                   (long long)product_id);
    n = store_handle_request("GET", path, NULL, 0, page, SB_RESP_MAX, datadir);
    if (n == 0)
        goto out;
    page[(n < SB_RESP_MAX) ? n : (SB_RESP_MAX - 1)] = '\0';

    if (!sb_scrape_attr((const char *)page, "name='csrf_token' value",
                        csrf, csrf_max) ||
        !sb_scrape_attr((const char *)page, "data-pow-seed",
                        seed_hex, sizeof(seed_hex)) ||
        !sb_scrape_attr((const char *)page, "data-pow-token",
                        token_hex, sizeof(token_hex)) ||
        !sb_scrape_attr((const char *)page, "data-pow-ts",
                        ts_str, sizeof(ts_str)) ||
        !sb_scrape_attr((const char *)page, "data-pow-bits",
                        bits_str, sizeof(bits_str)))
        goto out;
    if (strlen(seed_hex) != 64 || strlen(token_hex) != 64)
        goto out;
    if (ParseHex(seed_hex, seed, sizeof(seed)) != sizeof(seed) ||
        ParseHex(token_hex, token, sizeof(token)) != sizeof(token))
        goto out;

    ts = strtoll(ts_str, NULL, 10);
    bits = (int)strtol(bits_str, NULL, 10);
    if (bits <= 0)
        goto out;
    if (!puzzle_solve_random(seed, token, ts, bits, &nonce))
        goto out;

    (void)snprintf(pow_ts, ts_max, "%lld", (long long)ts);
    (void)snprintf(pow_nonce, nonce_max, "%llu", (unsigned long long)nonce);
    ok = true;
out:
    free(page);
    return ok;
}

/* Pull the order id out of the store's "Order #<n>" payment page. That
 * heading is the response's identity, and everything else the buyer needs
 * (payment address, amount) is then read from the order row rather than
 * scraped — the memo format is a contract of the merchant's matcher
 * (db_store_received_payment_for_memo), not of this HTML. */
static bool sb_scrape_order_id(const char *page, int64_t *out)
{
    const char *p = strstr(page, "Order #");
    char *end = NULL;
    long long v;

    if (!p)
        return false;
    p += strlen("Order #");
    v = strtoll(p, &end, 10);
    if (!end || end == p || v <= 0)
        return false;
    *out = (int64_t)v;
    return true;
}

enum store_buyer_status store_buyer_order(const char *datadir,
                                          int64_t product_id,
                                          const char *customer_addr,
                                          const char *output_path,
                                          struct store_buyer_order *out)
{
    struct node_db ndb;
    struct db_store_product product;
    struct db_store_order_view order_view;
    struct db_store_purchase purchase;
    char csrf[80] = "", pow_ts[32] = "", pow_nonce[32] = "";
    char body[512];
    uint8_t *resp = NULL;
    size_t n;
    int64_t order_id = 0;
    enum store_buyer_status st = STORE_BUYER_ERR_ORDER_CREATE_FAILED;

    if (!datadir || product_id <= 0 || !customer_addr || !customer_addr[0] ||
        !out)
        return STORE_BUYER_ERR_ARGS;
    memset(out, 0, sizeof(*out));
    if (!store_buyer_network_allows())
        return STORE_BUYER_ERR_MAINNET_REFUSED;

    /* Refuse an unknown product before spending a proof-of-work solve on it. */
    if (!sb_open_db(datadir, &ndb, "store_buyer.order_precheck"))
        return STORE_BUYER_ERR_DB;
    bool known = db_store_product_find_active(&ndb, product_id, &product);
    node_db_close(&ndb);
    if (!known)
        return STORE_BUYER_ERR_UNKNOWN_PRODUCT;

    if (!sb_solve_order_form(datadir, product_id, csrf, sizeof(csrf),
                             pow_ts, sizeof(pow_ts),
                             pow_nonce, sizeof(pow_nonce)))
        LOG_RETURN(STORE_BUYER_ERR_ORDER_CREATE_FAILED, SB_TAG,
                   "order: could not obtain a CSRF token and a solved "
                   "proof-of-work for product %lld", (long long)product_id);

    resp = zcl_malloc(SB_RESP_MAX, "store_buyer_order_resp");
    if (!resp)
        LOG_RETURN(STORE_BUYER_ERR_INTERNAL, SB_TAG,
                   "order: response buffer allocation failed");

    (void)snprintf(body, sizeof(body),
                   "product_id=%lld&customer_addr=%s&csrf_token=%s"
                   "&pow_ts=%s&pow_nonce=%s",
                   (long long)product_id, customer_addr, csrf, pow_ts,
                   pow_nonce);
    n = store_handle_request("POST", "/store/orders",
                             (const uint8_t *)body, strlen(body),
                             resp, SB_RESP_MAX, datadir);
    if (n == 0)
        goto done;
    resp[(n < SB_RESP_MAX) ? n : (SB_RESP_MAX - 1)] = '\0';
    if (!strstr((const char *)resp, "HTTP/1.1 200 OK") ||
        !sb_scrape_order_id((const char *)resp, &order_id)) {
        LOG_WARN(SB_TAG, "order: store refused product %lld (response head: "
                 "%.40s)", (long long)product_id, (const char *)resp);
        goto done;
    }

    if (!sb_open_db(datadir, &ndb, "store_buyer.order_record")) {
        st = STORE_BUYER_ERR_DB;
        goto done;
    }
    if (!db_store_order_find_view(&ndb, order_id, &order_view)) {
        node_db_close(&ndb);
        LOG_WARN(SB_TAG, "order: store answered with order %lld but no such "
                 "order row exists", (long long)order_id);
        goto done;
    }

    /* Idempotent by merchant order id: if this order already has a buyer
     * row, return that one rather than minting a second obligation. */
    if (!db_store_purchase_find_by_order(&ndb, order_id, &purchase))
        memset(&purchase, 0, sizeof(purchase));

    purchase.order_id = order_id;
    purchase.product_id = product_id;
    (void)snprintf(purchase.product_name, sizeof(purchase.product_name), "%s",
                   product.name);
    (void)snprintf(purchase.token_id, sizeof(purchase.token_id), "%s",
                   product.token_id);
    (void)snprintf(purchase.payment_addr, sizeof(purchase.payment_addr), "%s",
                   order_view.payment_addr);
    (void)snprintf(purchase.customer_addr, sizeof(purchase.customer_addr),
                   "%s", customer_addr);
    (void)snprintf(purchase.memo, sizeof(purchase.memo), "ZCL23ORDER:%lld",
                   (long long)order_id);
    purchase.amount_zatoshi = order_view.amount_zatoshi;
    purchase.has_content_hash = product.has_content;
    if (product.has_content)
        memcpy(purchase.content_hash, product.content_hash,
               sizeof(purchase.content_hash));
    if (output_path && output_path[0])
        (void)snprintf(purchase.output_path, sizeof(purchase.output_path),
                       "%s", output_path);
    if (purchase.id == 0)
        purchase.stage = STORE_PURCHASE_CREATED;
    purchase.last_error[0] = '\0';

    bool saved = db_store_purchase_save(&ndb, &purchase);
    node_db_close(&ndb);
    if (!saved) {
        st = STORE_BUYER_ERR_DB;
        LOG_WARN(SB_TAG, "order: created merchant order %lld but could not "
                 "record the buyer purchase row", (long long)order_id);
        goto done;
    }

    out->purchase_id = purchase.id;
    out->order_id = order_id;
    (void)snprintf(out->payment_addr, sizeof(out->payment_addr), "%s",
                   purchase.payment_addr);
    (void)snprintf(out->memo, sizeof(out->memo), "%s", purchase.memo);
    out->amount_zatoshi = purchase.amount_zatoshi;
    st = STORE_BUYER_OK;
done:
    free(resp);
    return st;
}

/* ── pay ────────────────────────────────────────────────────────────── */

enum store_buyer_status store_buyer_prepare_payment(
    const char *datadir, int64_t purchase_id, const char *from_addr,
    struct store_buyer_payment *out)
{
    struct node_db ndb;
    struct db_store_purchase purchase;
    enum store_buyer_status st;

    if (!datadir || !from_addr || !from_addr[0] || !out)
        return STORE_BUYER_ERR_ARGS;
    memset(out, 0, sizeof(*out));
    if (!store_buyer_network_allows())
        return STORE_BUYER_ERR_MAINNET_REFUSED;

    if (!sb_open_db(datadir, &ndb, "store_buyer.prepare_payment"))
        return STORE_BUYER_ERR_DB;
    st = sb_load_purchase(&ndb, purchase_id, &purchase);
    node_db_close(&ndb);
    if (st != STORE_BUYER_OK)
        return st;

    if (purchase.stage == STORE_PURCHASE_PAYING ||
        purchase.stage == STORE_PURCHASE_PAID ||
        purchase.stage == STORE_PURCHASE_DELIVERED)
        return STORE_BUYER_ERR_ALREADY_PAID;

    /* No proving backend ⇒ no shielded output can be built at all. Refuse
     * here, loudly and by name, rather than letting the send fail hundreds
     * of lines later inside coin selection — and never silently no-op. */
    if (!zclassic_sapling_prover_is_ready()) {
        LOG_WARN(SB_TAG, "pay: refusing purchase %lld — no Sapling proving "
                 "backend (backend=%s status=%s); build with ZCL_WITH_RUST=1 "
                 "and install the Sapling parameters",
                 (long long)purchase_id, zclassic_sapling_prover_backend(),
                 zclassic_sapling_prover_status());
        return STORE_BUYER_ERR_PROVER_UNAVAILABLE;
    }

    /* The node's own spend guard. Never weakened here: if it refuses, the
     * purchase stays unpaid and says so. */
    {
        char reason[96] = {0};
        if (!sovereignty_guard_allow("wallet_spend", reason, sizeof(reason))) {
            LOG_WARN(SB_TAG, "pay: refusing purchase %lld — spend guard: %s",
                     (long long)purchase_id, reason);
            return STORE_BUYER_ERR_SPEND_REFUSED;
        }
    }

    /* Whole-wallet floor. Coin selection is z_sendmany's job; this only
     * refuses the case where no selection could possibly succeed. */
    {
        const struct wallet *w = app_runtime_wallet();
        if (w) {
            int64_t pool = wallet_addr_is_sapling(from_addr)
                               ? wallet_get_sapling_balance(w)
                               : wallet_get_balance(w);
            if (pool < purchase.amount_zatoshi) {
                LOG_WARN(SB_TAG, "pay: refusing purchase %lld — wallet holds "
                         "%lld zatoshi, order needs %lld",
                         (long long)purchase_id, (long long)pool,
                         (long long)purchase.amount_zatoshi);
                return STORE_BUYER_ERR_INSUFFICIENT_FUNDS;
            }
        }
    }

    (void)snprintf(out->from_addr, sizeof(out->from_addr), "%s", from_addr);
    (void)snprintf(out->to_addr, sizeof(out->to_addr), "%s",
                   purchase.payment_addr);
    out->amount_zatoshi = purchase.amount_zatoshi;
    out->amount_zcl = (double)purchase.amount_zatoshi / 100000000.0;

    /* Hex-encode the memo and append an explicit 00 terminator. The
     * merchant credits an order only when the byte after
     * "ZCL23ORDER:<id>" is NUL or ';' — a memo that merely starts with the
     * token and runs into padding is not credited. Emitting the terminator
     * here means the buyer never depends on which padding byte the sender
     * happens to use. */
    {
        size_t memo_len = strlen(purchase.memo);
        size_t need = memo_len * 2 + 2 + 1;
        if (need > sizeof(out->memo_hex))
            LOG_RETURN(STORE_BUYER_ERR_INTERNAL, SB_TAG,
                       "pay: memo for purchase %lld does not fit the hex "
                       "buffer (%zu bytes needed)",
                       (long long)purchase_id, need);
        for (size_t i = 0; i < memo_len; i++)
            (void)snprintf(out->memo_hex + i * 2, 3, "%02x",
                           (unsigned char)purchase.memo[i]);
        (void)snprintf(out->memo_hex + memo_len * 2, 3, "00");
    }
    return STORE_BUYER_OK;
}

enum store_buyer_status store_buyer_record_payment(const char *datadir,
                                                   int64_t purchase_id,
                                                   const char *operation_id)
{
    struct node_db ndb;
    struct db_store_purchase purchase;
    enum store_buyer_status st;

    if (!datadir)
        return STORE_BUYER_ERR_ARGS;
    if (!sb_open_db(datadir, &ndb, "store_buyer.record_payment"))
        return STORE_BUYER_ERR_DB;
    st = sb_load_purchase(&ndb, purchase_id, &purchase);
    if (st != STORE_BUYER_OK) {
        node_db_close(&ndb);
        return st;
    }
    purchase.stage = STORE_PURCHASE_PAYING;
    purchase.last_error[0] = '\0';
    (void)snprintf(purchase.operation_id, sizeof(purchase.operation_id), "%s",
                   operation_id ? operation_id : "");
    bool ok = db_store_purchase_save(&ndb, &purchase);
    node_db_close(&ndb);
    if (!ok) {
        LOG_WARN(SB_TAG, "pay: payment submitted for purchase %lld but the "
                 "row would not persist — operation id %s",
                 (long long)purchase_id,
                 operation_id && operation_id[0] ? operation_id : "(none)");
        return STORE_BUYER_ERR_DB;
    }
    return STORE_BUYER_OK;
}

enum store_buyer_status store_buyer_fail(const char *datadir,
                                         int64_t purchase_id,
                                         enum store_buyer_status why,
                                         const char *detail)
{
    struct node_db ndb;
    struct db_store_purchase purchase;
    enum store_buyer_status st;

    if (!datadir)
        return STORE_BUYER_ERR_ARGS;
    if (!sb_open_db(datadir, &ndb, "store_buyer.fail"))
        return STORE_BUYER_ERR_DB;
    st = sb_load_purchase(&ndb, purchase_id, &purchase);
    if (st != STORE_BUYER_OK) {
        node_db_close(&ndb);
        return st;
    }
    (void)snprintf(purchase.last_error, sizeof(purchase.last_error), "%s: %s",
                   store_buyer_status_code(why),
                   detail && detail[0] ? detail
                                       : store_buyer_status_message(why));
    if (store_buyer_status_is_terminal(why))
        purchase.stage = STORE_PURCHASE_FAILED;
    bool ok = db_store_purchase_save(&ndb, &purchase);
    node_db_close(&ndb);
    return ok ? STORE_BUYER_OK : STORE_BUYER_ERR_DB;
}

/* ── poll ───────────────────────────────────────────────────────────── */

enum store_buyer_status store_buyer_refresh(const char *datadir,
                                            int64_t purchase_id,
                                            struct store_buyer_state *out)
{
    struct node_db ndb;
    struct db_store_purchase purchase;
    struct db_store_order_view order_view;
    enum store_buyer_status st;

    if (!datadir || !out)
        return STORE_BUYER_ERR_ARGS;
    memset(out, 0, sizeof(*out));
    if (!sb_open_db(datadir, &ndb, "store_buyer.refresh"))
        return STORE_BUYER_ERR_DB;
    st = sb_load_purchase(&ndb, purchase_id, &purchase);
    if (st != STORE_BUYER_OK) {
        node_db_close(&ndb);
        return st;
    }

    out->tip_height = db_store_chain_tip_height(&ndb);
    out->merchant_order_found =
        db_store_order_find_view(&ndb, purchase.order_id, &order_view);
    if (out->merchant_order_found)
        out->merchant_order_status = order_view.status;

    /* The SAME memo-bound matcher the merchant's reconcile uses. There is
     * deliberately no second payment finder here: the address-and-amount
     * finder credits a payment that names a different order, and the whole
     * point of the memo bind is that it does not. Confirmation depth
     * mirrors the merchant's (tip - 3). */
    out->confirmed_zatoshi = db_store_received_payment_for_memo(
        &ndb, purchase.payment_addr, purchase.order_id, out->tip_height - 3);

    /* Advance the stage on the merchant's verdict, never on our own opinion
     * of the payment: STORE_ORDER_SENT means the merchant credited the order
     * AND minted the access tokens, which is exactly the precondition the
     * gated download checks. */
    if (purchase.stage != STORE_PURCHASE_DELIVERED &&
        out->merchant_order_found &&
        order_view.status == STORE_ORDER_SENT) {
        if (purchase.stage != STORE_PURCHASE_PAID) {
            purchase.stage = STORE_PURCHASE_PAID;
            purchase.last_error[0] = '\0';
            if (order_view.payment_txid[0])
                (void)snprintf(purchase.operation_id,
                               sizeof(purchase.operation_id), "%s",
                               order_view.payment_txid);
            (void)db_store_purchase_save(&ndb, &purchase);
        }
    }

    out->purchase = purchase;
    out->ready_to_collect = (purchase.stage == STORE_PURCHASE_PAID);
    node_db_close(&ndb);
    return STORE_BUYER_OK;
}

enum store_buyer_status store_buyer_list(const char *datadir,
                                         struct db_store_purchase *out,
                                         size_t max, size_t *n_out)
{
    struct node_db ndb;
    int count;

    if (n_out)
        *n_out = 0;
    if (!datadir || !out || max == 0 || !n_out)
        return STORE_BUYER_ERR_ARGS;
    if (!sb_open_db(datadir, &ndb, "store_buyer.list"))
        return STORE_BUYER_ERR_DB;
    count = db_store_purchase_list(&ndb, out, max);
    node_db_close(&ndb);
    *n_out = (size_t)(count > 0 ? count : 0);
    return STORE_BUYER_OK;
}

/* ── collect ────────────────────────────────────────────────────────── */

/* Locate the body of an HTTP response already in memory. Returns NULL when
 * the response carries no header terminator (which is itself a refusal —
 * we never guess where a body starts). */
static const uint8_t *sb_http_body(const uint8_t *resp, size_t n,
                                   size_t *body_len)
{
    for (size_t i = 0; i + 4 <= n; i++) {
        if (memcmp(resp + i, "\r\n\r\n", 4) == 0) {
            *body_len = n - (i + 4);
            return resp + i + 4;
        }
    }
    return NULL;
}

/* Write `len` bytes to `path` atomically: a sibling temporary in the same
 * directory, fsync, rename. A caller that dies mid-write leaves the target
 * either absent or complete — never a truncated file that hashes to nothing
 * and looks like a delivered purchase. On any failure the temporary is
 * removed, so a failed collect leaves no debris either. */
static bool sb_write_atomic(const char *path, const uint8_t *data, size_t len)
{
    char tmp[STORE_PURCHASE_PATH_MAX + 16];
    int n = snprintf(tmp, sizeof(tmp), "%s.part", path);
    if (n <= 0 || (size_t)n >= sizeof(tmp))
        return false;

    FILE *f = fopen(tmp, "wb");
    if (!f)
        return false;
    bool ok = (len == 0) || (fwrite(data, 1, len, f) == len);
    if (ok)
        ok = (fflush(f) == 0);
    if (ok)
        ok = (fsync(fileno(f)) == 0);
    if (fclose(f) != 0)
        ok = false;
    if (ok && rename(tmp, path) != 0)
        ok = false;
    if (!ok)
        (void)unlink(tmp);
    return ok;
}

enum store_buyer_status store_buyer_collect(const char *datadir,
                                            int64_t purchase_id,
                                            const char *output_path,
                                            struct store_buyer_delivery *out)
{
    struct node_db ndb;
    struct db_store_purchase purchase;
    enum store_buyer_status st;
    uint8_t *resp = NULL;
    char path[512];
    char target[STORE_PURCHASE_PATH_MAX + 1];

    if (!datadir || !out)
        return STORE_BUYER_ERR_ARGS;
    memset(out, 0, sizeof(*out));

    /* Re-poll the merchant first. This is what makes a collect a RESUME: a
     * node that paid, then restarted, comes back with the row still at
     * "paying" — the credit happened while it was gone. Asking the merchant
     * before refusing means the operator does not have to know to run a
     * status call first. */
    {
        struct store_buyer_state state;
        st = store_buyer_refresh(datadir, purchase_id, &state);
        if (st != STORE_BUYER_OK)
            return st;
    }

    if (!sb_open_db(datadir, &ndb, "store_buyer.collect"))
        return STORE_BUYER_ERR_DB;
    st = sb_load_purchase(&ndb, purchase_id, &purchase);
    node_db_close(&ndb);
    if (st != STORE_BUYER_OK)
        return st;

    if (purchase.stage != STORE_PURCHASE_PAID &&
        purchase.stage != STORE_PURCHASE_DELIVERED)
        return STORE_BUYER_ERR_PAYMENT_NOT_CONFIRMED;

    const char *want_path = (output_path && output_path[0]) ? output_path
                                                            : purchase.output_path;
    if (!want_path || !want_path[0])
        return STORE_BUYER_ERR_ARGS;
    if (strlen(want_path) > STORE_PURCHASE_PATH_MAX)
        return STORE_BUYER_ERR_ARGS;
    (void)snprintf(target, sizeof(target), "%s", want_path);

    /* A product with no attached payload has nothing to verify and nothing
     * to write; the store answers with an HTML access page. Refusing here
     * is the honest answer — there is no file to deliver. */
    if (!purchase.has_content_hash) {
        LOG_WARN(SB_TAG, "collect: purchase %lld is for a product with no "
                 "file payload; there is nothing to download",
                 (long long)purchase_id);
        return STORE_BUYER_ERR_DELIVERY_FAILED;
    }

    resp = zcl_malloc(SB_RESP_MAX, "store_buyer_collect_resp");
    if (!resp)
        LOG_RETURN(STORE_BUYER_ERR_INTERNAL, SB_TAG,
                   "collect: response buffer allocation failed");

    (void)snprintf(path, sizeof(path), "/store/access?addr=%s&token=%s",
                   purchase.customer_addr, purchase.token_id);
    size_t n = store_handle_request("GET", path, NULL, 0, resp, SB_RESP_MAX,
                                    datadir);
    if (n == 0 ||
        !strstr((const char *)resp, "HTTP/1.1 200 OK")) {
        free(resp);
        LOG_WARN(SB_TAG, "collect: token gate did not serve purchase %lld "
                 "(token=%s addr=%s)", (long long)purchase_id,
                 purchase.token_id, purchase.customer_addr);
        return STORE_BUYER_ERR_DELIVERY_FAILED;
    }

    size_t body_len = 0;
    const uint8_t *body = sb_http_body(resp, n, &body_len);
    if (!body || body_len == 0) {
        free(resp);
        LOG_WARN(SB_TAG, "collect: purchase %lld got a response with no body",
                 (long long)purchase_id);
        return STORE_BUYER_ERR_DELIVERY_FAILED;
    }

    /* Verify BEFORE writing. The hash is the product's content hash recorded
     * when the order was placed, so this also catches a merchant that swapped
     * the payload after the sale — not just a corrupted transfer. */
    uint8_t got[32];
    zcl_sha3_256(body, body_len, got);
    if (memcmp(got, purchase.content_hash, sizeof(got)) != 0) {
        free(resp);
        LOG_WARN(SB_TAG, "collect: purchase %lld delivered %zu bytes whose "
                 "SHA3-256 does not match the product content hash — "
                 "nothing written", (long long)purchase_id, body_len);
        (void)store_buyer_fail(datadir, purchase_id,
                               STORE_BUYER_ERR_HASH_MISMATCH, NULL);
        return STORE_BUYER_ERR_HASH_MISMATCH;
    }

    if (!sb_write_atomic(target, body, body_len)) {
        free(resp);
        LOG_WARN(SB_TAG, "collect: purchase %lld verified %zu bytes but they "
                 "could not be written to %s (%s)", (long long)purchase_id,
                 body_len, target, strerror(errno));
        (void)store_buyer_fail(datadir, purchase_id,
                               STORE_BUYER_ERR_WRITE_FAILED, target);
        return STORE_BUYER_ERR_WRITE_FAILED;
    }
    free(resp);

    if (!sb_open_db(datadir, &ndb, "store_buyer.collect_record"))
        return STORE_BUYER_ERR_DB;
    if (sb_load_purchase(&ndb, purchase_id, &purchase) == STORE_BUYER_OK) {
        purchase.stage = STORE_PURCHASE_DELIVERED;
        purchase.last_error[0] = '\0';
        (void)snprintf(purchase.output_path, sizeof(purchase.output_path),
                       "%s", target);
        (void)db_store_purchase_save(&ndb, &purchase);
    }
    node_db_close(&ndb);

    (void)snprintf(out->output_path, sizeof(out->output_path), "%s", target);
    out->bytes = (int64_t)body_len;
    memcpy(out->content_hash, got, sizeof(out->content_hash));
    out->hash_verified = true;
    return STORE_BUYER_OK;
}
