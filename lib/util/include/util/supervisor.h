/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Supervisor — a typed liveness contract + a dedicated time-driven
 * thread that owns restart/escalate policy for long-running children.
 *
 * Why this exists
 * ----------------
 * Previously, every periodic check in the node ticked on a single
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
/* Static registry capacity. Raised 32 -> 64 (2026-07-13, wf/universal-
 * supervision) when the cross-cutting infrastructure threads in lib/ and
 * config/ (health sweep, metrics, event dispatch, RPC-timeout, DB
 * worker/checkpoint — via util/thread_liveness.h) joined the tree as ROOT
 * children on top of the ~30 existing chain/net/op-domain children. Sizes
 * the g_contracts[]/snapshot stack arrays; keep generous headroom so a
 * register never silently drops a child on a fully-featured boot. */
#define SUPERVISOR_CAP      64
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

/* Erlang/OTP-style restart policy for a supervised child. Default is
 * TEMPORARY (== 0, the zero-initialized value) so a child that does NOT opt
 * in keeps the historical named-blocker-only behavior — the supervisor names a
 * typed blocker on stall and never respawns. A child opts in by declaring one
 * of the restart policies below AND providing an `on_respawn` callback.
 *
 * Which threads may be PERMANENT (safe to auto-restart): ONLY pure, stateless
 * periodic-loop workers with NO consensus/shared mutable state — re-entering
 * the loop from scratch must be a no-op on correctness. The consensus reducer
 * stages, the reducer drive, and the DB worker/checkpointer threads MUST stay
 * TEMPORARY: respawning them mid-fold is a correctness hazard, so they keep the
 * named-blocker + operator model. See docs/DEFENSIVE_CODING.md (Gate #23). */
enum supervisor_restart_policy {
    SUPERVISOR_RESTART_TEMPORARY = 0, /* never auto-restart (default) */
    SUPERVISOR_RESTART_TRANSIENT,     /* restart only on abnormal exit */
    SUPERVISOR_RESTART_PERMANENT,     /* always restart while the node runs */
};

const char *supervisor_restart_policy_name(enum supervisor_restart_policy p);

/* Liveness signal that distinguishes "thread died" from "thread slow". The
 * worker publishes it: ALIVE at loop entry (and on every heartbeat — a beating
 * thread is alive), EXITED right before it returns. The supervisor reads it to
 * decide restart-vs-stall. Honesty caveat: a hard SIGSEGV takes down the whole
 * process (POSIX C has no per-thread crash recovery), so the recoverable
 * failure is an abnormal RETURN from the worker loop, not an independent
 * segfault. The respawn path additionally reaps/guards with pthread_tryjoin_np
 * so a false EXITED (caught mid-return) can never double-spawn a live thread. */
enum supervisor_worker_state {
    SUPERVISOR_WORKER_UNKNOWN = 0,    /* never spawned / not tracked */
    SUPERVISOR_WORKER_ALIVE,          /* worker is running its loop (worker-set) */
    SUPERVISOR_WORKER_EXITED,         /* worker returned; candidate for restart
                                       * (worker-set, just before return) */
    SUPERVISOR_WORKER_RESTARTING,     /* supervisor claimed the death and is
                                       * respawning — a single-writer (supervisor)
                                       * state the worker never writes, so a fast
                                       * new worker's EXITED can never be lost to
                                       * the claim. Cleared to ALIVE by the fresh
                                       * worker entering its loop. */
};

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
    /* Sub-second tick override (microseconds). >0 takes precedence over
     * period_secs for the on_tick due-check; 0 (the default set by
     * liveness_contract_init) ⇒ use period_secs unchanged. Only the
     * mint/refold-accelerated staged-sync stages set this (gated on
     * refold_cadence_active); every other child leaves it 0, so their tick
     * cadence is byte-identical. */
    _Atomic int64_t period_us;
    _Atomic int64_t deadline_secs;     /* >0 ⇒ stall if last_tick lapses */
    _Atomic int64_t progress_max_quiet_us;
                                       /* >0 ⇒ stall if marker frozen */
    _Atomic bool    completed;         /* true ⇒ no tick or stall checks */

    /* status (supervisor-owned, atomic-readable from anywhere) */
    _Atomic int      stall_reason;     /* enum supervisor_stall_reason */
    _Atomic uint32_t ticks_run;
    _Atomic uint32_t stall_fires;
    _Atomic uint32_t restart_count;
    _Atomic int64_t  last_restart_us;

    /* restart policy (Erlang/OTP). Default TEMPORARY (0) ⇒ no auto-restart.
     *   - restart_policy:      child-set once via supervisor_set_restart_policy
     *   - worker_state:        child-published (ALIVE/EXITED); supervisor reads
     *   - restart_intensity_max / restart_period_us: the OTP intensity/period
     *     cap — at most N restarts within M microseconds; the (N+1)th trip
     *     inside the window escalates to a sticky SUPERVISOR_STALL_REPEATED_
     *     RESTART instead of infinite respawn.
     *   - restart_window_start_us / restarts_in_window: supervisor-owned window
     *     bookkeeping. */
    _Atomic int      restart_policy;        /* enum supervisor_restart_policy */
    _Atomic int      worker_state;          /* enum supervisor_worker_state */
    _Atomic int64_t  restart_intensity_max; /* N: max restarts per window */
    _Atomic int64_t  restart_period_us;     /* M: window length (us) */
    _Atomic int64_t  restart_window_start_us;
    _Atomic uint32_t restarts_in_window;

    /* internal tracking (supervisor-owned) */
    _Atomic int64_t  progress_changed_at_us;
    _Atomic int64_t  last_progress_seen;

    /* Tick-runner handoff. The sweep thread does ZERO I/O and never runs a
     * child's on_tick inline (that caused the 2026-07-19 jbd2_log_wait_commit
     * wedge, where a child's SQLite-commit tick froze the sweep heartbeat
     * >=30s and the backstop killed a healthy node). Instead the sweep sets
     * tick_pending when a period-driven child is due; the dedicated
     * "zcl_supervisor_tick_runner" thread CAS-consumes it and executes
     * on_tick + stamps last_tick/ticks_run. Supervisor+runner owned. */
    _Atomic bool     tick_pending;

    /* vtable (set at init; never mutated after register) */
    void  *ctx;
    void (*on_tick)(struct liveness_contract *self);
    void (*on_stall)(struct liveness_contract *self);
    /* Respawn callback (restartable children only). The supervisor calls it on
     * its own thread when the child's worker_state is EXITED and the restart
     * policy + intensity cap permit. It MUST re-create the worker thread (via
     * thread_registry_spawn) and return true when it has handled the death
     * (spawned a fresh worker, or definitively failed to and wants the attempt
     * counted); it returns false ONLY when the worker turned out to still be
     * alive (a false EXITED caught mid-return) so the supervisor does not count
     * it and re-checks next sweep. NULL ⇒ the child is never auto-restarted
     * regardless of policy. */
    bool (*on_respawn)(struct liveness_contract *self);
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

/* ── Independent liveness backstop (Pillar 7) ──────────────────────────
 * The root supervisor thread cannot register a liveness contract on the
 * tree it drives (Gate #23 exemption — see check_thread_supervision.sh),
 * so its ONLY exposed liveness signal is this counter. It increments once
 * per sweep_once() call — production thread loop AND the ZCL_TESTING
 * synchronous seam alike — BEFORE any child callback runs, so a thread
 * death or a hang inside a child's on_tick/on_stall freezes it forever.
 * An independent watcher (util/supervisor_backstop.h) and
 * boot_sd_watchdog.c both poll this with their OWN clock; no lock, no
 * dependency on any registered child. */
uint64_t supervisor_sweep_heartbeat(void);

/* CLOCK_MONOTONIC microseconds at the start of the most recent sweep_once()
 * call. A caller computes "how long has the counter been frozen" as
 * `platform_time_monotonic_us() - supervisor_sweep_last_us()` without
 * needing to have polled supervisor_sweep_heartbeat() itself every tick. */
int64_t supervisor_sweep_last_us(void);

/* Remove a contract from the registry. Idempotent. */
void supervisor_unregister(supervisor_child_id id);

/* Mark a child finished without compacting the registry. This preserves cached
 * child ids held by sibling workers while disabling all liveness gates for the
 * completed child. */
void supervisor_child_complete(supervisor_child_id id);

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

/* ── Restart policy (Erlang/OTP) ───────────────────────────────────── */

/* Opt a child into auto-restart. `policy` selects TEMPORARY (default; no
 * restart) / TRANSIENT / PERMANENT. `intensity_max` (N) and `period_secs` (M)
 * form the storm cap: at most N restarts within M seconds, else the supervisor
 * gives up and names a sticky SUPERVISOR_STALL_REPEATED_RESTART. The caller
 * must also set `c->on_respawn` (typically before register). O(1) atomics. */
void supervisor_set_restart_policy(supervisor_child_id id,
                                   enum supervisor_restart_policy policy,
                                   int64_t intensity_max, int64_t period_secs);

/* Worker publishes its liveness. ALIVE at loop entry / on (re)spawn; EXITED
 * right before the worker returns. O(1) atomic store. */
void supervisor_worker_alive(supervisor_child_id id);
void supervisor_worker_exited(supervisor_child_id id);

/* ── Process-wide stall observer (diagnostics auto-capture) ────────── */

/* Invoked on the rising edge of EVERY stall fire (any trigger site:
 * child-reported, deadline lapse, frozen progress, restart-storm), in
 * addition to the per-contract on_stall. Runs on the DETECTING thread —
 * for sweep-detected stalls that is the supervisor thread, which drives
 * every child's on_tick, so the observer MUST be cheap and non-blocking
 * (rate-limit, then hand off to a worker; the ops.debug.bundle
 * auto-capture in app/controllers does exactly that). Register once at
 * boot; NULL clears. Release-store / acquire-load publication. */
typedef void (*supervisor_stall_observer_fn)(
    const char *child_name, enum supervisor_stall_reason reason);
void supervisor_set_stall_observer(supervisor_stall_observer_fn fn);

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

/* Lower the supervisor loop wake interval to AT MOST `ms` (monotonic min: a
 * smaller current value is kept). Production use: an accelerated mint/refold
 * fold sets a sub-second stage period_us and calls this so the sweep runs
 * often enough to honor it. A NORMAL boot never calls this, so g_tick_ms stays
 * at its 1000 ms default and every child's tick/stall cadence is unchanged.
 * No-op for ms <= 0. */
void supervisor_request_min_tick_ms(int ms);

/* Test hook: clear the registry and reset internal state. Each test_*
 * function in the fork-based runner is its own process, so per-process
 * isolation is already free, but the sequential runner (test.c) calls
 * this to keep test_supervisor side-effect free. */
#ifdef ZCL_TESTING
void supervisor_reset_for_testing(void);

/* Run exactly one supervisor sweep synchronously on the CALLING thread (no
 * dedicated supervisor thread, no real-time wait). Lets restart-policy tests
 * drive death→respawn→storm deterministically. Because no tick-runner thread
 * is running under this seam, the sweep drains due on_tick callbacks INLINE
 * (the same degraded-but-driven fallback used if the runner ever fails to
 * spawn), so a synchronous caller still observes on_tick + ticks_run.
 * Production never calls this. */
void supervisor_sweep_once_for_testing(void);

/* Tick-runner monitoring seam. The runner thread is monitored INLINE by the
 * sweep (deadline + edge-triggered named blocker), not via g_contracts, so a
 * wedged child tick becomes the "supervisor.tick_runner_wedged" blocker while
 * the sweep keeps heartbeating. These let a test drive that path without a
 * real wedged thread: arm the runner contract with a short deadline, backdate
 * its heartbeat, then run one monitor pass. */
void supervisor_tick_runner_setup_for_testing(int64_t deadline_secs);
void supervisor_tick_runner_backdate_hb_for_testing(int64_t age_us);
void supervisor_tick_runner_monitor_for_testing(void);
#endif

/* ── `zclassic23 dumpstate supervisor` ────────────────────────────── */

/* ── Tick-runner introspection ─────────────────────────────────────────
 * Age (microseconds) since the tick-runner thread last heartbeat, and its
 * stall_fires counter. A frozen age past the runner deadline means one
 * child's on_tick is wedged (named as a blocker); the sweep is unaffected. */
int64_t  supervisor_tick_runner_last_hb_age_us(void);
uint32_t supervisor_tick_runner_stall_fires(void);
bool     supervisor_tick_runner_running(void);

/* ── Introspection (zcl_state subsystem=supervisor) ────────────────── */

struct supervisor_snapshot {
    char     name[SUPERVISOR_NAME_MAX];
    int      parent;
    int64_t  last_tick_age_us;       /* now - last_tick_us */
    int64_t  progress_marker;
    int64_t  period_secs;
    int64_t  period_us;              /* sub-second tick override; 0 ⇒ period_secs */
    int64_t  deadline_secs;
    bool     completed;
    int      stall_reason;           /* enum supervisor_stall_reason */
    uint32_t ticks_run;
    uint32_t stall_fires;
    uint32_t restart_count;
    int      restart_policy;         /* enum supervisor_restart_policy */
    int      worker_state;           /* enum supervisor_worker_state */
    uint32_t restarts_in_window;
};

/* Fill `out` with up to `max` snapshots. Returns the number written. */
int  supervisor_snapshot_all(struct supervisor_snapshot *out, int max);

/* Dump-state-json wired into diagnostics_controller's `g_dumpers[]`.
 * `out` must be JSON-initialized by the caller. `key` is unused (one
 * dump returns all children). */
bool supervisor_dump_state_json(struct json_value *out, const char *key);

#endif /* ZCL_SUPERVISOR_H */
