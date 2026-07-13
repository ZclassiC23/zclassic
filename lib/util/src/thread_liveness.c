/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * thread_liveness — see util/thread_liveness.h for the contract. This is the
 * single lib/util seam that registers cross-cutting infrastructure threads
 * (health sweep, metrics, event dispatch, RPC-timeout, DB worker/checkpoint)
 * as ROOT supervisor children, so a wedged loop in lib/ or config/ is a
 * named blocker instead of a silent stop. */

#include "util/thread_liveness.h"

#include "util/log_macros.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* Supervisor stall callback (runs on the supervisor thread, edge-triggered).
 * Name a typed TRANSIENT blocker so the wedge is visible via
 * `dumpstate blocker` / node.log; thread_liveness_beat() clears it the moment
 * the thread resumes ticking. */
static void tl_on_stall(struct liveness_contract *self)
{
    if (!self) return;
    struct thread_liveness_child *c =
        (struct thread_liveness_child *)self->ctx;
    if (!c) return;

    int r = atomic_load(&self->stall_reason);
    char reason[BLOCKER_REASON_MAX];
    snprintf(reason, sizeof reason,
             "supervised thread '%s' stopped heartbeating (%s) — its loop is "
             "wedged or exited without retiring; clears when it ticks again",
             self->name, supervisor_stall_reason_name(r));

    struct blocker_record rec;
    if (blocker_init(&rec, c->blocker_id, "thread_liveness",
                     BLOCKER_TRANSIENT, reason)) {
        (void)blocker_set(&rec);
        atomic_store(&c->blocker_standing, true);
    }

    LOG_WARN("thread_liveness",
             "[thread_liveness] %s stalled (%s) — named blocker %s",
             self->name, supervisor_stall_reason_name(r), c->blocker_id);
}

supervisor_child_id thread_liveness_register(struct thread_liveness_child *c,
                                             const char *name,
                                             int64_t deadline_secs,
                                             int64_t progress_quiet_us)
{
    if (!c || !name || !name[0]) return SUPERVISOR_INVALID_ID;
    if (c->id != SUPERVISOR_INVALID_ID) return c->id;  /* idempotent */

    liveness_contract_init(&c->contract, name);
    /* Child-driven heartbeat: the supervisor never calls on_tick. */
    atomic_store(&c->contract.period_secs, (int64_t)0);
    atomic_store(&c->contract.deadline_secs, deadline_secs);
    atomic_store(&c->contract.progress_max_quiet_us, progress_quiet_us);
    c->contract.ctx      = c;
    c->contract.on_tick  = NULL;
    c->contract.on_stall = tl_on_stall;

    atomic_store(&c->blocker_standing, false);
    snprintf(c->blocker_id, sizeof c->blocker_id, "thread_stalled_%s", name);

    c->id = supervisor_register(&c->contract);  // supervisor-root-ok:cross-cutting-infra-thread
    if (c->id == SUPERVISOR_INVALID_ID) {
        LOG_WARN("thread_liveness",
                 "[thread_liveness] register failed for '%s' — thread runs "
                 "UNsupervised this boot (registry full?)", name);
    }
    return c->id;
}

void thread_liveness_beat(struct thread_liveness_child *c, int64_t progress)
{
    if (!c || c->id == SUPERVISOR_INVALID_ID) return;
    if (progress >= 0)
        supervisor_progress(c->id, progress);
    supervisor_tick(c->id);
    if (atomic_load(&c->blocker_standing)) {
        blocker_clear(c->blocker_id);
        atomic_store(&c->blocker_standing, false);
    }
}

void thread_liveness_retire(struct thread_liveness_child *c)
{
    if (!c || c->id == SUPERVISOR_INVALID_ID) return;
    if (atomic_load(&c->blocker_standing)) {
        blocker_clear(c->blocker_id);
        atomic_store(&c->blocker_standing, false);
    }
    supervisor_unregister(c->id);
    c->id = SUPERVISOR_INVALID_ID;
}
