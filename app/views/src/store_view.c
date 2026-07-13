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
#include "platform/time_compat.h"
#include "net/fast_sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CSRF form-token + PoW-challenge helpers stay in the controller as
 * security machinery; serve_product_detail only embeds their outputs,
 * so it calls them by declaration here. Defined in store_controller.c. */
void store_csrf_token(const char *context, char out[33]);
void store_csrf_context(char *out, size_t outmax, int64_t product_id);
void store_pow_challenge(int64_t product_id, char peer_id_hex[65]);

/* Client-side hashcash puzzle solver embedded in the order-form page (see
 * app/controllers/src/store_controller.c "Proof-of-work order gate" for
 * the server-side half of this contract). Reimplements SHA3-256
 * (Keccak-f[1600], FIPS 202) from scratch in plain JS — no CDN, no
 * external dependency — because browsers have no native SHA3, only
 * SHA-2, so fast_sync_verify_pow's SHA3-256(peer_id||timestamp||nonce)
 * scheme can't be solved via window.crypto.subtle. Its digests match
 * Node's native `sha3-256` for empty/short/multi-block inputs.
 *
 * On DOMContentLoaded, wires <form id='orderForm'> (data-pow-peer /
 * data-pow-ts / data-pow-bits attributes, written by serve_product_detail
 * below) so submit: (1) intercepts, (2) chunks a search over `nonce`
 * (20000 tries per event-loop tick, so the tab stays responsive) until
 * SHA3-256(peer||ts||nonce) has the required leading zero bits, (3)
 * writes the solution into #pow_nonce_field, (4) calls form.submit()
 * (bypasses the 'submit' listener — no infinite loop). Programmatic /
 * curl-style buyers can solve the same puzzle without a browser: the
 * wire contract is just the two POST fields `pow_ts` (echo the value
 * this page embeds) and `pow_nonce` (any value making the hash check in
 * store_pow_verify_and_claim() pass). */
/* Two literals because one 4750-char literal trips -Woverlength-strings
 * (-Werror; ISO C99's 4095-char minimum translation limit) even though
 * this compiler would accept more. Concatenated at point of use via two
 * adjacent %s substitutions — semantically one script; the boundary sits
 * at the end of sha3_256() to keep each half readable. */
static const char STORE_ORDER_POW_JS_1[] =
    "(function(){\n"
    "'use strict';\n"
    "var RHO=[0,1,62,28,27,36,44,6,55,20,3,10,43,25,39,41,45,15,21,8,18,2,61,56,14];\n"
    "var PI=[0,10,20,5,15,16,1,11,21,6,7,17,2,12,22,23,8,18,3,13,14,24,9,19,4];\n"
    "var RCLO=[0x1,0x8082,0x808a,0x80008000,0x808b,0x80000001,0x80008081,0x8009,0x8a,0x88,0x80008009,0x8000000a,0x8000808b,0x8b,0x8089,0x8003,0x8002,0x80,0x800a,0x8000000a,0x80008081,0x8080,0x80000001,0x80008008];\n"
    "var RCHI=[0,0,0x80000000,0x80000000,0,0,0x80000000,0x80000000,0,0,0,0,0,0x80000000,0x80000000,0x80000000,0x80000000,0x80000000,0,0x80000000,0x80000000,0x80000000,0,0x80000000];\n"
    "function rotl(lo,hi,n){\n"
    "  if(n===0) return [lo,hi];\n"
    "  if(n<32) return [((lo<<n)|(hi>>>(32-n)))>>>0,((hi<<n)|(lo>>>(32-n)))>>>0];\n"
    "  if(n===32) return [hi,lo];\n"
    "  var m=n-32;\n"
    "  return [((hi<<m)|(lo>>>(32-m)))>>>0,((lo<<m)|(hi>>>(32-m)))>>>0];\n"
    "}\n"
    "function keccakF1600(lo,hi){\n"
    "  var Clo=new Uint32Array(5),Chi=new Uint32Array(5),Dlo=new Uint32Array(5),Dhi=new Uint32Array(5);\n"
    "  var Blo=new Uint32Array(25),Bhi=new Uint32Array(25);\n"
    "  for(var round=0;round<24;round++){\n"
    "    for(var x=0;x<5;x++){\n"
    "      Clo[x]=lo[x]^lo[x+5]^lo[x+10]^lo[x+15]^lo[x+20];\n"
    "      Chi[x]=hi[x]^hi[x+5]^hi[x+10]^hi[x+15]^hi[x+20];\n"
    "    }\n"
    "    for(x=0;x<5;x++){\n"
    "      var r=rotl(Clo[(x+1)%5],Chi[(x+1)%5],1);\n"
    "      Dlo[x]=Clo[(x+4)%5]^r[0];\n"
    "      Dhi[x]=Chi[(x+4)%5]^r[1];\n"
    "    }\n"
    "    var i;\n"
    "    for(i=0;i<25;i++){ lo[i]^=Dlo[i%5]; hi[i]^=Dhi[i%5]; }\n"
    "    for(i=0;i<25;i++){\n"
    "      var rr=rotl(lo[i],hi[i],RHO[i]);\n"
    "      Blo[PI[i]]=rr[0]; Bhi[PI[i]]=rr[1];\n"
    "    }\n"
    "    for(var y=0;y<5;y++){\n"
    "      var base=5*y;\n"
    "      for(x=0;x<5;x++){\n"
    "        lo[base+x]=Blo[base+x]^((~Blo[base+(x+1)%5])&Blo[base+(x+2)%5]);\n"
    "        hi[base+x]=Bhi[base+x]^((~Bhi[base+(x+1)%5])&Bhi[base+(x+2)%5]);\n"
    "      }\n"
    "    }\n"
    "    lo[0]^=RCLO[round]; hi[0]^=RCHI[round];\n"
    "  }\n"
    "}\n"
    "var RATE=136;\n"
    "function sha3_256(bytes){\n"
    "  var lo=new Uint32Array(25),hi=new Uint32Array(25);\n"
    "  var padLen=RATE-(bytes.length%RATE);\n"
    "  var full=new Uint8Array(bytes.length+padLen);\n"
    "  full.set(bytes,0);\n"
    "  full[bytes.length]=0x06;\n"
    "  full[full.length-1]|=0x80;\n"
    "  for(var off=0;off<full.length;off+=RATE){\n"
    "    var i;\n"
    "    for(i=0;i<RATE/8;i++){\n"
    "      var o=off+i*8;\n"
    "      lo[i]^=(full[o]|(full[o+1]<<8)|(full[o+2]<<16)|(full[o+3]<<24))>>>0;\n"
    "      hi[i]^=(full[o+4]|(full[o+5]<<8)|(full[o+6]<<16)|(full[o+7]<<24))>>>0;\n"
    "    }\n"
    "    keccakF1600(lo,hi);\n"
    "  }\n"
    "  var out=new Uint8Array(32);\n"
    "  for(var j=0;j<4;j++){\n"
    "    var oo=j*8;\n"
    "    out[oo]=lo[j]&0xff; out[oo+1]=(lo[j]>>>8)&0xff; out[oo+2]=(lo[j]>>>16)&0xff; out[oo+3]=(lo[j]>>>24)&0xff;\n"
    "    out[oo+4]=hi[j]&0xff; out[oo+5]=(hi[j]>>>8)&0xff; out[oo+6]=(hi[j]>>>16)&0xff; out[oo+7]=(hi[j]>>>24)&0xff;\n"
    "  }\n"
    "  return out;\n"
    "}\n";

static const char STORE_ORDER_POW_JS_2[] =
    "function hexToBytes(hex){\n"
    "  var out=new Uint8Array(hex.length/2);\n"
    "  for(var i=0;i<out.length;i++) out[i]=parseInt(hex.substr(i*2,2),16);\n"
    "  return out;\n"
    "}\n"
    "function numToBytesLE(num,n){\n"
    "  var out=new Uint8Array(n);\n"
    "  for(var i=0;i<n;i++){ out[i]=num%256; num=Math.floor(num/256); }\n"
    "  return out;\n"
    "}\n"
    "function leadingZeroBitsOk(hash,bits){\n"
    "  var fullBytes=Math.floor(bits/8);\n"
    "  for(var i=0;i<fullBytes;i++) if(hash[i]!==0) return false;\n"
    "  var rem=bits%8;\n"
    "  if(rem>0){\n"
    "    var mask=(0xff<<(8-rem))&0xff;\n"
    "    if(hash[fullBytes]&mask) return false;\n"
    "  }\n"
    "  return true;\n"
    "}\n"
    "function storePowSolveChunked(peerHex,ts,bits,statusEl,onDone){\n"
    "  var peer=hexToBytes(peerHex);\n"
    "  var tsBytes=numToBytesLE(ts,8);\n"
    "  var buf=new Uint8Array(48);\n"
    "  buf.set(peer,0);\n"
    "  buf.set(tsBytes,32);\n"
    "  var nonce=0;\n"
    "  var startTime=Date.now();\n"
    "  function step(){\n"
    "    var chunkEnd=nonce+20000;\n"
    "    for(;nonce<chunkEnd;nonce++){\n"
    "      var nb=numToBytesLE(nonce,8);\n"
    "      buf.set(nb,40);\n"
    "      var h=sha3_256(buf);\n"
    "      if(leadingZeroBitsOk(h,bits)){\n"
    "        onDone(nonce);\n"
    "        return;\n"
    "      }\n"
    "    }\n"
    "    if(statusEl) statusEl.textContent='Solving proof-of-work... '+nonce+' tries, '+((Date.now()-startTime)/1000).toFixed(1)+'s';\n"
    "    setTimeout(step,0);\n"
    "  }\n"
    "  step();\n"
    "}\n"
    "window.storePowSolveChunked=storePowSolveChunked;\n"
    "document.addEventListener('DOMContentLoaded',function(){\n"
    "  var form=document.getElementById('orderForm');\n"
    "  if(!form) return;\n"
    "  form.addEventListener('submit',function(ev){\n"
    "    var nonceField=document.getElementById('pow_nonce_field');\n"
    "    if(nonceField.value) return;\n"
    "    ev.preventDefault();\n"
    "    var btn=document.getElementById('buyBtn');\n"
    "    var status=document.getElementById('powStatus');\n"
    "    if(btn) btn.disabled=true;\n"
    "    var peerHex=form.getAttribute('data-pow-peer');\n"
    "    var ts=parseInt(form.getAttribute('data-pow-ts'),10);\n"
    "    var bits=parseInt(form.getAttribute('data-pow-bits'),10);\n"
    "    storePowSolveChunked(peerHex,ts,bits,status,function(nonce){\n"
    "      nonceField.value=String(nonce);\n"
    "      if(status) status.textContent='Proof-of-work solved ('+nonce+' tries). Submitting...';\n"
    "      form.submit();\n"
    "    });\n"
    "  });\n"
    "});\n"
    "})();\n";

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
 * memcpy'd unmodified after the header terminator — never routed through a
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

    /* Sized for the base page plus the ~5 KB embedded PoW solver
     * (STORE_ORDER_POW_JS) — the other product pages stay at the
     * smaller 8192 shared elsewhere in this file. */
    char body[16384];
    size_t off = 0;
    int n = html_body_start(body, sizeof(body), safe_name);
    if (n > 0) off = (size_t)n;

    char detail_price[32];
    format_zcl_price(detail_price, sizeof(detail_price), product.price_zatoshi);

    char csrf_ctx[64], csrf_tok[33];
    store_csrf_context(csrf_ctx, sizeof(csrf_ctx), product_id);
    store_csrf_token(csrf_ctx, csrf_tok);

    /* Proof-of-work puzzle challenge: peer_id is bound to this product
     * (see store_pow_challenge / store_pow_bind_product in
     * store_controller.c) and ts is "now" — the client has up to
     * fast_sync_verify_pow's [-300s,+60s] window (minus solve time) to
     * finish and POST. The order-create gate refuses the expensive
     * z-address mint without a solution to this exact challenge. */
    char pow_peer_hex[65];
    store_pow_challenge(product_id, pow_peer_hex);
    int64_t pow_ts = (int64_t)platform_time_wall_time_t();

    n = snprintf(body + off, sizeof(body) - off,
        "<div class='product'>"
        "<h2>%s</h2>"
        "<p>%s</p>"
        "<div class='price'>%s ZCL</div>"
        "<p>You will receive <b>%lld</b> %s tokens.</p>"
        "<h3>Purchase</h3>"
        "<form id='orderForm' method='post' action='/store/orders' "
        "data-pow-peer='%s' data-pow-ts='%lld' data-pow-bits='%d'>"
        "<input type='hidden' name='product_id' value='%lld'>"
        "<input type='hidden' name='csrf_token' value='%s'>"
        "<input type='hidden' name='pow_ts' value='%lld'>"
        "<input type='hidden' name='pow_nonce' id='pow_nonce_field' value=''>"
        "<label>Your t-address (to receive tokens):</label>"
        "<input type='text' name='customer_addr' placeholder='t1...' required>"
        "<br><br>"
        "<button type='submit' class='btn' id='buyBtn'>Generate Payment Address</button>"
        "<p id='powStatus' style='color:#888;font-size:12px'></p>"
        "<noscript><p style='color:#f80'>JavaScript is required to solve "
        "the anti-flood proof-of-work puzzle in your browser. Scripted "
        "buyers may instead solve it directly: SHA3-256(peer_id || "
        "timestamp || nonce) must have %d leading zero bits, where "
        "peer_id=%s (hex) and timestamp is the pow_ts value above; submit "
        "the winning nonce as pow_nonce.</p></noscript>"
        "</form>"
        "</div>"
        "<p><a href='/store/products'>&larr; Back to store</a></p>"
        "<script>%s%s</script>"
        "</body></html>",
        safe_name,
        safe_desc,
        detail_price,
        (long long)product.tokens_per_purchase,
        safe_token,
        pow_peer_hex,
        (long long)pow_ts,
        FAST_SYNC_POW_BITS,
        (long long)product_id,
        csrf_tok,
        (long long)pow_ts,
        FAST_SYNC_POW_BITS,
        pow_peer_hex,
        STORE_ORDER_POW_JS_1,
        STORE_ORDER_POW_JS_2);
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
