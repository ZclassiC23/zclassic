/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * log_throttle — the shared "de-storm" idiom for a WARN that would otherwise
 * repeat the SAME line every reducer pass (millions of times in minutes) while
 * a held frontier / pending-tear / suppressed-gate condition persists.
 *
 * Three reducer sites grew the same throttle by hand (tip_finalize_stage's
 * cursor-gap + precondition WARNs, reducer_frontier's coin-tear WARN, and
 * reducer_frontier_reconcile_light's gate-suppress WARN). This collapses that
 * shape into ONE primitive. The primitive decides WHEN to emit; the caller
 * still formats WHAT (it owns its message text and its suppressed-count
 * placeholder).
 *
 * Cadence (byte-identical to the three originals):
 *   - First time a key is seen, OR the key CHANGES: emit, and report the PRIOR
 *     key's suppressed-repeat count (reset to 0 for the new key). last_emit is
 *     stamped to `now`.
 *   - Same key as last emit-check: increment the repeat counter, then emit only
 *     if at least `keepalive_secs` have elapsed since the last emit (a periodic
 *     keep-alive carrying the running suppressed count). On the keep-alive emit
 *     the counter is NOT reset — it keeps accumulating across keep-alives.
 *
 * `out_reps` therefore carries:
 *   - on a key change: the PRIOR key's accumulated suppressed count, and
 *   - on a same-key keep-alive: the running count including this tick.
 * which is exactly what each original LOG_WARN already printed.
 *
 * Concurrency: all state is in atomics, so should_emit() is torn-read-safe
 * without a lock. Callers that already serialize their throttle update under a
 * mutex (tip_finalize's g_block_reason_mu) may keep doing so; the atomics make
 * a concurrent lock-free dump read safe regardless. Sites that
 * run on a single serial thread (the reducer step path, the condition-engine
 * tick) pay only relaxed atomic ops.
 */
#ifndef ZCL_UTIL_LOG_THROTTLE_H
#define ZCL_UTIL_LOG_THROTTLE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

/* Reserved key meaning "no key seen yet". A real key must never equal this;
 * the first should_emit() with any key is treated as a change (first emit). */
#define LOG_THROTTLE_KEY_NONE ((uint64_t)UINT64_MAX)

struct log_throttle {
    _Atomic uint64_t last_key;     /* LOG_THROTTLE_KEY_NONE until first seen */
    _Atomic uint64_t reps;         /* suppressed repeats for the current key  */
    _Atomic int64_t  last_emit_unix;
};

/* Static initializer for a fresh throttle (no key seen, zero reps). */
#define LOG_THROTTLE_INIT \
    { .last_key = LOG_THROTTLE_KEY_NONE, .reps = 0, .last_emit_unix = 0 }

/* Key-based entry point. Returns true when the caller should emit its WARN now.
 *
 *   t             — throttle state (atomics).
 *   key           — packed identity of the condition (e.g. a height pair). The
 *                   primitive emits on first key / key change / keepalive.
 *   now_unix      — current wall-clock unix seconds (caller-supplied so tests
 *                   and callers control the clock).
 *   keepalive_secs— minimum seconds between same-key keep-alive emits.
 *   out_reps      — (optional) receives the suppressed-repeat count to print:
 *                   the prior key's count on a change, or the running count on
 *                   a keep-alive. May be NULL.
 */
bool log_throttle_should_emit(struct log_throttle *t, uint64_t key,
                              int64_t now_unix, int64_t keepalive_secs,
                              uint64_t *out_reps);

/* Boolean-change entry point for callers whose "key" is not a single integer
 * (e.g. a (height, reason-string) tuple): the caller computes `changed` itself
 * and the primitive applies the identical first/change/keepalive cadence. This
 * is exactly the old tipfin_warn_throttled(changed, ...) shape. */
bool log_throttle_should_emit_changed(struct log_throttle *t, bool changed,
                                      int64_t now_unix, int64_t keepalive_secs,
                                      uint64_t *out_reps);

/* Reset to the fresh (no-key, zero-reps) state. Used by callers whose
 * condition can END (e.g. the gate-suppress site re-arms when evidence
 * clears) so the next activation emits immediately with reps=0. */
void log_throttle_reset(struct log_throttle *t);

/* Lock-free read of the current suppressed-repeat counter, for diagnostics
 * (native dumpstate) that surface the running count without driving an emit. */
uint64_t log_throttle_reps(const struct log_throttle *t);

#endif /* ZCL_UTIL_LOG_THROTTLE_H */
