/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tip-stuck watchdog: a single-purpose supervisor child that does ONE
 * thing — watch active_chain_height advance — and escalates when it
 * doesn't. Designed to be independent of lag_breach_severity logic,
 * mirror catchup state machines, projection deferral counters, and
 * source-scoring competitions, all of which have proven able to mask
 * the underlying "tip is not advancing" signal in prior wedges.
 *
 * Contract (Sync-and-Solid invariant):
 *   The node must advance its tip at least once every 300 s when not
 *   genuinely caught up. If it doesn't, escalate within 30 s.
 *
 * Escalation ladder (configurable thresholds; defaults below):
 *   t=300s  no advance → emit a named chain.advance_decision stall event
 *   t=600s  no advance → reserved
 *   t=1200s no advance → request orderly shutdown; systemd Restart=always
 *                        brings us back with fresh state.
 *
 * Restart is bounded, not infinite (P0 resilience fix). A restart is a
 * remedy; if restarting did NOT make the tip advance, restarting again
 * is a thrash, not a fix. The watchdog persists, in progress.kv, the
 * wedge-episode anchor height and how many consecutive no-progress
 * restarts it has already requested in that episode. The counter
 * therefore survives the restart it triggers. Re-wedging within
 * CHAIN_TIP_WD_EPISODE_MARGIN blocks of the anchor is the SAME episode
 * (a creeping wedge that gains 1-2 blocks per restart must not refresh
 * its budget). After the count reaches CHAIN_TIP_WD_MAX_RESTARTS it
 * STOPS requesting shutdown and instead pages a human
 * (EV_OPERATOR_NEEDED + a loud stderr line), leaving the node up
 * (degraded) so an operator/MCP can intervene. A transient hang still
 * recovers: sustained progress (CHAIN_TIP_WD_EPISODE_CLEAR blocks past
 * the anchor) resets the counter, so the next hang gets a fresh budget.
 *
 * Persisted keys (progress.kv, progress_meta table):
 *   "chain_tip_wd.stuck_height"          int64  (8 bytes, native order)
 *   "chain_tip_wd.no_progress_restarts"  int32  (4 bytes, native order)
 *
 * Verify-never-trust: this primitive does NOT lower any validation
 * gate. The most-aggressive action it can take is to ask the OS to
 * restart the process. Every block written after restart still goes
 * through the full crypto pipeline.
 *
 * Reentrancy: the contract is registered once at boot, ticked from
 * the supervisor thread. All cross-thread state is atomic. */

#ifndef SERVICES_TIP_WATCHDOG_H
#define SERVICES_TIP_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

struct main_state;
struct json_value;

/* Register the watchdog as a supervisor child. Idempotent. Must be
 * called after main_state + chain_active are initialized and after
 * `supervisor_start()`. */
void chain_tip_watchdog_register(struct main_state *ms);

/* Optional retuning. All thresholds in seconds. Zero leaves the
 * existing value unchanged. */


/* Hard cap on consecutive no-progress restarts within one wedge episode.
 * After this many restarts fail to advance the tip, the watchdog stops
 * power-cycling and pages a human (EV_OPERATOR_NEEDED). */
#define CHAIN_TIP_WD_MAX_RESTARTS 3

/* Wedge-episode window: a stuck height within MARGIN blocks past the
 * recorded anchor is the same episode (count carries); only CLEAR
 * blocks of progress past the anchor end the episode and restore a
 * fresh restart budget. */
#define CHAIN_TIP_WD_EPISODE_MARGIN  16
#define CHAIN_TIP_WD_EPISODE_CLEAR  100

/* Snapshot for diagnostics. */
struct chain_tip_watchdog_stats {
    bool     registered;
    int64_t  highest_tip;
    int64_t  last_advance_unix;     /* CLOCK_REALTIME seconds */
    int64_t  age_secs;              /* seconds since last advance */
    int      escalation_level;      /* 0=ok, 1=mirror, 2=reserved, 3=restart */
    uint64_t fires_mirror;
    uint64_t fires_reserved;
    uint64_t fires_restart;
    int64_t  threshold_mirror_secs;
    int64_t  threshold_reserved_secs;
    int64_t  threshold_restart_secs;
    /* Bounded-restart state (persisted in progress.kv). */
    int64_t  persisted_stuck_height;     /* height restarts accumulate at; -1 = none */
    int      no_progress_restarts;       /* consecutive restarts at that height */
    int      max_restarts;               /* CHAIN_TIP_WD_MAX_RESTARTS */
    bool     operator_needed;            /* true once the cap is hit (paging) */
    uint64_t fires_operator_needed;      /* times we paged instead of restarting */
};
void chain_tip_watchdog_get_stats(struct chain_tip_watchdog_stats *out);

/* `zcl_state subsystem=chain_tip_watchdog` dumper. `out` is an
 * already-initialized json object; `key` is ignored. */
bool chain_tip_watchdog_dump_state_json(struct json_value *out, const char *key);

#ifdef ZCL_TESTING
/* ── Test seams (compiled only into test_zcl / test_parallel) ──────────
 *
 * These let the unit test drive the bounded-restart logic deterministically
 * without a running supervisor, real chain, or systemd. They model "what a
 * fresh process boot would do": clear in-memory state, then reload the
 * persisted counters from the currently-open progress.kv. */

/* Wipe ALL in-memory module state to defaults (as if the process just
 * started). Does NOT touch progress.kv. Call once per simulated boot. */
void chain_tip_watchdog_test_reset_runtime(void);

/* Reload (stuck_height, no_progress_restarts) from the open progress.kv
 * into in-memory state. Mirrors the boot-time load done by register(). */
void chain_tip_watchdog_test_load_persisted(void);

/* Execute the restart-escalation decision for a tip stuck at height `h`.
 * Returns true if a real shutdown WOULD have been requested (and the
 * persisted counter was incremented); returns false if the cap was hit
 * and the watchdog paged an operator instead. No actual shutdown is
 * performed in test builds. */
bool chain_tip_watchdog_test_escalate_restart(int64_t h);

/* Like the above but drives the DETERMINISTIC-stall cause-probe path: the
 * watchdog pages an operator immediately (no restart, no counter increment)
 * because a restart cannot clear a byte-identical on-disk wedge. Returns
 * false (no shutdown requested). */
bool chain_tip_watchdog_test_escalate_deterministic(int64_t h);

/* Record that the tip advanced to height `h`: resets the no-progress
 * counter (in memory + persisted) if `h` is at least
 * CHAIN_TIP_WD_EPISODE_CLEAR blocks past the episode anchor.
 * Mirrors the advance branch of the supervisor tick. */
void chain_tip_watchdog_test_observe_advance(int64_t h);
#endif

#endif
