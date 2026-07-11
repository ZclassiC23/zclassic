/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Store controller — ZSLP token commerce. */


#include "controllers/store_controller_internal.h"
#include "controllers/zslp_controller.h"

/* Forward declarations (helpers that stay static to this file) */
static bool store_csrf_verify(const char *context, const char *provided);
static bool store_parse_access_query(const char *path,
                                     char *addr, size_t addr_max,
                                     char *token, size_t token_max);
static bool store_mark_order_paid(const char *datadir,
                                  int64_t order_id,
                                  int status);


/* POST /store/buy/:id — create order. This is a request action (it mints a
 * payment z-address and writes the order row), so it lives in the controller;
 * it calls the store view's render helpers to build the response page. */
static size_t serve_create_order(sqlite3 *db, int64_t product_id,
                                  const char *customer_addr,
                                  const char *datadir,
                                  uint8_t *resp, size_t max)
{
    struct node_db ndb = { .db = db, .open = true };
    struct db_store_product product;
    memset(&product, 0, sizeof(product));

    if (!db_store_product_find_active(&ndb, product_id, &product)) {
        return (size_t)snprintf((char *)resp, max,
            "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n"
            "Connection: close\r\n\r\n<h1>Product not found</h1>");
    }

    /* Generate a unique Sapling z-address for this payment.
     * NEVER fall back to a fake address — that loses user funds. */
    char payment_addr[128];
    if (!zslp_generate_payment_address(datadir, payment_addr,
                                        sizeof(payment_addr))) {
        printf("store: CRITICAL — z-address generation failed for product %lld\n",
               (long long)product_id);
        fflush(stdout);
        return (size_t)snprintf((char *)resp, max,
            "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/html\r\n"
            "Connection: close\r\n\r\n"
            "<h1>Payment Temporarily Unavailable</h1>"
            "<p>The node is still loading cryptographic keys. "
            "Please try again in a few minutes.</p>"
            "<p><a href='/store/products'>Back to Store</a></p>");
    }

    struct db_store_order order;
    memset(&order, 0, sizeof(order));
    order.product_id = product_id;
    snprintf(order.customer_addr, sizeof(order.customer_addr), "%s",
             customer_addr ? customer_addr : "");
    snprintf(order.payment_addr, sizeof(order.payment_addr), "%s", payment_addr);
    order.amount_zatoshi = product.price_zatoshi;
    order.status = STORE_ORDER_PENDING;
    if (!db_store_order_save(&ndb, &order)) {
        printf("store: order INSERT failed: %s\n", sqlite3_errmsg(db));
        return (size_t)snprintf((char *)resp, max,
            "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/html\r\n"
            "Connection: close\r\n\r\n<h1>Order creation failed</h1>");
    }
    int64_t order_id = order.id;

    /* Show payment page */
    char body[8192];
    size_t off = 0;
    int n = html_body_start(body, sizeof(body), "Payment");
    if (n > 0) off = (size_t)n;

    char safe_pay[256], safe_cust[256];
    html_escape(safe_pay, sizeof(safe_pay), payment_addr);
    html_escape(safe_cust, sizeof(safe_cust),
                customer_addr ? customer_addr : "(not provided)");

    char order_price[32];
    format_zcl_price(order_price, sizeof(order_price), product.price_zatoshi);

    n = snprintf(body + off, sizeof(body) - off,
        "<h2>Order #%lld</h2>"
        "<div class='product'>"
        "<p>Send exactly <span class='price'>%s ZCL</span> to:</p>"
        "<div class='addr'>%s</div>"
        "<button class='btn' style='font-size:12px;padding:6px 12px;cursor:pointer;border:none' "
        "onclick=\"navigator.clipboard?navigator.clipboard.writeText('%s'):void(0);"
        "this.textContent='Copied!'\">Copy Address</button>"
        "<p><strong>You must include this memo</strong> so your payment is "
        "matched to this order:</p>"
        "<div class='addr'>ZCL23ORDER:%lld</div>"
        "<button class='btn' style='font-size:12px;padding:6px 12px;cursor:pointer;border:none' "
        "onclick=\"navigator.clipboard?navigator.clipboard.writeText('ZCL23ORDER:%lld'):void(0);"
        "this.textContent='Copied!'\">Copy Memo</button>"
        "<p>After payment confirms, tokens will be sent to:</p>"
        "<div class='addr'>%s</div>"
        "<p><a href='/store/orders/%lld'>Check payment status</a></p>"
        "</div>"
        "<p><a href='/store/products'>&larr; Back to store</a></p>"
        "</body></html>",
        (long long)order_id,
        order_price,
        safe_pay,
        safe_pay,
        (long long)order_id,
        (long long)order_id,
        safe_cust,
        (long long)order_id);
    if (n > 0) off += (size_t)n;

    return store_html_response(body, off, resp, max);
}


/* Parse resource id from the last path segment and reject malformed ids. */
static bool parse_positive_path_id(const char *path, int64_t *id_out)
{
    const char *last = strrchr(path, '/');
    char *end = NULL;
    long long value;

    if (!id_out)
        return false;
    *id_out = -1;
    if (!last || !last[1])
        return false;
    value = strtoll(last + 1, &end, 10);
    if (!end || *end != '\0' || value <= 0)
        return false;
    *id_out = (int64_t)value;
    return true;
}

static bool path_eq(const char *path, const char *expected)
{
    return path && expected && strcmp(path, expected) == 0;
}

static bool path_has_prefix(const char *path, const char *prefix)
{
    return path && prefix && strncmp(path, prefix, strlen(prefix)) == 0;
}

static bool route_is_product_index(const char *path)
{
    return path_eq(path, "/store") || path_eq(path, "/store/") ||
           path_eq(path, "/store/products") || path_eq(path, "/store/products/");
}

static bool route_is_product_show(const char *path)
{
    return path_has_prefix(path, "/store/product/") ||
           path_has_prefix(path, "/store/products/");
}

static bool route_is_order_show(const char *path)
{
    return path_has_prefix(path, "/store/order/") ||
           path_has_prefix(path, "/store/orders/");
}

static bool route_is_order_index(const char *path)
{
    return path_eq(path, "/store/orders") || path_eq(path, "/store/orders/");
}

static bool route_is_order_create(const char *method, const char *path)
{
    return method && strcmp(method, "POST") == 0 &&
           (path_eq(path, "/store/orders") ||
            path_eq(path, "/store/orders/") ||
            path_has_prefix(path, "/store/buy/"));
}

/* Validate address: must be a valid ZClassic t-address or z-address,
 * with the Base58Check / Bech32 *checksum* verified — not just a
 * syntactically-plausible prefix.  A one-character typo in a t-addr
 * passes a syntactic prefix check but decodes to a random 20-byte hash
 * whose payments are unspendable: funds sent to such an order are
 * burned.  Also prevents XSS via customer_addr in HTML output.
 *
 * Implementation: zcl_validate_zcl_address in app/models/src/shared_validators.c. */

static bool store_validate_access_addr(const char *addr)
{
    return addr && addr[0] &&
           zslp_service_validate_recipient_addr(addr, false);
}

static bool store_validate_access_token(const char *token)
{
    return token && token[0] &&
           zslp_service_validate_token_key(token);
}

static bool store_parse_query_field(const char *path, const char *field,
                                    char *out, size_t out_max)
{
    const char *p;
    size_t i = 0;
    char needle[32];

    if (!path || !field || !out || out_max == 0)
        return false;
    out[0] = '\0';
    snprintf(needle, sizeof(needle), "%s=", field);
    p = strstr(path, needle);
    if (!p)
        return false;
    p += strlen(needle);
    while (p[i] && p[i] != '&' && i < out_max - 1) {
        if ((unsigned char)p[i] < 32 || (unsigned char)p[i] > 126)
            return false;
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return i > 0;
}

static bool store_parse_access_query(const char *path,
                                     char *addr, size_t addr_max,
                                     char *token, size_t token_max)
{
    if (!addr || !token || addr_max == 0 || token_max == 0)
        return false;
    addr[0] = '\0';
    token[0] = '\0';

    if (!store_parse_query_field(path, "addr", addr, addr_max))
        return false; // raw-return-ok:malformed-client-query-not-a-server-error
    if (!store_parse_query_field(path, "token", token, token_max))
        snprintf(token, token_max, "%s", "ZCL23ACCESS");

    return store_validate_access_addr(addr) &&
           store_validate_access_token(token);
}

static bool store_mark_order_paid(const char *datadir,
                                  int64_t order_id,
                                  int status)
{
    char db_path[1024];
    struct node_db ndb;
    bool ok;

    if (!datadir || order_id <= 0)
        return false;

    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    memset(&ndb, 0, sizeof(ndb));
    if (!node_db_open_runtime(&ndb, db_path, "store.mark_order_paid"))
        LOG_FAIL("store", "store_mark_order_paid: node_db_open failed path=%s order=%lld",
                 db_path, (long long)order_id);

    ok = db_store_order_mark_paid(&ndb, order_id, status);
    if (!ok) {
        const char *em = sqlite3_errmsg(ndb.db);
        LOG_WARN("controller", "Store: failed to persist status=%d for order #%lld: %s", status, (long long)order_id, em ? em : "unknown");
    }
    node_db_close(&ndb);
    return ok;
}

/* ── CSRF form token ─────────────────────────────────────
 *
 * Without a token, a malicious third-party page can `<form action=
 * 'http://<onion>/store/orders'>` and trick any visiting browser into
 * silently POSTing an unwanted order.  The store has no login/session
 * cookie to bind to, so classical per-session CSRF isn't reachable
 * without plumbing cookies through onion_service.c.  Instead, sign a
 * small context string (order form scope + product-id) with a
 * per-process random HMAC key and embed it as a hidden field.  The
 * browser's same-origin policy prevents JS on a third-party page from
 * reading our GET response body, so it cannot learn the signed token.
 * A server-side attacker with their own curl can, but that's the same
 * capability as direct submission — no amplification from the victim
 * browser. */
static unsigned char s_csrf_key[32];
static bool s_csrf_key_ready = false;

static void store_csrf_init(void)
{
    if (s_csrf_key_ready) return;
    GetRandBytes(s_csrf_key, sizeof(s_csrf_key));
    s_csrf_key_ready = true;
}

/* Write 32-char lowercase-hex token for `context` into out (33 bytes incl NUL). */
void store_csrf_token(const char *context, char out[33])
{
    store_csrf_init();
    struct hmac_sha256_ctx ctx;
    unsigned char mac[HMAC_SHA256_OUTPUT_SIZE];
    hmac_sha256_init(&ctx, s_csrf_key, sizeof(s_csrf_key));
    hmac_sha256_write(&ctx, (const unsigned char *)context, strlen(context));
    hmac_sha256_finalize(&ctx, mac);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 16; i++) {
        out[i * 2]     = hex[(mac[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[mac[i] & 0x0f];
    }
    out[32] = '\0';
}

/* Constant-time check: does `provided` match the token for `context`? */
static bool store_csrf_verify(const char *context, const char *provided)
{
    if (!context || !provided) return false;
    if (strlen(provided) != 32) return false;
    char expected[33];
    store_csrf_token(context, expected);
    unsigned char diff = 0;
    for (size_t i = 0; i < 32; i++)
        diff |= (unsigned char)(expected[i] ^ provided[i]);
    return diff == 0;
}

/* Context string — the token is bound to the specific order form so a
 * leaked token from one product page can't be replayed to another.
 * Format: "store:order:<product_id>".  Writes into a caller buffer. */
void store_csrf_context(char *out, size_t outmax, int64_t product_id)
{
    snprintf(out, outmax, "store:order:%lld", (long long)product_id);
}

/* Decode `%XX` and `+` escapes in an x-www-form-urlencoded value.
 * Ported from app/controllers/src/wallet_view_helpers.c. Without
 * this, "a%20b" in a form field is stored literally as the four bytes
 * '%','2','0','b' in the DB and rendered back to the user unchanged —
 * breaking display, search, and anything downstream that interprets
 * the stored value. */
static void store_url_decode(char *dst, size_t dstmax, const char *src, size_t srclen)
{
    size_t di = 0;
    if (!dstmax) return;
    for (size_t si = 0; si < srclen && di < dstmax - 1; si++) {
        char c = src[si];
        if (c == '%' && si + 2 < srclen) {
            char h1 = src[si + 1], h2 = src[si + 2];
            int hi = (h1 >= '0' && h1 <= '9') ? h1 - '0' :
                     (h1 >= 'a' && h1 <= 'f') ? h1 - 'a' + 10 :
                     (h1 >= 'A' && h1 <= 'F') ? h1 - 'A' + 10 : -1;
            int lo = (h2 >= '0' && h2 <= '9') ? h2 - '0' :
                     (h2 >= 'a' && h2 <= 'f') ? h2 - 'a' + 10 :
                     (h2 >= 'A' && h2 <= 'F') ? h2 - 'A' + 10 : -1;
            if (hi >= 0 && lo >= 0) {
                dst[di++] = (char)((hi << 4) | lo);
                si += 2;
                continue;
            }
        }
        dst[di++] = (c == '+') ? ' ' : c;
    }
    dst[di] = '\0';
}

/* Parse x-www-form-urlencoded body for `field=value` and URL-decode
 * the value into `out`. */
static const char *parse_form_field(const char *body, size_t len,
                                     const char *field, char *out, size_t outmax)
{
    if (!body || !len || !field || !out || outmax == 0)
        LOG_NULL("store", "parse_form_field: null args body=%p len=%zu field=%s",
                 (void *)body, len, field ? field : "(null)");
    char search[128];
    snprintf(search, sizeof(search), "%s=", field);
    const char *p = strstr(body, search);
    if (!p) return NULL;
    p += strlen(search);
    /* Value ends at &, space, or end of body. */
    size_t remaining = len - (size_t)(p - body);
    size_t vlen = 0;
    while (vlen < remaining && p[vlen] && p[vlen] != '&' && p[vlen] != ' ')
        vlen++;
    store_url_decode(out, outmax, p, vlen);
    return out;
}

static bool parse_positive_form_id(const char *body, size_t body_len,
                                   const char *field, int64_t *id_out)
{
    char raw[32];
    char *end = NULL;
    long long value;

    if (!id_out)
        return false;
    *id_out = -1;
    if (!parse_form_field(body, body_len, field, raw, sizeof(raw)))
        return false; // raw-return-ok:form-field-absent-not-a-server-error
    value = strtoll(raw, &end, 10);
    if (!end || *end != '\0' || value <= 0)
        return false;
    *id_out = (int64_t)value;
    return true;
}

/* Main request handler */
size_t store_handle_request(const char *method, const char *path,
                             const uint8_t *body, size_t body_len,
                             uint8_t *response, size_t response_max,
                             const char *datadir)
{
    if (!path || !response) return 0;

    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    if (!node_db_open_runtime(&ndb, db_path, "store.request")) return 0;
    sqlite3 *db = ndb.db;
    store_ensure_schema(db, datadir);

    size_t result = 0;

    if (route_is_product_index(path)) {
        result = serve_product_list(db, response, response_max);

    } else if (route_is_product_show(path)) {
        int64_t id = -1;
        if (!parse_positive_path_id(path, &id)) {
            const char *err_body = "<h1>Invalid product</h1>"
                "<p>Product id must be a positive integer.</p>"
                "<p><a href='/store/products'>&larr; Back to store</a></p>";
            result = store_error_response("400 Bad Request",
                err_body, strlen(err_body), response, response_max);
        } else {
            result = serve_product_detail(db, id, response, response_max);
        }

    } else if (route_is_order_index(path) &&
               method && strcmp(method, "GET") == 0) {
        result = serve_order_index(db, response, response_max);

    } else if (route_is_order_create(method, path)) {
        int64_t id = -1;
        char addr[128] = "";
        char csrf[64] = "";
        if (body && body_len > 0) {
            parse_form_field((const char *)body, body_len,
                             "customer_addr", addr, sizeof(addr));
            parse_form_field((const char *)body, body_len,
                             "csrf_token", csrf, sizeof(csrf));
        }
        if (path_has_prefix(path, "/store/buy/")) {
            if (!parse_positive_path_id(path, &id))
                id = -1;
        } else if (!parse_positive_form_id((const char *)body, body_len,
                                           "product_id", &id)) {
            const char *err_body = "<h1>Invalid product</h1>"
                "<p>product_id must be a positive integer.</p>"
                "<p><a href='/store/products'>&larr; Back to store</a></p>";
            result = store_error_response("400 Bad Request",
                err_body, strlen(err_body), response, response_max);
            node_db_close(&ndb);
            return result;
        }
        char csrf_ctx[64];
        store_csrf_context(csrf_ctx, sizeof(csrf_ctx), id);
        if (!store_csrf_verify(csrf_ctx, csrf)) {
            const char *err_body = "<h1>Invalid CSRF token</h1>"
                "<p>Form token missing or did not verify. "
                "Please reload the product page and resubmit.</p>"
                "<p><a href='/store/products'>&larr; Back to store</a></p>";
            result = store_error_response("400 Bad Request",
                err_body, strlen(err_body), response, response_max);
            node_db_close(&ndb);
            return result;
        }
        if (!zcl_validate_zcl_address(addr)) {
            const char *err_body = "<h1>Invalid address</h1>"
                "<p>Must be a ZClassic t-address (t1.../t3...) or "
                "z-address (zs1...).</p>"
                "<p><a href='/store/products'>&larr; Back to store</a></p>";
            result = store_error_response("400 Bad Request",
                err_body, strlen(err_body), response, response_max);
        } else {
            result = serve_create_order(db, id, addr, datadir,
                                          response, response_max);
        }

    } else if (route_is_order_show(path)) {
        int64_t id = -1;
        if (!parse_positive_path_id(path, &id)) {
            const char *err_body = "<h1>Invalid order</h1>"
                "<p>Order id must be a positive integer.</p>"
                "<p><a href='/store/orders'>&larr; Back to orders</a></p>";
            result = store_error_response("400 Bad Request",
                err_body, strlen(err_body), response, response_max);
        } else {
            result = serve_order_status(db, id, response, response_max);
        }

    } else if (strncmp(path, "/store/access", 13) == 0) {
        /* Token-gated content: /store/access?addr=t1...&token=ZCL23ACCESS */
        char addr[128] = "", token[64] = "";
        if (!store_parse_access_query(path, addr, sizeof(addr),
                                      token, sizeof(token))) {
            const char *err_body = "<h1>Invalid access request</h1>"
                "<p>addr must be a valid ZClassic address and token must be a valid token id.</p>"
                "<p><a href='/store/products'>&larr; Back to store</a></p>";
            result = store_error_response("400 Bad Request",
                err_body, strlen(err_body), response, response_max);
        } else {
            result = serve_gated_content(db, addr, token, 1, datadir,
                                          response, response_max);
        }
    }

    node_db_close(&ndb);
    return result;
}

/* Background payment processor — called periodically from boot.c.
 * Checks pending orders for payments, mints tokens when paid. */
void store_process_payments(const char *datadir)
{
    if (!datadir) return;

    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    /* Background 30 s scan — a runtime reopen, NOT a boot. Re-running the boot
     * ceremony here every cycle (quick_check + staging DELETE + version banner)
     * is what made a merely-stalled node look like a silent boot loop. */
    if (!node_db_open_runtime(&ndb, db_path, "store.payment_scan")) return;

    struct db_store_pending_payment pending_orders[64];
    int pending_count = db_store_order_list_pending_payments(&ndb,
        pending_orders, sizeof(pending_orders) / sizeof(pending_orders[0]),
        (int64_t)platform_time_wall_time_t() - 3600);

    for (int i = 0; i < pending_count; ++i) {
        int64_t order_id = pending_orders[i].id;
        const char *pay_addr = pending_orders[i].payment_addr;
        int64_t expected = pending_orders[i].amount_zatoshi;
        const char *cust_addr = pending_orders[i].customer_addr;
        const char *token_id = pending_orders[i].token_id;
        int64_t tokens = pending_orders[i].tokens_per_purchase;

        if (!pay_addr[0] || !cust_addr[0] || !token_id[0])
            continue;

        /* Credit only payments whose recovered Sapling memo binds them to
         * THIS order (prefix "ZCL23ORDER:<order_id>", which the payment page
         * instructs the buyer to include). This closes the hole that an
         * unrelated same-amount note at the same one-time address could
         * satisfy the order: the legacy address-only finder counts any note
         * at the address, the memo-bound finder counts only this order's.
         * Require minimum 3 confirmations to prevent reorg-based double-spend
         * (payment reversed but tokens already minted). */
        int64_t tip_height = db_store_chain_tip_height(&ndb);
        int64_t min_height = tip_height - 3; /* 3 confirmations */

        int64_t received = db_store_received_payment_for_memo(&ndb, pay_addr,
                                                              order_id, min_height);

        if (received >= expected) {
            /* Payment confirmed — mint tokens FIRST, then update status.
             * This ensures we never show "Tokens Sent" if mint failed. */
            bool mint_ok = zslp_mint(datadir, token_id, cust_addr,
                                      (uint64_t)tokens);
            int new_status = mint_ok ? STORE_ORDER_SENT : STORE_ORDER_FAILED;
            if (!store_mark_order_paid(datadir, order_id, new_status)) {
                printf("Store: order #%lld payment processed but status "
                       "persist failed\n", (long long)order_id);
                fflush(stdout);
            }

            if (mint_ok) {
                printf("Store: order #%lld paid, minted %lld %s -> %s\n",
                       (long long)order_id, (long long)tokens,
                       token_id, cust_addr);
            } else {
                printf("Store: order #%lld paid but MINT FAILED for %s\n",
                       (long long)order_id, cust_addr);
            }
            fflush(stdout);
        }
    }
    node_db_close(&ndb);
}

/* Check if a customer has enough tokens to access a service.
 * Used as a before_action hook on protected routes. */
bool store_check_token_access(const char *datadir,
                               const char *customer_addr,
                               const char *token_id,
                               uint64_t required)
{
    if (!datadir ||
        !store_validate_access_addr(customer_addr) ||
        !store_validate_access_token(token_id))
        LOG_FAIL("store", "check_token_access: invalid args datadir=%p addr=%s token=%s",
                 (void *)datadir,
                 customer_addr ? customer_addr : "(null)",
                 token_id ? token_id : "(null)");

    uint64_t balance = zslp_balance(datadir, token_id, customer_addr);
    return balance >= required;
}
