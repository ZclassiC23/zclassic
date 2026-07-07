/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * API controller unit tests — routing, input validation, edge cases,
 * and supply calculation correctness. */

#include "test/test_helpers.h"
#include "controllers/agent_controller.h"
#include "controllers/api_controller.h"
#include "controllers/explorer_internal.h"
#include "controllers/file_controller.h"
#include "controllers/name_controller.h"
#include "controllers/network_controller.h"
#include "event/event.h"
#include "jobs/reducer_frontier.h"
#include "json/json.h"
#include "models/file_service.h"
#include "models/znam.h"
#include "net/connman.h"
#include "net/net.h"
#include "platform/time_compat.h"
#include "services/block_source_policy.h"
#include "services/node_health_service.h"
#include "storage/progress_store.h"
#include "sync/sync_state.h"
#include "util/alerts.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include <string.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

size_t api_json_error(uint8_t *r, size_t max, const char *headers,
                      const char *message);
size_t api_resource_route_count(void);
const char *api_resource_route_resource_at(size_t i);
const char *api_resource_route_action_at(size_t i);
size_t api_route_contract_count(void);
size_t api_dynamic_resource_route_count(void);
const char *api_dynamic_resource_route_pattern_at(size_t i);
const char *api_dynamic_resource_route_resource_at(size_t i);
const char *api_dynamic_resource_route_action_at(size_t i);
void api_test_seed_supply_caches(const char *canonical, const char *legacy);
size_t compute_deep_stats(uint8_t *r, size_t max);
typedef int (*api_test_rpc_call_fn)(const char *method,
                                    const char *params_json,
                                    char *out,
                                    size_t outmax);
void api_test_set_rpc_call(api_test_rpc_call_fn fn);

#define API_TEST_BLOCK_HASH \
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define API_TEST_PREV_HASH \
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define API_TEST_NEXT_HASH \
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
#define API_TEST_TXID \
    "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
#define API_TEST_TXID2 \
    "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
#define API_TEST_ADDR "t1TestLookupAddr0000000000"

static int api_test_write_rpc(char *out, size_t outmax, const char *json)
{
    size_t len;

    if (!out || outmax == 0 || !json)
        return 0;
    len = strlen(json);
    if (len >= outmax)
        len = outmax - 1;
    memcpy(out, json, len);
    out[len] = '\0';
    return (int)len;
}

static int api_test_lookup_rpc(const char *method,
                               const char *params_json,
                               char *out,
                               size_t outmax)
{
    (void)params_json;

    if (strcmp(method, "getblockhash") == 0)
        return api_test_write_rpc(out, outmax,
            "{\"result\":\"" API_TEST_BLOCK_HASH "\",\"error\":null}");
    if (strcmp(method, "getblock") == 0)
        return api_test_write_rpc(out, outmax,
            "{\"result\":{\"hash\":\"" API_TEST_BLOCK_HASH "\","
            "\"height\":10,\"time\":1700000010,\"size\":1234,"
            "\"difficulty\":12.5,\"confirmations\":2,"
            "\"merkleroot\":\"abababababababababababababababababababababababababababababababab\","
            "\"previousblockhash\":\"" API_TEST_PREV_HASH "\","
            "\"nextblockhash\":\"" API_TEST_NEXT_HASH "\","
            "\"nonce\":\"01020304\","
            "\"tx\":[\"" API_TEST_TXID "\",\"" API_TEST_TXID2 "\"]},"
            "\"error\":null}");
    if (strcmp(method, "getrawtransaction") == 0)
        return api_test_write_rpc(out, outmax,
            "{\"result\":{\"txid\":\"" API_TEST_TXID "\","
            "\"version\":4,\"size\":456,\"locktime\":0,"
            "\"confirmations\":2,\"blockhash\":\"" API_TEST_BLOCK_HASH "\","
            "\"height\":10,\"valuebalance\":0,"
            "\"vout\":[{\"n\":0,\"value\":1.25,"
            "\"scriptPubKey\":{\"addresses\":[\"" API_TEST_ADDR "\"]}}],"
            "\"vin\":[{\"txid\":\"" API_TEST_TXID2 "\",\"vout\":1}]},"
            "\"error\":null}");
    if (strcmp(method, "getaddressbalance") == 0)
        return api_test_write_rpc(out, outmax,
            "{\"result\":{\"balance\":123456789},\"error\":null}");
    if (strcmp(method, "getaddressutxos") == 0)
        return api_test_write_rpc(out, outmax,
            "{\"result\":[{\"txid\":\"" API_TEST_TXID "\","
            "\"outputIndex\":0,\"satoshis\":123456789,\"height\":10}],"
            "\"error\":null}");
    return 0;
}

static const struct json_value *api_test_find_contract(
    const struct json_value *routes,
    const char *path)
{
    if (!routes || !path)
        return NULL;
    for (size_t i = 0; i < json_size(routes); i++) {
        const struct json_value *item = json_at(routes, i);
        const char *item_path = json_get_str(json_get(item, "path"));
        if (item_path && strcmp(item_path, path) == 0)
            return item;
    }
    return NULL;
}

static bool api_test_contract_has_query(const struct json_value *contract,
                                        const char *name)
{
    const struct json_value *params = json_get(contract, "query_params");
    if (!params || !name)
        return false;
    for (size_t i = 0; i < json_size(params); i++) {
        const char *param = json_get_str(json_at(params, i));
        if (param && strcmp(param, name) == 0)
            return true;
    }
    return false;
}

static bool api_test_contract_has_id_param(const struct json_value *contract,
                                           const char *name)
{
    const struct json_value *params = json_get(contract, "id_params");
    if (!params || !name)
        return false;
    for (size_t i = 0; i < json_size(params); i++) {
        const char *param = json_get_str(json_at(params, i));
        if (param && strcmp(param, name) == 0)
            return true;
    }
    return false;
}

static bool api_test_array_has_str(const struct json_value *arr,
                                   const char *needle)
{
    if (!arr || !needle)
        return false;
    for (size_t i = 0; i < json_size(arr); i++) {
        const char *value = json_get_str(json_at(arr, i));
        if (value && strcmp(value, needle) == 0)
            return true;
    }
    return false;
}

static const struct json_value *api_test_find_named(
    const struct json_value *arr,
    const char *name)
{
    if (!arr || !name)
        return NULL;
    for (size_t i = 0; i < json_size(arr); i++) {
        const struct json_value *item = json_at(arr, i);
        const char *item_name = json_get_str(json_get(item, "name"));
        if (item_name && strcmp(item_name, name) == 0)
            return item;
    }
    return NULL;
}

static const struct json_value *api_test_find_str_field(
    const struct json_value *arr,
    const char *field,
    const char *value)
{
    if (!arr || !field || !value)
        return NULL;
    for (size_t i = 0; i < json_size(arr); i++) {
        const struct json_value *item = json_at(arr, i);
        const char *item_value = json_get_str(json_get(item, field));
        if (item_value && strcmp(item_value, value) == 0)
            return item;
    }
    return NULL;
}

static bool api_test_expect_readiness_shape(const struct json_value *root)
{
    const struct json_value *readiness = json_get(root, "readiness");
    if (!readiness || readiness->type != JSON_OBJ)
        return false;

    return strcmp(json_get_str(json_get(readiness, "schema")),
                  "zcl.agent_readiness.v1") == 0 &&
           json_get(root, "readiness_status") != NULL &&
           json_get(root, "chain_serving_ready") != NULL &&
           json_get(root, "index_projection_ready") != NULL &&
           json_get(root, "agent_work_ready") != NULL &&
           json_get(root, "operator_action_required") != NULL &&
           json_get(root, "readiness_next_action") != NULL &&
           json_get_int(json_get(readiness, "schema_version")) == 1 &&
           json_get(readiness, "status") != NULL &&
           json_get(readiness, "chain_serving_ready") != NULL &&
           json_get(readiness, "index_projection_ready") != NULL &&
           json_get(readiness, "agent_work_ready") != NULL &&
           json_get(readiness, "operator_action_required") != NULL &&
           json_get(readiness, "tip_gap_blocks") != NULL &&
           json_get(readiness, "index_gap_blocks") != NULL &&
           json_get(readiness, "reducer_log_gap_blocks") != NULL &&
           json_get(readiness, "next_action") != NULL &&
           json_get(readiness, "semantics") != NULL;
}

static bool api_test_expect_security_posture_shape(
    const struct json_value *root)
{
    const struct json_value *posture = json_get(root, "security_posture");
    if (!posture || posture->type != JSON_OBJ)
        return false;

    return strcmp(json_get_str(json_get(posture, "schema")),
                  "zcl.security_posture.v1") == 0 &&
           json_get_int(json_get(posture, "schema_version")) == 1 &&
           json_get(posture, "status") != NULL &&
           json_get(posture, "review_required") != NULL &&
           json_get(posture, "bootstrap_model") != NULL &&
           json_get(posture, "snapshot_full_validation_complete") != NULL &&
           json_get(posture, "full_history_validation_state") != NULL &&
           json_get(posture, "nullifier_history_complete") != NULL &&
           json_get(posture, "nullifier_activation_cursor") != NULL &&
           json_get(posture, "nullifier_history_state") != NULL &&
           json_get(posture, "next_action") != NULL &&
           strstr(json_get_str(json_get(posture, "semantics")),
                  "serving/healthy are liveness signals") != NULL;
}

static bool api_test_expect_lane_safety_fields(
    const struct json_value *root, const char *lane,
    bool restart_ok, bool deploy_ok, bool requires,
    const char *target, const char *action)
{
    return strcmp(json_get_str(json_get(root, "operator_lane_name")),
                  lane) == 0 &&
           json_get_bool(json_get(root, "automation_restart_ok")) ==
               restart_ok &&
           json_get_bool(json_get(root, "automation_deploy_ok")) ==
               deploy_ok &&
           json_get_bool(json_get(root,
                                  "requires_operator_confirmation")) ==
               requires &&
           strcmp(json_get_str(json_get(root, "preferred_deploy_target")),
                  target) == 0 &&
           strcmp(json_get_str(json_get(root, "safe_default_action")),
                  action) == 0;
}

static const struct json_value *api_test_openapi_get(
    const struct json_value *root,
    const char *path)
{
    const struct json_value *paths = json_get(root, "paths");
    const struct json_value *path_item = json_get(paths, path);
    return json_get(path_item, "get");
}

static bool api_test_openapi_has_param(const struct json_value *operation,
                                       const char *name,
                                       const char *location)
{
    const struct json_value *params = json_get(operation, "parameters");
    if (!params || !name || !location)
        return false;
    for (size_t i = 0; i < json_size(params); i++) {
        const struct json_value *param = json_at(params, i);
        if (strcmp(json_get_str(json_get(param, "name")), name) == 0 &&
            strcmp(json_get_str(json_get(param, "in")), location) == 0)
            return true;
    }
    return false;
}

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

static bool api_test_seed_durable_tip(const char *dir, int height)
{
    if (!dir || height < 0)
        return false;

    progress_store_close();
    if (!progress_store_open(dir))
        return false;

    sqlite3 *db = progress_store_db();
    if (!db)
        return false;

    if (sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS tip_finalize_log ("
        "height INTEGER PRIMARY KEY, status TEXT NOT NULL, ok INTEGER NOT NULL,"
        "work_delta_high INTEGER NOT NULL, work_delta_low INTEGER NOT NULL,"
        "utxo_size_after INTEGER NOT NULL, reorg_depth INTEGER NOT NULL,"
        "finalized_at INTEGER NOT NULL, tip_hash BLOB)",
        NULL, NULL, NULL) != SQLITE_OK)
        return false;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO stage_cursor(name,cursor,updated_at) "
        "VALUES('tip_finalize',?,0)",
        -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok)
        return false;

    uint8_t hash[32] = {0};
    hash[0] = (uint8_t)(height & 0xff);
    hash[1] = (uint8_t)((height >> 8) & 0xff);
    hash[2] = 0xA9;

    st = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO tip_finalize_log "
        "(height,status,ok,work_delta_high,work_delta_low,utxo_size_after,"
        "reorg_depth,finalized_at,tip_hash) "
        "VALUES(?,'anchor',1,0,0,0,0,0,?)",
        -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    sqlite3_bind_blob(st, 2, hash, sizeof(hash), SQLITE_TRANSIENT);
    ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
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

static bool api_test_expect_freshness(const struct json_value *root,
                                      const char *source_projection,
                                      int64_t served_height,
                                      int64_t indexed_height,
                                      bool fresh)
{
    if (!root || !source_projection)
        return false;
    const char *actual_source =
        json_get_str(json_get(root, "source_projection"));
    const char *actual_freshness =
        json_get_str(json_get(root, "freshness"));
    const char *actual_blocker = json_get_str(json_get(root, "blocker"));
    if (!actual_source || strcmp(actual_source, source_projection) != 0)
        return false;
    if (json_get_int(json_get(root, "served_height")) != served_height)
        return false;
    if (json_get_int(json_get(root, "indexed_height")) != indexed_height)
        return false;
    if (json_get_bool(json_get(root, "fresh")) != fresh)
        return false;
    if (fresh) {
        return actual_freshness && strcmp(actual_freshness, "fresh") == 0 &&
               actual_blocker && strcmp(actual_blocker, "none") == 0;
    }
    return actual_freshness && strcmp(actual_freshness, "fresh") != 0 &&
           actual_blocker && strcmp(actual_blocker, "none") != 0;
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
        const char *body = api_test_body(resp, n, sizeof(resp));
        struct json_value root;
        json_init(&root);
        bool ok = (n > 0 && strstr((char *)resp, "405") != NULL &&
                   body && json_read(&root, body, strlen(body)));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.rest_error.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "api_version")),
                          "v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "error")),
                          "Method not allowed") == 0;
        json_free(&root);
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
        const char *body = api_test_body(resp, n, sizeof(resp));
        struct json_value root;
        json_init(&root);
        bool ok = (n > 0 && strstr((char *)resp, "404") != NULL &&
                   body && json_read(&root, body, strlen(body)));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.rest_error.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "api_version")),
                          "v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "error")),
                          "Unknown API endpoint") == 0;
        json_free(&root);
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
            ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                              "zcl.rest_error.v1") == 0;
            ok = ok && strcmp(json_get_str(json_get(&root, "api_version")),
                              "v1") == 0;
            ok = ok && strcmp(json_get_str(json_get(&root, "error")),
                              msg) == 0;
        }
        json_free(&root);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: json error truncation returns written bytes... ");
    {
        const char *headers = "HTTP/1.1 500\r\n\r\n";
        uint8_t tiny[48];
        memset(tiny, 0xA5, sizeof(tiny));
        size_t n = api_json_error(tiny, sizeof(tiny), headers,
                                  "this message is intentionally longer than the response buffer");
        bool ok = n < sizeof(tiny) && tiny[n] == '\0';
        ok = ok && strstr((char *)tiny, "HTTP/1.1 500") != NULL;
        ok = ok && strstr((char *)tiny, "\r\n\r\n") != NULL;
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: health escapes runtime error strings... ");
    {
        test_reset_shared_globals();
        progress_store_close();
        reducer_frontier_provable_tip_reset();
        api_set_state(NULL, NULL, NULL, NULL, NULL);
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
        ok = ok && strstr((char *)resp,
                          "HTTP/1.1 503 Service Unavailable") != NULL;
        struct json_value root;
        json_init(&root);
        if (ok) {
            body += 4;
            ok = json_read(&root, body, strlen(body));
            ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                              "zcl.health.v1") == 0;
            ok = ok && api_test_expect_freshness(&root, "served_tip",
                                                 0, 0, true);
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
        api_set_state(NULL, NULL, NULL, NULL, NULL);
        reducer_frontier_provable_tip_reset();
        test_reset_shared_globals();
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

    printf("api: name show includes service and address records... ");
    {
        char dbdir[256];
        char dbpath[320];
        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_api_name_show_%d",
                 (int)getpid());
        mkdir(dbdir, 0755);
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", dbdir);

        bool opened = node_db_open(&ndb, dbpath);
        bool ok = opened;
        if (ok) {
            struct znam_entry e;
            memset(&e, 0, sizeof(e));
            snprintf(e.name, sizeof(e.name), "alice");
            snprintf(e.owner_address, sizeof(e.owner_address),
                     "t1owner-for-name-show");
            e.target_type = ZNAM_TYPE_BTC;
            snprintf(e.target_value, sizeof(e.target_value),
                     "1primary-target-address");
            memset(e.reg_txid, 0x42, sizeof(e.reg_txid));
            e.reg_height = 42;
            memset(e.last_update_txid, 0x43, sizeof(e.last_update_txid));
            ok = db_znam_save(&ndb, &e);
        }
        ok = ok && db_znam_text_save(&ndb, "alice", "url",
                                     "https://alice.example");
        ok = ok && db_znam_text_save(&ndb, "alice", "service.onion",
                                     "aliceexample.onion:8033");
        ok = ok && db_znam_addr_save(&ndb, "alice", ZNAM_TYPE_LTC,
                                     "LaliceAddress");
        ok = ok && db_znam_addr_save(&ndb, "alice", ZNAM_TYPE_BTC,
                                     "1aliceAddress");

        if (ok) {
            uint8_t resp[32768];
            rpc_name_set_state(&ndb);
            size_t n = api_handle_request("GET", "/api/v1/names/alice",
                                          NULL, 0, resp, sizeof(resp));
            rpc_name_set_state(NULL);
            const char *body = api_test_body(resp, n, sizeof(resp));
            struct json_value root;
            json_init(&root);
            ok = n > 0 && body && json_read(&root, body, strlen(body));
            const struct json_value *texts =
                ok ? json_get(&root, "text_records") : NULL;
            const struct json_value *services =
                ok ? json_get(&root, "service_records") : NULL;
            const struct json_value *addrs =
                ok ? json_get(&root, "address_records") : NULL;
            const struct json_value *url =
                api_test_find_str_field(texts, "key", "url");
            const struct json_value *svc =
                api_test_find_str_field(services, "key", "service.onion");
            const struct json_value *btc =
                api_test_find_str_field(addrs, "type", "bitcoin");
            const struct json_value *ltc =
                api_test_find_str_field(addrs, "type", "litecoin");
            ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                              "zcl.names.show.v1") == 0;
            ok = ok && strcmp(json_get_str(json_get(&root, "name")),
                              "alice") == 0;
            ok = ok && json_get_int(json_get(&root, "target_type")) ==
                       ZNAM_TYPE_BTC;
            ok = ok && strcmp(json_get_str(json_get(&root, "type")),
                              "bitcoin") == 0;
            ok = ok && json_size(texts) == 2 &&
                       json_get_int(json_get(&root, "text_record_count")) == 2;
            ok = ok && json_size(services) == 1 &&
                       json_get_int(json_get(&root,
                                             "service_record_count")) == 1;
            ok = ok && json_size(addrs) == 2 &&
                       json_get_int(json_get(&root,
                                             "address_record_count")) == 2;
            ok = ok && url &&
                 strcmp(json_get_str(json_get(url, "value")),
                        "https://alice.example") == 0;
            ok = ok && svc &&
                 strcmp(json_get_str(json_get(svc, "value")),
                        "aliceexample.onion:8033") == 0;
            ok = ok && btc &&
                 strcmp(json_get_str(json_get(btc, "address")),
                        "1aliceAddress") == 0;
            ok = ok && ltc &&
                 strcmp(json_get_str(json_get(ltc, "address")),
                        "LaliceAddress") == 0;
            json_free(&root);
        }

        rpc_name_set_state(NULL);
        if (opened)
            node_db_close(&ndb);
        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: REST index explains v1 first call and CRUD shape... ");
    {
        static uint8_t index_resp[262144];
        size_t n = api_handle_request("GET", "/api/v1", NULL, 0,
                                      index_resp, sizeof(index_resp));
        const char *body = api_test_body(index_resp, n, sizeof(index_resp));
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
        ok = ok && strcmp(json_get_str(json_get(json_get(&root, "aliases"),
                                                "protocols")),
                          "/api/v1/protocols") == 0;
        ok = ok && json_get(json_get(&root, "crud"), "read_collection") != NULL;
        ok = ok && json_get(json_get(&root, "crud"), "read_item") != NULL;
        ok = ok && json_get(json_get(&root, "crud"),
                            "read_singleton") != NULL;
        ok = ok && json_get(json_get(&root, "crud"),
                            "read_subcollection") != NULL;
        ok = ok && json_get(json_get(&root, "crud"),
                            "contract_fields") != NULL;
        const struct json_value *layer_model =
            json_get(&root, "layer_model");
        ok = ok && layer_model &&
             strcmp(json_get_str(json_get(layer_model, "schema")),
                    "zcl.rest_layer_model.v1") == 0;
        ok = ok && layer_model &&
             strcmp(json_get_str(json_get(layer_model, "base_layer")),
                    "zclassic_l1") == 0;
        ok = ok && layer_model &&
             strcmp(json_get_str(json_get(layer_model, "service_layer")),
                    "zclassic23_application_layer") == 0;
        ok = ok && layer_model &&
             strcmp(json_get_str(json_get(layer_model,
                                          "service_layer_alias")),
                    "zclassic23_l2") == 0;
        ok = ok && layer_model &&
             strcmp(json_get_str(json_get(layer_model,
                                          "application_protocol_umbrella")),
                    "zlsp") == 0;
        ok = ok && layer_model &&
             strcmp(json_get_str(json_get(layer_model,
                                          "consensus_authority")),
                    "local_consensus_reducer") == 0;
        ok = ok && layer_model &&
             strstr(json_get_str(json_get(layer_model,
                                          "crud_service_rule")),
                    "transaction-construction requests") != NULL;
        ok = ok && layer_model &&
             strstr(json_get_str(json_get(layer_model,
                                          "consensus_boundary")),
                    "must not change block") != NULL;
        const struct json_value *protocols =
            layer_model ? json_get(layer_model, "application_protocols") : NULL;
        const struct json_value *zlsp_protocol =
            api_test_find_named(protocols, "zlsp");
        const struct json_value *zslp_protocol =
            api_test_find_named(protocols, "zslp");
        const struct json_value *znam_protocol =
            api_test_find_named(protocols, "znam");
        const struct json_value *script_protocol =
            api_test_find_named(protocols, "script_contracts");
        const struct json_value *swap_protocol =
            api_test_find_named(protocols, "atomic_swaps");
        ok = ok && protocols && protocols->type == JSON_ARR &&
             json_size(protocols) >= 7;
        ok = ok && zlsp_protocol &&
             strcmp(json_get_str(json_get(zlsp_protocol, "status")),
                    "design") == 0;
        ok = ok && zlsp_protocol &&
             strcmp(json_get_str(json_get(zlsp_protocol, "family")),
                    "application_protocol_framework") == 0;
        ok = ok && zlsp_protocol &&
             strcmp(json_get_str(json_get(zlsp_protocol, "anchor_kind")),
                    "base_layer_transaction_contract") == 0;
        ok = ok && zlsp_protocol &&
             api_test_array_has_str(json_get(zlsp_protocol,
                                             "crud_capabilities"),
                                    "construct_transaction");
        ok = ok && zlsp_protocol &&
             api_test_array_has_str(json_get(zlsp_protocol,
                                             "ux_surfaces"),
                                    "agent_command_center");
        ok = ok && zslp_protocol &&
             strcmp(json_get_str(json_get(zslp_protocol, "schema")),
                    "zcl.application_protocol_contract.v1") == 0;
        ok = ok && zslp_protocol &&
             strcmp(json_get_str(json_get(zslp_protocol, "status")),
                    "active") == 0;
        ok = ok && zslp_protocol &&
             strcmp(json_get_str(json_get(zslp_protocol, "base_layer")),
                    "zclassic_l1") == 0;
        ok = ok && zslp_protocol &&
             strcmp(json_get_str(json_get(zslp_protocol, "family")),
                    "token") == 0;
        ok = ok && zslp_protocol &&
             strcmp(json_get_str(json_get(zslp_protocol, "anchor_kind")),
                    "op_return") == 0;
        ok = ok && zslp_protocol &&
             strcmp(json_get_str(json_get(zslp_protocol, "rest_resource")),
                    "/api/v1/zslp/tokens") == 0;
        ok = ok && zslp_protocol &&
             api_test_array_has_str(json_get(zslp_protocol,
                                             "crud_capabilities"),
                                    "read_item");
        ok = ok && zslp_protocol &&
             strcmp(json_get_str(json_get(zslp_protocol,
                                          "construction_status")),
                    "transaction_builders_active") == 0;
        ok = ok && zslp_protocol &&
             strcmp(json_get_str(json_get(zslp_protocol,
                                          "consensus_boundary")),
                    "interprets_or_constructs_valid_zcl_transactions_only")
             == 0;
        ok = ok && zslp_protocol &&
             api_test_array_has_str(json_get(zslp_protocol,
                                             "object_types"),
                                    "token_genesis");
        ok = ok && zslp_protocol &&
             strcmp(json_get_str(json_get(zslp_protocol,
                                          "projection_model")),
                    "confirmed_op_return_projection_at_served_frontier") == 0;
        ok = ok && znam_protocol &&
             api_test_array_has_str(json_get(znam_protocol, "object_types"),
                                    "service_record");
        ok = ok && znam_protocol &&
             api_test_array_has_str(json_get(znam_protocol, "ux_surfaces"),
                                    "identity_profile");
        ok = ok && znam_protocol &&
             strstr(json_get_str(json_get(znam_protocol, "crypto_model")),
                    "owner_authority") != NULL;
        ok = ok && script_protocol &&
             strstr(json_get_str(json_get(script_protocol, "crypto_model")),
                    "legacy_valid_zclassic_script") != NULL;
        ok = ok && znam_protocol &&
             strcmp(json_get_str(json_get(znam_protocol, "status")),
                    "active") == 0;
        ok = ok && znam_protocol &&
             strcmp(json_get_str(json_get(znam_protocol, "anchor")),
                    "OP_RETURN name registry transactions") == 0;
        ok = ok && script_protocol &&
             strcmp(json_get_str(json_get(script_protocol, "status")),
                    "in_progress") == 0;
        ok = ok && script_protocol &&
             strcmp(json_get_str(json_get(script_protocol, "anchor_kind")),
                    "standard_script") == 0;
        ok = ok && script_protocol &&
             strcmp(json_get_str(json_get(script_protocol,
                                          "mutation_authority")),
                    "operator_wallet_transaction") == 0;
        ok = ok && script_protocol &&
             strstr(json_get_str(json_get(script_protocol, "anchor")),
                    "HTLC atomic swaps") != NULL;
        ok = ok && swap_protocol &&
             strcmp(json_get_str(json_get(swap_protocol, "status")),
                    "in_progress") == 0;
        const struct json_value *resources = json_get(&root, "resources");
        const struct json_value *zslp_resource =
            api_test_find_named(resources, "zslp_tokens");
        const struct json_value *protocols_resource =
            api_test_find_named(resources, "protocols");
        ok = ok && resources && json_size(resources) >= 4;
        ok = ok && protocols_resource &&
             strcmp(json_get_str(json_get(protocols_resource, "collection")),
                    "/api/v1/protocols") == 0;
        ok = ok && protocols_resource &&
             strcmp(json_get_str(json_get(protocols_resource, "item")),
                    "/api/v1/protocols/{name}") == 0;
        ok = ok && zslp_resource &&
             strcmp(json_get_str(json_get(zslp_resource, "collection")),
                    "/api/v1/zslp/tokens") == 0;
        const struct json_value *routes = json_get(&root, "route_contracts");
        ok = ok && routes &&
             json_size(routes) == api_route_contract_count();
        ok = ok && json_get_int(json_get(&root, "route_contract_count")) ==
             (int64_t)api_route_contract_count();
        const struct json_value *hodl =
            api_test_find_contract(routes, "/api/v1/hodl");
        const struct json_value *wallet =
            api_test_find_contract(routes, "/api/v1/wallet");
        const struct json_value *zslp =
            api_test_find_contract(routes, "/api/v1/zslp/tokens");
        const struct json_value *protocols_route =
            api_test_find_contract(routes, "/api/v1/protocols");
        const struct json_value *protocol_show =
            api_test_find_contract(routes, "/api/v1/protocols/{name}");
        const struct json_value *names =
            api_test_find_contract(routes, "/api/v1/names/{name}");
        const struct json_value *legacy_name =
            api_test_find_contract(routes, "/api/v1/name/{name}");
        const struct json_value *swap_chains =
            api_test_find_contract(routes, "/api/v1/swaps/chains");
        const struct json_value *legacy_swap_chains =
            api_test_find_contract(routes, "/api/v1/swap_chains");
        const struct json_value *events =
            api_test_find_contract(routes, "/api/v1/events");
        const struct json_value *supply =
            api_test_find_contract(routes, "/api/v1/supply");
        const struct json_value *block_show =
            api_test_find_contract(routes, "/api/v1/blocks/{height_or_hash}");
        const struct json_value *legacy_block_show =
            api_test_find_contract(routes, "/api/v1/block/{height_or_hash}");
        const struct json_value *tx_show =
            api_test_find_contract(routes, "/api/v1/transactions/{txid}");
        const struct json_value *legacy_tx_show =
            api_test_find_contract(routes, "/api/v1/tx/{txid}");
        const struct json_value *address_show =
            api_test_find_contract(routes, "/api/v1/addresses/{address}");
        const struct json_value *legacy_address_show =
            api_test_find_contract(routes, "/api/v1/address/{address}");
        ok = ok && hodl && strcmp(json_get_str(json_get(hodl,
                                    "response_schema")),
                                  "zcl.hodl_wave.v1") == 0;
        ok = ok && hodl && strcmp(json_get_str(json_get(hodl,
                                    "error_schema")),
                                  "zcl.rest_error.v1") == 0;
        ok = ok && hodl && strcmp(json_get_str(json_get(hodl,
                                    "crud_operation")),
                                  "read") == 0;
        ok = ok && hodl && strcmp(json_get_str(json_get(hodl,
                                    "resource_scope")),
                                  "singleton") == 0;
        ok = ok && hodl && strcmp(json_get_str(json_get(hodl,
                                    "crud_name")),
                                  "read_singleton") == 0;
        ok = ok && hodl && json_get_bool(json_get(hodl,
                                                  "freshness_scoped"));
        ok = ok && wallet && json_get_bool(json_get(wallet, "private"));
        ok = ok && wallet && strcmp(json_get_str(json_get(wallet, "auth")),
                                    "operator_private") == 0;
        ok = ok && wallet && strcmp(json_get_str(json_get(wallet,
                                    "auth_policy")),
                                    "operator_private") == 0;
        ok = ok && hodl && json_get_bool(json_get(hodl,
                                    "gateway_auth_compatible"));
        ok = ok && hodl && strcmp(json_get_str(json_get(hodl,
                                    "preferred_service_auth")),
                                  "hash512_sha3_gost_commitments") == 0;
        const struct json_value *hodl_telemetry =
            json_get(hodl, "telemetry");
        ok = ok && hodl_telemetry &&
             strcmp(json_get_str(json_get(hodl_telemetry, "counter")),
                    "zcl_api_requests_total") == 0;
        ok = ok && hodl_telemetry &&
             strcmp(json_get_str(json_get(hodl_telemetry,
                                          "latency_histogram")),
                    "zcl_api_request_duration_seconds") == 0;
        const struct json_value *hodl_crypto =
            json_get(hodl, "crypto_policy");
        ok = ok && hodl_crypto &&
             strcmp(json_get_str(json_get(hodl_crypto,
                                          "service_auth_primary_digest")),
                    "SHA3-512") == 0;
        ok = ok && hodl_crypto &&
             strcmp(json_get_str(json_get(hodl_crypto,
                                          "service_auth_secondary_digest")),
                    "GOST R 34.11-2012-512") == 0;
        ok = ok && hodl_crypto &&
             json_get_int(json_get(hodl_crypto, "hash_output_bits")) == 512;
        ok = ok && hodl_crypto &&
             json_get_bool(json_get(hodl_crypto, "requires_all_digests"));
        ok = ok && hodl_crypto &&
             !json_get_bool(json_get(hodl_crypto,
                                     "signature_scheme_claimed"));
        ok = ok && zslp && api_test_contract_has_query(zslp, "limit");
        ok = ok && zslp && json_get_bool(json_get(zslp, "pagination"));
        ok = ok && zslp && strcmp(json_get_str(json_get(zslp,
                                    "crud_name")),
                                  "read_collection") == 0;
        ok = ok && zslp &&
             strcmp(json_get_str(json_get(zslp,
                                    "application_protocol")),
                    "zslp") == 0;
        ok = ok && zslp &&
             strcmp(json_get_str(json_get(zslp, "base_layer")),
                    "zclassic_l1") == 0;
        ok = ok && zslp &&
             strcmp(json_get_str(json_get(zslp, "protocol_family")),
                    "token") == 0;
        ok = ok && zslp &&
             strcmp(json_get_str(json_get(zslp, "protocol_anchor_kind")),
                    "op_return") == 0;
        ok = ok && zslp &&
             api_test_array_has_str(json_get(zslp, "protocol_crud"),
                                    "read_collection");
        ok = ok && zslp &&
             strcmp(json_get_str(json_get(zslp,
                                    "protocol_construction_status")),
                    "transaction_builders_active") == 0;
        ok = ok && zslp &&
             strcmp(json_get_str(json_get(zslp,
                                    "mutation_authority")),
                    "operator_wallet_transaction") == 0;
        ok = ok && zslp &&
             strcmp(json_get_str(json_get(zslp, "layer")),
                    "zclassic23_application_layer") == 0;
        ok = ok && zslp &&
             strcmp(json_get_str(json_get(zslp, "source_anchor")),
                    "OP_RETURN token transactions") == 0;
        ok = ok && protocols_route &&
             strcmp(json_get_str(json_get(protocols_route,
                                    "response_schema")),
                    "zcl.application_protocols.index.v1") == 0;
        ok = ok && protocols_route &&
             strcmp(json_get_str(json_get(protocols_route,
                                    "application_protocol")),
                    "zlsp") == 0;
        ok = ok && protocols_route &&
             strcmp(json_get_str(json_get(protocols_route,
                                    "protocol_family")),
                    "application_protocol_framework") == 0;
        ok = ok && protocol_show &&
             strcmp(json_get_str(json_get(protocol_show, "crud_name")),
                    "read_item") == 0;
        ok = ok && protocol_show &&
             api_test_contract_has_id_param(protocol_show, "name");
        ok = ok && events && api_test_contract_has_query(events, "limit");
        ok = ok && events && api_test_contract_has_query(events, "type");
        ok = ok && events && strcmp(json_get_str(json_get(events,
                                    "resource_scope")),
                                    "collection") == 0;
        ok = ok && events && strcmp(json_get_str(json_get(events,
                                    "freshness")),
                                    "event_projection") == 0;
        ok = ok && supply && strcmp(json_get_str(json_get(supply,
                                    "response_schema")),
                                    "zcl.supply.v1") == 0;
        ok = ok && supply && strcmp(json_get_str(json_get(supply,
                                    "compat_response_schema")),
                                    "zcl.supply_legacy_number.v1") == 0;
        ok = ok && block_show && json_get_bool(json_get(block_show,
                                                        "canonical"));
        ok = ok && block_show &&
             strcmp(json_get_str(json_get(block_show, "crud_name")),
                    "read_item") == 0;
        ok = ok && block_show &&
             api_test_contract_has_id_param(block_show, "height_or_hash");
        ok = ok && legacy_block_show &&
             strcmp(json_get_str(json_get(legacy_block_show,
                                          "legacy_alias_of")),
                    "/api/v1/blocks/{height_or_hash}") == 0;
        ok = ok && tx_show && strcmp(json_get_str(json_get(tx_show,
                                    "response_schema")),
                                    "zcl.transactions.show.v1") == 0;
        ok = ok && legacy_tx_show &&
             strcmp(json_get_str(json_get(legacy_tx_show,
                                          "legacy_alias_of")),
                    "/api/v1/transactions/{txid}") == 0;
        ok = ok && address_show &&
             strcmp(json_get_str(json_get(address_show, "freshness")),
                    "utxo_projection") == 0;
        ok = ok && legacy_address_show &&
             strcmp(json_get_str(json_get(legacy_address_show,
                                          "legacy_alias_of")),
                    "/api/v1/addresses/{address}") == 0;
        ok = ok && names && json_get_bool(json_get(names, "canonical"));
        ok = ok && names && strcmp(json_get_str(json_get(names,
                                    "resource_scope")),
                                   "item") == 0;
        ok = ok && names &&
             strcmp(json_get_str(json_get(names,
                                    "application_protocol")),
                    "znam") == 0;
        ok = ok && names &&
             api_test_array_has_str(json_get(names,
                                             "protocol_object_types"),
                                    "service_record");
        ok = ok && names &&
             api_test_array_has_str(json_get(names,
                                             "protocol_ux_surfaces"),
                                    "identity_profile");
        ok = ok && names &&
             strcmp(json_get_str(json_get(names, "reorg_model")),
                    "rebuild_name_state_from_confirmed_chain_after_disconnect")
             == 0;
        ok = ok && names &&
             strstr(json_get_str(json_get(names, "privacy_model")),
                    "public") != NULL;
        ok = ok && names && api_test_contract_has_id_param(names, "name");
        ok = ok && legacy_name &&
             !json_get_bool(json_get(legacy_name, "canonical"));
        ok = ok && legacy_name &&
             strcmp(json_get_str(json_get(legacy_name, "legacy_alias_of")),
                    "/api/v1/names/{name}") == 0;
        ok = ok && swap_chains && json_get_bool(json_get(swap_chains,
                                                         "canonical"));
        ok = ok && swap_chains &&
             strcmp(json_get_str(json_get(swap_chains,
                                    "application_protocol")),
                    "script_contracts") == 0;
        ok = ok && swap_chains &&
             strstr(json_get_str(json_get(swap_chains,
                                          "source_anchor")),
                    "HTLC atomic swaps") != NULL;
        ok = ok && swap_chains &&
             api_test_array_has_str(json_get(swap_chains,
                                             "protocol_object_types"),
                                    "contract_template");
        ok = ok && swap_chains &&
             strstr(json_get_str(json_get(swap_chains, "crypto_model")),
                    "hashlocks_timelocks") != NULL;
        ok = ok && legacy_swap_chains &&
             strcmp(json_get_str(json_get(legacy_swap_chains,
                                          "legacy_alias_of")),
                    "/api/v1/swaps/chains") == 0;
        ok = ok && strcmp(json_get_str(json_get(json_get(&root, "mcp"),
                                                "first_tool")),
                          "zcl_agent") == 0;
        ok = ok && strcmp(json_get_str(json_get(json_get(&root, "mcp"),
                                                "api_tool")),
                          "zcl_openapi") == 0;
        ok = ok && strcmp(json_get_str(json_get(json_get(&root, "mcp"),
                                                "app_protocols_tool")),
                          "zcl_app_protocols") == 0;
        ok = ok && strcmp(json_get_str(json_get(json_get(&root, "mcp"),
                                                "drilldown_tool")),
                          "zcl_health") == 0;
        ok = ok && strcmp(json_get_str(json_get(json_get(&root, "mcp"),
                                                "milestone_tool")),
                          "zcl_milestone") == 0;
        ok = ok && strcmp(json_get_str(json_get(json_get(&root, "mcp"),
                                                "refold_tool")),
                          "zcl_refold_status") == 0;
        ok = ok && strcmp(json_get_str(json_get(json_get(&root, "cli"),
                                                "api_command")),
                          "zclassic23 api") == 0;
        ok = ok && strcmp(json_get_str(json_get(json_get(&root, "cli"),
                                                "app_protocols_command")),
                          "zclassic23 appprotocols") == 0;
        ok = ok && strcmp(json_get_str(json_get(json_get(&root, "cli"),
                                                "first_command")),
                          "zclassic23 agent") == 0;
        ok = ok && strcmp(json_get_str(json_get(json_get(&root, "cli"),
                                                "drilldown_command")),
                          "zclassic23 healthcheck") == 0;
        ok = ok && strcmp(json_get_str(json_get(json_get(&root, "cli"),
                                                "milestone_command")),
                          "zclassic23 milestone") == 0;
        ok = ok && strcmp(json_get_str(json_get(json_get(&root, "cli"),
                                                "refold_command")),
                          "zclassic23 refold") == 0;
        ok = ok && json_get(json_get(&root, "cli"),
                            "compat_command") == NULL;
        json_free(&root);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: protocol registry endpoints expose overlay contracts... ");
    {
        static uint8_t protocols_resp[262144];
        size_t n = api_handle_request("GET", "/api/v1/protocols", NULL, 0,
                                      protocols_resp, sizeof(protocols_resp));
        const char *body = api_test_body(protocols_resp, n,
                                         sizeof(protocols_resp));
        struct json_value root;
        json_init(&root);
        bool ok = n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.application_protocols.index.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "base_layer")),
                          "zclassic_l1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "service_layer")),
                          "zclassic23_application_layer") == 0;
        const struct json_value *protocols = json_get(&root, "protocols");
        ok = ok && protocols && json_get_int(json_get(&root,
                          "protocol_count")) == (int64_t)json_size(protocols);
        const struct json_value *zslp =
            api_test_find_named(protocols, "zslp");
        const struct json_value *znam =
            api_test_find_named(protocols, "znam");
        const struct json_value *market =
            api_test_find_named(protocols, "market");
        ok = ok && zslp &&
             api_test_array_has_str(json_get(zslp, "crud_capabilities"),
                                    "read_collection");
        ok = ok && zslp &&
             strcmp(json_get_str(json_get(zslp, "anchor_kind")),
                    "op_return") == 0;
        ok = ok && zslp &&
             api_test_array_has_str(json_get(zslp, "object_types"),
                                    "token_transfer");
        ok = ok && znam &&
             api_test_array_has_str(json_get(znam, "ux_surfaces"),
                                    "node_service_directory");
        ok = ok && market &&
             api_test_array_has_str(json_get(market, "object_types"),
                                    "content_descriptor");
        ok = ok && market &&
             strstr(json_get_str(json_get(market, "transport_model")),
                    "direct_p2p") != NULL;
        json_free(&root);

        n = api_handle_request("GET", "/api/v1/protocols/script_contracts",
                               NULL, 0, protocols_resp,
                               sizeof(protocols_resp));
        body = api_test_body(protocols_resp, n, sizeof(protocols_resp));
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.application_protocol_contract.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "name")),
                          "script_contracts") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "anchor_kind")),
                          "standard_script") == 0;
        ok = ok && api_test_array_has_str(json_get(&root,
                          "crud_capabilities"), "construct_contract");
        ok = ok && strcmp(json_get_str(json_get(&root,
                          "construction_status")),
                          "htlc_builders_active_settlement_in_progress") == 0;
        ok = ok && api_test_array_has_str(json_get(&root,
                          "object_types"), "contract_template");
        ok = ok && api_test_array_has_str(json_get(&root,
                          "ux_surfaces"), "script_preview");
        ok = ok && strstr(json_get_str(json_get(&root, "crypto_model")),
                          "legacy_valid_zclassic_script") != NULL;
        json_free(&root);

        n = api_handle_request("GET", "/api/v1/protocols/not_real",
                               NULL, 0, protocols_resp,
                               sizeof(protocols_resp));
        protocols_resp[n < sizeof(protocols_resp) ? n :
                       sizeof(protocols_resp) - 1] = '\0';
        ok = ok && strstr((char *)protocols_resp,
                          "HTTP/1.1 404 Not Found") != NULL;

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: OpenAPI document is generated from route contracts... ");
    {
        static uint8_t openapi_resp[262144];
        size_t n = api_handle_request("GET", "/api/v1/openapi", NULL, 0,
                                      openapi_resp, sizeof(openapi_resp));
        const char *body = api_test_body(openapi_resp, n,
                                         sizeof(openapi_resp));
        struct json_value root;
        json_init(&root);
        bool ok = n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.openapi.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "openapi")),
                          "3.1.0") == 0;
        ok = ok && strcmp(json_get_str(json_get(json_get(&root, "info"),
                                                "version")),
                          "v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(json_at(json_get(&root,
                                      "servers"), 0), "url")),
                          "/api/v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(json_get(&root,
                          "x-zcl-crypto-policy"),
                          "service_auth_primary_digest")),
                          "SHA3-512") == 0;
        ok = ok && strcmp(json_get_str(json_get(json_get(&root,
                          "x-zcl-crypto-policy"),
                          "service_auth_secondary_digest")),
                          "GOST R 34.11-2012-512") == 0;
        const struct json_value *openapi_layer =
            json_get(&root, "x-zcl-layer-model");
        ok = ok && openapi_layer &&
             strcmp(json_get_str(json_get(openapi_layer, "schema")),
                    "zcl.rest_layer_model.v1") == 0;
        ok = ok && openapi_layer &&
             strcmp(json_get_str(json_get(openapi_layer, "base_layer")),
                    "zclassic_l1") == 0;
        ok = ok && openapi_layer &&
             strcmp(json_get_str(json_get(openapi_layer,
                                          "application_protocol_umbrella")),
                    "zlsp") == 0;
        ok = ok && openapi_layer &&
             api_test_find_named(json_get(openapi_layer,
                                          "application_protocols"),
                                 "zlsp") != NULL;
        ok = ok && openapi_layer &&
             api_test_find_named(json_get(openapi_layer,
                                          "application_protocols"),
                                 "zslp") != NULL;
        ok = ok && openapi_layer &&
             api_test_find_named(json_get(openapi_layer,
                                          "application_protocols"),
                                 "znam") != NULL;
        ok = ok && openapi_layer &&
             api_test_find_named(json_get(openapi_layer,
                                          "application_protocols"),
                                 "script_contracts") != NULL;
        ok = ok && json_get_int(json_get(&root,
                          "x-route-contract-count")) ==
                          (int64_t)api_route_contract_count();

        const struct json_value *hodl =
            api_test_openapi_get(&root, "/api/v1/hodl");
        const struct json_value *wallet =
            api_test_openapi_get(&root, "/api/v1/wallet");
        const struct json_value *events =
            api_test_openapi_get(&root, "/api/v1/events");
        const struct json_value *zslp =
            api_test_openapi_get(&root, "/api/v1/zslp/tokens");
        const struct json_value *protocols =
            api_test_openapi_get(&root, "/api/v1/protocols");
        const struct json_value *protocol_show =
            api_test_openapi_get(&root, "/api/v1/protocols/{name}");
        const struct json_value *names =
            api_test_openapi_get(&root, "/api/v1/names/{name}");
        const struct json_value *swap_chains =
            api_test_openapi_get(&root, "/api/v1/swaps/chains");
        const struct json_value *block_show =
            api_test_openapi_get(&root, "/api/v1/blocks/{height_or_hash}");
        const struct json_value *legacy_name =
            api_test_openapi_get(&root, "/api/v1/name/{name}");
        const struct json_value *supply =
            api_test_openapi_get(&root, "/api/v1/supply");
        const struct json_value *openapi =
            api_test_openapi_get(&root, "/api/v1/openapi");

        ok = ok && hodl &&
             strcmp(json_get_str(json_get(hodl, "x-resource")),
                    "hodl") == 0;
        ok = ok && hodl &&
             strcmp(json_get_str(json_get(hodl, "x-crud-operation")),
                    "read") == 0;
        ok = ok && hodl &&
             strcmp(json_get_str(json_get(hodl, "x-resource-scope")),
                    "singleton") == 0;
        ok = ok && hodl &&
             strcmp(json_get_str(json_get(hodl, "x-crud-name")),
                    "read_singleton") == 0;
        ok = ok && hodl &&
             strcmp(json_get_str(json_get(hodl, "x-response-schema")),
                    "zcl.hodl_wave.v1") == 0;
        ok = ok && hodl &&
             strcmp(json_get_str(json_get(hodl, "x-auth-policy")),
                    "public") == 0;
        ok = ok && hodl &&
             strcmp(json_get_str(json_get(json_get(hodl,
                    "x-zcl-telemetry"), "counter")),
                    "zcl_api_requests_total") == 0;
        ok = ok && hodl &&
             strcmp(json_get_str(json_get(json_get(hodl,
                    "x-zcl-crypto-policy"), "service_auth_primary_digest")),
                    "SHA3-512") == 0;
        ok = ok && hodl &&
             strcmp(json_get_str(json_get(json_get(hodl,
                    "x-zcl-crypto-policy"),
                    "service_auth_secondary_digest")),
                    "GOST R 34.11-2012-512") == 0;
        const struct json_value *hodl_200 =
            json_get(json_get(hodl, "responses"), "200");
        ok = ok && strcmp(json_get_str(json_get(json_get(json_get(
            json_get(hodl_200, "content"), "application/json"), "schema"),
            "$ref")), "#/components/schemas/zcl.hodl_wave.v1") == 0;

        ok = ok && wallet && json_get_bool(json_get(wallet, "x-private"));
        ok = ok && wallet && json_size(json_get(wallet, "security")) == 1;
        ok = ok && events &&
             api_test_openapi_has_param(events, "limit", "query");
        ok = ok && events &&
             api_test_openapi_has_param(events, "type", "query");
        ok = ok && events &&
             strcmp(json_get_str(json_get(events, "x-crud-name")),
                    "read_collection") == 0;
        ok = ok && zslp &&
             strcmp(json_get_str(json_get(zslp,
                    "x-zcl-application-protocol")), "zslp") == 0;
        ok = ok && zslp &&
             strcmp(json_get_str(json_get(zslp, "x-zcl-base-layer")),
                    "zclassic_l1") == 0;
        ok = ok && zslp &&
             strcmp(json_get_str(json_get(zslp, "x-zcl-layer")),
                    "zclassic23_application_layer") == 0;
        ok = ok && zslp &&
             strcmp(json_get_str(json_get(zslp,
                    "x-zcl-protocol-family")), "token") == 0;
        ok = ok && zslp &&
             strcmp(json_get_str(json_get(zslp,
                    "x-zcl-protocol-anchor-kind")), "op_return") == 0;
        ok = ok && zslp &&
             api_test_array_has_str(json_get(zslp, "x-zcl-protocol-crud"),
                                    "read_collection");
        ok = ok && zslp &&
             api_test_array_has_str(json_get(zslp,
                    "x-zcl-protocol-object-types"), "token_genesis");
        ok = ok && zslp &&
             strcmp(json_get_str(json_get(zslp,
                    "x-zcl-protocol-construction-status")),
                    "transaction_builders_active") == 0;
        ok = ok && zslp &&
             strcmp(json_get_str(json_get(zslp,
                    "x-zcl-mutation-authority")),
                    "operator_wallet_transaction") == 0;
        ok = ok && protocols &&
             strcmp(json_get_str(json_get(protocols, "x-response-schema")),
                    "zcl.application_protocols.index.v1") == 0;
        ok = ok && protocol_show &&
             strcmp(json_get_str(json_get(protocol_show, "x-crud-name")),
                    "read_item") == 0;
        ok = ok && protocol_show &&
             api_test_openapi_has_param(protocol_show, "name", "path");
        ok = ok && names &&
             strcmp(json_get_str(json_get(names,
                    "x-zcl-application-protocol")), "znam") == 0;
        ok = ok && names &&
             api_test_array_has_str(json_get(names,
                    "x-zcl-protocol-object-types"), "service_record");
        ok = ok && names &&
             api_test_array_has_str(json_get(names,
                    "x-zcl-protocol-ux-surfaces"), "identity_profile");
        ok = ok && names &&
             strcmp(json_get_str(json_get(names, "x-zcl-reorg-model")),
                    "rebuild_name_state_from_confirmed_chain_after_disconnect")
             == 0;
        ok = ok && names &&
             strstr(json_get_str(json_get(names, "x-zcl-privacy-model")),
                    "public") != NULL;
        ok = ok && swap_chains &&
             strcmp(json_get_str(json_get(swap_chains,
                    "x-zcl-application-protocol")),
                    "script_contracts") == 0;
        ok = ok && swap_chains &&
             strstr(json_get_str(json_get(swap_chains,
                    "x-zcl-source-anchor")), "HTLC atomic swaps") != NULL;
        ok = ok && swap_chains &&
             strstr(json_get_str(json_get(swap_chains,
                    "x-zcl-crypto-model")),
                    "legacy_valid_zclassic_script") != NULL;
        ok = ok && block_show &&
             api_test_openapi_has_param(block_show, "height_or_hash", "path");
        ok = ok && block_show &&
             strcmp(json_get_str(json_get(block_show, "x-crud-name")),
                    "read_item") == 0;
        ok = ok && block_show &&
             strcmp(json_get_str(json_at(json_get(block_show,
                                                  "x-id-params"), 0)),
                    "height_or_hash") == 0;
        ok = ok && legacy_name &&
             strcmp(json_get_str(json_get(legacy_name,
                                          "x-legacy-alias-of")),
                    "/api/v1/names/{name}") == 0;
        ok = ok && supply &&
             strcmp(json_get_str(json_get(supply,
                                          "x-compat-response-schema")),
                    "zcl.supply_legacy_number.v1") == 0;
        ok = ok && openapi &&
             strcmp(json_get_str(json_get(openapi, "x-response-schema")),
                    "zcl.openapi.v1") == 0;

        const struct json_value *components = json_get(&root, "components");
        const struct json_value *schemas = json_get(components, "schemas");
        const struct json_value *security =
            json_get(components, "securitySchemes");
        const struct json_value *error_schema =
            json_get(schemas, "zcl.rest_error.v1");
        const struct json_value *error_properties =
            json_get(error_schema, "properties");
        ok = ok && json_get(schemas, "zcl.wallet_status.v1") != NULL;
        ok = ok && error_schema != NULL;
        ok = ok && strcmp(json_get_str(json_get(error_schema, "type")),
                          "object") == 0;
        ok = ok && json_get(error_properties, "schema") != NULL;
        ok = ok && json_get(error_properties, "api_version") != NULL;
        ok = ok && json_get(error_properties, "error") != NULL;
        ok = ok && json_size(json_get(error_schema, "required")) == 3;
        ok = ok && strcmp(json_get_str(json_get(json_get(schemas,
                                      "zcl.supply_legacy_number.v1"),
                                      "type")),
                          "number") == 0;
        ok = ok && json_get(security, "operatorAuth") != NULL;
        ok = ok && json_get(security, "serviceHash512Auth") != NULL;
        json_free(&root);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: milestone endpoint returns node-computed ASCII bars... ");
    {
        size_t n = api_handle_request("GET", "/api/v1/milestone", NULL, 0,
                                      resp, sizeof(resp));
        const char *body = api_test_body(resp, n, sizeof(resp));
        struct json_value root;
        json_init(&root);
        bool ok = n > 0 && body && json_read(&root, body, strlen(body));
        const struct json_value *ascii = json_get(&root, "ascii");
        const struct json_value *bars = json_get(&root, "bars");
        const struct json_value *criteria = json_get(&root, "criteria");
        const struct json_value *live = json_get(&root, "live");
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.milestone_status.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "milestone")),
                          "v1 MVP") == 0;
        ok = ok && json_get_int(json_get(&root,
                          "mvp_readiness_score")) == 4;
        ok = ok && json_get_int(json_get(&root, "target_score")) == 8;
        ok = ok && ascii && strstr(json_get_str(json_get(ascii, "goals")),
                                   "goals [#####-----] 4/8") != NULL;
        ok = ok && bars && strcmp(json_get_str(json_get(json_get(bars,
                          "subgoals"), "bar")), "[########--]") == 0;
        ok = ok && criteria && json_size(criteria) == 8;
        ok = ok && live && strcmp(json_get_str(json_get(live, "source")),
                                  "agent_cached_summary") == 0;
        ok = ok && strcmp(json_get_str(json_get(live, "source_schema")),
                          "zcl.public_status.v1") == 0;
        ok = ok && json_get(live, "agent_status") != NULL;
        ok = ok && json_get(live, "readiness_status") != NULL;
        ok = ok && json_get(live, "height_contract_status") != NULL;
        json_free(&root);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: refold endpoint reports anchor readiness... ");
    {
        size_t n = api_handle_request("GET", "/api/v1/refold", NULL, 0,
                                      resp, sizeof(resp));
        const char *body = api_test_body(resp, n, sizeof(resp));
        struct json_value root;
        json_init(&root);
        bool ok = n > 0 && body && json_read(&root, body, strlen(body));
        const struct json_value *snap = json_get(&root, "anchor_snapshot");
        const struct json_value *commands = json_get(&root, "commands");
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.refold_status.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "api_version")),
                          "v1") == 0;
        ok = ok && strstr(json_get_str(json_get(&root, "purpose")),
                          "UTXO anchor rebuild") != NULL;
        ok = ok && strstr(json_get_str(json_get(&root, "plain_english")),
                          "borrowed snapshot seed") != NULL;
        ok = ok && strcmp(json_get_str(json_get(&root,
                                                "internal_mechanism")),
                          "-refold-from-anchor") == 0;
        ok = ok && !json_get_bool(json_get(&root, "ready_for_refold"));
        ok = ok && strcmp(json_get_str(json_get(&root, "primary_blocker")),
                          "missing_verified_anchor_snapshot") == 0;
        ok = ok && snap && json_get(snap, "path") != NULL;
        ok = ok && commands &&
             strcmp(json_get_str(json_get(commands, "native")),
                    "zclassic23 refold") == 0;
        json_free(&root);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: unsupported version reports supported versions... ");
    {
        size_t n = api_handle_request("GET", "/api/v2/agent", NULL, 0,
                                      resp, sizeof(resp));
        const char *body = api_test_body(resp, n, sizeof(resp));
        struct json_value root;
        json_init(&root);
        bool ok = n > 0 && strstr((char *)resp,
                                  "HTTP/1.1 400 Bad Request") != NULL;
        ok = ok && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.rest_error.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "api_version")),
                          "v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "error")),
                          "unsupported_api_version") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root,
                          "requested_version")), "v2") == 0;
        const struct json_value *supported =
            ok ? json_get(&root, "supported_versions") : NULL;
        ok = ok && json_size(supported) == 1;
        ok = ok && strcmp(json_get_str(json_at(supported, 0)), "v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "base_path")),
                          "/api/v1") == 0;
        json_free(&root);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: resource route table exposes controller-style names... ");
    {
        bool saw_agent = false;
        bool saw_milestone = false;
        bool saw_refold = false;
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
            if (strcmp(resource, "milestone") == 0 &&
                strcmp(action, "show") == 0)
                saw_milestone = true;
            if (strcmp(resource, "refold") == 0 &&
                strcmp(action, "show") == 0)
                saw_refold = true;
            if (strcmp(resource, "blocks") == 0 &&
                strcmp(action, "index") == 0)
                saw_blocks = true;
            if (strcmp(resource, "factoids") == 0 &&
                strcmp(action, "show") == 0)
                saw_factoids = true;
        }
        bool ok = count >= 18 && saw_agent && saw_milestone && saw_refold &&
                  saw_blocks && saw_factoids;
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: dynamic resource routes expose REST member metadata... ");
    {
        bool saw_block = false;
        bool saw_legacy_block = false;
        bool saw_tx = false;
        bool saw_legacy_tx = false;
        bool saw_address = false;
        bool saw_legacy_address = false;
        bool saw_token = false;
        bool saw_transfer = false;
        bool saw_file = false;
        bool saw_name = false;
        bool saw_legacy_name = false;
        size_t count = api_dynamic_resource_route_count();
        for (size_t i = 0; i < count; i++) {
            const char *pattern = api_dynamic_resource_route_pattern_at(i);
            const char *resource = api_dynamic_resource_route_resource_at(i);
            const char *action = api_dynamic_resource_route_action_at(i);
            if (!pattern || !resource || !action)
                continue;
            if (strcmp(pattern, "/api/blocks/{height_or_hash}") == 0 &&
                strcmp(resource, "blocks") == 0 &&
                strcmp(action, "show") == 0)
                saw_block = true;
            if (strcmp(pattern, "/api/block/{height_or_hash}") == 0 &&
                strcmp(resource, "blocks") == 0 &&
                strcmp(action, "show") == 0)
                saw_legacy_block = true;
            if (strcmp(pattern, "/api/transactions/{txid}") == 0 &&
                strcmp(resource, "transactions") == 0 &&
                strcmp(action, "show") == 0)
                saw_tx = true;
            if (strcmp(pattern, "/api/tx/{txid}") == 0 &&
                strcmp(resource, "transactions") == 0 &&
                strcmp(action, "show") == 0)
                saw_legacy_tx = true;
            if (strcmp(pattern, "/api/addresses/{address}") == 0 &&
                strcmp(resource, "addresses") == 0 &&
                strcmp(action, "show") == 0)
                saw_address = true;
            if (strcmp(pattern, "/api/address/{address}") == 0 &&
                strcmp(resource, "addresses") == 0 &&
                strcmp(action, "show") == 0)
                saw_legacy_address = true;
            if (strcmp(pattern, "/api/zslp/tokens/{token_id}") == 0 &&
                strcmp(resource, "zslp_tokens") == 0 &&
                strcmp(action, "show") == 0)
                saw_token = true;
            if (strcmp(pattern, "/api/zslp/tokens/{token_id}/transfers") == 0 &&
                strcmp(resource, "zslp_token_transfers") == 0 &&
                strcmp(action, "index") == 0)
                saw_transfer = true;
            if (strcmp(pattern, "/api/files/{sha3}") == 0 &&
                strcmp(resource, "files") == 0 &&
                strcmp(action, "show") == 0)
                saw_file = true;
            if (strcmp(pattern, "/api/names/{name}") == 0 &&
                strcmp(resource, "names") == 0 &&
                strcmp(action, "show") == 0)
                saw_name = true;
            if (strcmp(pattern, "/api/name/{name}") == 0 &&
                strcmp(resource, "names") == 0 &&
                strcmp(action, "show") == 0)
                saw_legacy_name = true;
        }
        bool ok = count >= 16 && saw_block && saw_legacy_block && saw_tx &&
                  saw_legacy_tx && saw_address && saw_legacy_address &&
                  saw_token && saw_transfer && saw_file && saw_name &&
                  saw_legacy_name;
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: lookup resources emit REST schemas and freshness... ");
    {
        reducer_frontier_provable_tip_reset();
        reducer_frontier_provable_tip_set(11);
        api_test_set_rpc_call(api_test_lookup_rpc);

        size_t n = api_handle_request("GET", "/api/v1/blocks/10", NULL, 0,
                                      resp, sizeof(resp));
        const char *body = api_test_body(resp, n, sizeof(resp));
        struct json_value root;
        json_init(&root);
        bool ok = n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.blocks.show.v1") == 0;
        ok = ok && api_test_expect_freshness(&root, "served_height",
                                             11, 11, true);
        ok = ok && json_get_int(json_get(&root, "height")) == 10;
        ok = ok && json_get_int(json_get(&root, "num_tx")) == 2;
        ok = ok && json_get_int(json_get(&root, "tx_returned")) == 2;
        ok = ok && !json_get_bool(json_get(&root, "tx_truncated"));
        ok = ok && strcmp(json_get_str(json_at(json_get(&root, "tx"), 0)),
                          API_TEST_TXID) == 0;
        json_free(&root);

        n = api_handle_request("GET", "/api/v1/block/10", NULL, 0,
                               resp, sizeof(resp));
        body = api_test_body(resp, n, sizeof(resp));
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.blocks.show.v1") == 0;
        ok = ok && json_get_int(json_get(&root, "height")) == 10;
        json_free(&root);

        n = api_handle_request("GET",
                               "/api/v1/transactions/" API_TEST_TXID,
                               NULL, 0, resp, sizeof(resp));
        body = api_test_body(resp, n, sizeof(resp));
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.transactions.show.v1") == 0;
        ok = ok && api_test_expect_freshness(&root, "served_height",
                                             11, 11, true);
        ok = ok && strcmp(json_get_str(json_get(&root, "txid")),
                          API_TEST_TXID) == 0;
        const struct json_value *vout = json_get(&root, "vout");
        const struct json_value *vin = json_get(&root, "vin");
        ok = ok && json_get_int(json_get(&root, "vout_returned")) == 1;
        ok = ok && strcmp(json_get_str(json_get(json_at(vout, 0),
                                                "address")),
                          API_TEST_ADDR) == 0;
        ok = ok && json_get_int(json_get(json_at(vin, 0), "vout")) == 1;
        json_free(&root);

        n = api_handle_request("GET", "/api/v1/addresses/" API_TEST_ADDR,
                               NULL, 0, resp, sizeof(resp));
        body = api_test_body(resp, n, sizeof(resp));
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.addresses.show.v1") == 0;
        ok = ok && api_test_expect_freshness(&root, "utxo_projection",
                                             11, 11, true);
        ok = ok && json_get_int(json_get(&root, "balance_sat")) ==
                  123456789;
        ok = ok && json_get_int(json_get(&root, "utxo_count")) == 1;
        const struct json_value *utxos = json_get(&root, "utxos");
        ok = ok && strcmp(json_get_str(json_get(json_at(utxos, 0),
                                                "txid")),
                          API_TEST_TXID) == 0;
        json_free(&root);

        n = api_handle_request("GET", "/api/v1/address/" API_TEST_ADDR,
                               NULL, 0, resp, sizeof(resp));
        body = api_test_body(resp, n, sizeof(resp));
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.addresses.show.v1") == 0;
        ok = ok && json_get_int(json_get(&root, "balance_sat")) ==
                  123456789;
        json_free(&root);

        n = api_handle_request("GET", "/api/tx/" API_TEST_TXID,
                               NULL, 0, resp, sizeof(resp));
        body = api_test_body(resp, n, sizeof(resp));
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.transactions.show.v1") == 0;
        json_free(&root);

        api_test_set_rpc_call(NULL);
        reducer_frontier_provable_tip_reset();

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: events resource emits REST envelope and freshness... ");
    {
        event_log_init();
        progress_store_close();
        reducer_frontier_provable_tip_reset();
        api_set_state(NULL, NULL, NULL, NULL, NULL);
        event_emitf(EV_NODE_READY, 0, "height=4 peers=1");

        size_t n = api_handle_request("GET",
                                      "/api/events?limit=5&type=sys.",
                                      NULL, 0, resp, sizeof(resp));
        const char *body = api_test_body(resp, n, sizeof(resp));
        struct json_value root;
        json_init(&root);
        bool ok = n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.events.index.v1") == 0;
        ok = ok && api_test_expect_freshness(&root, "event_projection",
                                             0, 0, true);
        ok = ok && strcmp(json_get_str(json_get(&root, "type")),
                          "sys.") == 0;
        ok = ok && json_get_int(json_get(&root, "limit")) == 5;
        const struct json_value *events = json_get(&root, "events");
        ok = ok && events && json_size(events) == 1;
        ok = ok && strcmp(json_get_str(json_get(json_at(events, 0),
                                                "type")),
                          "sys.ready") == 0;
        json_free(&root);

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
        const char *body = api_test_body(resp, n, sizeof(resp));
        struct json_value root;
        json_init(&root);
        ok = ok && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.node_status.v1") == 0;
        ok = ok && api_test_expect_freshness(&root, "served_tip",
                                             1, 1, true);
        const struct json_value *chain = json_get(&root, "chain");
        ok = ok && chain != NULL;
        ok = ok && json_get_int(json_get(chain, "tip_height")) == 1;
        ok = ok && strcmp(json_get_str(json_get(chain, "tip_hash")),
                          hstar_hex) == 0;
        const struct json_value *errors = json_get(&root, "errors");
        ok = ok && errors && json_get(errors, "recent") != NULL;
        json_free(&root);

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
        rpc_agent_set_boot_context("canonical", "full",
                                   "/tmp/zcl-canonical", 18232, 8033,
                                   8443, 18034);
        api_set_state(&ms, NULL, NULL, NULL, "/tmp");

        size_t n = api_handle_request("GET", "/api/status", NULL, 0,
                                      resp, sizeof(resp));
        const char *body = api_test_body(resp, n, sizeof(resp));
        struct json_value root;
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.public_status.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "build_commit")),
                          zcl_build_commit()) == 0;
        ok = ok && api_test_expect_freshness(&root, "served_tip",
                                             2, 2, true);
        ok = ok && json_get(&root, "status") != NULL;
        ok = ok && json_get(&root, "height") != NULL;
        ok = ok && json_get(&root, "recommended_endpoints") != NULL;
        ok = ok && api_test_expect_readiness_shape(&root);
        ok = ok && api_test_expect_security_posture_shape(&root);
        ok = ok && api_test_expect_lane_safety_fields(
            &root, "canonical", false, false, true, "dev",
            "observe_only_or_use_dev_lane");
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
        struct cac_decision decision;
        bool ok = api_test_build_chain(&ms, blocks, 3);
        memset(&decision, 0, sizeof(decision));
        decision.result = CAC_DECISION_USE_SOURCE;
        decision.selected_source = CAC_SOURCE_P2P;
        decision.local_height = 2;
        decision.target_height = 2;
        decision.projection_height = 2;
        node_health_test_set_chain_advance_decision_override(&decision);
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
        ok = ok && strcmp(json_get_str(json_get(&root, "build_commit")),
                          zcl_build_commit()) == 0;
        ok = ok && api_test_expect_freshness(&root, "served_tip",
                                             2, 2, true);
        ok = ok && api_test_expect_readiness_shape(&root);
        ok = ok && api_test_expect_security_posture_shape(&root);
        ok = ok && api_test_expect_lane_safety_fields(
            &root, "canonical", false, false, true, "dev",
            "observe_only_or_use_dev_lane");
        const struct json_value *height_contract =
            json_get(&root, "height_contract");
        ok = ok && height_contract && height_contract->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(height_contract,
                                                "schema")),
                          "zcl.height_contract.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(height_contract,
                                                "status")),
                          "current") == 0;
        ok = ok && !json_get_bool(json_get(height_contract,
                                           "normal_lookahead"));
        ok = ok && json_get_int(json_get(height_contract,
                                         "served_tip_height")) == 2;
        ok = ok && json_get_int(json_get(height_contract,
                                         "active_tip_height")) == 2;
        ok = ok && json_get_int(json_get(height_contract,
                                         "header_tip_height")) == 2;
        ok = ok && strcmp(json_get_str(json_get(height_contract,
                                                "external_height_is")),
                          "served_tip_height") == 0;
        const struct json_value *resources =
            json_get(&root, "resources");
        ok = ok && resources && resources->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(resources, "schema")),
                          "zcl.node_resources.v1") == 0;
        ok = ok && json_get(resources, "rss_mb") != NULL;
        ok = ok && json_get(resources, "cgroup_memory_available") != NULL;
        ok = ok && json_get(resources, "cgroup_memory_current_mb") != NULL;
        ok = ok && json_get(resources, "cgroup_memory_high_mb") != NULL;
        ok = ok && json_get(resources, "cgroup_memory_max_mb") != NULL;
        ok = ok && json_get(resources,
                            "cgroup_memory_stat_available") != NULL;
        ok = ok && json_get(resources, "cgroup_memory_anon_mb") != NULL;
        ok = ok && json_get(resources, "cgroup_memory_file_mb") != NULL;
        ok = ok && json_get(resources,
                            "cgroup_memory_working_set_mb") != NULL;
        ok = ok && json_get(resources,
                            "cgroup_memory_reclaimable_mb") != NULL;
        ok = ok && json_get(resources, "cgroup_memory_watch") != NULL;
        ok = ok && json_get(resources, "memory_pressure") != NULL;
        ok = ok && json_get(resources, "memory_pressure_detail") != NULL;
        ok = ok && json_get(resources, "pressure_basis") != NULL;
        ok = ok && json_get(resources, "uptime_seconds") != NULL;
        const struct json_value *lane =
            json_get(&root, "operator_lane");
        ok = ok && lane && lane->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(lane, "schema")),
                          "zcl.operator_lane.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(lane, "lane")),
                          "canonical") == 0;
        ok = ok && json_get_bool(json_get(lane, "canonical"));
        ok = ok && !json_get_bool(json_get(lane, "development"));
        ok = ok && strcmp(json_get_str(json_get(lane,
                                                "restart_policy")),
                          "operator_gated") == 0;
        ok = ok && !json_get_bool(json_get(lane,
                                           "automation_restart_ok"));
        ok = ok && !json_get_bool(json_get(lane,
                                           "automation_deploy_ok"));
        ok = ok && json_get_bool(json_get(lane,
                                          "requires_operator_confirmation"));
        const struct json_value *safety =
            json_get(lane, "deployment_safety");
        ok = ok && safety && safety->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(safety, "schema")),
                          "zcl.operator_deployment_safety.v1") == 0;
        ok = ok && json_get_bool(json_get(safety,
                                          "protects_public_endpoint"));
        ok = ok && !json_get_bool(json_get(safety,
                                           "automation_deploy_ok"));
        ok = ok && strcmp(json_get_str(json_get(safety,
                                                "safe_default_action")),
                          "observe_only_or_use_dev_lane") == 0;
        const struct json_value *restart_watchdog =
            json_get(&root, "restart_watchdog");
        ok = ok && restart_watchdog && restart_watchdog->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(restart_watchdog,
                                                "schema")),
                          "zcl.restart_watchdog.v1") == 0;
        ok = ok && json_get(restart_watchdog, "status") != NULL;
        ok = ok && json_get(restart_watchdog,
                            "last_restart_autonomous") != NULL;
        ok = ok && json_get(restart_watchdog,
                            "last_restart_reason") != NULL;
        ok = ok && json_get(restart_watchdog,
                            "no_progress_restarts") != NULL;
        json_free(&root);

        reducer_frontier_provable_tip_set(1);
        n = api_handle_request("GET", "/api/v1/agent", NULL, 0,
                               resp, sizeof(resp));
        body = api_test_body(resp, n, sizeof(resp));
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        height_contract = json_get(&root, "height_contract");
        ok = ok && height_contract && height_contract->type == JSON_OBJ;
        ok = ok && strcmp(json_get_str(json_get(height_contract,
                                                "status")),
                          "normal_lookahead") == 0;
        ok = ok && json_get_bool(json_get(height_contract,
                                          "normal_lookahead"));
        ok = ok && json_get_int(json_get(height_contract,
                                         "served_tip_height")) == 1;
        ok = ok && json_get_int(json_get(height_contract,
                                         "active_tip_height")) == 2;
        ok = ok && json_get_int(json_get(height_contract,
                                         "target_height")) == 2;
        ok = ok && json_get_int(json_get(height_contract,
                                         "served_gap_blocks")) == 1;
        ok = ok && json_get(height_contract,
                            "external_height_semantics") != NULL;
        ok = ok && json_get(height_contract,
                            "active_tip_semantics") != NULL;
        json_free(&root);
        reducer_frontier_provable_tip_set(2);

        n = api_handle_request("GET", "/api/v1/node", NULL, 0,
                               resp, sizeof(resp));
        body = api_test_body(resp, n, sizeof(resp));
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.public_status.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "build_commit")),
                          zcl_build_commit()) == 0;
        ok = ok && strcmp(json_get_str(json_get(&root,
                                                "source_projection")),
                          "served_tip") == 0;
        json_free(&root);

        n = api_handle_request("GET", "/api/agent", NULL, 0,
                               resp, sizeof(resp));
        body = api_test_body(resp, n, sizeof(resp));
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.public_status.v1") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "build_commit")),
                          zcl_build_commit()) == 0;
        ok = ok && strcmp(json_get_str(json_get(&root,
                                                "source_projection")),
                          "served_tip") == 0;
        json_free(&root);

        api_set_state(NULL, NULL, NULL, NULL, NULL);
        rpc_agent_set_boot_context(NULL, NULL, NULL, 0, 0, 0, 0);
        node_health_test_set_chain_advance_decision_override(NULL);
        reducer_frontier_provable_tip_reset();
        main_state_free(&ms);
        test_reset_shared_globals();

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: agentimpact maps block source policy to focused gates... ");
    {
        const char *params_json =
            "[\"app/services/src/block_source_policy.c\","
            "\"app/services/include/services/block_source_policy.h\"]";
        struct json_value params, result;
        json_init(&params);
        json_init(&result);
        bool ok = json_read(&params, params_json, strlen(params_json));
        ok = ok && rpc_agent_impact(&params, false, &result);
        ok = ok && strcmp(json_get_str(json_get(&result, "schema")),
                          "zcl.agent_impact.v1") == 0;
        ok = ok && json_get_int(json_get(&result, "files_count")) == 2;
        ok = ok && json_get_int(json_get(&result,
                                         "relevant_test_groups_count")) == 3;
        const struct json_value *groups =
            json_get(&result, "relevant_test_groups");
        ok = ok && api_test_array_has_str(groups,
                                          "chain_advance_coordinator");
        ok = ok && api_test_array_has_str(groups, "mcp_controllers");
        ok = ok && api_test_array_has_str(groups, "make_lint_gates");
        const struct json_value *commands =
            json_get(&result, "recommended_commands");
        ok = ok && api_test_array_has_str(
            commands, "make t ONLY=chain_advance_coordinator");
        ok = ok && api_test_array_has_str(commands,
                                          "make t ONLY=mcp_controllers");
        json_free(&params);
        json_free(&result);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: public status names health blocking reason... ");
    {
        test_reset_shared_globals();
        alerts_shutdown();
        unsetenv("ZCL_ALERTS_DISABLE");
        unsetenv("ZCL_ALERT_WEBHOOK_URL");
        alerts_init();
        alerts_reset();
        event_emitf(EV_OPERATOR_NEEDED, 0, "chain_integrity_failed");

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
        ok = ok && strcmp(json_get_str(json_get(&root, "status")),
                          "blocked") == 0;
        ok = ok && !json_get_bool(json_get(&root, "healthy"));
        ok = ok && !json_get_bool(json_get(&root, "serving"));
        ok = ok && json_get_bool(json_get(&root, "operator_needed"));
        ok = ok && strcmp(json_get_str(json_get(&root,
                                                "primary_blocker")),
                          "operator_needed:chain_integrity_failed") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "summary")),
                          "node has an active health blocker") == 0;
        ok = ok && strcmp(json_get_str(json_get(&root, "next_endpoint")),
                          "/api/v1/health") == 0;
        json_free(&root);

        n = api_handle_request("GET", "/api/v1/agent", NULL, 0,
                               resp, sizeof(resp));
        body = api_test_body(resp, n, sizeof(resp));
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root,
                                                "primary_blocker")),
                          "operator_needed:chain_integrity_failed") == 0;
        json_free(&root);

        alerts_shutdown();
        api_set_state(NULL, NULL, NULL, NULL, NULL);
        reducer_frontier_provable_tip_reset();
        main_state_free(&ms);
        test_reset_shared_globals();

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: public status treats small served gap as healthy... ");
    {
        test_reset_shared_globals();
        struct main_state ms;
        struct connman cm = {0};
        struct net_address addr = {0};
        struct p2p_node *node = NULL;
        struct block_index *blocks[ZCL_NODE_HEALTH_LAG_WARN_BLOCKS + 3] = {0};
        struct cac_decision decision;
        const int served = 1;
        const int target = ZCL_NODE_HEALTH_LAG_WARN_BLOCKS;
        bool ok = api_test_build_chain(&ms, blocks, target + 1);
        ok = ok && api_test_init_connman_peer(&cm, &addr, &node, target);

        memset(&decision, 0, sizeof(decision));
        decision.result = CAC_DECISION_USE_SOURCE;
        decision.selected_source = CAC_SOURCE_P2P;
        decision.local_height = target;
        decision.target_height = target;
        decision.projection_height = target;
        struct cac_source_status *p2p =
            &decision.sources[CAC_SOURCE_P2P];
        p2p->source = CAC_SOURCE_P2P;
        p2p->available = true;
        p2p->healthy = true;
        p2p->selectable = true;
        p2p->height = target;

        if (ok) {
            (void)node;
            reducer_frontier_provable_tip_set(served);
            node_health_test_set_log_head_override(target);
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
            ok = ok && json_get_int(json_get(&root, "height")) == served;
            ok = ok && json_get_int(json_get(&root, "target_height")) ==
                target;
            ok = ok && json_get_int(json_get(&root, "gap")) ==
                target - served;
            ok = ok && strcmp(json_get_str(json_get(&root,
                                                    "primary_blocker")),
                              "none") == 0;
            ok = ok && json_get_int(json_get(&root, "served_height")) ==
                served;
            ok = ok && strcmp(json_get_str(json_get(&root,
                                                    "source_projection")),
                              "served_tip") == 0;
            ok = ok && json_get(&root, "freshness") != NULL;
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

    printf("api: agent clears recovered chain-advance operator latch... ");
    {
        test_reset_shared_globals();
        alerts_shutdown();
        unsetenv("ZCL_ALERTS_DISABLE");
        unsetenv("ZCL_ALERT_WEBHOOK_URL");
        alerts_init();
        alerts_reset();
        event_emitf(EV_OPERATOR_NEEDED, 0,
                    "condition=chain_advance_local_recovery_gate attempts=5");

        struct main_state ms;
        struct connman cm = {0};
        struct net_address addr = {0};
        struct p2p_node *node = NULL;
        struct block_index *blocks[3] = {0};
        bool ok = api_test_build_chain(&ms, blocks, 3);
        ok = ok && api_test_init_connman_peer(&cm, &addr, &node, 2);

        if (ok) {
            (void)node;
            reducer_frontier_provable_tip_set(2);
            ok = ok && sync_set_state(SYNC_IDLE,
                                      "api agent latch reset");
            ok = ok && sync_set_state(SYNC_FINDING_PEERS,
                                      "api agent latch");
            ok = ok && sync_set_state(SYNC_HEADERS_DOWNLOAD,
                                      "api agent latch");
            ok = ok && sync_set_state(SYNC_AT_TIP,
                                      "api agent latch");
            node_health_test_set_log_head_override(2);
            api_set_state(&ms, NULL, NULL, NULL, "/tmp");

            size_t n = api_handle_request("GET", "/api/v1/agent", NULL, 0,
                                          resp, sizeof(resp));
            const char *body = api_test_body(resp, n, sizeof(resp));
            struct json_value root;
            json_init(&root);
            ok = n > 0 && body && json_read(&root, body, strlen(body));
            ok = ok && strcmp(json_get_str(json_get(&root, "status")),
                              "healthy") == 0;
            ok = ok && json_get_bool(json_get(&root, "healthy"));
            ok = ok && !json_get_bool(json_get(&root, "operator_needed"));
            ok = ok && strcmp(json_get_str(json_get(&root,
                                                    "primary_blocker")),
                              "none") == 0;
            ok = ok && json_get_bool(json_get(&root,
                                              "operator_latch_recovered"));
            json_free(&root);
        }

        ok = ok && !alerts_operator_needed(NULL, 0, NULL);
        alerts_shutdown();
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
        struct block_index *blocks[ZCL_NODE_HEALTH_LAG_WARN_BLOCKS + 3] = {0};
        struct cac_decision decision;
        const int served = 1;
        const int target = ZCL_NODE_HEALTH_LAG_WARN_BLOCKS + 2;
        bool ok = api_test_build_chain(&ms, blocks, target + 1);
        ok = ok && api_test_init_connman_peer(&cm, &addr, &node, target);

        memset(&decision, 0, sizeof(decision));
        decision.result = CAC_DECISION_USE_SOURCE;
        decision.selected_source = CAC_SOURCE_P2P;
        decision.local_height = target;
        decision.target_height = target;
        decision.projection_height = target;
        struct cac_source_status *p2p =
            &decision.sources[CAC_SOURCE_P2P];
        p2p->source = CAC_SOURCE_P2P;
        p2p->available = true;
        p2p->healthy = true;
        p2p->selectable = true;
        p2p->height = target;

        if (ok) {
            (void)node;
            reducer_frontier_provable_tip_set(served);
            node_health_test_set_log_head_override(target);
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
            ok = ok && json_get_int(json_get(&root, "height")) == served;
            ok = ok && json_get_int(json_get(&root, "target_height")) ==
                target;
            ok = ok && json_get_int(json_get(&root, "gap")) ==
                target - served;
            ok = ok && strcmp(json_get_str(json_get(&root,
                                                    "primary_blocker")),
                              "download_queue_idle") == 0;
            ok = ok && json_get_int(json_get(&root, "served_height")) ==
                served;
            ok = ok && strcmp(json_get_str(json_get(&root,
                                                    "source_projection")),
                              "served_tip") == 0;
            ok = ok && json_get(&root, "freshness") != NULL;
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

    printf("api: public status uses durable tip before H* publication... ");
    {
        test_reset_shared_globals();
        struct main_state ms;
        struct connman cm;
        struct net_address addr;
        struct p2p_node *node = NULL;
        struct block_index *blocks[4] = {0};
        struct cac_decision decision;
        char dbdir[256];
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_api_durable_status_%d",
                 (int)getpid());
        mkdir(dbdir, 0755);

        bool ok = api_test_build_chain(&ms, blocks, 4);
        ok = ok && api_test_init_connman_peer(&cm, &addr, &node, 3);
        ok = ok && api_test_seed_durable_tip(dbdir, 2);

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
            reducer_frontier_provable_tip_reset();
            node_health_test_set_log_head_override(-2);
            node_health_test_set_chain_advance_decision_override(&decision);
            sync_set_state(SYNC_IDLE, "api status durable reset");
            sync_set_state(SYNC_FINDING_PEERS, "api status durable");
            sync_set_state(SYNC_HEADERS_DOWNLOAD, "api status durable");
            api_set_state(&ms, NULL, NULL, NULL, dbdir);

            size_t n = api_handle_request("GET", "/api/status", NULL, 0,
                                          resp, sizeof(resp));
            const char *body = api_test_body(resp, n, sizeof(resp));
            struct json_value root;
            json_init(&root);
            ok = n > 0 && body && json_read(&root, body, strlen(body));
            ok = ok && strcmp(json_get_str(json_get(&root, "status")),
                              "healthy") == 0;
            ok = ok && json_get_int(json_get(&root, "height")) == 2;
            ok = ok && json_get_int(json_get(&root, "target_height")) == 3;
            ok = ok && json_get_int(json_get(&root, "gap")) == 1;
            ok = ok && strcmp(json_get_str(json_get(&root,
                                                    "primary_blocker")),
                              "none") == 0;
            ok = ok && json_get_int(json_get(&root, "served_height")) == 2;
            ok = ok && json_get_int(json_get(&root, "indexed_height")) >= 2;
            ok = ok && json_get_bool(json_get(&root, "fresh"));
            ok = ok && strcmp(json_get_str(json_get(&root,
                                                    "source_projection")),
                              "served_tip") == 0;
            ok = ok && strcmp(json_get_str(json_get(&root, "blocker")),
                              "none") == 0;
            json_free(&root);
        }

        node_health_test_set_chain_advance_decision_override(NULL);
        node_health_test_set_log_head_override(-2);
        api_set_state(NULL, NULL, NULL, NULL, NULL);
        reducer_frontier_provable_tip_reset();
        progress_store_close();
        main_state_free(&ms);
        rpc_net_set_connman(NULL);
        net_manager_free(&cm.manager);
        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);

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
        ok = ok && api_test_seed_durable_tip(dbdir, 7);
        reducer_frontier_provable_tip_reset();
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
        ok = ok && api_test_expect_freshness(&root, "utxo_projection",
                                             7, 8, true);
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
        ok = ok && api_test_expect_freshness(&root, "utxo_projection",
                                             8, 8, true);
        json_free(&root);

        api_set_state(NULL, NULL, NULL, NULL, NULL);
        api_stop_cache();
        reducer_frontier_provable_tip_reset();
        progress_store_close();
        node_db_close(&ndb);

        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: deep stats suppressed envelope has schema and freshness... ");
    {
        char dbdir[256];
        char dbpath[320];
        struct node_db ndb;
        uint8_t stats_resp[65536];
        memset(&ndb, 0, sizeof(ndb));
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_api_deep_stats_%d",
                 (int)getpid());
        mkdir(dbdir, 0755);
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", dbdir);

        progress_store_close();
        reducer_frontier_provable_tip_reset();
        bool ok = node_db_open(&ndb, dbpath);
        api_set_state(NULL, NULL, NULL, &ndb, dbdir);

        size_t n = compute_deep_stats(stats_resp, sizeof(stats_resp));
        const char *body = api_test_body(stats_resp, n, sizeof(stats_resp));
        struct json_value root;
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.stats.deep.v1") == 0;
        ok = ok && api_test_expect_freshness(&root, "served_height",
                                             0, 0, true);
        ok = ok && !json_get_bool(json_get(&root, "history_index_usable"));
        ok = ok && json_get_bool(json_get(&root,
                                          "unsafe_sections_suppressed"));
        ok = ok && strcmp(json_get_str(json_get(&root, "reason")),
                          "blocks projection is empty") == 0;
        ok = ok && json_get(json_get(&root, "utxo"), "count") != NULL;
        ok = ok && json_get(json_get(&root, "index"), "blocks") != NULL;
        json_free(&root);

        api_set_state(NULL, NULL, NULL, NULL, NULL);
        progress_store_close();
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
        ok = ok && api_test_seed_durable_tip(dbdir, 7);
        reducer_frontier_provable_tip_reset();
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
        progress_store_close();
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

    printf("api: canonical supply is REST; compat supply stays numeric... ");
    {
        const char *canonical =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n\r\n"
            "{\"schema\":\"zcl.supply.v1\",\"served_height\":9,"
            "\"indexed_height\":9,\"fresh\":true,\"freshness\":\"fresh\","
            "\"source_projection\":\"served_height\",\"blocker\":\"none\","
            "\"height\":9,\"supply_zatoshi\":11250000000,"
            "\"supply\":112.5}";
        const char *legacy =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n\r\n"
            "112.50000000";
        api_test_seed_supply_caches(canonical, legacy);

        size_t n = api_handle_request("GET", "/api/v1/supply", NULL, 0,
                                      resp, sizeof(resp));
        const char *body = api_test_body(resp, n, sizeof(resp));
        struct json_value root;
        json_init(&root);
        bool ok = n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.supply.v1") == 0;
        ok = ok && api_test_expect_freshness(&root, "served_height",
                                             9, 9, true);
        ok = ok && json_get_int(json_get(&root, "height")) == 9;
        json_free(&root);

        n = api_handle_request("GET", "/api/supply", NULL, 0,
                               resp, sizeof(resp));
        body = api_test_body(resp, n, sizeof(resp));
        ok = ok && body && strcmp(body, "112.50000000") == 0;
        api_test_seed_supply_caches(NULL, NULL);

        if (ok) printf("OK\n");
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
            ok = ok && api_test_seed_durable_tip(dbdir, 99);
            reducer_frontier_provable_tip_reset();
            api_set_state(NULL, NULL, NULL, &ndb, dbdir);

            size_t n = api_handle_request("GET", "/api/zslp/tokens?limit=10",
                                          NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0) && (strstr((char *)resp, "200 OK") != NULL);
            const char *body = api_test_body(resp, n, sizeof(resp));
            struct json_value root;
            json_init(&root);
            ok = ok && body && json_read(&root, body, strlen(body));
            ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                              "zcl.zslp_tokens.index.v1") == 0;
            ok = ok && api_test_expect_freshness(&root, "zslp_projection",
                                                 99, 99, true);
            ok = ok && json_size(json_get(&root, "tokens")) >= 2;
            ok = ok && (strstr(body, "APITOKEN") != NULL);
            json_free(&root);

            n = api_handle_request("GET", "/api/zslp/tokens/APITOKEN",
                                   NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0);
            body = api_test_body(resp, n, sizeof(resp));
            json_init(&root);
            ok = ok && body && json_read(&root, body, strlen(body));
            ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                              "zcl.zslp_tokens.show.v1") == 0;
            ok = ok && api_test_expect_freshness(&root, "zslp_projection",
                                                 99, 99, true);
            ok = ok && strcmp(json_get_str(json_get(&root, "token_id")),
                              "APITOKEN") == 0;
            json_free(&root);

            n = api_handle_request("GET",
                "/api/zslp/tokens/5555555555555555555555555555555555555555555555555555555555555555/transfers?limit=5",
                NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0);
            body = api_test_body(resp, n, sizeof(resp));
            json_init(&root);
            ok = ok && body && json_read(&root, body, strlen(body));
            ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                              "zcl.zslp_token_transfers.index.v1") == 0;
            ok = ok && api_test_expect_freshness(&root, "zslp_projection",
                                                 99, 99, true);
            ok = ok && json_size(json_get(&root, "transfers")) == 1;
            ok = ok && json_get_int(json_get(json_at(json_get(&root,
                                      "transfers"), 0), "amount")) == 77;
            json_free(&root);

            n = api_handle_request("GET", "/api/zslp/tokens/BAD-TOKEN!",
                                   NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0) && (strstr((char *)resp, "404") != NULL);

            n = api_handle_request("GET", "/api/zslp/tokens?limit=999",
                                   NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0) && (strstr((char *)resp, "404") != NULL);

            api_set_state(NULL, NULL, NULL, NULL, NULL);
            reducer_frontier_provable_tip_reset();
            progress_store_close();
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
            ok = ok && api_test_seed_durable_tip(dbdir, 77);
            reducer_frontier_provable_tip_reset();
            api_set_state(NULL, NULL, NULL, &ndb, dbdir);

            size_t n = api_handle_request("GET", "/api/onion/announcements?limit=2",
                                          NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0);
            const char *body = api_test_body(resp, n, sizeof(resp));
            struct json_value root;
            json_init(&root);
            ok = ok && body && json_read(&root, body, strlen(body));
            ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                              "zcl.onion_announcements.index.v1") == 0;
            ok = ok && api_test_expect_freshness(&root, "onion_projection",
                                                 77, 77, true);
            ok = ok && json_size(json_get(&root, "announcements")) == 2;
            ok = ok && (strstr(body,
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.onion") != NULL);
            json_free(&root);

            n = api_handle_request("GET", "/api/onion/announcements?limit=99",
                                   NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0) && (strstr((char *)resp, "404") != NULL);

            api_set_state(NULL, NULL, NULL, NULL, NULL);
            reducer_frontier_provable_tip_reset();
            progress_store_close();
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
            ok = ok && api_test_seed_durable_tip(dbdir, 88);
            reducer_frontier_provable_tip_reset();
            api_set_state(NULL, NULL, NULL, &ndb, dbdir);

            size_t n = api_handle_request("GET", "/api/file-services?limit=1",
                                          NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0);
            const char *body = api_test_body(resp, n, sizeof(resp));
            struct json_value root;
            json_init(&root);
            ok = ok && body && json_read(&root, body, strlen(body));
            ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                              "zcl.file_services.index.v1") == 0;
            ok = ok && api_test_expect_freshness(&root,
                                                 "file_service_projection",
                                                 88, 88, true);
            ok = ok && json_size(json_get(&root, "file_services")) == 1;
            ok = ok && json_get_int(json_get(json_at(json_get(&root,
                                      "file_services"), 0), "port")) == 8080;
            json_free(&root);

            n = api_handle_request("GET", "/api/file-services?limit=99",
                                   NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0) && (strstr((char *)resp, "404") != NULL);

            api_set_state(NULL, NULL, NULL, NULL, NULL);
            reducer_frontier_provable_tip_reset();
            progress_store_close();
            node_db_close(&ndb);
        }

        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: file manifest exposes REST envelope and freshness... ");
    {
        char dbdir[256];
        char blocksdir[320];
        char blkpath[384];
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_api_manifest_%d",
                 (int)getpid());
        snprintf(blocksdir, sizeof(blocksdir), "%s/blocks", dbdir);
        snprintf(blkpath, sizeof(blkpath), "%s/blk00000.dat", blocksdir);
        mkdir(dbdir, 0755);
        mkdir(blocksdir, 0755);

        FILE *f = fopen(blkpath, "wb");
        bool ok = f != NULL;
        if (ok) {
            static const unsigned char payload[] = {
                0x5a, 0x43, 0x4c, 0x32, 0x33, 0x2d, 0x61, 0x70, 0x69
            };
            ok = fwrite(payload, 1, sizeof(payload), f) == sizeof(payload);
            fclose(f);
        }
        if (ok) {
            struct utimbuf old_time;
            time_t stable_time =
                (time_t)(platform_time_wall_time_t() - 7200);
            old_time.actime = stable_time;
            old_time.modtime = stable_time;
            ok = utime(blkpath, &old_time) == 0;
        }
        if (ok) {
            progress_store_close();
            reducer_frontier_provable_tip_reset();
            api_set_state(NULL, NULL, NULL, NULL, dbdir);
            file_controller_init(dbdir);
            ok = file_controller_refresh_manifest();
        }
        if (ok) {
            size_t n = api_handle_request("GET", "/api/files/manifest",
                                          NULL, 0, resp, sizeof(resp));
            const char *body = api_test_body(resp, n, sizeof(resp));
            struct json_value root;
            json_init(&root);
            ok = n > 0 && body && json_read(&root, body, strlen(body));
            ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                              "zcl.files_manifest.v1") == 0;
            ok = ok && api_test_expect_freshness(&root, "file_manifest",
                                                 0, 0, true);
            ok = ok && json_get_int(json_get(&root, "num_chunks")) == 1;
            ok = ok && json_get_int(json_get(&root, "total_bytes")) == 9;
            const struct json_value *chunks = json_get(&root, "chunks");
            ok = ok && chunks && json_size(chunks) == 1;
            ok = ok && json_get_int(json_get(json_at(chunks, 0),
                                             "size")) == 9;
            ok = ok && json_get_int(json_get(json_at(chunks, 0),
                                             "file")) == 0;
            json_free(&root);
        }

        api_set_state(NULL, NULL, NULL, NULL, NULL);
        reducer_frontier_provable_tip_reset();
        progress_store_close();
        file_controller_init(NULL);

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
            ok = ok && api_test_seed_durable_tip(dbdir, 66);
            reducer_frontier_provable_tip_reset();
            api_set_state(NULL, NULL, NULL, &ndb, dbdir);

            size_t n = api_handle_request("GET", "/api/peers?limit=1",
                                          NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0);
            const char *body = api_test_body(resp, n, sizeof(resp));
            struct json_value root;
            json_init(&root);
            ok = ok && body && json_read(&root, body, strlen(body));
            ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                              "zcl.peers.index.v1") == 0;
            ok = ok && api_test_expect_freshness(&root, "peer_projection",
                                                 66, 66, true);
            ok = ok && json_size(json_get(&root, "peers")) == 1;
            ok = ok && json_get_int(json_get(json_at(json_get(&root,
                                      "peers"), 0), "port")) == 8333;
            json_free(&root);

            n = api_handle_request("GET", "/api/peers?limit=99",
                                   NULL, 0, resp, sizeof(resp));
            ok = ok && (n > 0) && (strstr((char *)resp, "404") != NULL);

            api_set_state(NULL, NULL, NULL, NULL, NULL);
            reducer_frontier_provable_tip_reset();
            progress_store_close();
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
                  api_route_is_operator_private("/api/wallet/keys") &&
                  api_route_is_operator_private("/api/messages") &&
                  api_route_is_operator_private("/api/messages/thread/1") &&
                  api_route_is_operator_private("/api/v1/messages") &&
                  api_route_is_operator_private("/api/v1/swaps") &&
                  api_route_is_operator_private("/api/swaps") &&
                  api_route_is_operator_private("/api/swaps/contracts");
        /* Public routes must stay public — swap chain discovery must not be
         * captured by the private /api/swaps resource prefix. */
        ok = ok && !api_route_is_operator_private("/api/swap_chains") &&
                   !api_route_is_operator_private("/api/v1/swap_chains") &&
                   !api_route_is_operator_private("/api/swaps/chains") &&
                   !api_route_is_operator_private("/api/v1/swaps/chains") &&
                   !api_route_is_operator_private("/api/swaps/chains?x=1") &&
                   !api_route_is_operator_private("/api/swaps/chains/zcl") &&
                   !api_route_is_operator_private("/api/blocks") &&
                   !api_route_is_operator_private("/api/v1/blocks") &&
                   !api_route_is_operator_private("/api/stats") &&
                   !api_route_is_operator_private("/api/walletfoo") &&
                   !api_route_is_operator_private(NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("api: wallet route exposes projection freshness... ");
    {
        char dbdir[256];
        char dbpath[320];
        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        snprintf(dbdir, sizeof(dbdir), ".zcl_test_api_wallet_%d",
                 (int)getpid());
        mkdir(dbdir, 0755);
        snprintf(dbpath, sizeof(dbpath), "%s/node.db", dbdir);

        bool ok = node_db_open(&ndb, dbpath);
        ok = ok && api_test_save_model_block(&ndb, 4, 0x94);
        ok = ok && api_test_seed_durable_tip(dbdir, 4);
        reducer_frontier_provable_tip_reset();
        api_set_state(NULL, NULL, NULL, &ndb, dbdir);

        size_t n = api_handle_request("GET", "/api/wallet", NULL, 0,
                                      resp, sizeof(resp));
        const char *body = api_test_body(resp, n, sizeof(resp));
        struct json_value root;
        json_init(&root);
        ok = ok && n > 0 && body && json_read(&root, body, strlen(body));
        ok = ok && strcmp(json_get_str(json_get(&root, "schema")),
                          "zcl.wallet_status.v1") == 0;
        ok = ok && api_test_expect_freshness(&root, "wallet_projection",
                                             4, 4, true);
        ok = ok && json_get_int(json_get(&root, "height")) == 4;
        ok = ok && json_size(json_get(&root, "activity")) == 0;
        json_free(&root);

        api_set_state(NULL, NULL, NULL, NULL, NULL);
        reducer_frontier_provable_tip_reset();
        progress_store_close();
        node_db_close(&ndb);

        char cmd[384];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dbdir);
        system(cmd);

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
