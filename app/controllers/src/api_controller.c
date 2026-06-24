/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * REST API controller — fast JSON API for the block explorer.
 * Serves /api routes. All RPC calls happen in a background thread;
 * HTTPS handler threads only serve from cache (api_rpc_call crashes
 * when called from HTTPS handler threads). */

#include "platform/time_compat.h"
#include "controllers/api_controller.h"
#include "controllers/explorer_internal.h"
#include "encoding/utilstrencodings.h"
#include "controllers/file_controller.h"
#include "net/snapshot_sync_contract.h"
#include "services/zslp_service.h"
#include "controllers/blockchain_controller.h"
#include "controllers/game_controller.h"
#include "controllers/name_controller.h"
#include "controllers/file_market_controller.h"
#include "controllers/swap_controller.h"
#include "controllers/messaging_controller.h"
#include "controllers/health_controller.h"
#include "services/node_health_service.h"
#include "chain/mmb.h"
#include "config/boot.h"
#include "config/runtime.h"
#include "views/format_helpers.h"
#include "views/explorer_factoids_view.h"
#include "event/event.h"
#include "net/download.h"
#include "validation/contextual_check_tx.h"
#include "validation/main_state.h"
#include "sapling/incremental_merkle_tree.h"
#include "keys/key_io.h"
#include "models/database.h"
#include "models/block.h"
#include "models/file_service.h"
#include "models/hodl_wave.h"
#include "models/onion_announcement.h"
#include "models/peer.h"
#include "models/zslp.h"
#include "json/json.h"
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdatomic.h>
#include <unistd.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <sqlite3.h>
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include "api_controller_internal.h"

/* ── State ────────────────────────────────────────────────── */

/* struct api_context defined in api_controller_internal.h */

/* struct api_rpc_backend defined in api_controller_internal.h */

struct api_context g_api_ctx = {0};
struct api_rpc_backend g_api_rpc = {
    .user = "zcluser",
    .pass = "zclpass",
    .port = 18232,
};

/* ── Background cache ─────────────────────────────────────── */

#define API_BLOCKS_CACHE_SIZE 131072   /* 128KB */
#define API_STATS_CACHE_SIZE  16384    /* 16KB  */
#define API_SUPPLY_CACHE_SIZE 4096     /* 4KB   */
#define API_HODL_CACHE_SIZE   8192     /* 8KB   */

static char   g_api_blocks_cache[API_BLOCKS_CACHE_SIZE];
static size_t g_api_blocks_cache_len = 0;

static char   g_api_stats_cache[API_STATS_CACHE_SIZE];
static size_t g_api_stats_cache_len = 0;

static char   g_api_supply_cache[API_SUPPLY_CACHE_SIZE];
static size_t g_api_supply_cache_len = 0;

static char   g_api_hodl_cache[API_HODL_CACHE_SIZE];
static size_t g_api_hodl_cache_len = 0;

#define API_DEEP_STATS_CACHE_SIZE 65536  /* 64KB */
static char   g_api_deep_stats_cache[API_DEEP_STATS_CACHE_SIZE];
static size_t g_api_deep_stats_cache_len = 0;

static _Atomic int g_api_cache_thread_running = 0;
static pthread_mutex_t g_api_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool api_response_cacheable(const uint8_t *buf, size_t len);

void api_set_state(struct main_state *ms, struct tx_mempool *mp,
                    struct coins_view_cache *coins_tip,
                    struct node_db *ndb, const char *datadir)
{
    g_api_ctx.main_state = ms;
    g_api_ctx.mempool = mp;
    g_api_ctx.coins_tip = coins_tip;
    g_api_ctx.node_db = ndb;
    g_api_ctx.datadir = datadir;
}

void api_set_rpc_backend(const char *rpc_user, const char *rpc_pass,
                          int rpc_port)
{
    if (rpc_user)
        snprintf(g_api_rpc.user, sizeof(g_api_rpc.user), "%s", rpc_user);
    if (rpc_pass)
        snprintf(g_api_rpc.pass, sizeof(g_api_rpc.pass), "%s", rpc_pass);
    if (rpc_port > 0)
        g_api_rpc.port = rpc_port;
}

/* ── RPC call to local zclassicd ─────────────────────────── */
/* ONLY called from the background cache thread, never from
 * HTTPS handler threads (socket ops crash in that context). */

int api_rpc_call(const char *method, const char *params_json,
                     char *out, size_t outmax)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) LOG_ERR("api", "api_rpc_call(%s): socket() failed", method);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)g_api_rpc.port);

    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        LOG_ERR("api", "api_rpc_call(%s): connect to port %d failed", method, g_api_rpc.port);
    }

    char body[4096];
    int blen = snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"%s\",\"params\":%s}",
        method, params_json);

    /* Base64 encode auth */
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char auth_plain[256];
    snprintf(auth_plain, sizeof(auth_plain), "%s:%s",
             g_api_rpc.user, g_api_rpc.pass);
    char auth_b64[512];
    size_t alen = strlen(auth_plain), bo = 0;
    for (size_t i = 0; i < alen; i += 3) {
        uint32_t n = ((uint32_t)(uint8_t)auth_plain[i]) << 16;
        if (i + 1 < alen) n |= ((uint32_t)(uint8_t)auth_plain[i+1]) << 8;
        if (i + 2 < alen) n |= (uint32_t)(uint8_t)auth_plain[i+2];
        auth_b64[bo++] = b64[(n >> 18) & 63];
        auth_b64[bo++] = b64[(n >> 12) & 63];
        auth_b64[bo++] = (i + 1 < alen) ? b64[(n >> 6) & 63] : '=';
        auth_b64[bo++] = (i + 2 < alen) ? b64[n & 63] : '=';
    }
    auth_b64[bo] = '\0';

    char req[8192];
    int rlen = snprintf(req, sizeof(req),
        "POST / HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: Basic %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n%s",
        auth_b64, blen, body);

    if (write(fd, req, (size_t)rlen) != rlen) { close(fd); LOG_ERR("api", "api_rpc_call(%s): write failed (len=%d)", method, rlen); }

    size_t total = 0;
    while (total < outmax - 1) {
        ssize_t r = read(fd, out + total, outmax - 1 - total);
        if (r <= 0) break;
        total += (size_t)r;
    }
    out[total] = '\0';
    close(fd);

    /* Skip HTTP headers */
    char *body_start = strstr(out, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        size_t body_len = total - (size_t)(body_start - out);
        memmove(out, body_start, body_len);
        out[body_len] = '\0';
        return (int)body_len;
    }
    return (int)total;
}

/* JSON extraction and validation: delegated to shared format_helpers.
 * Call sites use zcl_json_int/zcl_json_real (return-value wrappers with the
 * -1 int / 0.0 real defaults) and zcl_json_extract_str directly. */

/* Validate address/param is safe to embed in JSON (alphanumeric only).
 * Prevents JSON injection via crafted params. */
bool api_is_json_safe_param(const char *s, size_t maxlen)
{
    if (!s || !*s) return false;
    for (size_t i = 0; s[i] && i < maxlen; i++) {
        char c = s[i];
        if (!isalnum((unsigned char)c)) return false;
    }
    return true;
}

/* ── HTTP response helpers ───────────────────────────────── */

/* SECURITY_HEADERS + JSON_*_HEADERS macros live in
 * api_controller_internal.h so all api_controller_*.c siblings
 * share a single source of truth. */

static size_t cors_preflight(uint8_t *r, size_t max)
{
    return (size_t)snprintf((char *)r, max,
        "HTTP/1.1 204 No Content\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Access-Control-Max-Age: 86400\r\n"
        "Connection: close\r\n\r\n");
}

size_t api_json_error(uint8_t *r, size_t max, const char *headers,
                          const char *message)
{
    if (!r || max == 0)
        return 0;

    struct json_value body;
    json_init(&body);
    json_set_object(&body);
    json_push_kv_str(&body, "error", message ? message : "error");

    const char *h = headers ? headers : "";
    size_t hlen = strlen(h);
    size_t blen = json_write(&body, NULL, 0);
    size_t total = hlen + blen;

    if (hlen >= max) {
        memcpy(r, h, max - 1);
        r[max - 1] = '\0';
        json_free(&body);
        return total;
    }

    memcpy(r, h, hlen);
    json_write(&body, (char *)r + hlen, max - hlen);
    json_free(&body);
    return total;
}

static size_t api_json_ok(uint8_t *r, size_t max, const struct json_value *body)
{
    if (!r || max == 0 || !body)
        return 0;

    size_t blen = json_write(body, NULL, 0);
    char headers[256];
    int hn = snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Content-Length: %zu\r\n\r\n", blen);
    if (hn < 0 || (size_t)hn >= sizeof(headers))
        return 0;

    size_t hlen = (size_t)hn;
    if (blen > SIZE_MAX - hlen || hlen + blen > max) {
        LOG_WARN("api", "api_json_ok: response too large body=%zu max=%zu",
                 blen, max);
        return api_json_error(r, max, JSON_500_HEADERS,
                              "Response too large");
    }

    memcpy(r, headers, hlen);
    json_write(body, (char *)r + hlen, max - hlen);
    return hlen + blen;
}

/* ── Compute functions (called ONLY from background thread) ── */

static void *api_cache_refresh_thread(void *arg)
{
    (void)arg;

    /* Wait for RPC server to start */
    sleep(5);

    printf("API cache: background refresh thread started\n");
    fflush(stdout);

    int iteration = 0;
    while (g_api_cache_thread_running) {
        /* Refresh /api/blocks every 30 seconds */
        if (iteration % 3 == 0) {
            uint8_t *tmp = zcl_malloc(API_BLOCKS_CACHE_SIZE, "api_blocks_tmp");
            if (tmp) {
                size_t len = compute_blocks(tmp, API_BLOCKS_CACHE_SIZE);
                if (len > 0 && len < API_BLOCKS_CACHE_SIZE &&
                    api_response_cacheable(tmp, len)) {
                    pthread_mutex_lock(&g_api_cache_mutex);
                    memcpy(g_api_blocks_cache, tmp, len);
                    g_api_blocks_cache_len = len;
                    pthread_mutex_unlock(&g_api_cache_mutex);
                }
                free(tmp);
            }
        }

        /* Refresh /api/stats every 60 seconds */
        if (iteration % 6 == 0) {
            uint8_t *tmp = zcl_malloc(API_STATS_CACHE_SIZE, "api_stats_tmp");
            if (tmp) {
                size_t len = compute_stats(tmp, API_STATS_CACHE_SIZE);
                if (len > 0 && len < API_STATS_CACHE_SIZE &&
                    api_response_cacheable(tmp, len)) {
                    pthread_mutex_lock(&g_api_cache_mutex);
                    memcpy(g_api_stats_cache, tmp, len);
                    g_api_stats_cache_len = len;
                    pthread_mutex_unlock(&g_api_cache_mutex);
                }
                free(tmp);
            }
        }

        /* Refresh /api/supply every 60 seconds */
        if (iteration % 6 == 0) {
            uint8_t *tmp = zcl_malloc(API_SUPPLY_CACHE_SIZE, "api_supply_tmp");
            if (tmp) {
                size_t len = compute_supply(tmp, API_SUPPLY_CACHE_SIZE);
                if (len > 0 && len < API_SUPPLY_CACHE_SIZE &&
                    api_response_cacheable(tmp, len)) {
                    pthread_mutex_lock(&g_api_cache_mutex);
                    memcpy(g_api_supply_cache, tmp, len);
                    g_api_supply_cache_len = len;
                    pthread_mutex_unlock(&g_api_cache_mutex);
                }
                free(tmp);
            }
        }

        /* Refresh /api/hodl every 60 seconds */
        if (iteration % 6 == 0) {
            uint8_t *tmp = zcl_malloc(API_HODL_CACHE_SIZE, "api_hodl_tmp");
            if (tmp) {
                size_t len = compute_hodl(tmp, API_HODL_CACHE_SIZE);
                if (len > 0 && len < API_HODL_CACHE_SIZE &&
                    api_response_cacheable(tmp, len)) {
                    pthread_mutex_lock(&g_api_cache_mutex);
                    memcpy(g_api_hodl_cache, tmp, len);
                    g_api_hodl_cache_len = len;
                    pthread_mutex_unlock(&g_api_cache_mutex);
                }
                free(tmp);
            }
        }

        /* Refresh /api/stats/deep every 300 seconds (30 iterations) */
        if (iteration % 30 == 0) {
            uint8_t *tmp = zcl_malloc(API_DEEP_STATS_CACHE_SIZE, "api_deep_stats_tmp");
            if (tmp) {
                size_t len = compute_deep_stats(tmp, API_DEEP_STATS_CACHE_SIZE);
                if (len > 0 && len < API_DEEP_STATS_CACHE_SIZE &&
                    api_response_cacheable(tmp, len)) {
                    pthread_mutex_lock(&g_api_cache_mutex);
                    memcpy(g_api_deep_stats_cache, tmp, len);
                    g_api_deep_stats_cache_len = len;
                    pthread_mutex_unlock(&g_api_cache_mutex);
                }
                free(tmp);
            }
        }

        if (iteration == 0)
            printf("API cache: initial refresh complete (blocks=%zu stats=%zu supply=%zu hodl=%zu)\n",
                   g_api_blocks_cache_len, g_api_stats_cache_len,
                   g_api_supply_cache_len, g_api_hodl_cache_len);

        iteration++;
        /* Sleep 10 seconds between iterations; blocks refresh every 3rd (30s),
         * stats/supply/hodl every 6th (60s) */
        for (int s = 0; s < 10 && g_api_cache_thread_running; s++)
            sleep(1);
    }

    printf("API cache: background refresh thread stopped\n");
    fflush(stdout);
    return NULL;
}

bool api_start_detached_thread(pthread_t *thread_out,
                               void *(*entry)(void *),
                               void *arg)
{
    pthread_attr_t attr;
    bool ok = false;

    if (!thread_out || !entry)
        LOG_FAIL("api", "start_detached_thread: NULL thread_out or entry");
    if (pthread_attr_init(&attr) != 0)
        LOG_FAIL("api", "start_detached_thread: pthread_attr_init failed");
    if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0)
        goto cleanup;
    if (pthread_attr_setstacksize(&attr, 2 * 1024 * 1024) != 0)
        goto cleanup;
    /* raw-pthread-ok: detached-helper-wrapper (custom stack via pthread_attr) */
    if (pthread_create(thread_out, &attr, entry, arg) != 0)
        goto cleanup;
    ok = true;

cleanup:
    pthread_attr_destroy(&attr);
    return ok;
}

static bool ensure_cache_thread(void)
{
    int expected = 0;
    pthread_t t;

    if (!atomic_compare_exchange_strong(&g_api_cache_thread_running,
                                        &expected, 1))
        return expected == 1;
    if (!api_start_detached_thread(&t, api_cache_refresh_thread, NULL)) {
        atomic_store(&g_api_cache_thread_running, 0);
        LOG_FAIL("api", "ensure_cache_thread: failed to start cache refresh thread");
    }
    return true;
}

static bool api_response_cacheable(const uint8_t *buf, size_t len)
{
    if (!buf || len == 0)
        return false;
    const char *s = (const char *)buf;
    if (strstr(s, "HTTP/1.1 200 OK") != s)
        return false;
    if (strstr(s, "{\"error\":") || strstr(s, "\"error\":\""))
        return false;
    return true;
}

void api_start_cache(void)
{
    if (!ensure_cache_thread())
        LOG_WARN("controller", "API cache: failed to start background thread");
}

void api_stop_cache(void)
{
    atomic_store(&g_api_cache_thread_running, 0);
}

/* ── Serve from cache helpers ────────────────────────────── */

/* `cache_len` is read under g_api_cache_mutex in the same hold as the
 * memcpy — the refresh thread updates buffer+len together under the
 * mutex, so a by-value len read outside the lock can be stale (a
 * larger old len against a newer shorter buffer serves garbage). */
static size_t serve_from_cache(const char *cache, const size_t *cache_len,
                                uint8_t *r, size_t max)
{
    pthread_mutex_lock(&g_api_cache_mutex);
    size_t len = *cache_len;
    if (len == 0) {
        pthread_mutex_unlock(&g_api_cache_mutex);
        return api_json_error(r, max, JSON_503_HEADERS,
                          "Data loading, please retry in a few seconds");
    }
    size_t copy = len < max ? len : max;
    memcpy(r, cache, copy);
    pthread_mutex_unlock(&g_api_cache_mutex);
    return copy;
}

/* ── Operator-private route classifier ───────────────────── */

/* True when `path` begins with `prefix` at a path boundary — the
 * char after the prefix must be '\0', '/' or '?' so e.g.
 * "/api/swaps" never captures the public "/api/swap_chains". */
static bool api_path_prefix_boundary(const char *path, const char *prefix)
{
    size_t n = strlen(prefix);
    if (strncmp(path, prefix, n) != 0)
        return false;
    return path[n] == '\0' || path[n] == '/' || path[n] == '?';
}

bool api_route_is_operator_private(const char *path)
{
    if (!path)
        return false;
    return api_path_prefix_boundary(path, "/api/wallet") ||
           api_path_prefix_boundary(path, "/api/messages") ||
           api_path_prefix_boundary(path, "/api/swaps");
}

/* ── Main router ─────────────────────────────────────────── */
/* IMPORTANT: This function is called from HTTPS handler threads.
 * It must NEVER call api_rpc_call directly. All data comes from
 * background caches or the lookup thread.
 *
 * SECURITY INVARIANT: this router never sees HTTP headers or peer
 * identity, so it cannot authenticate anyone. ANY transport that
 * forwards /api traffic here (the clearnet TLS listener, any future
 * ingress) MUST apply the operator-private gate
 * (api_route_is_operator_private) listener-side BEFORE dispatching.
 * The onion service exposes no /api; wallet_gui/zcl-browser call
 * in-process and are trusted. */

size_t api_handle_request(const char *method, const char *path,
                           const uint8_t *body, size_t body_len,
                           uint8_t *response, size_t response_max)
{
    (void)body; (void)body_len;
    if (!method || !path || !response || response_max == 0) return 0;

    /* Start background cache thread on first request */
    if (!ensure_cache_thread()) {
        return api_json_error(response, response_max, JSON_503_HEADERS,
                          "API cache unavailable, please retry");
    }

    /* Handle CORS preflight */
    if (strcmp(method, "OPTIONS") == 0)
        return cors_preflight(response, response_max);

    /* Only GET requests */
    if (strcmp(method, "GET") != 0)
        return api_json_error(response, response_max,
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Content-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n\r\n",
            "Method not allowed");

    /* Strip trailing slash */
    char clean_path[512];
    snprintf(clean_path, sizeof(clean_path), "%s", path);
    size_t plen = strlen(clean_path);
    if (plen > 1 && clean_path[plen - 1] == '/')
        clean_path[plen - 1] = '\0';

    /* Route: /api/blocks — served from cache */
    if (strcmp(clean_path, "/api/blocks") == 0)
        return serve_from_cache(g_api_blocks_cache, &g_api_blocks_cache_len,
                                response, response_max);

    /* Route: /api/block/:id — served via lookup thread */
    if (strncmp(clean_path, "/api/block/", 11) == 0 && clean_path[11])
        return do_lookup(LOOKUP_BLOCK, clean_path + 11, response, response_max);

    /* Route: /api/tx/:txid — served via lookup thread */
    if (strncmp(clean_path, "/api/tx/", 8) == 0 && clean_path[8])
        return do_lookup(LOOKUP_TX, clean_path + 8, response, response_max);

    /* Route: /api/address/:addr — served via lookup thread */
    if (strncmp(clean_path, "/api/address/", 13) == 0 && clean_path[13])
        return do_lookup(LOOKUP_ADDRESS, clean_path + 13, response, response_max);

    /* Route: /api/zslp/tokens — resource collection */
    if (strcmp(clean_path, "/api/zslp/tokens") == 0 ||
        strncmp(clean_path, "/api/zslp/tokens?", 17) == 0)
        return api_serve_zslp_tokens(path, response, response_max);

    /* Route: /api/zslp/tokens/:id/transfers — member subresource */
    if (strncmp(clean_path, "/api/zslp/tokens/", 17) == 0 && clean_path[17]) {
        const char *token_id = clean_path + 17;
        const char *suffix = strstr(token_id, "/transfers");
        if (suffix &&
            (strcmp(suffix, "/transfers") == 0 ||
             strncmp(suffix, "/transfers?", 11) == 0)) {
            char token_buf[ZSLP_TOKEN_KEY_MAX + 1];
            size_t token_len = (size_t)(suffix - token_id);
            if (token_len == 0 || token_len > ZSLP_TOKEN_KEY_MAX)
                return api_json_error(response, response_max, JSON_404_HEADERS,
                                  "Invalid token id");
            memcpy(token_buf, token_id, token_len);
            token_buf[token_len] = '\0';
            return api_serve_zslp_token_transfers(path, token_buf,
                                                  response, response_max);
        }
        return api_serve_zslp_token(token_id, response, response_max);
    }

    /* Route: /api/onion/announcements — resource collection */
    if (strcmp(clean_path, "/api/onion/announcements") == 0 ||
        strncmp(clean_path, "/api/onion/announcements?", 25) == 0)
        return api_serve_onion_announcements(path, response, response_max);

    /* Route: /api/file-services — resource collection */
    if (strcmp(clean_path, "/api/file-services") == 0 ||
        strncmp(clean_path, "/api/file-services?", 19) == 0)
        return api_serve_file_services(path, response, response_max);

    /* Route: /api/peers — resource collection */
    if (strcmp(clean_path, "/api/peers") == 0 ||
        strncmp(clean_path, "/api/peers?", 11) == 0)
        return api_serve_peers(path, response, response_max);

    /* Route: /api/stats — served from cache */
    if (strcmp(clean_path, "/api/stats") == 0)
        return serve_from_cache(g_api_stats_cache, &g_api_stats_cache_len,
                                response, response_max);

    /* Route: /api/stats/deep — deep stats served from cache */
    if (strcmp(clean_path, "/api/stats/deep") == 0)
        return serve_from_cache(g_api_deep_stats_cache, &g_api_deep_stats_cache_len,
                                response, response_max);

    /* Route: /api/supply — served from cache */
    if (strcmp(clean_path, "/api/supply") == 0)
        return serve_from_cache(g_api_supply_cache, &g_api_supply_cache_len,
                                response, response_max);

    /* Route: /api/hodl — served from cache */
    if (strcmp(clean_path, "/api/hodl") == 0)
        return serve_from_cache(g_api_hodl_cache, &g_api_hodl_cache_len,
                                response, response_max);

    /* Route: /api/factoids — built from SQLite (read-only, safe from handler) */
    if (strcmp(clean_path, "/api/factoids") == 0) {
        if (!g_api_ctx.datadir)
            return api_json_error(response, response_max, JSON_500_HEADERS, "No datadir");
        return explorer_factoids_build_json(response, response_max, g_api_ctx.datadir);
    }

    /* Event log — lock-free atomic reads, safe from any handler thread */
    if (strncmp(clean_path, "/api/events", 11) == 0 &&
        (clean_path[11] == '\0' || clean_path[11] == '?'))
        return api_serve_events(path, response, response_max);

    /* Sync state — minimal monitoring endpoint */
    if (strcmp(clean_path, "/api/syncstate") == 0)
        return api_serve_syncstate(response, response_max);

    /* Download stats — IBD progress monitoring */
    if (strcmp(clean_path, "/api/downloadstats") == 0)
        return api_serve_downloadstats(response, response_max);

    /* Health check — lightweight, machine-readable */
    if (strcmp(clean_path, "/api/health") == 0)
        return api_serve_health(response, response_max);

    /* Route: /api/node/snapshot — snapshot sync service status */
    if (strcmp(clean_path, "/api/node/snapshot") == 0)
        return api_serve_node_snapshot(response, response_max);

    /* Route: /api/node/mmb — Merkle Mountain Belt status */
    if (strcmp(clean_path, "/api/node/mmb") == 0)
        return api_serve_node_mmb(response, response_max);

    /* Route: /api/node/status — comprehensive one-stop diagnostics */
    if (strcmp(clean_path, "/api/node/status") == 0)
        return api_serve_node_status(response, response_max);

    /* Wallet data — balance, address, activity */
    if (strcmp(clean_path, "/api/wallet") == 0)
        return api_serve_wallet(response, response_max);

    /* ── File Transfer Service — SHA3-verified chunks ──────────── */

    /* GET /api/files/manifest — JSON manifest of all chunks */
    if (strcmp(clean_path, "/api/files/manifest") == 0)
        return api_serve_files_manifest(response, response_max);

    /* GET /api/files/:sha3hash — raw chunk bytes by SHA3 hash */
    if (strncmp(clean_path, "/api/files/", 11) == 0 &&
        strlen(clean_path + 11) == 64)
        return api_serve_file_chunk(clean_path + 11, response, response_max);

    /* ── P2P Services Platform REST API ─────────────────────── */

    /* Helper: call an API function and return its JSON as HTTP response */
    #define API_JSON_ROUTE(path_str, api_fn) \
    if (strcmp(clean_path, path_str) == 0) { \
        struct json_value jr = {0}; \
        if (api_fn(&jr)) { \
            size_t n = api_json_ok(response, response_max, &jr); \
            json_free(&jr); \
            return n; \
        } \
        json_free(&jr); \
        return api_json_error(response, response_max, JSON_500_HEADERS, \
                          "Internal error"); \
    }

    API_JSON_ROUTE("/api/sync/detail", api_getsyncdetail)
    API_JSON_ROUTE("/api/services",    api_getservicehealth)
    API_JSON_ROUTE("/api/latency",     api_getpeerlatency)
    API_JSON_ROUTE("/api/games",       api_gametypes)
    API_JSON_ROUTE("/api/names",       api_name_list)

    /* Route: /api/name/:name — resolve single name */
    if (strncmp(clean_path, "/api/name/", 10) == 0 && clean_path[10]) {
        extern bool rpc_name_resolve_api(const char *name, struct json_value *result);
        struct json_value jr = {0};
        if (rpc_name_resolve_api(clean_path + 10, &jr)) {
            size_t n = api_json_ok(response, response_max, &jr);
            json_free(&jr);
            return n;
        }
        json_free(&jr);
        return api_json_error(response, response_max, JSON_404_HEADERS, "Name not found");
    }
    API_JSON_ROUTE("/api/market",      api_market_list)
    API_JSON_ROUTE("/api/swaps",       api_swap_list)
    API_JSON_ROUTE("/api/swap_chains", api_swap_chains)
    API_JSON_ROUTE("/api/messages",    api_msg_inbox)

    #undef API_JSON_ROUTE

    return api_json_error(response, response_max, JSON_404_HEADERS,
                      "Unknown API endpoint");
}
