/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * HTTP RPC request timeout.
 *
 * The RPC server already applies a 5-second `SO_RCVTIMEO` so a client
 * can't sit on the socket reading bytes one-at-a-time.  What it does
 * NOT have is a deadline on the *dispatch* phase: once the request
 * body is parsed, `rpc_table_execute()` runs unbounded.  A slow
 * method, a stuck database, or an unexpectedly large response can
 * pin a worker forever — the worker pool is only 4 threads wide, so
 * four misbehaving calls wedge the entire surface.
 *
 * This module adds a watchdog that tracks every in-flight request
 * with its start-time + method name + client IP.  A background thread
 * wakes every `watchdog_period_ms` and `shutdown(SHUT_RDWR)`s the
 * socket of any request whose elapsed time exceeds `timeout_ms` — the
 * worker's in-flight read/write then returns EPIPE/ECONNRESET and
 * `handle_client()` unwinds cleanly.  One `EV_RPC_TIMEOUT` event is
 * emitted per kill with the method + elapsed + ip in the payload.
 *
 * Config is read from the environment:
 *
 *   ZCL_RPC_TIMEOUT_MS          per-request deadline (default 10000)
 *   ZCL_RPC_TIMEOUT_SWEEP_MS    watchdog poll interval (default 250)
 *
 * Setting `ZCL_RPC_TIMEOUT_MS=0` disables the module entirely — every
 * register() call returns a valid but no-op slot, the watchdog never
 * kills anything, and the stats counters freeze.
 *
 * Thread safety: all public functions are safe to call concurrently.
 * Registration + sweep share a single mutex; the slot table is
 * bounded (RPC_TIMEOUT_MAX_SLOTS) so the scan is O(slots) per sweep
 * which is cheap for 128 entries.
 */

#ifndef ZCL_RPC_TIMEOUT_H
#define ZCL_RPC_TIMEOUT_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RPC_TIMEOUT_MAX_SLOTS   128
#define RPC_TIMEOUT_METHOD_LEN  64

struct rpc_timeout_slot {
    bool     in_use;
    bool     killed;             /* set by sweep when deadline exceeded */
    int      client_fd;
    uint32_t ip_be;              /* network byte order, 0x0100007F for loopback */
    int64_t  start_us;
    char     method[RPC_TIMEOUT_METHOD_LEN];
};

struct rpc_timeout_mgr {
    /* Config */
    int     timeout_ms;          /* 0 = disabled */
    int     watchdog_period_ms;

    /* Slots */
    struct rpc_timeout_slot slots[RPC_TIMEOUT_MAX_SLOTS];

    /* Watchdog thread */
    pthread_t       watchdog_thread;
    pthread_mutex_t lock;
    pthread_cond_t  wakeup;
    bool            watchdog_running;
    bool            watchdog_started;

    /* Stats — read by tests and native RPC diagnostics. */
    uint64_t stat_registered;
    uint64_t stat_completed;
    uint64_t stat_killed;
    uint64_t stat_sweeps;

    bool initialized;
};

/* Lifecycle — init with defaults, then optionally load env overrides.
 * destroy() stops the watchdog if running and tears down the mutex. */
void rpc_timeout_init(struct rpc_timeout_mgr *mgr);
void rpc_timeout_destroy(struct rpc_timeout_mgr *mgr);
void rpc_timeout_load_from_env(struct rpc_timeout_mgr *mgr);

/* Watchdog control — start/stop the background sweeper.  Safe to
 * start+stop multiple times.  When the module is disabled
 * (timeout_ms == 0) start() is a no-op that returns true. */
bool rpc_timeout_start_watchdog(struct rpc_timeout_mgr *mgr);
void rpc_timeout_stop_watchdog(struct rpc_timeout_mgr *mgr);

/* Register an in-flight request.  Returns the slot index (>= 0) on
 * success, or -1 if the table is full.  Callers ignore -1 and fall
 * back to best-effort (the SO_RCVTIMEO still protects against the
 * slowloris case). */
int  rpc_timeout_register(struct rpc_timeout_mgr *mgr,
                           int client_fd, uint32_t ip_be);

/* Update the method name on an existing slot once the JSON-RPC
 * request has been parsed.  Truncates to RPC_TIMEOUT_METHOD_LEN-1. */
void rpc_timeout_set_method(struct rpc_timeout_mgr *mgr,
                             int slot, const char *method);

/* Release a slot.  Increments `stat_completed` unless the watchdog
 * already marked it killed (in which case the kill stat is retained). */
void rpc_timeout_unregister(struct rpc_timeout_mgr *mgr, int slot);

/* Ask whether the watchdog killed this slot — the worker uses this
 * to decide whether to log its read/write failure as "timeout" vs.
 * "client closed". */
bool rpc_timeout_was_killed(struct rpc_timeout_mgr *mgr, int slot);

/* Sweep the slot table synchronously — any slot whose elapsed time
 * exceeds `timeout_ms` is shutdown() and marked killed.  Returns the
 * number of slots killed this sweep.  Tests call this directly with a
 * synthetic `now_us` to avoid depending on wall-clock time. */
int rpc_timeout_sweep(struct rpc_timeout_mgr *mgr, int64_t now_us);

/* Global handle — httpserver.c publishes its struct at start so tests
 * and observability code can reach the live stats without a reach-in. */
void                    rpc_timeout_set_global(struct rpc_timeout_mgr *mgr);
struct rpc_timeout_mgr *rpc_timeout_get_global(void);

/* Lock-consistent counters + config snapshot, for rendering into
 * native RPC diagnostics and Prometheus. */
struct rpc_timeout_snapshot {
    int      timeout_ms;
    int      watchdog_period_ms;
    size_t   active_slots;
    uint64_t registered;
    uint64_t completed;
    uint64_t killed;
    uint64_t sweeps;
};

void rpc_timeout_snapshot_take(struct rpc_timeout_mgr *mgr,
                                struct rpc_timeout_snapshot *out);

/* Reset stats + clear all slots.  Config preserved.  Tests call
 * this between cases. */
void rpc_timeout_reset_state(struct rpc_timeout_mgr *mgr);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_RPC_TIMEOUT_H */
