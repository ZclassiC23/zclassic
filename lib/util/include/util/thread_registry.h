/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Central registry for spawned threads, plus a single-source-of-
 * truth shutdown flag. Before this module landed, the node spawned ~50
 * threads from 40-plus call sites and each subsystem rolled its own
 * stop flag: `g_shutdown_requested` (signal handler), `svc->stop_requested`
 * (bg_validation), `cancel_requested` (sync jobs), etc. SIGTERM
 * propagation to every long-running loop depended on each subsystem's
 * shutdown hook being called in the correct order, so a hang anywhere
 * in the orderly-shutdown sequence left other threads spinning and
 * the systemd SIGTERM → SIGKILL grace period (5 min default) had to
 * expire.
 *
 * This header exposes a minimal API:
 *
 *   thread_registry_spawn(name, fn, arg, out_tid)
 *     Wraps pthread_create + records the tid with a human-readable
 *     name. pthread_setname_np is set when available so `top -H` and
 *     `gdb info threads` identify the thread by purpose rather than
 *     "zclassic23". Pass NULL for `out_tid` for long-running daemons
 *     that exit via thread_registry_shutdown_requested polling alone;
 *     pass a non-NULL pthread_t* for bounded-lifetime services that
 *     already have their own stop() routine and need to pthread_join
 *     the spawned thread directly.
 *
 *   thread_registry_shutdown_requested()
 *     True once shutdown has been signaled. Every long-running loop
 *     should poll this (alongside any local stop flag) — it is the
 *     single source of truth.
 *
 *   thread_registry_request_shutdown()
 *     Idempotent setter. The main signal handler sets it; programmatic
 *     shutdown paths call it too.
 *
 *   thread_registry_join_all(timeout_sec)
 *     Walks the registry, pthread_timedjoin_np's each entry with
 *     `timeout_sec` seconds, and returns the count that failed to
 *     exit in time. Diagnostic output names any stragglers so the
 *     operator can see which subsystem is hanging shutdown.
 *
 * This module does NOT own pthread_t lifetime — callers that need to
 * pthread_join from their subsystem code still can; the registry's
 * join_all is for shutdown's final sweep. Unregister on normal exit
 * via thread_registry_unregister_self().
 */

#ifndef ZCL_THREAD_REGISTRY_H
#define ZCL_THREAD_REGISTRY_H

#include <pthread.h>
#include <stdbool.h>

/* Max concurrent registered threads. Sized generously above the ~50
 * currently spawned so we have headroom for swarm/parallel-sync
 * workers. */
#define ZCL_THREAD_REGISTRY_CAP 256

/* Spawn a thread via pthread_create and record it in the registry.
 * `name` is copied; pass NULL for "unnamed". When `out_tid` is
 * non-NULL, writes the spawned thread's pthread_t into *out_tid so the
 * caller can pthread_join it from its own subsystem stop() path. The
 * registry's trampoline still self-unregisters on normal exit, so
 * join_all's sweep will skip an already-exited entry — doing both
 * subsystem-local pthread_join AND relying on join_all is safe in that
 * order.
 *
 * Pass a non-NULL `out_tid` for bounded-lifetime services that already
 * have their own stop() routine; pass NULL for long-running daemons
 * that exit via thread_registry_shutdown_requested polling alone.
 *
 * Returns 0 on success, pthread errno on pthread_create failure, -1 on
 * registry-full. */
int thread_registry_spawn(const char *name,
                          void *(*entry)(void *), void *arg,
                          pthread_t *out_tid);

/* True once thread_registry_request_shutdown has been called. Safe
 * to call from any thread. */
bool thread_registry_shutdown_requested(void);

/* Idempotent. Must be callable from a signal handler (atomic-only,
 * no heap allocation, no lock acquisition). */
void thread_registry_request_shutdown(void);

/* Remove the calling thread's registry entry. Called at the top of a
 * normal thread exit so the shutdown sweep doesn't try to join a
 * thread that has already returned. */
void thread_registry_unregister_self(void);

/* pthread_timedjoin_np each registered thread with `timeout_sec`.
 * Returns the number that failed to join in time (0 on clean
 * shutdown). Prints the name of every straggler. */
int thread_registry_join_all(int timeout_sec);

/* Current count of active entries. Exposed for the stress test. */
int thread_registry_live_count(void);

/* Reset all registry state. For test harness use only — production
 * code should never call this. */
void thread_registry_reset_for_test(void);

#endif /* ZCL_THREAD_REGISTRY_H */
