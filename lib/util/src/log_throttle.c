/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * log_throttle — implementation. See util/log_throttle.h. */

#include "util/log_throttle.h"

/* Core decision shared by both entry points. `changed` is true on the first
 * key / a key transition (the caller has already swapped the new key in for the
 * key-based path). Cadence matches the three hand-rolled originals exactly:
 *
 *   changed   -> emit; out_reps = the PRIOR key's accumulated count; reset the
 *                counter to 0 for the new key; stamp last_emit = now.
 *   !changed  -> increment the counter; out_reps = the running count (incl.
 *                this tick); emit only if keepalive_secs have elapsed since the
 *                last emit, and stamp last_emit only when emitting. The counter
 *                is NOT reset on a keep-alive emit. */
static bool throttle_decide(struct log_throttle *t, bool changed,
                            int64_t now_unix, int64_t keepalive_secs,
                            uint64_t *out_reps)
{
    if (changed) {
        uint64_t prior = atomic_exchange(&t->reps, 0);
        atomic_store(&t->last_emit_unix, now_unix);
        if (out_reps)
            *out_reps = prior;
        return true;
    }

    uint64_t running = atomic_fetch_add(&t->reps, 1) + 1;
    if (out_reps)
        *out_reps = running;

    bool emit = now_unix - atomic_load(&t->last_emit_unix) >= keepalive_secs;
    if (emit)
        atomic_store(&t->last_emit_unix, now_unix);
    return emit;
}

bool log_throttle_should_emit(struct log_throttle *t, uint64_t key,
                              int64_t now_unix, int64_t keepalive_secs,
                              uint64_t *out_reps)
{
    if (!t) {
        if (out_reps)
            *out_reps = 0;
        return false;
    }
    /* Swap the new key in and detect a transition in one atomic step (the
     * reducer_frontier original's atomic_exchange(&last_pair, ...) != pair). */
    bool changed = atomic_exchange(&t->last_key, key) != key;
    return throttle_decide(t, changed, now_unix, keepalive_secs, out_reps);
}

bool log_throttle_should_emit_changed(struct log_throttle *t, bool changed,
                                      int64_t now_unix, int64_t keepalive_secs,
                                      uint64_t *out_reps)
{
    if (!t) {
        if (out_reps)
            *out_reps = 0;
        return false;
    }
    return throttle_decide(t, changed, now_unix, keepalive_secs, out_reps);
}

void log_throttle_reset(struct log_throttle *t)
{
    if (!t)
        return;
    atomic_store(&t->last_key, LOG_THROTTLE_KEY_NONE);
    atomic_store(&t->reps, 0);
    atomic_store(&t->last_emit_unix, 0);
}

uint64_t log_throttle_reps(const struct log_throttle *t)
{
    if (!t)
        return 0;
    return atomic_load(&t->reps);
}
