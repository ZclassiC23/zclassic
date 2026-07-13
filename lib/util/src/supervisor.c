/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Supervisor — implementation. See util/supervisor.h for design notes. */
#define _GNU_SOURCE  /* pthread_timedjoin_np */
/*
 *
 * Architecture:
 *   - Fixed-cap pointer registry (caller owns contracts).
 *   - One dedicated thread "zcl_supervisor", loop period configurable
 *     (default 1000 ms). Loop body: snapshot now; for each registered
 *     contract: maybe-tick (period_secs), maybe-stall (deadline_secs
 *     OR progress_max_quiet_us). All edge-triggered: stall_reason set
 *     to non-zero means "we already fired"; child clearing it by
 *     ticking re-arms the edge.
 *   - Registry mutex protects the array of pointers; supervisor
 *     snapshots pointers under lock, then releases the lock BEFORE
 *     invoking any callback — callbacks may freely call back into the
 *     supervisor API. */

#include "platform/time_compat.h"
#include "util/supervisor.h"

#include "json/json.h"
#include "util/thread_registry.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── Internal state ────────────────────────────────────────────────── */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static struct liveness_contract *g_contracts[SUPERVISOR_CAP];
static supervisor_domain_t      *g_contract_domains[SUPERVISOR_CAP];
static int                       g_contract_count = 0;

struct supervisor_domain {
    char label[SUPERVISOR_NAME_MAX];
};

static supervisor_domain_t       g_domains[SUPERVISOR_DOMAIN_CAP];
static int                       g_domain_count = 0;

static _Atomic bool   g_running       = false;
static _Atomic bool   g_thread_alive  = false;
static _Atomic int    g_tick_ms       = 1000;
static pthread_t      g_thread_id;
static _Atomic bool   g_thread_handle_set = false;

const char *supervisor_stall_reason_name(enum supervisor_stall_reason r)
{
    switch (r) {
    case SUPERVISOR_STALL_NONE:             return "none";
    case SUPERVISOR_STALL_TIME_DEADLINE:    return "time_deadline";
    case SUPERVISOR_STALL_NO_PROGRESS:      return "no_progress";
    case SUPERVISOR_STALL_CHILD_REPORTED:   return "child_reported";
    case SUPERVISOR_STALL_REPEATED_RESTART: return "repeated_restart";
    }
    return "(invalid)";
}

const char *supervisor_restart_policy_name(enum supervisor_restart_policy p)
{
    switch (p) {
    case SUPERVISOR_RESTART_TEMPORARY: return "temporary";
    case SUPERVISOR_RESTART_TRANSIENT: return "transient";
    case SUPERVISOR_RESTART_PERMANENT: return "permanent";
    }
    return "(invalid)";
}

/* ── Init / registry ───────────────────────────────────────────────── */

void liveness_contract_init(struct liveness_contract *c, const char *name)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    if (name) {
        size_t n = strnlen(name, SUPERVISOR_NAME_MAX - 1);
        memcpy(c->name, name, n);
        c->name[n] = '\0';
    }
    c->parent = -1;
    /* All atomic fields zero-initialize to their value-equivalent of
     * the underlying type (0 for ints), which is correct for first use. */
    atomic_store(&c->last_tick_us, platform_time_monotonic_us());
    atomic_store(&c->progress_changed_at_us, platform_time_monotonic_us());
}

static supervisor_child_id supervisor_register_locked(
    supervisor_domain_t *domain, struct liveness_contract *c)
{
    if (!c) return SUPERVISOR_INVALID_ID;
    if (g_contract_count >= SUPERVISOR_CAP) {
        fprintf(stderr,  // obs-ok:pre-existing-diagnostic
            "[supervisor] FAIL register '%s': registry full (cap=%d)\n",
            c->name, SUPERVISOR_CAP);
        return SUPERVISOR_INVALID_ID;
    }
    /* Reject duplicate name (helps tests + catches double-registers). */
    for (int i = 0; i < g_contract_count; i++) {
        if (strncmp(g_contracts[i]->name, c->name, SUPERVISOR_NAME_MAX) == 0) {
            fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                "[supervisor] FAIL register '%s': duplicate name\n", c->name);
            return SUPERVISOR_INVALID_ID;
        }
    }
    int id = g_contract_count++;
    g_contracts[id] = c;
    g_contract_domains[id] = domain;
    return id;
}

supervisor_child_id supervisor_register(struct liveness_contract *c)
{
    pthread_mutex_lock(&g_lock);
    supervisor_child_id id = supervisor_register_locked(NULL, c);
    pthread_mutex_unlock(&g_lock);
    return id;
}

supervisor_domain_t *supervisor_create_domain(const char *label)
{
    if (!label || !label[0]) return NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_domain_count; i++) {
        if (strncmp(g_domains[i].label, label, SUPERVISOR_NAME_MAX) == 0) {
            pthread_mutex_unlock(&g_lock);
            return &g_domains[i];
        }
    }
    if (g_domain_count >= SUPERVISOR_DOMAIN_CAP) {
        pthread_mutex_unlock(&g_lock);
        fprintf(stderr,  // obs-ok:supervisor-domain-full
            "[supervisor] FAIL create domain '%s': domain registry full (cap=%d)\n",
            label, SUPERVISOR_DOMAIN_CAP);
        return NULL;
    }
    supervisor_domain_t *domain = &g_domains[g_domain_count++];
    memset(domain, 0, sizeof(*domain));
    size_t n = strnlen(label, SUPERVISOR_NAME_MAX - 1);
    memcpy(domain->label, label, n);
    domain->label[n] = '\0';
    pthread_mutex_unlock(&g_lock);
    return domain;
}

supervisor_child_id supervisor_register_in_domain(
    supervisor_domain_t *domain, struct liveness_contract *c)
{
    if (!domain) return SUPERVISOR_INVALID_ID;
    pthread_mutex_lock(&g_lock);
    supervisor_child_id id = supervisor_register_locked(domain, c);
    pthread_mutex_unlock(&g_lock);
    return id;
}

void supervisor_unregister(supervisor_child_id id)
{
    pthread_mutex_lock(&g_lock);
    if (id < 0 || id >= g_contract_count) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    /* Compact: move last into the freed slot. Note: this changes the
     * effective id of the last child. Unregister is rare (test teardown
     * + shutdown), so we accept the rename rather than holding zombie
     * slots. */
    g_contracts[id] = g_contracts[g_contract_count - 1];
    g_contract_domains[id] = g_contract_domains[g_contract_count - 1];
    g_contracts[g_contract_count - 1] = NULL;
    g_contract_domains[g_contract_count - 1] = NULL;
    g_contract_count--;
    pthread_mutex_unlock(&g_lock);
}

/* ── Child-side O(1) helpers ───────────────────────────────────────── */

static struct liveness_contract *contract_for(supervisor_child_id id)
{
    /* Reads under lock; the returned pointer is to caller-owned static
     * storage, so dereferencing without the lock is safe. */
    pthread_mutex_lock(&g_lock);
    struct liveness_contract *c = NULL;
    if (id >= 0 && id < g_contract_count) c = g_contracts[id];
    pthread_mutex_unlock(&g_lock);
    return c;
}

void supervisor_child_complete(supervisor_child_id id)
{
    struct liveness_contract *c = contract_for(id);
    if (!c) return;

    atomic_store(&c->period_secs, 0);
    atomic_store(&c->deadline_secs, 0);
    atomic_store(&c->progress_max_quiet_us, 0);
    atomic_store(&c->stall_reason, SUPERVISOR_STALL_NONE);
    atomic_store(&c->last_tick_us, platform_time_monotonic_us());
    atomic_store(&c->completed, true);
}

void supervisor_tick(supervisor_child_id id)
{
    struct liveness_contract *c = contract_for(id);
    if (!c) return;
    if (atomic_load(&c->completed)) return;
    atomic_store(&c->last_tick_us, platform_time_monotonic_us());
    atomic_fetch_add(&c->ticks_run, 1u);
    /* Edge-rearm: ticking clears a TIME_DEADLINE stall. NO_PROGRESS
     * is cleared by progress changes, not by ticks. */
    int prev = atomic_load(&c->stall_reason);
    if (prev == SUPERVISOR_STALL_TIME_DEADLINE ||
        prev == SUPERVISOR_STALL_CHILD_REPORTED) {
        atomic_store(&c->stall_reason, SUPERVISOR_STALL_NONE);
    }
}

void supervisor_progress(supervisor_child_id id, int64_t marker)
{
    struct liveness_contract *c = contract_for(id);
    if (!c) return;
    if (atomic_load(&c->completed)) return;
    int64_t prev = atomic_load(&c->progress_marker);
    if (marker != prev) {
        atomic_store(&c->progress_marker, marker);
        atomic_store(&c->progress_changed_at_us, platform_time_monotonic_us());
        /* Progress rearm: clears NO_PROGRESS. */
        int sr = atomic_load(&c->stall_reason);
        if (sr == SUPERVISOR_STALL_NO_PROGRESS)
            atomic_store(&c->stall_reason, SUPERVISOR_STALL_NONE);
    }
}

void supervisor_report_stall(supervisor_child_id id,
                             enum supervisor_stall_reason r)
{
    struct liveness_contract *c = contract_for(id);
    if (!c) return;
    if (atomic_load(&c->completed)) return;
    /* Only fire on the rising edge. */
    int expected = SUPERVISOR_STALL_NONE;
    if (atomic_compare_exchange_strong(&c->stall_reason, &expected, (int)r)) {
        atomic_fetch_add(&c->stall_fires, 1u);
        if (c->on_stall) c->on_stall(c);
    }
}

void supervisor_set_period(supervisor_child_id id, int64_t secs)
{
    struct liveness_contract *c = contract_for(id);
    if (!c) {
        fprintf(stderr,  // obs-ok:supervisor-invalid-child
            "[supervisor] set_period: invalid/unregistered child_id=%d — "
            "ignored (liveness contract keeps its default)\n", (int)id);
        return;
    }
    atomic_store(&c->period_secs, secs);
}

void supervisor_set_deadline(supervisor_child_id id, int64_t secs)
{
    struct liveness_contract *c = contract_for(id);
    if (!c) {
        fprintf(stderr,  // obs-ok:supervisor-invalid-child
            "[supervisor] set_deadline: invalid/unregistered child_id=%d — "
            "ignored (a stale deadline is a liveness hazard)\n", (int)id);
        return;
    }
    atomic_store(&c->deadline_secs, secs);
}

void supervisor_set_progress_max_quiet(supervisor_child_id id,
                                       int64_t microseconds)
{
    struct liveness_contract *c = contract_for(id);
    if (!c) {
        fprintf(stderr,  // obs-ok:supervisor-invalid-child
            "[supervisor] set_progress_max_quiet: invalid/unregistered "
            "child_id=%d — ignored\n", (int)id);
        return;
    }
    atomic_store(&c->progress_max_quiet_us, microseconds);
}

void supervisor_set_restart_policy(supervisor_child_id id,
                                   enum supervisor_restart_policy policy,
                                   int64_t intensity_max, int64_t period_secs)
{
    struct liveness_contract *c = contract_for(id);
    if (!c) {
        fprintf(stderr,  // obs-ok:supervisor-invalid-child
            "[supervisor] set_restart_policy: invalid/unregistered child_id=%d "
            "— ignored (child keeps the default TEMPORARY policy)\n", (int)id);
        return;
    }
    if (intensity_max < 1) intensity_max = 1;
    atomic_store(&c->restart_intensity_max, intensity_max);
    atomic_store(&c->restart_period_us,
                 period_secs > 0 ? period_secs * 1000000 : 0);
    atomic_store(&c->restart_window_start_us, platform_time_monotonic_us());
    atomic_store(&c->restarts_in_window, 0u);
    atomic_store(&c->restart_policy, (int)policy);
}

void supervisor_worker_alive(supervisor_child_id id)
{
    struct liveness_contract *c = contract_for(id);
    if (!c) return;
    atomic_store(&c->worker_state, SUPERVISOR_WORKER_ALIVE);
}

void supervisor_worker_exited(supervisor_child_id id)
{
    struct liveness_contract *c = contract_for(id);
    if (!c) return;
    atomic_store(&c->worker_state, SUPERVISOR_WORKER_EXITED);
}

/* ── Supervisor loop ───────────────────────────────────────────────── */

/* Bounded restart engine (OTP intensity/period). Runs on the supervisor
 * thread, single-writer for every field it touches here. Called once per child
 * per sweep, BEFORE the stall block so a storm escalation sets stall_reason and
 * the stall block then skips the child. Pure contract math + the on_respawn
 * callback — no pthread/thread_registry knowledge (that lives in the caller's
 * on_respawn, e.g. util/thread_liveness.c), which keeps this unit-testable. */
static void maybe_restart(struct liveness_contract *c, int64_t now)
{
    int policy = atomic_load(&c->restart_policy);
    if (policy == SUPERVISOR_RESTART_TEMPORARY) return;   /* opt-in only */
    if (!c->on_respawn) return;
    if (atomic_load(&c->completed)) return;
    if (atomic_load(&c->worker_state) != SUPERVISOR_WORKER_EXITED) return;
    /* Already gave up (sticky): never respawn again this boot. */
    if (atomic_load(&c->stall_reason) == SUPERVISOR_STALL_REPEATED_RESTART)
        return;

    /* Roll the intensity window forward if it has elapsed. */
    int64_t win_us = atomic_load(&c->restart_period_us);
    if (win_us > 0) {
        int64_t start = atomic_load(&c->restart_window_start_us);
        if ((now - start) > win_us) {
            atomic_store(&c->restart_window_start_us, now);
            atomic_store(&c->restarts_in_window, 0u);
        }
    }

    /* Storm cap: the (N+1)th restart inside the window escalates instead of
     * respawning. Sticky REPEATED_RESTART ⇒ node stays alive + named, never
     * infinite-spawns. */
    int64_t cap = atomic_load(&c->restart_intensity_max);
    if (cap < 1) cap = 1;
    if ((int64_t)atomic_load(&c->restarts_in_window) >= cap) {
        int expected = SUPERVISOR_STALL_NONE;
        if (atomic_compare_exchange_strong(&c->stall_reason,
                &expected, SUPERVISOR_STALL_REPEATED_RESTART)) {
            atomic_fetch_add(&c->stall_fires, 1u);
            if (c->on_stall) c->on_stall(c);
        }
        return;
    }

    /* Claim the death by CAS EXITED → RESTARTING. RESTARTING is a state only
     * the supervisor writes, so it cannot clobber a fast new worker's EXITED
     * (the lost-signal race): on_respawn never writes ALIVE — the fresh worker
     * does, on entry. On a false EXITED (worker still terminating) on_respawn
     * restores EXITED and returns false, so we count nothing and retry next
     * sweep. The single supervisor thread is the only claimer, so the CAS never
     * contends with itself. */
    int expected = SUPERVISOR_WORKER_EXITED;
    if (!atomic_compare_exchange_strong(&c->worker_state, &expected,
                                        SUPERVISOR_WORKER_RESTARTING))
        return;   /* state moved under us; re-check next sweep */
    if (c->on_respawn(c)) {
        atomic_fetch_add(&c->restarts_in_window, 1u);
        atomic_fetch_add(&c->restart_count, 1u);
        atomic_store(&c->last_restart_us, now);
    }
}

static void sweep_once(void)
{
    int64_t now = platform_time_monotonic_us();

    /* Snapshot the list of pointers under lock; release before
     * invoking any callback so callbacks can re-enter the API safely. */
    struct liveness_contract *snap[SUPERVISOR_CAP];
    int n;
    pthread_mutex_lock(&g_lock);
    n = g_contract_count;
    memcpy(snap, g_contracts, (size_t)n * sizeof(snap[0]));
    pthread_mutex_unlock(&g_lock);

    for (int i = 0; i < n; i++) {
        /* Shutdown gate: once process shutdown is requested, dispatch no
         * further callbacks. A staged-sync on_tick drains validation work
         * that can run for seconds and must not outlive the chainstate
         * app_shutdown frees; the loop predicate alone cannot stop a
         * sweep already in flight. */
        if (thread_registry_shutdown_requested()) return;
        struct liveness_contract *c = snap[i];
        if (!c) continue;
        if (atomic_load(&c->completed)) continue;

        int64_t last_tick = atomic_load(&c->last_tick_us);
        int64_t period_s  = atomic_load(&c->period_secs);
        int64_t period_u  = atomic_load(&c->period_us);
        int64_t deadl_s   = atomic_load(&c->deadline_secs);
        int64_t quiet_us  = atomic_load(&c->progress_max_quiet_us);

        /* Effective tick window: period_us (sub-second override) wins when set,
         * else period_secs. Default period_us=0 ⇒ byte-identical to before for
         * every child that does not opt into the sub-second cadence. */
        int64_t period_window_us = period_u > 0 ? period_u
                                                : period_s * 1000000;

        /* Periodic on_tick driving. Supervisor calls on_tick when the
         * configured period has elapsed; on_tick is expected to do the
         * child's actual work and then call supervisor_tick to record
         * the heartbeat (or do work that emits supervisor_progress). */
        if (period_window_us > 0 && (now - last_tick) >= period_window_us) {
            if (c->on_tick) c->on_tick(c);
            /* If on_tick didn't call supervisor_tick itself, stamp it
             * now so we don't busy-fire. */
            int64_t after = atomic_load(&c->last_tick_us);
            if (after == last_tick) {
                atomic_store(&c->last_tick_us, platform_time_monotonic_us());
                atomic_fetch_add(&c->ticks_run, 1u);
            }
        }

        /* Bounded restart policy (OTP): a restartable child whose worker has
         * EXITED is respawned here, or escalated to REPEATED_RESTART on storm.
         * Runs before the stall block: a storm sets stall_reason, which the
         * block below then honors (skips). TEMPORARY children are a no-op. */
        maybe_restart(c, now);

        /* Edge-triggered stall fires. Only on the rising edge:
         * stall_reason transitions NONE → something. */
        int sr = atomic_load(&c->stall_reason);
        if (sr != SUPERVISOR_STALL_NONE) continue;

        /* Reload last_tick_us in case on_tick just bumped it. */
        int64_t lt = atomic_load(&c->last_tick_us);

        if (deadl_s > 0 && (now - lt) >= deadl_s * 1000000) {
            int expected = SUPERVISOR_STALL_NONE;
            if (atomic_compare_exchange_strong(&c->stall_reason,
                    &expected, SUPERVISOR_STALL_TIME_DEADLINE)) {
                atomic_fetch_add(&c->stall_fires, 1u);
                if (c->on_stall) c->on_stall(c);
            }
            continue;
        }

        if (quiet_us > 0) {
            int64_t changed = atomic_load(&c->progress_changed_at_us);
            if ((now - changed) >= quiet_us) {
                int expected = SUPERVISOR_STALL_NONE;
                if (atomic_compare_exchange_strong(&c->stall_reason,
                        &expected, SUPERVISOR_STALL_NO_PROGRESS)) {
                    atomic_fetch_add(&c->stall_fires, 1u);
                    if (c->on_stall) c->on_stall(c);
                }
            }
        }
    }
}

static void *supervisor_thread_main(void *arg)
{
    (void)arg;
    atomic_store(&g_thread_alive, true);
    while (atomic_load(&g_running) &&
           !thread_registry_shutdown_requested())
    {
        sweep_once();
        int ms = atomic_load(&g_tick_ms);
        if (ms < 1) ms = 1;
        if (ms > 60000) ms = 60000;
        struct timespec req = { ms / 1000, (long)(ms % 1000) * 1000000L };
        nanosleep(&req, NULL);
    }
    atomic_store(&g_thread_alive, false);
    thread_registry_unregister_self();
    return NULL;
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

bool supervisor_start(void)
{
    bool was = atomic_exchange(&g_running, true);
    if (was) return true;        /* idempotent */
    atomic_store(&g_thread_alive, false);

    pthread_t tid;
    int rc = thread_registry_spawn("zcl_supervisor",
                                       supervisor_thread_main, NULL, &tid);
    if (rc != 0) {
        atomic_store(&g_running, false);
        fprintf(stderr,
            "[supervisor] FAIL thread_registry_spawn rc=%d\n", rc);
        return false;
    }
    g_thread_id = tid;
    atomic_store(&g_thread_handle_set, true);
    return true;
}

void supervisor_stop(void)
{
    if (!atomic_load(&g_running)) return;
    atomic_store(&g_running, false);

    /* Join the loop. It polls g_running and the global shutdown flag
     * between sweeps, and sweep_once dispatches no callbacks once
     * shutdown is requested, so the wait is bounded by the one in-flight
     * on_tick. On the shutdown path that tick can be a multi-second
     * staged-sync drain reading the chainstate app_shutdown frees next —
     * abandoning the thread there is a use-after-free, so keep re-arming
     * the 2 s join with progress logs (the alarm() watchdog in
     * app_shutdown_svc is the hard backstop for a truly hung tick).
     * Non-shutdown callers (test teardown) keep the single ≤2 s attempt. */
    if (atomic_load(&g_thread_handle_set)) {
        for (;;) {
            struct timespec deadline;
            platform_time_realtime_timespec(&deadline);
            deadline.tv_sec += 2;
            int rc = pthread_timedjoin_np(g_thread_id, NULL, &deadline);
            if (rc == 0) {
                atomic_store(&g_thread_handle_set, false);
                break;
            }
            if (rc != ETIMEDOUT || !thread_registry_shutdown_requested()) {
                fprintf(stderr,  // obs-ok:pre-existing-diagnostic
                    "[supervisor] WARN supervisor_stop join rc=%d (thread still alive)\n",
                    rc);
                break;
            }
            fprintf(stderr,  // obs-ok:shutdown-join-progress
                "[supervisor] shutdown join: in-flight tick still draining; waiting\n");
        }
    }
}

void supervisor_set_tick_ms_for_testing(int ms)
{
    if (ms < 1) ms = 1;
    atomic_store(&g_tick_ms, ms);
}

void supervisor_request_min_tick_ms(int ms)
{
    if (ms < 1) return;              /* no-op on a nonsense request */
    if (ms > 60000) ms = 60000;      /* mirror the loop's own clamp */
    /* Monotonic min via a CAS loop: never RAISE the interval (another
     * accelerated child may have already lowered it), only lower it. */
    int cur = atomic_load(&g_tick_ms);
    while (ms < cur) {
        if (atomic_compare_exchange_weak(&g_tick_ms, &cur, ms))
            break;
        /* cur reloaded by the CAS; loop re-checks ms < cur. */
    }
}

#ifdef ZCL_TESTING
void supervisor_reset_for_testing(void)
{
    /* Stop first so the sweeper isn't iterating while we clear. */
    supervisor_stop();
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < SUPERVISOR_CAP; i++) g_contracts[i] = NULL;
    for (int i = 0; i < SUPERVISOR_CAP; i++) g_contract_domains[i] = NULL;
    g_contract_count = 0;
    memset(g_domains, 0, sizeof(g_domains));
    g_domain_count = 0;
    pthread_mutex_unlock(&g_lock);
    atomic_store(&g_tick_ms, 1000);
}

void supervisor_sweep_once_for_testing(void)
{
    sweep_once();
}
#endif

/* ── Introspection ─────────────────────────────────────────────────── */

int supervisor_child_count_total(void)
{
    pthread_mutex_lock(&g_lock);
    int n = g_contract_count;
    pthread_mutex_unlock(&g_lock);
    return n;
}

int supervisor_snapshot_all(struct supervisor_snapshot *out, int max)
{
    if (!out || max <= 0) return 0;
    int64_t now = platform_time_monotonic_us();
    pthread_mutex_lock(&g_lock);
    int n = g_contract_count;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        const struct liveness_contract *c = g_contracts[i];
        memset(&out[i], 0, sizeof(out[i]));
        memcpy(out[i].name, c->name, sizeof(out[i].name));
        out[i].parent           = c->parent;
        int64_t lt              = atomic_load(&c->last_tick_us);
        out[i].last_tick_age_us = now - lt;
        out[i].progress_marker  = atomic_load(&c->progress_marker);
        out[i].period_secs      = atomic_load(&c->period_secs);
        out[i].period_us        = atomic_load(&c->period_us);
        out[i].deadline_secs    = atomic_load(&c->deadline_secs);
        out[i].completed        = atomic_load(&c->completed);
        out[i].stall_reason     = atomic_load(&c->stall_reason);
        out[i].ticks_run        = atomic_load(&c->ticks_run);
        out[i].stall_fires      = atomic_load(&c->stall_fires);
        out[i].restart_count    = atomic_load(&c->restart_count);
        out[i].restart_policy   = atomic_load(&c->restart_policy);
        out[i].worker_state     = atomic_load(&c->worker_state);
        out[i].restarts_in_window = atomic_load(&c->restarts_in_window);
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

bool supervisor_dump_state_json(struct json_value *out, const char *key)
{
    if (!out) return false;
    json_set_object(out);
    json_push_kv_bool(out, "running",      atomic_load(&g_running));
    json_push_kv_bool(out, "thread_alive", atomic_load(&g_thread_alive));
    json_push_kv_int (out, "tick_ms",      atomic_load(&g_tick_ms));

    if (key && key[0]) {
        pthread_mutex_lock(&g_lock);
        supervisor_domain_t *domain = NULL;
        for (int i = 0; i < g_domain_count; i++) {
            if (strncmp(g_domains[i].label, key, SUPERVISOR_NAME_MAX) == 0) {
                domain = &g_domains[i];
                break;
            }
        }
        pthread_mutex_unlock(&g_lock);
        return supervisor_domain_dump_state_json(domain, out);
    }

    json_push_kv_int(out, "child_count", supervisor_child_count_total());

    struct json_value domains;
    json_init(&domains);
    json_set_array(&domains);

    pthread_mutex_lock(&g_lock);
    supervisor_domain_t *domain_snap[SUPERVISOR_DOMAIN_CAP];
    int dn = g_domain_count;
    for (int i = 0; i < dn; i++) domain_snap[i] = &g_domains[i];
    pthread_mutex_unlock(&g_lock);

    for (int i = 0; i < dn; i++) {
        struct json_value domain_json;
        json_init(&domain_json);
        if (supervisor_domain_dump_state_json(domain_snap[i], &domain_json)) {
            json_push_back(&domains, &domain_json);
        }
        json_free(&domain_json);
    }
    json_push_kv(out, "domains", &domains);
    json_free(&domains);

    struct json_value orphans;
    json_init(&orphans);
    json_set_array(&orphans);
    struct supervisor_domain root_domain;
    memset(&root_domain, 0, sizeof(root_domain));
    if (supervisor_domain_dump_state_json(&root_domain, &orphans)) {
        const struct json_value *kids = json_get(&orphans, "children");
        if (kids) json_push_kv(out, "root_orphans", kids);
    } else {
        json_push_kv(out, "root_orphans", &orphans);
    }
    json_free(&orphans);
    return true;
}

static void push_contract_json(struct json_value *arr,
                               const struct liveness_contract *c,
                               int64_t now)
{
    struct json_value child;
    json_init(&child);
    json_set_object(&child);
    json_push_kv_str (&child, "name",   c->name);
    json_push_kv_int (&child, "parent", c->parent);
    int64_t lt = atomic_load(&c->last_tick_us);
    json_push_kv_int (&child, "last_tick_age_us", now - lt);
    json_push_kv_int (&child, "progress_marker",
                      atomic_load(&c->progress_marker));
    json_push_kv_int (&child, "period_secs",
                      atomic_load(&c->period_secs));
    json_push_kv_int (&child, "period_us",
                      atomic_load(&c->period_us));
    json_push_kv_int (&child, "deadline_secs",
                      atomic_load(&c->deadline_secs));
    json_push_kv_bool(&child, "completed",
                      atomic_load(&c->completed));
    json_push_kv_str (&child, "stall_reason",
                      supervisor_stall_reason_name(
                          (enum supervisor_stall_reason)
                          atomic_load(&c->stall_reason)));
    json_push_kv_int (&child, "ticks_run",
                      atomic_load(&c->ticks_run));
    json_push_kv_int (&child, "stall_fires",
                      atomic_load(&c->stall_fires));
    json_push_kv_int (&child, "restart_count",
                      atomic_load(&c->restart_count));
    json_push_kv_str (&child, "restart_policy",
                      supervisor_restart_policy_name(
                          (enum supervisor_restart_policy)
                          atomic_load(&c->restart_policy)));
    json_push_kv_int (&child, "restarts_in_window",
                      atomic_load(&c->restarts_in_window));
    json_push_back(arr, &child);
    json_free(&child);
}

bool supervisor_domain_dump_state_json(supervisor_domain_t *domain,
                                       struct json_value *out)
{
    if (!domain || !out) return false;
    bool root = (domain->label[0] == '\0');
    json_set_object(out);
    json_push_kv_str(out, "name", root ? "root" : domain->label);

    struct liveness_contract *snap[SUPERVISOR_CAP];
    int n = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_contract_count && n < SUPERVISOR_CAP; i++) {
        bool match = root ? (g_contract_domains[i] == NULL)
                          : (g_contract_domains[i] == domain);
        if (match) snap[n++] = g_contracts[i];
    }
    pthread_mutex_unlock(&g_lock);

    json_push_kv_int(out, "child_count", n);
    struct json_value children;
    json_init(&children);
    json_set_array(&children);
    int64_t now = platform_time_monotonic_us();
    for (int i = 0; i < n; i++) {
        if (snap[i]) push_contract_json(&children, snap[i], now);
    }
    json_push_kv(out, "children", &children);
    json_free(&children);
    return true;
}
