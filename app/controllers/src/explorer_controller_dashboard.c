/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Block explorer DASHBOARD controller — thin parse-fetch-delegate glue
 * for the /explorer landing page. Supports two data sources: the
 * RPC-proxy mode (when no in-process chain is loaded) and the
 * native-chain mode. The controller fetches the four header stats and
 * the latest-block rows, packs them into the view structs, and delegates
 * the HTML/HTTP assembly to the dashboard view in
 *   app/views/src/explorer_dashboard_view.c */

#include "platform/time_compat.h"
#include "controllers/explorer_controller.h"
#include "chain/chain.h"
#include "chain/pow.h"
#include "core/uint256.h"
#include "primitives/block.h"
#include "jobs/reducer_frontier.h"
#include "validation/main_state.h"
#include "validation/txmempool.h"
#include <stdio.h>
#include <string.h>

#include "controllers/explorer_internal.h"
#include "views/explorer_dashboard_view.h"
#include "views/format_helpers.h"
#include "explorer_controller_internal.h"

/* ── Dashboard (RPC proxy mode) ───────────────────────────── */

static size_t serve_dashboard_rpc(uint8_t *r, size_t max)
{
    char buf[65536];

    /* Get blockchain info */
    rpc_call("getblockchaininfo", "[]", buf, sizeof(buf));
    int tip = (int)zcl_json_int(buf, "blocks");
    double diff = zcl_json_real(buf, "difficulty");

    /* Get mempool info */
    rpc_call("getmempoolinfo", "[]", buf, sizeof(buf));
    int64_t mp_count = zcl_json_int(buf, "size");
    int64_t mp_bytes = zcl_json_int(buf, "bytes");

    int show = 25;
    static struct explorer_dashboard_rpc_row rows[25];
    int n = 0;

    for (int h = tip; h > tip - show && h >= 0 && n < show; h--) {
        char params[64];
        snprintf(params, sizeof(params), "[%d]", h);
        rpc_call("getblockhash", params, buf, sizeof(buf));

        /* Extract hash from {"result":"<hash>",...} */
        char hash[65] = "";
        zcl_json_extract_str(buf, "result", hash, sizeof(hash));
        if (!hash[0]) continue;

        /* Get block details */
        char params2[128];
        snprintf(params2, sizeof(params2), "[\"%s\"]", hash);
        rpc_call("getblock", params2, buf, sizeof(buf));

        int64_t blk_time = zcl_json_int(buf, "time");
        int64_t ntx = zcl_json_int(buf, "tx");  /* this is actually array, use size */
        double blk_diff = zcl_json_real(buf, "difficulty");

        /* Count txs by counting "tx":[ array elements */
        int tx_count = explorer_count_json_tx_array(buf);
        (void)ntx;

        struct explorer_dashboard_rpc_row *row = &rows[n++];
        row->height = h;
        snprintf(row->hash, sizeof(row->hash), "%s", hash);
        snprintf(row->short_hash, sizeof(row->short_hash), "%.8s...%.4s", hash, hash + 60);
        format_time_ago(row->ago, sizeof(row->ago), (uint32_t)blk_time);
        format_time(row->ts, sizeof(row->ts), (uint32_t)blk_time);
        row->tx_count = tx_count;
        row->difficulty = blk_diff;
    }

    struct explorer_dashboard_rpc_view v = {
        .tip = tip,
        .difficulty = diff,
        .mempool_count = mp_count,
        .mempool_bytes = mp_bytes,
        .rows = rows,
        .row_count = n,
    };
    return explorer_dashboard_view_rpc(r, max, &v);
}

/* ── Dashboard (native chain mode) ───────────────────────── */

static size_t serve_dashboard_native_page(uint8_t *r, size_t max, int page)
{
    struct explorer_context *ctx = explorer_ctx();

    /* Externally-served explorer tip = the PROVABLE tip (H*). The block-list
     * walk below starts here and steps DOWN via active_chain_at, which is
     * always valid at/below H* (those blocks are in the window). */
    int tip = reducer_frontier_provable_tip_cached();
    const struct block_index *tip_bi =
        active_chain_at(&ctx->main_state->chain_active, tip);
    if (!tip_bi)
        tip_bi = active_chain_tip(&ctx->main_state->chain_active);

    size_t mp_count = ctx->mempool ? tx_mempool_size(ctx->mempool) : 0;
    uint64_t mp_bytes = ctx->mempool ? tx_mempool_total_size(ctx->mempool) : 0;

    int per_page = 25;
    if (page < 0) page = 0;
    int start_height = tip - page * per_page;
    int end_height = start_height - per_page + 1;
    if (end_height < 0) end_height = 0;

    static struct explorer_dashboard_native_row rows[25];
    int n = 0;

    for (int h = start_height; h >= end_height && h >= 0; h--) {
        const struct block_index *bi = active_chain_at(&ctx->main_state->chain_active, h);
        if (!bi) continue;

        struct explorer_dashboard_native_row *row = &rows[n++];
        row->height = h;
        char hash[65] = "";
        if (bi->phashBlock) uint256_get_hex(bi->phashBlock, hash);
        snprintf(row->hash, sizeof(row->hash), "%s", hash);
        format_time(row->ts, sizeof(row->ts), bi->nTime);
        snprintf(row->short_hash, sizeof(row->short_hash), "%.8s...%.4s", hash, hash + 60);
        row->sapling[0] = '\0';
        if (bi->nSaplingValue != 0)
            zcl_format_zcl(row->sapling, sizeof(row->sapling), bi->nSaplingValue);
        format_with_commas(row->h_fmt, sizeof(row->h_fmt), h);
        row->ntx = bi->nTx;
        row->difficulty = difficulty_from_index(bi);
    }

    struct explorer_dashboard_native_view v = {
        .tip = tip,
        .difficulty = difficulty_from_index(tip_bi),
        .mempool_count = mp_count,
        .mempool_bytes = mp_bytes,
        .rows = rows,
        .row_count = n,
        .page = page,
        .end_height = end_height,
    };
    return explorer_dashboard_view_native(r, max, &v);
}

/* ── Dashboard (SQLite-only, no RPC or main_state needed) ── */


size_t serve_dashboard_with_page(uint8_t *r, size_t max, int page)
{
    struct explorer_context *ctx = explorer_ctx();
    /* Use native if chain is loaded, otherwise fall back to RPC proxy */
    if (ctx->main_state && active_chain_height(&ctx->main_state->chain_active) > 0)
        return serve_dashboard_native_page(r, max, page);
    return serve_dashboard_rpc(r, max);
}

size_t explorer_serve_dashboard(uint8_t *r, size_t max)
{
    return serve_dashboard_with_page(r, max, 0);
}
