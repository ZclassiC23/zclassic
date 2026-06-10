/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * NAT traversal — pure C23, no external libraries.
 * Supports NAT-PMP (RFC 6886) and UPnP IGD (SSDP + SOAP).
 * Graceful degradation: tries NAT-PMP first, then UPnP, then gives up. */

#ifndef ZCL_NET_NAT_H
#define ZCL_NET_NAT_H

#include <stdint.h>
#include <stdbool.h>

/* Try to open an external port mapping.
 * Tries NAT-PMP first, falls back to UPnP IGD.
 * Returns true if a mapping was created.
 * external_port: the port to open on the router
 * internal_port: the local port to forward to
 * lifetime: mapping lifetime in seconds (0 = delete)
 * protocol: "TCP" or "UDP"
 * public_ip_out: if non-NULL, filled with our public IP (4 bytes) */
bool nat_add_port_mapping(uint16_t external_port, uint16_t internal_port,
                           uint32_t lifetime, const char *protocol,
                           uint8_t public_ip_out[4]);

/* Discover public IP via NAT-PMP or UPnP. */
bool nat_discover_public_ip(uint8_t ip_out[4]);

/* Get the default gateway IP. */
bool nat_get_gateway(uint8_t gw_out[4]);

#endif
