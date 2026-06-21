/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Typed blocker primitive — implementation. See util/blocker.h. */

#include "platform/time_compat.h"
#include "util/blocker.h"

#include "json/json.h"
#include "util/log_macros.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Internal types ────────────────────────────────────────────────── */

struct blocker_slot {
    bool                    in_use;
    char                    id[BLOCKER_ID_MAX];
    char                    owner_subsystem[BLOCKER_OWNER_MAX];
    enum blocker_class      class;
    int64_t                 since_us;            /* first set time */
    int64_t                 escape_deadline_us;  /* absolute, or 0 */
    int64_t                 last_set_us;         /* for rate limit */
    char                    escape_action[BLOCKER_ACTION_MAX];
    int32_t                 retry_count;
    int32_t                 retry_budget;
    uint32_t                fire_count;
    char                    reason[BLOCKER_REASON_MAX];
    bool                    escape_fired;        /* edge-triggered guard */
};

struct escape_entry {
    bool               in_use;
    const char        *name;       /* interned, static-lifetime */
    blocker_escape_fn  fn;
};

/* ── Module state ──────────────────────────────────────────────────── */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct blocker_slot   g_slots[BLOCKER_CAP];
static struct escape_entry   g_escapes[BLOCKER_ESCAPE_CAP];
static int g_rate_limit_ms = BLOCKER_DEFAULT_RATE_LIMIT_MS;

static _Atomic bool      g_module_inited = false;
static _Atomic int       g_dispatched_count = 0;  /* test diagnostic */
static _Atomic int64_t   g_test_clock_us = 0;     /* 0 = use real clock */

/* ── Time ──────────────────────────────────────────────────────────── */

static int64_t mono_us(void)
{
    int64_t over = atomic_load(&g_test_clock_us);
    if (over != 0) return over;
    struct timespec ts;
    platform_time_monotonic_timespec(&ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ── Class name ────────────────────────────────────────────────────── */

const char *blocker_class_name(enum blocker_class c)
{
    switch (c) {
    case BLOCKER_PERMANENT:  return "permanent";
    case BLOCKER_TRANSIENT:  return "transient";
    case BLOCKER_DEPENDENCY: return "dependency";
    case BLOCKER_RESOURCE:   return "resource";
    }
    return "(invalid)";
}

/* ── Init helpers (lock-free) ─────────────────────────────────────── */

bool blocker_init(struct blocker_record *out,
                  const char *id,
                  const char *owner,
                  enum blocker_class class,
                  const char *reason)
{
    if (!out || !id || !owner) LOG_FAIL("blocker", "init: null arg");
    if (strlen(id) >= BLOCKER_ID_MAX)
        LOG_FAIL("blocker", "init: id too long: %s", id);
    if (strlen(owner) >= BLOCKER_OWNER_MAX)
        LOG_FAIL("blocker", "init: owner too long: %s", owner);
    memset(out, 0, sizeof(*out));
    out->class = class;
    out->retry_budget = (class == BLOCKER_PERMANENT) ? -1 : 0;
    snprintf(out->id, BLOCKER_ID_MAX, "%s", id);
    snprintf(out->owner_subsystem, BLOCKER_OWNER_MAX, "%s", owner);
    if (reason) {
        snprintf(out->reason, BLOCKER_REASON_MAX, "%s", reason);
    }
    return true;
}

/* ── Registry mutators (under lock) ──────────────────────────────── */

static int find_slot_locked(const char *id)
{
    for (int i = 0; i < BLOCKER_CAP; i++) {
        if (g_slots[i].in_use && strcmp(g_slots[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_free_slot_locked(void)
{
    for (int i = 0; i < BLOCKER_CAP; i++) {
        if (!g_slots[i].in_use) return i;
    }
    return -1;
}

int blocker_set(const struct blocker_record *r)
{
    if (!r) LOG_ERR("blocker", "set: null record");
    if (r->id[0] == '\0') LOG_ERR("blocker", "set: empty id");

    pthread_mutex_lock(&g_lock);
    int64_t now = mono_us();
    int idx = find_slot_locked(r->id);

    if (idx >= 0) {
        struct blocker_slot *s = &g_slots[idx];
        s->fire_count++;
        /* Rate limit: same id within rate-limit window → suppress field
         * update (still counts the fire). */
        int64_t since_last = now - s->last_set_us;
        if (since_last < (int64_t)g_rate_limit_ms * 1000) {
            pthread_mutex_unlock(&g_lock);
            return 1; /* rate-limited dup */
        }
        s->last_set_us = now;
        /* Refresh fields except since_us (preserves age semantics). */
        s->class = r->class;
        s->retry_budget = r->retry_budget;
        snprintf(s->owner_subsystem, BLOCKER_OWNER_MAX, "%s", r->owner_subsystem);
        snprintf(s->escape_action, BLOCKER_ACTION_MAX, "%s", r->escape_action);
        snprintf(s->reason, BLOCKER_REASON_MAX, "%s", r->reason);
        if (r->escape_deadline_secs > 0) {
            s->escape_deadline_us = s->since_us + r->escape_deadline_secs * 1000000;
        } else {
            s->escape_deadline_us = 0;
        }
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    /* New record. */
    idx = find_free_slot_locked();
    if (idx < 0) {
        pthread_mutex_unlock(&g_lock);
        LOG_ERR("blocker", "set: registry full (cap=%d)", BLOCKER_CAP);
    }
    struct blocker_slot *s = &g_slots[idx];
    memset(s, 0, sizeof(*s));
    s->in_use = true;
    snprintf(s->id, BLOCKER_ID_MAX, "%s", r->id);
    snprintf(s->owner_subsystem, BLOCKER_OWNER_MAX, "%s", r->owner_subsystem);
    snprintf(s->escape_action, BLOCKER_ACTION_MAX, "%s", r->escape_action);
    snprintf(s->reason, BLOCKER_REASON_MAX, "%s", r->reason);
    s->class = r->class;
    s->retry_budget = r->retry_budget;
    s->since_us = now;
    s->last_set_us = now;
    s->fire_count = 1;
    s->escape_fired = false;
    s->escape_deadline_us = (r->escape_deadline_secs > 0)
        ? now + r->escape_deadline_secs * 1000000
        : 0;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void blocker_record_retry(const char *id)
{
    if (!id) return;
    pthread_mutex_lock(&g_lock);
    int idx = find_slot_locked(id);
    if (idx >= 0) {
        g_slots[idx].retry_count++;
    }
    pthread_mutex_unlock(&g_lock);
}

void blocker_clear(const char *id)
{
    if (!id) return;
    pthread_mutex_lock(&g_lock);
    int idx = find_slot_locked(id);
    if (idx >= 0) {
        memset(&g_slots[idx], 0, sizeof(g_slots[idx]));
    }
    pthread_mutex_unlock(&g_lock);
}

int blocker_class_for(const char *id)
{
    if (!id) return -1;
    pthread_mutex_lock(&g_lock);
    int idx = find_slot_locked(id);
    int cls = (idx >= 0) ? (int)g_slots[idx].class : -1;
    pthread_mutex_unlock(&g_lock);
    return cls;
}

bool blocker_exists(const char *id)
{
    if (!id) return false;
    pthread_mutex_lock(&g_lock);
    bool b = (find_slot_locked(id) >= 0);
    pthread_mutex_unlock(&g_lock);
    return b;
}

/* ── Snapshot ──────────────────────────────────────────────────────── */

static void fill_snapshot(struct blocker_snapshot *out,
                          const struct blocker_slot *s,
                          int64_t now)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->id, s->id, BLOCKER_ID_MAX);
    memcpy(out->owner_subsystem, s->owner_subsystem, BLOCKER_OWNER_MAX);
    out->class = (int)s->class;
    out->since_us = s->since_us;
    out->age_us = now - s->since_us;
    out->escape_deadline_us = s->escape_deadline_us;
    out->deadline_remaining_us = (s->escape_deadline_us > 0)
        ? (s->escape_deadline_us - now) : 0;
    memcpy(out->escape_action, s->escape_action, BLOCKER_ACTION_MAX);
    out->retry_count = s->retry_count;
    out->retry_budget = s->retry_budget;
    out->fire_count = s->fire_count;
    memcpy(out->reason, s->reason, BLOCKER_REASON_MAX);
}

int blocker_snapshot_all(struct blocker_snapshot *out, int max)
{
    if (!out || max <= 0) return 0;
    pthread_mutex_lock(&g_lock);
    int64_t now = mono_us();
    int n = 0;
    for (int i = 0; i < BLOCKER_CAP && n < max; i++) {
        if (!g_slots[i].in_use) continue;
        fill_snapshot(&out[n], &g_slots[i], now);
        n++;
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

int blocker_count_by_class(enum blocker_class c)
{
    pthread_mutex_lock(&g_lock);
    int n = 0;
    for (int i = 0; i < BLOCKER_CAP; i++) {
        if (g_slots[i].in_use && g_slots[i].class == c) n++;
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

int blocker_count_active(void)
{
    pthread_mutex_lock(&g_lock);
    int n = 0;
    for (int i = 0; i < BLOCKER_CAP; i++) {
        if (g_slots[i].in_use) n++;
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

/* ── JSON dump ─────────────────────────────────────────────────────── */

bool blocker_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    struct blocker_snapshot snaps[BLOCKER_CAP];
    int n = blocker_snapshot_all(snaps, BLOCKER_CAP);

    int counts[4] = {0, 0, 0, 0};
    for (int i = 0; i < n; i++) {
        if (snaps[i].class >= 0 && snaps[i].class < 4)
            counts[snaps[i].class]++;
    }

    json_push_kv_int(out, "active_count", n);
    json_push_kv_int(out, "permanent_count",  counts[BLOCKER_PERMANENT]);
    json_push_kv_int(out, "transient_count",  counts[BLOCKER_TRANSIENT]);
    json_push_kv_int(out, "dependency_count", counts[BLOCKER_DEPENDENCY]);
    json_push_kv_int(out, "resource_count",   counts[BLOCKER_RESOURCE]);
    json_push_kv_int(out, "escape_dispatched_total",
                     atomic_load(&g_dispatched_count));
    json_push_kv_int(out, "rate_limit_ms", g_rate_limit_ms);

    struct json_value arr;
    json_init(&arr);
    json_set_array(&arr);
    for (int i = 0; i < n; i++) {
        struct json_value child;
        json_init(&child);
        json_set_object(&child);
        json_push_kv_str(&child, "id", snaps[i].id);
        json_push_kv_str(&child, "owner", snaps[i].owner_subsystem);
        json_push_kv_str(&child, "class",
                         blocker_class_name((enum blocker_class)snaps[i].class));
        json_push_kv_int(&child, "age_us", snaps[i].age_us);
        json_push_kv_int(&child, "deadline_remaining_us",
                         snaps[i].deadline_remaining_us);
        json_push_kv_str(&child, "escape_action", snaps[i].escape_action);
        json_push_kv_int(&child, "retry_count", snaps[i].retry_count);
        json_push_kv_int(&child, "retry_budget", snaps[i].retry_budget);
        json_push_kv_int(&child, "fire_count", snaps[i].fire_count);
        json_push_kv_str(&child, "reason", snaps[i].reason);
        json_push_back(&arr, &child);
        json_free(&child);
    }
    json_push_kv(out, "blockers", &arr);
    json_free(&arr);
    return true;
}

/* ── Escape registry ───────────────────────────────────────────────── */

bool blocker_register_escape(const char *action_name, blocker_escape_fn fn)
{
    if (!action_name || !fn) LOG_FAIL("blocker", "register_escape: null");
    pthread_mutex_lock(&g_lock);
    /* Duplicate name? */
    for (int i = 0; i < BLOCKER_ESCAPE_CAP; i++) {
        if (g_escapes[i].in_use && strcmp(g_escapes[i].name, action_name) == 0) {
            pthread_mutex_unlock(&g_lock);
            LOG_FAIL("blocker", "register_escape: duplicate: %s", action_name);
        }
    }
    for (int i = 0; i < BLOCKER_ESCAPE_CAP; i++) {
        if (!g_escapes[i].in_use) {
            g_escapes[i].in_use = true;
            g_escapes[i].name = action_name;
            g_escapes[i].fn = fn;
            pthread_mutex_unlock(&g_lock);
            return true;
        }
    }
    pthread_mutex_unlock(&g_lock);
    LOG_FAIL("blocker", "register_escape: full (cap=%d)", BLOCKER_ESCAPE_CAP);
}

blocker_escape_fn blocker_lookup_escape(const char *action_name)
{
    if (!action_name) return NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < BLOCKER_ESCAPE_CAP; i++) {
        if (g_escapes[i].in_use &&
            strcmp(g_escapes[i].name, action_name) == 0) {
            blocker_escape_fn fn = g_escapes[i].fn;
            pthread_mutex_unlock(&g_lock);
            return fn;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

/* ── Sweep ─────────────────────────────────────────────────────────── */

int blocker_supervisor_sweep(void)
{
    /* Collect (snapshot, fn) pairs under lock, release, dispatch.
     * Edge-triggered: set escape_fired = true under lock, never re-fire
     * until blocker is cleared or the deadline is refreshed. */
    struct {
        struct blocker_snapshot snap;
        blocker_escape_fn fn;
    } batch[BLOCKER_CAP];
    int batch_n = 0;

    pthread_mutex_lock(&g_lock);
    int64_t now = mono_us();
    for (int i = 0; i < BLOCKER_CAP; i++) {
        struct blocker_slot *s = &g_slots[i];
        if (!s->in_use) continue;
        if (s->escape_fired) continue;
        if (s->escape_deadline_us <= 0) continue;
        if (now < s->escape_deadline_us) continue;
        if (s->escape_action[0] == '\0') continue;
        /* Look up function. */
        blocker_escape_fn fn = NULL;
        for (int k = 0; k < BLOCKER_ESCAPE_CAP; k++) {
            if (g_escapes[k].in_use &&
                strcmp(g_escapes[k].name, s->escape_action) == 0) {
                fn = g_escapes[k].fn;
                break;
            }
        }
        if (!fn) {
            /* Action registered? No → log once per record, skip. We mark
             * escape_fired to avoid spam; operator must clear. */
            fprintf(stderr, "[blocker] escape '%s' not registered (id=%s)\n",
                    s->escape_action, s->id);  // obs-ok:blocker-escape-unregistered
            s->escape_fired = true;
            continue;
        }
        fill_snapshot(&batch[batch_n].snap, s, now);
        batch[batch_n].fn = fn;
        batch_n++;
        s->escape_fired = true;
    }
    pthread_mutex_unlock(&g_lock);

    /* Dispatch outside lock. */
    for (int i = 0; i < batch_n; i++) {
        batch[i].fn(&batch[i].snap);
    }
    if (batch_n > 0) {
        atomic_fetch_add(&g_dispatched_count, batch_n);
    }
    return batch_n;
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

bool blocker_module_init(void)
{
    if (atomic_exchange(&g_module_inited, true)) return true;
    pthread_mutex_lock(&g_lock);
    memset(g_slots, 0, sizeof(g_slots));
    memset(g_escapes, 0, sizeof(g_escapes));
    g_rate_limit_ms = BLOCKER_DEFAULT_RATE_LIMIT_MS;
    pthread_mutex_unlock(&g_lock);
    /* Env override for rate limit (testing/operations). */
    const char *env = getenv("ZCL_BLOCKER_RATE_LIMIT_MS");
    if (env) {
        int v = atoi(env);
        if (v >= 0) g_rate_limit_ms = v;
    }
    return true;
}

void blocker_module_shutdown(void)
{
    if (!atomic_exchange(&g_module_inited, false)) return;
    pthread_mutex_lock(&g_lock);
    memset(g_slots, 0, sizeof(g_slots));
    memset(g_escapes, 0, sizeof(g_escapes));
    pthread_mutex_unlock(&g_lock);
}

/* ── Test hooks ────────────────────────────────────────────────────── */

void blocker_reset_for_testing(void)
{
    pthread_mutex_lock(&g_lock);
    memset(g_slots, 0, sizeof(g_slots));
    memset(g_escapes, 0, sizeof(g_escapes));
    g_rate_limit_ms = BLOCKER_DEFAULT_RATE_LIMIT_MS;
    pthread_mutex_unlock(&g_lock);
    atomic_store(&g_dispatched_count, 0);
    atomic_store(&g_test_clock_us, 0);
    atomic_store(&g_module_inited, true);
}

void blocker_set_clock_for_testing(int64_t now_us)
{
    atomic_store(&g_test_clock_us, now_us);
}

void blocker_advance_clock_for_testing(int64_t delta_us)
{
    int64_t cur = atomic_load(&g_test_clock_us);
    if (cur == 0) {
        struct timespec ts;
        platform_time_monotonic_timespec(&ts);
        cur = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    }
    atomic_store(&g_test_clock_us, cur + delta_us);
}

uint32_t blocker_fire_count_for_testing(const char *id)
{
    if (!id) return 0;
    pthread_mutex_lock(&g_lock);
    int idx = find_slot_locked(id);
    uint32_t fc = (idx >= 0) ? g_slots[idx].fire_count : 0;
    pthread_mutex_unlock(&g_lock);
    return fc;
}

int blocker_escape_dispatched_count(void)
{
    return atomic_load(&g_dispatched_count);
}

void blocker_set_rate_limit_ms_for_testing(int ms)
{
    pthread_mutex_lock(&g_lock);
    if (ms >= 0) g_rate_limit_ms = ms;
    pthread_mutex_unlock(&g_lock);
}
