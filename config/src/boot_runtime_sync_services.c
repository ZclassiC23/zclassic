/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_runtime_sync_services.c — runtime service-kernel start/stop adapters
 * for the sync-adjacent services that influence the tip / header path.
 *
 * Part of the boot composition root. These ten adapters wrap the long-running
 * services that drive header and body acquisition toward the tip:
 *   - header_probe      (diagnostic header lag probe + supervised poll Job)
 *   - legacy_mirror     (always-on mirror sync against the legacy node)
 *   - gap_fill          (background body gap-fill)
 *   - zclassicd_oracle  (external-node height oracle)
 *   - rolling_anchor    (rolling SHA3 anchor supervisor contract)
 *
 * They are runtime-service start/stop WRAPPERS invoked through the runtime
 * service-kernel spec table (boot_register_runtime_services() in
 * boot_services.c); they are NOT part of the SIGTERM shutdown sequence and
 * touch no coins-flush ordering. Each wrapper operates purely on the
 * boot_svc_ctx passed as ctx — it owns no file-static. Each underlying
 * *_start / *_register owns its own worker thread and supervisor liveness
 * contract inside its service module, so no thread is spawned here.
 *
 * Registered into the runtime service kernel by boot_register_runtime_services()
 * in boot_services.c, so the ten prototypes live in config/boot_internal.h. */

#include "config/boot_internal.h"
#include "services/header_probe.h"
#include "jobs/header_probe_poll.h"
#include "services/legacy_mirror_sync_service.h"
#include "services/gap_fill_service.h"
#include "services/zclassicd_oracle_service.h"
#include "services/rolling_anchor_service.h"
#include "net/download.h"
#include <stdio.h>

/* Initialize header_probe and register its supervised poll Job (runtime svc). */
bool boot_header_probe_start(void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    if (!svc)
        return false;
    struct header_probe_config cfg = {0};
    /* Keep header_probe initialized/running for diagnostics and manual
     * use, but leave lockstep catch-up ownership with legacy_mirror. */
    cfg.lag_threshold = 1000000000;
    {
        struct zcl_result hpr = header_probe_init(&cfg, svc->state, svc->params);
        if (!hpr.ok) {
            fprintf(stderr,  // obs-ok:header-probe-init-fail
                    "[header-probe] init failed: %s\n", hpr.message);
            return false;
        }
    }
    /* Register the polling cadence as a supervised Job in the network
     * domain. Same 30 s cadence, same RPC, same accept_block_header path;
     * the supervisor owns liveness so `zcl_state subsystem=supervisor`
     * exposes last_tick_age_us and ticks_run for the poll. */
    header_probe_poll_register();
    if (header_probe_poll_is_registered()) {
        printf("[header-probe] poll Job registered with net supervisor\n");
        return true;
    }
    fprintf(stderr,  // obs-ok:header-probe-poll-fallback
            "[header-probe] WARN poll Job register failed; "
            "header_probe RPC + state introspection still available\n");
    return true;  /* non-fatal — manual MCP calls still work */
}

/* Stop the header_probe runtime service. */
void boot_header_probe_stop(void *ctx)
{
    (void)ctx;
    /* No-op: supervisor unregister happens at supervisor_stop(). */
}

/* Initialize and start the always-on legacy mirror sync (runtime svc). */
bool boot_legacy_mirror_start(void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    if (!svc)
        return false;
    struct legacy_mirror_sync_config cfg = {0};
    cfg.enabled = true;
    if (!legacy_mirror_sync_init(&cfg, svc->state, svc->coins_tip,
                                 svc->params, svc->datadir).ok)
        return false;
    if (legacy_mirror_sync_start().ok) {
        printf("[legacy-mirror] always-on mirror sync started\n");
        return true;
    }
    return false;
}

/* Stop the legacy mirror sync runtime service. */
void boot_legacy_mirror_stop(void *ctx)
{
    (void)ctx;
    legacy_mirror_sync_stop();
}

/* Start the background body gap-fill service (runtime svc). */
bool boot_gap_fill_start(void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    if (!svc)
        return false;
    struct zcl_result gr = gap_fill_start(svc->state, msg_get_download_mgr());
    if (gr.ok) {
        printf("[gap-fill] background gap-fill service started\n");
        return true;
    }
    fprintf(stderr, "WARNING: gap_fill_start failed: %s:%d code=%d %s\n",
            gr.source_file, gr.source_line, gr.code, gr.message);
    return false;
}

/* Stop the background body gap-fill service. */
void boot_gap_fill_stop(void *ctx)
{
    (void)ctx;
    gap_fill_stop();
}

/* Start the external zclassicd height oracle (runtime svc). */
bool boot_zclassicd_oracle_start(void *ctx)
{
    (void)ctx;
    if (zclassicd_oracle_start().ok)
        printf("[oracle] zclassicd oracle service started\n");
    return true;
}

/* Stop the external zclassicd height oracle. */
void boot_zclassicd_oracle_stop(void *ctx)
{
    (void)ctx;
    zclassicd_oracle_stop();
}

/* Register the rolling SHA3 anchor supervisor contract (runtime svc). */
bool boot_rolling_anchor_start(void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    if (!svc)
        return false;
    if (rolling_anchor_start(svc->state, svc->datadir).ok)
        printf("[rolling-anchor] supervisor contract registered\n");
    return true;
}

/* Stop the rolling SHA3 anchor service. */
void boot_rolling_anchor_stop(void *ctx)
{
    (void)ctx;
    rolling_anchor_stop();
}
