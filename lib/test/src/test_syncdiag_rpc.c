/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Regression test: `getsyncdiag` RPC crashes via `json_free` on uninitialized
 * stack memory.
 *
 * The bug: `rpc_getsyncdiag` in `app/controllers/src/health_controller.c`
 * declares `struct json_value wd;` (and `hdr`) without `json_init()` or
 * `= {0}`. `json_set_object(&wd)` internally calls `json_free(&wd)`,
 * which reads uninitialized `type`, `num_children`, and `children` —
 * typically crashing with SIGSEGV/SIGABRT once the stack region holds
 * non-zero residue from earlier RPCs (which is always the case on a
 * live node).
 *
 * This test dirties the lower stack with 0xCC before calling the RPC
 * to force the uninitialized read to observe garbage in a fresh test
 * process, making the repro deterministic. Post-fix (wd/hdr explicitly
 * zero-initialized), the RPC must return a well-formed JSON object
 * containing non-empty `watchdog` and `headers` sub-objects. */

#include "test/test_helpers.h"
#include "controllers/event_controller.h"
#include "controllers/health_controller.h"
#include "controllers/network_controller.h"
#include "framework/condition.h"
#include "services/block_source_policy.h"
#include "services/legacy_mirror_sync_service.h"
#include "services/sync_monitor.h"
#include "validation/mirror_consensus.h"
#include "event/event.h"
#include "net/connman.h"
#include "net/fast_sync.h"
#include "net/version.h"
#include "rpc/httpserver.h"
#include "rpc/server.h"
#include "json/json.h"
#include "util/alerts.h"
#include "util/clientversion.h"
#include "validation/main_state.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Push a 64 KiB frame filled with 0xCC onto the stack, then return.
 * The frame is freed on return but the bytes persist in memory — any
 * subsequent callee with a smaller combined frame size reuses that
 * region, observing 0xCC where `= {0}` would have given zeros. */
static __attribute__((noinline)) void dirty_stack_region(void)
{
    volatile unsigned char junk[65536];
    for (size_t i = 0; i < sizeof(junk); i++)
        junk[i] = 0xCC;
    /* Force the compiler to materialize the writes. */
    __asm__ volatile("" : : "r"(junk) : "memory");
}

static const struct json_value *find_service(const struct json_value *arr,
                                             const char *name)
{
    if (!arr || arr->type != JSON_ARR || !name)
        return NULL;
    for (size_t i = 0; i < json_size(arr); i++) {
        const struct json_value *svc = json_at(arr, i);
        const struct json_value *n = json_get(svc, "name");
        if (n && strcmp(json_get_str(n), name) == 0)
            return svc;
    }
    return NULL;
}

static const struct json_value *find_source_json(const struct json_value *arr,
                                                 const char *source)
{
    if (!arr || arr->type != JSON_ARR || !source)
        return NULL;
    for (size_t i = 0; i < json_size(arr); i++) {
        const struct json_value *child = json_at(arr, i);
        const struct json_value *name = json_get(child, "source");
        if (name && strcmp(json_get_str(name), source) == 0)
            return child;
    }
    return NULL;
}

static void syncdiag_set_ipv4(struct net_address *addr,
                              uint8_t a, uint8_t b,
                              uint8_t c, uint8_t d,
                              uint16_t port)
{
    net_address_init(addr);
    addr->svc.addr.ip[10] = 0xff;
    addr->svc.addr.ip[11] = 0xff;
    addr->svc.addr.ip[12] = a;
    addr->svc.addr.ip[13] = b;
    addr->svc.addr.ip[14] = c;
    addr->svc.addr.ip[15] = d;
    addr->svc.port = port;
}

static struct p2p_node *syncdiag_add_peer(struct connman *cm,
                                          uint8_t last_octet,
                                          bool inbound,
                                          enum peer_state state)
{
    struct net_address addr;
    if (!cm)
        return NULL;
    if (!cm->manager.nodes) {
        cm->manager.nodes = zcl_calloc(4, sizeof(*cm->manager.nodes),
                                       "syncdiag_net_nodes");
        cm->manager.nodes_cap = 4;
        if (!cm->manager.nodes)
            return NULL;
    }
    if (cm->manager.num_nodes >= cm->manager.nodes_cap)
        return NULL;
    syncdiag_set_ipv4(&addr, 198, 51, 100, last_octet, 8033);
    struct p2p_node *node = p2p_node_create(
        &cm->manager, ZCL_INVALID_SOCKET, &addr, "syncdiag-net", inbound);
    if (!node)
        return NULL;
    node->state = state;
    node->services = NODE_NETWORK | NODE_ZCL23;
    snprintf(node->sub_ver, sizeof(node->sub_ver),
             "%s", "/ZClassic23:0.1.0/");
    snprintf(node->clean_sub_ver, sizeof(node->clean_sub_ver),
             "%s", node->sub_ver);
    node->starting_height = 3117074;
    cm->manager.nodes[cm->manager.num_nodes++] = node;
    return node;
}

int test_syncdiag_rpc(void)
{
    int failures = 0;

    printf("rpc_getsyncdiag: returns valid JSON without abort "
           "(RED)... ");
    {
        dirty_stack_region();

        struct rpc_table tbl;
        rpc_table_init(&tbl);
        rpc_health_set_state(NULL, NULL, NULL, NULL);
        register_health_rpc_commands(&tbl);
        sync_monitor_init();
        sync_monitor_record_snapshot_resnapshot(
            100, 110, 4, 101, 111,
            "block_failed_mask_exhausted",
            "condition:tip_wedged_resnapshot");
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value result;
        json_init(&result);

        bool executed = rpc_table_execute(&tbl, "getsyncdiag",
                                          &params, &result);

        bool ok = executed && result.type == JSON_OBJ;

        const struct json_value *wd  = json_get(&result, "watchdog");
        const struct json_value *hdr = json_get(&result, "headers");
        ok = ok && wd  && wd->type  == JSON_OBJ && wd->num_children  > 0;
        ok = ok && hdr && hdr->type == JSON_OBJ && hdr->num_children > 0;
        ok = ok && json_get(wd, "last_recovery_reason") != NULL;
        ok = ok && json_get(wd, "last_recovery_local_height") != NULL;
        ok = ok && json_get(wd, "last_recovery_peer_height") != NULL;
        ok = ok && json_get(wd, "last_recovery_peer_count") != NULL;
        ok = ok && json_get(wd, "last_recovery_target_height") != NULL;
        ok = ok && json_get(wd, "last_recovery_manifest_height") != NULL;
        ok = ok && json_get(wd, "last_recovery_trigger") != NULL;
        ok = ok && json_get(wd, "recoveries_total") != NULL &&
            json_get_int(json_get(wd, "recoveries_total")) == 1;
        ok = ok && json_get(wd, "last_recovery") != NULL &&
            strcmp(json_get_str(json_get(wd, "last_recovery")),
                   "SNAPSHOT_RESNAPSHOT") == 0;
        ok = ok && strcmp(json_get_str(json_get(
            wd, "last_recovery_reason")),
            "condition:tip_wedged_resnapshot") == 0;
        ok = ok && json_get_int(json_get(
            wd, "last_recovery_local_height")) == 100;
        ok = ok && json_get_int(json_get(
            wd, "last_recovery_peer_height")) == 110;
        ok = ok && json_get_int(json_get(
            wd, "last_recovery_peer_count")) == 4;
        ok = ok && json_get_int(json_get(
            wd, "last_recovery_target_height")) == 101;
        ok = ok && json_get_int(json_get(
            wd, "last_recovery_manifest_height")) == 111;
        ok = ok && strcmp(json_get_str(json_get(
            wd, "last_recovery_trigger")),
            "block_failed_mask_exhausted") == 0;

        ok = ok && json_get(&result, "sync_state")         != NULL;
        ok = ok && json_get(&result, "chain_height")       != NULL;
        ok = ok && json_get(&result, "best_header_height") != NULL;

        json_free(&params);
        json_free(&result);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("getsyncwatchdog: exposes last recovery context "
           "(RED)... ");
    {
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_health_rpc_commands(&tbl);
        sync_monitor_init();
        sync_monitor_record_snapshot_resnapshot(
            120, 130, 2, 121, 131,
            "local_import_exhausted",
            "condition:tip_wedged_resnapshot");
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value result;
        json_init(&result);

        bool ok = rpc_table_execute(&tbl, "getsyncwatchdog",
                                    &params, &result);

        ok = ok && result.type == JSON_OBJ;
        ok = ok && json_get(&result, "enabled") != NULL;
        ok = ok && json_get(&result, "recoveries_total") != NULL &&
            json_get_int(json_get(&result, "recoveries_total")) == 1;
        ok = ok && json_get(&result, "last_recovery") != NULL &&
            strcmp(json_get_str(json_get(&result, "last_recovery")),
                   "SNAPSHOT_RESNAPSHOT") == 0;
        ok = ok && json_get(&result, "last_recovery_time") != NULL &&
            json_get_int(json_get(&result, "last_recovery_time")) > 0;
        ok = ok && json_get(&result, "last_recovery_reason") != NULL &&
            strcmp(json_get_str(json_get(
            &result, "last_recovery_reason")),
            "condition:tip_wedged_resnapshot") == 0;
        ok = ok && json_get(&result, "last_recovery_local_height") != NULL &&
            json_get_int(json_get(
            &result, "last_recovery_local_height")) == 120;
        ok = ok && json_get(&result, "last_recovery_peer_height") != NULL &&
            json_get_int(json_get(
            &result, "last_recovery_peer_height")) == 130;
        ok = ok && json_get(&result, "last_recovery_peer_count") != NULL &&
            json_get_int(json_get(
            &result, "last_recovery_peer_count")) == 2;
        ok = ok && json_get(
            &result, "last_recovery_target_height") != NULL &&
            json_get_int(json_get(
            &result, "last_recovery_target_height")) == 121;
        ok = ok && json_get(
            &result, "last_recovery_manifest_height") != NULL &&
            json_get_int(json_get(
            &result, "last_recovery_manifest_height")) == 131;
        ok = ok && json_get(&result, "last_recovery_trigger") != NULL &&
            strcmp(json_get_str(json_get(
            &result, "last_recovery_trigger")),
            "local_import_exhausted") == 0;

        json_free(&params);
        json_free(&result);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("rpc_http response envelope: dirty stack still builds JSON "
           "(RED)... ");
    {
        dirty_stack_region();

        struct json_value result;
        json_init(&result);
        json_set_object(&result);
        json_push_kv_str(&result, "watchdog", "ok");

        struct json_value id;
        json_init(&id);
        json_set_int(&id, 1);

        struct json_value response;
        bool ok = rpc_http_test_build_response_envelope(
            true, "getsyncdiag", &result, &id, &response);

        ok = ok && response.type == JSON_OBJ;
        ok = ok && json_get(&response, "result") != NULL;
        ok = ok && json_get(&response, "error") != NULL;
        ok = ok && json_get(&response, "id") != NULL;

        json_free(&result);
        json_free(&id);
        json_free(&response);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("getnetworkinfo: reports stable startup reachability schema "
           "(RED)... ");
    {
        struct rpc_table tbl;
        struct json_value params;
        struct json_value result;

        rpc_table_init(&tbl);
        register_net_rpc_commands(&tbl);
        rpc_net_set_connman(NULL);

        json_init(&params);
        json_set_array(&params);
        json_init(&result);
        bool ok = rpc_table_execute(&tbl, "getnetworkinfo",
                                    &params, &result);

        const struct json_value *inbound =
            json_get(&result, "inbound_connections");
        const struct json_value *outbound =
            json_get(&result, "outbound_connections");
        const struct json_value *handshaked =
            json_get(&result, "handshaked_connections");
        const struct json_value *inbound_hs =
            json_get(&result, "inbound_handshaked_connections");
        const struct json_value *outbound_hs =
            json_get(&result, "outbound_handshaked_connections");
        const struct json_value *listen_count =
            json_get(&result, "listen_socket_count");
        const struct json_value *listening =
            json_get(&result, "listening");
        const struct json_value *inbound_seen =
            json_get(&result, "inbound_handshake_seen");
        const struct json_value *remote_seen =
            json_get(&result, "remote_handshake_seen");
        const struct json_value *life =
            json_get(&result, "peer_lifecycle");
        const struct json_value *life_sources =
            life ? json_get(life, "sources") : NULL;
        const struct json_value *addnodes =
            json_get(&result, "addnode_status");

        ok = ok && result.type == JSON_OBJ;
        ok = ok && inbound && json_get_int(inbound) == 0;
        ok = ok && outbound && json_get_int(outbound) == 0;
        ok = ok && handshaked && json_get_int(handshaked) == 0;
        ok = ok && inbound_hs && json_get_int(inbound_hs) == 0;
        ok = ok && outbound_hs && json_get_int(outbound_hs) == 0;
        ok = ok && listen_count && json_get_int(listen_count) == 0;
        ok = ok && listening && !json_get_bool(listening);
        ok = ok && inbound_seen && !json_get_bool(inbound_seen);
        ok = ok && remote_seen && !json_get_bool(remote_seen);
        ok = ok && life && life->type == JSON_OBJ;
        ok = ok && life && json_get(life, "attempted") != NULL;
        ok = ok && life && json_get(life, "connected") != NULL;
        ok = ok && life && json_get(life, "version_sent") != NULL;
        ok = ok && life && json_get(life, "version_received") != NULL;
        ok = ok && life && json_get(life, "verack_received") != NULL;
        ok = ok && life && json_get(life, "handshake_complete") != NULL;
        ok = ok && life && json_get(life, "active") != NULL;
        ok = ok && life && json_get(life, "disconnected") != NULL;
        ok = ok && life && json_get(life, "timeout") != NULL;
        ok = ok && life && json_get(life, "rejected") != NULL;
        ok = ok && life && json_get(life, "cache_skipped") != NULL;
        ok = ok && life && json_get(life, "magicbean_handshakes") != NULL;
        ok = ok && life && json_get(life, "zclassic23_handshakes") != NULL;
        ok = ok && life && json_get(life, "zclassic_c23_handshakes") != NULL;
        ok = ok && life_sources && life_sources->type == JSON_ARR;
        ok = ok && addnodes && addnodes->type == JSON_ARR;
        ok = ok && json_size(addnodes) == 0;
        ok = ok && find_source_json(life_sources, "unknown") != NULL;
        ok = ok && find_source_json(life_sources, "inbound") != NULL;
        ok = ok && find_source_json(life_sources, "addnode") != NULL;
        ok = ok && find_source_json(life_sources, "addrman") != NULL;
        ok = ok && find_source_json(life_sources, "zcl23_db") != NULL;
        ok = ok && find_source_json(life_sources, "manual") != NULL;

        json_free(&params);
        json_free(&result);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("getnetworkinfo: exposes configured external endpoint "
           "(RED)... ");
    {
        struct rpc_table tbl;
        struct json_value params;
        struct json_value result;

        msg_version_clear_external_ip_for_test();
        msg_version_set_external_ip("203.0.113.7:8023", 8033);
        rpc_table_init(&tbl);
        register_net_rpc_commands(&tbl);
        rpc_net_set_connman(NULL);

        json_init(&params);
        json_set_array(&params);
        json_init(&result);
        bool ok = rpc_table_execute(&tbl, "getnetworkinfo",
                                    &params, &result);

        const struct json_value *localaddrs =
            json_get(&result, "localaddresses");
        const struct json_value *first =
            localaddrs && localaddrs->type == JSON_ARR
                ? json_at(localaddrs, 0)
                : NULL;
        ok = ok && result.type == JSON_OBJ;
        ok = ok && json_get_bool(json_get(&result,
                                          "externalip_configured"));
        ok = ok && localaddrs && localaddrs->type == JSON_ARR;
        ok = ok && json_size(localaddrs) == 1;
        ok = ok && first && strcmp(json_get_str(json_get(first, "address")),
                                   "203.0.113.7") == 0;
        ok = ok && first &&
             json_get_int(json_get(first, "port")) == 8023;
        ok = ok && first &&
             json_get_int(json_get(first, "score")) == 1;
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "advertised_subver")),
                          msg_version_user_agent()) == 0;

        json_free(&params);
        json_free(&result);
        rpc_net_set_connman(NULL);
        msg_version_clear_external_ip_for_test();

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("getnetworkinfo: separates inbound reachability from outbound "
           "handshakes (RED)... ");
    {
        struct connman cm;
        struct node_signals sigs;
        struct rpc_table tbl;
        struct json_value params;
        struct json_value result;

        chain_params_select(CHAIN_MAIN);
        memset(&cm, 0, sizeof(cm));
        memset(&sigs, 0, sizeof(sigs));
        bool ok = connman_init(&cm, chain_params_get(), &sigs);
        ok = ok && syncdiag_add_peer(&cm, 11, false,
                                     PEER_HANDSHAKE_COMPLETE) != NULL;
        ok = ok && syncdiag_add_peer(&cm, 12, true,
                                     PEER_HANDSHAKE_COMPLETE) != NULL;
        if (ok) {
            struct net_address addr;
            net_address_init(&addr);
            memset(addr.svc.addr.ip, 0, 10);
            addr.svc.addr.ip[10] = 0xff;
            addr.svc.addr.ip[11] = 0xff;
            addr.svc.addr.ip[12] = 51;
            addr.svc.addr.ip[13] = 178;
            addr.svc.addr.ip[14] = 179;
            addr.svc.addr.ip[15] = 75;
            addr.svc.port = 8033;
            cm.addnodes[cm.num_addnodes++] = addr;
            connman_record_addnode_failure(&cm, 0,
                                           CONNMAN_ADDNODE_FAILURE_TCP);
        }

        rpc_table_init(&tbl);
        register_net_rpc_commands(&tbl);
        rpc_net_set_connman(&cm);

        json_init(&params);
        json_set_array(&params);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "getnetworkinfo",
                                     &params, &result);

        ok = ok && result.type == JSON_OBJ;
        ok = ok && json_get_int(json_get(&result, "handshaked_connections"))
                  == 2;
        ok = ok && json_get_int(json_get(&result,
                                          "inbound_handshaked_connections"))
                  == 1;
        ok = ok && json_get_int(json_get(&result,
                                          "outbound_handshaked_connections"))
                  == 1;
        ok = ok && json_get_bool(json_get(&result,
                                          "inbound_handshake_seen"));
        ok = ok && json_get_bool(json_get(&result,
                                          "remote_handshake_seen"));
        ok = ok && json_get_int(json_get(&result,
                                          "legacy_compatible_peers")) ==
                  json_get_int(json_get(&result, "magicbean_peers"));
        ok = ok && json_get_int(json_get(&result,
                                          "legacy_magicbean_peers")) ==
                  json_get_int(json_get(&result, "magicbean_peers"));
        ok = ok && json_get_int(json_get(&result, "zclassic23_peers")) ==
                  json_get_int(json_get(&result, "zclassic_c23_peers"));
        const struct json_value *addnodes =
            json_get(&result, "addnode_status");
        const struct json_value *first =
            addnodes && addnodes->type == JSON_ARR ? json_at(addnodes, 0)
                                                   : NULL;
        ok = ok && addnodes && addnodes->type == JSON_ARR;
        ok = ok && json_size(addnodes) == 1;
        ok = ok && first && json_get(first, "address") != NULL;
        ok = ok && first && json_get_int(json_get(first, "index")) == 0;
        ok = ok && first && !json_get_bool(json_get(first, "connected"));
        ok = ok && first &&
             json_get_int(json_get(first, "backoff_seconds")) > 0;
        ok = ok && first &&
             json_get_int(json_get(first, "backoff_remaining_seconds")) >= 0;
        ok = ok && first &&
             json_get_int(json_get(first, "tcp_failures")) == 1;
        ok = ok && first &&
             json_get_int(json_get(first, "protocol_failures")) == 0;

        json_free(&result);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "getpeerinfo",
                                     &params, &result);
        const struct json_value *peer0 =
            result.type == JSON_ARR ? json_at(&result, 0) : NULL;
        ok = ok && peer0 && json_get_bool(json_get(peer0, "zclassic23"));
        ok = ok && peer0 && json_get_bool(json_get(peer0, "zclassic_c23"));

        json_free(&params);
        json_free(&result);
        rpc_net_set_connman(NULL);
        connman_free(&cm);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("getservicehealth: exposes chain advance coordinator "
           "(RED)... ");
    {
        struct json_value result;
        json_init(&result);
        block_source_policy_reset_for_test();
        condition_engine_reset_for_testing();
        mirror_consensus_reset_for_test();
        mirror_consensus_set_enabled(true);
        mirror_consensus_record_blocker("body-hash-mismatch");
        bool seeded = block_source_policy_snapshot_offer_allowed(
            100, 10000, 10100, true, "manifest_ok", NULL);
        bool ok = seeded && api_getservicehealth(&result);
        const struct json_value *svc =
            find_service(&result, "chain_advance_coordinator");
        ok = ok && result.type == JSON_ARR && svc != NULL;
        ok = ok && json_get(svc, "authority") != NULL;
        ok = ok && json_get(svc, "decision") != NULL;
        ok = ok && json_get(svc, "selected_source") != NULL;
        ok = ok && json_get(svc, "selected_source_trust") != NULL;
        ok = ok && json_get(svc, "mirror_fallback_allowed") != NULL;
        ok = ok && json_get(svc, "activation_allowed") != NULL;
        ok = ok && json_get(svc, "best_header_height") != NULL;
        ok = ok && json_get(svc, "projection_height") != NULL;
        ok = ok && json_get(svc, "projection_lag") != NULL;
        ok = ok && json_get(svc, "projection_deferred") != NULL;
        ok = ok && json_get(svc, "projection_state") != NULL;
        ok = ok && json_get(svc, "projection_deferred_total") != NULL;
        ok = ok && json_get(svc, "last_projection_deferred_height") != NULL;
        ok = ok && json_get(svc, "last_projection_deferred_time") != NULL;
        ok = ok && json_get(svc, "last_projection_deferred_reason") != NULL;
        ok = ok && json_get(svc, "reason") != NULL;
        ok = ok && json_get(svc, "initialized") != NULL;
        ok = ok && json_get(svc, "has_connman") != NULL;
        ok = ok && json_get(svc, "has_main_state") != NULL;
        ok = ok && json_get(svc, "has_node_db") != NULL;
        const struct json_value *sources =
            svc ? json_get(svc, "sources") : NULL;
        const struct json_value *current_snapshot =
            find_source_json(sources, "snapshot");
        ok = ok && sources && sources->type == JSON_ARR;
        ok = ok && current_snapshot && current_snapshot->type == JSON_OBJ;
        ok = ok && json_get(current_snapshot, "selectable") != NULL;
        ok = ok && json_get(current_snapshot, "selection_blocker") != NULL;
        ok = ok && json_get(current_snapshot,
                            "score_target_lag_penalty") != NULL;
        ok = ok && json_get(current_snapshot,
                            "score_failure_penalty") != NULL;
        const struct json_value *has_last =
            svc ? json_get(svc, "has_last_decision") : NULL;
        const struct json_value *last =
            svc ? json_get(svc, "last_decision") : NULL;
        ok = ok && has_last && json_get_bool(has_last);
        ok = ok && last && last->type == JSON_OBJ;
        ok = ok && json_get(last, "op") != NULL &&
            strcmp(json_get_str(json_get(last, "op")),
                   "snapshot_offer") == 0;
        ok = ok && json_get(last, "selected_source_reason") != NULL &&
            strcmp(json_get_str(json_get(last, "selected_source_reason")),
                   "manifest_ok") == 0;
        ok = ok && json_get(last, "selected_source_selectable") != NULL;
        ok = ok && json_get_bool(json_get(
            last, "selected_source_selectable"));
        ok = ok && json_get(last,
                            "selected_source_selection_blocker") != NULL;
        ok = ok && strcmp(json_get_str(json_get(
            last, "selected_source_selection_blocker")), "") == 0;
        ok = ok && json_get(last, "selected_source_score_base") != NULL;
        ok = ok && json_get_int(json_get(
            last, "selected_source_score_base")) == 85;
        ok = ok && json_get(last,
                            "selected_source_score_target_lag_penalty")
                 != NULL;
        ok = ok && json_get_int(json_get(
            last, "selected_source_score_target_lag_penalty")) == 25;
        ok = ok && json_get(last,
                            "selected_source_score_failure_penalty") != NULL;
        ok = ok && json_get_int(json_get(
            last, "selected_source_score_failure_penalty")) == 0;
        const struct json_value *last_sources = json_get(last, "sources");
        const struct json_value *snapshot =
            find_source_json(last_sources, "snapshot");
        ok = ok && last_sources && last_sources->type == JSON_ARR;
        ok = ok && snapshot && snapshot->type == JSON_OBJ;
        ok = ok && json_get(snapshot, "trust") != NULL &&
            strcmp(json_get_str(json_get(snapshot, "trust")),
                   "native_snapshot_proof_validated") == 0;
        ok = ok && json_get(snapshot, "reason") != NULL &&
            strcmp(json_get_str(json_get(snapshot, "reason")),
                   "manifest_ok") == 0;

        json_free(&result);
        block_source_policy_reset_for_test();

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("getservicehealth: exposes canonical mirror trust "
           "(RED)... ");
    {
        struct legacy_mirror_sync_stats stats;
        struct json_value result;

        sync_monitor_init();
        legacy_mirror_sync_reset_for_test();
        mirror_consensus_reset_for_test();
        mirror_consensus_set_enabled(true);
        mirror_consensus_record_override(200, "body-hash-mismatch");
        mirror_consensus_record_blocker("body-hash-mismatch");

        memset(&stats, 0, sizeof(stats));
        stats.enabled = true;
        stats.running = true;
        stats.reachable = true;
        stats.legacy_height = 200;
        stats.local_height = 199;
        stats.target_height = 200;
        stats.stalls_total = 3;
        snprintf(stats.last_blocker_id, sizeof(stats.last_blocker_id),
                 "%s", "body-hash-mismatch");
        legacy_mirror_sync_test_set_stats(&stats, NULL);

        json_init(&result);
        bool ok = api_getservicehealth(&result);
        const struct json_value *svc = find_service(&result, "legacy_mirror");
        ok = ok && result.type == JSON_ARR && svc != NULL;
        ok = ok && json_get(svc, "state") != NULL &&
            strcmp(json_get_str(json_get(svc, "state")), "blocked") == 0;
        ok = ok && json_get(svc, "consensus_authority") != NULL &&
            strcmp(json_get_str(json_get(svc, "consensus_authority")),
                   "local_consensus_validation") == 0;
        ok = ok && json_get(svc, "candidate_source") != NULL &&
            strcmp(json_get_str(json_get(svc, "candidate_source")),
                   "legacy_advisory") == 0;
        ok = ok && json_get(svc, "mirror_authorization_enabled") == NULL;
        ok = ok && json_get(svc, "mirror_source_trust") == NULL;
        ok = ok && json_get(svc, "candidate_trust") != NULL &&
            strcmp(json_get_str(json_get(svc, "candidate_trust")),
                   "bounded_advisory_fallback") == 0;
        ok = ok && json_get(svc, "candidate_lag_observed") != NULL &&
            json_is_null(json_get(svc, "candidate_lag_observed"));
        ok = ok && json_get(svc, "candidate_lag") != NULL &&
            json_get_int(json_get(svc, "candidate_lag")) == 0;
        ok = ok && json_get(svc, "mirror_monitor_running") != NULL &&
            json_get_bool(json_get(svc, "mirror_monitor_running"));
        ok = ok && json_get(svc, "zclassicd_rpc_transport_reachable") != NULL &&
            json_get_bool(json_get(svc, "zclassicd_rpc_transport_reachable"));
        ok = ok && json_get(svc, "legacy_oracle_usable") != NULL &&
            json_get_bool(json_get(svc, "legacy_oracle_usable"));
        ok = ok && json_get(svc, "zclassicd_rpc_error_code") != NULL &&
            json_get_int(json_get(svc, "zclassicd_rpc_error_code")) == 0;
        ok = ok && json_get(svc, "zclassicd_rpc_error_message") != NULL &&
            strcmp(json_get_str(json_get(svc,
                                         "zclassicd_rpc_error_message")),
                   "") == 0;
        ok = ok && json_get(svc, "candidate_blocker") != NULL &&
            strcmp(json_get_str(json_get(svc, "candidate_blocker")),
                   "body-hash-mismatch") == 0;
        ok = ok && json_get(svc, "candidate_blocker_scope") != NULL &&
            strcmp(json_get_str(json_get(svc, "candidate_blocker_scope")),
                   "advisory_source") == 0;
        ok = ok && json_get(svc, "activation_blocker") != NULL &&
            strcmp(json_get_str(json_get(svc, "activation_blocker")),
                   "body-hash-mismatch") == 0;
        ok = ok && json_get(svc, "last_blocker_code") != NULL &&
            strcmp(json_get_str(json_get(svc, "last_blocker_code")),
                   "body-hash-mismatch") == 0;
        ok = ok && json_get(svc,
                             "legacy_advisory_gated_by_native_retries") != NULL;
        ok = ok && json_get(svc,
                             "mirror_repair_gated_by_local_retries") != NULL;
        ok = ok && json_get(svc, "local_retries_exhausted") != NULL;
        ok = ok && json_get(svc, "overrides_total") != NULL;
        ok = ok && json_get(svc, "unsafe_overrides_total") != NULL &&
            json_get_int(json_get(svc, "unsafe_overrides_total")) == 1;
        ok = ok && json_get(svc, "last_override_safe") != NULL &&
            !json_get_bool(json_get(svc, "last_override_safe"));
        ok = ok && json_get(svc, "last_override_reason") != NULL &&
            strcmp(json_get_str(json_get(svc, "last_override_reason")),
                   "body-hash-mismatch") == 0;
        ok = ok && json_get(svc, "last_override_scope") != NULL &&
            strcmp(json_get_str(json_get(svc, "last_override_scope")),
                   "unsafe_no_authorized_scope") == 0;
        ok = ok && json_get(svc, "blockers_total") != NULL &&
            json_get_int(json_get(svc, "blockers_total")) == 1;
        ok = ok && json_get(svc, "stalls_total") != NULL &&
            json_get_int(json_get(svc, "stalls_total")) == 3;
        ok = ok && json_get(svc, "lag_observed") != NULL &&
            json_is_null(json_get(svc, "lag_observed"));
        ok = ok && json_get(svc, "lag") != NULL &&
            json_get_int(json_get(svc, "lag")) == 0;

        json_free(&result);
        legacy_mirror_sync_reset_for_test();
        mirror_consensus_reset_for_test();

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("healthcheck: exposes chain advance decision "
           "(RED)... ");
    {
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value result;
        json_init(&result);

        block_source_policy_reset_for_test();
        legacy_mirror_sync_reset_for_test();
        mirror_consensus_reset_for_test();
        struct legacy_mirror_sync_stats stats;
        memset(&stats, 0, sizeof(stats));
        stats.enabled = true;
        stats.running = true;
        stats.reachable = false;
        stats.legacy_height = 0;
        stats.local_height = 3157703;
        snprintf(stats.last_blocker_id, sizeof(stats.last_blocker_id),
                 "%s", "rpc-unreachable");
        snprintf(stats.last_error, sizeof(stats.last_error),
                 "%s", "connect failed");
        legacy_mirror_sync_test_set_stats(&stats, NULL);
        mirror_consensus_set_enabled(true);
        mirror_consensus_record_override(100, "body-hash-mismatch");
        mirror_consensus_record_blocker("body-hash-mismatch");
        bool seeded = block_source_policy_snapshot_offer_allowed(
            100, 10000, 10100, true, "manifest_ok", NULL);
        bool executed = rpc_table_execute(&tbl, "healthcheck",
                                          &params, &result);

        const struct json_value *checks = json_get(&result, "checks");
        const struct json_value *ca =
            checks ? json_get(checks, "chain_advance") : NULL;
        const struct json_value *ce =
            checks ? json_get(checks, "chain_evidence") : NULL;
        const struct json_value *condition_engine =
            checks ? json_get(checks, "condition_engine") : NULL;
        bool ok = seeded && executed && result.type == JSON_OBJ;
        ok = ok && json_get(&result, "build_commit") != NULL &&
            strcmp(json_get_str(json_get(&result, "build_commit")),
                   zcl_build_commit()) == 0;
        ok = ok && checks && checks->type == JSON_OBJ;
        ok = ok && json_get(checks, "error_total") != NULL;
        ok = ok && json_get(checks, "last_error_age_seconds") != NULL;
        ok = ok && json_get(checks, "last_error_recent") != NULL;
        ok = ok && json_get(&result, "candidate_lag_known") != NULL &&
            !json_get_bool(json_get(&result, "candidate_lag_known"));
        ok = ok && json_get(&result, "candidate_lag_valid") != NULL &&
            !json_get_bool(json_get(&result, "candidate_lag_valid"));
        ok = ok && json_get(&result, "candidate_lag") != NULL &&
            json_get_int(json_get(&result, "candidate_lag")) == 0;
        ok = ok && json_get(&result, "candidate_lag_observed") != NULL &&
            json_is_null(json_get(&result, "candidate_lag_observed"));
        ok = ok && json_get(&result, "mirror_lag") != NULL &&
            json_get_int(json_get(&result, "mirror_lag")) == 0;
        ok = ok && json_get(&result, "mirror_lag_observed") != NULL &&
            json_is_null(json_get(&result, "mirror_lag_observed"));
        ok = ok && json_get(&result, "mirror_monitor_running") != NULL &&
            json_get_bool(json_get(&result, "mirror_monitor_running"));
        ok = ok && json_get(&result,
                            "zclassicd_rpc_transport_reachable") != NULL &&
            !json_get_bool(json_get(&result,
                                    "zclassicd_rpc_transport_reachable"));
        ok = ok && json_get(&result, "legacy_oracle_usable") != NULL &&
            !json_get_bool(json_get(&result, "legacy_oracle_usable"));
        ok = ok && json_get(&result, "zclassicd_rpc_error_code") != NULL &&
            json_get_int(json_get(&result, "zclassicd_rpc_error_code")) == 0;
        ok = ok && json_get(&result,
                            "zclassicd_rpc_error_message") != NULL &&
            strcmp(json_get_str(json_get(
                       &result, "zclassicd_rpc_error_message")),
                   "connect failed") == 0;
        ok = ok && json_get(&result, "mirror_rpc_errors") != NULL &&
            json_get_int(json_get(&result, "mirror_rpc_errors")) == 0;
        ok = ok && json_get(&result, "mirror_active_error_code") != NULL &&
            strcmp(json_get_str(json_get(&result,
                                         "mirror_active_error_code")),
                   "body-hash-mismatch") == 0;
        ok = ok && json_get(&result, "mirror_active_error_detail") != NULL &&
            strcmp(json_get_str(json_get(&result,
                                         "mirror_active_error_detail")),
                   "connect failed") == 0;
        ok = ok && json_get(&result, "candidate_blocker") != NULL &&
            strcmp(json_get_str(json_get(&result, "candidate_blocker")),
                   "body-hash-mismatch") == 0;
        ok = ok && json_get(&result, "candidate_blocker_scope") != NULL &&
            strcmp(json_get_str(json_get(
                       &result, "candidate_blocker_scope")),
                   "active_or_safety") == 0;
        ok = ok && json_get(&result, "legacy_advisory_blocker") != NULL &&
            strcmp(json_get_str(json_get(
                       &result, "legacy_advisory_blocker")),
                   "body-hash-mismatch") == 0;
        ok = ok && json_get(&result, "mirror_blockers_total") != NULL &&
            json_get_int(json_get(&result, "mirror_blockers_total")) == 1;
        ok = ok && json_get(&result, "mirror_stalls_total") != NULL &&
            json_get_int(json_get(&result, "mirror_stalls_total")) == 0;
        ok = ok && json_get(&result,
                            "mirror_unsafe_overrides_total") != NULL &&
            json_get_int(json_get(&result,
                                  "mirror_unsafe_overrides_total")) == 1;
        ok = ok && json_get(&result, "mirror_last_override_safe") != NULL &&
            !json_get_bool(json_get(&result, "mirror_last_override_safe"));
        ok = ok && json_get(&result, "mirror_last_override_reason") != NULL &&
            strcmp(json_get_str(json_get(&result,
                                         "mirror_last_override_reason")),
                   "body-hash-mismatch") == 0;
        ok = ok && json_get(&result, "mirror_last_override_scope") != NULL &&
            strcmp(json_get_str(json_get(&result,
                                         "mirror_last_override_scope")),
                   "unsafe_no_authorized_scope") == 0;
        ok = ok && json_get(&result, "mirror_activation_blocker") != NULL &&
            strcmp(json_get_str(json_get(&result,
                                         "mirror_activation_blocker")),
                   "body-hash-mismatch") == 0;
        ok = ok && condition_engine && condition_engine->type == JSON_OBJ;
        ok = ok && json_get(condition_engine, "registered_count") != NULL;
        ok = ok && json_get(condition_engine, "active_count") != NULL;
        ok = ok && json_get(condition_engine, "unresolved_count") != NULL;
        ok = ok && json_get(condition_engine, "conditions") != NULL;
        ok = ok && ce && ce->type == JSON_OBJ;
        ok = ok && json_get(ce, "state") != NULL;
        ok = ok && json_get(ce, "publish_state") != NULL;
        ok = ok && json_get(ce, "active_tip_source_class") != NULL;
        ok = ok && json_get(ce, "active_tip") != NULL;
        ok = ok && json_get(ce, "header_tip") != NULL;
        ok = ok && json_get(ce, "persisted_active_tip") != NULL;
        ok = ok && json_get(ce, "utxo_max_height") != NULL;
        ok = ok && json_get(ce, "coins_best_block_height") != NULL;
        ok = ok && json_get(ce, "csr_sqlite_max_height") != NULL;
        ok = ok && json_get(ce, "missing_active_tip_evidence") != NULL;
        ok = ok && json_get(ce, "publish_state_not_local") != NULL;
        ok = ok && json_get(ce, "active_tip_hash_mismatch") != NULL;
        ok = ok && json_get(ce, "csr_cursor_mismatch") != NULL;
        ok = ok && ca && ca->type == JSON_OBJ;
        ok = ok && json_get(ca, "authority") != NULL;
        ok = ok && json_get(ca, "decision") != NULL;
        ok = ok && json_get(ca, "selected_source") != NULL;
        ok = ok && json_get(ca, "selected_source_trust") != NULL;
        ok = ok && json_get(ca, "mirror_fallback_allowed") != NULL;
        ok = ok && json_get(ca, "activation_allowed") != NULL;
        ok = ok && json_get(ca, "best_header_height") != NULL;
        ok = ok && json_get(ca, "projection_height") != NULL;
        ok = ok && json_get(ca, "projection_lag") != NULL;
        ok = ok && json_get(ca, "projection_deferred") != NULL;
        ok = ok && json_get(ca, "projection_state") != NULL;
        ok = ok && json_get(ca, "projection_deferred_total") != NULL;
        ok = ok && json_get(ca, "last_projection_deferred_height") != NULL;
        ok = ok && json_get(ca, "last_projection_deferred_time") != NULL;
        ok = ok && json_get(ca, "last_projection_deferred_reason") != NULL;
        ok = ok && json_get(ca, "reason") != NULL;
        ok = ok && json_get(ca, "initialized") != NULL;
        ok = ok && json_get(ca, "has_connman") != NULL;
        ok = ok && json_get(ca, "has_main_state") != NULL;
        ok = ok && json_get(ca, "has_node_db") != NULL;
        const struct json_value *sources =
            ca ? json_get(ca, "sources") : NULL;
        const struct json_value *current_snapshot =
            find_source_json(sources, "snapshot");
        ok = ok && sources && sources->type == JSON_ARR;
        ok = ok && current_snapshot && current_snapshot->type == JSON_OBJ;
        ok = ok && json_get(current_snapshot, "selectable") != NULL;
        ok = ok && json_get(current_snapshot, "selection_blocker") != NULL;
        ok = ok && json_get(current_snapshot,
                            "score_target_lag_penalty") != NULL;
        ok = ok && json_get(current_snapshot,
                            "score_failure_penalty") != NULL;
        const struct json_value *has_last = json_get(ca, "has_last_decision");
        const struct json_value *last = json_get(ca, "last_decision");
        ok = ok && has_last && json_get_bool(has_last);
        ok = ok && last && last->type == JSON_OBJ;
        ok = ok && json_get(last, "op") != NULL &&
            strcmp(json_get_str(json_get(last, "op")),
                   "snapshot_offer") == 0;
        ok = ok && json_get(last, "selected_source_reason") != NULL &&
            strcmp(json_get_str(json_get(last, "selected_source_reason")),
                   "manifest_ok") == 0;
        ok = ok && json_get(last, "selected_source_selectable") != NULL;
        ok = ok && json_get_bool(json_get(
            last, "selected_source_selectable"));
        ok = ok && json_get(last,
                            "selected_source_selection_blocker") != NULL;
        ok = ok && strcmp(json_get_str(json_get(
            last, "selected_source_selection_blocker")), "") == 0;
        ok = ok && json_get(last, "selected_source_score_base") != NULL;
        ok = ok && json_get_int(json_get(
            last, "selected_source_score_base")) == 85;
        ok = ok && json_get(last,
                            "selected_source_score_target_lag_penalty")
                 != NULL;
        ok = ok && json_get_int(json_get(
            last, "selected_source_score_target_lag_penalty")) == 25;
        ok = ok && json_get(last,
                            "selected_source_score_failure_penalty") != NULL;
        ok = ok && json_get_int(json_get(
            last, "selected_source_score_failure_penalty")) == 0;
        const struct json_value *last_sources = json_get(last, "sources");
        const struct json_value *snapshot =
            find_source_json(last_sources, "snapshot");
        ok = ok && last_sources && last_sources->type == JSON_ARR;
        ok = ok && snapshot && snapshot->type == JSON_OBJ;
        ok = ok && json_get(snapshot, "trust") != NULL &&
            strcmp(json_get_str(json_get(snapshot, "trust")),
                   "native_snapshot_proof_validated") == 0;
        ok = ok && json_get(snapshot, "reason") != NULL &&
            strcmp(json_get_str(json_get(snapshot, "reason")),
                   "manifest_ok") == 0;

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
        json_free(&params);
        json_free(&result);
        block_source_policy_reset_for_test();
        legacy_mirror_sync_reset_for_test();
        mirror_consensus_reset_for_test();
    }

    printf("api: native RPC returns versioned discovery document... ");
    {
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value result;
        json_init(&result);

        bool executed = rpc_table_execute(&tbl, "api", &params, &result);
        const struct json_value *resources = json_get(&result, "resources");
        const struct json_value *mcp = json_get(&result, "mcp");
        const struct json_value *cli = json_get(&result, "cli");
        bool ok = executed && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "schema")),
                          "zcl.rest_index.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "api_version")),
                          "v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "base_path")),
                          "/api/v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "compat_base_path")),
                          "/api") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "first_call")),
                          "/api/v1/agent") == 0;
        ok = ok && resources && resources->type == JSON_ARR &&
            json_size(resources) >= 4;
        ok = ok && mcp && mcp->type == JSON_OBJ &&
            strcmp(json_get_str(json_get(mcp, "first_tool")),
                   "zcl_agent") == 0;
        ok = ok && strcmp(json_get_str(json_get(mcp, "milestone_tool")),
                          "zcl_milestone") == 0;
        ok = ok && strcmp(json_get_str(json_get(mcp, "refold_tool")),
                          "zcl_refold_status") == 0;
        ok = ok && cli && cli->type == JSON_OBJ &&
            strcmp(json_get_str(json_get(cli, "api_command")),
                   "zclassic23 api") == 0;
        ok = ok && strcmp(json_get_str(json_get(cli, "first_command")),
                          "zclassic23 agent") == 0;
        ok = ok && strcmp(json_get_str(json_get(cli, "milestone_command")),
                          "zclassic23 milestone") == 0;
        ok = ok && strcmp(json_get_str(json_get(cli, "refold_command")),
                          "zclassic23 refold") == 0;

        struct json_value alias;
        json_init(&alias);
        bool alias_executed = rpc_table_execute(&tbl, "apiindex",
                                                &params, &alias);
        ok = ok && alias_executed && alias.type == JSON_OBJ &&
            strcmp(json_get_str(json_get(&alias, "schema")),
                   "zcl.rest_index.v1") == 0;

        json_free(&alias);
        json_free(&params);
        json_free(&result);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("api: native RPC agent names health blocking reason... ");
    {
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        alerts_shutdown();
        unsetenv("ZCL_ALERTS_DISABLE");
        unsetenv("ZCL_ALERT_WEBHOOK_URL");
        alerts_init();
        alerts_reset();
        event_emitf(EV_OPERATOR_NEEDED, 0, "chain_integrity_failed");

        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value result;
        json_init(&result);

        bool executed = rpc_table_execute(&tbl, "agent", &params, &result);
        bool ok = executed && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "schema")),
                          "zcl.public_status.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "status")),
                          "blocked") == 0;
        ok = ok && !json_get_bool(json_get(&result, "healthy"));
        ok = ok && !json_get_bool(json_get(&result, "serving"));
        ok = ok && json_get_bool(json_get(&result, "operator_needed"));
        ok = ok && strcmp(json_get_str(json_get(&result,
                                                "primary_blocker")),
                          "operator_needed:chain_integrity_failed") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "summary")),
                          "node has an active health blocker") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "next")),
                          "zclassic23 healthcheck") == 0;

        json_free(&params);
        json_free(&result);
        alerts_shutdown();

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("api: native RPC returns milestone ASCII bars... ");
    {
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value result;
        json_init(&result);

        bool executed = rpc_table_execute(&tbl, "milestone",
                                          &params, &result);
        const struct json_value *ascii = json_get(&result, "ascii");
        const struct json_value *bars = json_get(&result, "bars");
        const struct json_value *criteria = json_get(&result, "criteria");
        bool ok = executed && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "schema")),
                          "zcl.milestone_status.v1") == 0;
        ok = ok && json_get_int(json_get(&result,
                          "mvp_readiness_score")) == 4;
        ok = ok && ascii && strstr(json_get_str(json_get(ascii, "goals")),
                                   "goals [#####-----] 4/8") != NULL;
        ok = ok && bars && strcmp(json_get_str(json_get(json_get(bars,
                          "subgoals"), "bar")), "[########--]") == 0;
        ok = ok && criteria && json_size(criteria) == 8;

        struct json_value alias;
        json_init(&alias);
        bool alias_executed = rpc_table_execute(&tbl, "mvpstatus",
                                                &params, &alias);
        ok = ok && alias_executed && alias.type == JSON_OBJ &&
            strcmp(json_get_str(json_get(&alias, "schema")),
                   "zcl.milestone_status.v1") == 0;

        json_free(&alias);
        json_free(&params);
        json_free(&result);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("api: native RPC returns refold anchor readiness... ");
    {
        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();

        struct json_value params;
        json_init(&params);
        json_set_array(&params);

        struct json_value result;
        json_init(&result);

        bool executed = rpc_table_execute(&tbl, "refold",
                                          &params, &result);
        const struct json_value *snap = json_get(&result, "anchor_snapshot");
        const struct json_value *commands = json_get(&result, "commands");
        bool ok = executed && result.type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(&result, "schema")),
                          "zcl.refold_status.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&result, "api_version")),
                          "v1") == 0;
        ok = ok && !json_get_bool(json_get(&result, "ready_for_refold"));
        ok = ok && snap && json_get(snap, "verification") != NULL;
        ok = ok && commands &&
            strcmp(json_get_str(json_get(commands, "native")),
                   "zclassic23 refold") == 0;

        struct json_value alias;
        json_init(&alias);
        bool alias_executed = rpc_table_execute(&tbl, "refoldstatus",
                                                &params, &alias);
        ok = ok && alias_executed && alias.type == JSON_OBJ &&
            strcmp(json_get_str(json_get(&alias, "schema")),
                   "zcl.refold_status.v1") == 0;

        json_free(&alias);
        json_free(&params);
        json_free(&result);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("healthcheck: scopes zclassicd warmup as advisory when P2P "
           "is active (RED)... ");
    {
        struct connman cm;
        struct node_signals sigs;
        struct main_state ms;
        struct block_index tip;
        struct block_index best_header;
        struct rpc_table tbl;
        struct json_value params;
        struct json_value result;
        struct legacy_mirror_sync_stats stats;

        chain_params_select(CHAIN_MAIN);
        memset(&cm, 0, sizeof(cm));
        memset(&sigs, 0, sizeof(sigs));
        memset(&ms, 0, sizeof(ms));
        memset(&tip, 0, sizeof(tip));
        memset(&best_header, 0, sizeof(best_header));
        memset(&stats, 0, sizeof(stats));

        bool ok = connman_init(&cm, chain_params_get(), &sigs);
        main_state_init(&ms);
        tip.nHeight = 3117074;
        best_header.nHeight = 3117074;
        ok = ok && active_chain_move_window_tip(&ms.chain_active, &tip);
        ms.pindex_best_header = &best_header;
        ok = ok && syncdiag_add_peer(&cm, 21, false,
                                     PEER_HANDSHAKE_COMPLETE) != NULL;
        ok = ok && syncdiag_add_peer(&cm, 22, false,
                                     PEER_HANDSHAKE_COMPLETE) != NULL;
        ok = ok && syncdiag_add_peer(&cm, 23, false,
                                     PEER_HANDSHAKE_COMPLETE) != NULL;

        block_source_policy_reset_for_test();
        legacy_mirror_sync_reset_for_test();
        mirror_consensus_reset_for_test();
        block_source_policy_init(&cm, &ms, NULL);

        stats.enabled = true;
        stats.running = true;
        stats.reachable = false;
        stats.legacy_height = 0;
        stats.local_height = 3117074;
        stats.best_header_height = 3117074;
        stats.target_height = 3117074;
        stats.rpc_errors = 940;
        stats.last_attempt = 123456;
        snprintf(stats.last_blocker_id, sizeof(stats.last_blocker_id),
                 "%s", "rpc-unreachable");
        snprintf(stats.last_error, sizeof(stats.last_error),
                 "%s",
                 "rpc error -28: Activating best chain... height 0 (1%)");
        legacy_mirror_sync_test_set_stats(&stats, &ms);

        rpc_table_init(&tbl);
        register_event_rpc_commands(&tbl);
        if (rpc_is_in_warmup(NULL, 0))
            set_rpc_warmup_finished();
        json_init(&params);
        json_set_array(&params);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "healthcheck", &params, &result);

        const struct json_value *checks = json_get(&result, "checks");
        const struct json_value *ca =
            checks ? json_get(checks, "chain_advance") : NULL;
        ok = ok && result.type == JSON_OBJ;
        ok = ok && ca && ca->type == JSON_OBJ;
        ok = ok && json_get(ca, "selected_source") != NULL &&
            strcmp(json_get_str(json_get(ca, "selected_source")),
                   "p2p") == 0;
        ok = ok && json_get(ca, "selected_source_trust") != NULL &&
            strcmp(json_get_str(json_get(ca, "selected_source_trust")),
                   "native_peer_validated") == 0;
        ok = ok && json_get(&result, "active_source") != NULL &&
            strcmp(json_get_str(json_get(&result, "active_source")),
                   "p2p") == 0;
        ok = ok && json_get(&result, "active_source_trust") != NULL &&
            strcmp(json_get_str(json_get(&result, "active_source_trust")),
                   "native_peer_validated") == 0;
        ok = ok && json_get(&result, "active_blocker") != NULL &&
            strcmp(json_get_str(json_get(&result, "active_blocker")),
                   "") == 0;
        ok = ok && json_get(&result, "candidate_source") != NULL &&
            strcmp(json_get_str(json_get(&result, "candidate_source")),
                   "legacy_advisory") == 0;
        ok = ok && json_get(&result, "candidate_blocker") != NULL &&
            strcmp(json_get_str(json_get(&result, "candidate_blocker")),
                   "") == 0;
        ok = ok && json_get(&result, "candidate_blocker_scope") != NULL &&
            strcmp(json_get_str(json_get(
                       &result, "candidate_blocker_scope")),
                   "advisory_only") == 0;
        ok = ok && json_get(&result, "legacy_advisory_blocker") != NULL &&
            strcmp(json_get_str(json_get(
                       &result, "legacy_advisory_blocker")),
                   "rpc-unreachable") == 0;
        ok = ok && json_get(&result, "mirror_monitor_running") != NULL &&
            json_get_bool(json_get(&result, "mirror_monitor_running"));
        ok = ok && json_get(&result,
                            "zclassicd_rpc_transport_reachable") != NULL &&
            json_get_bool(json_get(&result,
                                   "zclassicd_rpc_transport_reachable"));
        ok = ok && json_get(&result, "legacy_oracle_usable") != NULL &&
            !json_get_bool(json_get(&result, "legacy_oracle_usable"));
        ok = ok && json_get(&result, "zclassicd_rpc_error_code") != NULL &&
            json_get_int(json_get(&result,
                                  "zclassicd_rpc_error_code")) == -28;
        ok = ok && json_get(&result,
                            "zclassicd_rpc_error_message") != NULL &&
            strstr(json_get_str(json_get(
                       &result, "zclassicd_rpc_error_message")),
                   "Activating best chain") != NULL;
        ok = ok && json_get(&result, "mirror_rpc_errors") != NULL &&
            json_get_int(json_get(&result, "mirror_rpc_errors")) == 940;
        ok = ok && json_get(&result, "mirror_last_attempt") != NULL &&
            json_get_int(json_get(&result, "mirror_last_attempt")) == 123456;
        ok = ok && json_get(&result, "mirror_active_error_code") != NULL &&
            strcmp(json_get_str(json_get(&result,
                                         "mirror_active_error_code")),
                   "rpc-unreachable") == 0;
        ok = ok && json_get(&result, "mirror_active_error_detail") != NULL &&
            strstr(json_get_str(json_get(
                       &result, "mirror_active_error_detail")),
                   "Activating best chain") != NULL;

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
        json_free(&params);
        json_free(&result);
        rpc_net_set_connman(NULL);
        block_source_policy_reset_for_test();
        legacy_mirror_sync_reset_for_test();
        mirror_consensus_reset_for_test();
        main_state_free(&ms);
        connman_free(&cm);
    }

    printf("getsyncdetail: exposes mirror override safety context "
           "(RED)... ");
    {
        struct json_value result;
        json_init(&result);
        legacy_mirror_sync_reset_for_test();
        mirror_consensus_reset_for_test();
        mirror_consensus_set_enabled(true);
        mirror_consensus_record_override(300, "body-hash-mismatch");
        mirror_consensus_record_blocker("body-hash-mismatch");

        bool ok = api_getsyncdetail(&result);
        ok = ok && result.type == JSON_OBJ;
        ok = ok && json_get(&result, "mirror_unsafe_overrides_total") != NULL &&
            json_get_int(json_get(&result,
                                  "mirror_unsafe_overrides_total")) == 1;
        ok = ok && json_get(&result, "mirror_last_override_safe") != NULL &&
            !json_get_bool(json_get(&result, "mirror_last_override_safe"));
        ok = ok && json_get(&result, "mirror_last_override_reason") != NULL &&
            strcmp(json_get_str(json_get(&result,
                                         "mirror_last_override_reason")),
                   "body-hash-mismatch") == 0;
        ok = ok && json_get(&result, "mirror_last_override_scope") != NULL &&
            strcmp(json_get_str(json_get(&result,
                                         "mirror_last_override_scope")),
                   "unsafe_no_authorized_scope") == 0;

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
        json_free(&result);
        legacy_mirror_sync_reset_for_test();
        mirror_consensus_reset_for_test();
    }

    return failures;
}
