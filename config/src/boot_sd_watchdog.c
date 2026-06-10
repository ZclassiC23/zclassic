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
#include "health/heartbeat.h"
#include "util/sd_notify.h"
#include "util/boot_progress.h"
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
    if (snap.healthy || recent_progress) {
        sd_notify_watchdog_ping();
    }
    /* Refresh status line — useful for `systemctl status zclassic23`.
     * Include the recent-progress label so operators can see which
     * subsystem is keeping the watchdog alive during bulk ops. */
    char status[320];
    const char *label = recent_progress ? boot_progress_last_label() : NULL;
    snprintf(status, sizeof(status),
             "h=%d peers=%zu mirror_lag=%lld sev=%s%s%s",
             snap.tip_height, snap.peer_count,
             (long long)snap.mirror_lag_blocks,
             snap.mirror_lag_breach_severity[0]
                 ? snap.mirror_lag_breach_severity : "none",
             label ? " busy=" : "",
             label ? label : "");
    sd_notify_status(status);
}

/* Start the systemd watchdog heartbeat (runtime service kernel entry). */
bool boot_sd_watchdog_start(void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    if (!svc)
        return false;
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
    g_sd_watchdog_ctx = NULL;
}
