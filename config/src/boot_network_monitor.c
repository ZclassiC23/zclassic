/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot glue for the network-observability maintenance services — see
 * config/boot_network_monitor.h. Adapts network_monitor_start/stop AND
 * network_crawler_start/stop to the service-kernel start/stop contract and
 * registers each as an OPTIONAL maintenance service. Kept out of boot.c so the
 * boot service table stays under the file-size ceiling. */

#include "config/boot_network_monitor.h"

#include "kernel/service_kernel.h"
#include "models/database.h"
#include "net/connman.h"
#include "services/network_monitor.h"
#include "services/network_crawler.h"
#include "services/sync_monitor.h"
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

/* ── network crawler (whole-network observatory) ─────────────────────────
 *
 * By the time maintenance services start (SERVICES_RUNNING boot stage),
 * NETWORK_READY has already run app_init_services -> sync_monitor_set_context,
 * so sync_monitor_connman() is populated here. The crawler only READs the
 * address table it is seeded with. */

static bool boot_network_crawler_service_start(void *ctx)
{
    (void)ctx;
    struct connman *cm = sync_monitor_connman();
    struct addr_man *am = cm ? connman_addrman(cm) : NULL;
    /* A NULL addrman is not a boot failure: the crawler worker still registers
     * and idles (each round no-ops on a NULL address table), naming the
     * degradation instead of stopping the node. Pass NULL cfg so the crawler
     * builds its own defaults and applies the -netcrawl / ZCL_NETWORK_CRAWLER
     * opt-out itself, then logs its real enabled/idle state. */
    struct zcl_result nr = network_crawler_start(NULL, am);
    if (nr.ok) {
        printf("Network crawler registered (seed_addrman=%s)\n",
               am ? "wired" : "none-yet");
        return true;
    }
    fprintf(stderr, "[boot] %s:%d network_crawler_start failed: code=%d %s\n",
            nr.source_file, nr.source_line, nr.code, nr.message);
    return false;
}

static void boot_network_crawler_service_stop(void *ctx)
{
    (void)ctx;
    network_crawler_stop();
}

bool boot_register_network_crawler_service(struct zcl_service_kernel *k)
{
    const struct zcl_service_spec spec = {
        .name = "network_crawler",
        .start = boot_network_crawler_service_start,
        .stop = boot_network_crawler_service_stop,
        .ctx = NULL,
        .flags = ZCL_SERVICE_OPTIONAL,
    };
    return zcl_service_kernel_register(k, &spec);
}
