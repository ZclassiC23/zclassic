/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Store VIEW — ZSLP token commerce presentation layer.
 * View shape: the store controller parses and dispatches requests, then
 * delegates rendering here. This file owns read-only page assembly (product
 * list/detail, order index/status, token-gated content) plus response/format
 * helpers. Render functions take already-validated arguments and already-open
 * DB handles; routing, auth, request parsing, and state mutation stay in the
 * controller. The balance check in serve_gated_content delegates to
 * zslp_balance / store_check_token_access, which remain owned by the
 * controller / service layer. */

#include "views/store_internal.h"
#include "views/format_helpers.h"
#include "controllers/zslp_controller.h"
#include "models/database.h"
#include "models/store_blob.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CSRF form-token helpers stay in the controller as security machinery;
 * serve_product_detail only embeds the token, so it calls them by
 * declaration here. Defined in store_controller.c. */
void store_csrf_token(const char *context, char out[33]);
void store_csrf_context(char *out, size_t outmax, int64_t product_id);

/* Format ZCL price: trim trailing zeros but keep at least 2 decimals. */
void format_zcl_price(char *out, size_t out_len, int64_t zatoshi)
{
    zcl_format_zcl_trimmed(out, out_len, zatoshi, 2);
}

const char *store_get_onion_address(void)
{
    extern const char *onion_service_get_address(void);
    return onion_service_get_address();
}

/* HTML body start (no HTTP headers — those are added by store_wrap_response) */
int html_body_start(char *buf, size_t max, const char *title)
{
    const char *onion = store_get_onion_address();
    return snprintf(buf, max,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<title>%s</title><style>"
        "body{font-family:monospace;background:#0a0a0a;color:#e0e0e0;"
        "max-width:800px;margin:0 auto;padding:20px}"
        "h1{color:#00ff88}h2{color:#00cc66}"
        "a{color:#00aaff;text-decoration:none}"
        ".header-nav{display:flex;align-items:center;gap:16px;"
        "border-bottom:1px solid #333;padding-bottom:12px;margin-bottom:16px;"
        "flex-wrap:wrap}"
        ".header-nav a{font-size:13px}"
        ".onion-id{font-size:11px;color:#666;word-break:break-all}"
        ".product{background:#1a1a1a;padding:20px;margin:15px 0;"
        "border-radius:8px;border-left:3px solid #00ff88}"
        ".price{color:#00ff88;font-size:20px;font-weight:bold}"
        ".btn{display:inline-block;background:#00ff88;color:#0a0a0a;"
        "padding:10px 20px;border-radius:4px;font-weight:bold;margin-top:10px}"
        ".addr{background:#111;padding:10px;border-radius:4px;word-break:break-all;"
        "font-size:12px;margin:10px 0}"
        ".status{padding:8px 16px;border-radius:4px;display:inline-block}"
        ".pending{background:#333;color:#ff8800}"
        ".paid{background:#1a3a1a;color:#00ff88}"
        ".failed{background:#3a1a1a;color:#ff4444}"
        "input{background:#1a1a1a;color:#e0e0e0;border:1px solid #333;"
        "padding:8px;font-family:monospace;width:100%%;margin:5px 0;"
        "box-sizing:border-box}"
        "</style></head><body>"
        "<div class='header-nav'>"
        "<h1 style='margin:0'><a href='/store'>ZCL Store</a></h1>"
        "<a href='/'>Home</a>"
        "<a href='/store/products'>Products</a>"
        "<a href='/store/orders'>Orders</a>"
        "%s%s%s"
        "</div>",
        title,
        onion ? "<div class='onion-id'>" : "",
        onion ? onion : "",
        onion ? "</div>" : "");
}

/* Wrap an HTML body with HTTP headers including Content-Length. */
static size_t store_wrap_response(const char *body, size_t body_len,
                                   const char *status,
                                   const char *content_type,
                                   uint8_t *resp, size_t max)
{
    return (size_t)snprintf((char *)resp, max,
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n"
        "%.*s",
        status, content_type, body_len,
        (int)body_len, body);
}

/* Wrap arbitrary bytes (which may contain embedded NULs) as a 200 OK
 * response with the given content-type and Content-Length. The body is
 * memcpy'd verbatim after the header terminator — never routed through a
 * printf %s, which would truncate at the first NUL. When
 * download_filename is non-NULL a Content-Disposition: attachment header
 * is added so browsers save (rather than render) the payload.
 *
 * Returns the total response length, or 0 (writing nothing) if the
 * headers+body would not fit in `max` — callers must treat 0 as failure
 * so we never overflow the fixed dynhost response buffer. */
size_t store_binary_response(const uint8_t *body, size_t body_len,
                             const char *content_type,
                             const char *download_filename,
                             uint8_t *resp, size_t max)
{
    if (!resp || max == 0 || (!body && body_len > 0))
        return 0;
    if (!content_type || !content_type[0])
        content_type = "application/octet-stream";

    char disposition[STORE_BLOB_NAME_MAX + 64];
    disposition[0] = '\0';
    if (download_filename && download_filename[0]) {
        /* Strip any quote/CR/LF from the filename to keep the header
         * well-formed (header injection guard). */
        char safe_name[STORE_BLOB_NAME_MAX + 1];
        size_t j = 0;
        for (size_t i = 0; download_filename[i] && j < sizeof(safe_name) - 1; i++) {
            char c = download_filename[i];
            if (c == '"' || c == '\r' || c == '\n')
                continue;
            safe_name[j++] = c;
        }
        safe_name[j] = '\0';
        if (safe_name[0])
            snprintf(disposition, sizeof(disposition),
                     "Content-Disposition: attachment; filename=\"%s\"\r\n",
                     safe_name);
    }

    int hdr_len = snprintf((char *)resp, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        "Connection: close\r\n\r\n",
        content_type, body_len, disposition);
    if (hdr_len < 0)
        return 0;
    /* snprintf may have truncated the header (return >= max). Reject. */
    if ((size_t)hdr_len >= max)
        return 0;
    if (body_len > max - (size_t)hdr_len)
        return 0;   /* body would overflow the response buffer */

    if (body_len > 0)
        memcpy(resp + hdr_len, body, body_len);
    return (size_t)hdr_len + body_len;
}

/* Convenience: wrap a 200 OK HTML response with Content-Length. */
size_t store_html_response(const char *body, size_t body_len,
                                   uint8_t *resp, size_t max)
{
    return store_wrap_response(body, body_len,
        "200 OK", "text/html; charset=utf-8", resp, max);
}

/* Convenience: wrap an error HTML response with Content-Length. */
size_t store_error_response(const char *status_code,
                                    const char *body, size_t body_len,
                                    uint8_t *resp, size_t max)
{
    return store_wrap_response(body, body_len,
        status_code, "text/html; charset=utf-8", resp, max);
}

const char *store_order_status_text(int status)
{
    switch (status) {
    case STORE_ORDER_PENDING: return "Pending Payment";
    case STORE_ORDER_PAID: return "Payment Received";
    case STORE_ORDER_SENT: return "Tokens Sent";
    case STORE_ORDER_FAILED: return "Mint Failed (contact support)";
    default: return "Unknown";
    }
}

const char *store_order_status_class(int status)
{
    switch (status) {
    case STORE_ORDER_PENDING: return "pending";
    case STORE_ORDER_FAILED: return "failed";
    default: return "paid";
    }
}

static const char PRODUCT_CARD_TEMPLATE[] =
    "<div class='product'>"
    "<h3><a href='/store/products/{{id}}'>{{name}}</a></h3>"
    "<p>{{description}}</p>"
    "<div class='price'>{{price}} ZCL</div>"
    "<a href='/store/products/{{id}}' class='btn'>View</a>"
    "</div>";

size_t serve_order_index(sqlite3 *db, uint8_t *resp, size_t max)
{
    struct node_db ndb = { .db = db, .open = true };
    struct db_store_order_summary orders[50];
    char body[16384];
    size_t off = 0;
    int n = html_body_start(body, sizeof(body), "Orders");
    if (n > 0) off = (size_t)n;

    n = snprintf(body + off, sizeof(body) - off,
        "<h2>Recent Orders</h2>");
    if (n > 0) off += (size_t)n;

    int count = db_store_order_list_recent(&ndb, orders,
        sizeof(orders) / sizeof(orders[0]));
    for (int i = 0; i < count && off + 768 < sizeof(body); ++i) {
        char safe_product[256], price_str[32];
        html_escape(safe_product, sizeof(safe_product),
                    orders[i].product_name[0] ? orders[i].product_name
                                              : "Unknown Product");
        format_zcl_price(price_str, sizeof(price_str), orders[i].amount_zatoshi);
        n = snprintf(body + off, sizeof(body) - off,
            "<div class='product'>"
            "<h3><a href='/store/orders/%lld'>Order #%lld</a></h3>"
            "<p>%s</p>"
            "<div class='price'>%s ZCL</div>"
            "<p>Status: %s</p>"
            "</div>",
            (long long)orders[i].id, (long long)orders[i].id,
            safe_product, price_str, store_order_status_text(orders[i].status));
        if (n > 0) off += (size_t)n;
    }

    if (count == 0) {
        n = snprintf(body + off, sizeof(body) - off,
            "<p style='color:#666'>No orders yet.</p>");
        if (n > 0) off += (size_t)n;
    }

    n = snprintf(body + off, sizeof(body) - off, "</body></html>");
    if (n > 0) off += (size_t)n;
    return store_html_response(body, off, resp, max);
}

/* GET /store — list products */
size_t serve_product_list(sqlite3 *db, uint8_t *resp, size_t max)
{
    struct node_db ndb = { .db = db, .open = true };
    struct db_store_product products[64];
    char body[16384];
    size_t off = 0;
    int n = html_body_start(body, sizeof(body), "ZCL Store");
    if (n > 0) off = (size_t)n;

    n = snprintf(body + off, sizeof(body) - off,
        "<h2>Products</h2>");
    if (n > 0) off += (size_t)n;

    int count = db_store_product_list_active(&ndb, products,
        sizeof(products) / sizeof(products[0]));
    for (int i = 0; i < count && off + 1024 < sizeof(body); ++i) {
        char id_str[32], price_str[32];
        snprintf(id_str, sizeof(id_str), "%lld", (long long)products[i].id);
        format_zcl_price(price_str, sizeof(price_str), products[i].price_zatoshi);

        struct template_var vars[] = {
            { "id",          id_str },
            { "name",        products[i].name[0] ? products[i].name : "?" },
            { "description", products[i].description },
            { "price",       price_str },
        };

        size_t rendered = template_render(PRODUCT_CARD_TEMPLATE,
            vars, sizeof(vars) / sizeof(vars[0]),
            body + off, sizeof(body) - off);
        off += rendered;
    }

    if (count == 0) {
        n = snprintf(body + off, sizeof(body) - off,
            "<p style='color:#666'>No products yet. "
            "Add products to the SQLite database.</p>");
        if (n > 0) off += (size_t)n;
    }

    n = snprintf(body + off, sizeof(body) - off, "</body></html>");
    if (n > 0) off += (size_t)n;

    return store_html_response(body, off, resp, max);
}

/* GET /store/product/:id — product detail */
size_t serve_product_detail(sqlite3 *db, int64_t product_id,
                                    uint8_t *resp, size_t max)
{
    struct node_db ndb = { .db = db, .open = true };
    struct db_store_product product;
    memset(&product, 0, sizeof(product));

    if (!db_store_product_find_active(&ndb, product_id, &product)) {
        const char *body = "<h1>Product not found</h1>"
            "<p><a href='/store/products'>Back to store</a></p>";
        return store_error_response("404 Not Found", body, strlen(body),
                                     resp, max);
    }

    char safe_name[256], safe_desc[512], safe_token[128];
    html_escape(safe_name, sizeof(safe_name),
                product.name[0] ? product.name : "?");
    html_escape(safe_desc, sizeof(safe_desc), product.description);
    html_escape(safe_token, sizeof(safe_token),
                product.token_id[0] ? product.token_id : "TOKENS");

    char body[8192];
    size_t off = 0;
    int n = html_body_start(body, sizeof(body), safe_name);
    if (n > 0) off = (size_t)n;

    char detail_price[32];
    format_zcl_price(detail_price, sizeof(detail_price), product.price_zatoshi);

    char csrf_ctx[64], csrf_tok[33];
    store_csrf_context(csrf_ctx, sizeof(csrf_ctx), product_id);
    store_csrf_token(csrf_ctx, csrf_tok);

    n = snprintf(body + off, sizeof(body) - off,
        "<div class='product'>"
        "<h2>%s</h2>"
        "<p>%s</p>"
        "<div class='price'>%s ZCL</div>"
        "<p>You will receive <b>%lld</b> %s tokens.</p>"
        "<h3>Purchase</h3>"
        "<form method='post' action='/store/orders'>"
        "<input type='hidden' name='product_id' value='%lld'>"
        "<input type='hidden' name='csrf_token' value='%s'>"
        "<label>Your t-address (to receive tokens):</label>"
        "<input type='text' name='customer_addr' placeholder='t1...' required>"
        "<br><br>"
        "<button type='submit' class='btn'>Generate Payment Address</button>"
        "</form>"
        "</div>"
        "<p><a href='/store/products'>&larr; Back to store</a></p>"
        "</body></html>",
        safe_name,
        safe_desc,
        detail_price,
        (long long)product.tokens_per_purchase,
        safe_token,
        (long long)product_id,
        csrf_tok);
    if (n > 0) off += (size_t)n;

    return store_html_response(body, off, resp, max);
}

/* GET /store/order/:id — check status */
size_t serve_order_status(sqlite3 *db, int64_t order_id,
                                  uint8_t *resp, size_t max)
{
    struct node_db ndb = { .db = db, .open = true };
    struct db_store_order_view order;
    memset(&order, 0, sizeof(order));

    if (!db_store_order_find_view(&ndb, order_id, &order)) {
        const char *body = "<h1>Order not found</h1>"
            "<p><a href='/store/orders'>Back to store</a></p>";
        return store_error_response("404 Not Found", body, strlen(body),
                                     resp, max);
    }

    char body[8192];
    size_t off = 0;
    int n = html_body_start(body, sizeof(body), "Order Status");
    if (n > 0) off = (size_t)n;

    /* Auto-refresh while pending */
    if (order.status == STORE_ORDER_PENDING) {
        n = snprintf(body + off, sizeof(body) - off,
            "<meta http-equiv='refresh' content='15'>");
        if (n > 0) off += (size_t)n;
    }

    char safe_pay[256], safe_cust[256];
    char safe_ptxid[256], safe_mtxid[256];
    char safe_product[256];
    html_escape(safe_pay, sizeof(safe_pay), order.payment_addr[0] ? order.payment_addr : "?");
    html_escape(safe_cust, sizeof(safe_cust), order.customer_addr[0] ? order.customer_addr : "?");
    html_escape(safe_ptxid, sizeof(safe_ptxid), order.payment_txid);
    html_escape(safe_mtxid, sizeof(safe_mtxid), order.mint_txid);
    html_escape(safe_product, sizeof(safe_product),
                order.product_name[0] ? order.product_name : "Unknown Product");

    char status_price[32];
    format_zcl_price(status_price, sizeof(status_price), order.amount_zatoshi);

    n = snprintf(body + off, sizeof(body) - off,
        "<h2>Order #%lld</h2>"
        "<div class='product'>"
        "<h3>%s</h3>"
        "<div class='status %s'>%s</div>"
        "<p>Amount: <span class='price'>%s ZCL</span></p>"
        "<p>Payment address:</p><div class='addr'>%s</div>"
        "<p>Deliver to:</p><div class='addr'>%s</div>"
        "%s%s%s%s%s%s"
        "<p><a href='/store/orders/%lld'>Refresh</a> | "
        "<a href='/store/orders'>&larr; Back to orders</a></p>"
        "</div></body></html>",
        (long long)order_id,
        safe_product,
        store_order_status_class(order.status),
        store_order_status_text(order.status),
        status_price,
        safe_pay,
        safe_cust,
        order.payment_txid[0] ? "<p>Payment: <code>" : "",
        order.payment_txid[0] ? safe_ptxid : "",
        order.payment_txid[0] ? "</code></p>" : "",
        order.mint_txid[0] ? "<p>Mint: <code>" : "",
        order.mint_txid[0] ? safe_mtxid : "",
        order.mint_txid[0] ? "</code></p>" : "",
        (long long)order_id);
    if (n > 0) off += (size_t)n;

    return store_html_response(body, off, resp, max);
}

/* Serve a token-gated page. Checks balance, returns content or 403. */
size_t serve_gated_content(sqlite3 *db, const char *customer_addr,
                                   const char *token_id, uint64_t required,
                                   const char *datadir,
                                   uint8_t *resp, size_t max)
{
    uint64_t balance = zslp_balance(datadir, token_id, customer_addr);

    char safe_token[128], safe_addr[256];
    html_escape(safe_token, sizeof(safe_token), token_id ? token_id : "");
    html_escape(safe_addr, sizeof(safe_addr),
                customer_addr ? customer_addr : "");

    if (!store_check_token_access(datadir, customer_addr, token_id, required)) {
        char body[2048];
        int blen = snprintf(body, sizeof(body),
            "<!DOCTYPE html><html><head><style>"
            "body{font-family:monospace;background:#0a0a0a;color:#e0e0e0;"
            "max-width:800px;margin:0 auto;padding:40px}"
            "h1{color:#ff4444}a{color:#00aaff;text-decoration:none}"
            "</style></head><body>"
            "<h1>Access Denied</h1>"
            "<p>This service requires %llu %s tokens.</p>"
            "<p>Your balance: %llu</p>"
            "<p><a href='/store'>&larr; Get tokens</a> | "
            "<a href='/'>Home</a></p>"
            "</body></html>",
            (unsigned long long)required, safe_token,
            (unsigned long long)balance);
        if (blen < 0) blen = 0;
        return store_error_response("403 Forbidden",
            body, (size_t)blen, resp, max);
    }

    /* Customer holds the token. If the product behind this token attached
     * a file payload, stream the real bytes; otherwise fall back to the
     * HTML "access granted" page (backward compatible for all tokens
     * without an attached file). */
    struct node_db ndb = { .db = db, .open = (db != NULL) };
    struct db_store_product product;
    memset(&product, 0, sizeof(product));

    if (db && token_id &&
        db_store_product_find_by_token(&ndb, token_id, &product) &&
        product.has_content) {

        struct db_store_blob blob;
        memset(&blob, 0, sizeof(blob));

        if (db_store_blob_find(&ndb, product.content_hash, &blob)) {
            int64_t blob_size = blob.size_bytes;
            if (blob.data_len <= STORE_BLOB_INLINE_MAX) {
                /* Inline path: stream the actual file bytes. */
                size_t out_len = store_binary_response(
                    blob.data, blob.data_len, blob.content_type,
                    blob.filename[0] ? blob.filename : NULL, resp, max);
                db_store_blob_free(&blob);
                if (out_len > 0)
                    return out_len;
                /* Did not fit the response buffer — fall through to the
                 * too-large HTML page below rather than overflow. */
            } else {
                db_store_blob_free(&blob);
            }

            /* Too large for the inline (.onion 64 KiB) path: serve an
             * honest HTML page stating size + content hash rather than
             * silently truncating the payload. */
            char hex[2 * 32 + 1];
            for (int i = 0; i < 32; i++)
                snprintf(hex + i * 2, 3, "%02x", product.content_hash[i]);
            char big_body[2048];
            int blen = snprintf(big_body, sizeof(big_body),
                "<!DOCTYPE html><html><head><style>"
                "body{font-family:monospace;background:#0a0a0a;color:#e0e0e0;"
                "max-width:800px;margin:0 auto;padding:40px}"
                "h1{color:#00ff88}a{color:#00aaff;text-decoration:none}"
                ".card{background:#1a1a1a;padding:20px;margin:15px 0;"
                "border-radius:8px;border-left:3px solid #00ff88}"
                "code{word-break:break-all}"
                "</style></head><body>"
                "<h1>Premium Content</h1>"
                "<div class='card'>"
                "<p>Welcome, %s</p>"
                "<p>This download is %lld bytes — too large to serve inline "
                "over the onion path (max %d bytes). Chunked download is a "
                "future extension.</p>"
                "<p>Content hash (SHA3-256):<br><code>%s</code></p>"
                "</div>"
                "<p><a href='/store'>&larr; Back to store</a> | "
                "<a href='/'>Home</a></p>"
                "</body></html>",
                safe_addr,
                (long long)blob_size,
                STORE_BLOB_INLINE_MAX,
                hex);
            if (blen < 0) blen = 0;
            return store_html_response(big_body, (size_t)blen, resp, max);
        }
        /* Product references a blob that isn't present — fall through to
         * the access-granted page rather than 500. */
    }

    /* No file payload — emit the existing "access granted" page. */
    char body[2048];
    int blen = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head><style>"
        "body{font-family:monospace;background:#0a0a0a;color:#e0e0e0;"
        "max-width:800px;margin:0 auto;padding:40px}"
        "h1{color:#00ff88}a{color:#00aaff;text-decoration:none}"
        ".card{background:#1a1a1a;padding:20px;margin:15px 0;border-radius:8px;"
        "border-left:3px solid #00ff88}"
        "</style></head><body>"
        "<h1>Premium Service</h1>"
        "<div class='card'>"
        "<p>Welcome, %s</p>"
        "<p>Your token balance: %llu %s</p>"
        "<p>You have access to this service.</p>"
        "</div>"
        "<p><a href='/store'>&larr; Back to store</a> | "
        "<a href='/'>Home</a></p>"
        "</body></html>",
        safe_addr,
        (unsigned long long)balance,
        safe_token);
    if (blen < 0) blen = 0;
    return store_html_response(body, (size_t)blen, resp, max);
}
