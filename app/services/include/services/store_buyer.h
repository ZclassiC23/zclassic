/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Store BUYER service — the buying half of the store, with no browser.
 *
 * The selling half already worked end to end: a merchant lists a product,
 * the store mints a one-time Sapling payment address per order, a background
 * worker credits the order from the note whose memo names it, and the file
 * is served token-gated. What did not exist was a buyer that a program could
 * be: the order form is HTML with a CSRF token and a proof-of-work puzzle
 * solved by embedded JavaScript, and the download is an HTTP GET. A human
 * with a Tor Browser was the only client.
 *
 * This service is that client, in-process. It drives the SAME surfaces:
 *   store_handle_request()                — the real order-create route, so
 *                                           CSRF, the PoW gate, and the
 *                                           pending-pool caps all apply
 *   db_store_received_payment_for_memo()  — the existing memo-bound matcher;
 *                                           there is no second payment finder
 *   /store/access                         — the real token gate
 * and records what it is owed in `store_purchases` (models/store_purchase.h)
 * so a purchase paid for but not yet collected survives a restart.
 *
 * Every entry point returns one typed status. The caller renders it; nothing
 * here prints, and nothing here half-writes: the delivery step verifies the
 * SHA3-256 of the received bytes against the product's content hash BEFORE
 * any byte reaches the output path, and on a mismatch it writes nothing at
 * all rather than leaving a partial or wrong file behind.
 *
 * MAINNET IS REFUSED. Paying a merchant with real value from a scripted
 * buyer is not a thing this ships enabled; the guard is checked first, before
 * an order is created or a payment prepared. */

#ifndef ZCL_SERVICES_STORE_BUYER_H
#define ZCL_SERVICES_STORE_BUYER_H

#include "models/store_purchase.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Distinguishable outcomes. Each maps to one stable UPPER_SNAKE code
 * (store_buyer_status_code) so a caller can branch on the reason rather than
 * on prose. Ordered roughly by where in the purchase they can occur. */
enum store_buyer_status {
    STORE_BUYER_OK = 0,
    STORE_BUYER_ERR_ARGS,                 /* caller passed nonsense */
    STORE_BUYER_ERR_MAINNET_REFUSED,      /* not on the real chain */
    STORE_BUYER_ERR_DB,                   /* node.db unavailable */
    STORE_BUYER_ERR_UNKNOWN_PRODUCT,      /* no such active product */
    STORE_BUYER_ERR_ORDER_CREATE_FAILED,  /* the store refused the order */
    STORE_BUYER_ERR_UNKNOWN_PURCHASE,     /* no such buyer purchase row */
    STORE_BUYER_ERR_ALREADY_PAID,         /* payment already submitted */
    STORE_BUYER_ERR_PROVER_UNAVAILABLE,   /* no Sapling proving backend */
    STORE_BUYER_ERR_SPEND_REFUSED,        /* sovereignty guard said no */
    STORE_BUYER_ERR_INSUFFICIENT_FUNDS,   /* wallet cannot cover the order */
    STORE_BUYER_ERR_PAYMENT_NOT_CONFIRMED,/* merchant has not credited it */
    STORE_BUYER_ERR_DELIVERY_FAILED,      /* paid, but no bytes came back */
    STORE_BUYER_ERR_HASH_MISMATCH,        /* bytes are not the product */
    STORE_BUYER_ERR_WRITE_FAILED,         /* verified bytes would not land */
    STORE_BUYER_ERR_INTERNAL              /* out of memory / impossible state */
};

/* Stable machine token, e.g. "HASH_MISMATCH". Never NULL. */
const char *store_buyer_status_code(enum store_buyer_status st);

/* One-line human explanation for the same status. Never NULL. */
const char *store_buyer_status_message(enum store_buyer_status st);

/* True on any status that means "this purchase is stuck, not merely
 * unfinished" — i.e. the row should be stamped FAILED with the reason. */
bool store_buyer_status_is_terminal(enum store_buyer_status st);

/* ── discover ───────────────────────────────────────────────────────── */

struct store_buyer_offer {
    int64_t product_id;
    char name[STORE_PURCHASE_NAME_MAX + 1];
    char token_id[STORE_PURCHASE_TOKEN_MAX + 1];
    int64_t price_zatoshi;
    int tokens_per_purchase;
    bool has_content;              /* a file payload is attached */
};

/* Active products offered by the store in `datadir`, newest id last.
 * `*n_out` receives the row count written (0 is a valid answer: a store with
 * nothing for sale is not an error). */
enum store_buyer_status store_buyer_catalog(const char *datadir,
                                            struct store_buyer_offer *out,
                                            size_t max, size_t *n_out);

/* ── place an order ─────────────────────────────────────────────────── */

struct store_buyer_order {
    int64_t purchase_id;
    int64_t order_id;
    char payment_addr[STORE_PURCHASE_ADDR_MAX + 1];
    char memo[STORE_PURCHASE_MEMO_MAX + 1];   /* "ZCL23ORDER:<order_id>" */
    int64_t amount_zatoshi;
};

/* Place an order for `product_id` through the store's real order-create
 * route — CSRF token, proof-of-work puzzle and pending-pool caps included —
 * and record the buyer's side of it. `customer_addr` is the transparent
 * address the merchant mints access tokens to; `output_path` is where the
 * purchased bytes will be written when they are collected (may be NULL now
 * and supplied at collect time). */
enum store_buyer_status store_buyer_order(const char *datadir,
                                          int64_t product_id,
                                          const char *customer_addr,
                                          const char *output_path,
                                          struct store_buyer_order *out);

/* ── pay ────────────────────────────────────────────────────────────── */

struct store_buyer_payment {
    char from_addr[STORE_PURCHASE_ADDR_MAX + 1];
    char to_addr[STORE_PURCHASE_ADDR_MAX + 1];
    int64_t amount_zatoshi;
    double amount_zcl;
    /* The order memo as hex WITH an explicit trailing 00. The merchant's
     * matcher requires the byte after "ZCL23ORDER:<id>" to be NUL or ';',
     * so the terminator is part of the instruction, not an afterthought. */
    char memo_hex[2 * (STORE_PURCHASE_MEMO_MAX + 1) + 1];
};

/* Everything needed to pay a purchase, after every refusal that can be
 * decided without moving value: mainnet, unknown purchase, already paid,
 * missing Sapling proving backend, and the sovereignty spend guard. Writes
 * nothing. The caller performs the shielded send and then calls
 * store_buyer_record_payment (or store_buyer_fail if the send did not
 * happen).
 *
 * The funds check here is a WHOLE-WALLET floor, not coin selection: it
 * refuses when the wallet's total balance in the right pool cannot possibly
 * cover the order. Per-address selection stays where it belongs, in
 * z_sendmany, and its refusal is mapped back onto this same status. */
enum store_buyer_status store_buyer_prepare_payment(
    const char *datadir, int64_t purchase_id, const char *from_addr,
    struct store_buyer_payment *out);

/* Stamp a submitted payment onto the purchase: stage PAYING, operation id
 * recorded, last_error cleared. */
enum store_buyer_status store_buyer_record_payment(const char *datadir,
                                                   int64_t purchase_id,
                                                   const char *operation_id);

/* Stamp a refusal onto the purchase so the reason survives the process.
 * Only moves the row to FAILED for a terminal status
 * (store_buyer_status_is_terminal); otherwise the stage is left alone and
 * just the reason is recorded, because "not confirmed yet" is not a failure
 * and must stay resumable. */
enum store_buyer_status store_buyer_fail(const char *datadir,
                                         int64_t purchase_id,
                                         enum store_buyer_status why,
                                         const char *detail);

/* ── poll ───────────────────────────────────────────────────────────── */

struct store_buyer_state {
    struct db_store_purchase purchase;
    bool merchant_order_found;
    int merchant_order_status;      /* enum store_order_status */
    int64_t confirmed_zatoshi;      /* memo-bound, confirmation-depth bound */
    int64_t tip_height;
    bool ready_to_collect;
};

/* Re-read a purchase against the merchant's current view and advance the
 * stage when the merchant has credited the order. Idempotent; safe to call
 * on any stage. */
enum store_buyer_status store_buyer_refresh(const char *datadir,
                                            int64_t purchase_id,
                                            struct store_buyer_state *out);

/* Newest-first list of this node's purchases. `*n_out` gets the count. */
enum store_buyer_status store_buyer_list(const char *datadir,
                                         struct db_store_purchase *out,
                                         size_t max, size_t *n_out);

/* ── collect ────────────────────────────────────────────────────────── */

struct store_buyer_delivery {
    char output_path[STORE_PURCHASE_PATH_MAX + 1];
    int64_t bytes;
    uint8_t content_hash[32];
    bool hash_verified;
};

/* Fetch the purchased bytes through the real token gate, verify their
 * SHA3-256 against the product's content hash, and only then write them to
 * `output_path` (or the path recorded at order time when NULL).
 *
 * The write is atomic-by-rename via a sibling temporary file, and a hash
 * mismatch leaves NOTHING behind: no temporary, no partial, no stale
 * overwrite of an existing file. */
enum store_buyer_status store_buyer_collect(const char *datadir,
                                            int64_t purchase_id,
                                            const char *output_path,
                                            struct store_buyer_delivery *out);

#endif /* ZCL_SERVICES_STORE_BUYER_H */
