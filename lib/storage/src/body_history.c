/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * body_history — see storage/body_history.h for the design and, more
 * importantly, for why every failure path in this file lands on
 * BODY_HISTORY_UNKNOWN rather than on a zero missing count.
 *
 * one-result-type-ok:body-history-predicates — the bool returns here are
 * caller-consumed ANSWERS paired with an out-parameter that carries the
 * fail-closed UNKNOWN verdict, not error signals that need a code+message.
 * A false return always leaves *out at the pessimistic default, so a caller
 * that ignores the bool still cannot publish a cheerier state than the
 * truth. The fallible durable paths (save/load) route through LOG_FAIL. */

#include "storage/body_history.h"
#include "storage/progress_store.h"

#include "json/json.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/sync.h"

#include <string.h>
#include <stdlib.h>

const char *body_history_status_name(enum body_history_status s)
{
    switch (s) {
    case BODY_HISTORY_INCOMPLETE: return "incomplete";
    case BODY_HISTORY_COMPLETE:   return "complete";
    case BODY_HISTORY_UNKNOWN:    break;
    }
    /* Any value that is not positively one of the two established states —
     * including a corrupted enum — reads as "could not determine". */
    return "unknown";
}

/* Zero the verdict into the pessimistic default. Every early return in this
 * file goes through here first, so there is no path that leaves a caller's
 * verdict looking like a clean bill of health. */
static void bh_verdict_reset(struct body_history_verdict *v)
{
    if (!v)
        return;
    memset(v, 0, sizeof(*v));
    v->status = BODY_HISTORY_UNKNOWN;
    v->window_lo = -1;
    v->window_hi = -1;
    v->lowest_missing = -1;
    v->lowest_unmeasured = -1;
}

bool body_history_verdict_is_proven(const struct body_history_verdict *v)
{
    return v && v->status == BODY_HISTORY_COMPLETE;
}

bool body_history_evaluate(const struct body_coverage_map *held,
                           const struct body_coverage_map *measured,
                           int64_t lo, int64_t hi,
                           struct body_history_verdict *out)
{
    if (!out)
        return false;
    bh_verdict_reset(out);

    if (!held || !measured)
        return false;
    if (lo < 0 || lo > hi)
        return false;

    out->window_lo = lo;
    out->window_hi = hi;
    out->window_heights = hi - lo + 1;

    /* "Definitively probed" is measured OR held: the above-tip gap-fill feed
     * writes coverage without going through the census, and holding a body
     * is itself proof somebody looked. Union into scratch so neither input
     * map is mutated by a read-only question. */
    struct body_coverage_map probed;
    body_coverage_init(&probed);
    if (!body_coverage_union_into(&probed, measured) ||
        !body_coverage_union_into(&probed, held)) {
        body_coverage_free(&probed);
        /* Allocation failure: we genuinely do not know. Leave UNKNOWN. */
        bh_verdict_reset(out);
        return false;
    }

    int64_t held_in_w = body_coverage_covered_in_window(held, lo, hi);
    int64_t probed_in_w = body_coverage_covered_in_window(&probed, lo, hi);

    out->held_count = held_in_w;
    out->missing_count = probed_in_w - held_in_w;
    out->unmeasured_count = out->window_heights - probed_in_w;

    /* held is unioned into probed, so probed_in_w >= held_in_w and
     * probed_in_w <= window_heights by construction. A negative here would
     * mean the range algebra broke; refuse to publish a verdict over it. */
    if (out->missing_count < 0 || out->unmeasured_count < 0) {
        LOG_WARN("body_history",
                 "evaluate: inconsistent counts window=[%lld..%lld] "
                 "held=%lld probed=%lld — reporting unknown",
                 (long long)lo, (long long)hi,
                 (long long)held_in_w, (long long)probed_in_w);
        body_coverage_free(&probed);
        bh_verdict_reset(out);
        return false;
    }

    /* Lowest unmeasured: the first height in the window nobody has probed. */
    struct bc_range hole;
    if (body_coverage_find_first_hole(&probed, lo, hi, &hole))
        out->lowest_unmeasured = hole.lo;

    /* Lowest KNOWN missing: the first hole in `held` that falls inside a
     * probed range. Walking probed's ranges keeps this exact — a hole in
     * `held` that nobody probed is unmeasured, not missing. */
    for (size_t i = 0; i < probed.count && out->lowest_missing < 0; i++) {
        int64_t rlo = probed.ranges[i].lo;
        int64_t rhi = probed.ranges[i].hi;
        if (rhi < lo)
            continue;
        if (rlo > hi)
            break;
        if (rlo < lo) rlo = lo;
        if (rhi > hi) rhi = hi;
        struct bc_range mh;
        if (body_coverage_find_first_hole(held, rlo, rhi, &mh))
            out->lowest_missing = mh.lo;
    }

    body_coverage_free(&probed);

    /* Order matters and is deliberate:
     *   a positively-found hole is the strongest, most actionable fact;
     *   otherwise any unprobed height keeps us at UNKNOWN;
     *   COMPLETE requires BOTH "probed everything" and "held everything".
     * The verdict carries both counts, so INCOMPLETE never hides a
     * simultaneous unmeasured remainder. */
    if (out->missing_count > 0)
        out->status = BODY_HISTORY_INCOMPLETE;
    else if (out->unmeasured_count > 0)
        out->status = BODY_HISTORY_UNKNOWN;
    else
        out->status = BODY_HISTORY_COMPLETE;

    return true;
}

/* ── Bounded, resumable census ──────────────────────────────────── */

void body_history_census_init(struct body_history_census *c)
{
    if (!c)
        return;
    memset(c, 0, sizeof(*c));
    c->cursor = -1;
    c->cursor_valid = false;
    c->window_lo = -1;
    c->window_hi = -1;
    c->last_pass_lo = -1;
    c->last_pass_hi = -1;
}

bool body_history_census_plan(struct body_history_census *c,
                              int64_t window_lo, int64_t window_hi,
                              int64_t budget,
                              int64_t *out_lo, int64_t *out_hi)
{
    if (!c || !out_lo || !out_hi)
        return false;
    if (window_lo < 0 || window_lo > window_hi || budget <= 0)
        return false;

    /* Re-anchor when the window moved (the tip advanced) or on first use.
     * A grown window restarts the sweep from the new top; the measured map
     * is retained, so re-probing already-established heights is cheap and
     * is how a pruned or lost body gets noticed. */
    if (!c->cursor_valid || c->window_hi != window_hi ||
        c->window_lo != window_lo) {
        c->window_lo = window_lo;
        c->window_hi = window_hi;
        c->cursor = window_hi;
        c->cursor_valid = true;
    }
    if (c->cursor < window_lo || c->cursor > window_hi)
        c->cursor = window_hi;

    int64_t hi = c->cursor;
    int64_t lo = hi - budget + 1;
    if (lo < window_lo)
        lo = window_lo;

    *out_lo = lo;
    *out_hi = hi;
    return true;
}

void body_history_census_advance(struct body_history_census *c,
                                 int64_t lo, int64_t hi)
{
    if (!c || lo < 0 || lo > hi)
        return;
    c->passes++;
    c->last_pass_lo = lo;
    c->last_pass_hi = hi;
    c->cursor = lo - 1;
    if (c->cursor < c->window_lo) {
        c->sweeps_completed++;
        c->cursor = c->window_hi;
    }
    c->cursor_valid = true;
}

size_t body_history_census_probe_window(int64_t lo, int64_t hi,
                                        body_history_probe_fn probe,
                                        void *ctx,
                                        uint8_t *classes,
                                        struct uint256 *hashes,
                                        size_t cap)
{
    if (!classes || !hashes || cap == 0)
        return 0;

    /* Pre-fill INDETERMINATE (and zero the hashes) BEFORE anything can go
     * wrong. Every slot the probe does not positively answer therefore
     * stays unmeasured, which is the whole point of this module. */
    memset(classes, BODY_HISTORY_PROBE_INDETERMINATE, cap);
    memset(hashes, 0, cap * sizeof(*hashes));

    if (!probe || lo < 0 || lo > hi)
        return 0;

    int64_t span = hi - lo + 1;
    if (span > (int64_t)cap)
        span = (int64_t)cap;

    for (int64_t i = 0; i < span; i++) {
        enum body_history_probe r = probe(lo + i, &hashes[i], ctx);
        if (r == BODY_HISTORY_PROBE_HAVE || r == BODY_HISTORY_PROBE_MISSING)
            classes[i] = (uint8_t)r;
        /* Anything else — including a probe that returns a value outside
         * the enum — stays INDETERMINATE. */
    }
    return (size_t)span;
}

bool body_history_census_fold(struct body_history_census *c,
                              struct body_coverage_map *held,
                              struct body_coverage_map *measured,
                              int64_t lo,
                              const uint8_t *classes, size_t n,
                              struct body_history_pass_result *out)
{
    struct body_history_pass_result res;
    memset(&res, 0, sizeof(res));
    res.lo = -1;
    res.hi = -1;
    if (out)
        *out = res;

    if (!c || !held || !measured || !classes)
        LOG_FAIL("body_history", "fold: null census, map or classes");
    if (lo < 0 || n == 0)
        return true; /* nothing to fold; measured stays as it was */

    res.lo = lo;
    res.hi = lo + (int64_t)n - 1;

    /* Coalesce equal-classification runs into range inserts so a contiguous
     * chain costs a couple of inserts per pass, not one per height. */
    size_t i = 0;
    while (i < n) {
        uint8_t cls = classes[i];
        size_t j = i + 1;
        while (j < n && classes[j] == cls)
            j++;
        int64_t rlo = lo + (int64_t)i;
        int64_t rhi = lo + (int64_t)j - 1;
        int64_t run = rhi - rlo + 1;

        switch (cls) {
        case BODY_HISTORY_PROBE_HAVE:
            if (!body_coverage_insert(held, rlo, rhi) ||
                !body_coverage_insert(measured, rlo, rhi))
                LOG_FAIL("body_history",
                         "fold: insert have [%lld..%lld] failed",
                         (long long)rlo, (long long)rhi);
            res.have += run;
            res.examined += run;
            break;
        case BODY_HISTORY_PROBE_MISSING:
            /* Measured — we looked and it was definitively absent. The body
             * is NOT inserted into `held`; if a stale entry claims it is
             * held, remove it so the two maps stop disagreeing. */
            if (!body_coverage_insert(measured, rlo, rhi) ||
                !body_coverage_remove(held, rlo, rhi))
                LOG_FAIL("body_history",
                         "fold: insert missing [%lld..%lld] failed",
                         (long long)rlo, (long long)rhi);
            res.missing += run;
            res.examined += run;
            break;
        default:
            /* INDETERMINATE: enters NEITHER map. This is the branch the
             * previous attempt at this fix got wrong — an unreadable index
             * must leave the height unmeasured, never silently "covered"
             * and never silently "clean". */
            res.indeterminate += run;
            break;
        }
        i = j;
    }

    c->heights_examined += (uint64_t)res.examined;
    c->heights_have += (uint64_t)res.have;
    c->heights_missing += (uint64_t)res.missing;
    c->heights_indeterminate += (uint64_t)res.indeterminate;

    if (out)
        *out = res;
    return true;
}

size_t body_history_census_collect_missing(int64_t lo,
                                           const uint8_t *classes,
                                           const struct uint256 *hashes,
                                           size_t n,
                                           struct uint256 *out_hashes,
                                           int32_t *out_heights,
                                           size_t cap)
{
    if (!classes || !hashes || !out_hashes || !out_heights || cap == 0)
        return 0;
    if (lo < 0)
        return 0;

    size_t got = 0;
    for (size_t i = 0; i < n && got < cap; i++) {
        if (classes[i] != BODY_HISTORY_PROBE_MISSING)
            continue;
        int64_t h = lo + (int64_t)i;
        if (h < 0 || h > INT32_MAX)
            continue;
        out_hashes[got] = hashes[i];
        out_heights[got] = (int32_t)h;
        got++;
    }
    return got;
}

/* ── Process-wide singleton ─────────────────────────────────────── */

static struct body_history_census  g_bh_census;
static struct body_coverage_map    g_bh_measured;
static struct body_history_verdict g_bh_verdict;
static bool                        g_bh_verdict_published = false;
static zcl_mutex_t                 g_bh_lock;
static bool                        g_bh_inited = false;

static void bh_global_init_once(void)
{
    if (g_bh_inited)
        return;
    zcl_mutex_init(&g_bh_lock);
    body_history_census_init(&g_bh_census);
    body_coverage_init(&g_bh_measured);
    bh_verdict_reset(&g_bh_verdict);
    g_bh_verdict_published = false;
    g_bh_inited = true;
}

void body_history_global_lock(void)
{
    bh_global_init_once();
    zcl_mutex_lock(&g_bh_lock);
    /* Ordered acquire: body_history then body_coverage. The census driver
     * needs both (the `held` map is body_coverage's), and this is the only
     * place the pair is taken, so the order cannot invert. */
    body_coverage_global_lock();
}

void body_history_global_unlock(void)
{
    if (!g_bh_inited)
        return;
    body_coverage_global_unlock();
    zcl_mutex_unlock(&g_bh_lock);
}

struct body_history_census *body_history_global_census(void)
{
    bh_global_init_once();
    return &g_bh_census;
}

struct body_coverage_map *body_history_global_measured(void)
{
    bh_global_init_once();
    return &g_bh_measured;
}

void body_history_publish(const struct body_history_verdict *v)
{
    bh_global_init_once();
    zcl_mutex_lock(&g_bh_lock);
    if (v) {
        g_bh_verdict = *v;
    } else {
        /* A caller with nothing to publish publishes ignorance, not the
         * last good news. */
        bh_verdict_reset(&g_bh_verdict);
    }
    g_bh_verdict_published = true;
    zcl_mutex_unlock(&g_bh_lock);
}

bool body_history_get_verdict(struct body_history_verdict *out)
{
    if (!out)
        return false;
    bh_verdict_reset(out);
    bh_global_init_once();
    zcl_mutex_lock(&g_bh_lock);
    bool published = g_bh_verdict_published;
    if (published)
        *out = g_bh_verdict;
    zcl_mutex_unlock(&g_bh_lock);
    return published;
}

bool body_history_is_proven(void)
{
    struct body_history_verdict v;
    if (!body_history_get_verdict(&v))
        return false;
    return body_history_verdict_is_proven(&v);
}

enum body_history_status body_history_status_now(void)
{
    struct body_history_verdict v;
    /* get_verdict resets *out to UNKNOWN before it does anything else, so an
     * unpublished verdict returns UNKNOWN without a separate branch. */
    (void)body_history_get_verdict(&v);
    return v.status;
}

void body_history_reset(void)
{
    bh_global_init_once();
    zcl_mutex_lock(&g_bh_lock);
    body_history_census_init(&g_bh_census);
    body_coverage_reset(&g_bh_measured);
    bh_verdict_reset(&g_bh_verdict);
    g_bh_verdict_published = false;
    zcl_mutex_unlock(&g_bh_lock);
}

/* ── Durable resume ─────────────────────────────────────────────── */

bool body_history_save(struct sqlite3 *db)
{
    if (!db)
        LOG_FAIL("body_history", "save: null db");
    bh_global_init_once();

    zcl_mutex_lock(&g_bh_lock);
    int64_t cursor = g_bh_census.cursor_valid ? g_bh_census.cursor : -1;
    bool ok = body_coverage_save_key(&g_bh_measured, db,
                                     BODY_HISTORY_MEASURED_META_KEY);
    zcl_mutex_unlock(&g_bh_lock);
    if (!ok)
        LOG_FAIL("body_history", "save: measured map persist failed");

    if (!progress_meta_set(db, BODY_HISTORY_CURSOR_META_KEY,
                           &cursor, sizeof(cursor)))
        LOG_FAIL("body_history", "save: cursor persist failed");
    return true;
}

bool body_history_load(struct sqlite3 *db)
{
    if (!db)
        LOG_FAIL("body_history", "load: null db");
    bh_global_init_once();

    zcl_mutex_lock(&g_bh_lock);
    bool ok = body_coverage_load_key(&g_bh_measured, db,
                                     BODY_HISTORY_MEASURED_META_KEY);
    if (!ok) {
        /* A malformed blob means we do not know what was measured. Clear
         * it: an empty measured map reads as "nothing established", which
         * is exactly right, and the census rebuilds it. */
        body_coverage_reset(&g_bh_measured);
        body_history_census_init(&g_bh_census);
        bh_verdict_reset(&g_bh_verdict);
        g_bh_verdict_published = false;
        zcl_mutex_unlock(&g_bh_lock);
        LOG_FAIL("body_history", "load: measured map read failed");
    }

    int64_t cursor = -1;
    size_t got = 0;
    bool found = false;
    if (progress_meta_get(db, BODY_HISTORY_CURSOR_META_KEY,
                          &cursor, sizeof(cursor), &got, &found) &&
        found && got == sizeof(cursor) && cursor >= 0) {
        g_bh_census.cursor = cursor;
        g_bh_census.cursor_valid = true;
    }
    /* The verdict itself is NEVER restored from disk. A restarted node has
     * not established anything until it runs a pass, and a persisted
     * "complete" would be exactly the borrowed claim this module exists to
     * prevent. */
    bh_verdict_reset(&g_bh_verdict);
    g_bh_verdict_published = false;
    zcl_mutex_unlock(&g_bh_lock);
    return true;
}

/* ── Diagnostics ────────────────────────────────────────────────── */

bool body_history_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;
    json_set_object(out);

    bh_global_init_once();
    zcl_mutex_lock(&g_bh_lock);
    struct body_history_verdict v = g_bh_verdict;
    bool published = g_bh_verdict_published;
    struct body_history_census c = g_bh_census;
    int64_t measured_ranges = (int64_t)body_coverage_range_count(&g_bh_measured);
    int64_t measured_total = body_coverage_total_covered(&g_bh_measured);
    zcl_mutex_unlock(&g_bh_lock);

    /* `status` and `proven` are the two fields an operator or agent reads.
     * They are deliberately BOTH published: "unknown" is a status, not a
     * missing field, and proven=false covers unknown and incomplete alike. */
    json_push_kv_str(out, "status", body_history_status_name(v.status));
    json_push_kv_bool(out, "proven",
                      published && v.status == BODY_HISTORY_COMPLETE);
    json_push_kv_bool(out, "verdict_published", published);
    json_push_kv_str(out, "blocker_id",
                     (published && v.status == BODY_HISTORY_COMPLETE)
                         ? "" : BODY_HISTORY_UNPROVEN_BLOCKER);

    json_push_kv_int(out, "window_lo", v.window_lo);
    json_push_kv_int(out, "window_hi", v.window_hi);
    json_push_kv_int(out, "window_heights", v.window_heights);
    json_push_kv_int(out, "held_count", v.held_count);
    json_push_kv_int(out, "missing_count", v.missing_count);
    json_push_kv_int(out, "lowest_missing", v.lowest_missing);
    json_push_kv_int(out, "unmeasured_count", v.unmeasured_count);
    json_push_kv_int(out, "lowest_unmeasured", v.lowest_unmeasured);

    struct json_value census;
    json_init(&census);
    json_set_object(&census);
    json_push_kv_int(&census, "cursor", c.cursor_valid ? c.cursor : -1);
    json_push_kv_bool(&census, "cursor_valid", c.cursor_valid);
    json_push_kv_int(&census, "passes", (int64_t)c.passes);
    json_push_kv_int(&census, "sweeps_completed", (int64_t)c.sweeps_completed);
    json_push_kv_int(&census, "heights_examined", (int64_t)c.heights_examined);
    json_push_kv_int(&census, "heights_have", (int64_t)c.heights_have);
    json_push_kv_int(&census, "heights_missing", (int64_t)c.heights_missing);
    json_push_kv_int(&census, "heights_indeterminate",
                     (int64_t)c.heights_indeterminate);
    json_push_kv_int(&census, "blocks_enqueued", (int64_t)c.blocks_enqueued);
    json_push_kv_int(&census, "last_pass_lo", c.last_pass_lo);
    json_push_kv_int(&census, "last_pass_hi", c.last_pass_hi);
    json_push_kv_int(&census, "measured_ranges", measured_ranges);
    json_push_kv_int(&census, "measured_heights", measured_total);
    json_push_kv(out, "census", &census);
    json_free(&census);
    return true;
}
