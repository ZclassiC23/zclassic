/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Supervisor contract for the zclassicd mirror observer.
 */

#include "supervisors/legacy_mirror_supervisor.h"

#include "services/legacy_mirror_sync_service.h"
#include "supervisors/domains.h"
#include "event/event.h"
#include "util/log_macros.h"
#include "util/supervisor.h"

#include <stdatomic.h>

static _Atomic supervisor_child_id g_legacy_mirror_supervisor_id =
    SUPERVISOR_INVALID_ID;
static struct liveness_contract g_legacy_mirror_contract;

static int64_t legacy_mirror_progress_marker(
    const struct legacy_mirror_sync_stats *s)
{
    if (!s)
        return -1;
    if (s->last_attempt > 0)
        return s->last_attempt;
    return s->catchups_total;
}

static void legacy_mirror_on_stall(struct liveness_contract *c)
{
    struct legacy_mirror_sync_stats s;
    legacy_mirror_sync_stats_snapshot(&s);

    const char *reason = c
        ? supervisor_stall_reason_name(
              (enum supervisor_stall_reason)atomic_load(&c->stall_reason))
        : "unknown";
    LOG_WARN("legacy_mirror",
             "[legacy_mirror] supervisor stall reason=%s legacy=%d local=%d rpc_errors=%lld",
             reason, s.legacy_height, s.local_height,
             (long long)s.rpc_errors);
    event_emitf(EV_CHAIN_ADVANCE_DECISION, 0,
                "source=legacy_mirror decision=dependency_stall "
                "reason=%s legacy_height=%d local_height=%d rpc_errors=%lld",
                reason, s.legacy_height, s.local_height,
                (long long)s.rpc_errors);
}

static void legacy_mirror_on_tick(struct liveness_contract *c)
{
    (void)c;
    supervisor_child_id id =
        atomic_load(&g_legacy_mirror_supervisor_id);
    if (id == SUPERVISOR_INVALID_ID)
        return;

    supervisor_tick(id);

    struct legacy_mirror_sync_stats before;
    legacy_mirror_sync_stats_snapshot(&before);
    bool ok = legacy_mirror_sync_request_catchup("supervisor_tick");

    struct legacy_mirror_sync_stats after;
    legacy_mirror_sync_stats_snapshot(&after);
    supervisor_progress(id, legacy_mirror_progress_marker(&after));
    if (!ok || after.rpc_errors > before.rpc_errors)
        supervisor_report_stall(id, SUPERVISOR_STALL_CHILD_REPORTED);
}

bool legacy_mirror_supervisor_start(int cadence_secs)
{
    if (cadence_secs <= 0)
        cadence_secs = 3;
    if (!supervisor_start())
        LOG_FAIL("legacy_mirror", "legacy_mirror_supervisor_start failed");

    supervisor_child_id id =
        atomic_load(&g_legacy_mirror_supervisor_id);
    if (id != SUPERVISOR_INVALID_ID) {
        struct legacy_mirror_sync_stats s;
        legacy_mirror_sync_stats_snapshot(&s);
        supervisor_set_period(id, cadence_secs);
        supervisor_progress(id, legacy_mirror_progress_marker(&s));
        supervisor_tick(id);
        return true;
    }

    liveness_contract_init(&g_legacy_mirror_contract, "chain.legacy_mirror");
    atomic_store(&g_legacy_mirror_contract.period_secs, cadence_secs);
    atomic_store(&g_legacy_mirror_contract.deadline_secs, 0);
    atomic_store(&g_legacy_mirror_contract.progress_max_quiet_us, 0);
    g_legacy_mirror_contract.on_tick = legacy_mirror_on_tick;
    g_legacy_mirror_contract.on_stall = legacy_mirror_on_stall;

    supervisor_domains_init();
    id = supervisor_register_in_domain(g_chain_sup, &g_legacy_mirror_contract);
    if (id == SUPERVISOR_INVALID_ID)
        LOG_FAIL("legacy_mirror", "legacy_mirror_supervisor_register failed");

    atomic_store(&g_legacy_mirror_supervisor_id, id);
    {
        struct legacy_mirror_sync_stats s;
        legacy_mirror_sync_stats_snapshot(&s);
        supervisor_progress(id, legacy_mirror_progress_marker(&s));
    }
    supervisor_tick(id);
    return true;
}

void legacy_mirror_supervisor_stop(void)
{
    supervisor_child_id id =
        atomic_load(&g_legacy_mirror_supervisor_id);
    if (id == SUPERVISOR_INVALID_ID)
        return;
    supervisor_set_period(id, 0);
#ifdef ZCL_TESTING
    id = atomic_exchange(&g_legacy_mirror_supervisor_id,
                         SUPERVISOR_INVALID_ID);
    if (id != SUPERVISOR_INVALID_ID)
        supervisor_unregister(id);
#endif
}

bool legacy_mirror_supervisor_running(void)
{
    supervisor_child_id id =
        atomic_load(&g_legacy_mirror_supervisor_id);
    return id != SUPERVISOR_INVALID_ID &&
           atomic_load(&g_legacy_mirror_contract.period_secs) > 0;
}
