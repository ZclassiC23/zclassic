/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_NET_PORT_POLICY_H
#define ZCL_NET_PORT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define ZCL_NET_REACHABLE_PORTS_SQL "8033,18033,8034,9033,20022"

static inline bool zcl_net_port_is_reachable_candidate(uint16_t port)
{
    switch (port) {
    case 8033:
    case 18033:
    case 8034:
    case 9033:
    case 20022:
        return true;
    default:
        return false;
    }
}

#endif /* ZCL_NET_PORT_POLICY_H */
