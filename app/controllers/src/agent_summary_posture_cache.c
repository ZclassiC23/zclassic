/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Bounded/cached front for agent_security_posture_collect(), used
 * exclusively by rpc_agent_summary() (event_agent_summary.c) -- split out
 * to its own file so that one keeps growing its own field list without
 * pushing the other file over its line-count ceiling (see
 * tools/scripts/check_file_size_ceiling.sh).
 *
 * agent_security_posture_collect()'s bootstrap step
 * (chain_evidence_controller_snapshot, see chain_evidence_snapshot.c) issues
 * on the order of a dozen synchronous node_db reads per call -- fine for the
 * health heartbeat's own periodic tick, but rpc_agent_summary ("agent", and
 * its "status"/"summary"/"operatorsummary" aliases) is on the per-request
 * path event_agent_summary.c's header promises to keep cheap. Repeated
 * calls -- an operator polling `zclassic23 status`, or a monitoring script
 * -- would otherwise redo the full read set every time, and each read is
 * subject to the shared node.db connection's multi-second busy_timeout, so
 * a request landing while that connection is merely contended (not flagged
 * as the rarer "long op") had no bound at all. Read-through with a short
 * wall-clock TTL bounds that: most calls in a burst are answered from
 * memory, and the very first call after a quiet stretch pays for one real
 * collection, not every collection.
 *
 * Disabled under ZCL_TESTING: several tests flip
 * agent_security_posture_test_override_review_required() and immediately
 * re-invoke this RPC expecting the override to apply to that very call; a
 * cache would serve the pre-override snapshot and break that contract.
 * Release binaries never define ZCL_TESTING.
 *
 * The cache-miss fallthrough below still calls
 * agent_security_posture_collect(out, NULL) directly, but that call is no
 * longer able to block this TTL's whole duration: it now tries the shared
 * node.db connection's own mutex non-blockingly before reading (see
 * posture_ndb_try_lock() in agent_security_posture.c) and, on contention,
 * returns the last-known-good (or labeled "posture_unavailable_busy")
 * snapshot immediately instead of queuing behind a writer's SQLITE_BUSY
 * retry. This TTL cache remains valuable for coalescing a burst of calls
 * into one real collection; it is no longer the only thing standing between
 * a contended connection and a stalled front door. */

#include "event_agent_summary_internal.h"

#include "controllers/agent_security_posture.h"
#include "platform/time_compat.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define AGENT_SUMMARY_POSTURE_CACHE_TTL_MS 1500

#ifndef ZCL_TESTING
static pthread_mutex_t g_agent_posture_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static struct agent_security_posture g_agent_posture_cache;
static int64_t g_agent_posture_cache_captured_ms;
static bool g_agent_posture_cache_valid;
#endif

static bool agent_summary_posture_cache_load(struct agent_security_posture *out)
{
#ifdef ZCL_TESTING
    (void)out;
    return false;
#else
    bool ok;
    pthread_mutex_lock(&g_agent_posture_cache_lock);
    ok = g_agent_posture_cache_valid;
    if (ok) {
        int64_t now = platform_time_monotonic_ms();
        int64_t age = now > g_agent_posture_cache_captured_ms
            ? now - g_agent_posture_cache_captured_ms : 0;
        ok = age < AGENT_SUMMARY_POSTURE_CACHE_TTL_MS;
        if (ok) {
            *out = g_agent_posture_cache;
            out->served_from_cache = true;
            out->cache_age_ms = age;
        }
    }
    pthread_mutex_unlock(&g_agent_posture_cache_lock);
    return ok;
#endif
}

static void agent_summary_posture_cache_store(const struct agent_security_posture *p)
{
#ifndef ZCL_TESTING
    pthread_mutex_lock(&g_agent_posture_cache_lock);
    g_agent_posture_cache = *p;
    g_agent_posture_cache_captured_ms = platform_time_monotonic_ms();
    g_agent_posture_cache_valid = true;
    pthread_mutex_unlock(&g_agent_posture_cache_lock);
#else
    (void)p;
#endif
}

/* A cache hit still carries served_from_cache=true and an honest
 * cache_age_ms, same as agent_security_posture_collect()'s own db-busy
 * snapshot path, so the emitted zcl.security_posture.v1 body never claims a
 * live read it did not do. */
void agent_summary_posture_collect_bounded(struct agent_security_posture *out)
{
    if (agent_summary_posture_cache_load(out))
        return;
    agent_security_posture_collect(out, NULL);
    agent_summary_posture_cache_store(out);
}
