/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: the Yardsale app's /yardsale web mount — see
 * controllers/yardsale_site_controller.h for the route table and the
 * no-session/no-CSRF rationale. Renders straight off the zswap_ads
 * rebuildable projection; mutating routes drive the ceremony controller. */

#include "controllers/yardsale_site_controller.h"

#include "base/hex.h"
#include "chain/chainparams.h"
#include "config/runtime.h"
#include "controllers/yardsale_controller.h"
#include "keys/key_io.h"
#include "models/database.h"
#include "models/zswap_ad.h"
#include "net/onion_service.h"
#include "platform/time_compat.h"
#include "support/cleanse.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/template.h"
#include "znam/znam.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── HTTP plumbing (blog_post_controller.c idiom) ────────────────── */

static size_t yardsale_http_response(const char *status,
                                     const char *content_type,
                                     const uint8_t *body, size_t body_len,
                                     uint8_t *response, size_t response_max)
{
    if (!status || !content_type || !response)
        return 0;
    int header_len = snprintf((char *)response, response_max,
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Cache-Control: no-store\r\n"
        "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; "
        "base-uri 'none'; frame-ancestors 'none'\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        status, content_type, body_len);
    if (header_len < 0 || (size_t)header_len >= response_max)
        return 0;
    if (body_len > 0 && body) {
        if (body_len > response_max - (size_t)header_len)
            return 0;
        memcpy(response + header_len, body, body_len);
    }
    return (size_t)header_len + body_len;
}

static size_t yardsale_error_page(const char *status, const char *title,
                                  const char *detail,
                                  uint8_t *response, size_t response_max)
{
    char body[1024];
    int n = snprintf(body, sizeof(body),
        "<!doctype html><html><head><title>%s</title>"
        "<style>body{background:#0a0a0a;color:#e0e0e0;font-family:monospace;"
        "padding:60px 24px}a{color:#00ff88}</style></head><body>"
        "<h1>%s</h1><p>%s</p><p><a href='/yardsale'>Back to the yard</a>"
        "</p></body></html>", title, title, detail);
    if (n < 0)
        return 0;
    return yardsale_http_response(status, "text/html; charset=utf-8",
                                  (const uint8_t *)body, (size_t)n,
                                  response, response_max);
}

/* ── Small render helpers ────────────────────────────────────────── */

static void hex_short(const uint8_t *bytes, size_t len, size_t keep,
                      char *out, size_t out_cap)
{
    if (keep > len)
        keep = len;
    size_t need = 2 * keep + 1;
    if (out_cap < need + 3)
        return;
    zcl_hex_encode(bytes, keep, out);
    memcpy(out + 2 * keep, "...", 4);
}

/* ── Form parsing (name_site_controller.c idiom) ─────────────────── */

static void url_decode(char *dst, size_t dstmax, const char *src,
                       size_t srclen)
{
    size_t di = 0;
    if (!dstmax)
        return;
    for (size_t si = 0; si < srclen && di < dstmax - 1; si++) {
        char c = src[si];
        if (c == '%' && si + 2 < srclen) {
            int hi = zcl_hex_nibble(src[si + 1], true);
            int lo = zcl_hex_nibble(src[si + 2], true);
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

static const char *parse_form_field(const char *body, size_t len,
                                    const char *field, char *out,
                                    size_t outmax)
{
    if (!body || !len || !field || !out || outmax == 0)
        return NULL;
    char search[96];
    snprintf(search, sizeof(search), "%s=", field);
    const char *p = strstr(body, search);
    if (!p)
        return NULL;
    p += strlen(search);
    size_t remaining = len - (size_t)(p - body);
    size_t vlen = 0;
    while (vlen < remaining && p[vlen] && p[vlen] != '&' && p[vlen] != ' ')
        vlen++;
    url_decode(out, outmax, p, vlen);
    return out;
}

/* ── GET /yardsale — the yard ────────────────────────────────────── */

#define YARDSALE_PAGE_STYLE \
    "body{background:#0a0a0a;color:#e0e0e0;font-family:monospace;" \
    "padding:40px 24px;max-width:1100px;margin:auto}" \
    "a{color:#00ff88}table{border-collapse:collapse;width:100%%}" \
    "td,th{border:1px solid #333;padding:6px 8px;text-align:left;" \
    "font-size:13px}th{color:#00ff88}input{width:100%%;background:#111;" \
    "color:#e0e0e0;border:1px solid #333;font-family:monospace;" \
    "padding:4px}label{font-size:12px;color:#999}" \
    ".seller{margin:10px 0}.desc{color:#999;font-size:12px}"

/* Defined just below the index renderer; appends the "Known sellers"
 * section at `off` and returns the byte count it wrote. */
static size_t yardsale_render_known_sellers(struct node_db *ndb, int64_t now,
                                            uint8_t *out, size_t out_cap,
                                            size_t off);

static size_t yardsale_render_index(struct node_db *ndb, int64_t now,
                                    uint8_t *out, size_t out_cap)
{
    struct zswap_yardsale_ad ads[ZSWAP_YARDSALE_QUERY_CAP];
    int count = db_zswap_ad_list_live(ndb, now, ads,
                                      ZSWAP_YARDSALE_QUERY_CAP);

    size_t off = 0;
    int n = snprintf((char *)out, out_cap,
        "<!doctype html><html><head><title>ZClassic Yardsale</title>"
        "<style>" YARDSALE_PAGE_STYLE "</style></head><body>"
        "<h1>The Yardsale</h1>"
        "<p>Signed for-sale-by-owner signs this node remembers, best unit "
        "price first. Every sign is Ed25519-sealed by its seller and "
        "relayed byte-identically; buying is a direct two-message ceremony "
        "with the seller — there is no counterparty in the middle.</p>"
        "<table><tr><th>sign</th><th>token</th><th>amount</th>"
        "<th>price (sats)</th><th>seller</th><th>expires</th>"
        "<th>seen</th></tr>");
    if (n > 0)
        off = (size_t)n;

    for (int i = 0; i < count && off < out_cap - 512; i++) {
        const struct zswap_yardsale_ad *ad = &ads[i];
        char root_hex[65], root_short[40], token_short[40], seller_short[40];
        zcl_hex_encode(ad->quote_root, 32, root_hex);
        hex_short(ad->quote_root, 32, 8, root_short, sizeof(root_short));
        hex_short(ad->quote.token_id, 32, 8, token_short,
                  sizeof(token_short));
        hex_short(ad->quote.seller_pubkey, 32, 8, seller_short,
                  sizeof(seller_short));
        n = snprintf((char *)out + off, out_cap - off,
            "<tr><td><a href='/yardsale/ad/%s'>%s</a></td><td>%s</td>"
            "<td>%llu</td><td>%llu</td><td>%s</td>"
            "<td>%llds</td><td>%llu</td></tr>",
            root_hex, root_short, token_short,
            (unsigned long long)ad->quote.token_amount,
            (unsigned long long)ad->quote.zcl_amount,
            seller_short,
            (long long)(ad->quote.expires_unix - now),
            (unsigned long long)ad->seen_count);
        if (n < 0)
            break;
        off += (size_t)n;
    }
    if (count == 0)
        off += (size_t)snprintf((char *)out + off, out_cap - off,
            "<tr><td colspan='7'>no live signs — the yard is empty"
            "</td></tr>");
    int closed = snprintf((char *)out + off, out_cap - off, "</table>");
    if (closed > 0)
        off += (size_t)closed;
    off += yardsale_render_known_sellers(ndb, now, out, out_cap, off);
    snprintf((char *)out + off, out_cap - off,
        "<p><a href='/'>Home</a></p></body></html>");
    return strlen((const char *)out);
}

/* ── Known sellers (peer-directory read, discovery hints only) ───────
 *
 * Fresh peer_directory rows whose apps advertisement names the yardsale
 * App, rendered as links to their /yardsale mounts. These are discovery
 * HINTS — a row says where to look, never who is there (the directory
 * contract in net/onion_service.h) — and every rendered value passes
 * html_escape on top of the directory's own read-time re-validation. The
 * empty state is honest: gossip carries the ads with or without this
 * section. */

#define YARDSALE_KNOWN_SELLERS_CAP 50

static size_t yardsale_render_known_sellers(struct node_db *ndb, int64_t now,
                                            uint8_t *out, size_t out_cap,
                                            size_t off)
{
    size_t start = off;
    int n = snprintf((char *)out + off, out_cap - off,
        "<h2>Known sellers</h2>");
    if (n > 0)
        off += (size_t)n;

    struct onion_directory_app_peer peers[YARDSALE_KNOWN_SELLERS_CAP];
    int n_peers = onion_directory_app_peers_db(ndb->db, "yardsale", now,
                                               peers,
                                               YARDSALE_KNOWN_SELLERS_CAP);

    int shown = 0;
    for (int i = 0; i < n_peers && off + 768 < out_cap; i++) {
        /* The ZNAM label join that already exists for directory rows: a
         * name is a label for an address, never a substitute, so the raw
         * .onion the buyer would dial is always shown beside it. */
        char name[ZNAM_NAME_MAX + 1];
        name[0] = '\0';
        (void)onion_directory_name_for_db(ndb->db, peers[i].onion,
                                          name, sizeof(name));
        char esc_onion[384], esc_name[256], esc_apps[1600];
        html_escape(esc_onion, sizeof(esc_onion), peers[i].onion);
        html_escape(esc_name, sizeof(esc_name), name);
        html_escape(esc_apps, sizeof(esc_apps), peers[i].apps);
        bool named = name[0] != '\0';
        n = snprintf((char *)out + off, out_cap - off,
            "<div class='seller'><a href='http://%s/yardsale'>%s</a>"
            "<div class='desc'>%s &middot; serves: %s</div></div>",
            esc_onion, named ? esc_name : esc_onion,
            named ? esc_onion : "this node",
            esc_apps);
        if (n < 0)
            break;
        off += (size_t)n;
        shown++;
    }

    if (shown == 0) {
        n = snprintf((char *)out + off, out_cap - off,
            "<p>no sellers discovered yet &mdash; ads still propagate "
            "by gossip.</p>");
        if (n > 0)
            off += (size_t)n;
    }
    return off - start;
}

/* ── GET /yardsale/ad/<root> — one sign + the buy form ───────────── */

static size_t yardsale_render_ad(struct node_db *ndb, int64_t now,
                                 const char *root_hex,
                                 uint8_t *out, size_t out_cap)
{
    uint8_t root[32];
    if (!zcl_hex_decode_lower(root_hex, root, 32))
        return yardsale_error_page("400 Bad Request", "400 Bad Request",
            "the sign root must be 64 lowercase hex characters",
            out, out_cap);

    struct zswap_yardsale_ad ad;
    if (!db_zswap_ad_find(ndb, root, &ad) ||
        ad.quote.expires_unix <= now)
        return yardsale_error_page("404 Not Found", "404 Not Found",
            "no live sign with that root", out, out_cap);

    char token_hex[65], seller_hex[65];
    zcl_hex_encode(ad.quote.token_id, 32, token_hex);
    zcl_hex_encode(ad.quote.seller_pubkey, 32, seller_hex);

    int n = snprintf((char *)out, out_cap,
        "<!doctype html><html><head><title>Yardsale sign %s</title>"
        "<style>" YARDSALE_PAGE_STYLE "</style></head><body>"
        "<h1>Sign <code>%.16s...</code></h1>"
        "<table>"
        "<tr><th>token</th><td><code>%s</code></td></tr>"
        "<tr><th>amount for sale</th><td>%llu base units</td></tr>"
        "<tr><th>price</th><td>%llu sats ZCL (whole amount)</td></tr>"
        "<tr><th>seller</th><td><code>%s</code></td></tr>"
        "<tr><th>valid</th><td>%lld &rarr; %lld (%llds left)</td></tr>"
        "</table>"
        "<h2>Buy it</h2>"
        "<p>Your node builds a <code>zswap_accept.v1</code> from the fields "
        "below and gossips it to the seller. No signatures cross the wire "
        "until the seller's partial answer returns and your node verifies "
        "every term of the sign byte-for-byte; walking away at any point "
        "leaves no transaction and no loss. Input txids are node-internal "
        "byte order, hex.</p>"
        "<form method='POST' action='/yardsale/buy'>"
        "<input type='hidden' name='root' value='%s'>"
        "<label>ZCL input 1: txid:vout:value:scripthex</label>"
        "<input name='in1'>"
        "<label>ZCL input 2 (optional)</label><input name='in2'>"
        "<label>ZCL input 3 (optional)</label><input name='in3'>"
        "<label>ZCL input 4 (optional)</label><input name='in4'>"
        "<label>WIF for input 1</label><input name='key1'>"
        "<label>WIF for input 2</label><input name='key2'>"
        "<label>WIF for input 3</label><input name='key3'>"
        "<label>WIF for input 4</label><input name='key4'>"
        "<label>token receive address</label><input name='token_recv'>"
        "<label>change address</label><input name='change'>"
        "<label>fee (sats)</label><input name='fee'>"
        "<p><button type='submit'>Pin the accept on the seller's "
        "door</button></p>"
        "</form>"
        "<p><a href='/yardsale'>Back to the yard</a></p></body></html>",
        root_hex, root_hex, token_hex,
        (unsigned long long)ad.quote.token_amount,
        (unsigned long long)ad.quote.zcl_amount, seller_hex,
        (long long)ad.quote.issued_unix, (long long)ad.quote.expires_unix,
        (long long)(ad.quote.expires_unix - now), root_hex);
    if (n < 0 || (size_t)n >= out_cap)
        return 0;
    return (size_t)n;
}

/* ── POST /yardsale/buy ──────────────────────────────────────────── */

/* Parse one "txid:vout:value:scripthex" form field. */
static bool parse_input_field(const char *text, struct zswap_swap_input *in)
{
    memset(in, 0, sizeof(*in));
    char txid_hex[65], script_hex[2 * ZSWAP_MAX_INPUT_SCRIPT_BYTES + 1];
    unsigned long vout;
    unsigned long long value;
    /* The script is last (hex has no colons), the txid first. */
    const char *c1 = strchr(text, ':');
    const char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
    const char *c3 = c2 ? strchr(c2 + 1, ':') : NULL;
    if (!c3 || c3 == c2 + 1)
        return false;
    size_t txid_len = (size_t)(c1 - text);
    if (txid_len != 64 ||
        (size_t)(c2 - c1 - 1) >= 16 || (size_t)(c3 - c2 - 1) >= 32)
        return false;
    memcpy(txid_hex, text, 64);
    txid_hex[64] = 0;
    char vout_s[16], value_s[32];
    memcpy(vout_s, c1 + 1, (size_t)(c2 - c1 - 1));
    vout_s[c2 - c1 - 1] = 0;
    memcpy(value_s, c2 + 1, (size_t)(c3 - c2 - 1));
    value_s[c3 - c2 - 1] = 0;
    size_t script_hex_len = strlen(c3 + 1);
    if (script_hex_len == 0 || script_hex_len % 2 != 0 ||
        script_hex_len > 2 * ZSWAP_MAX_INPUT_SCRIPT_BYTES)
        return false;
    memcpy(script_hex, c3 + 1, script_hex_len + 1);
    vout = strtoul(vout_s, NULL, 10);
    value = strtoull(value_s, NULL, 10);
    if (vout > UINT32_MAX || value == 0 || value > INT64_MAX)
        return false;
    if (!zcl_hex_decode_lower(txid_hex, in->txid, 32))
        return false; /* raw-return-ok:form-refusal-named-by-caller */
    size_t script_len = script_hex_len / 2;
    if (!zcl_hex_decode_lower(script_hex, in->script_pub_key, script_len))
        return false; /* raw-return-ok:form-refusal-named-by-caller */
    in->vout = (uint32_t)vout;
    in->value_sats = (int64_t)value;
    in->script_len = (uint16_t)script_len;
    return true;
}

static size_t yardsale_handle_buy_post(const uint8_t *body, size_t body_len,
                                       uint8_t *response,
                                       size_t response_max)
{
    /* The body is form text; bound it before scanning. */
    if (!body || body_len == 0 || body_len > 16384)
        return yardsale_error_page("400 Bad Request", "400 Bad Request",
            "missing or oversized form body", response, response_max);

    char *form = zcl_malloc(body_len + 1, "yardsale buy form");
    if (!form)
        LOG_RETURN(0, "yardsale", "buy POST: form copy allocation failed");
    memcpy(form, body, body_len);
    form[body_len] = 0;

    char root_hex[80], token_recv[ZSWAP_ADDRESS_FIELD_BYTES],
         change[ZSWAP_ADDRESS_FIELD_BYTES], fee_s[24];
    const char *bad = NULL;
    struct zswap_buyer_accept buyer;
    memset(&buyer, 0, sizeof(buyer));
    struct privkey keys[ZSWAP_MAX_BUYER_INPUTS];
    memset(keys, 0, sizeof(keys));

    if (!parse_form_field(form, body_len, "root", root_hex,
                          sizeof(root_hex)) ||
        !parse_form_field(form, body_len, "token_recv", token_recv,
                          sizeof(token_recv)) ||
        !parse_form_field(form, body_len, "change", change,
                          sizeof(change)) ||
        !parse_form_field(form, body_len, "fee", fee_s, sizeof(fee_s)))
        bad = "root, token_recv, change, and fee are required";

    uint8_t root[32];
    if (!bad && !zcl_hex_decode_lower(root_hex, root, 32))
        bad = "root must be 64 lowercase hex characters";

    /* Up to four inputs via in1..in4 / key1..key4 (the web form's v1
     * convenience bound; the controller API takes the full 16). */
    const struct chain_params *cp = chain_params_get();
    size_t sec_pfx_len = 0;
    const unsigned char *sec_pfx =
        chain_params_base58_prefix(cp, B58_SECRET_KEY, &sec_pfx_len);
    for (size_t i = 1; i <= 4 && !bad; i++) {
        char fname[8], kname[8], in_s[640], wif_s[128];
        snprintf(fname, sizeof(fname), "in%zu", i);
        snprintf(kname, sizeof(kname), "key%zu", i);
        bool have_in = parse_form_field(form, body_len, fname, in_s,
                                        sizeof(in_s)) && in_s[0];
        bool have_key = parse_form_field(form, body_len, kname, wif_s,
                                         sizeof(wif_s)) && wif_s[0];
        if (!have_in && !have_key)
            continue;
        if (have_in != have_key) {
            bad = "every input needs its WIF, and every WIF its input";
            break;
        }
        struct zswap_swap_input *in = &buyer.inputs[buyer.num_inputs];
        if (!parse_input_field(in_s, in)) {
            bad = "an input is not txid:vout:value:scripthex";
            break;
        }
        if (!decode_secret(wif_s, sec_pfx, sec_pfx_len,
                           &keys[buyer.num_inputs])) {
            bad = "a WIF did not decode";
            memory_cleanse(wif_s, sizeof(wif_s));
            break;
        }
        memory_cleanse(wif_s, sizeof(wif_s));
        buyer.num_inputs++;
    }

    uint64_t fee = 0;
    if (!bad) {
        if (buyer.num_inputs == 0)
            bad = "at least one ZCL input is required";
        fee = strtoull(fee_s, NULL, 10);
        if (fee == 0)
            bad = "fee must be positive sats";
    }

    size_t wire_len = 0;
    enum yardsale_error result = YARDSALE_ERR_NULL;
    if (!bad) {
        snprintf(buyer.token_recv_address,
                 sizeof(buyer.token_recv_address), "%s", token_recv);
        snprintf(buyer.change_address, sizeof(buyer.change_address),
                 "%s", change);
        buyer.fee_sats = fee;

        struct zswap_yardsale_ad ad;
        int64_t now = (int64_t)platform_time_wall_time_t();
        if (!zswap_yardsale_find(root, &ad)) {
            bad = "that sign is not in this node's yardsale";
        } else {
            buyer.deadline_unix = ad.quote.expires_unix;
            uint8_t wire[ZSWAP_ACCEPT_WIRE_MAX_BYTES];
            result = yardsale_buyer_begin(&ad.quote, &buyer, keys,
                                          buyer.num_inputs, now,
                                          wire, sizeof(wire), &wire_len);
        }
    }
    memory_cleanse(keys, sizeof(keys));
    memory_cleanse(form, body_len); /* raw body holds the WIF strings */
    free(form);

    if (bad)
        return yardsale_error_page("400 Bad Request", "400 Bad Request",
                                   bad, response, response_max);
    if (result != YARDSALE_OK)
        return yardsale_error_page("502 Bad Gateway",
                                   "the accept never left the yard",
                                   yardsale_error_string(result),
                                   response, response_max);

    char page[2048];
    int n = snprintf(page, sizeof(page),
        "<!doctype html><html><head><title>accept out</title>"
        "<style>" YARDSALE_PAGE_STYLE "</style></head><body>"
        "<h1>Accept pinned on the seller's door</h1>"
        "<p>Your <code>zswap_accept.v1</code> (%zu bytes) is gossiping "
        "toward the seller. When his <code>zswap_partial.v1</code> "
        "returns, this node verifies every term of the sign, signs your "
        "inputs, and broadcasts the swap — check <code>zclassic23 "
        "status</code> or the logs for the settlement. If the sign "
        "expires first, the ceremony dies with it and nothing was "
        "lost.</p><p><a href='/yardsale'>Back to the yard</a></p>"
        "</body></html>", wire_len);
    if (n < 0)
        return 0;
    return yardsale_http_response("200 OK", "text/html; charset=utf-8",
                                  (const uint8_t *)page, (size_t)n,
                                  response, response_max);
}

/* ── POST /yardsale/accept — the seller endpoint ─────────────────── */

static size_t yardsale_handle_accept_post(const uint8_t *body,
                                          size_t body_len,
                                          uint8_t *response,
                                          size_t response_max)
{
    if (!body || body_len == 0 || body_len > ZSWAP_ACCEPT_WIRE_MAX_BYTES)
        return yardsale_error_page("400 Bad Request", "400 Bad Request",
            "the body must be one zswap_accept.v1 wire",
            response, response_max);

    uint8_t partial[ZSWAP_PARTIAL_WIRE_MAX_BYTES];
    size_t partial_len = 0;
    int64_t now = (int64_t)platform_time_wall_time_t();
    enum yardsale_error e = yardsale_seller_handle_accept_wire(
        body, body_len, now, partial, sizeof(partial), &partial_len);
    if (e != YARDSALE_OK)
        return yardsale_error_page("422 Unprocessable Content",
                                   "the accept was refused",
                                   yardsale_error_string(e),
                                   response, response_max);
    return yardsale_http_response("200 OK", "application/octet-stream",
                                  partial, partial_len,
                                  response, response_max);
}

/* ── The mount ───────────────────────────────────────────────────── */

size_t yardsale_site_handle_request(const char *method, const char *path,
                                    const uint8_t *body, size_t body_len,
                                    uint8_t *response, size_t response_max)
{
    if (!method || !path || !response || response_max < 1024)
        return 0;

    const char *query = strchr(path, '?');
    size_t path_len = query ? (size_t)(query - path) : strlen(path);
    char clean[256];
    if (path_len == 0 || path_len >= sizeof(clean))
        return yardsale_error_page("400 Bad Request", "400 Bad Request",
            "path too long", response, response_max);
    memcpy(clean, path, path_len);
    clean[path_len] = 0;

    if (strncmp(clean, "/yardsale", 9) != 0 ||
        (clean[9] != 0 && clean[9] != '/'))
        return 0;

    if (strcmp(method, "POST") == 0) {
        if (strcmp(clean, "/yardsale/buy") == 0)
            return yardsale_handle_buy_post(body, body_len, response,
                                            response_max);
        if (strcmp(clean, "/yardsale/accept") == 0)
            return yardsale_handle_accept_post(body, body_len, response,
                                               response_max);
        return yardsale_error_page("404 Not Found", "404 Not Found",
            "no such yardsale action", response, response_max);
    }
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0)
        return yardsale_error_page("405 Method Not Allowed",
            "405 Method Not Allowed", "GET, HEAD or POST only",
            response, response_max);

    /* Read-only pages need the zswap_ads projection. */
    struct node_db *ndb = app_runtime_node_db();
    if (!ndb || !ndb->open) {
        LOG_WARN("yardsale", "public Yardsale requested while node.db is "
                 "unavailable");
        return 0; /* the dispatcher serves its own 503 */
    }

    int64_t now = (int64_t)platform_time_wall_time_t();
    uint8_t *rendered = zcl_malloc(response_max, "yardsale page");
    if (!rendered)
        LOG_RETURN(0, "yardsale", "response allocation failed");

    size_t rendered_len = 0;
    if (clean[9] == 0 || strcmp(clean, "/yardsale/") == 0) {
        rendered_len = yardsale_render_index(ndb, now, rendered,
                                             response_max);
    } else if (strncmp(clean, "/yardsale/ad/", 13) == 0 &&
               strlen(clean + 13) == 64) {
        rendered_len = yardsale_render_ad(ndb, now, clean + 13, rendered,
                                          response_max);
    } else {
        free(rendered);
        return yardsale_error_page("404 Not Found", "404 Not Found",
            "no such yardsale page", response, response_max);
    }

    size_t response_len = 0;
    if (rendered_len > 0)
        response_len = yardsale_http_response(
            "200 OK", "text/html; charset=utf-8", rendered, rendered_len,
            response, response_max);
    free(rendered);
    if (response_len == 0)
        return yardsale_error_page("404 Not Found", "404 Not Found",
            "no such yardsale page", response, response_max);
    return response_len;
}
