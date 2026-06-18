/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Explorer secondary-page CONTROLLER: thin parse-delegate glue for
 * stats, tokens, factoids, hodl, events, names, market, swaps, messages,
 * css. The page assembly (HTML/SVG/JSON) lives in the view shape:
 *   app/views/src/explorer_stats_view.c
 *   app/views/src/explorer_factoids_view.c
 *   app/views/src/explorer_pages_view.c
 * This file retains only the request dispatch, the in-RAM page cache, and the
 * background compute-thread orchestration used by the prewarm pipeline.
 * See explorer_controller_internal.h for shared declarations. */

#include "platform/time_compat.h"
#include "controllers/explorer_controller.h"
#include "controllers/explorer_internal.h"
#include "explorer_controller_internal.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/subsidy.h"
#include "core/uint256.h"
#include "encoding/utilstrencodings.h"
#include "models/database.h"
#include "models/hodl_wave.h"
#include "models/tx_index.h"
#include "models/utxo.h"
#include "services/hodl_history_service.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/template.h"
#include "validation/main_state.h"
#include "validation/txmempool.h"
#include "views/format_helpers.h"
#include "views/explorer_stats_view.h"
#include "views/explorer_factoids_view.h"
#include "views/explorer_pages_view.h"
#include "views/explorer_pages_loading_view.h"
#include "views/wallet_templates_gen.h"
#include "zslp/slp.h"

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Once-flags driving compute-thread spawning. Referenced from
 * explorer_controller.c's prewarm pipeline via the extern declarations
 * in explorer_controller_internal.h. */
_Atomic int g_stats_computing = 0;
_Atomic int g_tokens_computing = 0;
_Atomic int g_factoids_computing = 0;

/* Stats page — computed in background thread, served from cache */
#define STATS_CACHE_SIZE (1024 * 1024) /* 1MB for comprehensive stats */
static char g_stats_cache[STATS_CACHE_SIZE] = "";
static size_t g_stats_cache_len = 0;

/* ── Disk cache helpers (survive restarts) ────────────────── */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

void cache_save(const char *name, const char *data, size_t len)
{
    struct explorer_assets *assets = explorer_assets();
    if (!assets->explorer_dir[0]) ensure_explorer_dir();
    if (!assets->explorer_dir[0] || len == 0) return;
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s.cache", assets->explorer_dir, name);
    FILE *f = fopen(path, "w");
    if (f) { fwrite(data, 1, len, f); fclose(f); }
}

size_t cache_load(const char *name, char *buf, size_t max)
{
    struct explorer_assets *assets = explorer_assets();
    if (!assets->explorer_dir[0]) ensure_explorer_dir();
    if (!assets->explorer_dir[0]) return 0;
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s.cache", assets->explorer_dir, name);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t len = fread(buf, 1, max - 1, f);
    fclose(f);
    buf[len] = '\0';
    return len;
}

#pragma GCC diagnostic pop

/* serve_loading_placeholder() and the page renderers (tokens, token detail,
 * hodl, events, names, market, swaps, messages) live in
 * app/views/src/explorer_pages_view.c. The controller parses and delegates;
 * views render. */

void *stats_compute_thread(void *arg)
{
    (void)arg;
    struct explorer_context *ctx = explorer_ctx();
    /* Load previous cache from disk for instant serving while recomputing */
    if (g_stats_cache_len == 0) {
        size_t disk_len = cache_load("stats", g_stats_cache, STATS_CACHE_SIZE);
        if (disk_len > 0) {
            g_stats_cache_len = disk_len;
            printf("Stats: loaded %zu bytes from disk cache (instant)\n", disk_len);
            fflush(stdout);
        }
    }
    printf("Stats background: computing comprehensive stats...\n");
    fflush(stdout);
    /* Compute fresh into a temp buffer so we don't blank the disk-loaded cache */
    char *tmp = zcl_malloc(STATS_CACHE_SIZE, "stats_cache_tmp");
    if (!tmp) { g_stats_computing = 0; LOG_NULL("explorer", "stats_compute_thread: malloc(%d) failed", STATS_CACHE_SIZE); }
    size_t len = explorer_stats_build((uint8_t *)tmp, STATS_CACHE_SIZE, ctx->datadir);
    if (len > 0) {
        memcpy(g_stats_cache, tmp, len);
        g_stats_cache_len = len;
        cache_save("stats", g_stats_cache, len);
    }
    free(tmp);
    g_stats_computing = 0;
    return NULL;
}

/* (stats_query_int64, stats_query_double, stats_tab_css, and stats body
 * moved to explorer_stats.c — see explorer_stats_build()) */

size_t serve_stats(uint8_t *r, size_t max)
{
    /* Return cached version if available */
    if (g_stats_cache_len > 0) {
        size_t copy = g_stats_cache_len < max ? g_stats_cache_len : max;
        memcpy(r, g_stats_cache, copy);
        return copy;
    }

    /* Not cached yet — trigger background computation if not running */
    explorer_start_once(&g_stats_computing, stats_compute_thread,
                        "stats_compute");
    return explorer_view_loading_placeholder(r, max, "Loading Statistics...",
        "#33ff99", "Computing charts from blockchain data.");
}

/* ── Factoids Page ────────────────────────────────────────── */

#define FACTOIDS_CACHE_SIZE (1024 * 1024)  /* 1MB — 17 sections with SHA3 */
static char g_factoids_cache[FACTOIDS_CACHE_SIZE] = "";
static size_t g_factoids_cache_len = 0;

void *factoids_compute_thread(void *arg)
{
    (void)arg;
    struct explorer_context *ctx = explorer_ctx();
    /* Load previous cache from disk for instant serving */
    if (g_factoids_cache_len == 0) {
        size_t disk_len = cache_load("factoids", g_factoids_cache, FACTOIDS_CACHE_SIZE);
        if (disk_len > 0) {
            g_factoids_cache_len = disk_len;
            printf("Factoids: loaded %zu bytes from disk cache (instant)\n", disk_len);
            fflush(stdout);
        }
    }
    printf("Factoids background: computing historian data...\n");
    fflush(stdout);
    size_t len = explorer_factoids_build((uint8_t *)g_factoids_cache,
                                          FACTOIDS_CACHE_SIZE, ctx->datadir);
    if (len > 0) {
        g_factoids_cache_len = len;
        cache_save("factoids", g_factoids_cache, len);
    }
    g_factoids_computing = 0;
    return NULL;
}

size_t serve_factoids(uint8_t *r, size_t max)
{
    /* Return cached version if available */
    if (g_factoids_cache_len > 0) {
        size_t copy = g_factoids_cache_len < max ? g_factoids_cache_len : max;
        memcpy(r, g_factoids_cache, copy);
        return copy;
    }

    /* Not cached yet -- trigger background computation */
    explorer_start_once(&g_factoids_computing, factoids_compute_thread,
                        "factoids_compute");
    return explorer_view_loading_placeholder(r, max, "Loading Factoids...",
        "#33ff99", "Computing historian data from blockchain.");
}

/* ── ZSLP Tokens Page ─────────────────────────────────────── */

/* Tokens page cache — precomputed in background */
static char g_tokens_cache[131072] = "";
static size_t g_tokens_cache_len = 0;

void *tokens_compute_thread(void *arg)
{
    (void)arg;
    struct explorer_context *ctx = explorer_ctx();
    printf("Tokens background: computing...\n");
    fflush(stdout);

    uint8_t *r = zcl_malloc(131072, "tokens_html_buf");
    if (!r) {
        g_tokens_computing = 0;
        LOG_NULL("explorer", "tokens_compute_thread: malloc(131072) failed");
    }

    /* Delegate page assembly to the view (app/views). */
    size_t off = explorer_view_tokens(ctx->datadir, r, 131072);

    /* Cache result */
    if (off > 0 && off < sizeof(g_tokens_cache)) {
        memcpy(g_tokens_cache, r, off);
        g_tokens_cache_len = off;
    }
    free(r);
    g_tokens_computing = 0;
    printf("Tokens background: cached %zu bytes\n", g_tokens_cache_len);
    fflush(stdout);
    return NULL;
}

size_t serve_tokens(uint8_t *r, size_t max)
{
    if (g_tokens_cache_len > 0) {
        size_t copy = g_tokens_cache_len < max ? g_tokens_cache_len : max;
        memcpy(r, g_tokens_cache, copy);
        return copy;
    }
    /* Trigger the background compute. Call explorer_start_once directly —
     * do NOT pre-set g_tokens_computing=1 first, or its compare_exchange(0→1)
     * fails and the thread never starts, wedging the page on "Loading" forever
     * (a request landing inside the boot prewarm window used to trip this). */
    explorer_start_once(&g_tokens_computing, tokens_compute_thread,
                        "tokens_compute");
    return explorer_view_tokens_loading(r, max);
}

/* ── ZSLP Token Detail Page ────────────────────────────────── */

size_t serve_token_detail(const char *token_id_hex, uint8_t *r, size_t max)
{
    struct explorer_context *ctx = explorer_ctx();
    return explorer_view_token_detail(token_id_hex, ctx->datadir, r, max);
}

/* ── HODL Wave Page ────────────────────────────────────────── */

size_t serve_hodl(uint8_t *r, size_t max)
{
    struct explorer_context *ctx = explorer_ctx();
    return explorer_view_hodl(ctx->datadir, r, max);
}


/* ── CSS Stylesheet ───────────────────────────────────────── */

size_t serve_css(uint8_t *r, size_t max)
{
    struct explorer_assets *assets = explorer_assets();
    /* Reload CSS from disk each time (allows live editing) */
    load_css();
    size_t off = 0;
    int n = snprintf((char *)r, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/css; charset=utf-8\r\n"
        "Cache-Control: public, max-age=60\r\n"
        "Connection: close\r\n\r\n");
    if (n > 0) off = (size_t)n;
    if (off + assets->css_len < max) {
        memcpy(r + off, assets->css_cache, assets->css_len);
        off += assets->css_len;
    }
    return off;
}

/* ── Event Log Page ───────────────────────────────────────── */

size_t serve_events(uint8_t *r, size_t max)
{
    return explorer_view_events(r, max);
}

/* ── Names Page ──────────────────────────────────────────── */

size_t serve_names(uint8_t *r, size_t max)
{
    return explorer_view_names(r, max);
}

/* ── Market Page ─────────────────────────────────────────── */

size_t serve_market(uint8_t *r, size_t max)
{
    return explorer_view_market(r, max);
}

/* ── Swaps Page ──────────────────────────────────────────── */

size_t serve_swaps(uint8_t *r, size_t max)
{
    return explorer_view_swaps(r, max);
}

/* ── Messages Page ───────────────────────────────────────── */

size_t serve_messages(uint8_t *r, size_t max)
{
    return explorer_view_messages(r, max);
}
