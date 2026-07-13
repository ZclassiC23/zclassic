/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Onion service: bridges Tor dynhost to zclassic23 MVC controllers.
 * All .onion traffic flows through here. */

#include "platform/time_compat.h"
#include "net/onion_service.h"
#include "net/tor_integration.h"
#include "util/log_json.h"
#include "util/log_macros.h"
#include "util/path_check.h"
#include "util/template.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <sqlite3.h>
#include "util/ar_step_readonly.h"

struct onion_context {
    char address[128];
    const char *datadir;
    time_t start_time;
    onion_blog_serve_fn blog_serve;
    onion_peer_discover_fn peer_discover;
};

static struct onion_context g_onion_ctx = {0};

static struct onion_context *onion_ctx(void)
{
    return &g_onion_ctx;
}

void onion_service_set_app_handlers(onion_blog_serve_fn blog_serve,
                                    onion_peer_discover_fn peer_discover)
{
    struct onion_context *ctx = onion_ctx();
    ctx->blog_serve = blog_serve;
    ctx->peer_discover = peer_discover;
}

/* On-chain hostnames are attacker-controlled. Only the exact Tor v3
 * shape (56 base32 [a-z2-7] chars + ".onion" = 62) may reach HTML,
 * JSON, or the peer_directory table. */
static bool onion_hostname_valid(const char *h)
{
    if (!h) return false;
    if (strlen(h) != 62 || strcmp(h + 56, ".onion") != 0) return false;
    for (size_t i = 0; i < 56; i++) {
        char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '2' && c <= '7')))
            return false;
    }
    return true;
}

static int onion_discover_peers(struct onion_peer *out, size_t max)
{
    struct onion_context *ctx = onion_ctx();
    if (!ctx->datadir || !ctx->peer_discover)
        return 0;
    int found = ctx->peer_discover(ctx->datadir, out, max);
    /* Drop malformed hostnames before they reach any sink. */
    int kept = 0;
    for (int i = 0; i < found; i++) {
        if (!onion_hostname_valid(out[i].hostname))
            continue;
        if (kept != i) out[kept] = out[i];
        kept++;
    }
    if (kept < found)
        log_jsonf(LOG_JSON_WARN, "onion_hostname_rejected",
                  "\"dropped\":%d", found - kept);
    return kept;
}

/* Simple global rate limiter: max 100 requests/second */
static _Atomic int64_t g_request_count = 0;
static _Atomic int64_t g_rate_window_start = 0;
#define MAX_REQUESTS_PER_SECOND 100

static bool rate_limit_check(void)
{
    int64_t now = (int64_t)platform_time_wall_time_t();
    int64_t window = atomic_load(&g_rate_window_start);
    if (now != window) {
        if (atomic_compare_exchange_strong(&g_rate_window_start, &window, now))
            atomic_store(&g_request_count, 1);
    }
    /* A fresh window seeds the counter at 1 and the line below also fetch_adds,
     * so the first request of a window counts as 2 — the limiter trips one
     * request early (≈99 instead of 100). This conservative bias is INTENTIONAL:
     * for a DoS guard, over-limiting (slightly stricter) is the fail-safe
     * direction. Do not "fix" it toward 100 in a way that risks under-limiting. */
    int64_t count = atomic_fetch_add(&g_request_count, 1);
    return count < MAX_REQUESTS_PER_SECOND;
}

/* ── Query node stats from SQLite ─────────────────────────── */

static void query_node_stats(int *out_height, int *out_peers)
{
    struct onion_context *ctx = onion_ctx();
    *out_height = 0;
    *out_peers = 0;
    if (!ctx->datadir) return;

    char db_path[1024];
    zcl_node_db_path(db_path, sizeof(db_path), ctx->datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        LOG_WARN("net", "node db open failed: %s", db_path);
        return;
    }
    /* 5s timeout — allows reads even during heavy block sync */
    sqlite3_busy_timeout(db, 5000);

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT MAX(height) FROM blocks", -1, &s, NULL) == SQLITE_OK && s) {
        if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW)
            *out_height = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM peers WHERE last_seen > strftime('%s','now') - 3600",
            -1, &s, NULL) == SQLITE_OK && s) {
        if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW)
            *out_peers = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    sqlite3_close(db);
}

/* ── Landing page: node dashboard + directory ─────────────── */

static size_t serve_landing_page(uint8_t *response, size_t max)
{
    struct onion_context *ctx = onion_ctx();
    /* Gather node info */
    int height = 0, peer_count = 0;
    query_node_stats(&height, &peer_count);

    long uptime = 0;
    if (ctx->start_time > 0)
        uptime = (long)(platform_time_wall_time_t() - ctx->start_time);

    /* Discover registered .onion sites from chain */
    struct onion_peer peers[64];
    int num_peers = 0;
    num_peers = onion_discover_peers(peers, 64);

    const char *onion = ctx->address[0] ? ctx->address : NULL;

    /* Build body into a temp buffer, then wrap with Content-Length */
    char body[32768];
    size_t off = 0;
    int n = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<title>ZClassic23 Node</title>"
        "<style>"
        "body{font-family:monospace;background:#0a0a0a;color:#e0e0e0;"
        "max-width:800px;margin:0 auto;padding:20px}"
        "h1{color:#00ff88;text-align:center;font-size:28px}"
        "h2{color:#00cc66;border-bottom:1px solid #333;padding-bottom:8px}"
        "input[type=text]{background:#1a1a1a;color:#e0e0e0;border:1px solid #333;"
        "padding:10px;width:100%%;font-family:monospace;font-size:16px;"
        "border-radius:4px;box-sizing:border-box}"
        ".dashboard{display:grid;grid-template-columns:1fr 1fr 1fr;"
        "gap:12px;margin:20px 0}"
        ".stat{background:#1a1a1a;padding:16px;border-radius:8px;"
        "text-align:center;border-top:2px solid #00ff88}"
        ".stat .val{color:#00ff88;font-size:24px;font-weight:bold}"
        ".stat .label{color:#888;font-size:12px;margin-top:4px}"
        ".onion-addr{background:#111;padding:10px;border-radius:4px;"
        "word-break:break-all;font-size:12px;text-align:center;"
        "color:#00aaff;margin:10px 0}"
        ".nav{display:flex;gap:12px;justify-content:center;margin:20px 0;"
        "flex-wrap:wrap}"
        ".nav a{background:#1a1a1a;color:#00aaff;padding:10px 20px;"
        "border-radius:4px;text-decoration:none;border:1px solid #333}"
        ".nav a:hover{border-color:#00ff88;color:#00ff88}"
        ".apps{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));"
        "gap:12px;margin:24px 0}"
        ".app{background:#121212;padding:14px;border-radius:8px;border:1px solid #2a2a2a}"
        ".app a{color:#00aaff;text-decoration:none;font-size:16px}"
        ".app a:hover{color:#00ff88}"
        ".app .desc{color:#888;font-size:13px;margin-top:6px;line-height:1.35}"
        ".site{background:#1a1a1a;padding:15px;margin:10px 0;border-radius:8px;"
        "border-left:3px solid #00ff88}"
        ".site a{color:#00aaff;text-decoration:none;font-size:16px}"
        ".site a:hover{text-decoration:underline}"
        ".site .desc{color:#888;font-size:13px;margin-top:5px}"
        ".tagline{text-align:center;color:#666;margin:10px 0}"
        "footer{text-align:center;color:#333;margin-top:40px;font-size:11px}"
        "</style></head><body>"
        "<h1>ZClassic23 Node</h1>"
        "<p class='tagline'>A new internet. Tor-only. No DNS. No cloud.</p>");
    if (n > 0) off = (size_t)n;

    /* Node .onion address */
    if (onion) {
        n = snprintf(body + off, sizeof(body) - off,
            "<div class='onion-addr'>%s</div>", onion);
        if (n > 0) off += (size_t)n;
    }

    /* Dashboard stats — detect sync-in-progress */
    bool syncing = (height == 0 && uptime < 600) || (height > 0 && height < 100);
    n = snprintf(body + off, sizeof(body) - off,
        "<div class='dashboard'>"
        "<div class='stat'><div class='val'>%s%d</div>"
        "<div class='label'>Block Height</div></div>"
        "<div class='stat'><div class='val'>%d</div>"
        "<div class='label'>Peers (1h)</div></div>"
        "<div class='stat'><div class='val'>%ldm</div>"
        "<div class='label'>Uptime</div></div>"
        "</div>",
        syncing ? "syncing... " : "", height, peer_count, uptime / 60);
    if (syncing) {
        n += snprintf(body + off + (n > 0 ? n : 0),
            sizeof(body) - off - (size_t)(n > 0 ? n : 0),
            "<p style='text-align:center;color:#ffaa00;font-size:14px'>"
            "Node is syncing the blockchain. Stats will update as blocks are indexed."
            "</p>");
    }
    if (n > 0) off += (size_t)n;

    /* Navigation */
    n = snprintf(body + off, sizeof(body) - off,
        "<div class='nav'>"
        "<a href='/explorer'>Explorer</a>"
        "<a href='/store'>Store</a>"
        "<a href='/blog'>Blog</a>"
        "<a href='/search'>Search</a>"
        "<a href='/directory'>Directory</a>"
        "<a href='/status'>Status API</a>"
        "</div>");
    if (n > 0) off += (size_t)n;

    n = snprintf(body + off, sizeof(body) - off,
        "<h2>Power Node Apps</h2>"
        "<div class='apps'>"
        "<div class='app'><a href='/explorer'>Explorer</a>"
        "<div class='desc'>REST-style chain, block, transaction, address, and token reads.</div></div>"
        "<div class='app'><a href='/store'>Store</a>"
        "<div class='desc'>Commerce and token purchase flows hosted directly on the node.</div></div>"
        "<div class='app'><a href='/blog'>Blog</a>"
        "<div class='desc'>Static site hosting from your datadir over the onion service.</div></div>"
        "<div class='app'><a href='/directory'>Directory</a>"
        "<div class='desc'>On-chain discovered peer/app directory for the Tor-only network.</div></div>"
        "<div class='app'><a href='/status'>Status API</a>"
        "<div class='desc'>Machine-readable node, sync, and onion reachability status.</div></div>"
        "</div>");
    if (n > 0) off += (size_t)n;

    /* Search bar */
    n = snprintf(body + off, sizeof(body) - off,
        "<form action='/search' method='get'>"
        "<input type='text' name='q' placeholder='Search .onion sites by hostname...'>"
        "</form>");
    if (n > 0) off += (size_t)n;

    /* Peer directory */
    n = snprintf(body + off, sizeof(body) - off,
        "<h2>Network Directory (%d peers)</h2>", num_peers);
    if (n > 0) off += (size_t)n;

    if (num_peers == 0 && onion) {
        n = snprintf(body + off, sizeof(body) - off,
            "<div class='site'>"
            "<a href='http://%s/'>This node</a>"
            "<div class='desc'>Your local ZClassic23 node</div></div>",
            onion);
        if (n > 0) off += (size_t)n;
    }

    for (int i = 0; i < num_peers && off + 512 < sizeof(body); i++) {
        char esc_host[384]; /* hostname[64] worst-case html-escaped */
        html_escape(esc_host, sizeof(esc_host), peers[i].hostname);
        n = snprintf(body + off, sizeof(body) - off,
            "<div class='site'>"
            "<a href='http://%s/'>%s</a>"
            "<div class='desc'>Discovered at height %d</div></div>",
            esc_host, esc_host, peers[i].height);
        if (n > 0) off += (size_t)n;
    }

    /* Seed node */
    n = snprintf(body + off, sizeof(body) - off,
        "<div class='site'>"
        "<a href='http://zc23kenfdqqkgamthif3m7lbbdsyrotsl2dlw35qrh3iuzopozmpjnad.onion/'>"
        "zc23kenf...jnad.onion</a>"
        "<div class='desc'>ZClassic23 seed node</div></div>"
        "<h2>Host Your Site</h2>"
        "<div class='site'>"
        "<div class='desc'>Every zclassic23 node is a .onion web server.<br>"
        "Put HTML in <code>{datadir}/blog/</code> and it's live.<br>"
        "Explorer, store, blog, and directory are first-class power-node apps.<br>"
        "Register on-chain via ZSLP for network discovery.</div></div>"
        "<footer>ZClassic23 v0.1.0 &mdash; pure C23 full node + Tor</footer>"
        "</body></html>");
    if (n > 0) off += (size_t)n;

    /* Wrap with HTTP headers including Content-Length */
    return (size_t)snprintf((char *)response, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n"
        "%s", off, body);
}

/* ── Search handler ───────────────────────────────────────── */

static size_t serve_search(const char *query, uint8_t *response, size_t max)
{
    struct onion_peer peers[64];
    int num_peers = 0;
    num_peers = onion_discover_peers(peers, 64);

    char safe_query[512];
    html_escape(safe_query, sizeof(safe_query), query ? query : "");

    char body[16384];
    size_t off = 0;
    int n = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'><title>Search: %s</title>"
        "<style>body{font-family:monospace;background:#0a0a0a;color:#e0e0e0;"
        "max-width:800px;margin:0 auto;padding:20px}"
        "h1{color:#00ff88;text-align:center}a{color:#00aaff}"
        ".nav{display:flex;gap:12px;justify-content:center;margin:20px 0;flex-wrap:wrap}"
        ".nav a{background:#1a1a1a;color:#00aaff;padding:10px 20px;"
        "border-radius:4px;text-decoration:none;border:1px solid #333}"
        ".nav a:hover{border-color:#00ff88;color:#00ff88}"
        ".site{background:#1a1a1a;padding:15px;margin:10px 0;border-radius:8px;"
        "border-left:3px solid #00ff88}"
        ".note{color:#666;font-size:13px;margin:15px 0}"
        "</style></head><body>"
        "<h1><a href='/' style='text-decoration:none;color:#00ff88'>ZClassic23</a></h1>"
        "<div class='nav'>"
        "<a href='/'>Home</a>"
        "<a href='/explorer'>Explorer</a>"
        "<a href='/store'>Store</a>"
        "<a href='/blog'>Blog</a>"
        "<a href='/directory'>Directory</a>"
        "</div>"
        "<h2 style='color:#00cc66'>Search</h2>"
        "<p>Results for: <b>%s</b></p>",
        safe_query, safe_query);
    if (n > 0) off = (size_t)n;

    int found = 0;
    for (int i = 0; i < num_peers && off + 256 < sizeof(body); i++) {
        if (query && query[0] &&
            !strstr(peers[i].hostname, query))
            continue;
        char esc_host[384]; /* hostname[64] worst-case html-escaped */
        html_escape(esc_host, sizeof(esc_host), peers[i].hostname);
        n = snprintf(body + off, sizeof(body) - off,
            "<div class='site'><a href='http://%s/'>%s</a></div>",
            esc_host, esc_host);
        if (n > 0) off += (size_t)n;
        found++;
    }

    if (found == 0) {
        n = snprintf(body + off, sizeof(body) - off,
            "<p style='color:#666'>No results.</p>");
        if (n > 0) off += (size_t)n;
    }

    n = snprintf(body + off, sizeof(body) - off,
        "<p class='note'>Search matches against .onion hostnames registered "
        "on-chain via ZSLP. Peer nodes do not yet broadcast titles or "
        "descriptions &mdash; only hostnames are searchable.</p>"
        "</body></html>");
    if (n > 0) off += (size_t)n;

    return (size_t)snprintf((char *)response, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n"
        "%s", off, body);
}

/* ── Peer directory table ─────────────────────────────────── */

static void ensure_directory_table(sqlite3 *db)
{
    char *err = NULL;
    int rc = sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS peer_directory ("
        "onion_address TEXT PRIMARY KEY,"
        "port INTEGER NOT NULL DEFAULT 8033,"
        "services INTEGER NOT NULL DEFAULT 0,"
        "height INTEGER NOT NULL DEFAULT 0,"
        "last_seen INTEGER NOT NULL,"
        "version TEXT,"
        "self INTEGER NOT NULL DEFAULT 0,"
        "clearnet_ip TEXT DEFAULT '',"
        "clearnet_port INTEGER DEFAULT 0"
        ")", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "onion_service: failed to create directory table: %s\n",  // obs-ok:pre-existing-diagnostic
                err ? err : "unknown");
        sqlite3_free(err);
    }
    /* Add clearnet columns to existing databases */
    sqlite3_exec(db, "ALTER TABLE peer_directory ADD COLUMN clearnet_ip TEXT DEFAULT ''",
                 NULL, NULL, NULL);
    sqlite3_exec(db, "ALTER TABLE peer_directory ADD COLUMN clearnet_port INTEGER DEFAULT 0",
                 NULL, NULL, NULL);
}

/* Populate directory from chain scan (ZSLP .onion announcements) */
static void populate_directory_from_chain(sqlite3 *db)
{
    struct onion_context *ctx = onion_ctx();
    if (!ctx->datadir) return;

    struct onion_peer peers[256];
    int found = onion_discover_peers(peers, 256);

    if (found <= 0) return;

    sqlite3_stmt *ins = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO peer_directory "
        "(onion_address, height, last_seen, version) "
        "VALUES (?, ?, strftime('%s','now'), 'chain')",
        -1, &ins, NULL) != SQLITE_OK || !ins) {
        LOG_WARN("net", "failed to prepare peer insert: %s", sqlite3_errmsg(db));
        return;
    }

    for (int i = 0; i < found; i++) {
        if (!peers[i].hostname[0]) continue;
        sqlite3_reset(ins);
        sqlite3_bind_text(ins, 1, peers[i].hostname, -1, SQLITE_STATIC);
        sqlite3_bind_int(ins, 2, peers[i].height);
        (void)AR_STEP_WRITE(ins);
    }
    sqlite3_finalize(ins);

    log_jsonf(LOG_JSON_INFO, "onion_directory_loaded",
              "\"peers_loaded\":%d", found);
}

/* Register our own .onion address with clearnet IP if known */
static void register_self(sqlite3 *db)
{
    struct onion_context *ctx = onion_ctx();
    if (!ctx->address[0]) return;

    /* Discover our public IP */
    extern void peer_strategy_discover_self(void *profile, uint16_t port);
    struct { bool has_public_ip; bool nat; bool upnp; bool tor;
             uint8_t public_ip[4]; uint16_t public_port;
             char onion_address[68]; } profile = {0};
    peer_strategy_discover_self(&profile, 8033);

    char ip_str[64] = "";
    if (profile.has_public_ip) {
        snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
                 profile.public_ip[0], profile.public_ip[1],
                 profile.public_ip[2], profile.public_ip[3]);
    }

    sqlite3_stmt *ins = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO peer_directory "
        "(onion_address, port, services, height, last_seen, version, self,"
        " clearnet_ip, clearnet_port) "
        "VALUES (?, 8033, 1029, 0, strftime('%s','now'), '0.1.0', 1, ?, ?)",
        -1, &ins, NULL) != SQLITE_OK || !ins) {
        fprintf(stderr, "onion_service: failed to prepare self-register: %s\n",
                sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_text(ins, 1, ctx->address, -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, ip_str[0] ? ip_str : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(ins, 3, ip_str[0] ? 8033 : 0);
    (void)AR_STEP_WRITE(ins);
    sqlite3_finalize(ins);
}

/* ── Directory endpoints ─────────────────────────────────── */

static size_t serve_directory_json(uint8_t *response, size_t max)
{
    struct onion_context *ctx = onion_ctx();
    if (!ctx->datadir) return 0;

    char db_path[1024];
    zcl_node_db_path(db_path, sizeof(db_path), ctx->datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return 0;
    }
    sqlite3_busy_timeout(db, 5000);

    char body[65536];
    size_t off = 0;
    int n = snprintf(body, sizeof(body), "{\"nodes\":[");
    if (n > 0) off = (size_t)n;

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT onion_address, port, services, height, last_seen, "
        "version, self, clearnet_ip, clearnet_port FROM peer_directory "
        "ORDER BY self DESC, last_seen DESC LIMIT 500",
        -1, &s, NULL) != SQLITE_OK || !s) {
        sqlite3_close(db);
        return 0;
    }

    int count = 0;
    while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW && off + 512 < sizeof(body)) {
        const char *addr = (const char *)sqlite3_column_text(s, 0);
        int port = sqlite3_column_int(s, 1);
        int svc = sqlite3_column_int(s, 2);
        int h = sqlite3_column_int(s, 3);
        int64_t ls = sqlite3_column_int64(s, 4);
        const char *ver = (const char *)sqlite3_column_text(s, 5);
        int self = sqlite3_column_int(s, 6);
        const char *cip = (const char *)sqlite3_column_text(s, 7);
        int cport = sqlite3_column_int(s, 8);

        /* Rows stored by pre-validation binaries may be hostile. */
        if (!onion_hostname_valid(addr)) continue;
        char addr_esc[160], ver_esc[96], cip_esc[96];
        log_json_escape(addr_esc, sizeof(addr_esc), addr);
        log_json_escape(ver_esc, sizeof(ver_esc), ver);
        log_json_escape(cip_esc, sizeof(cip_esc), cip);

        if (count > 0) off += (size_t)snprintf(body + off, sizeof(body) - off, ",");
        off += (size_t)snprintf(body + off, sizeof(body) - off,
            "{\"onion\":\"%s\",\"port\":%d,\"services\":%d,"
            "\"height\":%d,\"last_seen\":%lld,"
            "\"version\":\"%s\",\"self\":%s,"
            "\"clearnet_ip\":\"%s\",\"clearnet_port\":%d}",
            addr_esc, port, svc, h,
            (long long)ls, ver_esc,
            self ? "true" : "false",
            cip_esc, cport);
        count++;
    }
    sqlite3_finalize(s);
    sqlite3_close(db);

    off += (size_t)snprintf(body + off, sizeof(body) - off,
        "],\"count\":%d}", count);

    return (size_t)snprintf((char *)response, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n"
        "%s", off, body);
}

static size_t serve_directory_html(uint8_t *response, size_t max)
{
    struct onion_context *ctx = onion_ctx();
    if (!ctx->datadir) return 0;

    char db_path[1024];
    zcl_node_db_path(db_path, sizeof(db_path), ctx->datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return 0;
    }
    sqlite3_busy_timeout(db, 5000);

    char body[65536];
    size_t off = 0;
    int n = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<title>ZClassic23 Node Directory</title>"
        "<style>"
        "body{font-family:monospace;background:#0a0a0a;color:#e0e0e0;"
        "max-width:900px;margin:0 auto;padding:20px}"
        "h1{color:#00ff88;text-align:center}"
        ".nav{display:flex;gap:12px;justify-content:center;margin:20px 0;flex-wrap:wrap}"
        ".nav a{background:#1a1a1a;color:#00aaff;padding:10px 20px;"
        "border-radius:4px;text-decoration:none;border:1px solid #333}"
        ".nav a:hover{border-color:#00ff88;color:#00ff88}"
        "table{width:100%%;border-collapse:collapse;margin:20px 0}"
        "th{background:#1a1a1a;color:#00ff88;padding:10px;text-align:left}"
        "td{padding:8px 10px;border-bottom:1px solid #222}"
        "a{color:#00aaff;text-decoration:none}"
        "a:hover{text-decoration:underline}"
        ".self{background:#0a1a0a;border-left:3px solid #00ff88}"
        ".count{text-align:center;color:#888;margin:10px 0}"
        "footer{text-align:center;color:#333;margin-top:40px;font-size:11px}"
        "</style></head><body>"
        "<h1>ZClassic23 Node Directory</h1>"
        "<div class='nav'>"
        "<a href='/'>Home</a>"
        "<a href='/explorer'>Explorer</a>"
        "<a href='/store'>Store</a>"
        "<a href='/blog'>Blog</a>"
        "<a href='/search'>Search</a>"
        "</div>"
        "<p class='count'>Decentralized .onion network &mdash; every node is a server</p>");
    if (n > 0) off = (size_t)n;

    off += (size_t)snprintf(body + off, sizeof(body) - off,
        "<table><tr><th>.onion Address</th><th>Port</th>"
        "<th>Height</th><th>Last Seen</th><th>Version</th></tr>");

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT onion_address, port, height, last_seen, version, self "
        "FROM peer_directory ORDER BY self DESC, last_seen DESC LIMIT 500",
        -1, &s, NULL) != SQLITE_OK || !s) {
        sqlite3_close(db);
        return 0;
    }

    int count = 0;
    while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW && off + 512 < sizeof(body)) {
        const char *addr = (const char *)sqlite3_column_text(s, 0);
        int port = sqlite3_column_int(s, 1);
        int h = sqlite3_column_int(s, 2);
        int64_t ls = sqlite3_column_int64(s, 3);
        const char *ver = (const char *)sqlite3_column_text(s, 4);
        int self = sqlite3_column_int(s, 5);

        /* Rows stored by pre-validation binaries may be hostile. */
        if (!onion_hostname_valid(addr)) continue;
        char addr_esc[160], ver_esc[96];
        html_escape(addr_esc, sizeof(addr_esc), addr);
        html_escape(ver_esc, sizeof(ver_esc), ver);

        /* Format last_seen as relative time */
        int64_t age = (int64_t)platform_time_wall_time_t() - ls;
        char age_str[32];
        if (age < 60) snprintf(age_str, sizeof(age_str), "%llds ago", (long long)age);
        else if (age < 3600) snprintf(age_str, sizeof(age_str), "%lldm ago", (long long)(age/60));
        else if (age < 86400) snprintf(age_str, sizeof(age_str), "%lldh ago", (long long)(age/3600));
        else snprintf(age_str, sizeof(age_str), "%lldd ago", (long long)(age/86400));

        off += (size_t)snprintf(body + off, sizeof(body) - off,
            "<tr%s><td><a href='http://%s/'>%s</a>%s</td>"
            "<td>%d</td><td>%d</td><td>%s</td><td>%s</td></tr>",
            self ? " class='self'" : "",
            addr_esc, addr_esc,
            self ? " (this node)" : "",
            port, h, age_str, ver_esc);
        count++;
    }
    sqlite3_finalize(s);
    sqlite3_close(db);

    off += (size_t)snprintf(body + off, sizeof(body) - off,
        "</table>"
        "<p class='count'>%d nodes in directory</p>"
        "<p class='count'><a href='/directory.json'>JSON API</a> | "
        "<a href='/'>Home</a></p>"
        "<footer>ZClassic23 &mdash; Decentralized Internet</footer>"
        "</body></html>", count);

    return (size_t)snprintf((char *)response, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n"
        "%s", off, body);
}

/* ── Status endpoint (JSON API) ───────────────────────────── */

static size_t serve_status(uint8_t *response, size_t max)
{
    struct onion_context *ctx = onion_ctx();
    int height = 0, peers = 0;
    query_node_stats(&height, &peers);

    long uptime = 0;
    if (ctx->start_time > 0)
        uptime = (long)(platform_time_wall_time_t() - ctx->start_time);

    /* Query extra stats from SQLite */
    int64_t last_block_time = 0, tx_count = 0;
    if (ctx->datadir) {
        char db_path[1024];
        zcl_node_db_path(db_path, sizeof(db_path), ctx->datadir);
        sqlite3 *db = NULL;
        if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
            sqlite3_busy_timeout(db, 5000);
            sqlite3_stmt *s = NULL;
            if (sqlite3_prepare_v2(db,
                    "SELECT time FROM blocks ORDER BY height DESC LIMIT 1",
                    -1, &s, NULL) == SQLITE_OK && s) {
                if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW)
                    last_block_time = sqlite3_column_int64(s, 0);
                sqlite3_finalize(s);
            }
            s = NULL;
            if (sqlite3_prepare_v2(db,
                    "SELECT count(*) FROM transactions",
                    -1, &s, NULL) == SQLITE_OK && s) {
                if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW)
                    tx_count = sqlite3_column_int64(s, 0);
                sqlite3_finalize(s);
            }
            sqlite3_close(db);
        }
    }

    int64_t now = (int64_t)platform_time_wall_time_t();
    int64_t last_block_age = (last_block_time > 0) ? now - last_block_time : -1;
    bool is_syncing = (height == 0 && uptime < 600) ||
                      (last_block_age > 600 && uptime > 300);

    const char *onion = ctx->address[0] ? ctx->address : NULL;

    char body[1024];
    int blen = snprintf(body, sizeof(body),
        "{\"height\":%d"
        ",\"peers\":%d"
        ",\"version\":\"0.1.0\""
        ",\"uptime\":%ld"
        ",\"syncing\":%s"
        ",\"tor_ready\":%s"
        ",\"onion_service_ready\":%s"
        ",\"last_block_age\":%lld"
        ",\"transactions\":%lld"
        "%s%s%s"
        "}",
        height, peers, uptime,
        is_syncing ? "true" : "false",
        tor_integration_is_ready() ? "true" : "false",
        onion ? "true" : "false",
        (long long)last_block_age,
        (long long)tx_count,
        onion ? ",\"onion\":\"" : "",
        onion ? onion : "",
        onion ? "\"" : "");
    if (blen < 0) blen = 0;

    return (size_t)snprintf((char *)response, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n"
        "%s", blen, body);
}

/* ── Main request handler ─────────────────────────────────── */

size_t onion_service_handle_request(const char *method,
                                     const char *path,
                                     const uint8_t *body,
                                     size_t body_len,
                                     uint8_t *response,
                                     size_t response_max)
{
    if (!path) path = "/";

    /* Rate limit: 100 requests/second across all circuits */
    if (!rate_limit_check()) {
        return (size_t)snprintf((char *)response, response_max,
            "HTTP/1.1 429 Too Many Requests\r\n"
            "Content-Type: text/html; charset=utf-8\r\nConnection: close\r\n"
            "Retry-After: 1\r\n\r\n"
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<title>429 Too Many Requests</title>"
            "<style>"
            "body{font-family:monospace;background:#0a0a0a;color:#e0e0e0;"
            "display:flex;flex-direction:column;align-items:center;"
            "justify-content:center;min-height:90vh;margin:0;padding:20px}"
            "h1{color:#ffaa00;font-size:28px}"
            "p{color:#888;font-size:16px;max-width:500px;text-align:center}"
            "a{color:#00aaff;text-decoration:none}"
            "a:hover{color:#00ff88}"
            ".nav{display:flex;gap:12px;margin-top:30px}"
            "</style></head><body>"
            "<h1>429 Too Many Requests</h1>"
            "<p>Too many requests. Please wait a moment and try again.</p>"
            "<div class='nav'>"
            "<a href='/'>Home</a> | "
            "<a href='/explorer'>Explorer</a>"
            "</div></body></html>");
    }

    /* JSON status endpoint */
    if (strcmp(path, "/status") == 0)
        return serve_status(response, response_max);

    /* Node directory — JSON API */
    if (strcmp(path, "/directory.json") == 0)
        return serve_directory_json(response, response_max);

    /* Node directory — HTML page */
    if (strcmp(path, "/directory") == 0 || strcmp(path, "/directory/") == 0)
        return serve_directory_html(response, response_max);

    /* Landing page / directory */
    if (strcmp(path, "/") == 0)
        return serve_landing_page(response, response_max);

    /* Search */
    if (strncmp(path, "/search", 7) == 0) {
        const char *q = strstr(path, "q=");
        return serve_search(q ? q + 2 : "", response, response_max);
    }

    /* Explorer — block explorer */
    extern const char *explorer_canonical_shortcut(const char *path);
    if (strncmp(path, "/explorer", 9) == 0 ||
        explorer_canonical_shortcut(path) != NULL) {
        extern size_t explorer_handle_request(const char *, const char *,
            const uint8_t *, size_t, uint8_t *, size_t);
        size_t n = explorer_handle_request(method, path, body, body_len,
                                           response, response_max);
        if (n > 0) return n;
    }

    /* Store — ZSLP token commerce */
    if (strncmp(path, "/store", 6) == 0 && onion_ctx()->datadir) {
        extern size_t store_handle_request(const char *, const char *,
            const uint8_t *, size_t, uint8_t *, size_t, const char *);
        return store_handle_request(method, path, body, body_len,
                                    response, response_max,
                                    onion_ctx()->datadir);
    }

    /* ZCL Names — name→site resolution (/n/<name>) + registry (/names).
     * Same handler is wired into the HTTPS dispatch chain, so a name
     * resolves identically over onion and HTTPS. */
    if (strncmp(path, "/n/", 3) == 0 || strncmp(path, "/names", 6) == 0) {
        extern size_t name_site_handle_request(const char *, const char *,
            const uint8_t *, size_t, uint8_t *, size_t);
        return name_site_handle_request(method, path, body, body_len,
                                        response, response_max);
    }

    /* Blog (static files from datadir) */
    if (strncmp(path, "/blog", 5) == 0) {
        struct onion_context *ctx = onion_ctx();
        if (ctx->datadir && ctx->blog_serve)
            return ctx->blog_serve(ctx->datadir, path,
                                   (char *)response, response_max);
    }

    /* 404 */
    return (size_t)snprintf((char *)response, response_max,
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<title>404 Not Found</title>"
        "<style>"
        "body{font-family:monospace;background:#0a0a0a;color:#e0e0e0;"
        "display:flex;flex-direction:column;align-items:center;"
        "justify-content:center;min-height:90vh;margin:0;padding:20px}"
        "h1{color:#ff4444;font-size:28px}"
        "p{color:#888;font-size:16px}"
        "a{color:#00aaff;text-decoration:none}"
        "a:hover{color:#00ff88}"
        ".nav{display:flex;gap:12px;margin-top:30px}"
        ".nav a{background:#1a1a1a;color:#00aaff;padding:10px 20px;"
        "border-radius:4px;border:1px solid #333}"
        ".nav a:hover{border-color:#00ff88;color:#00ff88}"
        "</style></head><body>"
        "<h1>404 Not Found</h1>"
        "<p>The page you requested does not exist.</p>"
        "<div class='nav'>"
        "<a href='/'>Home</a>"
        "<a href='/explorer'>Explorer</a>"
        "<a href='/store'>Store</a>"
        "<a href='/blog'>Blog</a>"
        "</div></body></html>");
}

/* ── Lifecycle ────────────────────────────────────────────── */

const char *onion_service_start(const char *datadir)
{
    struct onion_context *ctx = onion_ctx();
    ctx->datadir = datadir;
    ctx->start_time = platform_time_wall_time_t();

    /* Initialize peer directory from chain data */
    if (datadir) {
        char db_path[1024];
        zcl_node_db_path(db_path, sizeof(db_path), datadir);
        sqlite3 *db = NULL;
        if (sqlite3_open(db_path, &db) == SQLITE_OK) {
            sqlite3_busy_timeout(db, 5000);
            ensure_directory_table(db);
            populate_directory_from_chain(db);
            if (ctx->address[0])
                register_self(db);
            sqlite3_close(db);
        }
    }

    return ctx->address[0] ? ctx->address : NULL;
}

void onion_service_stop(void)
{
    onion_ctx()->datadir = NULL;
}

const char *onion_service_get_address(void)
{
    return onion_ctx()->address[0] ? onion_ctx()->address : NULL;
}

void onion_service_set_address(const char *address)
{
    struct onion_context *ctx = onion_ctx();
    if (address) {
        snprintf(ctx->address, sizeof(ctx->address), "%s", address);

        /* Register ourselves in the peer directory */
        if (ctx->datadir) {
            char db_path[1024];
            zcl_node_db_path(db_path, sizeof(db_path), ctx->datadir);
            sqlite3 *db = NULL;
            if (sqlite3_open(db_path, &db) == SQLITE_OK) {
                sqlite3_busy_timeout(db, 5000);
                ensure_directory_table(db);
                register_self(db);
                sqlite3_close(db);
                char addr_safe[96];
                log_json_escape(addr_safe, sizeof(addr_safe), address);
                log_jsonf(LOG_JSON_INFO, "onion_self_registered",
                          "\"address\":\"%s\"", addr_safe);
            }
        }
    } else {
        ctx->address[0] = '\0';
    }
}
