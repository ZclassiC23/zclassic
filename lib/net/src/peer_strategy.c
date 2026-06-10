/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Peer connectivity strategy implementation.
 * Probes every available transport at startup and selects the
 * fastest reachable path to each peer. */

#include "net/peer_strategy.h"
#include "net/nat.h"
#include "net/tor_integration.h"
#include "chain/chainparams.h"
#include "util/log_macros.h"

#include <string.h>
#include <stdio.h>

const char *peer_transport_name(enum peer_transport t)
{
    switch (t) {
    case TRANSPORT_CLEARNET: return "clearnet";
    case TRANSPORT_TOR:      return "tor";
    case TRANSPORT_NAT_PMP:  return "nat-pmp";
    case TRANSPORT_UPNP:     return "upnp";
    }
    return "unknown";
}

/* ── Self-discovery ──────────────────────────────────────── */

bool peer_strategy_discover_self(struct node_profile *profile,
                                 uint16_t listen_port)
{
    if (!profile)
        LOG_FAIL("net", "peer_strategy_discover_self: null profile");

    memset(profile, 0, sizeof(*profile));
    profile->public_port = listen_port;

    /* Regtest (fMineBlocksOnDemand) is a local, connect-only test chain that
     * never needs NAT-PMP/UPnP port mapping or public-reachability discovery.
     * Skip it: this runs SYNCHRONOUSLY during boot (config/src/boot_services.c)
     * ahead of the reducer stage-pipeline init, and the UPnP SSDP/SOAP probe
     * blocks for tens of seconds on a host whose gateway ignores it — wedging
     * boot so the consensus engine never starts (the node answers RPC because
     * the frontend already started, masking the stall). Gated on
     * fMineBlocksOnDemand: true ONLY for regtest, false on main/testnet
     * (lib/chain/src/chainparams.c), so live-network discovery is unchanged. */
    const struct chain_params *cp = chain_params_get();
    if (cp && cp->fMineBlocksOnDemand)
        return false;

    /* 1. Try NAT-PMP port mapping (nat.h tries NAT-PMP first). */
    uint8_t mapped_ip[4] = {0};
    if (nat_add_port_mapping(listen_port, listen_port, 7200, "TCP",
                             mapped_ip)) {
        /* Determine which method succeeded: NAT-PMP replies include the
         * public IP in the response; UPnP discovers it via SOAP.  The
         * nat layer doesn't expose which path won, but NAT-PMP is tried
         * first — so if the call succeeds we mark NAT-PMP and fall back
         * to checking UPnP only if NAT-PMP was unavailable. For now,
         * mark whichever succeeded as available. */
        memcpy(profile->public_ip, mapped_ip, 4);
        profile->has_public_ip = true;

        /* Heuristic: NAT-PMP gateways are typically Apple AirPort or
         * miniupnpd with PMP enabled.  Try a second PMP-only probe
         * to distinguish.  If the gateway answers, it's PMP. */
        uint8_t gw[4];
        if (nat_get_gateway(gw))
            profile->nat_pmp_available = true;
        else
            profile->upnp_available = true;
    } else {
        /* Port mapping failed — try naked IP discovery. */
        if (nat_discover_public_ip(mapped_ip)) {
            memcpy(profile->public_ip, mapped_ip, 4);
            profile->has_public_ip = true;
        }
    }

    /* 2. Check Tor .onion availability. */
    const char *onion = tor_integration_get_onion_address();
    if (onion && onion[0] != '\0') {
        size_t len = strlen(onion);
        if (len < sizeof(profile->onion_address)) {
            memcpy(profile->onion_address, onion, len + 1);
            profile->tor_available = true;
        }
    }

    return profile->has_public_ip || profile->tor_available;
}

/* ── Transport selection ─────────────────────────────────── */

static bool addr_is_onion(const char *addr)
{
    if (!addr)
        return false;
    size_t len = strlen(addr);
    /* .onion or .onion:port */
    const char *dot = strstr(addr, ".onion");
    if (!dot)
        return false;
    /* Must be at end or followed by ':' (port) */
    size_t suffix = (size_t)(dot - addr) + 6;
    return suffix == len || addr[suffix] == ':';
}

enum peer_transport peer_strategy_select(const struct node_profile *self,
                                         const char *peer_addr)
{
    if (!self || !peer_addr)
        return TRANSPORT_TOR;

    /* Onion addresses always go via Tor. */
    if (addr_is_onion(peer_addr))
        return TRANSPORT_TOR;

    /* Clearnet peer — use the fastest available outbound path. */
    if (self->has_public_ip)
        return TRANSPORT_CLEARNET;

    if (self->nat_pmp_available)
        return TRANSPORT_NAT_PMP;

    if (self->upnp_available)
        return TRANSPORT_UPNP;

    /* No clearnet reachability — route via Tor. */
    return TRANSPORT_TOR;
}

/* ── Address advertisement ───────────────────────────────── */

int peer_strategy_get_addresses(const struct node_profile *self,
                                char (*addrs_out)[68], int max_addrs)
{
    if (!self || !addrs_out || max_addrs <= 0)
        return 0;

    int count = 0;

    /* Advertise clearnet address if known. */
    if (self->has_public_ip && count < max_addrs) {
        snprintf(addrs_out[count], 68, "%u.%u.%u.%u:%u",
                 self->public_ip[0], self->public_ip[1],
                 self->public_ip[2], self->public_ip[3],
                 self->public_port);
        count++;
    }

    /* Advertise .onion address if available. */
    if (self->tor_available && self->onion_address[0] && count < max_addrs) {
        snprintf(addrs_out[count], 68, "%s", self->onion_address);
        count++;
    }

    return count;
}
