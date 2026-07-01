/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * API controller unit tests — routing, input validation, edge cases,
 * and supply calculation correctness. */

#include "test/test_helpers.h"
#include "controllers/api_controller.h"
#include "controllers/explorer_internal.h"
#include "controllers/name_controller.h"
#include "controllers/network_controller.h"
#include "jobs/reducer_frontier.h"
#include "json/json.h"
#include "models/file_service.h"
#include "models/znam.h"
#include "net/connman.h"
#include "net/net.h"
#include "services/block_source_policy.h"
#include "services/node_health_service.h"
#include "sync/sync_state.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include <string.h>
#include <inttypes.h>
#include <unistd.h>

size_t api_json_error(uint8_t *r, size_t max, const char *headers,
                      const char *message);
size_t api_resource_route_count(void);
const char *api_resource_route_resource_at(size_t i);
const char *api_resource_route_action_at(size_t i);

static struct block_index *api_test_insert_block(struct main_state *ms,
                                                 struct uint256 *hash,
                                                 int height,
                                                 struct block_index *prev)
{
    memset(hash, 0, sizeof(*hash));
    hash->data[0] = (uint8_t)(height & 0xff);
    hash->data[1] = (uint8_t)((height >> 8) & 0xff);
    hash->data[2] = 0x41;

    struct block_index *bi =
        chainstate_insert_block_index((struct chainstate *)ms, hash);
    if (!bi)
        return NULL;
    bi->nHeight = height;
    bi->nTime = 1000000 + (uint32_t)height * 150;
    bi->nStatus = BLOCK_HAVE_DATA | BLOCK_VALID_SCRIPTS;
    bi->pprev = prev;
    return bi;
}

static bool api_test_build_chain(struct main_state *ms,
                                 struct block_index **out,
                                 int count)
{
    static struct uint256 hashes[16];
    if (count <= 0 || count > (int)(sizeof(hashes) / sizeof(hashes[0])))
        return false;

    main_state_init(ms);
    struct block_index *prev = NULL;
    for (int h = 0; h < count; h++) {
        out[h] = api_test_insert_block(ms, &hashes[h], h, prev);
        if (!out[h])
            return false;
        prev = out[h];
    }
    ms->pindex_best_header = out[count - 1];
    return active_chain_move_window_tip(&ms->chain_active, out[count - 1]);
}

static bool api_test_init_connman_peer(struct connman *cm,
                                       struct net_address *addr,
                                       struct p2p_node **node_out,
                                       int height)
{
    if (!cm || !addr || !node_out)
        return false;
    memset(cm, 0, sizeof(*cm));
    memset(addr, 0, sizeof(*addr));
    net_manager_init(&cm->manager);
    cm->manager.nodes = zcl_calloc(1, sizeof(*cm->manager.nodes),
                                   "api_test_nodes");
    if (!cm->manager.nodes)
        return false;
    *node_out = p2p_node_create(&cm->manager, ZCL_INVALID_SOCKET, addr,
                                "api-status-peer", false);
    if (!*node_out)
        return false;
    (*node_out)->starting_height = height;
    (*node_out)->state = PEER_HANDSHAKE_COMPLETE;
    (*node_out)->services = NODE_NETWORK;
    cm->manager.nodes[0] = *node_out;
    cm->manager.num_nodes = 1;
    rpc_net_set_connman(cm);
    return true;
}

static const char *api_test_body(uint8_t *resp, size_t n, size_t cap)
{
    resp[n < cap ? n : cap - 1] = '\0';
    const char *body = strstr((char *)resp, "\r\n\r\n");
    return body ? body + 4 : NULL;
}

static bool api_test_save_model_block(struct node_db *ndb, int height,
                                      uint8_t seed)
{
    struct db_block b;
    uint8_t solution[1] = {seed};

    memset(&b, 0, sizeof(b));
    memset(b.hash, seed, sizeof(b.hash));
    memset(b.prev_hash, seed + 1, sizeof(b.prev_hash));
    memset(b.merkle_root, seed + 2, sizeof(b.merkle_root));
    memset(b.nonce, seed + 3, sizeof(b.nonce));
    memset(b.chain_work, seed + 4, sizeof(b.chain_work));
    memset(b.sapling_root, seed + 5, sizeof(b.sapling_root));
    memset(b.sprout_root, seed + 6, sizeof(b.sprout_root));
    b.height = height;
    b.version = 4;
    b.time = (uint32_t)(1000000 + height * 75);
    b.bits = 1;
    b.solution = solution;
    b.solution_len = sizeof(solution);
    b.status = 3;
    b.num_tx = 1;
    return db_block_save(ndb, &b);
}

static bool api_test_save_model_utxo(struct node_db *ndb, int height,
                                     uint8_t seed, int64_t value)
{
    struct db_utxo u;
    uint8_t script[1] = {0x51};

    memset(&u, 0, sizeof(u));
    memset(u.txid, seed, sizeof(u.txid));
    u.vout = 0;
    u.value = value;
    u.script = script;
    u.script_len = sizeof(script);
    u.script_type = SCRIPT_OTHER;
    u.height = height;
    return db_utxo_save(ndb, &u);
}

int test_api(void)
{
    int failures = 0;
    uint8_t resp[8192];

    printf("api: NULL params return 0... ");
    {
        size_t n = api_handle_request(NULL, "/api/blocks", NULL, 0, resp, sizeof(resp));
        bool ok = (n == 0);
        n = api_handle_request("GET", NULL, NULL, 0, resp, sizeof(resp));
        ok = ok && (n == 0);
        n = api_handle_request("GET", "/api/blocks", NULL, 0, NULL, sizeof(resp));
        ok = ok && (n == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: POST returns 405... ");
    {
        size_t n = api_handle_request("POST", "/api/blocks", NULL, 0,
                                       resp, sizeof(resp));
        bool ok = (n > 0 && strstr((char *)resp, "405") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: OPTIONS returns CORS headers... ");
    {
        size_t n = api_handle_request("OPTIONS", "/api/blocks", NULL, 0,
                                       resp, sizeof(resp));
        resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = '\0';
        bool ok = (n > 0 && strstr((char *)resp, "Access-Control") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: unknown endpoint returns 404... ");
    {
        size_t n = api_handle_request("GET", "/api/nonexistent", NULL, 0,
                                       resp, sizeof(resp));
        resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = '\0';
        bool ok = (n > 0 && strstr((char *)resp, "404") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: json error escapes runtime message... ");
    {
        const char *headers =
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Type: application/json\r\n\r\n";
        const char *msg = "bad \"msg\"\nretry";
        size_t n = api_json_error(resp, sizeof(resp), headers, msg);
        resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = '\0';
        const char *body = strstr((char *)resp, "\r\n\r\n");
        bool ok = n > 0 && body != NULL;
        struct json_value root;
        json_init(&root);
        if (ok) {
            body += 4;
            ok = json_read(&root, body, strlen(body));
            ok = ok && strcmp(json_get_str(json_get(&root, "error")),
                              msg) == 0;
        }
        json_free(&root);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: health escapes runtime error strings... ");
    {
        test_reset_shared_globals();
        struct error_ring *er = error_ring_global();
        const char *msg = "bad \"msg\"\n\"healthy\":true";
        error_ring_init(er);
        error_ring_observer(EV_BLOCK_REJECTED, 0, msg, (uint32_t)strlen(msg),
                            er);

        size_t n = api_handle_request("GET", "/api/health", NULL, 0,
                                      resp, sizeof(resp));
        resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = '\0';
        const char *body = strstr((char *)resp, "\r\n\r\n");
        bool ok = n > 0 && body != NULL;
        struct json_value root;
        json_init(&root);
        if (ok) {
            body += 4;
            ok = json_read(&root, body, strlen(body));
            const struct json_value *errors =
                ok ? json_get(&root, "errors") : NULL;
            ok = errors != NULL;
            ok = ok && strcmp(json_get_str(json_get(errors, "last")),
                              msg) == 0;
            ok = ok && strcmp(json_get_str(json_get(errors, "last_type")),
                              event_type_name(EV_BLOCK_REJECTED)) == 0;
            ok = ok && !json_get_bool(json_get(&root, "serving"));
            ok = ok && json_get_int(json_get(&root, "warning_count")) >= 1;
            const struct json_value *status =
                ok ? json_get(&root, "status") : NULL;
            ok = ok && status != NULL;
            ok = ok && !json_get_bool(json_get(status, "serving"));
            ok = ok && strcmp(json_get_str(json_get(status,
                                                    "blocking_reason")),
                              "no_peers") == 0;
            ok = ok && json_get_bool(json_get(status, "warning"));
            ok = ok && strstr(json_get_str(json_get(status,
                                                    "warning_reasons")),
                              "recent_error") != NULL;
        }
        json_free(&root);
        error_ring_init(er);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: json route emits oversized names payload safely... ");
    {
        char dbdir[256];
        char dbpath[320];
        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_api_names_%d",
                 (int)getpid());
        mkdir(dbdir, 0755);
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", dbdir);

        bool ok = node_db_open(&ndb, dbpath);
        for (int i = 0; ok && i < 100; i++) {
            struct znam_entry e;
            memset(&e, 0, sizeof(e));
            snprintf(e.name, sizeof(e.name), "api-name-%03d", i);
            snprintf(e.owner_address, sizeof(e.owner_address),
                     "owner-address-for-api-route-%03d", i);
            e.target_type = ZNAM_TYPE_CONTENT;
            snprintf(e.target_value, sizeof(e.target_value),
                     "sha3:%064d:%064d", i, 1000 + i);
            memset(e.reg_txid, (uint8_t)(i + 1), sizeof(e.reg_txid));
            e.reg_height = i + 1;
            ok = db_znam_save(&ndb, &e);
        }

        if (ok) {
            uint8_t big_resp[65536];
            rpc_name_set_state(&ndb);
            size_t n = api_handle_request("GET", "/api/names", NULL, 0,
                                          big_resp, sizeof(big_resp));
            big_resp[n < sizeof(big_resp) ? n : sizeof(big_resp) - 1] = '\0';
            ok = n > 16384 &&
                 strstr((char *)big_resp, "HTTP/1.1 200 OK") != NULL &&
                 strstr((char *)big_resp, "\"api-name-099\"") != NULL;
            rpc_name_set_state(NULL);
            node_db_close(&ndb);
        }

        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);

    if (ok) printf("OK\n");
    else { printf("FAIL\n"); failures++; }
    }

    printf("api: REST index explains v1 first call and CRUD shape... ");
    {
        size_t n = api_handle_request("GET", "/api/v1", NULL, 0,
                                      resp, sizeof(resp));
        const char *body = api_test_body(resp, n, sizeof(resp));
        struct json_value root;
        json_init(&root);
        bool ok = n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.rest_index.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "api_version")),
                          "v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "base_path")),
                          "/api/v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "compat_base_path")),
                          "/api") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "first_call")),
                          "/api/v1/agent") == 0;
        ok = ok && json_get(json_get(&root, "crud"), "read_collection") != NULL;
        ok = ok && json_size(json_get(&root, "resources")) >= 4;
        ok = ok && strcmp(json_get_str(json_get(json_get(&root, "mcp"),
                                                "first_tool")),
                          "zcl_agent") == 0;
        json_free(&root);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: resource route table exposes controller-style names... ");
    {
        bool saw_agent = false;
        bool saw_blocks = false;
        bool saw_factoids = false;
        size_t count = api_resource_route_count();
        for (size_t i = 0; i < count; i++) {
            const char *resource = api_resource_route_resource_at(i);
            const char *action = api_resource_route_action_at(i);
            if (!resource || !action)
                continue;
            if (strcmp(resource, "agent") == 0 &&
                strcmp(action, "show") == 0)
                saw_agent = true;
            if (strcmp(resource, "blocks") == 0 &&
                strcmp(action, "index") == 0)
                saw_blocks = true;
            if (strcmp(resource, "factoids") == 0 &&
                strcmp(action, "show") == 0)
                saw_factoids = true;
        }
        bool ok = count >= 16 && saw_agent && saw_blocks && saw_factoids;
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: empty block ID not routed... ");
    {
        size_t n = api_handle_request("GET", "/api/block/", NULL, 0,
                                       resp, sizeof(resp));
        resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = '\0';
        bool ok = (n > 0);
        if (ok) printf("OK (got response, %zu bytes)\n", n);
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: zero-length response buffer returns 0... ");
    {
        size_t n = api_handle_request("GET", "/api/blocks", NULL, 0, resp, 0);
        bool ok = (n == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: node status tip identity follows provable tip... ");
    {
        test_reset_shared_globals();
        struct main_state ms;
        struct block_index *blocks[4] = {0};
        bool ok = api_test_build_chain(&ms, blocks, 4);
        reducer_frontier_provable_tip_set(1);
        api_set_state(&ms, NULL, NULL, NULL, "/tmp");

        char hstar_hex[65] = {0};
        char active_hex[65] = {0};
        if (blocks[1] && blocks[1]->phashBlock)
            uint256_get_hex(blocks[1]->phashBlock, hstar_hex);
        if (blocks[3] && blocks[3]->phashBlock)
            uint256_get_hex(blocks[3]->phashBlock, active_hex);
        char expected[96];
        char forbidden[96];
        snprintf(expected, sizeof(expected), "\"tip_hash\":\"%s\"",
                 hstar_hex);
        snprintf(forbidden, sizeof(forbidden), "\"tip_hash\":\"%s\"",
                 active_hex);

        size_t n = api_handle_request("GET", "/api/node/status", NULL, 0,
                                      resp, sizeof(resp));
        resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = '\0';
        ok = ok && n > 0;
        ok = ok && strstr((char *)resp, "\"tip_height\":1") != NULL;
        ok = ok && strstr((char *)resp, expected) != NULL;
        ok = ok && strstr((char *)resp, forbidden) == NULL;

        api_set_state(NULL, NULL, NULL, NULL, NULL);
        reducer_frontier_provable_tip_reset();
        main_state_free(&ms);
        test_reset_shared_globals();

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: public status exposes compact summary shape... ");
    {
        test_reset_shared_globals();
        struct main_state ms;
        struct block_index *blocks[3] = {0};
        bool ok = api_test_build_chain(&ms, blocks, 3);
        reducer_frontier_provable_tip_set(2);
        api_set_state(&ms, NULL, NULL, NULL, "/tmp");

        size_t n = api_handle_request("GET", "/api/status", NULL, 0,
                                      resp, sizeof(resp));
        const char *body = api_test_body(resp, n, sizeof(resp));
        struct json_value root;
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.public_status.v1") == 0;
        ok = ok && json_get(&root, "status") != NULL;
        ok = ok && json_get(&root, "height") != NULL;
        ok = ok && json_get(&root, "recommended_endpoints") != NULL;
        json_free(&root);

        api_set_state(NULL, NULL, NULL, NULL, NULL);
        reducer_frontier_provable_tip_reset();
        main_state_free(&ms);
        test_reset_shared_globals();

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: /api/v1/agent and compat aliases compact summary... ");
    {
        test_reset_shared_globals();
        struct main_state ms;
        struct block_index *blocks[3] = {0};
        bool ok = api_test_build_chain(&ms, blocks, 3);
        reducer_frontier_provable_tip_set(2);
        api_set_state(&ms, NULL, NULL, NULL, "/tmp");

        size_t n = api_handle_request("GET", "/api/v1/agent", NULL, 0,
                                      resp, sizeof(resp));
        const char *body = api_test_body(resp, n, sizeof(resp));
        struct json_value root;
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.public_status.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "api_version")),
                          "v1") == 0;
        json_free(&root);

        n = api_handle_request("GET", "/api/v1/node", NULL, 0,
                               resp, sizeof(resp));
        body = api_test_body(resp, n, sizeof(resp));
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.public_status.v1") == 0;
        json_free(&root);

        n = api_handle_request("GET", "/api/agent", NULL, 0,
                               resp, sizeof(resp));
        body = api_test_body(resp, n, sizeof(resp));
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.public_status.v1") == 0;
        json_free(&root);

        api_set_state(NULL, NULL, NULL, NULL, NULL);
        reducer_frontier_provable_tip_reset();
        main_state_free(&ms);
        test_reset_shared_globals();

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: public status treats one-block served gap as healthy... ");
    {
        test_reset_shared_globals();
        struct main_state ms;
        struct connman cm;
        struct net_address addr;
        struct p2p_node *node = NULL;
        struct block_index *blocks[4] = {0};
        struct cac_decision decision;
        bool ok = api_test_build_chain(&ms, blocks, 4);
        ok = ok && api_test_init_connman_peer(&cm, &addr, &node, 3);

        memset(&decision, 0, sizeof(decision));
        decision.result = CAC_DECISION_USE_SOURCE;
        decision.selected_source = CAC_SOURCE_P2P;
        decision.local_height = 2;
        decision.target_height = 3;
        decision.projection_height = 2;
        struct cac_source_status *p2p =
            &decision.sources[CAC_SOURCE_P2P];
        p2p->source = CAC_SOURCE_P2P;
        p2p->available = true;
        p2p->healthy = true;
        p2p->selectable = true;
        p2p->height = 3;

        if (ok) {
            (void)node;
            reducer_frontier_provable_tip_set(2);
            node_health_test_set_log_head_override(3);
            node_health_test_set_chain_advance_decision_override(&decision);
            sync_set_state(SYNC_IDLE, "api status reset");
            sync_set_state(SYNC_FINDING_PEERS, "api status");
            sync_set_state(SYNC_HEADERS_DOWNLOAD, "api status");
            api_set_state(&ms, NULL, NULL, NULL, "/tmp");

            size_t n = api_handle_request("GET", "/api/status", NULL, 0,
                                          resp, sizeof(resp));
            const char *body = api_test_body(resp, n, sizeof(resp));
            struct json_value root;
            json_init(&root);
            ok = n > 0 && body && json_read(&root, body, strlen(body));
            ok = ok && strcmp(json_get_str(json_get(&root, "status")),
                              "healthy") == 0;
            ok = ok && json_get_bool(json_get(&root, "healthy"));
            ok = ok && json_get_bool(json_get(&root, "serving"));
            ok = ok && !json_get_bool(json_get(&root, "operator_needed"));
            ok = ok && json_get_int(json_get(&root, "height")) == 2;
            ok = ok && json_get_int(json_get(&root, "target_height")) == 3;
            ok = ok && json_get_int(json_get(&root, "gap")) == 1;
            ok = ok && strcmp(json_get_str(json_get(&root,
                                                    "primary_blocker")),
                              "none") == 0;
            json_free(&root);
        }

        node_health_test_set_chain_advance_decision_override(NULL);
        node_health_test_set_log_head_override(-2);
        api_set_state(NULL, NULL, NULL, NULL, NULL);
        reducer_frontier_provable_tip_reset();
        main_state_free(&ms);
        rpc_net_set_connman(NULL);
        net_manager_free(&cm.manager);
        test_reset_shared_globals();

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: public status still degrades material served gap... ");
    {
        test_reset_shared_globals();
        struct main_state ms;
        struct connman cm;
        struct net_address addr;
        struct p2p_node *node = NULL;
        struct block_index *blocks[4] = {0};
        struct cac_decision decision;
        bool ok = api_test_build_chain(&ms, blocks, 4);
        ok = ok && api_test_init_connman_peer(&cm, &addr, &node, 3);

        memset(&decision, 0, sizeof(decision));
        decision.result = CAC_DECISION_USE_SOURCE;
        decision.selected_source = CAC_SOURCE_P2P;
        decision.local_height = 3;
        decision.target_height = 3;
        decision.projection_height = 3;
        struct cac_source_status *p2p =
            &decision.sources[CAC_SOURCE_P2P];
        p2p->source = CAC_SOURCE_P2P;
        p2p->available = true;
        p2p->healthy = true;
        p2p->selectable = true;
        p2p->height = 3;

        if (ok) {
            (void)node;
            reducer_frontier_provable_tip_set(1);
            node_health_test_set_log_head_override(3);
            node_health_test_set_chain_advance_decision_override(&decision);
            sync_set_state(SYNC_IDLE, "api status reset");
            sync_set_state(SYNC_FINDING_PEERS, "api status");
            sync_set_state(SYNC_HEADERS_DOWNLOAD, "api status");
            api_set_state(&ms, NULL, NULL, NULL, "/tmp");

            size_t n = api_handle_request("GET", "/api/status", NULL, 0,
                                          resp, sizeof(resp));
            const char *body = api_test_body(resp, n, sizeof(resp));
            struct json_value root;
            json_init(&root);
            ok = n > 0 && body && json_read(&root, body, strlen(body));
            ok = ok && strcmp(json_get_str(json_get(&root, "status")),
                              "degraded") == 0;
            ok = ok && json_get_bool(json_get(&root, "operator_needed"));
            ok = ok && json_get_int(json_get(&root, "height")) == 1;
            ok = ok && json_get_int(json_get(&root, "target_height")) == 3;
            ok = ok && json_get_int(json_get(&root, "gap")) == 2;
            ok = ok && strcmp(json_get_str(json_get(&root,
                                                    "primary_blocker")),
                              "download_queue_idle") == 0;
            json_free(&root);
        }

        node_health_test_set_chain_advance_decision_override(NULL);
        node_health_test_set_log_head_override(-2);
        api_set_state(NULL, NULL, NULL, NULL, NULL);
        reducer_frontier_provable_tip_reset();
        main_state_free(&ms);
        rpc_net_set_connman(NULL);
        net_manager_free(&cm.manager);
        test_reset_shared_globals();

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: hodl caps to served frontier and refreshes when H* advances... ");
    {
        char dbdir[256];
        char dbpath[320];
        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_api_hodl_%d",
                 (int)getpid());
        mkdir(dbdir, 0755);
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", dbdir);

        api_stop_cache();
        bool ok = node_db_open(&ndb, dbpath);
        ok = ok && api_test_save_model_block(&ndb, 7, 0x71);
        ok = ok && api_test_save_model_block(&ndb, 8, 0x72);
        ok = ok && api_test_save_model_utxo(&ndb, 6, 0x61, 5000000000LL);
        reducer_frontier_provable_tip_set(7);
        api_set_state(NULL, NULL, NULL, &ndb, dbdir);

        size_t n = api_handle_request("GET", "/api/hodl", NULL, 0,
                                      resp, sizeof(resp));
        const char *body = api_test_body(resp, n, sizeof(resp));
        struct json_value root;
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.hodl_wave.v1") == 0;
        ok = ok && json_get_int(json_get(&root, "height")) == 7;
        ok = ok && json_get_int(json_get(&root, "served_tip_height")) == 7;
        ok = ok && json_get_int(json_get(&root, "indexed_tip_height")) == 8;
        ok = ok && json_get_int(json_get(&root, "block_tip_height")) == 8;
        ok = ok && json_get_int(json_get(&root, "utxo_tip_height")) == 6;
        ok = ok && json_get_bool(json_get(&root, "fresh"));
        json_free(&root);

        reducer_frontier_provable_tip_set(8);
        n = api_handle_request("GET", "/api/hodl", NULL, 0,
                               resp, sizeof(resp));
        body = api_test_body(resp, n, sizeof(resp));
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && json_get_int(json_get(&root, "height")) == 8;
        ok = ok && json_get_int(json_get(&root, "served_tip_height")) == 8;
        ok = ok && json_get_int(json_get(&root, "indexed_tip_height")) == 8;
        ok = ok && json_get_int(json_get(&root, "block_tip_height")) == 8;
        json_free(&root);

        api_set_state(NULL, NULL, NULL, NULL, NULL);
        api_stop_cache();
        reducer_frontier_provable_tip_reset();
        node_db_close(&ndb);

        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: factoids caps JSON to served frontier instead of 503... ");
    {
        char dbdir[256];
        char dbpath[320];
        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_api_factoids_%d",
                 (int)getpid());
        mkdir(dbdir, 0755);
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", dbdir);

        api_stop_cache();
        bool ok = node_db_open(&ndb, dbpath);
        ok = ok && api_test_save_model_block(&ndb, 7, 0x81);
        ok = ok && api_test_save_model_block(&ndb, 8, 0x82);
        ok = ok && api_test_save_model_utxo(&ndb, 6, 0x83, 2500000000LL);
        reducer_frontier_provable_tip_set(7);
        api_set_state(NULL, NULL, NULL, &ndb, dbdir);

        size_t n = api_handle_request("GET", "/api/factoids", NULL, 0,
                                      resp, sizeof(resp));
        resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = '\0';
        const char *body = api_test_body(resp, n, sizeof(resp));
        struct json_value root;
        json_init(&root);
        ok = ok && n > 0 &&
             strstr((char *)resp, "HTTP/1.1 200 OK") != NULL &&
             strstr((char *)resp, "Explorer index is ahead") == NULL &&
             body && json_read(&root, body, strlen(body));
        ok = ok && json_get_int(json_get(&root, "chain_height")) == 7;
        ok = ok && json_get_int(json_get(&root, "served_height")) == 7;
        ok = ok && json_get_int(json_get(&root, "indexed_height")) == 8;
        ok = ok && json_get_bool(json_get(&root, "index_capped"));
        json_free(&root);

        reducer_frontier_provable_tip_set(8);
        n = api_handle_request("GET", "/api/factoids", NULL, 0,
                               resp, sizeof(resp));
        resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = '\0';
        body = api_test_body(resp, n, sizeof(resp));
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && json_get_int(json_get(&root, "chain_height")) == 8;
        ok = ok && json_get_int(json_get(&root, "served_height")) == 8;
        ok = ok && json_get_int(json_get(&root, "indexed_height")) == 8;
        ok = ok && !json_get_bool(json_get(&root, "index_capped"));
        json_free(&root);

        api_set_state(NULL, NULL, NULL, NULL, NULL);
        api_stop_cache();
        reducer_frontier_provable_tip_reset();
        node_db_close(&ndb);

        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Supply calculation correctness tests ────────────── */

    printf("api: supply at height 0 is 0... ");
    {
        int64_t s = compute_supply_at_height(0);
        bool ok = (s == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL (got %" PRId64 ")\n", s); failures++; }
    }

    printf("api: supply at height 1 is 12.5 ZCL... ");
    {
        int64_t s = compute_supply_at_height(1);
        bool ok = (s == 1250000000LL);
        if (ok) printf("OK\n");
        else { printf("FAIL (got %" PRId64 ", expected 1250000000)\n", s); failures++; }
    }

    printf("api: supply at height 100 is 100*12.5 ZCL... ");
    {
        int64_t s = compute_supply_at_height(100);
        int64_t expected = 100LL * 1250000000LL;
        bool ok = (s == expected);
        if (ok) printf("OK\n");
        else { printf("FAIL (got %" PRId64 ", expected %" PRId64 ")\n", s, expected); failures++; }
    }

    printf("api: supply at Buttercup activation (707001) includes new rate... ");
    {
        int64_t s = compute_supply_at_height(707001);
        int64_t s_pre = compute_supply_at_height(707000);
        /* Block 707000 should earn post-BC rate: 0.78125 ZCL */
        int64_t expected_increment = 78125000LL;
        bool ok = (s - s_pre == expected_increment);
        if (ok) printf("OK (increment = %" PRId64 ")\n", s - s_pre);
        else { printf("FAIL (increment %" PRId64 ", expected %" PRId64 ")\n",
                      s - s_pre, expected_increment); failures++; }
    }

    printf("api: supply at 710000 is correct post-Buttercup... ");
    {
        int64_t s = compute_supply_at_height(710000);
        int64_t s_bc = compute_supply_at_height(707000);
        /* 3000 post-BC blocks at 78125000 sat each */
        int64_t expected = s_bc + 3000LL * 78125000LL;
        bool ok = (s == expected);
        if (ok) printf("OK\n");
        else { printf("FAIL (got %" PRId64 ", expected %" PRId64 ")\n", s, expected); failures++; }
    }

    printf("api: supply is monotonically increasing... ");
    {
        bool ok = true;
        int64_t prev = 0;
        int64_t test_heights[] = { 0, 1, 100, 1000, 100000, 706999,
                                    707000, 707001, 800000, 1000000,
                                    2000000, 2387000, 2387001, 3000000 };
        for (int i = 0; i < (int)(sizeof(test_heights)/sizeof(test_heights[0])); i++) {
            int64_t s = compute_supply_at_height(test_heights[i]);
            if (s < prev) {
                printf("FAIL (supply decreased at height %" PRId64 ": %" PRId64 " < %" PRId64 ")\n",
                       test_heights[i], s, prev);
                ok = false;
                break;
            }
            prev = s;
        }
        if (ok) printf("OK\n");
        else failures++;
    }

    printf("api: supply never exceeds 21M ZCL... ");
    {
        int64_t max_sat = 2100000000000000LL; /* 21M ZCL */
        bool ok = true;
        /* Check at very high block counts */
        int64_t test_heights[] = { 10000000, 50000000, 100000000 };
        for (int i = 0; i < 3; i++) {
            int64_t s = compute_supply_at_height(test_heights[i]);
            if (s > max_sat) {
                printf("FAIL (supply %" PRId64 " > 21M at height %" PRId64 ")\n",
                       s, test_heights[i]);
                ok = false;
                break;
            }
        }
        if (ok) printf("OK\n");
        else failures++;
    }

    printf("api: pre-Buttercup is all one era (no halving before 707000)... ");
    {
        /* Since 707000 < 840000, all pre-BC blocks at 12.5 ZCL.
         * supply(706999) - supply(706998) should equal 12.5 ZCL */
        int64_t s1 = compute_supply_at_height(706999);
        int64_t s2 = compute_supply_at_height(706998);
        bool ok = (s1 - s2 == 1250000000LL);
        if (ok) printf("OK\n");
        else { printf("FAIL (increment=%" PRId64 ")\n", s1 - s2); failures++; }
    }

    printf("api: supply_zcl_at_height matches compute_supply_at_height... ");
    {
        double zcl = supply_zcl_at_height(100000);
        int64_t sat = compute_supply_at_height(100000);
        bool ok = (zcl == (double)sat / 100000000.0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: trailing slash stripped in routing... ");
    {
        size_t n1 = api_handle_request("GET", "/api/stats/", NULL, 0,
                                        resp, sizeof(resp));
        /* Should either serve stats (503 if cache empty) or match the route */
        bool ok = (n1 > 0);
        if (ok) printf("OK (%zu bytes)\n", n1);
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: zslp token resources serve REST reads... ");
    {
        char dbdir[256];
        char dbpath[320];
        uint8_t txid[32];
        uint8_t token_id[32];
        uint8_t addr_hash[20];
        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        memset(txid, 0x44, sizeof(txid));
        memset(token_id, 0x55, sizeof(token_id));
        memset(addr_hash, 0x66, sizeof(addr_hash));
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_api_zslp_%d", (int)getpid());
        mkdir(dbdir, 0755);
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", dbdir);

        bool ok = node_db_open(&ndb, dbpath);
        if (ok) {
            ok = db_zslp_token_save_key(&ndb, "apitoken", "APITOKEN",
                                        "API Token", 0, "", 42, 1234);
            ok = ok && db_zslp_transfer_save(&ndb, txid, 99, token_id, 2, 77, 1, addr_hash);
            ok = ok && db_zslp_token_save_key(&ndb,
                "5555555555555555555555555555555555555555555555555555555555555555",
                "HEX55", "Hex Token", 0, "", 99, 77);
            api_set_state(NULL, NULL, NULL, &ndb, dbdir);

            size_t n = api_handle_request("GET", "/api/zslp/tokens?limit=10",
                                          NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0) && (strstr((char *)resp, "200 OK") != NULL);
            ok = ok && (strstr((char *)resp, "\"tokens\"") != NULL);
            ok = ok && (strstr((char *)resp, "APITOKEN") != NULL);

            n = api_handle_request("GET", "/api/zslp/tokens/APITOKEN",
                                   NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0) && (strstr((char *)resp, "\"token_id\":\"APITOKEN\"") != NULL);

            n = api_handle_request("GET",
                "/api/zslp/tokens/5555555555555555555555555555555555555555555555555555555555555555/transfers?limit=5",
                NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0) && (strstr((char *)resp, "\"transfers\"") != NULL);
            ok = ok && (strstr((char *)resp, "\"amount\":77") != NULL);

            n = api_handle_request("GET", "/api/zslp/tokens/BAD-TOKEN!",
                                   NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0) && (strstr((char *)resp, "404") != NULL);

            n = api_handle_request("GET", "/api/zslp/tokens?limit=999",
                                   NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0) && (strstr((char *)resp, "404") != NULL);

            api_set_state(NULL, NULL, NULL, NULL, NULL);
            node_db_close(&ndb);
        }

        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: onion announcements serve REST reads... ");
    {
        char dbdir[256];
        char dbpath[320];
        struct node_db ndb;
        bool ok;
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_api_onion_%d", (int)getpid());
        mkdir(dbdir, 0755);
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", dbdir);
        memset(&ndb, 0, sizeof(ndb));
        ok = node_db_open(&ndb, dbpath);

        if (ok) {
            struct db_onion_announcement a, b;
            memset(&a, 0, sizeof(a));
            memset(&b, 0, sizeof(b));
            snprintf(a.onion_address, sizeof(a.onion_address),
                     "%s", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.onion");
            snprintf(a.script_hex, sizeof(a.script_hex), "%s", "6a01");
            a.announced_at = 1;
            snprintf(b.onion_address, sizeof(b.onion_address),
                     "%s", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.onion");
            snprintf(b.script_hex, sizeof(b.script_hex), "%s", "6a02");
            b.announced_at = 2;
            ok = db_onion_announcement_save(&ndb, &a);
            ok = ok && db_onion_announcement_save(&ndb, &b);
            api_set_state(NULL, NULL, NULL, &ndb, dbdir);

            size_t n = api_handle_request("GET", "/api/onion/announcements?limit=2",
                                          NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0) && (strstr((char *)resp, "\"announcements\"") != NULL);
            ok = ok && (strstr((char *)resp,
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.onion") != NULL);

            n = api_handle_request("GET", "/api/onion/announcements?limit=99",
                                   NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0) && (strstr((char *)resp, "404") != NULL);

            api_set_state(NULL, NULL, NULL, NULL, NULL);
            node_db_close(&ndb);
        }

        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: file services serve REST reads... ");
    {
        char dbdir[256];
        char dbpath[320];
        struct node_db ndb;
        bool ok;
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_api_file_services_%d", (int)getpid());
        mkdir(dbdir, 0755);
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", dbdir);
        memset(&ndb, 0, sizeof(ndb));
        ok = node_db_open(&ndb, dbpath);

        if (ok) {
            struct db_file_service fs;
            memset(&fs, 0, sizeof(fs));
            memset(fs.ip, 0x77, sizeof(fs.ip));
            fs.port = 8080;
            fs.is_zcl23 = true;
            ok = db_file_service_save(&ndb, &fs);
            api_set_state(NULL, NULL, NULL, &ndb, dbdir);

            size_t n = api_handle_request("GET", "/api/file-services?limit=1",
                                          NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0) && (strstr((char *)resp, "\"file_services\"") != NULL);
            ok = ok && (strstr((char *)resp, "\"port\":8080") != NULL);

            n = api_handle_request("GET", "/api/file-services?limit=99",
                                   NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0) && (strstr((char *)resp, "404") != NULL);

            api_set_state(NULL, NULL, NULL, NULL, NULL);
            node_db_close(&ndb);
        }

        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: peers serve REST reads... ");
    {
        char dbdir[256];
        char dbpath[320];
        struct node_db ndb;
        bool ok;
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_api_peers_%d", (int)getpid());
        mkdir(dbdir, 0755);
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", dbdir);
        memset(&ndb, 0, sizeof(ndb));
        ok = node_db_open(&ndb, dbpath);

        if (ok) {
            struct db_peer peer;
            memset(&peer, 0, sizeof(peer));
            memset(peer.ip, 0x55, sizeof(peer.ip));
            peer.port = 8333;
            peer.services = 5;
            peer.is_zcl23 = true;
            ok = db_peer_save(&ndb, &peer);
            api_set_state(NULL, NULL, NULL, &ndb, dbdir);

            size_t n = api_handle_request("GET", "/api/peers?limit=1",
                                          NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0) && (strstr((char *)resp, "\"peers\"") != NULL);
            ok = ok && (strstr((char *)resp, "\"port\":8333") != NULL);

            n = api_handle_request("GET", "/api/peers?limit=99",
                                   NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0) && (strstr((char *)resp, "404") != NULL);

            api_set_state(NULL, NULL, NULL, NULL, NULL);
            node_db_close(&ndb);
        }

        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: DELETE method returns 405... ");
    {
        size_t n = api_handle_request("DELETE", "/api/blocks", NULL, 0,
                                       resp, sizeof(resp));
        resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = '\0';
        bool ok = (n > 0 && strstr((char *)resp, "405") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: PUT method returns 405... ");
    {
        size_t n = api_handle_request("PUT", "/api/blocks", NULL, 0,
                                       resp, sizeof(resp));
        resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = '\0';
        bool ok = (n > 0 && strstr((char *)resp, "405") != NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: operator-private classifier boundary-matches... ");
    {
        /* True only at a path boundary (next char '\0', '/', '?'). */
        bool ok = api_route_is_operator_private("/api/wallet") &&
                  api_route_is_operator_private("/api/v1/wallet") &&
                  api_route_is_operator_private("/api/wallet/") &&
                  api_route_is_operator_private("/api/v1/wallet/") &&
                  api_route_is_operator_private("/api/wallet?x=1") &&
                  api_route_is_operator_private("/api/v1/wallet?x=1") &&
                  api_route_is_operator_private("/api/messages") &&
                  api_route_is_operator_private("/api/v1/messages") &&
                  api_route_is_operator_private("/api/v1/swaps") &&
                  api_route_is_operator_private("/api/swaps");
        /* Public routes must stay public — in particular
         * /api/swap_chains must NOT match the /api/swaps prefix. */
        ok = ok && !api_route_is_operator_private("/api/swap_chains") &&
                   !api_route_is_operator_private("/api/v1/swap_chains") &&
                   !api_route_is_operator_private("/api/blocks") &&
                   !api_route_is_operator_private("/api/v1/blocks") &&
                   !api_route_is_operator_private("/api/stats") &&
                   !api_route_is_operator_private("/api/walletfoo") &&
                   !api_route_is_operator_private(NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* The router itself still serves /api/wallet — enforcement is
     * LISTENER-side (https_server 403s operator-private paths before
     * dispatch; in-process callers like wallet_gui stay trusted). See
     * the SECURITY INVARIANT note at api_handle_request. */
    printf("api: router still serves /api/wallet (gate is listener-side)... ");
    {
        size_t n = api_handle_request("GET", "/api/wallet", NULL, 0,
                                       resp, sizeof(resp));
        bool ok = (n > 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
