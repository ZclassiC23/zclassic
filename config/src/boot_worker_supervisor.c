/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Shared background-worker supervisor helpers. The boot worker modules own
 * their pthreads and worker loops; this TU owns the common observe-only stall
 * handler and idempotent domain registration plumbing.
 */

#include "config/boot_background_workers.h"
#include "event/event.h"
#include "util/blocker.h"
#include "util/log_macros.h"
#include "util/supervisor.h"
#include <stdatomic.h>
#include <stdio.h>

/* One shared, observe-only stall handler. The contract stores its own
 * name (set at register), so a single handler serves every boot worker:
 * it logs and emits EV_RECOVERY_ACTION but never blocks or tears down
 * the worker, because the supervisor cannot wedge a thread it does not own.
 */
void worker_on_stall(struct liveness_contract *c)
{
    const char *name = c ? c->name : "unknown";
    const char *reason = c
        ? supervisor_stall_reason_name(
              (enum supervisor_stall_reason)atomic_load(&c->stall_reason))
        : "unknown";
    LOG_WARN("boot", "[supervisor] %s stall reason=%s", name, reason);
    event_emitf(EV_RECOVERY_ACTION, 0,
                "action=worker-stall worker=%s reason=%s", name, reason);
    /* Raise a TRANSIENT typed blocker for the stalled worker. Worker stalls are
     * worker-scoped faults; the chain-tip escalator is only armed by the
     * chain_tip_watchdog for chain-progress stalls. */
    {
        struct blocker_record br;
        char id[BLOCKER_ID_MAX];
        snprintf(id, sizeof(id), "worker.stall.%s", name);
        if (blocker_init(&br, id, "boot.background_workers",
                         BLOCKER_TRANSIENT, reason)) {
            br.escape_deadline_secs = 60;
            (void)blocker_set(&br);
        }
    }
}

/* Register a single worker contract. Idempotent: supervisor_start and
 * supervisor_domains_init are both safe to call from every worker start,
 * and the stored child id guards re-registration. period_secs == 0 means the
 * supervisor never drives the worker; progress_max_quiet_us == 0 means
 * deadline-only (no NO_PROGRESS gate).
 */
void boot_register_worker_supervisor(
    _Atomic supervisor_child_id *slot,
    struct liveness_contract *contract,
    supervisor_domain_t **domain_slot,
    const char *name,
    int64_t deadline_secs,
    int64_t progress_max_quiet_us)
{
    if (atomic_load(slot) != SUPERVISOR_INVALID_ID)
        return;
    if (!supervisor_start())
        return;
    supervisor_domains_init();

    liveness_contract_init(contract, name);
    atomic_store(&contract->period_secs, 0);
    atomic_store(&contract->deadline_secs, deadline_secs);
    atomic_store(&contract->progress_max_quiet_us, progress_max_quiet_us);
    contract->on_stall = worker_on_stall;

    supervisor_child_id id =
        supervisor_register_in_domain(*domain_slot, contract);
    if (id == SUPERVISOR_INVALID_ID) {
        LOG_WARN("boot", "[supervisor] register failed for %s", name);
        return;
    }
    atomic_store(slot, id);
    supervisor_tick(id);
    supervisor_progress(id, 0);
}

void boot_complete_worker_supervisor(_Atomic supervisor_child_id *slot)
{
    if (!slot)
        return;
    supervisor_child_id id = atomic_load(slot);
    if (id == SUPERVISOR_INVALID_ID)
        return;
    supervisor_child_complete(id);
}
