/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "framework/condition.h"
#include "util/log_macros.h"

#include "services/legacy_mirror_sync_service.h"

#include <stdatomic.h>
#include <stdio.h>

static _Atomic int g_stuck_height_at_detect;
static _Atomic int g_lag_at_detect;
static _Atomic int64_t g_stalls_at_detect;

#ifdef ZCL_TESTING
static _Atomic int g_test_remedy_calls;
#endif

static bool detect_legacy_mirror_stuck(void)
{
    struct legacy_mirror_sync_stats s;
    legacy_mirror_sync_stats_snapshot(&s);
    if (!s.enabled || !s.running || !s.reachable || s.in_flight ||
        s.stuck_height <= 0)
        return false;

    atomic_store(&g_stuck_height_at_detect, s.stuck_height);
    atomic_store(&g_lag_at_detect, s.lag);
    atomic_store(&g_stalls_at_detect, s.stalls_total);
    return true;
}

static enum condition_remedy_result remedy_legacy_mirror_stuck(void)
{
    struct legacy_mirror_sync_stats s;
    legacy_mirror_sync_stats_snapshot(&s);
#ifdef ZCL_TESTING
    atomic_fetch_add(&g_test_remedy_calls, 1);
#endif
    LOG_WARN("condition", "[condition:legacy_mirror_stuck] stuck_height=%d lag=%d " "stalls=%lld reason=%s action=catchup", atomic_load(&g_stuck_height_at_detect), atomic_load(&g_lag_at_detect), (long long)atomic_load(&g_stalls_at_detect), s.stuck_reason[0] ? s.stuck_reason : "(none)");
    struct zcl_result r = legacy_mirror_sync_request_catchup_result(
        "condition:legacy_mirror_stuck");
    return r.ok ? COND_REMEDY_OK : COND_REMEDY_FAILED;
}

static bool witness_legacy_mirror_stuck(int64_t target_at_detect)
{
    (void)target_at_detect;
    struct legacy_mirror_sync_stats s;
    legacy_mirror_sync_stats_snapshot(&s);
    int stuck_height = atomic_load(&g_stuck_height_at_detect);
    return !s.enabled || s.stuck_height <= 0 ||
           (stuck_height > 0 && s.local_height > stuck_height);
}

static struct condition c_legacy_mirror_stuck = {
    .name = "legacy_mirror_stuck",
    .severity = COND_WARN,
    .poll_secs = 10,
    .backoff_secs = 60,
    .max_attempts = 3,
    .detect = detect_legacy_mirror_stuck,
    .remedy = remedy_legacy_mirror_stuck,
    .witness = witness_legacy_mirror_stuck,
    .witness_window_secs = 60,
};

void register_legacy_mirror_stuck(void)
{
    (void)condition_register(&c_legacy_mirror_stuck);
}

#ifdef ZCL_TESTING
void legacy_mirror_stuck_test_reset(void)
{
    atomic_store(&g_stuck_height_at_detect, 0);
    atomic_store(&g_lag_at_detect, 0);
    atomic_store(&g_stalls_at_detect, 0);
    atomic_store(&g_test_remedy_calls, 0);
    condition_reset_state(&c_legacy_mirror_stuck);
}

int legacy_mirror_stuck_test_remedy_calls(void)
{
    return atomic_load(&g_test_remedy_calls);
}
#endif
