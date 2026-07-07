/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Explorer HODL-wave VIEW.
 *
 * Part of the explorer secondary-pages split: this TU owns the HODL-wave
 * page — the historical snapshot chart assembly and explorer_view_hodl.
 * The other secondary pages (tokens,
 * token detail, event log, names, market, swaps, messages, loading
 * placeholder) live in explorer_pages_view.c. explorer_view_hodl is declared
 * in views/explorer_pages_view.h. */

#include "platform/time_compat.h"
#include "views/explorer_pages_view.h"
#include "controllers/explorer_internal.h"
#include "jobs/reducer_frontier.h"
#include "models/hodl_wave.h"
#include "services/hodl_history_service.h"
#include "util/safe_alloc.h"
#include "views/format_helpers.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── HODL Wave Page ────────────────────────────────────────── */

static int64_t hodl_view_cap_to_served_tip(int64_t index_tip)
{
    if (index_tip < 0)
        return index_tip;
    if (!reducer_frontier_provable_tip_is_published())
        return 0;

    int32_t served_tip = reducer_frontier_provable_tip_cached();
    if (served_tip >= 0 && index_tip > served_tip)
        return served_tip;
    return index_tip;
}

/* Per-sample chart row loaded from hodl_history, plus a live tip anchor. */
struct hodl_chart_row {
    int64_t height;
    int64_t time;
    int64_t total_zat;
    int64_t older_1y_zat;
    double  older_1y_pct;
};

#define HODL_VIEW_CACHE_DATADIR_MAX 1024
#define HODL_VIEW_CACHE_HASH_MAX 80

struct hodl_view_cache_entry {
    bool valid;
    char datadir[HODL_VIEW_CACHE_DATADIR_MAX];
    int64_t tip_height;
    char tip_hash[HODL_VIEW_CACHE_HASH_MAX];
    struct hodl_wave_snapshot snapshot;
};

static pthread_mutex_t g_hodl_view_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static struct hodl_view_cache_entry g_hodl_view_cache;

static bool hodl_view_datadir_key(const char *datadir, char out[HODL_VIEW_CACHE_DATADIR_MAX])
{
    if (!datadir)
        return false;
    int n = snprintf(out, HODL_VIEW_CACHE_DATADIR_MAX, "%s", datadir);
    return n >= 0 && n < HODL_VIEW_CACHE_DATADIR_MAX;
}

static void hodl_view_tip_hash(sqlite3 *db, int64_t tip, char out[HODL_VIEW_CACHE_HASH_MAX])
{
    char sql[128];
    out[0] = '\0';
    if (db && tip >= 0) {
        snprintf(sql, sizeof(sql),
                 "SELECT hex(hash) FROM blocks WHERE height=%" PRId64, tip);
        (void)sql_query_text(db, sql, out, HODL_VIEW_CACHE_HASH_MAX);
    }
    if (out[0] == '\0')
        snprintf(out, HODL_VIEW_CACHE_HASH_MAX, "height:%" PRId64 ":nohash", tip);
}

static bool hodl_view_cache_get(const char *datadir_key, int64_t tip,
                                const char *tip_hash,
                                struct hodl_wave_snapshot *out)
{
    bool hit = false;
    pthread_mutex_lock(&g_hodl_view_cache_lock);
    if (g_hodl_view_cache.valid &&
        g_hodl_view_cache.tip_height == tip &&
        strcmp(g_hodl_view_cache.datadir, datadir_key) == 0 &&
        strcmp(g_hodl_view_cache.tip_hash, tip_hash) == 0) {
        *out = g_hodl_view_cache.snapshot;
        hit = true;
    }
    pthread_mutex_unlock(&g_hodl_view_cache_lock);
    return hit;
}

static void hodl_view_cache_put(const char *datadir_key, int64_t tip,
                                const char *tip_hash,
                                const struct hodl_wave_snapshot *snapshot)
{
    pthread_mutex_lock(&g_hodl_view_cache_lock);
    snprintf(g_hodl_view_cache.datadir, sizeof(g_hodl_view_cache.datadir),
             "%s", datadir_key);
    snprintf(g_hodl_view_cache.tip_hash, sizeof(g_hodl_view_cache.tip_hash),
             "%s", tip_hash);
    g_hodl_view_cache.tip_height = tip;
    g_hodl_view_cache.snapshot = *snapshot;
    g_hodl_view_cache.valid = true;
    pthread_mutex_unlock(&g_hodl_view_cache_lock);
}

static bool hodl_chart_rows_have_gap(const struct hodl_chart_row *prev,
                                     const struct hodl_chart_row *next)
{
    if (!prev || !next || next->height <= prev->height)
        return false;
    return next->height - prev->height >
           HODL_HISTORY_SAMPLE_STRIDE + (HODL_HISTORY_SAMPLE_STRIDE / 2);
}

static int hodl_chart_x(const struct hodl_chart_row *row,
                        int pl, int pw, int64_t t_min, int64_t t_max)
{
    if (!row || t_max <= t_min)
        return pl;
    return pl + (int)((double)(row->time - t_min) /
                      (double)(t_max - t_min) * pw);
}

static int hodl_chart_y(const struct hodl_chart_row *row,
                        int pt, int ph, double y_min, double y_max)
{
    if (!row || y_max <= y_min)
        return pt + ph;
    return pt + ph - (int)((row->older_1y_pct - y_min) /
                           (y_max - y_min) * ph);
}

size_t explorer_view_hodl(const char *datadir, uint8_t *r, size_t max)
{
    sqlite3 *db = NULL;
    size_t off = 0;

    if (!datadir || !explorer_open_readonly_db(datadir, &db)) {
        APPEND(off, r, max, EXPLORER_HEADER("HODL Wave"));
        off += explorer_emit_nav((char *)r + off, max - off, "hodl");
        APPEND(off, r, max,
            "<div style='max-width:900px;margin:40px auto;color:#ccc'>"
            "<h1>HODL Wave</h1>"
            "<p>Database unavailable. This page will not publish cached HODL data.</p>"
            "</div>" EXPLORER_FOOTER);
        return off;
    }

    /* HODL is public chain data, so publish it at the same H* frontier served
     * by getblockcount/getblockhash. The projection DB can be one reducer
     * stage ahead during a tip-finalize hole; those rows are skipped until H*
     * advances instead of being advertised as the live explorer tip. */
    int64_t block_tip =
        sql_query_i64(db, "SELECT COALESCE(MAX(height),0) FROM blocks");
    int64_t utxo_tip = sql_query_i64(db, "SELECT COALESCE(MAX(height),0) FROM utxos");
    int64_t index_tip = block_tip > utxo_tip ? block_tip : utxo_tip;
    int64_t tip = hodl_view_cap_to_served_tip(index_tip);

    struct hodl_wave_snapshot hodl;
    char datadir_key[HODL_VIEW_CACHE_DATADIR_MAX];
    char tip_hash[HODL_VIEW_CACHE_HASH_MAX];
    bool cacheable = hodl_view_datadir_key(datadir, datadir_key);
    hodl_view_tip_hash(db, tip, tip_hash);
    bool ok = cacheable &&
        hodl_view_cache_get(datadir_key, tip, tip_hash, &hodl);
    if (!ok) {
        ok = hodl_wave_scan_current_utxos(db, tip, &hodl);
        if (ok && cacheable)
            hodl_view_cache_put(datadir_key, tip, tip_hash, &hodl);
    }
    sqlite3_close(db);

    APPEND(off, r, max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n"
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>HODL Wave</title>"
        "<link rel='stylesheet' href='/explorer/style.css'>"
        "</head><body>");
    off += explorer_emit_nav((char *)r + off, max - off, "hodl");

    if (!ok) {
        APPEND(off, r, max,
            "<div style='max-width:900px;margin:40px auto;color:#ccc'>"
            "<h1>HODL Wave</h1>"
            "<p>Degraded: %s.</p>"
            "</div>" EXPLORER_FOOTER,
            hodl.status);
        return off;
    }

    double older_pct = hodl_wave_older_than_1y_percent(&hodl);
    char total_fmt[64], older_fmt[64];
    zcl_format_zcl(total_fmt, sizeof(total_fmt), hodl.total_value);
    zcl_format_zcl(older_fmt, sizeof(older_fmt), hodl.older_than_1y_value);

    APPEND(off, r, max,
        "<div style='text-align:center;margin:30px 0 10px'>"
        "<h1 style='font-size:42px;color:#fff;font-weight:800;margin:0;"
        "font-family:Georgia,\"Times New Roman\",serif'>"
        "%.3f%% of Current Transparent UTXO Value Is Older Than 1 Year</h1>"
        "<p style='font-size:19px;color:#888;margin:8px 0 0;"
        "font-family:Georgia,serif'>Current UTXO age distribution at block %" PRId64 "</p>"
        "</div>",
        older_pct, hodl.tip_height);

    /* ── Time-series chart: % held > 1y over time ────────────────
     *
     * Metric: at each historical block height H, what fraction of the
     * transparent UTXO value alive at H was already older than 1 year?
     *
     * Source: hodl_history snapshots, filled by hodl_history_service from
     * tx_outputs + tx_inputs. The service computes "alive at H" by including
     * outputs created on or before H and excluding outputs spent on or before
     * H, so old chart points do not depend on today's surviving UTXOs. */
    {
        sqlite3 *udb = NULL;
        if (explorer_open_readonly_db(datadir, &udb)) {
            enum { HODL_CHART_MAX_ROWS = 2048 };
            struct hodl_chart_row *rows =
                zcl_calloc(HODL_CHART_MAX_ROWS, sizeof(*rows),
                           "hodl_chart_rows");
            struct hodl_history_row *hist =
                zcl_calloc(HODL_CHART_MAX_ROWS, sizeof(*hist),
                           "hodl_chart_history");
            int n = 0;

            int hist_n = (rows && hist)
                ? hodl_history_load_all(udb, hist, HODL_CHART_MAX_ROWS - 1)
                : 0;
            for (int i = 0; i < hist_n && n < HODL_CHART_MAX_ROWS - 1; i++) {
                if (hist[i].height < 1 || hist[i].height > hodl.tip_height ||
                    hist[i].time <= 0 || hist[i].total_zat <= 0)
                    continue;
                int64_t older = hist[i].older_1y_zat;
                if (older < 0) older = 0;
                if (older > hist[i].total_zat) older = hist[i].total_zat;
                rows[n].height       = hist[i].height;
                rows[n].time         = hist[i].time;
                rows[n].total_zat    = hist[i].total_zat;
                rows[n].older_1y_zat = older;
                rows[n].older_1y_pct =
                    (double)older / (double)hist[i].total_zat * 100.0;
                n++;
            }

            /* Append an exact live-tip anchor when the daily history filler has
             * not sampled the current tip yet. At tip, the current transparent
             * UTXO scan and the alive-at-H definition are the same set. */
            if (rows && hist &&
                n < HODL_CHART_MAX_ROWS &&
                hodl.total_value > 0 &&
                (n == 0 || rows[n - 1].height < hodl.tip_height)) {
                rows[n].height        = hodl.tip_height;
                rows[n].time          = (int64_t)platform_time_wall_time_t();
                rows[n].total_zat     = hodl.total_value;
                rows[n].older_1y_zat  = hodl.older_than_1y_value;
                rows[n].older_1y_pct  = older_pct;
                n++;
            }
            sqlite3_close(udb);

            if (!rows || !hist) {
                APPEND(off, r, max,
                    "<div style='max-width:1000px;margin:20px auto;"
                    "padding:16px;background:#0c0c0c;border:1px solid #1a1a1a;"
                    "border-radius:8px;color:#888'>"
                    "<h2 style='color:#bbb;margin-top:0'>"
                    "%% held &gt; 1 year over time</h2>"
                    "<p>The HODL chart is temporarily unavailable.</p>"
                    "</div>");
            } else if (n < 2) {
                int64_t latest_height = hodl.tip_height;
                int64_t latest_time = (int64_t)platform_time_wall_time_t();
                int64_t latest_total = hodl.total_value;
                int64_t latest_older = hodl.older_than_1y_value;
                double latest_pct = older_pct;
                if (n == 1) {
                    latest_height = rows[0].height;
                    latest_time = rows[0].time;
                    latest_total = rows[0].total_zat;
                    latest_older = rows[0].older_1y_zat;
                    latest_pct = rows[0].older_1y_pct;
                }

                char latest_total_fmt[64];
                char latest_older_fmt[64];
                char latest_date[32] = "current tip";
                zcl_format_zcl(latest_total_fmt, sizeof(latest_total_fmt),
                               latest_total);
                zcl_format_zcl(latest_older_fmt, sizeof(latest_older_fmt),
                               latest_older);
                if (latest_time > 0) {
                    time_t tt = (time_t)latest_time;
                    struct tm tm_;
                    if (gmtime_r(&tt, &tm_) != NULL)
                        strftime(latest_date, sizeof(latest_date), "%Y-%m-%d",
                                 &tm_);
                }

                APPEND(off, r, max,
                    "<div style='max-width:1000px;margin:20px auto;"
                    "padding:16px;background:#0c0c0c;border:1px solid #1a1a1a;"
                    "border-radius:8px;color:#888'>"
                    "<h2 style='color:#bbb;margin-top:0'>"
                    "Historical transparent UTXO value: %% held &gt; 1 year</h2>"
                    "<svg viewBox='0 0 1000 170' role='img' "
                    "aria-label='Latest HODL measurement at block %" PRId64 "' "
                    "style='width:100%%;height:auto;background:#080808;"
                    "border:1px solid #181818;border-radius:6px;display:block'>"
                    "<text x='30' y='34' fill='#bbb' font-size='18' "
                    "font-family='Georgia,serif'>Latest measurement</text>"
                    "<line x1='70' y1='104' x2='930' y2='104' "
                    "stroke='#1f1f1f'/>"
                    "<circle cx='500' cy='92' r='6' fill='#33ff99'/>"
                    "<text x='500' y='74' fill='#33ff99' font-size='26' "
                    "font-weight='700' text-anchor='middle'>%.3f%%</text>"
                    "<text x='500' y='128' fill='#999' font-size='14' "
                    "text-anchor='middle' font-family='Georgia,serif'>"
                    "block %" PRId64 " - %s</text>"
                    "</svg>"
                    "<div style='display:grid;grid-template-columns:"
                    "repeat(auto-fit,minmax(160px,1fr));gap:14px;"
                    "margin-top:14px'>"
                    "<div style='border-top:1px solid #202020;padding-top:10px'>"
                    "<div style='color:#777;font-size:13px'>Total transparent UTXO</div>"
                    "<div style='color:#eee;font-size:18px'>%s ZCL</div>"
                    "</div>"
                    "<div style='border-top:1px solid #202020;padding-top:10px'>"
                    "<div style='color:#777;font-size:13px'>Older than 1 year</div>"
                    "<div style='color:#eee;font-size:18px'>%s ZCL</div>"
                    "</div>"
                    "<div style='border-top:1px solid #202020;padding-top:10px'>"
                    "<div style='color:#777;font-size:13px'>Samples available</div>"
                    "<div style='color:#eee;font-size:18px'>1 current tip anchor</div>"
                    "</div>"
                    "</div>"
                    "<p style='margin:14px 0 0;color:#777'>This panel uses "
                    "the current verified transparent UTXO distribution now; "
                    "older samples are added to the chart when available.</p>"
                    "</div>",
                    latest_height, latest_pct, latest_height, latest_date,
                    latest_total_fmt, latest_older_fmt);
            } else {
                /* Compute min/max for y-axis scaling. Use [floor..100]
                 * with a 5%% headroom floor to keep the curve readable
                 * even when held>1y stays in a tight band. */
                double y_min = rows[0].older_1y_pct, y_max = rows[0].older_1y_pct;
                for (int i = 1; i < n; i++) {
                    if (rows[i].older_1y_pct < y_min) y_min = rows[i].older_1y_pct;
                    if (rows[i].older_1y_pct > y_max) y_max = rows[i].older_1y_pct;
                }
                y_min -= 2.0; y_max += 2.0;
                if (y_min < 0) y_min = 0;
                if (y_max > 100) y_max = 100;
                if (y_max - y_min < 5) { y_min = y_min > 5 ? y_min - 5 : 0; y_max = y_min + 10; }

                int W = 1000, H = 380;
                int pl = 70, pr = 25, pt = 50, pb = 60;
                int pw = W - pl - pr, ph = H - pt - pb;
                int64_t t_min = rows[0].time, t_max = rows[n-1].time;
                if (t_max <= t_min) t_max = t_min + 1;
                int gap_count = 0;
                for (int i = 1; i < n; i++) {
                    if (hodl_chart_rows_have_gap(&rows[i - 1], &rows[i]))
                        gap_count++;
                }
                char sample_meta[64];
                if (gap_count > 0) {
                    snprintf(sample_meta, sizeof(sample_meta),
                             "%d samples · %d gap%s",
                             n, gap_count, gap_count == 1 ? "" : "s");
                } else {
                    snprintf(sample_meta, sizeof(sample_meta),
                             "%d samples · daily", n);
                }

                APPEND(off, r, max,
                    "<div style='max-width:1000px;margin:20px auto'>"
                    "<svg id='hodl-ts' viewBox='0 0 %d %d' tabindex='0' "
                    "role='img' aria-label='Time series: percent of historical "
                    "transparent UTXO value held longer than one year at each "
                    "sampled block. Use left/right arrows to inspect samples.' "
                    "style='width:100%%;"
                    "height:auto;background:#0c0c0c;border:1px solid #1a1a1a;"
                    "border-radius:8px;display:block;outline:none'>"
                    "<text x='30' y='30' fill='#bbb' font-size='18' "
                    "font-family='Georgia,serif'>Historical transparent UTXO value: %% held &gt; 1 year</text>"
                    "<text x='%d' y='30' fill='#666' font-size='12' "
                    "text-anchor='end' font-family='Georgia,serif'>"
                    "%s</text>",
                    W, H, W - pr, sample_meta);

                /* Y-axis gridlines + labels */
                for (int g = 0; g <= 4; g++) {
                    double yv = y_min + (y_max - y_min) * g / 4.0;
                    int y = pt + ph - (int)((yv - y_min) / (y_max - y_min) * ph);
                    APPEND(off, r, max,
                        "<line x1='%d' y1='%d' x2='%d' y2='%d' "
                        "stroke='#1a1a1a'/>"
                        "<text x='%d' y='%d' fill='#777' font-size='12' "
                        "text-anchor='end'>%.1f%%</text>",
                        pl, y, pl + pw, y, pl - 8, y + 4, yv);
                }

                /* X-axis date labels: 5 evenly spaced ticks */
                for (int g = 0; g <= 4; g++) {
                    int64_t t = t_min + (t_max - t_min) * g / 4;
                    int x = pl + pw * g / 4;
                    time_t tt = (time_t)t;
                    struct tm tm_;
                    gmtime_r(&tt, &tm_);
                    char dbuf[16];
                    strftime(dbuf, sizeof(dbuf), "%Y-%m", &tm_);
                    APPEND(off, r, max,
                        "<line x1='%d' y1='%d' x2='%d' y2='%d' "
                        "stroke='#1a1a1a'/>"
                        "<text x='%d' y='%d' fill='#777' font-size='12' "
                        "text-anchor='middle' font-family='Georgia,serif'>%s</text>",
                        x, pt, x, pt + ph, x, pt + ph + 18, dbuf);
                }

                /* Missing historical samples are a real data gap while the
                 * repair worker backfills stale rows. Shade the gap and draw
                 * separate line segments so the chart never interpolates
                 * through years of unknown samples. */
                for (int i = 1; i < n; i++) {
                    if (!hodl_chart_rows_have_gap(&rows[i - 1], &rows[i]))
                        continue;
                    int x1 = hodl_chart_x(&rows[i - 1], pl, pw, t_min, t_max);
                    int x2 = hodl_chart_x(&rows[i], pl, pw, t_min, t_max);
                    int gx = x1 + 4;
                    int gw = x2 - x1 - 8;
                    if (gw <= 0)
                        continue;
                    APPEND(off, r, max,
                        "<rect x='%d' y='%d' width='%d' height='%d' "
                        "fill='#151515' opacity='0.55'/>"
                        "<line x1='%d' y1='%d' x2='%d' y2='%d' "
                        "stroke='#333' stroke-dasharray='3,4'/>"
                        "<line x1='%d' y1='%d' x2='%d' y2='%d' "
                        "stroke='#333' stroke-dasharray='3,4'/>",
                        gx, pt, gw, ph, x1, pt, x1, pt + ph,
                        x2, pt, x2, pt + ph);
                    if (gw >= 120) {
                        APPEND(off, r, max,
                            "<text x='%d' y='%d' fill='#555' "
                            "font-size='12' text-anchor='middle' "
                            "font-family='Georgia,serif'>backfilling</text>",
                            gx + gw / 2, pt + ph / 2);
                    }
                }

                int seg_start = 0;
                while (seg_start < n) {
                    int seg_end = seg_start;
                    while (seg_end + 1 < n &&
                           !hodl_chart_rows_have_gap(&rows[seg_end],
                                                      &rows[seg_end + 1])) {
                        seg_end++;
                    }
                    if (seg_end > seg_start) {
                        APPEND(off, r, max,
                            "<polyline fill='none' stroke='#33ff99' "
                            "stroke-width='2' points='");
                        for (int i = seg_start; i <= seg_end; i++) {
                            int x = hodl_chart_x(&rows[i], pl, pw,
                                                 t_min, t_max);
                            int y = hodl_chart_y(&rows[i], pt, ph,
                                                 y_min, y_max);
                            APPEND(off, r, max, "%s%d,%d",
                                   i == seg_start ? "" : " ", x, y);
                        }
                        APPEND(off, r, max, "'/>");
                    } else {
                        int x = hodl_chart_x(&rows[seg_start], pl, pw,
                                             t_min, t_max);
                        int y = hodl_chart_y(&rows[seg_start], pt, ph,
                                             y_min, y_max);
                        APPEND(off, r, max,
                            "<circle cx='%d' cy='%d' r='3' fill='#33ff99'/>",
                            x, y);
                    }
                    seg_start = seg_end + 1;
                }

                /* Hover crosshair + tooltip (hidden until JS shows it).
                 * pointer-events:none on every overlay node so the cursor
                 * always hits the svg below — no flicker/jitter when the
                 * pointer crosses the dot or tooltip. The tooltip's summary
                 * rows + per-band legend are built by JS; the static rect is
                 * a placeholder the script resizes to fit. */
                APPEND(off, r, max,
                    "<line id='hodl-xhair' x1='0' y1='%d' x2='0' y2='%d' "
                    "stroke='#33ff99' stroke-dasharray='2,3' stroke-width='1' "
                    "style='display:none;pointer-events:none'/>"
                    "<circle id='hodl-dot' cx='0' cy='0' r='4' "
                    "fill='#33ff99' style='display:none;pointer-events:none'/>"
                    "<g id='hodl-tip' style='display:none;pointer-events:none'>"
                    "<rect id='hodl-tip-bg' x='0' y='0' width='290' "
                    "height='104' rx='6' fill='#000' stroke='#33ff99' "
                    "opacity='0.95'/>"
                    "<text id='hodl-tip-date' x='10' y='20' fill='#fff' "
                    "font-size='13' font-family='Georgia,serif'>—</text>"
                    "<text id='hodl-tip-pct' x='10' y='40' fill='#33ff99' "
                    "font-size='15' font-weight='600'>—</text>"
                    "<text id='hodl-tip-amt' x='10' y='58' fill='#bbb' "
                    "font-size='12'>—</text>"
                    "<text id='hodl-tip-mv' x='10' y='74' fill='#ffcc66' "
                    "font-size='12'>—</text>"
                    "<text id='hodl-tip-h' x='10' y='90' fill='#666' "
                    "font-size='11'>—</text>"
                    "</g>",
                    pt, pt + ph);

                /* Close the SVG + wrapper BEFORE the <script>. An HTML
                 * <script> nested in SVG foreign content is tokenized in the
                 * DATA state, so comparisons like a<b open bogus tags and
                 * mangle the code — it must be a body-level sibling (same as
                 * explorer_pages_view.c / explorer_main_view.c). The script
                 * still reaches the SVG children via getElementById. */
                APPEND(off, r, max, "</svg></div>");

                /* Inline data block for JS — one row per sample, packed
                 * as [height, time, total_zat, older_zat, pct_x1000]. */
                APPEND(off, r, max,
                    "<script>(function(){"
                    "var data=[");
                for (int i = 0; i < n; i++) {
                    APPEND(off, r, max,
                        "%s[%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%d]",
                        i ? "," : "",
                        rows[i].height, rows[i].time,
                        rows[i].total_zat, rows[i].older_1y_zat,
                        (int)(rows[i].older_1y_pct * 1000.0));
                }
                APPEND(off, r, max,
                    "];"
                    "var W=%d,pl=%d,pr=%d,pt=%d,pb=%d,ph=%d,pw=W-pl-pr;"
                    "var ymin=%.3f,ymax=%.3f,tmin=%" PRId64 ",tmax=%" PRId64 ";"
                    "var svg=document.getElementById('hodl-ts');"
                    "var xhair=document.getElementById('hodl-xhair');"
                    "var dot=document.getElementById('hodl-dot');"
                    "var tip=document.getElementById('hodl-tip');"
                    "var tipBg=document.getElementById('hodl-tip-bg');"
                    "var td=document.getElementById('hodl-tip-date');"
                    "var tp=document.getElementById('hodl-tip-pct');"
                    "var ta=document.getElementById('hodl-tip-amt');"
                    "var tm=document.getElementById('hodl-tip-mv');"
                    "var th=document.getElementById('hodl-tip-h');"
                    "var TIPW=290,TIPH=104;"
                    "tipBg.setAttribute('width',TIPW);"
                    "tipBg.setAttribute('height',TIPH);"
                    "function fmtZcl(z){"
                    "var v=z/1e8;"
                    "if(v>=1e6)return(v/1e6).toFixed(2)+'M';"
                    "if(v>=1e3)return(v/1e3).toFixed(2)+'k';"
                    "return v.toFixed(2);"
                    "}"
                    "function fmtDate(t){"
                    "var d=new Date(t*1000);"
                    "return d.toISOString().slice(0,10);"
                    "}"
                    "var cur=-1;"
                    "function hide(){"
                    "xhair.style.display='none';"
                    "dot.style.display='none';"
                    "tip.style.display='none';"
                    "}"
                    "function pickNearest(svgX){"
                    "var tfrac=(svgX-pl)/pw;"
                    "var target=tmin+tfrac*(tmax-tmin);"
                    "var lo=0,hi=data.length-1;"
                    "while(lo<hi){var m=(lo+hi)>>1;"
                    "if(data[m][1]<target)lo=m+1;else hi=m;}"
                    "if(lo>0&&Math.abs(data[lo-1][1]-target)<"
                    "Math.abs(data[lo][1]-target))lo--;"
                    "return lo;"
                    "}"
                    /* render at a continuous svg x: crosshair tracks the
                     * cursor smoothly; dot + tooltip snap to the nearest
                     * sample. All coords rounded to integers (no shimmer). */
                    "function render(svgX){"
                    "if(svgX<pl)svgX=pl;if(svgX>W-pr)svgX=W-pr;"
                    "var cx=Math.round(svgX);"
                    "xhair.setAttribute('x1',cx);xhair.setAttribute('x2',cx);"
                    "xhair.style.display='';"
                    "var i=pickNearest(svgX);cur=i;var row=data[i];"
                    "var x=Math.round(pl+(row[1]-tmin)/(tmax-tmin)*pw);"
                    "var pct=row[4]/1000;"
                    "var y=Math.round(pt+ph-(pct-ymin)/(ymax-ymin)*ph);"
                    "dot.setAttribute('cx',x);dot.setAttribute('cy',y);"
                    "dot.style.display='';"
                    "var tx=cx+14;if(tx+TIPW>W-pr)tx=cx-TIPW-14;if(tx<pl)tx=pl;"
                    "var ty=Math.round(y-TIPH/2);"
                    "if(ty+TIPH>pt+ph)ty=pt+ph-TIPH;if(ty<pt)ty=pt;"
                    "tip.setAttribute('transform','translate('+tx+','+ty+')');"
                    "tip.style.display='';"
                    "td.textContent=fmtDate(row[1]);"
                    "tp.textContent=pct.toFixed(3)+'%% held > 1 year';"
                    "ta.textContent=fmtZcl(row[3])+' / '+fmtZcl(row[2])+' ZCL';"
                    "tm.textContent='Transparent UTXOs alive at sampled height';"
                    "th.textContent='Block '+row[0].toLocaleString();"
                    "}"
                    /* coalesce pointer moves into one rAF tick per frame */
                    "var pend=null,raf=0;"
                    "function sched(svgX){pend=svgX;if(!raf)"
                    "raf=requestAnimationFrame(function(){raf=0;"
                    "if(pend!=null)render(pend);});}"
                    "function pt2svg(clientX){"
                    "var rc=svg.getBoundingClientRect();"
                    "return (clientX-rc.left)*(W/rc.width);"
                    "}"
                    "svg.addEventListener('mousemove',function(e){"
                    "var sx=pt2svg(e.clientX);"
                    "if(sx<pl||sx>W-pr){hide();return;}"
                    "sched(sx);"
                    "});"
                    "svg.addEventListener('mouseleave',hide);"
                    "function onTouch(e){"
                    "if(!e.touches[0])return;"
                    "var sx=pt2svg(e.touches[0].clientX);"
                    "if(sx>=pl&&sx<=W-pr)sched(sx);"
                    "e.preventDefault();"
                    "}"
                    "svg.addEventListener('touchstart',onTouch,{passive:false});"
                    "svg.addEventListener('touchmove',onTouch,{passive:false});"
                    "svg.addEventListener('touchend',hide);"
                    "svg.addEventListener('touchcancel',hide);"
                    "svg.addEventListener('keydown',function(e){"
                    "var k=e.key,i=cur<0?data.length-1:cur;"
                    "if(k==='ArrowLeft')i=Math.max(0,i-1);"
                    "else if(k==='ArrowRight')i=Math.min(data.length-1,i+1);"
                    "else if(k==='Home')i=0;"
                    "else if(k==='End')i=data.length-1;"
                    "else if(k==='Escape'){hide();return;}"
                    "else return;"
                    "e.preventDefault();"
                    "render(pl+(data[i][1]-tmin)/(tmax-tmin)*pw);"
                    "});"
                    "})();</script>",
                    W, pl, pr, pt, pb, ph, y_min, y_max, t_min, t_max);
            }
            free(rows);
            free(hist);
        }
    }

    APPEND(off, r, max,
        "<div style='max-width:1000px;margin:20px auto'>"
        "<svg viewBox='0 0 1000 360' style='width:100%%;height:auto;"
        "background:#0c0c0c;border:1px solid #1a1a1a;border-radius:8px;"
        "display:block'>"
        "<text x='30' y='35' fill='#bbb' font-size='18' "
        "font-family='Georgia,serif'>Unspent transparent value by age</text>");

    int x0 = 70, y0 = 285, chart_w = 860, chart_h = 220;
    for (int g = 0; g <= 4; g++) {
        int y = y0 - chart_h * g / 4;
        APPEND(off, r, max,
            "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#1a1a1a'/>"
            "<text x='%d' y='%d' fill='#777' font-size='12' "
            "text-anchor='end'>%d%%</text>",
            x0, y, x0 + chart_w, y, x0 - 8, y + 4, g * 25);
    }

    int bar_gap = 10;
    int bar_w = (chart_w - bar_gap * (HODL_WAVE_BUCKETS - 1)) /
                HODL_WAVE_BUCKETS;
    for (int b = 0; b < HODL_WAVE_BUCKETS; b++) {
        double pct = hodl.total_value > 0
            ? (double)hodl.buckets[b].value / (double)hodl.total_value * 100.0 : 0.0;
        int bh = (int)(pct / 100.0 * chart_h);
        int x = x0 + b * (bar_w + bar_gap);
        int y = y0 - bh;
        APPEND(off, r, max,
            "<rect x='%d' y='%d' width='%d' height='%d' fill='%s' rx='3'>"
            "<title>%s: %.3f%%, %" PRId64 " UTXOs</title></rect>"
            "<text x='%d' y='%d' fill='#aaa' font-size='11' "
            "text-anchor='middle' transform='rotate(-35,%d,%d)'>%s</text>"
            "<text x='%d' y='%d' fill='#eee' font-size='12' "
            "text-anchor='middle'>%.2f%%</text>",
            x, y, bar_w, bh > 1 ? bh : 1, hodl.buckets[b].color,
            hodl.buckets[b].html_label, pct, hodl.buckets[b].count,
            x + bar_w / 2, y0 + 26, x + bar_w / 2, y0 + 26,
            hodl.buckets[b].html_label,
            x + bar_w / 2, y - 6, pct);
    }
    APPEND(off, r, max,
        "<text x='970' y='345' fill='#444' font-size='11' "
        "font-family='Georgia,serif' text-anchor='end'>"
        "Source: current transparent UTXO set</text></svg>");

    APPEND(off, r, max,
        "<div class='stats-row' style='margin-top:18px'>"
        "<div class='stat'><div class='num'>%s</div><div class='lbl'>Current transparent UTXO value</div></div>"
        "<div class='stat'><div class='num'>%s</div><div class='lbl'>Older than 1 year</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div><div class='lbl'>UTXOs counted</div></div>"
        "<div class='stat'><div class='num'>%" PRId64 "</div><div class='lbl'>Rows skipped</div></div>"
        "</div>",
        total_fmt, older_fmt, hodl.total_count, hodl.skipped_rows);

    APPEND(off, r, max,
        "<table class='txlist' style='max-width:1000px;margin:18px auto'>"
        "<tr><th>Age</th><th>UTXOs</th><th>Value</th><th>Share</th></tr>");
    for (int b = 0; b < HODL_WAVE_BUCKETS; b++) {
        char val_fmt[64];
        zcl_format_zcl(val_fmt, sizeof(val_fmt), hodl.buckets[b].value);
        double pct = hodl.total_value > 0
            ? (double)hodl.buckets[b].value / (double)hodl.total_value * 100.0 : 0.0;
        APPEND(off, r, max,
            "<tr><td><span style='display:inline-block;width:11px;height:11px;"
            "background:%s;border-radius:2px;margin-right:8px'></span>%s</td>"
            "<td>%" PRId64 "</td><td>%s ZCL</td><td>%.3f%%</td></tr>",
            hodl.buckets[b].color, hodl.buckets[b].html_label,
            hodl.buckets[b].count, val_fmt, pct);
    }
    APPEND(off, r, max, "</table>");
    APPEND(off, r, max,
        "<p style='max-width:900px;margin:18px auto;color:#888;"
        "font-family:Georgia,serif;font-size:16px;line-height:1.7'>"
        "Source: current transparent UTXO set. Metric: UTXO age distribution."
        "</p></div>" EXPLORER_FOOTER);
    return off;
}
