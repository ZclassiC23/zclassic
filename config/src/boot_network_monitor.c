/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot glue for the network monitor — see config/boot_network_monitor.h.
 * Adapts network_monitor_start/stop to the service-kernel start/stop contract
 * and registers it as an OPTIONAL maintenance service. Kept out of boot.c so
 * the boot service table stays under the file-size ceiling. */

#include "config/boot_network_monitor.h"

#include "kernel/service_kernel.h"
#include "models/database.h"
#include "services/network_monitor.h"
#include "util/result.h"

#include <stdio.h>

static bool boot_network_monitor_service_start(void *ctx)
{
    struct node_db *db = ctx;
    struct network_monitor_config cfg;
    network_monitor_config_defaults(&cfg);
    struct zcl_result nr =
        network_monitor_start(&cfg, db && db->open ? db : NULL);
    if (nr.ok) {
        printf("Network monitor started (interval=%ds retain=%d rows)\n",
               cfg.sample_interval_secs, cfg.retain_rows);
        return true;
    }
    fprintf(stderr, "[boot] %s:%d network_monitor_start failed: code=%d %s\n",
            nr.source_file, nr.source_line, nr.code, nr.message);
    return false;
}

static void boot_network_monitor_service_stop(void *ctx)
{
    (void)ctx;
    network_monitor_stop();
}

bool boot_register_network_monitor_service(struct zcl_service_kernel *k,
                                           struct node_db *db)
{
    const struct zcl_service_spec spec = {
        .name = "network_monitor",
        .start = boot_network_monitor_service_start,
        .stop = boot_network_monitor_service_stop,
        .ctx = db,
        .flags = ZCL_SERVICE_OPTIONAL,
    };
    return zcl_service_kernel_register(k, &spec);
}
