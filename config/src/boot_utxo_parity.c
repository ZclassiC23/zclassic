/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Boot wiring for the standing UTXO parity service. Kept in its own TU so the
 * 800-line composition-root mega-files (boot_services.c) gain only a single
 * registration call — this carries the start/stop bodies and the spec.
 *
 * The service is DORMANT by default: cfg.enabled=false and no reference is
 * wired unless an operator opts in via ZCL_PARITY_ENABLE (and a co-located
 * zclassicd is reachable). The EV_CHAIN_TIP_COMMIT observer and MCP
 * introspection are always installed because they are cheap and never touch
 * the live node.
 */

#include "config/boot_internal.h"
#include "config/runtime.h"

#include "services/utxo_parity_service.h"
#include "services/utxo_reference_source.h"
#include "jobs/utxo_parity_poll.h"

#include "util/log_macros.h"

#include <stdio.h>
#include <stdlib.h>

/* Caller-owned backing store for the (dormant-by-default) zclassicd reference.
 * Static lifetime so the vtable's `self` pointer outlives the service. */
static struct utxo_reference_source           g_parity_ref;
static struct utxo_reference_source_zclassicd g_parity_ref_zclassicd;

static bool boot_utxo_parity_start(void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    if (!svc)
        return false;

    struct utxo_parity_config cfg = {0};
    cfg.enabled = false;            /* dormant default — opt-in only */
    cfg.finality_depth = 100;       /* matches ORACLE_TIP_SAFETY_MARGIN */
    cfg.max_checks_per_tick = 1;    /* bound full-set SHA3 cost */
    snprintf(cfg.rpc.host, sizeof(cfg.rpc.host), "%s", "127.0.0.1");
    cfg.rpc.port = 8232;

    struct zcl_result r = utxo_parity_init(&cfg, app_runtime_node_db());
    if (!r.ok) {
        fprintf(stderr,  // obs-ok:utxo-parity-init-fail
                "[utxo-parity] init failed at %s:%d code=%d: %s\n",
                r.source_file ? r.source_file : "?", r.source_line,
                r.code, r.message);
        return false;
    }

    /* Always install the cheap, live-safe finalized-frontier observer +
     * leave the service introspectable via zcl_state. */
    utxo_parity_observe_finalization();

    /* Only when an operator opts in: wire the coarse zclassicd reference and
     * register the supervised poll Job. Even then the service stays inert
     * unless cfg.enabled is later turned on (the env gate is the runtime
     * switch). The reference is height-only, so it can never false-page. */
    if (cfg.enabled || getenv("ZCL_PARITY_ENABLE")) {
        struct zcl_result zr = utxo_reference_source_zclassicd_init(
            &g_parity_ref, &g_parity_ref_zclassicd, &cfg.rpc);
        if (zr.ok) {
            utxo_parity_set_reference_source(&g_parity_ref);
            utxo_parity_poll_register();
            if (utxo_parity_poll_is_registered())
                printf("[utxo-parity] poll Job registered (coarse reference)\n");
        } else {
            fprintf(stderr,  // obs-ok:utxo-parity-ref-dormant
                    "[utxo-parity] reference unavailable; staying dormant: %s\n",
                    zr.message);
        }
    }
    return true;  /* non-fatal: the observer + introspection still work */
}

static void boot_utxo_parity_stop(void *ctx)
{
    (void)ctx;
    /* No-op: supervisor unregister happens at supervisor_stop(); the static
     * reference store is process-lived. */
}

bool boot_utxo_parity_register(struct boot_svc_ctx *svc)
{
    if (!svc)
        return false;
    const struct zcl_service_spec spec = {
        .name = "utxo_parity",
        .start = boot_utxo_parity_start,
        .stop = boot_utxo_parity_stop,
        .ctx = svc,
        .flags = ZCL_SERVICE_OPTIONAL,
    };
    return zcl_service_kernel_register(&svc->runtime_kernel, &spec);
}
