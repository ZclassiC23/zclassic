/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Explorer dashboard VIEW: the /explorer landing page. Renders the
 * four-stat header row and the "Latest Blocks" table in two modes (RPC-proxy
 * and native-chain). The controller parses the request, fetches the data,
 * packs it into the view structs, and delegates the entire HTML/HTTP assembly
 * here. The per-row buffer-bound truncation guards live inside the emit loops. */

#include "platform/time_compat.h"
#include "views/explorer_dashboard_view.h"
#include "controllers/explorer_internal.h"
#include "views/format_helpers.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── Dashboard (RPC proxy mode) ───────────────────────────── */

size_t explorer_dashboard_view_rpc(uint8_t *r, size_t max,
                                   const struct explorer_dashboard_rpc_view *v)
{
    size_t off = 0;

    APPEND(off, r, max, EXPLORER_HEADER("Dashboard"));
    off += explorer_emit_nav((char *)r + off, max - off, "blocks");

    char ht_fmt[32];
    format_with_commas(ht_fmt, sizeof(ht_fmt), v->tip);
    APPEND(off, r, max,
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%s</div><div class='lbl'>Block Height</div></div>"
        "<div class='stat'><div class='num'>%.2f</div><div class='lbl'>Difficulty</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div><div class='lbl'>Mempool Txs</div></div>"
        "<div class='stat'><div class='num'>%.1f KB</div><div class='lbl'>Mempool Size</div></div>"
        "</div>",
        ht_fmt, v->difficulty, v->mempool_count, (double)v->mempool_bytes / 1024.0);

    /* Latest blocks */
    APPEND(off, r, max,
        "<h2>Latest Blocks</h2>"
        "<table><tr><th>Height</th><th>Hash</th><th>Time</th>"
        "<th>Txs</th><th>Difficulty</th></tr>");

    for (int i = 0; i < v->row_count && off + 600 < max; i++) {
        const struct explorer_dashboard_rpc_row *row = &v->rows[i];
        APPEND(off, r, max,
            "<tr><td><a href='/explorer/block/%d'><b>%d</b></a></td>"
            "<td class='hash'><a href='/explorer/block/%s'>%s</a></td>"
            "<td>%s<br><small style='color:#666'>%s</small></td>"
            "<td>%d</td><td>%.2f</td></tr>",
            row->height, row->height, row->hash, row->short_hash,
            row->ago, row->ts, row->tx_count, row->difficulty);
    }

    APPEND(off, r, max, "</table>" EXPLORER_FOOTER);
    return off;
}

/* ── Dashboard (native chain mode) ───────────────────────── */

size_t explorer_dashboard_view_native(uint8_t *r, size_t max,
                                      const struct explorer_dashboard_native_view *v)
{
    size_t off = 0;

    APPEND(off, r, max, EXPLORER_HEADER("Dashboard"));
    off += explorer_emit_nav((char *)r + off, max - off, "blocks");

    char ht_fmt[32];
    format_with_commas(ht_fmt, sizeof(ht_fmt), v->tip);
    APPEND(off, r, max,
        "<div class='stats-row'>"
        "<div class='stat'><div class='num'>%s</div><div class='lbl'>Block Height</div></div>"
        "<div class='stat'><div class='num'>%.2f</div><div class='lbl'>Difficulty</div></div>"
        "<div class='stat'><div class='num'>%zu</div><div class='lbl'>Mempool Txs</div></div>"
        "<div class='stat'><div class='num'>%.1f KB</div><div class='lbl'>Mempool Size</div></div>"
        "</div>",
        ht_fmt, v->difficulty, v->mempool_count, (double)v->mempool_bytes / 1024.0);

    APPEND(off, r, max,
        "<h2>Latest Blocks</h2>"
        "<table><tr><th>Height</th><th>Hash</th><th>Time</th>"
        "<th>Txs</th><th>Difficulty</th><th>Shielded</th></tr>");

    for (int i = 0; i < v->row_count; i++) {
        const struct explorer_dashboard_native_row *row = &v->rows[i];
        APPEND(off, r, max,
            "<tr><td><a href='/explorer/block/%d'>%s</a></td>"
            "<td class='hash'><a href='/explorer/block/%s'>%s</a></td>"
            "<td>%s</td><td>%u</td><td>%.4f</td><td class='amount'>%s</td></tr>",
            row->height, row->h_fmt, row->hash, row->short_hash,
            row->ts, row->ntx, row->difficulty, row->sapling);

        if (off + 512 >= max) break;
    }

    APPEND(off, r, max, "</table>");

    /* Pagination */
    APPEND(off, r, max, "<div class='pager'>");
    if (v->page > 0)
        APPEND(off, r, max, "<a href='/explorer?page=%d'>&larr; Newer</a>", v->page - 1);
    if (v->end_height > 0)
        APPEND(off, r, max, "<a href='/explorer?page=%d'>Older &rarr;</a>", v->page + 1);
    APPEND(off, r, max, "</div>");

    APPEND(off, r, max, EXPLORER_FOOTER);
    return off;
}
