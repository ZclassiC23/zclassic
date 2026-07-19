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
 * Owns: the two file-statics g_sd_watchdog_id / g_sd_watchdog_ctx that track
 * the registered health-ring periodic. The tick runs on the lib/health
 * periodic ring (no thread spawn here), so there is no separate supervisor
 * child to register.
 *
 * The start/stop entry points are registered into the runtime service kernel
 * by boot_register_runtime_services() in boot_services.c, so their prototypes
 * live in config/boot_internal.h. boot_sd_watchdog_tick stays private here. */

#include "platform/time_compat.h"
#include "config/boot_internal.h"
#include "services/node_health_service.h"
#include "services/binary_ab_fallback.h"
#include "health/heartbeat.h"
#include "util/sd_notify.h"
#include "util/boot_progress.h"
#include "util/supervisor.h"
#include "util/supervisor_backstop.h"
#include <stdio.h>
#include <time.h>

/* ── systemd watchdog heartbeat ─────────────────────────────────
 * Pings WATCHDOG=1 on the systemd notify socket every WATCHDOG_USEC/2
 * microseconds when node health is OK. When health degrades (mirror
 * lag SLO fatal, tip_advance_age past deadman, etc.) the heartbeat
 * stops and systemd's WatchdogSec=N timer trips, restarting the unit.
 *
 * No-op when NOTIFY_SOCKET is absent (e.g. CLI invocation). */
static health_subsystem_id g_sd_watchdog_id = HEALTH_INVALID_ID;
static struct boot_svc_ctx *g_sd_watchdog_ctx;

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

static void boot_sd_watchdog_tick(void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    if (!sd_notify_is_active() || !svc)
        return;
    struct node_health_snapshot snap = {0};
    node_health_collect(&snap, svc->node_db, svc->state);
    /* Hybrid heartbeat: ping if either the steady-state
     * health snapshot is healthy OR a long-running synchronous worker
     * has bumped boot_progress_tick recently. Snapshot import bulk
     * INSERT, block-by-block catchup, and UTXO replay all take longer
     * than WatchdogSec/2 and would otherwise be killed mid-write.
     * Freshness window mirrors WATCHDOG_USEC — if both signals expire,
     * the watchdog times out as intended. */
    bool recent_progress = false;
    {
        int64_t last_us = boot_progress_last_us();
        if (last_us > 0) {
            uint64_t wd_us = sd_notify_watchdog_usec();
            int64_t window_us = wd_us > 0 ? (int64_t)wd_us
                                          : (int64_t)(120 * 1000000LL);
            struct timespec now_ts;
            platform_time_monotonic_timespec(&now_ts);
            int64_t now_us = (int64_t)now_ts.tv_sec * 1000000
                           + (int64_t)now_ts.tv_nsec / 1000;
            if (now_us - last_us < window_us)
                recent_progress = true;
        }
    }
    bool supervisor_alive = boot_sd_watchdog_supervisor_alive();
    if ((snap.healthy || recent_progress) && supervisor_alive) {
        sd_notify_watchdog_ping();
    }
    /* Refresh status line — useful for `systemctl status zclassic23`.
     * Include the recent-progress label so operators can see which
     * subsystem is keeping the watchdog alive during bulk ops, and a
     * supervisor=FROZEN marker when Pillar 7's gate is the reason the
     * ping stopped (as opposed to a plain health/progress lapse). */
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

    /* Activation-ready reached. This is the LAST runtime service to start
     * (boot_services.c spec order), so arriving here IS the node's own
     * definition of "booted successfully". Tell the binary-A/B launcher:
     * reset its boot-failure streak to 0 and, unless we are the fallback
     * slot, promote the current binary to last-good. Runs before the
     * sd_notify_init() early-return below so it fires whether or not the
     * node is under systemd notify supervision — the signal is the launcher
     * env, not systemd. No-op when launched directly (env unset). */
    binary_ab_promote_on_ready_env();

    if (!sd_notify_init()) {
        /* Not running under systemd notify supervision (e.g. invoked
         * from a CLI). Silent success — the unit is functionally
         * complete without WatchdogSec. */
        return true;
    }
    g_sd_watchdog_ctx = svc;

    /* Pick cadence: half the configured WatchdogSec, clamped to at
     * least 5s so we never DoS systemd with too-frequent pings. When
     * WATCHDOG_USEC is unset the unit didn't ask for a watchdog —
     * still emit periodic STATUS= lines on a 30s cadence so operators
     * see a live status in `systemctl status`. */
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
    printf("[sd-watchdog] active, period=%ds WATCHDOG_USEC=%llu\n",
           period_secs, (unsigned long long)wd_us);
    return true;
}

/* Stop the systemd watchdog heartbeat (runtime service kernel entry). */
void boot_sd_watchdog_stop(void *ctx)
{
    (void)ctx;
    if (g_sd_watchdog_id != HEALTH_INVALID_ID) {
        health_unregister(g_sd_watchdog_id);
        g_sd_watchdog_id = HEALTH_INVALID_ID;
    }
    if (sd_notify_is_active())
        sd_notify_stopping("shutdown");
    sd_notify_set_health_check(NULL);
    g_sd_watchdog_ctx = NULL;
}
