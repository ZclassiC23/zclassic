/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Onion service: bridges Tor dynhost to zclassic23 MVC controllers.
 * All .onion traffic flows through here. */

#include "platform/time_compat.h"
#include "net/onion_service.h"
#include "net/onion_ratelimit.h"
#include "net/tor_integration.h"
#include "net/rom_seed.h"
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

/* ── Shared page chrome for the onion front door ──────────────
 *
 * Inline twin of the site design system's tokens (app/views/src/site.css).
 * These pages live in lib/net — below the app/views layer — and the front
 * door must render even when the explorer asset cache is not initialized,
 * so they carry their own compact copy of the palette/type instead of
 * linking /explorer/style.css or including an app/views header. Keep the
 * values in lockstep with site.css's :root tokens (dark variant; both
 * themes are declared via color-scheme so form controls follow the page).
 * Single % everywhere: always substituted via a "%s" argument, never
 * embedded in a printf format string. */
static const char ONION_PAGE_CSS[] =
    ":root{--bg:#0a0d12;--panel:#121722;--panel-2:#171d2a;--border:#232b3a;"
    "--border-strong:#313b4e;--ink:#e9eef6;--ink-dim:#c3ccd9;--muted:#97a3b6;"
    "--accent:#5ea8ff;--accent-ink:#b9d9ff;--ok:#3ddc97;--warn:#f0b44c;"
    "--bad:#f2708a;color-scheme:dark light}"
    "*{box-sizing:border-box}"
    "body{margin:0 auto;max-width:860px;padding:16px 24px 40px;"
    "background:var(--bg);color:var(--ink);font-family:-apple-system,"
    "BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;"
    "font-size:16px;line-height:1.6}"
    "h1{font-size:28px;font-weight:720;letter-spacing:-0.015em;margin:0 0 12px}"
    "h2{font-size:20px;font-weight:680;margin:28px 0 12px;padding-bottom:8px;"
    "border-bottom:1px solid var(--border)}"
    "a{color:var(--accent);text-decoration:none}"
    "a:hover{color:var(--accent-ink);text-decoration:underline}"
    "a:focus-visible,input:focus-visible,button:focus-visible{outline:3px "
    "solid var(--ok);outline-offset:2px;border-radius:6px}"
    ".site-top{display:flex;align-items:center;gap:4px 18px;flex-wrap:wrap;"
    "padding:10px 0 14px;margin-bottom:18px;border-bottom:1px solid var(--border)}"
    ".site-top .brand{display:flex;align-items:center;gap:10px;margin-right:auto;"
    "font-weight:760;color:var(--ink)}"
    ".site-top .brand:hover{text-decoration:none}"
    ".site-top .brand .glyph{display:grid;place-items:center;width:30px;height:30px;"
    "border-radius:9px;border:1px solid var(--border-strong);"
    "background:linear-gradient(145deg,rgba(94,168,255,.13),rgba(167,139,250,.14));"
    "color:var(--accent-ink);font-weight:800}"
    ".site-top nav{display:flex;gap:2px;flex-wrap:wrap}"
    ".site-top nav a{padding:6px 11px;border-radius:6px;border:1px solid transparent;"
    "color:var(--muted);font-size:14px;font-weight:620;white-space:nowrap}"
    ".site-top nav a:hover{color:var(--ink);background:var(--panel);"
    "border-color:var(--border);text-decoration:none}"
    ".card,.site{background:linear-gradient(180deg,var(--panel-2) 0,var(--panel) 100%);"
    "border:1px solid var(--border);border-left:3px solid var(--accent);"
    "border-radius:10px;padding:16px 20px;margin:12px 0}"
    ".site a{font-size:16px}"
    ".desc{color:var(--muted);font-size:13px;margin-top:5px;line-height:1.45}"
    ".dashboard{display:grid;grid-template-columns:1fr 1fr 1fr;gap:12px;margin:18px 0}"
    ".stat{background:var(--panel);border:1px solid var(--border);border-radius:10px;"
    "padding:16px;text-align:center}"
    ".stat .val{color:var(--ok);font-size:24px;font-weight:700;"
    "font-variant-numeric:tabular-nums}"
    ".stat .label{color:var(--muted);font-size:12px;margin-top:4px;"
    "text-transform:uppercase;letter-spacing:.05em}"
    ".addr,.onion-addr{background:var(--bg);border:1px solid var(--border);"
    "padding:10px 12px;border-radius:6px;word-break:break-all;"
    "font-family:ui-monospace,Menlo,Consolas,monospace;font-size:13px;"
    "color:var(--accent);margin:10px 0;text-align:center}"
    ".apps{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));"
    "gap:12px;margin:20px 0}"
    ".app{background:var(--panel);border:1px solid var(--border);border-radius:10px;"
    "padding:14px}"
    ".app a{font-size:16px;font-weight:640}"
    "input[type=text]{background:var(--bg);color:var(--ink);"
    "border:1px solid var(--border-strong);border-radius:6px;padding:10px 12px;"
    "width:100%;font-size:15px;box-sizing:border-box}"
    "table{width:100%;border-collapse:collapse;font-size:15px;background:var(--panel);"
    "border:1px solid var(--border);border-radius:10px;overflow:hidden;margin:16px 0}"
    "th{text-align:left;color:var(--muted);padding:10px 12px;"
    "border-bottom:1px solid var(--border-strong);font-size:12px;"
    "text-transform:uppercase;letter-spacing:.04em}"
    "td{padding:9px 12px;border-bottom:1px solid var(--border)}"
    ".self{border-left:3px solid var(--ok)}"
    ".table-wrap{overflow-x:auto;max-width:100%}"
    ".muted{color:var(--muted)}"
    ".veil{min-height:70vh;display:flex;flex-direction:column;align-items:center;"
    "justify-content:center;text-align:center}"
    ".veil p{color:var(--muted);max-width:560px}"
    "footer{text-align:center;color:var(--muted);font-size:13px;margin-top:40px;"
    "padding:16px;border-top:1px solid var(--border)}"
    "@media(max-width:640px){.dashboard{grid-template-columns:1fr}"
    ".site-top{gap:4px 10px}}";

/* The same global nav the app-side site_layout.h emits (kept in lockstep;
 * duplicated here because lib/net sits below app/views). */
#define ONION_GLOBAL_NAV \
    "<header class='site-top'>" \
    "<a class='brand' href='/'>" \
    "<span class='glyph' aria-hidden='true'>Z</span>" \
    "<span>ZClassic23</span></a>" \
    "<nav aria-label='Site'>" \
    "<a href='/explorer'>Explorer</a>" \
    "<a href='/names'>Names</a>" \
    "<a href='/store'>Store</a>" \
    "<a href='/blog'>Blog</a>" \
    "<a href='/directory'>Directory</a>" \
    "</nav></header>"

/* Request admission — cost-tiered budgets plus an adaptive client puzzle
 * on expensive routes under sustained pressure. See net/onion_ratelimit.h
 * for the route table, tier sizing, and escalation thresholds. Replaces a
 * single global 100 req/s counter, under which one flooding client on any
 * path starved every honest client on every path. */

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
        "<!DOCTYPE html><html lang='en'><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ZClassic23 Node</title>"
        "<style>%s</style></head><body>"
        ONION_GLOBAL_NAV
        "<h1>ZClassic23 Node</h1>"
        "<p class='muted'>A new internet. Tor-only. No DNS. No cloud.</p>",
        ONION_PAGE_CSS);
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
            "<p class='muted' style='color:var(--warn)'>"
            "Node is syncing the blockchain. Stats will update as blocks are indexed."
            "</p>");
    }
    if (n > 0) off += (size_t)n;

    /* Search bar */
    n = snprintf(body + off, sizeof(body) - off,
        "<form action='/search' method='get'>"
        "<label for='onion-q' class='muted' style='display:block;font-size:13px;"
        "font-weight:640;margin:12px 0 4px'>Search the network</label>"
        "<input type='text' id='onion-q' name='q' placeholder='Search .onion sites by hostname...'>"
        "</form>");
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
        "<!DOCTYPE html><html lang='en'><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Search: %s</title>"
        "<style>%s</style></head><body>"
        ONION_GLOBAL_NAV
        "<h1>Search</h1>"
        "<p>Results for: <b>%s</b></p>",
        safe_query, ONION_PAGE_CSS, safe_query);
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
            "<p class='muted'>No results.</p>");
        if (n > 0) off += (size_t)n;
    }

    n = snprintf(body + off, sizeof(body) - off,
        "<p class='muted'>Search matches against .onion hostnames registered "
        "on-chain via ZSLP. Peer nodes do not yet broadcast titles or "
        "descriptions &mdash; only hostnames are searchable.</p>"
        "<footer>ZClassic23 &mdash; one binary, one onion, one stack</footer>"
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

    /* Advertise the free ROM/sync artifacts this node seeds (kind + digest +
     * size + chunking) so a fresh node can discover WHO seeds WHAT without
     * waiting for gossip warm-up. Only appended when there is room; the JSON
     * stays well-formed either way. */
    char arts[2560];
    size_t an = rom_seed_directory_json(arts, sizeof(arts));
    const char *arts_body = (an > 0 && off + an + 64 < sizeof(body))
                                ? arts : "[]";
    off += (size_t)snprintf(body + off, sizeof(body) - off,
        "],\"count\":%d,\"artifacts\":%s}", count, arts_body);

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
        "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ZClassic23 Node Directory</title>"
        "<style>%s</style></head><body>"
        ONION_GLOBAL_NAV
        "<h1>Node Directory</h1>"
        "<p class='muted'>Decentralized .onion network &mdash; every node is a server</p>",
        ONION_PAGE_CSS);
    if (n > 0) off = (size_t)n;

    off += (size_t)snprintf(body + off, sizeof(body) - off,
        "<div class='table-wrap'><table><tr><th>.onion Address</th><th>Port</th>"
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
        "</table></div>"
        "<p class='muted'>%d nodes in directory</p>"
        "<p class='muted'><a href='/directory.json'>JSON API</a> | "
        "<a href='/'>Home</a></p>"
        "<footer>ZClassic23 &mdash; one binary, one onion, one stack</footer>"
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

/* ── Admission-control responses ──────────────────────────── */

/* Always serveable: a fixed page rendered from a stack buffer, no DB
 * access and no allocation. Every tier routes its over-budget case here,
 * so the node can always say why a request was refused. */
static size_t serve_rate_limited(uint8_t *response, size_t response_max)
{
    return (size_t)snprintf((char *)response, response_max,
        "HTTP/1.1 429 Too Many Requests\r\n"
        "Content-Type: text/html; charset=utf-8\r\nConnection: close\r\n"
        "Retry-After: 1\r\n\r\n"
        "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>429 Too Many Requests</title>"
        "<style>%s</style></head><body>"
        "<div class='veil'>"
        "<h1><span style='color:var(--warn)'>429</span> Too Many Requests</h1>"
        "<p>Too many requests. Please wait a moment and try again.</p>"
        "<p><a href='/'>Home</a> &middot; <a href='/explorer'>Explorer</a></p>"
        "</div></body></html>",
        ONION_PAGE_CSS);
}

/* 402 for an expensive route while the tier is escalated. The page is
 * readable by a human and carries the same challenge as a JSON block a
 * script can read, so the browser front-end and a CLI client solve from
 * one response. */
static size_t serve_puzzle_required(const struct onion_pow_challenge *ch,
                                    uint8_t *response, size_t response_max)
{
    return (size_t)snprintf((char *)response, response_max,
        "HTTP/1.1 402 Payment Required\r\n"
        "Content-Type: text/html; charset=utf-8\r\nConnection: close\r\n"
        "Cache-Control: no-store\r\nRetry-After: 1\r\n\r\n"
        "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>402 Proof of Work Required</title>"
        "<script type='application/json' id='pow-challenge'>"
        "{\"seed\":\"%s\",\"token\":\"%s\",\"bits\":%d,\"server_time\":%lld}"
        "</script>"
        "<style>%s</style></head><body>"
        "<div class='veil'>"
        "<h1><span style='color:var(--warn)'>402</span> Proof of Work Required</h1>"
        "<p>This node is under sustained load on this class of request and "
        "is temporarily pricing it with a small puzzle. Find a nonce such "
        "that SHA3-256(seed || token || ts || nonce), each of seed and "
        "token 32 raw bytes and each of ts and nonce 8 bytes "
        "little-endian, has %d leading zero bits.</p>"
        "<p>seed <code>%s</code></p>"
        "<p>token <code>%s</code></p>"
        "<p>server time <code>%lld</code></p>"
        "<p>Resubmit with <code>pow_ts</code> (the unix-second timestamp "
        "you hashed) and <code>pow_nonce</code> (decimal) as query "
        "parameters on a GET or form fields on a POST. One solution admits "
        "one request. This clears automatically once load drops.</p>"
        "<p><a href='/'>Home</a></p>"
        "</div></body></html>",
        ch->seed_hex, ch->token_hex, ch->bits, (long long)ch->server_time,
        ONION_PAGE_CSS,
        ch->bits, ch->seed_hex, ch->token_hex, (long long)ch->server_time);
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

    /* Admission: per-tier budgets, plus a route-bound puzzle on expensive
     * routes while that tier is escalated. Static assets and the landing
     * page draw from a budget the other tiers cannot touch, so a flood on
     * search or order creation can never take the node's front page down. */
    struct onion_pow_challenge challenge = {0};
    enum onion_admit_result admit =
        onion_ratelimit_admit(method, path, body, body_len, &challenge);

    if (admit == ONION_ADMIT_RATE_LIMITED)
        return serve_rate_limited(response, response_max);
    if (admit == ONION_ADMIT_POW_REQUIRED)
        return serve_puzzle_required(&challenge, response, response_max);

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

    /* ZCODE Library — packages, publishers, rankings, badges, downloads.
     * Same handler is wired into the HTTPS dispatch chain, so the site
     * reads identically over onion and HTTPS (the same lib/vcs read
     * projections the zcode.* typed commands call — no second package
     * truth). Read-only: no POST surface here. */
    if (strncmp(path, "/zcode", 6) == 0 &&
        (path[6] == 0 || path[6] == '/' || path[6] == '?') &&
        onion_ctx()->datadir) {
        extern size_t zcode_site_handle_request(const char *, const char *,
            const uint8_t *, size_t, uint8_t *, size_t, const char *);
        return zcode_site_handle_request(method, path, body, body_len,
                                         response, response_max,
                                         onion_ctx()->datadir);
    }

    /* Blog MVC App. The same path handler is used by public HTTPS, so
     * zclnet.net/blog and a node's onion expose the same local projection. */
    if (strncmp(path, "/blog", 5) == 0 &&
        (path[5] == 0 || path[5] == '/' || path[5] == '?')) {
        extern size_t blog_site_handle_request(const char *, const char *,
            const uint8_t *, size_t, uint8_t *, size_t);
        size_t n = blog_site_handle_request(method, path, body, body_len,
                                            response, response_max);
        if (n > 0)
            return n;
        /* Fail closed: this signed MVC mount must never degrade into the
         * unrelated legacy static-file Blog when proof storage is absent. */
        return (size_t)snprintf((char *)response, response_max,
            "HTTP/1.1 503 Service Unavailable\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n\r\n"
            "Blog storage is unavailable.\n");
    }

    /* 404 */
    return (size_t)snprintf((char *)response, response_max,
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>404 Not Found</title>"
        "<style>%s</style></head><body>"
        "<div class='veil'>"
        "<h1><span style='color:var(--bad)'>404</span> Not Found</h1>"
        "<p>The page you requested does not exist.</p>"
        "<p><a href='/'>Home</a> &middot; <a href='/explorer'>Explorer</a> &middot; "
        "<a href='/store'>Store</a> &middot; <a href='/blog'>Blog</a></p>"
        "</div></body></html>",
        ONION_PAGE_CSS);
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
