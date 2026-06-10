/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Supervisor — a typed liveness contract + a dedicated time-driven
 * thread that owns restart/escalate policy for long-running children.
 *
 * Why this exists
 * ----------------
 * Before Round 5, every periodic check in the node ticked on a single
 * shared sweeper thread (`zcl_health_sweep` in lib/health/heartbeat.c).
 * On 2026-05-21 that sweeper wedged for 8.6 h: `sync_watchdog.checks_run`
 * stayed at 0 the whole time, no `EV_TIP_STALE` fired, and the node
 * sat 4,428 blocks behind the legacy daemon with `download.requested=0`
 * and 0 outbound peers — silently — because the very thing that would
 * notice (the watchdog) lives on the same dead thread.
 *
 * The architectural fault was not the watchdog itself but its driver:
 * a single point of failure for all periodic work. The fix is
 * structural: have one *independent* time-driven supervisor whose only
 * job is to tick children's `on_tick` callbacks and fire `on_stall` when
 * a child's heartbeat lapses or its progress marker freezes.
 *
 * Modern-architecture analogues this implements (without ceremony):
 *   - Erlang/OTP supervisor trees: typed restart policy at a parent
 *     boundary, "let it crash" via signal-only callback.
 *   - Kubernetes controller reconciliation: desired vs. actual state,
 *     converge or escalate on a clock independent of the workload.
 *   - LMAX Disruptor single-writer principle: supervisor is the sole
 *     writer of status fields; children are the sole writers of
 *     heartbeat fields. No mutex.
 *
 * Children publish a `struct liveness_contract`, register it once at
 * boot, and either heartbeat (call supervisor_tick) on their own loop
 * OR let the supervisor drive them via `period_secs > 0 ⇒ on_tick`.
 * When a deadline is missed, the supervisor calls `on_stall(self)` on
 * its own thread, edge-triggered. The child observes the stall flag
 * on its next loop and re-initializes — supervisor never tears down a
 * child mid-`AR_BEGIN_SAVE`.
 *
 * The supervisor thread itself does *no* I/O, no DB, no RPC. Just
 * snapshot atomics + dispatch callbacks. Callbacks may freely call any
 * other supervisor_* function; the supervisor never holds a lock when
 * invoking a callback. */

#ifndef ZCL_SUPERVISOR_H
#define ZCL_SUPERVISOR_H

#include <stdbool.h>
#include <stdint.h>

#define SUPERVISOR_NAME_MAX 40
#define SUPERVISOR_CAP      32
#define SUPERVISOR_DOMAIN_CAP 16

typedef int supervisor_child_id;
#define SUPERVISOR_INVALID_ID (-1)

typedef struct supervisor_domain supervisor_domain_t;

/* Typed stall causes. Mirrors `enum watchdog_recovery_type` in concept
 * but lives in lib/util so it does not pull in app-side headers
 * (gate #14). */
enum supervisor_stall_reason {
    SUPERVISOR_STALL_NONE = 0,
    SUPERVISOR_STALL_TIME_DEADLINE,    /* no tick within deadline_secs */
    SUPERVISOR_STALL_NO_PROGRESS,      /* progress_marker frozen */
    SUPERVISOR_STALL_CHILD_REPORTED,   /* child set the reason itself */
    SUPERVISOR_STALL_REPEATED_RESTART, /* circuit breaker tripped */
};

const char *supervisor_stall_reason_name(enum supervisor_stall_reason r);

struct json_value; /* fwd; see json/json.h */

/* The liveness contract. Caller (the child being supervised) owns a
 * *static* instance of this struct. After `liveness_contract_init` +
 * `supervisor_register`, the supervisor keeps a pointer to it.
 *
 * Ownership rules (load-bearing):
 *   - Child writes:   last_tick_us, progress_marker, may set
 *                     stall_reason = SUPERVISOR_STALL_CHILD_REPORTED.
 *   - Supervisor writes: all status counters + non-CHILD stall_reason.
 *   - Configuration fields (period_secs, deadline_secs,
 *     progress_max_quiet_us) are written once at init and may be
 *     atomically retuned at runtime via supervisor_set_*.
 *   - vtable fields (on_tick, on_stall, ctx) are set at init and never
 *     mutated thereafter.
 *
 * All cross-thread fields are `_Atomic`. No mutex anywhere. */
struct liveness_contract {
    /* identity (supervisor-owned, set at register-time) */
    char name[SUPERVISOR_NAME_MAX];
    int  parent;                       /* parent child_id or -1 */

    /* heartbeat (child-owned; CLOCK_MONOTONIC microseconds) */
    _Atomic int64_t last_tick_us;
    /* monotonic progress signal (child-owned). Updated by child; a
     * frozen marker for `progress_max_quiet_us` triggers NO_PROGRESS. */
    _Atomic int64_t progress_marker;

    /* configuration (atomic-mutable at runtime) */
    _Atomic int64_t period_secs;       /* >0 ⇒ supervisor drives on_tick */
    _Atomic int64_t deadline_secs;     /* >0 ⇒ stall if last_tick lapses */
    _Atomic int64_t progress_max_quiet_us;
                                       /* >0 ⇒ stall if marker frozen */

    /* status (supervisor-owned, atomic-readable from anywhere) */
    _Atomic int      stall_reason;     /* enum supervisor_stall_reason */
    _Atomic uint32_t ticks_run;
    _Atomic uint32_t stall_fires;
    _Atomic uint32_t restart_count;
    _Atomic int64_t  last_restart_us;

    /* internal tracking (supervisor-owned) */
    _Atomic int64_t  progress_changed_at_us;
    _Atomic int64_t  last_progress_seen;

    /* vtable (set at init; never mutated after register) */
    void  *ctx;
    void (*on_tick)(struct liveness_contract *self);
    void (*on_stall)(struct liveness_contract *self);
};

/* Initialize a contract in-place. Safe to call on uninit memory.
 * `name` is copied (truncated to SUPERVISOR_NAME_MAX-1). */
void liveness_contract_init(struct liveness_contract *c, const char *name);

/* Register a caller-owned static contract. Returns the child id (>=0)
 * or SUPERVISOR_INVALID_ID on registry-full. The supervisor stores the
 * pointer — do NOT pass a stack-allocated contract. */
supervisor_child_id supervisor_register(struct liveness_contract *c);

/* Create or return a named domain supervisor. Children registered in a
 * domain still use the same global child ids, tick loop, and lifecycle;
 * the domain is an operator-facing grouping boundary for dumps and
 * future restart policy. */
supervisor_domain_t *supervisor_create_domain(const char *label);

supervisor_child_id supervisor_register_in_domain(
    supervisor_domain_t *domain,
    struct liveness_contract *c);

bool supervisor_domain_dump_state_json(supervisor_domain_t *domain,
                                       struct json_value *out);

int supervisor_child_count_total(void);

/* Remove a contract from the registry. Idempotent. */
void supervisor_unregister(supervisor_child_id id);

/* ── Child-side helpers (O(1) atomic stores) ───────────────────────── */

/* Record a heartbeat now. Resets the deadline timer. */
void supervisor_tick(supervisor_child_id id);

/* Update progress marker. If `marker` differs from the last observed
 * value, the supervisor will note the change time for stall detection. */
void supervisor_progress(supervisor_child_id id, int64_t marker);

/* Child signals it is stuck (used when the child itself notices a
 * deadlock before the supervisor's deadline fires). Edge-triggered. */
void supervisor_report_stall(supervisor_child_id id,
                             enum supervisor_stall_reason r);

/* Runtime knobs (atomic). Pass 0 to disable a particular gate. */
void supervisor_set_period(supervisor_child_id id,   int64_t secs);
void supervisor_set_deadline(supervisor_child_id id, int64_t secs);
void supervisor_set_progress_max_quiet(supervisor_child_id id,
                                       int64_t microseconds);

/* ── Lifecycle ─────────────────────────────────────────────────────── */

/* Spawn the dedicated supervisor thread (name: "zcl_supervisor"). Safe
 * to call multiple times — second + subsequent calls return true without
 * spawning. Returns false if the thread could not be created. */
bool supervisor_start(void);

/* Request stop and join the supervisor thread. Outside process shutdown
 * the join gives up after ≤ 2 s; once shutdown is requested it keeps
 * waiting (progress-logged) for the in-flight tick so a draining stage
 * never outlives the chainstate app_shutdown frees. Safe to call without
 * a prior start; safe to call multiple times. */
void supervisor_stop(void);

/* Test hook: configure the loop period (default 1000 ms). Tests that
 * exercise stall detection use a small value (e.g. 10 ms) to avoid
 * real-time waits. Production never calls this. */
void supervisor_set_tick_ms_for_testing(int ms);

/* Test hook: clear the registry and reset internal state. Each test_*
 * function in the fork-based runner is its own process, so per-process
 * isolation is already free, but the sequential runner (test.c) calls
 * this to keep test_supervisor side-effect free. */
#ifdef ZCL_TESTING
void supervisor_reset_for_testing(void);
#endif

/* ── Introspection (zcl_state subsystem=supervisor) ────────────────── */

struct supervisor_snapshot {
    char     name[SUPERVISOR_NAME_MAX];
    int      parent;
    int64_t  last_tick_age_us;       /* now - last_tick_us */
    int64_t  progress_marker;
    int64_t  period_secs;
    int64_t  deadline_secs;
    int      stall_reason;           /* enum supervisor_stall_reason */
    uint32_t ticks_run;
    uint32_t stall_fires;
    uint32_t restart_count;
};

/* Fill `out` with up to `max` snapshots. Returns the number written. */
int  supervisor_snapshot_all(struct supervisor_snapshot *out, int max);

/* Dump-state-json wired into diagnostics_controller's `g_dumpers[]`.
 * `out` must be JSON-initialized by the caller. `key` is unused (one
 * dump returns all children). */
bool supervisor_dump_state_json(struct json_value *out, const char *key);

#endif /* ZCL_SUPERVISOR_H */
