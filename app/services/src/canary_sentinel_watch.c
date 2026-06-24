/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * canary_sentinel_watch — see services/canary_sentinel_watch.h for the
 * contract (sentinel format, absence/staleness policy). This file is a pure
 * file-scan observer: no DB writes, no threads, no chain locks. */

// one-result-type-ok:canary-watch-observer-no-fallible-surface
//
// This is a read-only sentinel observer, not a fallible service executor.
// Its surfaces are: a void tick (absence of dir/files is a documented quiet
// no-op, never an error a caller branches on), bool predicates/probes
// (fail_active, resolve_dir), and the project-wide bool dump_state_json
// convention. Failure detail travels via the Condition's LOG_WARN + the
// dump, not a result object.

#include "services/canary_sentinel_watch.h"

#include "framework/condition.h"
#include "json/json.h"
#include "platform/time_compat.h"
#include "util/clientversion.h"
#include "util/log_macros.h"

#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define CANARY_WATCH_MAX_KINDS     8
#define CANARY_KIND_NAME_MAX       32
#define CANARY_VERDICT_MAX         16
#define CANARY_REASON_MAX          96
#define CANARY_COMMIT_MAX          24
/* A sentinel is one flat JSON object (~400 bytes); anything larger than this
 * is not a canary verdict and is treated as unreadable. */
#define CANARY_SENTINEL_MAX_BYTES  8192

#define CANARY_SENTINEL_PREFIX     "replay_canary_"
#define CANARY_SENTINEL_SUFFIX     ".json"

struct canary_kind_slot {
    bool    used;
    bool    fail;                       /* latched: latest verdict == FAIL */
    char    kind[CANARY_KIND_NAME_MAX];
    char    from[CANARY_KIND_NAME_MAX]; /* sentinel's own `from` — display only */
    char    verdict[CANARY_VERDICT_MAX];
    char    reason[CANARY_REASON_MAX];
    char    build_commit[CANARY_COMMIT_MAX]; /* sentinel's own build_commit */
    bool    stale_build;                /* verdict==FAIL but a DIFFERENT build
                                         * than the one running → not our page */
    bool    stale_run;                  /* verdict==FAIL but the run started
                                         * before this process → not our page */
    int64_t ts;                         /* sentinel's own write-time epoch */
    int64_t started_ts;                 /* sentinel's own run-start epoch */
    int64_t parse_fail_logged_mtime;    /* log-once-per-mtime for torn JSON */
    int64_t stale_build_logged_mtime;   /* log-once-per-mtime for stale-build */
    int64_t stale_run_logged_mtime;     /* log-once-per-mtime for stale-run */
};

static struct {
    pthread_mutex_t lock;
    struct canary_kind_slot slots[CANARY_WATCH_MAX_KINDS];
    _Atomic int64_t last_scan_unix;
    _Atomic int64_t scans_total;
    _Atomic int64_t files_seen_last;
    _Atomic int64_t parse_failures_logged;
    _Atomic int64_t process_start_unix;
    _Atomic bool    fail_latched;
} g_watch = { .lock = PTHREAD_MUTEX_INITIALIZER };

__attribute__((constructor))
static void canary_sentinel_watch_init_process_start(void)
{
    int64_t now = platform_time_wall_unix();
    if (now > 0)
        atomic_store(&g_watch.process_start_unix, now);
}

bool canary_sentinel_watch_resolve_dir(char *out, size_t cap)
{
    if (!out || cap == 0)
        return false;
    out[0] = '\0';
    const char *env = getenv("ZCL_CANARY_VERDICT_DIR");
    if (env && env[0])
        return snprintf(out, cap, "%s", env) < (int)cap;
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return false; /* quiet: no env, no HOME — nothing to watch */
    return snprintf(out, cap, "%s/.local/state/zclassic23-canary", home) <
           (int)cap;
}

/* Find (or claim) the slot for `kind`. Caller holds g_watch.lock. */
static struct canary_kind_slot *slot_for_kind_locked(const char *kind)
{
    struct canary_kind_slot *free_slot = NULL;
    for (int i = 0; i < CANARY_WATCH_MAX_KINDS; i++) {
        struct canary_kind_slot *s = &g_watch.slots[i];
        if (s->used && strcmp(s->kind, kind) == 0)
            return s;
        if (!s->used && !free_slot)
            free_slot = s;
    }
    if (free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->used = true;
        snprintf(free_slot->kind, sizeof(free_slot->kind), "%s", kind);
    }
    return free_slot; /* NULL when all slots taken (logged by caller) */
}

/* "replay_canary_<kind>.json" → <kind> into out. False for non-sentinels
 * (wrong prefix/suffix, or an atomic-writer ".tmp." leftover). */
static bool kind_from_filename(const char *name, char *out, size_t cap)
{
    size_t prefix_len = strlen(CANARY_SENTINEL_PREFIX);
    size_t suffix_len = strlen(CANARY_SENTINEL_SUFFIX);
    size_t name_len = strlen(name);
    if (name_len <= prefix_len + suffix_len)
        return false;
    if (strncmp(name, CANARY_SENTINEL_PREFIX, prefix_len) != 0)
        return false;
    if (strcmp(name + name_len - suffix_len, CANARY_SENTINEL_SUFFIX) != 0)
        return false;
    if (strstr(name, ".tmp."))
        return false; /* in-flight atomic-writer temp, never a verdict */
    size_t kind_len = name_len - prefix_len - suffix_len;
    if (kind_len >= cap)
        kind_len = cap - 1;
    memcpy(out, name + prefix_len, kind_len);
    out[kind_len] = '\0';
    return true;
}

/* Read the whole sentinel into a caller buffer (it is a single small JSON
 * line). Returns false on any I/O trouble or oversize. */
static bool read_sentinel(const char *path, char *buf, size_t cap,
                          size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false; /* raced with the harness's rm/rename: not an error */
    size_t n = fread(buf, 1, cap - 1, f);
    bool more = fgetc(f) != EOF; /* oversize ⇒ not a canary sentinel */
    fclose(f);
    if (more)
        return false;
    buf[n] = '\0';
    *out_len = n;
    return true;
}

/* Compare two build-commit strings ignoring a trailing "-dirty" suffix and
 * case, mirroring tools/deploy_verify.sh norm_commit. The binary's
 * zcl_build_commit() carries "-dirty" on a dirty build (Makefile), while the
 * canary harness (tools/scripts/replay_canary.sh) writes the BARE
 * `git rev-parse --short HEAD`. Without this, a genuine SAME-SOURCE FAIL on a
 * dirty deploy (the dev-lane default) would be misread as cross-build and
 * silently dropped off the pager — weakening the operator gate. */
static size_t commit_base_len(const char *c)
{
    if (!c)
        return 0;
    const char *d = strstr(c, "-dirty");
    return d ? (size_t)(d - c) : strlen(c);
}

static bool commit_same_source(const char *a, const char *b)
{
    size_t la = commit_base_len(a);
    size_t lb = commit_base_len(b);
    return la > 0 && la == lb && strncasecmp(a, b, la) == 0;
}

/* Parse one sentinel and fold its verdict into the kind slot. Corrupt/torn
 * JSON is skipped and logged once per file mtime — it NEVER raises alone. */
static void process_sentinel(const char *dir, const char *name)
{
    char kind[CANARY_KIND_NAME_MAX];
    if (!kind_from_filename(name, kind, sizeof(kind)))
        return;

    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
        return;

    struct stat st;
    int64_t mtime = (stat(path, &st) == 0) ? (int64_t)st.st_mtime : 0;

    char raw[CANARY_SENTINEL_MAX_BYTES];
    size_t raw_len = 0;
    bool readable = read_sentinel(path, raw, sizeof(raw), &raw_len);

    struct json_value v;
    json_init(&v);
    bool parsed = readable && json_read(&v, raw, raw_len) &&
                  v.type == JSON_OBJ;

    pthread_mutex_lock(&g_watch.lock);
    struct canary_kind_slot *slot = slot_for_kind_locked(kind);
    if (!slot) {
        pthread_mutex_unlock(&g_watch.lock);
        json_free(&v);
        LOG_WARN("canary_watch", "[canary_watch] kind table full (%d), "
                 "ignoring sentinel %s", CANARY_WATCH_MAX_KINDS, name);
        return;
    }

    if (!parsed) {
        /* Keep the slot's previous verdict — a torn read is transport noise,
         * never a page and never a clear. Log once per mtime, not per tick. */
        bool log_it = slot->parse_fail_logged_mtime != mtime;
        if (log_it)
            slot->parse_fail_logged_mtime = mtime;
        pthread_mutex_unlock(&g_watch.lock);
        json_free(&v);
        if (log_it) {
            atomic_fetch_add(&g_watch.parse_failures_logged, 1);
            LOG_WARN("canary_watch", "[canary_watch] unreadable/corrupt "
                     "sentinel %s (mtime=%lld) — skipped, will not page",
                     name, (long long)mtime);
        }
        return;
    }

    const char *verdict = json_get_str(json_get(&v, "verdict"));
    const char *from    = json_get_str(json_get(&v, "from"));
    const char *reason  = json_get_str(json_get(&v, "reason"));
    const char *scommit = json_get_str(json_get(&v, "build_commit"));
    int64_t ts          = json_get_int(json_get(&v, "ts"));
    int64_t started_ts  = json_get_int(json_get(&v, "started_ts"));

    /* Cross-build staleness (2026-06-16): the verdict dir is SHARED across
     * lanes and restarts, so a sentinel written by a DIFFERENT binary than
     * the one now running is not evidence about THIS binary. A prior build's
     * FAIL (e.g. an old wedged binary's rpc_unreachable canary, build
     * 6934ad512) must NOT latch the pager on a freshly-deployed node. We
     * still record the verdict for display, but a cross-build FAIL does not
     * raise. A sentinel with no build_commit (older format) keeps the legacy
     * behavior; only a SAME-build FAIL pages. */
    const char *my_commit = zcl_build_commit();
    bool cross_build = scommit && scommit[0] && my_commit && my_commit[0] &&
                       !commit_same_source(scommit, my_commit);
    int64_t process_start =
        atomic_load_explicit(&g_watch.process_start_unix,
                             memory_order_relaxed);
    bool stale_run = started_ts > 0 && process_start > 0 &&
                     started_ts < process_start;
    bool is_fail = strcmp(verdict, "FAIL") == 0;

    snprintf(slot->verdict, sizeof(slot->verdict), "%s", verdict);
    snprintf(slot->reason, sizeof(slot->reason), "%s", reason);
    snprintf(slot->build_commit, sizeof(slot->build_commit), "%s", scommit);
    /* The slot key is the filename-derived kind, ALWAYS. Renaming the slot
     * from the sentinel's `from` field would orphan a FAILed slot if the
     * two ever disagreed (the PASS that should clear it would land in a
     * fresh slot, leaving an un-clearable page). `from` is recorded for
     * display only. */
    snprintf(slot->from, sizeof(slot->from), "%s", from);
    slot->ts = ts;
    slot->started_ts = started_ts;
    slot->stale_build = is_fail && cross_build;
    slot->stale_run = is_fail && stale_run;
    slot->fail = is_fail && !cross_build && !stale_run;
    slot->parse_fail_logged_mtime = 0; /* readable again: re-arm log-once */

    /* A dropped cross-build FAIL must never be a SILENT swallow (fail-loud
     * doctrine): say so once per mtime, with the build mismatch named. */
    bool log_stale_build = slot->stale_build &&
                           slot->stale_build_logged_mtime != mtime;
    bool log_stale_run = slot->stale_run &&
                         slot->stale_run_logged_mtime != mtime;
    char log_kind[CANARY_KIND_NAME_MAX] = {0};
    char log_reason[CANARY_REASON_MAX] = {0};
    char log_scommit[CANARY_COMMIT_MAX] = {0};
    int64_t log_started_ts = 0;
    int64_t log_process_start = 0;
    if (log_stale_build || log_stale_run) {
        if (log_stale_build)
            slot->stale_build_logged_mtime = mtime;
        if (log_stale_run)
            slot->stale_run_logged_mtime = mtime;
        snprintf(log_kind, sizeof(log_kind), "%s", slot->kind);
        snprintf(log_reason, sizeof(log_reason), "%s", slot->reason);
        snprintf(log_scommit, sizeof(log_scommit), "%s", slot->build_commit);
        log_started_ts = slot->started_ts;
        log_process_start = process_start;
    }
    pthread_mutex_unlock(&g_watch.lock);
    json_free(&v);

    if (log_stale_build)
        LOG_INFO("canary_watch", "[canary_watch] ignoring STALE cross-build "
                 "FAIL sentinel kind=%s build=%s (running=%s) reason=%s — not "
                 "paging", log_kind, log_scommit,
                 my_commit ? my_commit : "-", log_reason);
    if (log_stale_run)
        LOG_INFO("canary_watch", "[canary_watch] ignoring STALE pre-start "
                 "FAIL sentinel kind=%s started_ts=%lld process_start=%lld "
                 "reason=%s — not paging", log_kind,
                 (long long)log_started_ts, (long long)log_process_start,
                 log_reason);
}

/* Recompute the OR of all per-kind FAIL latches. */
static void recompute_latch(void)
{
    bool any_fail = false;
    pthread_mutex_lock(&g_watch.lock);
    for (int i = 0; i < CANARY_WATCH_MAX_KINDS; i++) {
        if (g_watch.slots[i].used && g_watch.slots[i].fail)
            any_fail = true;
    }
    pthread_mutex_unlock(&g_watch.lock);
    atomic_store(&g_watch.fail_latched, any_fail);
}

void canary_sentinel_watch_tick_once(void)
{
    atomic_store(&g_watch.last_scan_unix, platform_time_wall_unix());
    atomic_fetch_add(&g_watch.scans_total, 1);

    char dir[PATH_MAX];
    if (!canary_sentinel_watch_resolve_dir(dir, sizeof(dir))) {
        atomic_store(&g_watch.files_seen_last, 0);
        return; /* quiet: nowhere to look (no env, no HOME) */
    }

    DIR *d = opendir(dir);
    if (!d) {
        /* Fresh install / canary never ran: stay silent (no log spam, no
         * health impact). Absence never transitions the latch (header). */
        atomic_store(&g_watch.files_seen_last, 0);
        return;
    }

    int64_t files = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        char kind[CANARY_KIND_NAME_MAX];
        if (!kind_from_filename(ent->d_name, kind, sizeof(kind)))
            continue;
        process_sentinel(dir, ent->d_name);
        files++;
    }
    closedir(d);

    atomic_store(&g_watch.files_seen_last, files);
    recompute_latch();
}

bool canary_sentinel_watch_fail_active(void)
{
    return atomic_load(&g_watch.fail_latched);
}

int canary_sentinel_watch_fail_detail(char *out, size_t cap)
{
    int fails = 0;
    size_t pos = 0;
    if (out && cap > 0)
        out[0] = '\0';
    pthread_mutex_lock(&g_watch.lock);
    for (int i = 0; i < CANARY_WATCH_MAX_KINDS; i++) {
        const struct canary_kind_slot *s = &g_watch.slots[i];
        if (!s->used || !s->fail)
            continue;
        fails++;
        if (out && pos < cap) {
            int n = snprintf(out + pos, cap - pos,
                             "%skind=%s reason=%s ts=%lld",
                             pos ? "; " : "", s->kind,
                             s->reason[0] ? s->reason : "-",
                             (long long)s->ts);
            if (n > 0)
                pos += (size_t)n < cap - pos ? (size_t)n : cap - pos;
        }
    }
    pthread_mutex_unlock(&g_watch.lock);
    return fails;
}

/* ── State dump (see CLAUDE.md "Adding state introspection") ───── */

bool canary_watch_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;
    json_set_object(out);

    char dir[PATH_MAX];
    if (!canary_sentinel_watch_resolve_dir(dir, sizeof(dir)))
        dir[0] = '\0';
    json_push_kv_str(out, "verdict_dir", dir[0] ? dir : "unresolved");
    json_push_kv_int(out, "last_scan_unix",
                     atomic_load(&g_watch.last_scan_unix));
    json_push_kv_int(out, "scans_total", atomic_load(&g_watch.scans_total));
    json_push_kv_int(out, "files_seen_last",
                     atomic_load(&g_watch.files_seen_last));
    json_push_kv_int(out, "parse_failures_logged",
                     atomic_load(&g_watch.parse_failures_logged));
    json_push_kv_bool(out, "fail_latched",
                      atomic_load(&g_watch.fail_latched));

    /* The Condition engine's view of replay_canary_failed (the pager). */
    struct condition_runtime_snapshot snap;
    bool cond_active =
        condition_engine_get_registered_snapshot("replay_canary_failed",
                                                 &snap) &&
        snap.currently_active;
    json_push_kv_bool(out, "condition_active", cond_active);

    struct json_value arr;
    json_init(&arr);
    json_set_array(&arr);
    pthread_mutex_lock(&g_watch.lock);
    for (int i = 0; i < CANARY_WATCH_MAX_KINDS; i++) {
        const struct canary_kind_slot *s = &g_watch.slots[i];
        if (!s->used)
            continue;
        struct json_value obj;
        json_init(&obj);
        json_set_object(&obj);
        json_push_kv_str(&obj, "kind", s->kind);
        json_push_kv_str(&obj, "from", s->from[0] ? s->from : "-");
        json_push_kv_str(&obj, "verdict", s->verdict[0] ? s->verdict : "-");
        json_push_kv_str(&obj, "reason", s->reason);
        json_push_kv_str(&obj, "build_commit",
                         s->build_commit[0] ? s->build_commit : "-");
        json_push_kv_int(&obj, "ts", s->ts);
        json_push_kv_int(&obj, "started_ts", s->started_ts);
        json_push_kv_bool(&obj, "fail", s->fail);
        json_push_kv_bool(&obj, "stale_build", s->stale_build);
        json_push_kv_bool(&obj, "stale_run", s->stale_run);
        json_push_back(&arr, &obj);
        json_free(&obj);
    }
    pthread_mutex_unlock(&g_watch.lock);
    bool ok = json_push_kv(out, "kinds", &arr);
    json_free(&arr);
    return ok;
}

#ifdef ZCL_TESTING
void canary_sentinel_watch_test_reset(void)
{
    pthread_mutex_lock(&g_watch.lock);
    memset(g_watch.slots, 0, sizeof(g_watch.slots));
    pthread_mutex_unlock(&g_watch.lock);
    atomic_store(&g_watch.last_scan_unix, 0);
    atomic_store(&g_watch.scans_total, 0);
    atomic_store(&g_watch.files_seen_last, 0);
    atomic_store(&g_watch.parse_failures_logged, 0);
    atomic_store(&g_watch.fail_latched, false);
    canary_sentinel_watch_init_process_start();
}

void canary_sentinel_watch_test_set_process_start(int64_t start_unix)
{
    atomic_store(&g_watch.process_start_unix, start_unix);
}
#endif
