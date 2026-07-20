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
    int64_t                 last_fire_us;        /* every blocker_set touch,
                                                   * incl. rate-limited dups;
                                                   * TTL retirement clock. */
    int64_t                 ttl_us;              /* TRANSIENT-only; 0 =
                                                   * use the module default. */
    int64_t                 escape_span_us;      /* original relative
                                                   * escape_deadline_secs, in
                                                   * us; used to re-arm. */
    int32_t                 rearm_count;         /* deadline re-arm attempts
                                                   * (rule 2), bounded. */
    bool                    escalated;           /* rule-2 budget exhausted */
    char                    caused_by[BLOCKER_ID_MAX];              /* see blocker.h */
    char                    cause_detail[BLOCKER_CAUSE_DETAIL_MAX];
};

struct escape_entry {
    bool               in_use;
    const char        *name;       /* interned, static-lifetime */
    blocker_escape_fn  fn;
};

/* Forward decl — defined in the Snapshot section below, used by
 * blocker_find_by_id_prefix which sits with the other registry mutators. */
static void fill_snapshot(struct blocker_snapshot *out,
                          const struct blocker_slot *s,
                          int64_t now);

/* ── Module state ──────────────────────────────────────────────────── */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct blocker_slot   g_slots[BLOCKER_CAP];
static struct escape_entry   g_escapes[BLOCKER_ESCAPE_CAP];
static int g_rate_limit_ms = BLOCKER_DEFAULT_RATE_LIMIT_MS;

static _Atomic bool      g_module_inited = false;
static _Atomic int       g_dispatched_count = 0;  /* test diagnostic */
static _Atomic int64_t   g_test_clock_us = 0;     /* 0 = use real clock */
static _Atomic uint64_t  g_generation = 0;

/* Lifecycle policy bookkeeping (rule 1: TTL retirement). Witnessed, never
 * a silent delete: a monotonic total plus the most recent instance. Both
 * guarded by g_lock (retirement always happens under lock, in the sweep). */
static _Atomic uint32_t  g_transient_retired_total = 0;
static struct blocker_retirement_info g_last_retired;

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
        /* Any touch — rate-limited or not — is a re-fire for TTL purposes
         * (rule 1): the caller is telling us the condition is still live. */
        s->last_fire_us = now;
        atomic_fetch_add(&g_generation, 1);
        /* Rate limit: same id within rate-limit window → suppress field
         * update (still counts the fire). */
        int64_t since_last = now - s->last_set_us;
        if (since_last < (int64_t)g_rate_limit_ms * 1000) {
            pthread_mutex_unlock(&g_lock);
            return 1; /* rate-limited dup */
        }
        s->last_set_us = now;
        /* Fault IDENTITY: does this refire describe the SAME live fault, or
         * a genuinely new occurrence? Identity = the class plus the
         * descriptive fields the producer varies when the underlying fault
         * changes (reason, and the root-cause edge caused_by/cause_detail).
         * A worker stall re-raised with the same enum reason is the SAME
         * stall still live; a stall re-raised at a new height/target changes
         * reason and is a fresh occurrence. This is the keystone
         * convergence fix: only a genuinely NEW occurrence may reset the
         * rule-2 escalation clock (rearm_count/escalated) and re-anchor the
         * deadline horizon. An actively-refiring SAME-identity blocker must
         * keep advancing toward escalation — the previous code reset both on
         * every touch and re-anchored the deadline to the ever-staler fixed
         * since_us, so a still-stalling worker dodged escalation forever. */
        bool identity_changed =
            (int)s->class != (int)r->class ||
            strcmp(s->reason, r->reason) != 0 ||
            strcmp(s->caused_by, r->caused_by) != 0 ||
            strcmp(s->cause_detail, r->cause_detail) != 0;

        /* Refresh fields except since_us (preserves age semantics). */
        s->class = r->class;
        s->retry_budget = r->retry_budget;
        s->ttl_us = r->transient_ttl_secs * 1000000;
        snprintf(s->owner_subsystem, BLOCKER_OWNER_MAX, "%s", r->owner_subsystem);
        snprintf(s->escape_action, BLOCKER_ACTION_MAX, "%s", r->escape_action);
        snprintf(s->reason, BLOCKER_REASON_MAX, "%s", r->reason);
        snprintf(s->caused_by, BLOCKER_ID_MAX, "%s", r->caused_by);
        snprintf(s->cause_detail, BLOCKER_CAUSE_DETAIL_MAX, "%s", r->cause_detail);
        if (r->escape_deadline_secs > 0) {
            s->escape_span_us = r->escape_deadline_secs * 1000000;
            /* Anchor a fresh forward horizon from NOW (never the stale
             * since_us) only on a new occurrence or when a deadline is
             * first requested. On a same-identity refire, LEAVE the
             * existing deadline/rearm_count/escalated untouched so the
             * sweep keeps driving the still-live fault toward escalation. */
            if (identity_changed || s->escape_deadline_us <= 0) {
                s->escape_deadline_us = now + s->escape_span_us;
                s->rearm_count = 0;
                s->escalated = false;
            }
        } else {
            s->escape_deadline_us = 0;
            s->escape_span_us = 0;
            if (identity_changed) {
                s->rearm_count = 0;
                s->escalated = false;
            }
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
    snprintf(s->caused_by, BLOCKER_ID_MAX, "%s", r->caused_by);
    snprintf(s->cause_detail, BLOCKER_CAUSE_DETAIL_MAX, "%s", r->cause_detail);
    s->class = r->class;
    s->retry_budget = r->retry_budget;
    s->since_us = now;
    s->last_set_us = now;
    s->last_fire_us = now;
    s->ttl_us = r->transient_ttl_secs * 1000000;
    s->fire_count = 1;
    s->escape_fired = false;
    s->escape_deadline_us = (r->escape_deadline_secs > 0)
        ? now + r->escape_deadline_secs * 1000000
        : 0;
    s->escape_span_us = (r->escape_deadline_secs > 0)
        ? r->escape_deadline_secs * 1000000
        : 0;
    s->rearm_count = 0;
    s->escalated = false;
    atomic_fetch_add(&g_generation, 1);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int blocker_set_with_cause(const struct blocker_record *r,
                           const char *caused_by,
                           const char *cause_detail)
{
    if (!r) LOG_ERR("blocker", "set_with_cause: null record");
    if (caused_by && strlen(caused_by) >= BLOCKER_ID_MAX)
        LOG_ERR("blocker", "set_with_cause: caused_by too long: %s", caused_by);

    struct blocker_record local = *r;
    if (caused_by && caused_by[0]) {
        snprintf(local.caused_by, sizeof(local.caused_by), "%s", caused_by);
    } else {
        local.caused_by[0] = '\0';
    }
    if (cause_detail) {
        snprintf(local.cause_detail, sizeof(local.cause_detail), "%s", cause_detail);
    } else {
        local.cause_detail[0] = '\0';
    }
    return blocker_set(&local);
}

/* Read-only registry scan by id prefix. See blocker.h. */
bool blocker_find_by_id_prefix(const char *id_prefix, struct blocker_snapshot *out)
{
    if (!id_prefix || !id_prefix[0] || !out) return false;
    size_t plen = strlen(id_prefix);

    pthread_mutex_lock(&g_lock);
    int64_t now = mono_us();
    bool found = false;
    for (int i = 0; i < BLOCKER_CAP; i++) {
        if (!g_slots[i].in_use) continue;
        if (strncmp(g_slots[i].id, id_prefix, plen) == 0) {
            fill_snapshot(out, &g_slots[i], now);
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return found;
}

void blocker_record_retry(const char *id)
{
    if (!id) return;
    pthread_mutex_lock(&g_lock);
    int idx = find_slot_locked(id);
    if (idx >= 0) {
        g_slots[idx].retry_count++;
        atomic_fetch_add(&g_generation, 1);
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
        atomic_fetch_add(&g_generation, 1);
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
    out->escalated = s->escalated;
    out->deadline_rearm_count = s->rearm_count;
    memcpy(out->caused_by, s->caused_by, BLOCKER_ID_MAX);
    memcpy(out->cause_detail, s->cause_detail, BLOCKER_CAUSE_DETAIL_MAX);
}

int blocker_snapshot_all_with_meta(struct blocker_snapshot *out, int max,
                                   uint64_t *generation_out,
                                   int *escape_dispatched_out,
                                   int *rate_limit_ms_out)
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
    if (generation_out)
        *generation_out = atomic_load(&g_generation);
    if (escape_dispatched_out)
        *escape_dispatched_out = atomic_load(&g_dispatched_count);
    if (rate_limit_ms_out)
        *rate_limit_ms_out = g_rate_limit_ms;
    pthread_mutex_unlock(&g_lock);
    return n;
}

int blocker_snapshot_all(struct blocker_snapshot *out, int max)
{
    return blocker_snapshot_all_with_meta(out, max, NULL, NULL, NULL);
}

int blocker_causal_priority(enum blocker_class c, const char *id)
{
    /* A disk/resource failure can prevent every recovery write, so retain the
     * generic class ordering above the history-specific refinement. */
    if (c == BLOCKER_RESOURCE)
        return 4000;

    if (c == BLOCKER_PERMANENT && id) {
        if (strcmp(id, "utxo_apply.anchor_backfill_gap") == 0)
            return 3500;
        if (strcmp(id, "utxo_apply.nullifier_backfill_gap") == 0)
            return 3400;
    }

    switch (c) {
    case BLOCKER_PERMANENT:  return 3000;
    case BLOCKER_DEPENDENCY: return 2000;
    case BLOCKER_TRANSIENT:  return 1000;
    case BLOCKER_RESOURCE:   return 4000;
    }
    return 0;
}

const struct blocker_snapshot *blocker_select_dominant(
    const struct blocker_snapshot *snapshots, int count)
{
    if (!snapshots || count <= 0)
        return NULL;

    const struct blocker_snapshot *dominant = NULL;
    int priority = -1;
    for (int i = 0; i < count; i++) {
        const struct blocker_snapshot *candidate = &snapshots[i];
        int candidate_priority = blocker_causal_priority(
            (enum blocker_class)candidate->class, candidate->id);
        if (!dominant || candidate_priority > priority ||
            (candidate_priority == priority &&
             candidate->age_us > dominant->age_us)) {
            dominant = candidate;
            priority = candidate_priority;
        }
    }
    return dominant;
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
    uint64_t generation = 0;
    int escape_dispatched = 0;
    int rate_limit_ms = 0;
    int n = blocker_snapshot_all_with_meta(
        snaps, BLOCKER_CAP, &generation, &escape_dispatched, &rate_limit_ms);

    int counts[4] = {0, 0, 0, 0};
    int escalated_count = 0;
    for (int i = 0; i < n; i++) {
        if (snaps[i].class >= 0 && snaps[i].class < 4)
            counts[snaps[i].class]++;
        if (snaps[i].escalated)
            escalated_count++;
    }

    json_push_kv_int(out, "active_count", n);
    json_push_kv_int(out, "permanent_count",  counts[BLOCKER_PERMANENT]);
    json_push_kv_int(out, "transient_count",  counts[BLOCKER_TRANSIENT]);
    json_push_kv_int(out, "dependency_count", counts[BLOCKER_DEPENDENCY]);
    json_push_kv_int(out, "resource_count",   counts[BLOCKER_RESOURCE]);
    json_push_kv_int(out, "generation", (int64_t)generation);
    json_push_kv_int(out, "escape_dispatched_total", escape_dispatched);
    json_push_kv_int(out, "rate_limit_ms", rate_limit_ms);
    /* Lifecycle policy (rule 1 + rule 2): witnessed retirement + escalation
     * counters — see "Lifecycle policy" in util/blocker.h. */
    json_push_kv_int(out, "escalated_count", escalated_count);
    json_push_kv_int(out, "transient_retired_total",
                     (int64_t)blocker_retired_transient_count());
    {
        struct blocker_retirement_info last;
        struct json_value lr;
        json_init(&lr);
        json_set_object(&lr);
        if (blocker_last_retired(&last)) {
            json_push_kv_str(&lr, "id", last.id);
            json_push_kv_str(&lr, "owner", last.owner_subsystem);
            json_push_kv_int(&lr, "retired_at_us", last.retired_at_us);
            json_push_kv_int(&lr, "fire_count_at_retirement",
                             (int64_t)last.fire_count_at_retirement);
            json_push_kv_str(&lr, "reason", last.reason);
        }
        json_push_kv(out, "last_retired", &lr);
        json_free(&lr);
    }

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
        json_push_kv_bool(&child, "escalated", snaps[i].escalated);
        json_push_kv_int(&child, "deadline_rearm_count",
                         snaps[i].deadline_rearm_count);
        json_push_kv_str(&child, "caused_by", snaps[i].caused_by);
        json_push_kv_str(&child, "cause_detail", snaps[i].cause_detail);
        json_push_back(&arr, &child);
        json_free(&child);
    }
    json_push_kv(out, "blockers", &arr);
    json_free(&arr);

    /* Root-cause chaining classification (see blocker.h "Root-cause
     * chaining"). Only meaningful when at least one active blocker carries
     * a non-empty caused_by — the common case (no edges wired) renders
     * three empty arrays, cheap and harmless.
     *   root    — caused_by empty AND at least one other active blocker's
     *             caused_by names it (an isolated blocker with no known
     *             cause and no known symptoms is neither).
     *   symptom — caused_by non-empty.
     *   orphaned symptom — caused_by non-empty but does not match any
     *             currently active blocker id (its root already cleared;
     *             clearing a root never silently clears its symptoms, so
     *             this is the surfaced signal instead). */
    {
        struct json_value roots, symptoms, orphans;
        json_init(&roots);    json_set_array(&roots);
        json_init(&symptoms); json_set_array(&symptoms);
        json_init(&orphans);  json_set_array(&orphans);

        for (int i = 0; i < n; i++) {
            if (snaps[i].caused_by[0] == '\0') continue;
            struct json_value id_v;
            json_init(&id_v);
            json_set_str(&id_v, snaps[i].id);
            json_push_back(&symptoms, &id_v);
            json_free(&id_v);

            bool root_active = false;
            for (int j = 0; j < n; j++) {
                if (strcmp(snaps[j].id, snaps[i].caused_by) == 0) {
                    root_active = true;
                    break;
                }
            }
            if (!root_active) {
                struct json_value o;
                json_init(&o);
                json_set_str(&o, snaps[i].id);
                json_push_back(&orphans, &o);
                json_free(&o);
            }
        }
        for (int i = 0; i < n; i++) {
            if (snaps[i].caused_by[0] != '\0') continue; /* symptoms aren't roots */
            bool referenced = false;
            for (int j = 0; j < n; j++) {
                if (j == i) continue;
                if (snaps[j].caused_by[0] != '\0' &&
                    strcmp(snaps[j].caused_by, snaps[i].id) == 0) {
                    referenced = true;
                    break;
                }
            }
            if (referenced) {
                struct json_value r_v;
                json_init(&r_v);
                json_set_str(&r_v, snaps[i].id);
                json_push_back(&roots, &r_v);
                json_free(&r_v);
            }
        }

        json_push_kv(out, "root_blocker_ids", &roots);
        json_push_kv(out, "symptom_blocker_ids", &symptoms);
        json_push_kv(out, "orphaned_symptom_ids", &orphans);
        json_free(&roots);
        json_free(&symptoms);
        json_free(&orphans);
    }

    /* Reserved `_health` key (see docs/work "Adding state introspection" +
     * app/controllers/src/diagnostics_health_rollup.c): { ok, reason }.
     * Maps the active-blocker registry above — any active blocker (of any
     * class) marks this subsystem unhealthy; no new health logic. */
    {
        char reason_buf[192] = "";
        if (n > 0) {
            snprintf(reason_buf, sizeof(reason_buf),
                     "%d active blocker(s); first: %s owner=%s class=%s (%s)",
                     n, snaps[0].id, snaps[0].owner_subsystem,
                     blocker_class_name((enum blocker_class)snaps[0].class),
                     snaps[0].reason);
        }
        diag_push_health(out, n == 0, reason_buf);
    }
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

/* ── Lifecycle policy helpers (rule 1: TTL retirement) ────────────────
 * Caller holds g_lock. Records a witnessed retirement (counter + last
 * instance) — see "Lifecycle policy" in util/blocker.h. Does not clear
 * the slot; caller does that immediately after. */
static void record_retirement_locked(const struct blocker_slot *s, int64_t now)
{
    g_last_retired.valid = true;
    memcpy(g_last_retired.id, s->id, BLOCKER_ID_MAX);
    memcpy(g_last_retired.owner_subsystem, s->owner_subsystem, BLOCKER_OWNER_MAX);
    g_last_retired.retired_at_us = now;
    g_last_retired.fire_count_at_retirement = s->fire_count;
    memcpy(g_last_retired.reason, s->reason, BLOCKER_REASON_MAX);
    atomic_fetch_add(&g_transient_retired_total, 1);
    atomic_fetch_add(&g_generation, 1);
}

uint32_t blocker_retired_transient_count(void)
{
    return atomic_load(&g_transient_retired_total);
}

bool blocker_last_retired(struct blocker_retirement_info *out)
{
    if (!out) return false;
    pthread_mutex_lock(&g_lock);
    bool valid = g_last_retired.valid;
    if (valid) {
        *out = g_last_retired;
    } else {
        memset(out, 0, sizeof(*out));
    }
    pthread_mutex_unlock(&g_lock);
    return valid;
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

        /* Rule 1 (TTL retirement, TRANSIENT only): a blocker is a live
         * claim, not a log line. One that has not re-fired within its TTL
         * window is stale evidence — retire it, witnessed via the counter
         * + last-retired record above, never a silent delete. Applies
         * uniformly whether or not the blocker also carries an
         * escape_deadline_us / is currently escalated (rule 2 below);
         * inactivity outranks escalation. */
        if (s->class == BLOCKER_TRANSIENT) {
            int64_t ttl_us = (s->ttl_us > 0)
                ? s->ttl_us
                : (int64_t)BLOCKER_DEFAULT_TRANSIENT_TTL_SECS * 1000000;
            if (now - s->last_fire_us >= ttl_us) {
                record_retirement_locked(s, now);
                memset(s, 0, sizeof(*s));
                continue;
            }
        }

        /* Rule 2 (overdue deadline, TRANSIENT only, no actionable escape):
         * an escape_deadline_us with an empty escape_action has nothing to
         * dispatch and would otherwise sit with a growing-negative
         * deadline_remaining_us forever (this was the live
         * worker.stall.op.projection_backfill defect). Re-arm a bounded
         * number of times, then escalate visibly instead. Blockers with a
         * real escape_action are untouched here — they fall through to the
         * dispatch pass below exactly as before. */
        if (s->class == BLOCKER_TRANSIENT && s->escape_deadline_us > 0 &&
            now >= s->escape_deadline_us && s->escape_action[0] == '\0') {
            if (s->rearm_count < BLOCKER_MAX_DEADLINE_REARMS) {
                int64_t span = (s->escape_span_us > 0)
                    ? s->escape_span_us
                    : (int64_t)BLOCKER_DEFAULT_TRANSIENT_TTL_SECS * 1000000;
                s->escape_deadline_us = now + span;
                s->rearm_count++;
            } else if (!s->escalated) {
                s->escalated = true;
                atomic_fetch_add(&g_generation, 1);
                fprintf(stderr,
                        "[blocker] %s escalated: deadline overdue after %d "
                        "re-arm(s) with no escape action (id=%s owner=%s)\n",
                        blocker_class_name(s->class), s->rearm_count,
                        s->id, s->owner_subsystem);  // obs-ok:blocker-deadline-escalated
            }
        }

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
        /* Keep snapshot metadata on the same registry-lock timeline as the
         * copied entries.  Atomic types preserve legacy lock-free readers;
         * the lock prevents a with-meta reader from observing half of this
         * two-field publication. */
        pthread_mutex_lock(&g_lock);
        atomic_fetch_add(&g_dispatched_count, batch_n);
        atomic_fetch_add(&g_generation, 1);
        pthread_mutex_unlock(&g_lock);
    }
    return batch_n;
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

bool blocker_module_init(void)
{
    if (atomic_exchange(&g_module_inited, true)) return true;
    int configured_rate_limit_ms = BLOCKER_DEFAULT_RATE_LIMIT_MS;
    const char *env = getenv("ZCL_BLOCKER_RATE_LIMIT_MS");
    if (env) {
        int v = atoi(env);
        if (v >= 0) configured_rate_limit_ms = v;
    }
    pthread_mutex_lock(&g_lock);
    memset(g_slots, 0, sizeof(g_slots));
    memset(g_escapes, 0, sizeof(g_escapes));
    memset(&g_last_retired, 0, sizeof(g_last_retired));
    g_rate_limit_ms = configured_rate_limit_ms;
    atomic_fetch_add(&g_generation, 1);
    pthread_mutex_unlock(&g_lock);
    atomic_store(&g_transient_retired_total, 0);
    return true;
}

void blocker_module_shutdown(void)
{
    if (!atomic_exchange(&g_module_inited, false)) return;
    pthread_mutex_lock(&g_lock);
    memset(g_slots, 0, sizeof(g_slots));
    memset(g_escapes, 0, sizeof(g_escapes));
    memset(&g_last_retired, 0, sizeof(g_last_retired));
    atomic_fetch_add(&g_generation, 1);
    pthread_mutex_unlock(&g_lock);
    atomic_store(&g_transient_retired_total, 0);
}

/* ── Test hooks ────────────────────────────────────────────────────── */

void blocker_reset_for_testing(void)
{
    pthread_mutex_lock(&g_lock);
    memset(g_slots, 0, sizeof(g_slots));
    memset(g_escapes, 0, sizeof(g_escapes));
    memset(&g_last_retired, 0, sizeof(g_last_retired));
    g_rate_limit_ms = BLOCKER_DEFAULT_RATE_LIMIT_MS;
    pthread_mutex_unlock(&g_lock);
    atomic_store(&g_dispatched_count, 0);
    atomic_store(&g_test_clock_us, 0);
    atomic_store(&g_module_inited, true);
    atomic_store(&g_transient_retired_total, 0);
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
    if (ms >= 0 && ms != g_rate_limit_ms) {
        g_rate_limit_ms = ms;
        atomic_fetch_add(&g_generation, 1);
    }
    pthread_mutex_unlock(&g_lock);
}
