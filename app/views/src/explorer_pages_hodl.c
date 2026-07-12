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
#include "util/ar_step_readonly.h"
#include "util/safe_alloc.h"
#include "util/template.h"
#include "util/thread_registry.h"
#include "views/format_helpers.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── HODL Wave Page ────────────────────────────────────────── */

static const char HODL_VIEW_CSS_BASE[] =
    ".hodl-page{max-width:1160px;margin:0 auto 10px;padding-bottom:4px;}"
    ".hodl-hero{display:flex;justify-content:space-between;gap:22px;"
    "align-items:flex-end;text-align:left;margin:14px auto 18px;"
    "padding:22px 0 18px;border-bottom:1px solid #26313a;}"
    ".hodl-kicker{color:#9bddff;font-size:13px;font-weight:760;"
    "text-transform:uppercase;letter-spacing:0;margin:0 0 8px;}"
    ".hodl-title{color:#f5f7fa;font-family:-apple-system,'Segoe UI',Roboto,"
    "Helvetica,Arial,sans-serif;font-size:40px;font-weight:820;line-height:1.08;"
    "letter-spacing:0;margin:0;max-width:920px;overflow-wrap:break-word;}"
    ".hodl-subtitle{font-size:15px;color:#a9b4bf;margin:9px 0 0;"
    "max-width:760px;line-height:1.55;}"
    ".hodl-hero-badge{flex:0 0 auto;color:#f3d18a;background:#161713;"
    "border:1px solid #3b3320;border-radius:8px;padding:9px 12px;"
    "font-size:13px;font-weight:720;white-space:nowrap;"
    "box-shadow:0 10px 24px rgba(0,0,0,.18);}"
    ".hodl-panel{margin:18px auto;padding:18px;background:#10151a;"
    "border:1px solid #24303a;border-radius:8px;color:#a3adb8;"
    "box-shadow:0 18px 42px rgba(0,0,0,.20);}"
    ".hodl-panel h2{color:#e1e8ee;margin-top:0;border-bottom-color:#242e37;}"
    ".hodl-chart-wrap{max-width:1160px;margin:18px auto;padding:16px;"
    "background:linear-gradient(180deg,#0f1519 0,#090d11 100%);"
    "border:1px solid #2a3741;border-radius:8px;"
    "box-shadow:0 14px 34px rgba(0,0,0,.20);position:relative;"
    "contain:layout paint;isolation:isolate;}"
    ".hodl-chart-head{display:flex;justify-content:space-between;gap:16px;"
    "align-items:flex-start;margin:2px 2px 10px;}"
    ".hodl-chart-title{color:#e1e8ee;margin:0 0 6px;font-size:25px;"
    "line-height:1.2;font-weight:780;letter-spacing:0;}"
    ".hodl-chart-subtitle{margin:0;color:#9aa8b5;font-size:14px;"
    "line-height:1.45;}"
    ".hodl-chart-meta{color:#68717d;font-size:13px;white-space:nowrap;"
    "padding-top:4px;}"
    ".hodl-legend{display:flex;flex-wrap:wrap;gap:12px 22px;"
    "margin:12px 2px 14px;color:#d5dbe3;font-size:13px;}"
    ".hodl-legend-item{display:inline-flex;align-items:center;gap:8px;}"
    ".hodl-legend-swatch{display:inline-block;width:26px;height:4px;"
    "border-radius:999px;}"
    ".hodl-current-strip{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));"
    "gap:10px;margin:0 2px 14px;}"
    ".hodl-current-chip{min-width:0;background:#0b1014;border:1px solid #26343f;"
    "border-left:3px solid var(--c);border-radius:8px;padding:9px 10px;}"
    ".hodl-current-chip span{display:block;color:#9aa8b5;font-size:12px;"
    "line-height:1.2;}"
    ".hodl-current-chip strong{display:block;color:#f2f7f4;font-size:18px;"
    "line-height:1.2;margin-top:3px;font-variant-numeric:tabular-nums;}"
    ".hodl-chart-canvas{overflow-x:auto;-webkit-overflow-scrolling:touch;"
    "touch-action:pan-x pan-y;scrollbar-color:#33414d #0b0f13;"
    "padding-bottom:4px;}"
    ".hodl-chart-canvas::-webkit-scrollbar{height:10px;}"
    ".hodl-chart-canvas::-webkit-scrollbar-track{background:#0b0f13;}"
    ".hodl-chart-canvas::-webkit-scrollbar-thumb{background:#30414e;"
    "border-radius:999px;border:2px solid #0b0f13;}"
    ".hodl-chart-wrap:focus-within{border-color:#365365;"
    "box-shadow:0 18px 46px rgba(0,0,0,.28),0 0 0 1px rgba(98,199,255,.16);}";

static const char HODL_VIEW_CSS_DETAIL[] =
    ".hodl-wave-interactive{position:relative;}"
    ".hodl-svg{width:100%;min-width:0;height:auto;background:transparent;"
    "border:0;border-radius:0;display:block;}"
    ".hodl-survival-svg{min-width:820px;}.hodl-age-svg{min-width:720px;}"
    ".hodl-axis-title{fill:#758290;font-size:11px;font-weight:680;"
    "letter-spacing:0;text-transform:uppercase;}"
    ".hodl-end-label{font-family:-apple-system,'Segoe UI',Roboto,sans-serif;"
    "font-weight:760;paint-order:stroke;stroke:#071016;stroke-width:3px;"
    "stroke-linejoin:round;}"
    ".hodl-series-line{mix-blend-mode:screen;}"
    ".hodl-hover-dot{filter:drop-shadow(0 0 4px rgba(255,255,255,.28));}"
    ".hodl-age-bar{transition:opacity .12s ease,stroke-width .12s ease,"
    "filter .12s ease;}"
    ".hodl-age-hit{cursor:crosshair;}"
    ".hodl-age-hit:focus{outline:none;}"
    ".hodl-stats{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));"
    "gap:16px;margin-top:18px;}"
    ".hodl-stats .stat{min-width:0;}"
    ".hodl-stats .num{font-size:30px;overflow-wrap:anywhere;"
    "word-break:break-word;}"
    ".hodl-table-wrap{max-width:1000px;margin:18px auto;overflow-x:auto;"
    "background:#0d1218;border:1px solid #22303a;border-radius:8px;}"
    ".hodl-table{min-width:660px;margin:0;}"
    ".hodl-table td,.hodl-table th{white-space:nowrap;}"
    ".hodl-age-key{display:inline-block;width:11px;height:11px;"
    "border-radius:2px;margin-right:8px;}"
    ".hodl-source{max-width:900px;margin:18px auto;color:#9aa3ad;"
    "font-size:16px;line-height:1.7;}"
    "@media (max-width:900px){.hodl-hero{display:block;}.hodl-hero-badge{"
    "display:inline-block;margin-top:12px;}.hodl-title{font-size:36px;}"
    ".hodl-stats{grid-template-columns:repeat(2,minmax(0,1fr));}}"
    "@media (max-width:560px){.hodl-page{max-width:none;}.hodl-hero{"
    "margin:14px auto 10px;padding:12px 0 10px;}.hodl-title{font-size:29px;"
    "line-height:1.14;}"
    ".hodl-subtitle{font-size:15px;}.hodl-panel{padding:12px;}"
    ".hodl-stats{grid-template-columns:1fr;gap:10px;}"
    ".hodl-stats .num{font-size:26px;}.hodl-chart-wrap{margin:14px -10px;"
    "padding:10px;border-left:0;border-right:0;border-radius:0;}"
    ".hodl-chart-head{display:block;margin:0 0 10px;}"
    ".hodl-chart-title{font-size:22px;}.hodl-chart-meta{margin-top:6px;}"
    ".hodl-legend{gap:10px 14px;margin:10px 0 12px;font-size:12px;}"
    ".hodl-current-strip{grid-template-columns:repeat(2,minmax(0,1fr));"
    "gap:8px;margin:0 0 12px;}"
    ".hodl-current-chip{padding:8px 9px;}.hodl-current-chip strong{font-size:16px;}"
    ".hodl-x-tick{display:none;}"
    ".hodl-survival-svg{min-width:660px;}.hodl-age-svg{min-width:620px;}"
    ".hodl-table{min-width:620px;}}"
    "@media (max-width:420px){.hodl-title{font-size:27px;}"
    ".hodl-survival-svg{min-width:620px;}.hodl-age-svg{min-width:590px;}}";

static const char HODL_PAGE_OPEN_TEMPLATE[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Cache-Control: no-store\r\n"
    "Connection: close\r\n\r\n"
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>HODL Wave</title>"
    "<link rel='icon' type='image/png' href='/explorer/favicon.png'>"
    "<link rel='stylesheet' href='/explorer/style.css'>"
    "<style>{{{css_base}}}{{{css_detail}}}</style>"
    "</head><body>";

static const char HODL_DEGRADED_TEMPLATE[] =
    "<section class='hodl-panel'>"
    "<h1>HODL Wave</h1>"
    "<p>Degraded: {{status}}.</p>"
    "</section></main>";

static const char HODL_WARMING_TEMPLATE[] =
    "<section class='hodl-panel'>"
    "<h1>HODL Wave</h1>"
    "<p>{{status}}.</p>"
    "</section></main>";

static const char HODL_HERO_TEMPLATE[] =
    "<section class='hodl-hero'>"
    "<div>"
    "<div class='hodl-kicker'>HODL Wave</div>"
    "<h1 class='hodl-title'>"
    "{{older_pct}}% held over 1 year"
    "</h1>"
    "<p class='hodl-subtitle'>{{hero_subject}} - {{snapshot_label}} at "
    "block {{tip_height}}</p>"
    "</div>"
    "<div class='hodl-hero-badge'>Transparent UTXO age</div>"
    "</section>";

static const char HODL_STATS_TEMPLATE[] =
    "<div class='stats-row hodl-stats'>"
    "<div class='stat'><div class='num'>{{total_value}}</div>"
    "<div class='lbl'>{{value_label}}</div></div>"
    "<div class='stat'><div class='num'>{{older_value}}</div>"
    "<div class='lbl'>Older than 1 year</div></div>"
    "<div class='stat'><div class='num'>{{total_count}}</div>"
    "<div class='lbl'>UTXOs counted</div></div>"
    "<div class='stat'><div class='num'>{{skipped_rows}}</div>"
    "<div class='lbl'>Rows skipped</div></div>"
    "</div>";

static const char HODL_TABLE_ROW_TEMPLATE[] =
    "<tr><td><span class='hodl-age-key' style='background:{{{color}}}'>"
    "</span>{{{age}}}</td><td>{{count}}</td><td>{{value}} ZCL</td>"
    "<td>{{share}}%</td></tr>";

struct hodl_page_model {
    char older_pct[32];
    char tip_height[32];
    char total_value[64];
    char older_value[64];
    char total_count[32];
    char skipped_rows[32];
    char hero_subject[80];
    char snapshot_label[96];
    char value_label[96];
};

static size_t hodl_append_template(size_t *off, uint8_t *r, size_t max,
                                   const char *tmpl,
                                   const struct template_var *vars,
                                   size_t var_count)
{
    size_t added = 0;
    if (!off || !r || !tmpl || *off >= max)
        return 0;
    added = template_render(tmpl, vars, var_count,
                            (char *)r + *off, max - *off);
    *off += added;
    return added;
}

static void hodl_page_model_init(struct hodl_page_model *m,
                                 const struct hodl_wave_snapshot *hodl,
                                 double older_pct, bool cached_snapshot)
{
    if (!m || !hodl)
        return;
    snprintf(m->older_pct, sizeof(m->older_pct), "%.3f", older_pct);
    snprintf(m->tip_height, sizeof(m->tip_height), "%" PRId64,
             hodl->tip_height);
    zcl_format_zcl(m->total_value, sizeof(m->total_value), hodl->total_value);
    zcl_format_zcl(m->older_value, sizeof(m->older_value),
                   hodl->older_than_1y_value);
    snprintf(m->total_count, sizeof(m->total_count), "%" PRId64,
             hodl->total_count);
    snprintf(m->skipped_rows, sizeof(m->skipped_rows), "%" PRId64,
             hodl->skipped_rows);
    if (cached_snapshot) {
        snprintf(m->hero_subject, sizeof(m->hero_subject),
                 "Transparent UTXO value");
        snprintf(m->snapshot_label, sizeof(m->snapshot_label),
                 "verified cached snapshot");
        snprintf(m->value_label, sizeof(m->value_label),
                 "Transparent UTXO value");
    } else {
        snprintf(m->hero_subject, sizeof(m->hero_subject),
                 "Transparent UTXO value");
        snprintf(m->snapshot_label, sizeof(m->snapshot_label),
                 "current verified snapshot");
        snprintf(m->value_label, sizeof(m->value_label),
                 "Current transparent UTXO value");
    }
}

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

enum {
    HODL_SURVIVAL_THRESHOLD_6M = 0,
    HODL_SURVIVAL_THRESHOLD_1Y = 1,
    HODL_SURVIVAL_THRESHOLD_2Y = 2,
    HODL_SURVIVAL_THRESHOLD_5Y = 3,
    HODL_SURVIVAL_THRESHOLDS = 4,
    HODL_SURVIVAL_MAX_ROWS = 2048,
    HODL_CUM_MAX_ROWS = 2000000,
    HODL_GENESIS_TIME = 1478403829LL,
    HODL_BUTTERCUP_HEIGHT = 707000LL,
    HODL_PRE_BUTTERCUP_SPACING = 150LL,
    HODL_POST_BUTTERCUP_SPACING = 75LL,
    HODL_HALF_YEAR_SECONDS = 15778800LL,
    HODL_ONE_YEAR_SECONDS = 31557600LL,
    HODL_TWO_YEAR_SECONDS = 63115200LL,
    HODL_FIVE_YEAR_SECONDS = 157788000LL
};

struct hodl_cum_row {
    int64_t height;
    int64_t cumulative_zat;
};

struct hodl_threshold_def {
    const char *label;
    const char *color;
    int64_t age_seconds;
};

static const struct hodl_threshold_def HODL_THRESHOLDS[HODL_SURVIVAL_THRESHOLDS] = {
    { "6 months", "#ffb454", HODL_HALF_YEAR_SECONDS },
    { "1 year",   "#35d07f", HODL_ONE_YEAR_SECONDS },
    { "2 years",  "#42a5f5", HODL_TWO_YEAR_SECONDS },
    { "5 years",  "#b56cff", HODL_FIVE_YEAR_SECONDS },
};

struct hodl_survival_row {
    int64_t height;
    int64_t time;
    int64_t total_zat;
    int64_t older_zat[HODL_SURVIVAL_THRESHOLDS];
    int pct_x1000[HODL_SURVIVAL_THRESHOLDS];
};

#define HODL_VIEW_CACHE_DATADIR_MAX 1024
#define HODL_VIEW_CACHE_HASH_MAX 80
#define HODL_VIEW_DISK_CACHE_PATH_MAX 1200
#define HODL_VIEW_DISK_CACHE_MAGIC "zcl_hodl_snapshot_v1"
#define HODL_VIEW_DISK_CACHE_FILE "hodl-current-v1.cache"
#define HODL_VIEW_SYNC_SCAN_DB_BYTES_MAX (128LL * 1024LL * 1024LL)

struct hodl_survival_cache_entry {
    bool valid;
    bool history_backed;
    char datadir[HODL_VIEW_CACHE_DATADIR_MAX];
    int64_t tip_height;
    char tip_hash[HODL_VIEW_CACHE_HASH_MAX];
    int row_count;
    struct hodl_survival_row rows[HODL_SURVIVAL_MAX_ROWS];
};

struct hodl_view_cache_entry {
    bool valid;
    char datadir[HODL_VIEW_CACHE_DATADIR_MAX];
    int64_t tip_height;
    char tip_hash[HODL_VIEW_CACHE_HASH_MAX];
    struct hodl_wave_snapshot snapshot;
};

struct hodl_view_refresh_task {
    char datadir[HODL_VIEW_CACHE_DATADIR_MAX];
    char datadir_key[HODL_VIEW_CACHE_DATADIR_MAX];
    int64_t tip_height;
    char tip_hash[HODL_VIEW_CACHE_HASH_MAX];
};

static pthread_mutex_t g_hodl_view_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static struct hodl_view_cache_entry g_hodl_view_cache;
static pthread_mutex_t g_hodl_view_refresh_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_hodl_view_refresh_active;
static pthread_mutex_t g_hodl_survival_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static struct hodl_survival_cache_entry g_hodl_survival_cache;

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

static bool hodl_view_hash_matches(sqlite3 *db, int64_t tip,
                                   const char *expected_hash)
{
    char actual_hash[HODL_VIEW_CACHE_HASH_MAX];
    if (!expected_hash || !expected_hash[0])
        return false; // raw-return-ok:cache-key-miss
    hodl_view_tip_hash(db, tip, actual_hash);
    return strcmp(actual_hash, expected_hash) == 0;
}

static bool hodl_view_cache_get_verified(sqlite3 *db, const char *datadir_key,
                                         int64_t tip,
                                         struct hodl_wave_snapshot *out,
                                         bool *cached_snapshot)
{
    bool hit = false;
    struct hodl_view_cache_entry local;
    memset(&local, 0, sizeof(local));

    pthread_mutex_lock(&g_hodl_view_cache_lock);
    if (g_hodl_view_cache.valid &&
        strcmp(g_hodl_view_cache.datadir, datadir_key) == 0 &&
        g_hodl_view_cache.tip_height <= tip) {
        local = g_hodl_view_cache;
        hit = true;
    }
    pthread_mutex_unlock(&g_hodl_view_cache_lock);
    if (!hit)
        return false;
    if (!hodl_view_hash_matches(db, local.tip_height, local.tip_hash))
        return false; // raw-return-ok:stale-cache-rejected

    *out = local.snapshot;
    if (cached_snapshot)
        *cached_snapshot = local.tip_height != tip;
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

#ifdef ZCL_TESTING
void explorer_test_reset_hodl_view_cache(void)
{
    pthread_mutex_lock(&g_hodl_view_cache_lock);
    memset(&g_hodl_view_cache, 0, sizeof(g_hodl_view_cache));
    pthread_mutex_unlock(&g_hodl_view_cache_lock);
}

bool explorer_test_hodl_view_refresh_active(void)
{
    bool active;
    pthread_mutex_lock(&g_hodl_view_refresh_lock);
    active = g_hodl_view_refresh_active;
    pthread_mutex_unlock(&g_hodl_view_refresh_lock);
    return active;
}
#endif

static bool hodl_view_disk_cache_paths(
    const char *datadir,
    char path[HODL_VIEW_DISK_CACHE_PATH_MAX],
    char tmp_path[HODL_VIEW_DISK_CACHE_PATH_MAX])
{
    char dir[HODL_VIEW_DISK_CACHE_PATH_MAX];
    int n;

    if (!datadir || !path)
        return false;

    n = snprintf(dir, sizeof(dir), "%s/explorer", datadir);
    if (n < 0 || (size_t)n >= sizeof(dir))
        return false;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return false;

    n = snprintf(path, HODL_VIEW_DISK_CACHE_PATH_MAX, "%s/%s",
                 dir, HODL_VIEW_DISK_CACHE_FILE);
    if (n < 0 || n >= HODL_VIEW_DISK_CACHE_PATH_MAX)
        return false;
    if (tmp_path) {
        n = snprintf(tmp_path, HODL_VIEW_DISK_CACHE_PATH_MAX,
                     "%s/%s.tmp.%ld", dir, HODL_VIEW_DISK_CACHE_FILE,
                     (long)getpid());
        if (n < 0 || n >= HODL_VIEW_DISK_CACHE_PATH_MAX)
            return false;
    }
    return true;
}

static void hodl_view_snapshot_base(struct hodl_wave_snapshot *out,
                                    int64_t tip)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->tip_height = tip;
    memcpy(out->buckets, hodl_wave_bucket_defs(), sizeof(out->buckets));
    snprintf(out->source, sizeof(out->source),
             "current_transparent_utxo_set");
    snprintf(out->metric, sizeof(out->metric), "utxo_age_distribution");
    snprintf(out->status, sizeof(out->status), "ok");
}

static bool hodl_view_disk_cache_read(
    const char *datadir,
    struct hodl_wave_snapshot *out,
    char cached_hash[HODL_VIEW_CACHE_HASH_MAX])
{
    char path[HODL_VIEW_DISK_CACHE_PATH_MAX];
    char line[256];
    int64_t cached_tip = 0;
    int64_t total_value = 0;
    int64_t total_count = 0;
    int64_t skipped_rows = 0;
    int64_t older_value = 0;
    int64_t older_count = 0;
    FILE *f;

    if (!out || !cached_hash ||
        !hodl_view_disk_cache_paths(datadir, path, NULL))
        return false; // raw-return-ok:cache-miss-not-error
    cached_hash[0] = '\0';

    f = fopen(path, "r");
    if (!f)
        return false;
    bool ok = false;
    if (!fgets(line, sizeof(line), f) ||
        strncmp(line, HODL_VIEW_DISK_CACHE_MAGIC,
                strlen(HODL_VIEW_DISK_CACHE_MAGIC)) != 0)
        goto done;
    if (!fgets(line, sizeof(line), f) ||
        sscanf(line, "tip_height=%" SCNd64, &cached_tip) != 1 ||
        cached_tip < 0)
        goto done;
    if (!fgets(line, sizeof(line), f) ||
        sscanf(line, "tip_hash=%79s", cached_hash) != 1 ||
        !cached_hash[0])
        goto done;
    if (!fgets(line, sizeof(line), f) ||
        sscanf(line, "total_value=%" SCNd64, &total_value) != 1)
        goto done;
    if (!fgets(line, sizeof(line), f) ||
        sscanf(line, "total_count=%" SCNd64, &total_count) != 1)
        goto done;
    if (!fgets(line, sizeof(line), f) ||
        sscanf(line, "skipped_rows=%" SCNd64, &skipped_rows) != 1)
        goto done;
    if (!fgets(line, sizeof(line), f) ||
        sscanf(line, "older_than_1y_value=%" SCNd64, &older_value) != 1)
        goto done;
    if (!fgets(line, sizeof(line), f) ||
        sscanf(line, "older_than_1y_count=%" SCNd64, &older_count) != 1)
        goto done;

    hodl_view_snapshot_base(out, cached_tip);
    out->total_value = total_value;
    out->total_count = total_count;
    out->skipped_rows = skipped_rows;
    out->older_than_1y_value = older_value;
    out->older_than_1y_count = older_count;

    for (int i = 0; i < HODL_WAVE_BUCKETS; i++) {
        int idx = -1;
        int64_t value = 0;
        int64_t count = 0;
        if (!fgets(line, sizeof(line), f) ||
            sscanf(line, "bucket %d value=%" SCNd64 " count=%" SCNd64,
                   &idx, &value, &count) != 3 ||
            idx != i || value < 0 || count < 0)
            goto done;
        out->buckets[i].value = value;
        out->buckets[i].count = count;
    }

    struct ar_errors errors;
    ok = hodl_wave_validate(out, &errors);
done:
    fclose(f);
    return ok;
}

static bool hodl_view_disk_cache_load_verified(
    const char *datadir, sqlite3 *db, int64_t tip,
    struct hodl_wave_snapshot *out,
    char cached_hash[HODL_VIEW_CACHE_HASH_MAX],
    bool *cached_snapshot)
{
    if (!hodl_view_disk_cache_read(datadir, out, cached_hash))
        return false; // raw-return-ok:cache-miss-not-error
    if (out->tip_height > tip)
        return false;
    if (!hodl_view_hash_matches(db, out->tip_height, cached_hash))
        return false; // raw-return-ok:stale-cache-rejected
    if (cached_snapshot)
        *cached_snapshot = out->tip_height != tip;
    return true;
}

static void hodl_view_disk_cache_save(const char *datadir, int64_t tip,
                                      const char *tip_hash,
                                      const struct hodl_wave_snapshot *h)
{
    char path[HODL_VIEW_DISK_CACHE_PATH_MAX];
    char tmp_path[HODL_VIEW_DISK_CACHE_PATH_MAX];
    FILE *f;
    bool ok = false;

    if (!h || !tip_hash || !tip_hash[0] ||
        !hodl_view_disk_cache_paths(datadir, path, tmp_path))
        return;

    f = fopen(tmp_path, "w");
    if (!f) {
        LOG_WARN("explorer", "hodl disk cache fopen(%s) failed: %s",
                 tmp_path, strerror(errno));
        return;
    }

    ok = fprintf(f, "%s\n", HODL_VIEW_DISK_CACHE_MAGIC) > 0 &&
         fprintf(f, "tip_height=%" PRId64 "\n", tip) > 0 &&
         fprintf(f, "tip_hash=%s\n", tip_hash) > 0 &&
         fprintf(f, "total_value=%" PRId64 "\n", h->total_value) > 0 &&
         fprintf(f, "total_count=%" PRId64 "\n", h->total_count) > 0 &&
         fprintf(f, "skipped_rows=%" PRId64 "\n", h->skipped_rows) > 0 &&
         fprintf(f, "older_than_1y_value=%" PRId64 "\n",
                 h->older_than_1y_value) > 0 &&
         fprintf(f, "older_than_1y_count=%" PRId64 "\n",
                 h->older_than_1y_count) > 0;
    for (int i = 0; ok && i < HODL_WAVE_BUCKETS; i++) {
        ok = fprintf(f, "bucket %d value=%" PRId64 " count=%" PRId64 "\n",
                     i, h->buckets[i].value, h->buckets[i].count) > 0;
    }
    ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0)
        ok = false;
    if (!ok) {
        LOG_WARN("explorer", "hodl disk cache write(%s) failed: %s",
                 tmp_path, strerror(errno));
        unlink(tmp_path);
        return;
    }
    if (rename(tmp_path, path) != 0) {
        LOG_WARN("explorer", "hodl disk cache rename(%s -> %s) failed: %s",
                 tmp_path, path, strerror(errno));
        unlink(tmp_path);
    }
}

static bool hodl_view_allow_sync_scan(const char *datadir)
{
    char dbpath[HODL_VIEW_DISK_CACHE_PATH_MAX];
    struct stat st;
    int n;

    if (!datadir)
        return true;
    n = snprintf(dbpath, sizeof(dbpath), "%s/node.db", datadir);
    if (n < 0 || (size_t)n >= sizeof(dbpath))
        return false;
    if (stat(dbpath, &st) != 0)
        return true;
    return st.st_size <= HODL_VIEW_SYNC_SCAN_DB_BYTES_MAX;
}

static void hodl_view_refresh_mark_done(void)
{
    pthread_mutex_lock(&g_hodl_view_refresh_lock);
    g_hodl_view_refresh_active = false;
    pthread_mutex_unlock(&g_hodl_view_refresh_lock);
}

static void *hodl_view_refresh_thread(void *arg)
{
    struct hodl_view_refresh_task *task = arg;
    sqlite3 *db = NULL;
    struct hodl_wave_snapshot hodl;
    bool ok = false;

    if (!task) {
        hodl_view_refresh_mark_done();
        return NULL;
    }

    if (explorer_open_readonly_db(task->datadir, &db)) {
        ok = hodl_wave_scan_current_utxos(db, task->tip_height, &hodl);
        sqlite3_close(db);
    } else {
        memset(&hodl, 0, sizeof(hodl));
        snprintf(hodl.status, sizeof(hodl.status),
                 "sqlite database unavailable");
    }

    if (ok) {
        hodl_view_cache_put(task->datadir_key, task->tip_height,
                            task->tip_hash, &hodl);
        hodl_view_disk_cache_save(task->datadir, task->tip_height,
                                  task->tip_hash, &hodl);
    } else {
        LOG_WARN("explorer", "hodl background refresh failed at height %" PRId64
                 ": %s", task->tip_height,
                 hodl.status[0] ? hodl.status : "unknown error");
    }

    free(task);
    hodl_view_refresh_mark_done();
    return NULL;
}

static bool hodl_view_refresh_start(const char *datadir,
                                    const char *datadir_key,
                                    int64_t tip,
                                    const char *tip_hash)
{
    int rc;
    struct hodl_view_refresh_task *task;

    if (!datadir || !datadir_key || !tip_hash || !tip_hash[0] || tip < 1)
        return false;

    pthread_mutex_lock(&g_hodl_view_refresh_lock);
    if (g_hodl_view_refresh_active) {
        pthread_mutex_unlock(&g_hodl_view_refresh_lock);
        return false;
    }
    g_hodl_view_refresh_active = true;
    pthread_mutex_unlock(&g_hodl_view_refresh_lock);

    task = zcl_calloc(1, sizeof(*task), "hodl_view_refresh_task");
    if (!task) {
        hodl_view_refresh_mark_done();
        return false;
    }
    snprintf(task->datadir, sizeof(task->datadir), "%s", datadir);
    snprintf(task->datadir_key, sizeof(task->datadir_key), "%s", datadir_key);
    task->tip_height = tip;
    snprintf(task->tip_hash, sizeof(task->tip_hash), "%s", tip_hash);

    rc = thread_registry_spawn("zcl_hodl_ref", hodl_view_refresh_thread, task, NULL);
    if (rc != 0) {
        LOG_WARN("explorer",
                 "hodl background refresh thread_registry_spawn failed: %d",
                 rc);
        free(task);
        hodl_view_refresh_mark_done();
        return false;
    }
    return true;
}

static int64_t hodl_cum_at(const struct hodl_cum_row *cum, int n,
                           int64_t target_height)
{
    int lo = 0;
    int hi = n - 1;
    int found = -1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (cum[mid].height <= target_height) {
            found = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return found >= 0 ? cum[found].cumulative_zat : 0;
}

static int64_t hodl_height_for_age(int64_t sample_height,
                                   int64_t age_seconds)
{
    if (age_seconds <= 0)
        return sample_height;
    if (sample_height >= HODL_BUTTERCUP_HEIGHT) {
        int64_t post_span =
            (sample_height - HODL_BUTTERCUP_HEIGHT) *
            HODL_POST_BUTTERCUP_SPACING;
        if (age_seconds <= post_span)
            return sample_height - age_seconds / HODL_POST_BUTTERCUP_SPACING;
        return HODL_BUTTERCUP_HEIGHT -
               (age_seconds - post_span) / HODL_PRE_BUTTERCUP_SPACING;
    }
    return sample_height - age_seconds / HODL_PRE_BUTTERCUP_SPACING;
}

static int64_t hodl_estimated_block_time(int64_t height)
{
    int64_t pre = height < HODL_BUTTERCUP_HEIGHT
        ? height : HODL_BUTTERCUP_HEIGHT;
    int64_t post = height < HODL_BUTTERCUP_HEIGHT
        ? 0 : height - HODL_BUTTERCUP_HEIGHT;
    return HODL_GENESIS_TIME +
           pre * HODL_PRE_BUTTERCUP_SPACING +
           post * HODL_POST_BUTTERCUP_SPACING;
}

static int64_t hodl_block_time_lookup(sqlite3_stmt *stmt, int64_t height)
{
    int64_t out = 0;

    if (!stmt)
        return hodl_estimated_block_time(height);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int64(stmt, 1, height);
    if (AR_STEP_ROW_READONLY(stmt) == SQLITE_ROW)
        out = sqlite3_column_int64(stmt, 0);
    if (out <= 0)
        out = hodl_estimated_block_time(height);
    return out;
}

static bool hodl_survival_cache_get(const char *datadir_key,
                                    int64_t tip_height,
                                    const char *tip_hash,
                                    struct hodl_survival_row *rows,
                                    int *row_count,
                                    bool *history_backed)
{
    bool hit = false;

    if (!datadir_key || !tip_hash || !rows || !row_count)
        return false;
    pthread_mutex_lock(&g_hodl_survival_cache_lock);
    if (g_hodl_survival_cache.valid &&
        g_hodl_survival_cache.tip_height == tip_height &&
        strcmp(g_hodl_survival_cache.datadir, datadir_key) == 0 &&
        strcmp(g_hodl_survival_cache.tip_hash, tip_hash) == 0 &&
        g_hodl_survival_cache.row_count > 0 &&
        g_hodl_survival_cache.row_count <= HODL_SURVIVAL_MAX_ROWS) {
        *row_count = g_hodl_survival_cache.row_count;
        if (history_backed)
            *history_backed = g_hodl_survival_cache.history_backed;
        memcpy(rows, g_hodl_survival_cache.rows,
               (size_t)*row_count * sizeof(*rows));
        hit = true;
    }
    pthread_mutex_unlock(&g_hodl_survival_cache_lock);
    return hit;
}

static void hodl_survival_cache_put(const char *datadir_key,
                                    int64_t tip_height,
                                    const char *tip_hash,
                                    const struct hodl_survival_row *rows,
                                    int row_count,
                                    bool history_backed)
{
    if (!datadir_key || !tip_hash || !rows || row_count <= 0 ||
        row_count > HODL_SURVIVAL_MAX_ROWS)
        return;
    pthread_mutex_lock(&g_hodl_survival_cache_lock);
    snprintf(g_hodl_survival_cache.datadir,
             sizeof(g_hodl_survival_cache.datadir), "%s", datadir_key);
    snprintf(g_hodl_survival_cache.tip_hash,
             sizeof(g_hodl_survival_cache.tip_hash), "%s", tip_hash);
    g_hodl_survival_cache.tip_height = tip_height;
    g_hodl_survival_cache.row_count = row_count;
    g_hodl_survival_cache.history_backed = history_backed;
    memcpy(g_hodl_survival_cache.rows, rows,
           (size_t)row_count * sizeof(*rows));
    g_hodl_survival_cache.valid = true;
    pthread_mutex_unlock(&g_hodl_survival_cache_lock);
}

static int hodl_pct_x1000(int64_t older_zat, int64_t total_zat)
{
    int pct = 0;
    if (older_zat < 0)
        older_zat = 0;
    if (older_zat > total_zat)
        older_zat = total_zat;
    if (total_zat > 0)
        pct = (int)((double)older_zat / (double)total_zat * 100000.0 + 0.5);
    if (pct < 0)
        pct = 0;
    if (pct > 100000)
        pct = 100000;
    return pct;
}

static int64_t hodl_current_threshold_zat(const struct hodl_wave_snapshot *h,
                                          int threshold)
{
    int first_bucket = 0;
    int64_t total = 0;

    if (!h)
        return 0;
    switch (threshold) {
    case HODL_SURVIVAL_THRESHOLD_6M:
        first_bucket = 5;  /* 6 - 12m and older */
        break;
    case HODL_SURVIVAL_THRESHOLD_1Y:
        return h->older_than_1y_value;
    case HODL_SURVIVAL_THRESHOLD_2Y:
        first_bucket = 7;  /* 2 - 3y and older */
        break;
    case HODL_SURVIVAL_THRESHOLD_5Y:
        first_bucket = 9;  /* > 5y */
        break;
    default:
        return 0;
    }
    for (int i = first_bucket; i < HODL_WAVE_BUCKETS; i++)
        total += h->buckets[i].value;
    return total;
}

static void hodl_history_row_to_wave(const struct hodl_history_row *hist,
                                     struct hodl_survival_row *out)
{
    if (!hist || !out)
        return;
    memset(out, 0, sizeof(*out));
    out->height = hist->height;
    out->time = hist->time;
    out->total_zat = hist->total_zat;
    out->older_zat[HODL_SURVIVAL_THRESHOLD_6M] = hist->older_6m_zat;
    out->older_zat[HODL_SURVIVAL_THRESHOLD_1Y] = hist->older_1y_zat;
    out->older_zat[HODL_SURVIVAL_THRESHOLD_2Y] = hist->older_2y_zat;
    out->older_zat[HODL_SURVIVAL_THRESHOLD_5Y] = hist->older_5y_zat;
    out->pct_x1000[HODL_SURVIVAL_THRESHOLD_6M] =
        hodl_pct_x1000(hist->older_6m_zat, hist->total_zat);
    out->pct_x1000[HODL_SURVIVAL_THRESHOLD_1Y] =
        hodl_pct_x1000(hist->older_1y_zat, hist->total_zat);
    out->pct_x1000[HODL_SURVIVAL_THRESHOLD_2Y] =
        hodl_pct_x1000(hist->older_2y_zat, hist->total_zat);
    out->pct_x1000[HODL_SURVIVAL_THRESHOLD_5Y] =
        hodl_pct_x1000(hist->older_5y_zat, hist->total_zat);
}

static bool hodl_load_history_wave_rows(sqlite3 *db,
                                        const struct hodl_wave_snapshot *hodl,
                                        struct hodl_survival_row *rows,
                                        int *row_count)
{
    struct hodl_history_row *hist;
    int hist_n;
    int n = 0;

    if (!db || !hodl || !rows || !row_count)
        return false;
    hist = zcl_calloc(HODL_SURVIVAL_MAX_ROWS, sizeof(*hist),
                      "hodl_history_wave_rows");
    if (!hist)
        return false;
    hist_n = hodl_history_load_all(db, hist, HODL_SURVIVAL_MAX_ROWS - 1);
    for (int i = 0; i < hist_n && n < HODL_SURVIVAL_MAX_ROWS - 1; i++) {
        if (hist[i].height < 1 || hist[i].height > hodl->tip_height ||
            hist[i].time <= 0 || hist[i].total_zat <= 0)
            continue;
        hodl_history_row_to_wave(&hist[i], &rows[n]);
        n++;
    }
    free(hist);
    if (n == 0)
        return false;
    if (rows[n - 1].height < hodl->tip_height &&
        n < HODL_SURVIVAL_MAX_ROWS) {
        rows[n].height = hodl->tip_height;
        rows[n].time = hodl_estimated_block_time(hodl->tip_height);
        rows[n].total_zat = hodl->total_value;
        for (int t = 0; t < HODL_SURVIVAL_THRESHOLDS; t++) {
            int64_t older = hodl_current_threshold_zat(hodl, t);
            rows[n].older_zat[t] = older;
            rows[n].pct_x1000[t] = hodl_pct_x1000(older, hodl->total_value);
        }
        n++;
    }
    *row_count = n;
    return n >= 2;
}

static bool hodl_history_wave_rows_graphable(
    const struct hodl_survival_row *rows, int row_count)
{
    int rows_with_signal = 0;

    if (!rows || row_count < 2)
        return false;
    for (int i = 0; i < row_count; i++) {
        for (int t = 0; t < HODL_SURVIVAL_THRESHOLDS; t++) {
            if (rows[i].pct_x1000[t] > 0) {
                rows_with_signal++;
                break;
            }
        }
    }
    return rows_with_signal >= 2;
}

static bool hodl_build_survival_rows(sqlite3 *db,
                                     const struct hodl_wave_snapshot *hodl,
                                     struct hodl_survival_row *rows,
                                     int *row_count)
{
    struct hodl_cum_row *cum = NULL;
    sqlite3_stmt *scan = NULL;
    sqlite3_stmt *time_lookup = NULL;
    int n_cum = 0;
    int n_rows = 0;
    int64_t running = 0;
    int64_t stride = HODL_HISTORY_SAMPLE_STRIDE;

    if (!db || !hodl || !rows || !row_count || hodl->tip_height < 1)
        return false;

    cum = zcl_calloc(HODL_CUM_MAX_ROWS, sizeof(*cum), "hodl_survival_cum");
    if (!cum)
        return false;

    if (sqlite3_prepare_v2(db,
            "SELECT height, SUM(value) FROM utxos "
            "WHERE value > 0 AND height >= 0 AND height <= ?1 "
            "GROUP BY height ORDER BY height ASC",
            -1, &scan, NULL) != SQLITE_OK || !scan) {
        free(cum);
        return false;
    }
    sqlite3_bind_int64(scan, 1, hodl->tip_height);
    while (AR_STEP_ROW_READONLY(scan) == SQLITE_ROW) {
        if (n_cum >= HODL_CUM_MAX_ROWS) {
            sqlite3_finalize(scan);
            free(cum);
            return false;
        }
        int64_t h = sqlite3_column_int64(scan, 0);
        int64_t v = sqlite3_column_int64(scan, 1);
        if (h < 0 || v <= 0)
            continue;
        running += v;
        cum[n_cum].height = h;
        cum[n_cum].cumulative_zat = running;
        n_cum++;
    }
    sqlite3_finalize(scan);
    if (n_cum < 2 || running <= 0) {
        free(cum);
        return false;
    }

    int64_t estimated_rows = hodl->tip_height / stride + 2;
    if (estimated_rows > HODL_SURVIVAL_MAX_ROWS - 1) {
        int64_t mul = (estimated_rows + HODL_SURVIVAL_MAX_ROWS - 2) /
                      (HODL_SURVIVAL_MAX_ROWS - 1);
        if (mul > 1)
            stride *= mul;
    }

    if (sqlite3_prepare_v2(db,
            "SELECT time FROM blocks WHERE height=?1 LIMIT 1",
            -1, &time_lookup, NULL) != SQLITE_OK) {
        time_lookup = NULL;
    }

    for (int64_t h = stride;
         h <= hodl->tip_height && n_rows < HODL_SURVIVAL_MAX_ROWS - 1;
         h += stride) {
        int64_t total = hodl_cum_at(cum, n_cum, h);
        if (total <= 0)
            continue;
        rows[n_rows].height = h;
        rows[n_rows].time = hodl_block_time_lookup(time_lookup, h);
        rows[n_rows].total_zat = total;
        for (int t = 0; t < HODL_SURVIVAL_THRESHOLDS; t++) {
            int64_t boundary =
                hodl_height_for_age(h, HODL_THRESHOLDS[t].age_seconds);
            int64_t older = hodl_cum_at(cum, n_cum, boundary);
            rows[n_rows].older_zat[t] = older;
            rows[n_rows].pct_x1000[t] = hodl_pct_x1000(older, total);
        }
        n_rows++;
    }

    if (n_rows == 0 || rows[n_rows - 1].height < hodl->tip_height) {
        int idx = n_rows;
        if (idx < HODL_SURVIVAL_MAX_ROWS) {
            int64_t total = hodl->total_value > 0
                ? hodl->total_value : hodl_cum_at(cum, n_cum, hodl->tip_height);
            rows[idx].height = hodl->tip_height;
            rows[idx].time = hodl_block_time_lookup(time_lookup,
                                                    hodl->tip_height);
            rows[idx].total_zat = total;
            for (int t = 0; t < HODL_SURVIVAL_THRESHOLDS; t++) {
                int64_t boundary = hodl_height_for_age(
                    hodl->tip_height, HODL_THRESHOLDS[t].age_seconds);
                int64_t older = hodl_cum_at(cum, n_cum, boundary);
                rows[idx].older_zat[t] = older;
                rows[idx].pct_x1000[t] = hodl_pct_x1000(older, total);
            }
            n_rows++;
        }
    }

    if (time_lookup)
        sqlite3_finalize(time_lookup);
    free(cum);
    *row_count = n_rows;
    return n_rows >= 2;
}

static int hodl_survival_x(const struct hodl_survival_row *row,
                           int pl, int pw, int64_t h_min, int64_t h_max)
{
    if (!row || h_max <= h_min)
        return pl;
    return pl + (int)((double)(row->height - h_min) /
                      (double)(h_max - h_min) * pw);
}

static int hodl_survival_y(const struct hodl_survival_row *row,
                           int threshold, int pt, int ph)
{
    int pct = row ? row->pct_x1000[threshold] : 0;
    if (pct < 0)
        pct = 0;
    if (pct > 100000)
        pct = 100000;
    return pt + ph - (int)((double)pct / 100000.0 * ph);
}

static void hodl_emit_survival_chart(size_t *off, uint8_t *r, size_t max,
                                     const char *datadir,
                                     const char *datadir_key,
                                     const struct hodl_wave_snapshot *hodl)
{
    sqlite3 *db = NULL;
    struct hodl_survival_row *rows = NULL;
    struct hodl_survival_row *fallback_rows = NULL;
    int n = 0;
    char tip_hash[HODL_VIEW_CACHE_HASH_MAX];
    bool history_backed = false;
    bool have_history_rows = false;
    bool have_fallback_rows = false;

    if (!off || !r || !datadir || !datadir_key || !hodl)
        return;
    rows = zcl_calloc(HODL_SURVIVAL_MAX_ROWS, sizeof(*rows),
                      "hodl_survival_rows");
    if (!rows)
        return;
    if (!explorer_open_readonly_db(datadir, &db)) {
        free(rows);
        return;
    }
    hodl_view_tip_hash(db, hodl->tip_height, tip_hash);
    have_history_rows = hodl_load_history_wave_rows(db, hodl, rows, &n);
    if (have_history_rows && hodl_history_wave_rows_graphable(rows, n)) {
        history_backed = true;
        hodl_survival_cache_put(datadir_key, hodl->tip_height, tip_hash,
                                rows, n, true);
    } else {
        history_backed = false;
        fallback_rows = zcl_calloc(HODL_SURVIVAL_MAX_ROWS,
                                   sizeof(*fallback_rows),
                                   "hodl_survival_fallback_rows");
        if (fallback_rows) {
            bool cached_history = false;
            int fallback_n = 0;
            have_fallback_rows =
                hodl_survival_cache_get(datadir_key, hodl->tip_height,
                                        tip_hash, fallback_rows, &fallback_n,
                                        &cached_history) && !cached_history;
            if (!have_fallback_rows &&
                hodl_build_survival_rows(db, hodl, fallback_rows,
                                         &fallback_n)) {
                hodl_survival_cache_put(datadir_key, hodl->tip_height,
                                        tip_hash, fallback_rows,
                                        fallback_n, false);
                have_fallback_rows = true;
            }
            if (have_fallback_rows) {
                memcpy(rows, fallback_rows,
                       (size_t)fallback_n * sizeof(*fallback_rows));
                n = fallback_n;
            }
            free(fallback_rows);
        }
        if (!have_fallback_rows && !have_history_rows)
            n = 0;
        if (!have_fallback_rows && have_history_rows)
            history_backed = true;
    }
    sqlite3_close(db);

    if (n < 2) {
        free(rows);
        return;
    }

    int W = 1000;
    int H = 410;
    int pl = 72;
    int pr = 134;
    int pt = 26;
    int pb = 74;
    int pw = W - pl - pr;
    int ph = H - pt - pb;
    int64_t h_min = rows[0].height;
    int64_t h_max = rows[n - 1].height;
    if (h_max <= h_min)
        h_max = h_min + 1;

    char sample_meta[96];
    snprintf(sample_meta, sizeof(sample_meta), "%d %s samples", n,
             history_backed ? "verified" : "cohort");
    const char *chart_title = history_backed
        ? "Historical HODL wave"
        : "HODL wave over time";
    const char *chart_subtitle = history_backed
        ? "Share of transparent UTXO value alive at each sample and older than each threshold"
        : "Current unspent transparent value grouped by the age each coin would have at past dates";
    const char *chart_source = history_backed
        ? "historical UTXO snapshots"
        : "current surviving transparent UTXO set while history backfills";

    APPEND(*off, r, max,
        "<section id='hodl-survival-wrap' "
        "class='hodl-chart-wrap hodl-wave-interactive'>"
        "<div class='hodl-chart-head'><div>"
        "<h2 class='hodl-chart-title'>%s</h2>"
        "<p class='hodl-chart-subtitle'>%s</p></div>"
        "<div class='hodl-chart-meta'>%s</div></div>"
        "<div class='hodl-legend' aria-label='Holding thresholds'>",
        chart_title, chart_subtitle, sample_meta);

    for (int t = 0; t < HODL_SURVIVAL_THRESHOLDS; t++) {
        APPEND(*off, r, max,
            "<span class='hodl-legend-item'><span class='hodl-legend-swatch' "
            "style='background:%s'></span>%s</span>",
            HODL_THRESHOLDS[t].color, HODL_THRESHOLDS[t].label);
    }

    APPEND(*off, r, max, "</div><div class='hodl-current-strip'>");
    for (int t = 0; t < HODL_SURVIVAL_THRESHOLDS; t++) {
        double pct = (double)rows[n - 1].pct_x1000[t] / 1000.0;
        APPEND(*off, r, max,
            "<div class='hodl-current-chip' style='--c:%s'>"
            "<span>%s current</span><strong>%.1f%%</strong></div>",
            HODL_THRESHOLDS[t].color, HODL_THRESHOLDS[t].label, pct);
    }

    APPEND(*off, r, max,
        "</div><div id='hodl-survival-canvas' class='hodl-chart-canvas'>"
        "<svg id='hodl-survival-wave' viewBox='0 0 %d %d' "
        "class='hodl-svg hodl-survival-svg' tabindex='0' role='img' "
        "aria-label='HODL Wave over time for 6 month, 1 year, 2 year, "
        "and 5 year holding thresholds.' style='outline:none'>"
        "<defs>"
        "<linearGradient id='hodl-survival-bg' x1='0' y1='0' x2='0' y2='1'>"
        "<stop offset='0%%' stop-color='#15212a'/>"
        "<stop offset='100%%' stop-color='#071117'/>"
        "</linearGradient>"
        "<linearGradient id='hodl-survival-area' x1='0' y1='0' x2='0' y2='1'>"
        "<stop offset='0%%' stop-color='#35d07f' stop-opacity='0.24'/>"
        "<stop offset='72%%' stop-color='#35d07f' stop-opacity='0.08'/>"
        "<stop offset='100%%' stop-color='#35d07f' stop-opacity='0.01'/>"
        "</linearGradient>"
        "<filter id='hodl-line-glow' x='-4%%' y='-8%%' width='108%%' height='116%%'>"
        "<feGaussianBlur stdDeviation='0.75' result='blur'/>"
        "<feMerge><feMergeNode in='blur'/><feMergeNode in='SourceGraphic'/></feMerge>"
        "</filter>"
        "</defs>",
        W, H);

    APPEND(*off, r, max,
        "<rect x='%d' y='%d' width='%d' height='%d' rx='7' "
        "fill='url(#hodl-survival-bg)' stroke='#314655'/>",
        pl, pt, pw, ph);
    APPEND(*off, r, max,
        "<text class='hodl-axis-title' x='%d' y='%d' "
        "text-anchor='middle'>Share of transparent value</text>"
        "<text class='hodl-axis-title' x='%d' y='%d' "
        "text-anchor='middle'>Block height</text>",
        pl + pw / 2, pt - 9, pl + pw / 2, pt + ph + 48);

    for (int g = 0; g <= 4; g++) {
        int pct = g * 25;
        int y = pt + ph - ph * g / 4;
        APPEND(*off, r, max,
            "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#253642' "
            "opacity='0.56'/>"
            "<text x='%d' y='%d' fill='#8d96a0' font-size='12' "
            "text-anchor='end'>%d%%</text>",
            pl, y, pl + pw, y, pl - 8, y + 4, pct);
    }

    for (int g = 0; g <= 4; g++) {
        int64_t hval = h_min + (h_max - h_min) * g / 4;
        int x = pl + pw * g / 4;
        time_t tt = (time_t)hodl_estimated_block_time(hval);
        struct tm tm_;
        char dbuf[16];
        gmtime_r(&tt, &tm_);
        strftime(dbuf, sizeof(dbuf), "%Y-%m", &tm_);
        APPEND(*off, r, max,
            "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#1b2b35' "
            "opacity='0.46'/>"
            "<text class='hodl-x-tick' x='%d' y='%d' fill='#8d96a0' font-size='12' "
            "text-anchor='middle'>%s</text>",
            x, pt, x, pt + ph, x, pt + ph + 20, dbuf);
    }

    APPEND(*off, r, max,
        "<path d='M%d,%d", hodl_survival_x(&rows[0], pl, pw, h_min, h_max),
        pt + ph);
    for (int i = 0; i < n; i++) {
        int x = hodl_survival_x(&rows[i], pl, pw, h_min, h_max);
        int y = hodl_survival_y(&rows[i], 1, pt, ph);
        APPEND(*off, r, max, " L%d,%d", x, y);
    }
    APPEND(*off, r, max, " L%d,%d Z' fill='url(#hodl-survival-area)'/>",
           hodl_survival_x(&rows[n - 1], pl, pw, h_min, h_max), pt + ph);

    for (int t = 0; t < HODL_SURVIVAL_THRESHOLDS; t++) {
        APPEND(*off, r, max,
            "<polyline fill='none' stroke='%s' stroke-width='%d' "
            "stroke-linejoin='round' stroke-linecap='round' opacity='0.96' "
            "filter='url(#hodl-line-glow)' class='hodl-series-line' "
            "vector-effect='non-scaling-stroke' points='",
            HODL_THRESHOLDS[t].color, t == 1 ? 4 : 3);
        for (int i = 0; i < n; i++) {
            int x = hodl_survival_x(&rows[i], pl, pw, h_min, h_max);
            int y = hodl_survival_y(&rows[i], t, pt, ph);
            APPEND(*off, r, max, "%s%d,%d", i ? " " : "", x, y);
        }
        APPEND(*off, r, max, "'/>");
    }

    for (int t = 0; t < HODL_SURVIVAL_THRESHOLDS; t++) {
        int x = hodl_survival_x(&rows[n - 1], pl, pw, h_min, h_max);
        int y = hodl_survival_y(&rows[n - 1], t, pt, ph);
        APPEND(*off, r, max,
            "<circle class='hodl-hover-dot' cx='%d' cy='%d' r='%d' fill='%s' stroke='#071015' "
            "stroke-width='2'/>",
            x, y, t == 1 ? 5 : 4, HODL_THRESHOLDS[t].color);
    }

    int label_y[HODL_SURVIVAL_THRESHOLDS];
    int label_x = pl + pw + 14;
    int label_top = pt + 12;
    int label_bottom = pt + ph - 8;
    int last_y = label_top - 17;
    for (int t = 0; t < HODL_SURVIVAL_THRESHOLDS; t++) {
        int y = hodl_survival_y(&rows[n - 1], t, pt, ph);
        if (y < last_y + 17)
            y = last_y + 17;
        label_y[t] = y;
        last_y = y;
    }
    if (last_y > label_bottom) {
        int shift = last_y - label_bottom;
        for (int t = 0; t < HODL_SURVIVAL_THRESHOLDS; t++) {
            label_y[t] -= shift;
            if (label_y[t] < label_top)
                label_y[t] = label_top;
        }
    }
    for (int t = 0; t < HODL_SURVIVAL_THRESHOLDS; t++) {
        int x = hodl_survival_x(&rows[n - 1], pl, pw, h_min, h_max);
        int y = hodl_survival_y(&rows[n - 1], t, pt, ph);
        double pct = (double)rows[n - 1].pct_x1000[t] / 1000.0;
        APPEND(*off, r, max,
            "<path d='M%d,%d L%d,%d L%d,%d' fill='none' stroke='%s' "
            "stroke-width='1.2' opacity='0.68'/>"
            "<text class='hodl-end-label' x='%d' y='%d' fill='%s' "
            "font-size='12'>%s %.1f%%</text>",
            x + 6, y, label_x - 8, label_y[t], label_x - 2, label_y[t],
            HODL_THRESHOLDS[t].color,
            label_x, label_y[t] + 4, HODL_THRESHOLDS[t].color,
            HODL_THRESHOLDS[t].label, pct);
    }

    APPEND(*off, r, max,
        "<line id='hodl-survival-xhair' x1='0' y1='%d' x2='0' y2='%d' "
        "stroke='#f5f7fa' stroke-dasharray='2,4' stroke-width='1' "
        "opacity='0.62' style='display:none;pointer-events:none'/>",
        pt, pt + ph);
    for (int t = 0; t < HODL_SURVIVAL_THRESHOLDS; t++) {
        APPEND(*off, r, max,
            "<circle id='hodl-survival-dot-%d' class='hodl-hover-dot' "
            "cx='0' cy='0' r='4' "
            "fill='%s' stroke='#050505' stroke-width='1' "
            "style='display:none;pointer-events:none'/>",
            t, HODL_THRESHOLDS[t].color);
    }
    APPEND(*off, r, max,
        "<g id='hodl-survival-tip' style='display:none;pointer-events:none'>"
        "<rect id='hodl-survival-tip-bg' x='0' y='0' width='330' "
        "height='150' rx='8' fill='#060b10' stroke='#6ee7b7' opacity='0.97'/>"
        "<text id='hodl-survival-date' x='12' y='22' fill='#fff' "
        "font-size='13'>-</text>"
        "<text id='hodl-survival-total' x='12' y='43' fill='#aaa' "
        "font-size='12'>-</text>"
        "<text id='hodl-survival-h' x='12' y='62' fill='#666' "
        "font-size='11'>-</text>");
    for (int t = 0; t < HODL_SURVIVAL_THRESHOLDS; t++) {
        int y = 86 + t * 17;
        APPEND(*off, r, max,
            "<rect x='12' y='%d' width='10' height='10' rx='2' fill='%s'/>"
            "<text id='hodl-survival-row-%d' x='30' y='%d' fill='#ddd' "
            "font-size='12'>-</text>",
            y - 9, HODL_THRESHOLDS[t].color, t, y);
    }
    APPEND(*off, r, max,
        "</g>"
        "<text x='%d' y='%d' fill='#566371' font-size='11' "
        "text-anchor='end'>"
        "Source: %s</text>"
        "</svg></div></section>"
        "<script>(function(){"
        "var data=[",
        W - 30, H - 21, chart_source);
    for (int i = 0; i < n; i++) {
        APPEND(*off, r, max,
            "%s[%" PRId64 ",%" PRId64 ",%" PRId64,
            i ? "," : "", rows[i].height, rows[i].time, rows[i].total_zat);
        for (int t = 0; t < HODL_SURVIVAL_THRESHOLDS; t++)
            APPEND(*off, r, max, ",%d", rows[i].pct_x1000[t]);
        for (int t = 0; t < HODL_SURVIVAL_THRESHOLDS; t++)
            APPEND(*off, r, max, ",%" PRId64, rows[i].older_zat[t]);
        APPEND(*off, r, max, "]");
    }
    APPEND(*off, r, max, "];var series=[");
    for (int t = 0; t < HODL_SURVIVAL_THRESHOLDS; t++) {
        APPEND(*off, r, max, "%s['%s','%s']",
               t ? "," : "", HODL_THRESHOLDS[t].label,
               HODL_THRESHOLDS[t].color);
    }
    APPEND(*off, r, max,
        "];var W=%d,pl=%d,pr=%d,pt=%d,pb=%d,ph=%d,pw=W-pl-pr;"
        "var hmin=%" PRId64 ",hmax=%" PRId64 ";"
        "var svg=document.getElementById('hodl-survival-wave');"
        "var wrap=document.getElementById('hodl-survival-canvas');"
        "var xhair=document.getElementById('hodl-survival-xhair');"
        "var tip=document.getElementById('hodl-survival-tip');"
        "var tipBg=document.getElementById('hodl-survival-tip-bg');"
        "var td=document.getElementById('hodl-survival-date');"
        "var tt=document.getElementById('hodl-survival-total');"
        "var th=document.getElementById('hodl-survival-h');"
        "var dots=series.map(function(_,i){return document.getElementById('hodl-survival-dot-'+i);});"
        "var rows=series.map(function(_,i){return document.getElementById('hodl-survival-row-'+i);});"
        "var TIPW=330,TIPH=150,cur=-1;"
        "tipBg.setAttribute('width',TIPW);tipBg.setAttribute('height',TIPH);"
        "function fmtZcl(z){var v=z/1e8;if(v>=1e6)return(v/1e6).toFixed(2)+'M';"
        "if(v>=1e3)return(v/1e3).toFixed(2)+'k';return v.toFixed(2);}"
        "function fmtDate(t){return new Date(t*1000).toISOString().slice(0,10);}"
        "function maybeScrollLatest(){if(!wrap||!window.matchMedia)return;"
        "if(wrap.scrollWidth>wrap.clientWidth+16&&"
        "window.matchMedia('(max-width: 640px)').matches)"
        "wrap.scrollLeft=wrap.scrollWidth;}"
        "function hide(){xhair.style.display='none';tip.style.display='none';"
        "dots.forEach(function(d){if(d)d.style.display='none';});}"
        "function pickNearest(svgX){var frac=(svgX-pl)/pw;"
        "var target=hmin+frac*(hmax-hmin);var lo=0,hi=data.length-1;"
        "while(lo<hi){var m=(lo+hi)>>1;if(data[m][0]<target)lo=m+1;else hi=m;}"
        "if(lo>0&&Math.abs(data[lo-1][0]-target)<Math.abs(data[lo][0]-target))lo--;"
        "return lo;}"
        "function yFor(p){return Math.round(pt+ph-(p/100000)*ph);}"
        "function render(svgX){if(svgX<pl)svgX=pl;if(svgX>W-pr)svgX=W-pr;"
        "var i=pickNearest(svgX);cur=i;var row=data[i];"
        "var x=Math.round(pl+(row[0]-hmin)/(hmax-hmin)*pw);"
        "xhair.setAttribute('x1',x);xhair.setAttribute('x2',x);"
        "xhair.style.display='';"
        "for(var s=0;s<series.length;s++){var y=yFor(row[3+s]);"
        "if(dots[s]){dots[s].setAttribute('cx',x);dots[s].setAttribute('cy',y);"
        "dots[s].style.display='';}}"
        "var tx=x+14;if(tx+TIPW>W-pr)tx=x-TIPW-14;if(tx<pl)tx=pl;"
        "var ty=pt+12;if(ty+TIPH>pt+ph)ty=pt+ph-TIPH;"
        "tip.setAttribute('transform','translate('+tx+','+ty+')');"
        "tip.style.display='';td.textContent=fmtDate(row[1]);"
        "tt.textContent='Current surviving total then: '+fmtZcl(row[2])+' ZCL';"
        "th.textContent='Block '+row[0].toLocaleString();"
        "for(var s=0;s<series.length;s++){var p=row[3+s]/1000;"
        "var z=row[3+series.length+s];"
        "rows[s].textContent=series[s][0]+': '+p.toFixed(2)+'%%, '+fmtZcl(z)+' ZCL';}"
        "}"
        "var pend=null,raf=0;"
        "function sched(x){pend=x;if(!raf)raf=requestAnimationFrame(function(){"
        "raf=0;if(pend!=null)render(pend);});}"
        "function pt2svg(clientX){var rc=svg.getBoundingClientRect();"
        "return (clientX-rc.left)*(W/rc.width);}"
        "svg.addEventListener('mousemove',function(e){var sx=pt2svg(e.clientX);"
        "if(sx<pl||sx>W-pr){hide();return;}sched(sx);});"
        "svg.addEventListener('mouseleave',hide);"
        "function onTouch(e){if(!e.touches[0])return;var sx=pt2svg(e.touches[0].clientX);"
        "if(sx>=pl&&sx<=W-pr)sched(sx);}"
        "svg.addEventListener('touchstart',onTouch,{passive:true});"
        "svg.addEventListener('touchmove',onTouch,{passive:true});"
        "svg.addEventListener('touchend',hide);svg.addEventListener('touchcancel',hide);"
        "svg.addEventListener('focus',function(){render(W-pr);});"
        "svg.addEventListener('blur',hide);"
        "svg.addEventListener('keydown',function(e){var k=e.key,i=cur<0?data.length-1:cur;"
        "if(k==='ArrowLeft')i=Math.max(0,i-1);else if(k==='ArrowRight')i=Math.min(data.length-1,i+1);"
        "else if(k==='Home')i=0;else if(k==='End')i=data.length-1;"
        "else if(k==='Escape'){hide();return;}else return;e.preventDefault();"
        "render(pl+(data[i][0]-hmin)/(hmax-hmin)*pw);});"
        "requestAnimationFrame(maybeScrollLatest);"
        "})();</script>",
        W, pl, pr, pt, pb, ph, h_min, h_max);

    free(rows);
}

static void hodl_emit_age_distribution_chart(size_t *off, uint8_t *r,
                                             size_t max,
                                             const struct hodl_wave_snapshot *h,
                                             bool cached_snapshot)
{
    if (!off || !r || !h)
        return;

    int selected = 0;
    for (int i = 1; i < HODL_WAVE_BUCKETS; i++) {
        if (h->buckets[i].value > h->buckets[selected].value)
            selected = i;
    }

    int W = 1000;
    int H = 408;
    APPEND(*off, r, max,
        "<section id='hodl-age-wrap' class='hodl-chart-wrap hodl-wave-interactive'>"
        "<div class='hodl-chart-head'><div>"
        "<h2 class='hodl-chart-title'>Unspent transparent value by age</h2>"
        "<p class='hodl-chart-subtitle'>Current transparent UTXO value distribution.</p>"
        "</div></div>"
        "<div id='hodl-age-canvas' class='hodl-chart-canvas'>"
        "<svg id='hodl-age-wave' viewBox='0 0 %d %d' class='hodl-svg hodl-age-svg' "
        "tabindex='0' role='img' "
        "aria-label='Interactive unspent transparent value by age. "
        "Use mouse, touch, or arrow keys to inspect each age band.'>"
        "<defs>"
        "<linearGradient id='hodl-age-bg' x1='0' y1='0' x2='0' y2='1'>"
        "<stop offset='0%%' stop-color='#15212a'/>"
        "<stop offset='100%%' stop-color='#071117'/>"
        "</linearGradient>"
        "<linearGradient id='hodl-age-glow' x1='0' y1='0' x2='0' y2='1'>"
        "<stop offset='0%%' stop-color='#35d07f' stop-opacity='0.18'/>"
        "<stop offset='100%%' stop-color='#35d07f' stop-opacity='0.02'/>"
        "</linearGradient>"
        "</defs>",
        W, H);

    int x0 = 76, y0 = 304, chart_w = 850, chart_h = 230;
    APPEND(*off, r, max,
        "<rect x='%d' y='%d' width='%d' height='%d' rx='7' "
        "fill='url(#hodl-age-bg)' stroke='#314655'/>",
        x0, y0 - chart_h, chart_w, chart_h);
    APPEND(*off, r, max,
        "<text class='hodl-axis-title' x='%d' y='%d' "
        "text-anchor='middle'>Share of transparent value</text>",
        x0 + chart_w / 2, y0 - chart_h - 31);
    for (int g = 0; g <= 4; g++) {
        int y = y0 - chart_h * g / 4;
        APPEND(*off, r, max,
            "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#253642' "
            "opacity='0.56'/>"
            "<text x='%d' y='%d' fill='#8d96a0' font-size='12' "
            "text-anchor='end'>%d%%</text>",
            x0, y, x0 + chart_w, y, x0 - 8, y + 4, g * 25);
    }

    int bar_gap = 10;
    int bar_w = (chart_w - bar_gap * (HODL_WAVE_BUCKETS - 1)) /
                HODL_WAVE_BUCKETS;
    int threshold_x = x0 + 6 * (bar_w + bar_gap) - bar_gap / 2;
    APPEND(*off, r, max,
        "<rect x='%d' y='%d' width='%d' height='%d' fill='#35d07f' "
        "opacity='0.055'/>"
        "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='#35d07f' "
        "stroke-width='1.2' stroke-dasharray='4,4' opacity='0.72'/>"
        "<text x='%d' y='%d' fill='#8fffc3' font-size='11' "
        "text-anchor='middle'>1 year+</text>",
        threshold_x, y0 - chart_h, x0 + chart_w - threshold_x, chart_h,
        threshold_x, y0 - chart_h, threshold_x, y0,
        threshold_x, y0 - chart_h - 8);

    APPEND(*off, r, max,
        "<path d='M%d,%d", x0, y0);
    for (int b = 0; b < HODL_WAVE_BUCKETS; b++) {
        double pct = h->total_value > 0
            ? (double)h->buckets[b].value / (double)h->total_value * 100.0
            : 0.0;
        int bh = (int)(pct / 100.0 * chart_h);
        int x = x0 + b * (bar_w + bar_gap) + bar_w / 2;
        int y = y0 - bh;
        APPEND(*off, r, max, " L%d,%d", x, y);
    }
    APPEND(*off, r, max,
        " L%d,%d Z' fill='url(#hodl-age-glow)' stroke='#35d07f' "
        "stroke-width='1' opacity='0.72'/>",
        x0 + chart_w, y0);

    for (int b = 0; b < HODL_WAVE_BUCKETS; b++) {
        double pct = h->total_value > 0
            ? (double)h->buckets[b].value / (double)h->total_value * 100.0
            : 0.0;
        int bh = (int)(pct / 100.0 * chart_h);
        int draw_h = bh > 3 ? bh : 3;
        int x = x0 + b * (bar_w + bar_gap);
        int y = y0 - draw_h;
        char val_fmt[64];
        zcl_format_zcl(val_fmt, sizeof(val_fmt), h->buckets[b].value);
        APPEND(*off, r, max,
            "<rect id='hodl-age-bar-%d' class='hodl-age-bar' "
            "x='%d' y='%d' width='%d' height='%d' fill='%s' rx='4' "
            "opacity='%s' stroke='%s' stroke-width='%d'/>"
            "<rect class='hodl-age-hit' data-i='%d' tabindex='-1' "
            "x='%d' y='%d' width='%d' height='%d' fill='transparent' "
            "aria-label='%s: %.3f%%, %s ZCL, %" PRId64 " UTXOs'>"
            "<title>%s: %.3f%%, %s ZCL, %" PRId64 " UTXOs</title>"
            "</rect>"
            "<text x='%d' y='%d' fill='#aaa' font-size='11' "
            "text-anchor='middle' transform='rotate(-35,%d,%d)'>%s</text>",
            b, x, y, bar_w, draw_h, h->buckets[b].color,
            b == selected ? "1" : "0.82",
            b == selected ? "#fff" : h->buckets[b].color,
            b == selected ? 2 : 1,
            b, x, y0 - chart_h, bar_w, chart_h,
            h->buckets[b].html_label, pct, val_fmt, h->buckets[b].count,
            h->buckets[b].html_label, pct, val_fmt, h->buckets[b].count,
            x + bar_w / 2, y0 + 26, x + bar_w / 2, y0 + 26,
            h->buckets[b].html_label);
        if (pct >= 1.0 || b == selected) {
            APPEND(*off, r, max,
                "<text x='%d' y='%d' fill='#eee' font-size='12' "
                "text-anchor='middle'>%.2f%%</text>",
                x + bar_w / 2, y - 6, pct);
        }
    }

    APPEND(*off, r, max,
        "<g id='hodl-age-tip' style='display:none;pointer-events:none'>"
        "<rect id='hodl-age-tip-bg' x='0' y='0' width='300' height='102' "
        "rx='8' fill='#060b10' stroke='#6ee7b7' opacity='0.97'/>"
        "<text id='hodl-age-tip-title' x='20' y='29' fill='#fff' "
        "font-size='18'>-</text>"
        "<text id='hodl-age-tip-pct' x='20' y='54' fill='#33ff99' "
        "font-size='18' font-weight='700'>-</text>"
        "<text id='hodl-age-tip-value' x='20' y='75' fill='#bbb' "
        "font-size='13'>-</text>"
        "<text id='hodl-age-tip-count' x='20' y='93' fill='#8d96a0' "
        "font-size='12'>-</text>"
        "</g>"
        "<text x='%d' y='%d' fill='#4d5662' font-size='11' text-anchor='end'>"
        "Source: %s</text></svg></div></section>"
        "<script>(function(){"
        "var svg=document.getElementById('hodl-age-wave');"
        "if(!svg)return;"
        "var wrap=document.getElementById('hodl-age-canvas');"
        "var data=[",
        W - 30, H - 17,
        cached_snapshot ? "verified cached transparent UTXO set" :
                          "current transparent UTXO set");
    for (int b = 0; b < HODL_WAVE_BUCKETS; b++) {
        double pct = h->total_value > 0
            ? (double)h->buckets[b].value / (double)h->total_value * 100.0
            : 0.0;
        APPEND(*off, r, max,
            "%s{label:'%s',pct:%d,value:%" PRId64 ",count:%" PRId64 "}",
            b ? "," : "", h->buckets[b].label, (int)(pct * 1000.0),
            h->buckets[b].value, h->buckets[b].count);
    }
    APPEND(*off, r, max,
        "];"
        "var selected=%d;"
        "var tip=document.getElementById('hodl-age-tip');"
        "var title=document.getElementById('hodl-age-tip-title');"
        "var pct=document.getElementById('hodl-age-tip-pct');"
        "var value=document.getElementById('hodl-age-tip-value');"
        "var count=document.getElementById('hodl-age-tip-count');"
        "function fmtZcl(z){var v=z/1e8;"
        "if(v>=1e6)return(v/1e6).toFixed(3)+'M ZCL';"
        "if(v>=1e3)return(v/1e3).toFixed(3)+'k ZCL';"
        "return v.toFixed(8)+' ZCL';}"
        "function fmtCount(n){return n.toLocaleString()+' UTXOs';}"
        "function maybeScrollSelected(){if(!wrap||!window.matchMedia)return;"
        "if(wrap.scrollWidth>wrap.clientWidth+16&&selected>=data.length/2&&"
        "window.matchMedia('(max-width: 640px)').matches)"
        "wrap.scrollLeft=wrap.scrollWidth;}"
        "function setBar(i,on){var b=document.getElementById('hodl-age-bar-'+i);"
        "if(!b)return;b.setAttribute('opacity',on?'1':'0.82');"
        "b.setAttribute('stroke',on?'#fff':b.getAttribute('fill'));"
        "b.setAttribute('stroke-width',on?'2':'1');}"
        "function render(i){if(i<0)i=0;if(i>=data.length)i=data.length-1;"
        "setBar(selected,false);selected=i;setBar(selected,true);"
        "var d=data[i];title.textContent=d.label;"
        "pct.textContent=(d.pct/1000).toFixed(3)+'%% of transparent value';"
        "value.textContent=fmtZcl(d.value);"
        "count.textContent=fmtCount(d.count);"
        "var x=i<4?620:150;tip.setAttribute('transform','translate('+x+',76)');"
        "tip.style.display='';}"
        "function hide(){tip.style.display='none';}"
        "svg.querySelectorAll('.hodl-age-hit').forEach(function(el){"
        "el.addEventListener('mouseenter',function(){render(+el.dataset.i);});"
        "el.addEventListener('mousemove',function(){render(+el.dataset.i);});"
        "el.addEventListener('touchstart',function(e){render(+el.dataset.i);"
        "},{passive:true});"
        "el.addEventListener('touchmove',function(e){render(+el.dataset.i);"
        "},{passive:true});"
        "});"
        "svg.addEventListener('mouseleave',hide);"
        "svg.addEventListener('touchend',hide);svg.addEventListener('touchcancel',hide);"
        "svg.addEventListener('focus',function(){render(selected);});"
        "svg.addEventListener('blur',hide);"
        "svg.addEventListener('keydown',function(e){"
        "var k=e.key,i=selected;"
        "if(k==='ArrowLeft')i--;else if(k==='ArrowRight')i++;"
        "else if(k==='Home')i=0;else if(k==='End')i=data.length-1;"
        "else if(k==='Escape'){hide();return;}"
        "else return;e.preventDefault();render(i);});"
        "hide();"
        "requestAnimationFrame(maybeScrollSelected);"
        "})();</script>",
        selected);
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
    char cached_hash[HODL_VIEW_CACHE_HASH_MAX];
    bool cacheable = hodl_view_datadir_key(datadir, datadir_key);
    bool cached_snapshot = false;
    bool warming_snapshot = false;
    hodl_view_tip_hash(db, tip, tip_hash);
    bool ok = cacheable &&
        hodl_view_cache_get_verified(db, datadir_key, tip, &hodl,
                                     &cached_snapshot);
    if (!ok && cacheable) {
        ok = hodl_view_disk_cache_load_verified(datadir, db, tip, &hodl,
                                                cached_hash,
                                                &cached_snapshot);
        if (ok)
            hodl_view_cache_put(datadir_key, hodl.tip_height, cached_hash,
                                &hodl);
    }
    if (ok && cached_snapshot && cacheable)
        (void)hodl_view_refresh_start(datadir, datadir_key, tip, tip_hash);
    if (!ok && cacheable && !hodl_view_allow_sync_scan(datadir)) {
        hodl_view_snapshot_base(&hodl, tip);
        snprintf(hodl.status, sizeof(hodl.status),
                 "Building the verified HODL snapshot in the background");
        warming_snapshot = true;
        (void)hodl_view_refresh_start(datadir, datadir_key, tip, tip_hash);
    } else if (!ok) {
        ok = hodl_wave_scan_current_utxos(db, tip, &hodl);
        if (ok && cacheable) {
            hodl_view_cache_put(datadir_key, tip, tip_hash, &hodl);
            hodl_view_disk_cache_save(datadir, tip, tip_hash, &hodl);
        }
    }
    sqlite3_close(db);

    struct template_var open_vars[] = {
        { "css_base", HODL_VIEW_CSS_BASE },
        { "css_detail", HODL_VIEW_CSS_DETAIL },
    };
    hodl_append_template(&off, r, max, HODL_PAGE_OPEN_TEMPLATE,
                         open_vars, sizeof(open_vars) / sizeof(open_vars[0]));
    off += explorer_emit_nav((char *)r + off, max - off, "hodl");
    APPEND(off, r, max, "<main class='hodl-page'>");

    if (!ok) {
        struct template_var degraded_vars[] = {
            { "status", hodl.status },
        };
        hodl_append_template(&off, r, max,
                             warming_snapshot ? HODL_WARMING_TEMPLATE :
                             HODL_DEGRADED_TEMPLATE,
                             degraded_vars,
                             sizeof(degraded_vars) /
                             sizeof(degraded_vars[0]));
        APPEND(off, r, max, EXPLORER_FOOTER);
        return off;
    }

    double older_pct = hodl_wave_older_than_1y_percent(&hodl);
    struct hodl_page_model page_model;
    memset(&page_model, 0, sizeof(page_model));
    hodl_page_model_init(&page_model, &hodl, older_pct, cached_snapshot);

    struct template_var hero_vars[] = {
        { "older_pct", page_model.older_pct },
        { "hero_subject", page_model.hero_subject },
        { "snapshot_label", page_model.snapshot_label },
        { "tip_height", page_model.tip_height },
    };
    hodl_append_template(&off, r, max, HODL_HERO_TEMPLATE,
                         hero_vars, sizeof(hero_vars) / sizeof(hero_vars[0]));
    if (cacheable)
        hodl_emit_survival_chart(&off, r, max, datadir, datadir_key, &hodl);
    hodl_emit_age_distribution_chart(&off, r, max, &hodl, cached_snapshot);

    struct template_var stats_vars[] = {
        { "total_value", page_model.total_value },
        { "value_label", page_model.value_label },
        { "older_value", page_model.older_value },
        { "total_count", page_model.total_count },
        { "skipped_rows", page_model.skipped_rows },
    };
    hodl_append_template(&off, r, max, HODL_STATS_TEMPLATE,
                         stats_vars,
                         sizeof(stats_vars) / sizeof(stats_vars[0]));

    APPEND(off, r, max,
        "<div class='hodl-table-wrap'>"
        "<table class='txlist hodl-table'>"
        "<tr><th>Age</th><th>UTXOs</th><th>Value</th><th>Share</th></tr>");
    for (int b = 0; b < HODL_WAVE_BUCKETS; b++) {
        char val_fmt[64];
        char count_fmt[32];
        char pct_fmt[32];
        zcl_format_zcl(val_fmt, sizeof(val_fmt), hodl.buckets[b].value);
        double pct = hodl.total_value > 0
            ? (double)hodl.buckets[b].value / (double)hodl.total_value * 100.0 : 0.0;
        snprintf(count_fmt, sizeof(count_fmt), "%" PRId64,
                 hodl.buckets[b].count);
        snprintf(pct_fmt, sizeof(pct_fmt), "%.3f", pct);
        struct template_var row_vars[] = {
            { "color", hodl.buckets[b].color },
            { "age", hodl.buckets[b].html_label },
            { "count", count_fmt },
            { "value", val_fmt },
            { "share", pct_fmt },
        };
        hodl_append_template(&off, r, max, HODL_TABLE_ROW_TEMPLATE,
                             row_vars,
                             sizeof(row_vars) / sizeof(row_vars[0]));
    }
    APPEND(off, r, max, "</table></div>");
    APPEND(off, r, max,
        "<p class='hodl-source'>"
        "Source: %s. Metric: UTXO age distribution."
        "</p></main>" EXPLORER_FOOTER,
        cached_snapshot ? "verified cached transparent UTXO set" :
                          "current transparent UTXO set");
    return off;
}
