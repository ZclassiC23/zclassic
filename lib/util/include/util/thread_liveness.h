/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * thread_liveness — a tiny adapter that puts a long-running background
 * thread onto the supervisor liveness tree with an honest contract, in
 * three lines at the call site:
 *
 *   static struct thread_liveness_child g_child = { .id = SUPERVISOR_INVALID_ID };
 *   ...start()... thread_liveness_register(&g_child, "zcl_foo", 120, 0);
 *   ...loop body... thread_liveness_beat(&g_child, work_counter);
 *   ...stop()...   thread_liveness_retire(&g_child);
 *
 * Why this exists
 * ----------------
 * Round 5 gave app-side services (disk_monitor, gap_fill, …) a supervisor
 * liveness contract, but the cross-cutting infrastructure threads that live
 * in lib/ and config/ (the health sweeper, the metrics printer, the async
 * event dispatcher, the RPC-timeout watchdog, the DB worker + WAL
 * checkpointer) were spawned through thread_registry_spawn and then never
 * appeared on the supervisor tree — a wedged loop there was silent, exactly
 * the failure mode Round 5 was built to make impossible. Those TUs cannot
 * include the app-side domain header (`supervisors/domains.h`) without a
 * lib-layering violation (Gate #15), so they register a ROOT child through
 * this lib/util adapter instead of a domain.
 *
 * Contract shape (see util/supervisor.h for the field semantics)
 * --------------------------------------------------------------
 *   - The child heartbeats itself (period_secs=0); the supervisor never
 *     drives on_tick. thread_liveness_beat() is the single heartbeat call
 *     the loop body makes — an atomic tick + optional atomic progress
 *     marker, zero behavior change on the thread's real work path.
 *   - deadline_secs > 0 arms the TIME_DEADLINE stall (the heartbeat
 *     lapsed). Use it for threads that tick on a bounded cadence.
 *   - progress_quiet_us > 0 arms the NO_PROGRESS stall (the marker froze).
 *     Use it when the thread does real, marker-advancing work on every
 *     healthy cadence.
 *   - Pass 0 for either gate to leave it disabled. A dormant-until-event
 *     thread (a condvar waiter that can idle indefinitely) registers with
 *     deadline_secs=0, progress_quiet_us=0 — it is present in the tree and
 *     heartbeats when it does work, but is never falsely flagged for
 *     legitimately sitting idle. Do NOT fake a progress marker for such a
 *     thread.
 *   - on_stall names a typed TRANSIENT blocker "thread_stalled_<name>";
 *     the next thread_liveness_beat() clears it.
 */

#ifndef ZCL_THREAD_LIVENESS_H
#define ZCL_THREAD_LIVENESS_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "util/blocker.h"
#include "util/supervisor.h"

/* Caller owns a *static* instance. Initialize `.id = SUPERVISOR_INVALID_ID`
 * at the declaration (a zero-initialized static would look like the valid
 * child id 0 and defeat the idempotent-register guard). All other fields
 * are set by thread_liveness_register(). */
struct thread_liveness_child {
    struct liveness_contract contract;   /* ctx points back to this struct */
    supervisor_child_id      id;
    _Atomic bool             blocker_standing;
    char                     blocker_id[BLOCKER_ID_MAX];
};

/* Register `c` as a supervised ROOT child named `name` (heartbeat-driven).
 * deadline_secs>0 arms the heartbeat-lapse stall; progress_quiet_us>0 arms
 * the frozen-marker stall; 0 disables that gate. Idempotent: a second call
 * on an already-registered child returns its existing id without
 * re-registering. Returns the child id, or SUPERVISOR_INVALID_ID on
 * registry-full. */
supervisor_child_id thread_liveness_register(struct thread_liveness_child *c,
                                             const char *name,
                                             int64_t deadline_secs,
                                             int64_t progress_quiet_us);

/* Heartbeat from the loop body. `progress` >= 0 publishes a monotonic
 * progress marker; pass -1 to record a heartbeat only. Clears a standing
 * "thread_stalled_<name>" blocker when the thread resumes ticking. O(1)
 * atomics only — no allocation, no locks on the hot path. Safe to call on
 * an unregistered child (no-op). */
void thread_liveness_beat(struct thread_liveness_child *c, int64_t progress);

/* Unregister on shutdown. Clears any standing blocker and removes the child
 * from the tree. Idempotent. */
void thread_liveness_retire(struct thread_liveness_child *c);

#endif /* ZCL_THREAD_LIVENESS_H */
