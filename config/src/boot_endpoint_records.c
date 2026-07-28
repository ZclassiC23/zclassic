/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Signed endpoint records at the composition root: the chain binding and
 * the discovery projection. See config/boot_endpoint_records.h.
 *
 * ── THE SEAM THAT IS DELIBERATELY LEFT OPEN ────────────────────────
 * A verified record carries clearnet address + port as well as an onion
 * hostname, and this file projects ONLY the onion half, because the
 * narrow discovery path (struct onion_peer) has nowhere to put an IP.
 * Feeding the clearnet half in as an addrman candidate is a real next
 * step and it is not built here: the only sanctioned way for a
 * directory to influence peer selection is addrman_set_reputation_weight
 * (lib/net/src/addrman.c), bounded to a [1.0, 4.0] dial-chance
 * multiplier that structurally cannot exclude a peer, and wiring it is
 * its own slice with its own proof. The clearnet fields are carried,
 * verified, and unused — stated, not hidden. */

#include "config/boot_endpoint_records.h"

#include "config/runtime.h"

#include "models/zid_identity.h"

#include "platform/time_compat.h"
#include "vcs/zendp_swarm.h"

#include "base/log_macros.h"

#include <stdio.h>
#include <string.h>

#define BER_LOG "boot.endpoint_records"

/* Bounded by the directory itself; a stack array of this size is one
 * page and the call is on the discovery path, never the hot path. */
#define BER_RECORDS_MAX ZENDP_DIR_MAX

/* Map one zid_identities row to the anchor verdict. The ONE place a
 * projection row becomes a chain answer.
 *
 * Returning false means "the question could not be asked" (no node.db,
 * or a row whose status literal is not one this build knows). It never
 * means "no such identity" — that is a true return carrying
 * ZENDP_ANCHOR_ABSENT, and the two are reported by different names all
 * the way up. */
static bool boot_endpoint_anchor_lookup(void *ctx, const uint8_t pubkey[32],
                                        struct zendp_anchor *out)
{
    (void)ctx;
    if (!pubkey || !out)
        LOG_FAIL(BER_LOG, "anchor lookup: NULL argument (pk=%p out=%p)",
                 (const void *)pubkey, (void *)out);
    memset(out, 0, sizeof(*out));

    struct node_db *ndb = app_runtime_node_db();
    if (!ndb)
        LOG_FAIL(BER_LOG,
                 "anchor lookup: node.db is not open yet — the chain cannot "
                 "be asked, so no record may be treated as anchored");

    struct zid_identity row;
    if (!db_zid_identity_find(ndb, pubkey, &row)) {
        /* A legitimate negative answer, not a failure. */
        out->state = ZENDP_ANCHOR_ABSENT;
        return true;
    }

    out->anchor_height = row.anchor_height;
    out->updated_height = row.updated_height;
    if (strcmp(row.status, ZID_IDENTITY_STATUS_ACTIVE) == 0)
        out->state = ZENDP_ANCHOR_ACTIVE;
    else if (strcmp(row.status, ZID_IDENTITY_STATUS_ROTATED) == 0)
        out->state = ZENDP_ANCHOR_ROTATED;
    else if (strcmp(row.status, ZID_IDENTITY_STATUS_REVOKED) == 0)
        out->state = ZENDP_ANCHOR_REVOKED;
    else
        LOG_FAIL(BER_LOG,
                 "anchor lookup: identity row carries an unknown status '%s' "
                 "— refusing to guess a verdict",
                 row.status);
    return true;
}

void boot_endpoint_records_register(void)
{
    zendp_set_anchor_lookup(boot_endpoint_anchor_lookup, NULL);
}

int boot_endpoint_record_peers(void *ctx, struct onion_peer *out, size_t max)
{
    (void)ctx;
    if (!out || max == 0)
        return 0;
    if (max > BER_RECORDS_MAX)
        max = BER_RECORDS_MAX;

    uint64_t now = (uint64_t)platform_time_wall_unix();

    struct zendp_record_view views[BER_RECORDS_MAX];
    size_t n = zendp_global_records(now, views, max);
    if (n == 0)
        return 0;

    /* Widen into the rich type (which carries the port, the services
     * bitmask, the expiry and the provenance), then narrow through the
     * one adapter, so the extra facts exist and are auditable even
     * though this path can only carry the hostname. */
    struct onion_endpoint eps[BER_RECORDS_MAX];
    int built = 0;
    for (size_t i = 0; i < n; i++) {
        const struct zendp_record_view *v = &views[i];
        struct onion_endpoint *ep = &eps[built];
        memset(ep, 0, sizeof(*ep));
        if (v->ep.flags & ZENDP_HAS_ONION)
            snprintf(ep->hostname, sizeof(ep->hostname), "%s", v->ep.onion);
        ep->onion_port = v->ep.onion_port;
        memcpy(ep->ipv4, v->ep.ipv4, sizeof(ep->ipv4));
        ep->ipv4_port = v->ep.ipv4_port;
        memcpy(ep->ipv6, v->ep.ipv6, sizeof(ep->ipv6));
        ep->ipv6_port = v->ep.ipv6_port;
        ep->services = v->ep.services;
        ep->height = (int)v->ep.height;
        ep->expiry = v->expiry;
        ep->seq = v->seq;
        memcpy(ep->master_pubkey, v->master_pubkey,
               sizeof(ep->master_pubkey));
        ep->anchor_height = v->anchor_height;
        ep->provenance = ONION_PROV_ANCHORED;
        built++;
    }

    int rejected = 0;
    int kept = onion_endpoints_to_peers(eps, built, out, max, now, &rejected);
    if (rejected > 0)
        LOG_WARN(BER_LOG,
                 "discovery: dropped %d endpoint record(s) with a malformed "
                 "or expired onion hostname (%d kept)", rejected, kept);
    return kept;
}
