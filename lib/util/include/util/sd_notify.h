/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Minimal sd_notify implementation. Talks to systemd's notification
 * socket directly (NOTIFY_SOCKET env var) — no libsystemd dependency.
 *
 * Why direct, not via libsystemd: zclassic23 ships its own statically
 * linked binary with zero non-libc runtime deps. The systemd notify
 * protocol is a stable, documented AF_UNIX datagram protocol; the
 * five lines of code below are easier to audit than dlopen'ing a
 * shared library at runtime.
 *
 * When invoked outside a systemd unit (no NOTIFY_SOCKET in env), all
 * functions are no-ops and return false.
 *
 * Lifecycle:
 *   sd_notify_init()          — once at boot, after env is loaded
 *   sd_notify_ready()         — once when the node is fully initialized
 *   sd_notify_watchdog_*()    — periodic heartbeat, gated on health
 *   sd_notify_status(msg)     — free-form status visible in systemctl
 *   sd_notify_stopping()      — once at shutdown
 *
 * WatchdogSec interaction: when the unit file sets WatchdogSec=N,
 * systemd exports WATCHDOG_USEC. sd_notify_watchdog_usec() returns
 * that value (in microseconds) so the heartbeat thread can pick a
 * cadence (typically WATCHDOG_USEC/2). Calling sd_notify_watchdog_ping()
 * sends "WATCHDOG=1" so systemd's timer resets. Stop pinging when the
 * node is unhealthy and systemd will Restart=always the unit after the
 * configured WatchdogSec interval.
 */
#ifndef ZCL_UTIL_SD_NOTIFY_H
#define ZCL_UTIL_SD_NOTIFY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read NOTIFY_SOCKET + WATCHDOG_USEC from env. Returns true when the
 * process is under systemd notify supervision. Cheap and idempotent.
 * Safe to call without ever calling the ping functions. */
bool sd_notify_init(void);

/* True iff sd_notify_init() found a NOTIFY_SOCKET. */
bool sd_notify_is_active(void);

/* Watchdog interval, in microseconds, taken from WATCHDOG_USEC. Zero
 * when not configured (WatchdogSec= not set on the unit). Useful for
 * picking the heartbeat cadence — half the configured interval is the
 * canonical safe choice. */
uint64_t sd_notify_watchdog_usec(void);

/* Send "READY=1" once initialization is complete. Type=notify units
 * stay in "activating" until this fires. */
bool sd_notify_ready(void);

/* Send "WATCHDOG=1". Heartbeat the systemd watchdog timer. Call from
 * the heartbeat thread when the node is healthy. Suppressed (returns
 * false, sends nothing) when a registered health-check callback (see
 * sd_notify_set_health_check) reports unhealthy. */
bool sd_notify_watchdog_ping(void);

/* Optional root-health gate, checked by sd_notify_watchdog_ping() before
 * every send. This is a defense-in-depth backstop independent of
 * whatever gating a caller already does at its own call site: a wedged
 * root supervisor (util/supervisor.h) must stop feeding the watchdog so
 * systemd's WatchdogSec timer restarts the process, and that guarantee
 * belongs at the primitive that actually sends the datagram, not only at
 * one call site that could someday be bypassed or duplicated.
 *
 * `fn` is called with no arguments immediately before every WATCHDOG=1
 * send, on the pinging thread. A NULL fn (the default) disables the gate
 * (always allow, matching pre-existing behavior). Pass NULL to clear.
 * Production wiring: config/src/boot_sd_watchdog.c registers its
 * supervisor-sweep-freshness check here. */
typedef bool (*sd_notify_health_check_fn)(void);
void sd_notify_set_health_check(sd_notify_health_check_fn fn);

/* Send free-form "STATUS=...". Visible in `systemctl status`. */
bool sd_notify_status(const char *msg);

/* Send "STOPPING=1" + "STATUS=..." once when shutdown begins. */
bool sd_notify_stopping(const char *reason);

#ifdef ZCL_TESTING
/* Test hook: reset all internal state (active flag, socket path,
 * WATCHDOG_USEC cache, health-check callback) as if the process had
 * never called sd_notify_init(). Production never calls this — the
 * module is designed to latch in its NOTIFY_SOCKET once per process
 * (matching real systemd semantics: the env var never changes mid-run).
 * Tests that exercise more than one NOTIFY_SOCKET scenario in a single
 * process need this between scenarios. */
void sd_notify_reset_for_testing(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ZCL_UTIL_SD_NOTIFY_H */
