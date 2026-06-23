/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "framework/condition.h"

#include "event/event.h"
#include "platform/time_compat.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static const struct condition *g_conditions[CONDITION_MAX_REGISTRY];
static int g_condition_count;
static pthread_mutex_t g_condition_mu = PTHREAD_MUTEX_INITIALIZER;
static struct main_state *g_main_state;

static int64_t now_unix(void)
{
    return platform_time_wall_unix();
}

const char *condition_severity_name(enum condition_severity s)
{
    switch (s) {
    case COND_INFO: return "info";
    case COND_WARN: return "warn";
    case COND_CRITICAL: return "critical";
    }
    return "unknown";
}

const char *condition_remedy_result_name(enum condition_remedy_result r)
{
    switch (r) {
    case COND_REMEDY_OK: return "ok";
    case COND_REMEDY_FAILED: return "failed";
    case COND_REMEDY_SKIP: return "skip";
    case COND_REMEDY_UNWITNESSED: return "unwitnessed";
    }
    return "unknown";
}

void condition_engine_set_main_state(struct main_state *ms)
{
    g_main_state = ms;
}

struct main_state *condition_engine_main_state(void)
{
    return g_main_state;
}

static int condition_find_locked(const char *name)
{
    for (int i = 0; i < g_condition_count; i++) {
        if (strcmp(g_conditions[i]->name, name) == 0)
            return i;
    }
    return -1;
}

/* Operator pages escalate per active episode (60s -> 120s -> ... -> 3600s
 * cap) instead of repaging every 60s forever. Ladder state lives in a
 * parallel array keyed by condition_state pointer (registrations are static
 * module objects, never freed; the public struct stays frozen). Slots are
 * claimed and advanced only on the engine tick thread; the reset path only
 * rewinds an existing slot, so the lock-free scan is safe. */
#define COND_PAGE_BASE_SECS 60
#define COND_PAGE_CAP_SECS 3600

struct page_ladder {
    const struct condition_state *owner;
    _Atomic int interval_secs;  /* required gap before the NEXT page */
    _Atomic int pages;          /* pages emitted this active episode */
};
static struct page_ladder g_page_ladders[CONDITION_MAX_REGISTRY];

static struct page_ladder *page_ladder_for(const struct condition_state *s,
                                           bool create)
{
    for (int i = 0; i < CONDITION_MAX_REGISTRY; i++) {
        if (g_page_ladders[i].owner == s)
            return &g_page_ladders[i];
    }
    if (!create)
        return NULL;
    for (int i = 0; i < CONDITION_MAX_REGISTRY; i++) {
        if (!g_page_ladders[i].owner) {
            atomic_store(&g_page_ladders[i].interval_secs,
                         COND_PAGE_BASE_SECS);
            atomic_store(&g_page_ladders[i].pages, 0);
            g_page_ladders[i].owner = s;
            return &g_page_ladders[i];
        }
    }
    return NULL;  /* full (mirrors CONDITION_MAX_REGISTRY); caller uses base */
}

static void condition_state_reset(struct condition_state *s)
{
    struct page_ladder *pl = page_ladder_for(s, false);
    if (pl) {
        atomic_store(&pl->interval_secs, COND_PAGE_BASE_SECS);
        atomic_store(&pl->pages, 0);
    }
    atomic_store(&s->first_detect_unix, 0);
    atomic_store(&s->last_poll_unix, 0);
    atomic_store(&s->last_remedy_unix, 0);
    atomic_store(&s->last_operator_needed_unix, 0);
    atomic_store(&s->target_at_detect, 0);
    atomic_store(&s->attempts, 0);
    atomic_store(&s->last_outcome, COND_REMEDY_SKIP);
    atomic_store(&s->currently_active, false);
    atomic_store(&s->operator_needed_emitted, false);
}

static void condition_mark_cleared(const struct condition *cond,
                                   struct condition_state *s,
                                   int64_t when)
{
    int cleared = atomic_fetch_add(&s->cleared_count, 1) + 1;
    fprintf(stderr,  // obs-ok:condition-cleared-paired
            "[condition_engine] cleared name=%s cleared_count=%d\n",
            cond->name, cleared);
    event_emitf(EV_CONDITION_CLEARED, 0,
                "name=%s at=%lld cleared_count=%d",
                cond->name, (long long)when, cleared);
    condition_state_reset(s);
}

bool condition_register(const struct condition *cond)
{
    if (!cond || !cond->name || !cond->name[0] ||
        !cond->detect || !cond->remedy || !cond->witness) {
        fprintf(stderr,  // obs-ok:condition-register-fail
                "[condition_engine] reject invalid condition registration\n");
        return false;
    }

    pthread_mutex_lock(&g_condition_mu);
    if (condition_find_locked(cond->name) >= 0) {
        pthread_mutex_unlock(&g_condition_mu);
        fprintf(stderr,  // obs-ok:condition-register-dup
                "[condition_engine] duplicate condition name=%s\n",
                cond->name);
        return false;
    }
    if (g_condition_count >= CONDITION_MAX_REGISTRY) {
        pthread_mutex_unlock(&g_condition_mu);
        fprintf(stderr,  // obs-ok:condition-register-full
                "[condition_engine] registry full cap=%d\n",
                CONDITION_MAX_REGISTRY);
        return false;
    }
    g_conditions[g_condition_count++] = cond;
    pthread_mutex_unlock(&g_condition_mu);
    return true;
}

bool condition_engine_has_registered(const char *name)
{
    if (!name || !name[0])
        return false;

    pthread_mutex_lock(&g_condition_mu);
    bool found = condition_find_locked(name) >= 0;
    pthread_mutex_unlock(&g_condition_mu);
    return found;
}

bool condition_engine_get_registered_snapshot(
    const char *name, struct condition_runtime_snapshot *out)
{
    if (!name || !name[0] || !out)
        return false;

    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&g_condition_mu);
    int idx = condition_find_locked(name);
    if (idx < 0) {
        pthread_mutex_unlock(&g_condition_mu);
        return false;
    }

    const struct condition *c = g_conditions[idx];
    const struct condition_state *s = &c->state;
    out->registered = true;
    out->severity = c->severity;
    out->poll_secs = c->poll_secs;
    out->backoff_secs = c->backoff_secs;
    out->max_attempts = c->max_attempts;
    out->witness_window_secs = c->witness_window_secs;
    out->currently_active = atomic_load(&s->currently_active);
    out->operator_needed_emitted =
        atomic_load(&s->operator_needed_emitted);
    out->attempts = atomic_load(&s->attempts);
    out->last_outcome = atomic_load(&s->last_outcome);
    out->cleared_count = atomic_load(&s->cleared_count);
    pthread_mutex_unlock(&g_condition_mu);
    return true;
}

static bool condition_due_for_remedy(const struct condition *cond,
                                     struct condition_state *s,
                                     int64_t now)
{
    int attempts = atomic_load(&s->attempts);
    int max_attempts = cond->max_attempts > 0 ? cond->max_attempts : 1;
    if (attempts >= max_attempts)
        return false;
    int64_t last = atomic_load(&s->last_remedy_unix);
    int backoff = cond->backoff_secs > 0 ? cond->backoff_secs : 0;
    return last == 0 || now - last >= backoff;
}

static void condition_tick_one(const struct condition *cond, int64_t now)
{
    struct condition_state *s = (struct condition_state *)&cond->state;
    int poll_secs = cond->poll_secs > 0 ? cond->poll_secs : 1;
    int64_t last_poll = atomic_load(&s->last_poll_unix);
    bool active = atomic_load(&s->currently_active);
    if (!active && last_poll != 0 && now - last_poll < poll_secs)
        return;
    if (!active)
        atomic_store(&s->last_poll_unix, now);

    int64_t target = atomic_load(&s->target_at_detect);
    bool detected = cond->detect();

    if (!detected) {
        if (atomic_load(&s->currently_active) && cond->witness(target))
            condition_mark_cleared(cond, s, now);
        else if (!atomic_load(&s->currently_active)) {
            condition_state_reset(s);
            atomic_store(&s->last_poll_unix, now);
        }
        return;
    }

    if (!atomic_load(&s->currently_active)) {
        atomic_store(&s->currently_active, true);
        atomic_store(&s->first_detect_unix, now);
        atomic_store(&s->target_at_detect, now);
        fprintf(stderr,  // obs-ok:condition-detected-paired
                "[condition_engine] detected name=%s severity=%s\n",
                cond->name, condition_severity_name(cond->severity));
        event_emitf(EV_CONDITION_DETECTED, 0,
                    "name=%s severity=%s",
                    cond->name, condition_severity_name(cond->severity));
    }

    target = atomic_load(&s->target_at_detect);

    /* If a prior remedy already cleared the symptom, witness it now (covers
     * the case where the symptom clears between remedy attempts). */
    if (cond->witness(target)) {
        condition_mark_cleared(cond, s, now);
        return;
    }

    if (condition_due_for_remedy(cond, s, now)) {
        enum condition_remedy_result r = cond->remedy();
        int attempts = atomic_fetch_add(&s->attempts, 1) + 1;
        atomic_store(&s->last_remedy_unix, now);

        /* DOCTRINE (REFACTOR_STATUS): a remedy may only be reported as `ok`
         * if the symptom actually cleared. We re-check the witness right
         * after the remedy ran. A remedy that returned COND_REMEDY_OK but did
         * NOT clear the symptom is reported as `unwitnessed` — NOT ok — so a
         * frozen tip can never self-report success. Witnessed success clears
         * the condition (and resets attempts); an unwitnessed OK leaves the
         * attempt counted so it accrues toward operator escalation. */
        bool witnessed = cond->witness(target);
        enum condition_remedy_result reported = r;
        if (r == COND_REMEDY_OK && !witnessed)
            reported = COND_REMEDY_UNWITNESSED;
        atomic_store(&s->last_outcome, reported);

        fprintf(stderr,  // obs-ok:condition-remedy-paired
                "[condition_engine] remedy name=%s attempt=%d result=%s\n",
                cond->name, attempts,
                condition_remedy_result_name(reported));
        event_emitf(EV_CONDITION_REMEDY_ATTEMPTED, 0,
                    "name=%s attempt=%d result=%s",
                    cond->name, attempts,
                    condition_remedy_result_name(reported));

        if (witnessed) {
            condition_mark_cleared(cond, s, now);
            return;
        }
    }

    int max_attempts = cond->max_attempts > 0 ? cond->max_attempts : 1;
    if (atomic_load(&s->attempts) >= max_attempts) {
        struct page_ladder *pl = page_ladder_for(s, true);
        int interval = pl ? atomic_load(&pl->interval_secs)
                          : COND_PAGE_BASE_SECS;
        int64_t last_page = atomic_load(&s->last_operator_needed_unix);
        if (last_page == 0 || now - last_page >= interval) {
            atomic_store(&s->last_operator_needed_unix, now);
            atomic_store(&s->operator_needed_emitted, true);
            /* Escalate the repage gap: next = min(last*2, cap). The first
             * page of an episode emits immediately and keeps the base gap. */
            int next = interval;
            if (last_page != 0) {
                next = interval * 2;
                if (next > COND_PAGE_CAP_SECS)
                    next = COND_PAGE_CAP_SECS;
            }
            int page_n = 1;
            if (pl) {
                atomic_store(&pl->interval_secs, next);
                page_n = atomic_fetch_add(&pl->pages, 1) + 1;
            }
            long long active_for =
                (long long)(now - atomic_load(&s->first_detect_unix));
            fprintf(stderr,  // obs-ok:condition-operator-paired
                    "[condition_engine] operator_needed name=%s attempts=%d"
                    " active_for=%llds page=%d next_page_in=%ds\n",
                    cond->name, atomic_load(&s->attempts),
                    active_for, page_n, next);
            event_emitf(EV_OPERATOR_NEEDED, 0,
                        "condition=%s attempts=%d active_for=%llds page=%d"
                        " next_page_in=%ds",
                        cond->name, atomic_load(&s->attempts),
                        active_for, page_n, next);
        }
    }
}

void condition_engine_tick(void)
{
    const struct condition *snapshot[CONDITION_MAX_REGISTRY];
    int count;
    pthread_mutex_lock(&g_condition_mu);
    count = g_condition_count;
    for (int i = 0; i < count; i++)
        snapshot[i] = g_conditions[i];
    pthread_mutex_unlock(&g_condition_mu);

    int64_t now = now_unix();
    for (int i = 0; i < count; i++)
        condition_tick_one(snapshot[i], now);
}

int condition_engine_get_active_count(void)
{
    int n = 0;
    pthread_mutex_lock(&g_condition_mu);
    for (int i = 0; i < g_condition_count; i++) {
        if (atomic_load(&g_conditions[i]->state.currently_active))
            n++;
    }
    pthread_mutex_unlock(&g_condition_mu);
    return n;
}

int condition_engine_get_unresolved_count(void)
{
    int n = 0;
    pthread_mutex_lock(&g_condition_mu);
    for (int i = 0; i < g_condition_count; i++) {
        const struct condition *c = g_conditions[i];
        int max_attempts = c->max_attempts > 0 ? c->max_attempts : 1;
        if (atomic_load(&c->state.currently_active) &&
            atomic_load(&c->state.attempts) >= max_attempts)
            n++;
    }
    pthread_mutex_unlock(&g_condition_mu);
    return n;
}

bool condition_engine_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;

    const struct condition *snapshot[CONDITION_MAX_REGISTRY];
    int count;
    pthread_mutex_lock(&g_condition_mu);
    count = g_condition_count;
    for (int i = 0; i < count; i++)
        snapshot[i] = g_conditions[i];
    pthread_mutex_unlock(&g_condition_mu);

    json_push_kv_int(out, "registered_count", count);
    json_push_kv_int(out, "active_count", condition_engine_get_active_count());
    json_push_kv_int(out, "unresolved_count",
                     condition_engine_get_unresolved_count());

    struct json_value arr;
    json_init(&arr);
    json_set_array(&arr);

    for (int i = 0; i < count; i++) {
        const struct condition *c = snapshot[i];
        const struct condition_state *s = &c->state;
        struct json_value obj;
        json_init(&obj);
        json_set_object(&obj);
        json_push_kv_str(&obj, "name", c->name);
        json_push_kv_str(&obj, "severity",
                         condition_severity_name(c->severity));
        json_push_kv_bool(&obj, "currently_active",
                          atomic_load(&s->currently_active));
        json_push_kv_int(&obj, "first_detect_unix",
                         atomic_load(&s->first_detect_unix));
        json_push_kv_int(&obj, "last_poll_unix",
                         atomic_load(&s->last_poll_unix));
        json_push_kv_int(&obj, "last_remedy_unix",
                         atomic_load(&s->last_remedy_unix));
        json_push_kv_int(&obj, "last_operator_needed_unix",
                         atomic_load(&s->last_operator_needed_unix));
        json_push_kv_int(&obj, "target_at_detect",
                         atomic_load(&s->target_at_detect));
        json_push_kv_int(&obj, "attempts", atomic_load(&s->attempts));
        json_push_kv_str(&obj, "last_outcome",
                         condition_remedy_result_name(
                             atomic_load(&s->last_outcome)));
        json_push_kv_bool(&obj, "operator_needed_emitted",
                          atomic_load(&s->operator_needed_emitted));
        json_push_kv_int(&obj, "cleared_count",
                         atomic_load(&s->cleared_count));

        if (c->detail) {
            struct json_value detail;
            json_init(&detail);
            json_set_object(&detail);
            if (c->detail(&detail))
                json_push_kv(&obj, "detail", &detail);
            json_free(&detail);
        }

        struct json_value thr;
        json_init(&thr);
        json_set_object(&thr);
        json_push_kv_int(&thr, "poll_secs", c->poll_secs);
        json_push_kv_int(&thr, "backoff_secs", c->backoff_secs);
        json_push_kv_int(&thr, "max_attempts", c->max_attempts);
        json_push_kv(&obj, "thresholds", &thr);
        json_free(&thr);

        json_push_back(&arr, &obj);
        json_free(&obj);
    }

    bool ok = json_push_kv(out, "conditions", &arr);
    json_free(&arr);
    return ok;
}

#ifdef ZCL_TESTING
void condition_reset_state(struct condition *c)
{
    if (!c)
        return;
    struct condition_state *s = &c->state;
    struct page_ladder *pl = page_ladder_for(s, false);
    if (pl) {
        atomic_store(&pl->interval_secs, COND_PAGE_BASE_SECS);
        atomic_store(&pl->pages, 0);
    }
    atomic_store(&s->attempts, 0);
    atomic_store(&s->last_outcome, COND_REMEDY_SKIP);
    atomic_store(&s->currently_active, false);
    atomic_store(&s->operator_needed_emitted, false);
}

void condition_engine_reset_for_testing(void)
{
    pthread_mutex_lock(&g_condition_mu);
    for (int i = 0; i < g_condition_count; i++)
        condition_state_reset((struct condition_state *)&g_conditions[i]->state);
    memset(g_conditions, 0, sizeof(g_conditions));
    /* Drop ladder slots too: test fixtures re-register conditions and the
     * 64-slot map must not fill with stale owners across resets. */
    memset(g_page_ladders, 0, sizeof(g_page_ladders));
    g_condition_count = 0;
    g_main_state = NULL;
    pthread_mutex_unlock(&g_condition_mu);
}
#endif
