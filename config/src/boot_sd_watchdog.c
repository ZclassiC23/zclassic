/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_sd_watchdog.c — systemd watchdog heartbeat service.
 *
 * Part of the boot composition root (extracted from boot_services.c). This
 * unit owns the systemd WATCHDOG=1 heartbeat: it pings the systemd notify
 * socket every WATCHDOG_USEC/2 microseconds when node health is OK (or a
 * long-running synchronous worker has bumped boot_progress recently). When
 * health degrades the heartbeat stops and systemd's WatchdogSec timer trips,
 * restarting the unit. No-op when NOTIFY_SOCKET is absent (e.g. CLI use).
 *
 * Pillar 7 — "supervise the supervisor": the ping is gated on the root
 * supervisor's sweep heartbeat (util/supervisor.h) being fresh, TWICE:
 * once here (the explicit `supervisor_alive` check below, which also
 * drives the STATUS= label) and once more inside sd_notify_watchdog_ping()
 * itself via sd_notify_set_health_check() (registered in
 * boot_sd_watchdog_start below) — a defense-in-depth backstop so the
 * guarantee holds even for a future caller of sd_notify_watchdog_ping()
 * that forgets to check supervisor health first. The node-health snapshot
 * below is collected independently of the supervisor tree, so a
 * wedged/dead zcl_supervisor thread would otherwise leave every
 * supervisor-driven stage frozen while this tick kept pinging happily
 * (health looking fine from a stale-but-not-yet-detected angle) — this is
 * the PREFERRED escalation path from the design: a frozen sweep stops the
 * ping, systemd's own WatchdogSec timer then kills + restarts the unit. The
 * independent off-systemd fallback (no ping to stop) is
 * lib/util/src/supervisor_backstop.c.
 *
 * Owns: the file-statics g_sd_watchdog_id / g_sd_watchdog_ctx tracking the
 * registered health-ring periodic (the COLLECT half), plus g_pet_tid and
 * friends for the dedicated PET thread. The pet thread is spawned here
 * because it must outlive health-ring contention: a supervised child riding
 * the ring could be starved by the very collect blocking it exists to
 * survive. boot_sd_watchdog_tick stays private here. */

#include "platform/time_compat.h"
#include "config/boot_internal.h"
#include "services/node_health_service.h"
#include "services/binary_ab_fallback.h"
#include "health/heartbeat.h"
#include "util/log_macros.h"
#include "util/sd_notify.h"
#include "util/boot_progress.h"
#include "util/supervisor.h"
#include "util/supervisor_backstop.h"
#include "util/thread_registry.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

/* ── systemd watchdog heartbeat ─────────────────────────────────
 * Two halves, deliberately on different threads:
 *
 *   1. COLLECT tick (lib/health periodic ring): runs node_health_collect,
 *      which publishes the verdict (node_health_last_verdict), and emits
 *      the STATUS= line. This half MAY block — a collect can wait minutes
 *      on reducer-held locks during bulk ingest — and when it does, only
 *      the verdict/status go stale.
 *
 *   2. PET thread (dedicated, this file): pings WATCHDOG=1 every
 *      WATCHDOG_USEC/4 from CHEAP ATOMICS ONLY (verdict publication,
 *      boot_progress, supervisor sweep heartbeat). It never runs a collect
 *      and never takes a node lock, so ring contention cannot starve the
 *      heartbeat. Before this split the ping rode the same ring as the
 *      collect: a >WatchdogSec collect block stopped the ping on a fully
 *      healthy, progressing node. The loop had a second half: the verdict
 *      also sits at healthy=false for an intentional posture a restart
 *      cannot fix — the sync FSM refuses SYNC_AT_TIP while the
 *      snapshot-seeded datadir's body history is unproven (38fe0885b) —
 *      leaving the pet entirely dependent on boot_progress, which goes
 *      stale minutes after each boot. Together those were the 2026-08-02
 *      kill loop: seven SIGABRTs in 3 h on the canonical node while it
 *      held tip and backfilled bodies. The body-gap posture flag in the
 *      published verdict is the precise, bounded forgiveness for the
 *      second half (see node_health_service.c).
 *
 * When health genuinely degrades, the published verdict flips unhealthy
 * and the ping stops; when the verdict pipeline itself dies (no collect
 * completes for a whole WatchdogSec window) the ping also stops — a wedged
 * health pipeline is a named stall, not a pet-forever blind spot. When the
 * supervisor sweep freezes, Pillar 7 stops the ping. In every stopped-ping
 * case systemd's WatchdogSec timer kills + restarts the unit, as designed.
 *
 * No-op when NOTIFY_SOCKET is absent (e.g. CLI invocation). */
static health_subsystem_id g_sd_watchdog_id = HEALTH_INVALID_ID;
static struct boot_svc_ctx *g_sd_watchdog_ctx;

/* Pet-thread state. */
static _Atomic bool g_pet_stop       = false;
static _Atomic bool g_pet_handle_set = false;
static pthread_t    g_pet_tid;

/* Pillar 7: true unless the root supervisor's sweep heartbeat
 * (util/supervisor.h) has gone stale. A heartbeat of 0 means the
 * supervisor hasn't completed its first sweep yet this boot (normal
 * during very early startup, before app_init_services starts it) —
 * that is NOT a wedge, so it does not block the ping. Uses the same
 * freeze threshold as the off-systemd fallback watcher
 * (lib/util/src/supervisor_backstop.c) so the two escalation paths
 * agree on what "frozen" means. */
static bool boot_sd_watchdog_supervisor_alive(void)
{
    uint64_t hb = supervisor_sweep_heartbeat();
    if (hb == 0)
        return true;
    int64_t age_us = platform_time_monotonic_us() - supervisor_sweep_last_us();
    return age_us < SUPERVISOR_BACKSTOP_DEFAULT_FREEZE_US;
}

/* boot_progress freshness, shared by the collect tick (STATUS label) and
 * the pet thread. Snapshot import bulk INSERT, block-by-block catchup, and
 * UTXO replay all take longer than WatchdogSec and would otherwise be
 * killed mid-write, so a recently-bumped boot_progress counts as alive.
 * Freshness window mirrors WATCHDOG_USEC. */
static bool boot_sd_watchdog_recent_progress(void)
{
    int64_t last_us = boot_progress_last_us();
    if (last_us <= 0)
        return false;
    uint64_t wd_us = sd_notify_watchdog_usec();
    int64_t window_us = wd_us > 0 ? (int64_t)wd_us
                                  : (int64_t)(120 * 1000000LL);
    struct timespec now_ts;
    platform_time_monotonic_timespec(&now_ts);
    int64_t now_us = (int64_t)now_ts.tv_sec * 1000000
                   + (int64_t)now_ts.tv_nsec / 1000;
    return now_us - last_us < window_us;
}

/* Pure pet decision — ONE code path for the pet thread and the ZCL_TESTING
 * seam (mirrors supervisor_backstop's backstop_decide factoring):
 *   - a frozen root supervisor sweep always stops the ping (Pillar 7);
 *   - otherwise ping when the last collected verdict is healthy AND fresh
 *     (younger than verdict_bound_us = one WatchdogSec window), OR when the
 *     verdict is the strict body-gap posture (unhealthy SOLELY because the
 *     body-history archive is unproven — see node_health_service.c), OR
 *     when no verdict exists yet but startup grace remains (the ring's
 *     first collect lands one period after registration), OR when a
 *     long-running synchronous worker bumped boot_progress recently. */
static bool boot_sd_watchdog_pet_decide(bool supervisor_alive,
                                        bool have_verdict,
                                        bool verdict_healthy,
                                        bool body_gap_posture,
                                        int64_t verdict_age_us,
                                        bool recent_progress,
                                        int64_t grace_left_us,
                                        int64_t verdict_bound_us)
{
    if (!supervisor_alive)
        return false;
    bool verdict_ok;
    if (have_verdict)
        verdict_ok = (verdict_healthy || body_gap_posture) &&
                     verdict_age_us < verdict_bound_us;
    else
        verdict_ok = grace_left_us > 0;
    return verdict_ok || recent_progress;
}

#ifdef ZCL_TESTING
bool boot_sd_watchdog_test_pet_decide(bool supervisor_alive, bool have_verdict,
                                      bool verdict_healthy,
                                      bool body_gap_posture,
                                      int64_t verdict_age_us,
                                      bool recent_progress,
                                      int64_t grace_left_us,
                                      int64_t verdict_bound_us)
{
    return boot_sd_watchdog_pet_decide(supervisor_alive, have_verdict,
                                       verdict_healthy, body_gap_posture,
                                       verdict_age_us, recent_progress,
                                       grace_left_us, verdict_bound_us);
}
#endif

/* PET half. Sleeps in 1 s slices so boot_sd_watchdog_stop's pthread_join
 * never waits a full period. */
static void *boot_sd_watchdog_pet_main(void *arg)
{
    (void)arg;
    uint64_t wd_us = sd_notify_watchdog_usec();
    int64_t bound_us = wd_us > 0 ? (int64_t)wd_us : 120LL * 1000000;
    int64_t period_us = bound_us / 4;
    if (period_us < 5000000)  period_us = 5000000;  /* never DoS systemd */
    if (period_us > 60000000) period_us = 60000000;
    const int64_t start_us = platform_time_monotonic_us();

    while (!atomic_load(&g_pet_stop) &&
           !thread_registry_shutdown_requested()) {
        int64_t now_us = platform_time_monotonic_us();
        bool healthy = false, posture = false;
        int64_t pub_us = 0;
        bool have = node_health_last_verdict(&healthy, &posture, &pub_us);
        int64_t verdict_age_us = have ? now_us - pub_us : 0;
        if (boot_sd_watchdog_pet_decide(boot_sd_watchdog_supervisor_alive(),
                                        have, healthy, posture,
                                        verdict_age_us,
                                        boot_sd_watchdog_recent_progress(),
                                        bound_us - (now_us - start_us),
                                        bound_us)) {
            sd_notify_watchdog_ping();
        }
        int64_t left_us = period_us;
        while (left_us > 0 && !atomic_load(&g_pet_stop) &&
               !thread_registry_shutdown_requested()) {
            int64_t slice_us = left_us < 1000000 ? left_us : 1000000;
            struct timespec req = {
                .tv_sec  = (time_t)(slice_us / 1000000),
                .tv_nsec = (long)((slice_us % 1000000) * 1000),
            };
            nanosleep(&req, NULL);
            left_us -= slice_us;
        }
    }
    thread_registry_unregister_self();
    return NULL;
}

/* COLLECT half: refresh the published verdict + STATUS= line. Runs on the
 * shared health ring, so it may lag behind reducer lock contention — by
 * design that lag now degrades only freshness, never the pet cadence. */
static void boot_sd_watchdog_tick(void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    if (!sd_notify_is_active() || !svc)
        return;
    struct node_health_snapshot snap = {0};
    node_health_collect(&snap, svc->node_db, svc->state);
    /* Refresh status line — useful for `systemctl status zclassic23`.
     * Include the recent-progress label so operators can see which
     * subsystem is keeping the watchdog alive during bulk ops, and a
     * supervisor=FROZEN marker when Pillar 7's gate is the reason the
     * ping stopped (as opposed to a plain health/progress lapse). */
    bool recent_progress = boot_sd_watchdog_recent_progress();
    bool supervisor_alive = boot_sd_watchdog_supervisor_alive();
    char status[320];
    const char *label = recent_progress ? boot_progress_last_label() : NULL;
    snprintf(status, sizeof(status),
             "h=%d peers=%zu mirror_lag=%lld sev=%s%s%s%s",
             snap.tip_height, snap.peer_count,
             (long long)snap.mirror_lag_blocks,
             snap.mirror_lag_breach_severity[0]
                 ? snap.mirror_lag_breach_severity : "none",
             label ? " busy=" : "",
             label ? label : "",
             supervisor_alive ? "" : " supervisor=FROZEN");
    sd_notify_status(status);
}

/* Start the systemd watchdog heartbeat (runtime service kernel entry). */
bool boot_sd_watchdog_start(void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    if (!svc)
        return false;

    /* NOTE: binary_ab_promote_on_ready_env() deliberately does NOT fire
     * here — sd_watchdog starts 8th of 15 runtime services, so "reached
     * sd_watchdog" is not "booted successfully". The promotion/streak-reset
     * fires at the END of boot_start_runtime_and_catchup()
     * (boot_services.c), after every runtime service, EV_NODE_READY, and
     * the catchup/backfill spin-up. */
    if (!sd_notify_init()) {
        /* Not running under systemd notify supervision (e.g. invoked
         * from a CLI). Silent success — the unit is functionally
         * complete without WatchdogSec. */
        return true;
    }
    g_sd_watchdog_ctx = svc;

    /* Collect/status cadence: half the configured WatchdogSec, clamped to
     * at least 5s. When WATCHDOG_USEC is unset the unit didn't ask for a
     * watchdog — still emit periodic STATUS= lines on a 30s cadence so
     * operators see a live status in `systemctl status`. (The PET cadence
     * is computed independently in boot_sd_watchdog_pet_main.) */
    uint64_t wd_us = sd_notify_watchdog_usec();
    int period_secs;
    if (wd_us > 0) {
        int64_t half = (int64_t)(wd_us / 2 / 1000000);
        if (half < 5) half = 5;
        if (half > 3600) half = 3600;
        period_secs = (int)half;
    } else {
        period_secs = 30;
    }
    g_sd_watchdog_id = health_register_periodic("sd_watchdog", period_secs,
                                                boot_sd_watchdog_tick, svc);
    if (g_sd_watchdog_id == HEALTH_INVALID_ID)
        return false;
    /* Defense-in-depth: sd_notify_watchdog_ping() itself now refuses to
     * send WATCHDOG=1 whenever the root supervisor sweep is stale, even
     * if some future call site forgets the explicit check above. */
    sd_notify_set_health_check(boot_sd_watchdog_supervisor_alive);
    sd_notify_ready();
    sd_notify_status("zclassic23 started");

    /* Pet half: dedicated thread — see the heartbeat section header for why
     * it must not ride the health ring. */
    atomic_store(&g_pet_stop, false);
    // thread-supervision-ok:pets-systemd-from-atomics-only-a-supervised-child-could-be-starved-by-the-ring-contention-this-thread-exists-to-survive
    if (thread_registry_spawn("zcl_sd_watchdog_pet",
                              boot_sd_watchdog_pet_main, NULL,
                              &g_pet_tid) != 0) {
        /* No pet thread = no heartbeat once boot progress goes quiet. Fail
         * the service start: under WatchdogSec a silently un-petted unit is
         * a kill loop waiting for the first long collect stall. */
        LOG_FAIL("sd_watchdog", "pet thread spawn failed");
        return false;
    }
    atomic_store(&g_pet_handle_set, true);
    printf("[sd-watchdog] active, collect_period=%ds WATCHDOG_USEC=%llu\n",
           period_secs, (unsigned long long)wd_us);
    return true;
}

/* Stop the systemd watchdog heartbeat (runtime service kernel entry). */
void boot_sd_watchdog_stop(void *ctx)
{
    (void)ctx;
    atomic_store(&g_pet_stop, true);
    if (atomic_load(&g_pet_handle_set)) {
        pthread_join(g_pet_tid, NULL);
        atomic_store(&g_pet_handle_set, false);
    }
    if (g_sd_watchdog_id != HEALTH_INVALID_ID) {
        health_unregister(g_sd_watchdog_id);
        g_sd_watchdog_id = HEALTH_INVALID_ID;
    }
    if (sd_notify_is_active())
        sd_notify_stopping("shutdown");
    sd_notify_set_health_check(NULL);
    g_sd_watchdog_ctx = NULL;
}
