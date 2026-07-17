/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * "network" dumpstate rollup — one call answering "what does the node know
 * about the ZClassic network right now?"
 *
 * This is a pure ROLLUP: every number here is already computed by an
 * existing owner. This file never re-derives a fold, a histogram, or a
 * peer-iteration loop — it calls each owner's existing dump/snapshot
 * function (or, for the two bare counters below, the same O(1) accessor
 * the owning controller already uses) and republishes the result under one
 * subsystem name:
 *
 *   - connman: live connection counts + addrman size, via the existing
 *     rpc_net_get_connman() handle and the same connman_get_node_count() /
 *     connman_outbound_healthy_count() / addrman_size() accessors
 *     network_controller.c's own network_counts_collect() uses — no
 *     re-walk of cm->manager.nodes[] here.
 *   - peer_floor: the "net.outbound_floor" supervisor liveness contract,
 *     read via supervisor_dump_state_json(..., "net") (the SAME JSON the
 *     `supervisor` subsystem already renders) rather than re-deriving the
 *     healthy-outbound count ourselves.
 *   - chain_view: network_monitor_dump_state_json() verbatim (modal/max
 *     height, our delta, fork clusters — our connected peers' view).
 *   - census: network_crawler_dump_state_json() verbatim (whole-network
 *     observatory: reachable count, version histogram, onion/clearnet
 *     split, eclipse_suspected + evidence).
 *   - peer_lifecycle: peer_lifecycle_dump_state_json() verbatim (connect/
 *     handshake/timeout/reject counters).
 *   - tip_comparison: our height vs. both modal heights above — plain
 *     subtraction over fields already folded by network_monitor / the
 *     crawler, read back out of the JSON already produced for chain_view /
 *     census (no second fold).
 *
 * See CLAUDE.md "Adding state introspection". Reentrant-safe: no state of
 * its own; every nested call already documents its own reentrancy/locking.
 */

#include "controllers/diagnostics_internal.h"
#include "controllers/network_controller.h"

#include "json/json.h"
#include "net/addrman.h"
#include "net/connman.h"
#include "net/peer_lifecycle.h"
#include "services/network_crawler.h"
#include "services/network_monitor.h"
#include "util/supervisor.h"

#include <string.h>

/* Name of the supervisor liveness contract registered by
 * net_supervisor_register() (app/supervisors/src/net_supervisor.c). Not
 * exported by that module's header, so it is mirrored here as a lookup key
 * only — this file reads the contract's already-published fields, it does
 * not reimplement or duplicate its tick/stall logic. */
#define NET_PEER_FLOOR_CHILD_NAME "net.outbound_floor"
/* Domain label net_supervisor_register() registers the child into
 * (app/supervisors/src/domains.c: supervisor_create_domain("net")). */
#define NET_SUPERVISOR_DOMAIN "net"

static void push_connman_json(struct json_value *out, struct connman *cm,
                              int64_t *out_outbound_healthy)
{
    int64_t connected = -1, outbound_healthy = -1, addrman_entries = -1;

    if (cm) {
        connected = (int64_t)connman_get_node_count(cm);
        outbound_healthy = (int64_t)connman_outbound_healthy_count(cm);
        zcl_mutex_lock(&cm->manager.addrman.cs);
        addrman_entries = (int64_t)addrman_size(&cm->manager.addrman);
        zcl_mutex_unlock(&cm->manager.addrman.cs);
    }
    *out_outbound_healthy = outbound_healthy;

    struct json_value obj = {0};
    json_set_object(&obj);
    json_push_kv_bool(&obj, "wired", cm != NULL);
    json_push_kv_int(&obj, "connected_peers", connected);
    json_push_kv_int(&obj, "outbound_healthy", outbound_healthy);
    json_push_kv_int(&obj, "addrman_size", addrman_entries);
    json_push_kv(out, "connman", &obj);
    json_free(&obj);
}

static void push_peer_floor_json(struct json_value *out,
                                 int64_t connman_outbound_healthy_fallback)
{
    struct json_value net_domain = {0};
    json_set_object(&net_domain);
    supervisor_dump_state_json(&net_domain, NET_SUPERVISOR_DOMAIN);

    bool found = false;
    int64_t healthy = connman_outbound_healthy_fallback;
    int64_t last_tick_age_us = -1, deadline_secs = -1;
    int64_t ticks_run = 0, stall_fires = 0;

    const struct json_value *children = json_get(&net_domain, "children");
    if (children && children->type == JSON_ARR) {
        size_t n = json_size(children);
        for (size_t i = 0; i < n; i++) {
            const struct json_value *c = json_at(children, i);
            const struct json_value *name = c ? json_get(c, "name") : NULL;
            const char *name_str = name ? json_get_str(name) : NULL;
            if (!name_str ||
                strcmp(name_str, NET_PEER_FLOOR_CHILD_NAME) != 0)
                continue;
            found = true;
            const struct json_value *v;
            if ((v = json_get(c, "progress_marker")))
                healthy = json_get_int(v);
            if ((v = json_get(c, "last_tick_age_us")))
                last_tick_age_us = json_get_int(v);
            if ((v = json_get(c, "deadline_secs")))
                deadline_secs = json_get_int(v);
            if ((v = json_get(c, "ticks_run")))
                ticks_run = json_get_int(v);
            if ((v = json_get(c, "stall_fires")))
                stall_fires = json_get_int(v);
            break;
        }
    }
    json_free(&net_domain);

    struct json_value pf = {0};
    json_set_object(&pf);
    json_push_kv_bool(&pf, "registered", found);
    json_push_kv_int(&pf, "healthy_outbound", healthy);
    json_push_kv_int(&pf, "last_tick_age_us", last_tick_age_us);
    json_push_kv_int(&pf, "deadline_secs", deadline_secs);
    json_push_kv_int(&pf, "ticks_run", ticks_run);
    json_push_kv_int(&pf, "stall_fires", stall_fires);
    json_push_kv(out, "peer_floor", &pf);
    json_free(&pf);
}

static void push_chain_view_json(struct json_value *out,
                                 int64_t *our_height, int64_t *peer_modal,
                                 int64_t *peer_delta)
{
    struct json_value chain_view = {0};
    json_set_object(&chain_view);
    network_monitor_dump_state_json(&chain_view, NULL);

    const struct json_value *v;
    if ((v = json_get(&chain_view, "our_height")))
        *our_height = json_get_int(v);
    if ((v = json_get(&chain_view, "modal_height")))
        *peer_modal = json_get_int(v);
    if ((v = json_get(&chain_view, "delta_behind_best")))
        *peer_delta = json_get_int(v);

    json_push_kv(out, "chain_view", &chain_view);
    json_free(&chain_view);
}

static void push_census_json(struct json_value *out, int64_t *network_modal)
{
    struct json_value census = {0};
    json_set_object(&census);
    network_crawler_dump_state_json(&census, NULL);

    const struct json_value *v = json_get(&census, "modal_height");
    if (v)
        *network_modal = json_get_int(v);

    json_push_kv(out, "census", &census);
    json_free(&census);
}

static void push_tip_comparison_json(struct json_value *out,
                                     int64_t our_height, int64_t peer_modal,
                                     int64_t peer_delta,
                                     int64_t network_modal)
{
    struct json_value tc = {0};
    json_set_object(&tc);
    json_push_kv_int(&tc, "our_height", our_height);
    json_push_kv_int(&tc, "peer_modal_height", peer_modal);
    json_push_kv_int(&tc, "peer_modal_delta", peer_delta);
    json_push_kv_int(&tc, "network_modal_height", network_modal);
    json_push_kv_int(&tc, "network_modal_delta",
                     (network_modal >= 0 && our_height >= 0)
                         ? (network_modal - our_height)
                         : 0);
    json_push_kv(out, "tip_comparison", &tc);
    json_free(&tc);
}

bool network_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;
    json_set_object(out);

    struct connman *cm = rpc_net_get_connman();
    int64_t connman_outbound_healthy = -1;
    push_connman_json(out, cm, &connman_outbound_healthy);
    push_peer_floor_json(out, connman_outbound_healthy);

    int64_t our_height = -1, peer_modal = -1, peer_delta = 0;
    push_chain_view_json(out, &our_height, &peer_modal, &peer_delta);

    int64_t network_modal = -1;
    push_census_json(out, &network_modal);

    push_tip_comparison_json(out, our_height, peer_modal, peer_delta,
                             network_modal);

    struct json_value pl = {0};
    json_set_object(&pl);
    peer_lifecycle_dump_state_json(&pl, NULL);
    json_push_kv(out, "peer_lifecycle", &pl);
    json_free(&pl);

    return true;
}
