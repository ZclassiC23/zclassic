/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Explorer HODL-wave VIEW.
 *
 * Part of the explorer secondary-pages split: this TU owns the HODL-wave
 * page — the cumulative-value bisect helpers, the time-series chart
 * assembly, and explorer_view_hodl. The other secondary pages (tokens,
 * token detail, event log, names, market, swaps, messages, loading
 * placeholder) live in explorer_pages_view.c. explorer_view_hodl is declared
 * in views/explorer_pages_view.h. */

#include "platform/time_compat.h"
#include "views/explorer_pages_view.h"
#include "controllers/explorer_internal.h"
#include "jobs/reducer_frontier.h"
#include "models/hodl_wave.h"
#include "services/hodl_history_service.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "views/format_helpers.h"

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── HODL Wave Page ────────────────────────────────────────── */

/* Helper for the HODL page: bisect a height-sorted cumulative-value
 * array. Returns the cumulative value at the largest stored height
 * that does not exceed target_h, or 0 when no such row exists. */
struct hodl_cum_row { int64_t h; int64_t cum_v; };

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

static int64_t hodl_cum_at(const struct hodl_cum_row *cum, int n,
                           int64_t target_h)
{
    int lo = 0, hi = n - 1, found = -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (cum[m].h <= target_h) { found = m; lo = m + 1; }
        else hi = m - 1;
    }
    return found >= 0 ? cum[found].cum_v : (int64_t)0;
}

/* Same bisect but returns the array INDEX (0-based) of the matched
 * row, or -1. Use to convert a height boundary into a UTXO count. */
static int hodl_cum_idx_at(const struct hodl_cum_row *cum, int n,
                           int64_t target_h)
{
    int lo = 0, hi = n - 1, found = -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (cum[m].h <= target_h) { found = m; lo = m + 1; }
        else hi = m - 1;
    }
    return found;
}

/* Invert hodl_wave_age_seconds: return the creation height whose age,
 * measured with sample_h as the tip, equals age_sec. Mirrors the model's
 * Buttercup-aware spacing (pre-BC 150 s/block, post-BC 75 s/block) so the
 * per-band split computed here lines up with hodl_wave_bucket_index. */
static int64_t hodl_height_for_age(int64_t sample_h, int64_t age_sec)
{
    const int64_t BC = 707000, PRE = 150, POST = 75;
    if (age_sec <= 0) return sample_h;
    if (sample_h >= BC) {
        int64_t post_span = (sample_h - BC) * POST; /* max age inside post-BC */
        if (age_sec <= post_span) return sample_h - age_sec / POST;
        int64_t rem = age_sec - post_span;           /* spill into pre-BC */
        return BC - rem / PRE;
    }
    /* sample_h below Buttercup: only the 150 s spacing applies */
    return sample_h - age_sec / PRE;
}

/* Per-sample chart row including the day-window movement counters and the
 * per-age-band value split (percent x1000) used by the hover legend. */
struct hodl_chart_row {
    int64_t height;
    int64_t time;
    int64_t total_zat;
    int64_t older_1y_zat;
    double  older_1y_pct;
    int64_t created_count;  /* UTXOs created in this day's window, still unspent */
    int64_t created_zat;
    int     band_x1000[HODL_WAVE_BUCKETS]; /* per-band share of total, pct x1000 */
};

/* Fill band_x1000[] with each age band's share (percent x1000) of `total`
 * at sample_h. Band b covers ages [prev_max, max); the youngest band starts
 * at 0, the oldest band runs to genesis. The split telescopes to `total`,
 * so the bands sum to ~100%. Bisects the cumulative-value array at the
 * height boundaries that the band's age edges map to. */
static void hodl_fill_bands(const struct hodl_cum_row *cum, int n_cum,
                            const struct hodl_wave_snapshot *hodl,
                            int64_t sample_h, int64_t total,
                            int *band_x1000)
{
    for (int b = 0; b < HODL_WAVE_BUCKETS; b++) {
        int64_t young_age = b == 0 ? 0 : hodl->buckets[b - 1].max_age_seconds;
        int64_t cv_young = hodl_cum_at(cum, n_cum,
                                       hodl_height_for_age(sample_h, young_age));
        int64_t cv_old = 0; /* last band runs to genesis → 0 */
        if (b < HODL_WAVE_BUCKETS - 1) {
            int64_t old_h = hodl_height_for_age(sample_h,
                                                hodl->buckets[b].max_age_seconds);
            cv_old = hodl_cum_at(cum, n_cum, old_h);
        }
        int64_t band_val = cv_young - cv_old;
        if (band_val < 0) band_val = 0;
        band_x1000[b] = total > 0
            ? (int)((double)band_val / (double)total * 100000.0) : 0;
    }
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
    bool ok = hodl_wave_scan_current_utxos(db, tip, &hodl);
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
     * Metric: at each historical block height H, what fraction of
     * TODAY'S transparent UTXO supply was already > 1 year old at H?
     *
     * Source: the live `utxos` table. Every row is a currently-unspent
     * output with its creation height — dense, no indexer dependency.
     *
     * Note: this metric measures "% of supply that survives today, as
     * a function of how long it was held when the chart sampled". It
     * does NOT include UTXOs that existed at H but have since been
     * spent (those rows aren't in the utxos table by definition). The
     * right edge of the chart equals the headline `older_pct` because
     * at H = today every current UTXO contributes to the denominator
     * and the >1y subset to the numerator.
     *
     * Correctness: each (total, older) pair is computed from a strict
     * subset of current UTXOs, so the displayed pct is always a real
     * ratio of real-value sums. No partial-index assumptions. */
    {
        sqlite3 *udb = NULL;
        if (explorer_open_readonly_db(datadir, &udb)) {
            /* Pass 1: load utxos sorted by creation height; build a
             * cumulative-value array so we can answer "sum of value of
             * UTXOs created at height ≤ H" via a binary search.
             * Static buffer sized for ~2 M current UTXOs (today: 1.34 M
             * on mainnet). If the table exceeds the cap, sample
             * accuracy degrades smoothly — we just stop accumulating. */
            #define HODL_CUM_MAX 2000000
            static struct hodl_cum_row cum[HODL_CUM_MAX];
            int n_cum = 0;
            int64_t total_supply_zat = 0;
            {
                sqlite3_stmt *s = NULL;
                if (sqlite3_prepare_v2(udb,
                        "SELECT height, value FROM utxos "
                        "WHERE value > 0 ORDER BY height ASC",
                        -1, &s, NULL) == SQLITE_OK && s) {
                    int64_t running = 0;
                    while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW &&
                           n_cum < HODL_CUM_MAX) {
                        int64_t h = sqlite3_column_int64(s, 0);
                        int64_t v = sqlite3_column_int64(s, 1);
                        if (h < 0) continue;
                        running += v;
                        cum[n_cum].h     = h;
                        cum[n_cum].cum_v = running;
                        n_cum++;
                    }
                    sqlite3_finalize(s);
                    total_supply_zat = running;
                }
            }
            /* bisect via the file-scope helper hodl_cum_at(cum, n_cum, h). */

            /* Real on-chain timestamps: one point lookup per sample height
             * against blocks.time (the canonical mined-block time). Prepared
             * once and reused; udb stays open through the sample loop and is
             * closed below. Falls back to the spacing estimate only if a row
             * is missing (a gap during indexing). */
            sqlite3_stmt *ts = NULL;
            if (sqlite3_prepare_v2(udb,
                    "SELECT time FROM blocks WHERE height=?1 AND status>=3 "
                    "LIMIT 1", -1, &ts, NULL) != SQLITE_OK) {
                LOG_WARN("hodl", "prepare blocks.time lookup failed: %s",
                         sqlite3_errmsg(udb));
                ts = NULL;
            }

            /* Sample one row per ~day from stride to tip. Use the
             * chain tip from the headline snapshot as the right edge
             * so the last sample lines up with the headline value.
             *
             * Per-sample movement metric: count + value of UTXOs whose
             * creation height falls inside the day's window
             * (sample_h - stride, sample_h] AND that are still unspent
             * today. This is a strict subset of true daily UTXO creation
             * (we can't see UTXOs that got spent before today since
             * those rows aren't in the utxos table) — labelled as
             * "surviving" in the hover so the metric isn't oversold. */
            int64_t bpY = HODL_HISTORY_BLOCKS_PER_YEAR;
            int64_t stride = HODL_HISTORY_SAMPLE_STRIDE;
            int64_t tip_h = hodl.tip_height;
            static struct hodl_chart_row rows[2048];
            int n = 0;
            const int64_t GENESIS_TIME = 1478403829LL;
            const int64_t BC_HEIGHT    = 707000;
            for (int64_t sample_h = stride;
                 sample_h <= tip_h &&
                 n < (int)(sizeof(rows)/sizeof(rows[0])) - 1;
                 sample_h += stride) {
                int64_t total = hodl_cum_at(cum, n_cum, sample_h);
                if (total <= 0) continue;
                int64_t older = sample_h > bpY
                              ? hodl_cum_at(cum, n_cum, sample_h - bpY)
                              : 0;

                /* Day-window: indices of (sample_h - stride, sample_h]. */
                int hi_idx = hodl_cum_idx_at(cum, n_cum, sample_h);
                int lo_idx = sample_h >= stride
                           ? hodl_cum_idx_at(cum, n_cum, sample_h - stride)
                           : -1;
                int64_t created_count = (int64_t)(hi_idx - lo_idx);
                int64_t created_zat =
                    (hi_idx >= 0 ? cum[hi_idx].cum_v : 0) -
                    (lo_idx >= 0 ? cum[lo_idx].cum_v : 0);

                /* True on-chain block time; fall back to the spacing
                 * estimate (pre-Buttercup 150 s, post-Buttercup 75 s) only
                 * when the row is absent. */
                int64_t btime = 0;
                if (ts) {
                    sqlite3_reset(ts);
                    sqlite3_bind_int64(ts, 1, sample_h);
                    if (AR_STEP_ROW_READONLY(ts) == SQLITE_ROW)
                        btime = sqlite3_column_int64(ts, 0);
                }
                if (btime <= 0) {
                    int64_t pre  = sample_h < BC_HEIGHT ? sample_h : BC_HEIGHT;
                    int64_t post = sample_h < BC_HEIGHT
                                 ? 0 : (sample_h - BC_HEIGHT);
                    btime = GENESIS_TIME + pre * 150 + post * 75;
                }
                rows[n].height        = sample_h;
                rows[n].time          = btime;
                rows[n].total_zat     = total;
                rows[n].older_1y_zat  = older;
                rows[n].older_1y_pct  = (double)older / (double)total * 100.0;
                rows[n].created_count = created_count;
                rows[n].created_zat   = created_zat;
                hodl_fill_bands(cum, n_cum, &hodl, sample_h, total,
                                rows[n].band_x1000);
                n++;
            }

            /* Append a "today" anchor — uses the live headline values
             * directly so the rightmost data point IS the headline. */
            if (n < (int)(sizeof(rows)/sizeof(rows[0])) &&
                hodl.total_value > 0) {
                int hi_idx = hodl_cum_idx_at(cum, n_cum, hodl.tip_height);
                int lo_idx = hodl.tip_height >= stride
                           ? hodl_cum_idx_at(cum, n_cum,
                                             hodl.tip_height - stride)
                           : -1;
                rows[n].height        = hodl.tip_height;
                rows[n].time          = (int64_t)platform_time_wall_time_t();
                rows[n].total_zat     = hodl.total_value;
                rows[n].older_1y_zat  = hodl.older_than_1y_value;
                rows[n].older_1y_pct  = older_pct;
                rows[n].created_count = (int64_t)(hi_idx - lo_idx);
                rows[n].created_zat   =
                    (hi_idx >= 0 ? cum[hi_idx].cum_v : 0) -
                    (lo_idx >= 0 ? cum[lo_idx].cum_v : 0);
                hodl_fill_bands(cum, n_cum, &hodl, hodl.tip_height,
                                hodl.total_value, rows[n].band_x1000);
                n++;
            }
            if (ts) sqlite3_finalize(ts);
            sqlite3_close(udb);
            #undef HODL_CUM_MAX

            if (n < 2 || total_supply_zat <= 0) {
                APPEND(off, r, max,
                    "<div style='max-width:1000px;margin:20px auto;"
                    "padding:16px;background:#0c0c0c;border:1px solid #1a1a1a;"
                    "border-radius:8px;color:#888'>"
                    "<h2 style='color:#bbb;margin-top:0'>"
                    "%% held &gt; 1 year over time</h2>"
                    "<p>The UTXO set is still being indexed. Refresh in "
                    "a minute.</p>"
                    "</div>");
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

                APPEND(off, r, max,
                    "<div style='max-width:1000px;margin:20px auto'>"
                    "<svg id='hodl-ts' viewBox='0 0 %d %d' tabindex='0' "
                    "role='img' aria-label='Time series: percent of the current "
                    "transparent supply already held longer than one year, by "
                    "historical block. Use left/right arrows to inspect samples.' "
                    "style='width:100%%;"
                    "height:auto;background:#0c0c0c;border:1px solid #1a1a1a;"
                    "border-radius:8px;display:block;outline:none'>"
                    "<text x='30' y='30' fill='#bbb' font-size='18' "
                    "font-family='Georgia,serif'>Current supply: %% already held &gt; 1 year, by historical block</text>"
                    "<text x='%d' y='30' fill='#666' font-size='12' "
                    "text-anchor='end' font-family='Georgia,serif'>"
                    "%d samples · daily</text>",
                    W, H, W - pr, n);

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

                /* Polyline through points */
                APPEND(off, r, max,
                    "<polyline fill='none' stroke='#33ff99' "
                    "stroke-width='2' points='");
                for (int i = 0; i < n; i++) {
                    int x = pl + (int)((double)(rows[i].time - t_min) /
                                       (double)(t_max - t_min) * pw);
                    int y = pt + ph - (int)((rows[i].older_1y_pct - y_min) /
                                            (y_max - y_min) * ph);
                    APPEND(off, r, max, "%s%d,%d", i ? " " : "", x, y);
                }
                APPEND(off, r, max, "'/>");

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
                 * as comma-separated integers
                 * [height, time, total_zat, older_zat, pct_x1000,
                 *  created_count, created_zat, band0_x1000 .. band9_x1000]
                 * to keep the inline blob compact. created_* are the UTXOs
                 * created in this day's window that are still unspent today
                 * ("surviving creation"); bandN_x1000 is the percent-x1000
                 * share of total value in age band N at the sample. */
                APPEND(off, r, max,
                    "<script>(function(){"
                    "var NS='http://www.w3.org/2000/svg';"
                    "var data=[");
                for (int i = 0; i < n; i++) {
                    APPEND(off, r, max,
                        "%s[%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%d,%" PRId64 ",%" PRId64,
                        i ? "," : "",
                        rows[i].height, rows[i].time,
                        rows[i].total_zat, rows[i].older_1y_zat,
                        (int)(rows[i].older_1y_pct * 1000.0),
                        rows[i].created_count, rows[i].created_zat);
                    for (int b = 0; b < HODL_WAVE_BUCKETS; b++)
                        APPEND(off, r, max, ",%d", rows[i].band_x1000[b]);
                    APPEND(off, r, max, "]");
                }
                APPEND(off, r, max, "];var bands=[");
                for (int b = 0; b < HODL_WAVE_BUCKETS; b++)
                    APPEND(off, r, max, "%s[\"%s\",\"%s\"]", b ? "," : "",
                           hodl.buckets[b].color, hodl.buckets[b].label);
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
                    /* tooltip geometry: summary block, then one 15px legend
                     * row per band; bg rect sized to fit + clamped in plot */
                    "var TIPW=290,SUMH=104,ROWH=15,NB=bands.length;"
                    "var TIPH=SUMH+NB*ROWH+6;"
                    "tipBg.setAttribute('width',TIPW);"
                    "tipBg.setAttribute('height',TIPH);"
                    "var legend=[];"
                    "for(var bi=0;bi<NB;bi++){"
                    "var yy=SUMH+bi*ROWH;"
                    "var sw=document.createElementNS(NS,'rect');"
                    "sw.setAttribute('x',10);sw.setAttribute('y',yy-9);"
                    "sw.setAttribute('width',10);sw.setAttribute('height',10);"
                    "sw.setAttribute('rx',2);sw.setAttribute('fill',bands[bi][0]);"
                    "tip.appendChild(sw);"
                    "var lt=document.createElementNS(NS,'text');"
                    "lt.setAttribute('x',26);lt.setAttribute('y',yy);"
                    "lt.setAttribute('fill','#bbb');lt.setAttribute('font-size','11');"
                    "lt.textContent=bands[bi][1];tip.appendChild(lt);"
                    "var pv=document.createElementNS(NS,'text');"
                    "pv.setAttribute('x',TIPW-12);pv.setAttribute('y',yy);"
                    "pv.setAttribute('text-anchor','end');"
                    "pv.setAttribute('fill','#fff');pv.setAttribute('font-size','11');"
                    "tip.appendChild(pv);legend.push(pv);"
                    "}"
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
                    "tm.textContent='+ '+row[5].toLocaleString()+' UTXOs, '+fmtZcl(row[6])+' ZCL surviving';"
                    "th.textContent='Block '+row[0].toLocaleString();"
                    "for(var bi=0;bi<NB;bi++)"
                    "legend[bi].textContent=(row[7+bi]/1000).toFixed(2)+'%%';"
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
