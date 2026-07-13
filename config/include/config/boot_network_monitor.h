/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot glue for the network monitor — registers the supervised network-monitor
 * sampler as an OPTIONAL maintenance service on a service kernel. Extracted from
 * boot.c so the boot service table stays small (file-size ceiling). */

#ifndef ZCL_CONFIG_BOOT_NETWORK_MONITOR_H
#define ZCL_CONFIG_BOOT_NETWORK_MONITOR_H

#include <stdbool.h>

struct zcl_service_kernel;
struct node_db;

/* Register the network_monitor service (start/stop wrappers + spec) on kernel k,
 * with db as the retained-history backing store. Returns the register result. */
bool boot_register_network_monitor_service(struct zcl_service_kernel *k,
                                           struct node_db *db);

#endif /* ZCL_CONFIG_BOOT_NETWORK_MONITOR_H */
