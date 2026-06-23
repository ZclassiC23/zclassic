/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "controllers/api_controller.h"
#include "controllers/blockchain_controller.h"
#include "controllers/explorer_internal.h"
#include "controllers/file_controller.h"
#include "controllers/file_market_controller.h"
#include "controllers/game_controller.h"
#include "controllers/health_controller.h"
#include "controllers/messaging_controller.h"
#include "controllers/name_controller.h"
#include "controllers/swap_controller.h"
#include "api_controller_internal.h"
#include "chain/mmb.h"
#include "config/boot.h"
#include "config/runtime.h"
#include "encoding/utilstrencodings.h"
#include "event/event.h"
#include "keys/key_io.h"
#include "models/block.h"
#include "models/database.h"
#include "models/file_service.h"
#include "models/hodl_wave.h"
#include "models/onion_announcement.h"
#include "models/peer.h"
#include "models/zslp.h"
#include "net/download.h"
#include "views/explorer_factoids_view.h"
#include "sapling/incremental_merkle_tree.h"
#include "services/node_health_service.h"
#include "net/snapshot_sync_contract.h"
#include "services/zslp_service.h"
#include "validation/contextual_check_tx.h"
#include "validation/main_state.h"
#include "views/format_helpers.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* REST resource collection handlers: zslp, onion, file-services, peers. Served directly from SQLite — no cache, no RPC. */

bool api_is_printable_ascii(const char *s)
{
    if (!s || !s[0])
        return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p < 32 || *p > 126)
            return false;
    }
    return true;
}

struct node_db *api_node_db(void)
{
    return g_api_ctx.node_db ? g_api_ctx.node_db : app_runtime_node_db();
}

bool api_parse_zslp_limit(const char *path, size_t *limit_out)
{
    const char *q;
    const char *lp;
    char *end = NULL;
    long value;

    if (!limit_out)
        LOG_FAIL("api", "parse_zslp_limit: NULL limit_out");
    *limit_out = 50;
    q = strchr(path, '?');
    if (!q)
        return true;
    lp = strstr(q, "limit=");
    if (!lp)
        return true;
    value = strtol(lp + 6, &end, 10);
    if (!end || (*end != '\0' && *end != '&') || value <= 0 || value > 64)
        LOG_FAIL("api", "parse_zslp_limit: invalid limit value");
    *limit_out = (size_t)value;
    return true;
}

bool api_parse_collection_limit(const char *path,
                                       const char *query_key,
                                       size_t default_limit,
                                       size_t max_limit,
                                       size_t *limit_out)
{
    const char *q;
    const char *lp;
    char *end = NULL;
    long value;
    char needle[64];

    if (!path || !query_key || !limit_out || default_limit == 0 ||
        max_limit == 0 || default_limit > max_limit)
        LOG_FAIL("api", "parse_collection_limit: invalid parameters (key=%s)", query_key ? query_key : "NULL");
    *limit_out = default_limit;
    q = strchr(path, '?');
    if (!q)
        return true;
    snprintf(needle, sizeof(needle), "%s=", query_key);
    lp = strstr(q, needle);
    if (!lp)
        return true;
    value = strtol(lp + strlen(needle), &end, 10);
    if (!end || (*end != '\0' && *end != '&') || value <= 0 ||
        (size_t)value > max_limit)
        LOG_FAIL("api", "parse_collection_limit: invalid %s value (max=%zu)", query_key, max_limit);
    *limit_out = (size_t)value;
    return true;
}

size_t api_serve_zslp_tokens(const char *path, uint8_t *response,
                                    size_t response_max)
{
    struct node_db *ndb = api_node_db();
    struct db_zslp_token_info tokens[64];
    size_t limit = 50;
    int count;
    size_t w = 0;

    if (!ndb || !ndb->db)
        return api_json_error(response, response_max, JSON_500_HEADERS, "No database");
    if (!api_parse_zslp_limit(path, &limit))
        return api_json_error(response, response_max, JSON_404_HEADERS,
                          "Invalid limit parameter");

    count = db_zslp_token_list(ndb, tokens, limit);
    w += (size_t)snprintf((char *)response + w, response_max - w,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n"
        "{\"tokens\":[");
    for (int i = 0; i < count && w + 256 < response_max; i++) {
        w += (size_t)snprintf((char *)response + w, response_max - w,
            "%s{\"token_id\":\"%s\",\"ticker\":\"%s\",\"name\":\"%s\","
            "\"decimals\":%d,\"genesis_height\":%d,\"total_minted\":%lld}",
            i > 0 ? "," : "",
            tokens[i].token_id,
            tokens[i].ticker,
            tokens[i].name,
            tokens[i].decimals,
            tokens[i].genesis_height,
            (long long)tokens[i].total_minted);
    }
    w += (size_t)snprintf((char *)response + w, response_max - w, "]}");
    return w < response_max ? w : response_max;
}

size_t api_serve_zslp_token(const char *token_id, uint8_t *response,
                                   size_t response_max)
{
    struct node_db *ndb = api_node_db();
    struct db_zslp_token_info token;

    if (!ndb || !ndb->db)
        return api_json_error(response, response_max, JSON_500_HEADERS, "No database");
    if (!api_is_printable_ascii(token_id) ||
        !zslp_service_validate_token_key(token_id))
        return api_json_error(response, response_max, JSON_404_HEADERS,
                          "Invalid token id");
    if (!db_zslp_token_find(ndb, token_id, &token))
        return api_json_error(response, response_max, JSON_404_HEADERS,
                          "Token not found");

    return (size_t)snprintf((char *)response, response_max,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n"
        "{\"token_id\":\"%s\",\"ticker\":\"%s\",\"name\":\"%s\","
        "\"decimals\":%d,\"genesis_height\":%d,\"total_minted\":%lld}",
        token.token_id, token.ticker, token.name, token.decimals,
        token.genesis_height, (long long)token.total_minted);
}

size_t api_serve_zslp_token_transfers(const char *path,
                                             const char *token_id,
                                             uint8_t *response,
                                             size_t response_max)
{
    struct node_db *ndb = api_node_db();
    struct db_zslp_transfer_info transfers[64];
    size_t limit = 50;
    int count;
    size_t w = 0;

    if (!ndb || !ndb->db)
        return api_json_error(response, response_max, JSON_500_HEADERS, "No database");
    if (!api_is_printable_ascii(token_id) ||
        !zslp_service_validate_token_key(token_id))
        return api_json_error(response, response_max, JSON_404_HEADERS,
                          "Invalid token id");
    if (!api_parse_zslp_limit(path, &limit))
        return api_json_error(response, response_max, JSON_404_HEADERS,
                          "Invalid limit parameter");

    count = db_zslp_transfer_list_by_token(ndb, token_id, transfers, limit);
    w += (size_t)snprintf((char *)response + w, response_max - w,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n"
        "{\"token_id\":\"%s\",\"transfers\":[",
        token_id);
    for (int i = 0; i < count && w + 256 < response_max; i++) {
        w += (size_t)snprintf((char *)response + w, response_max - w,
            "%s{\"txid\":\"%s\",\"token_id\":\"%s\",\"block_height\":%d,"
            "\"tx_type\":%d,\"amount\":%lld,\"vout\":%d%s%s%s}",
            i > 0 ? "," : "",
            transfers[i].txid,
            transfers[i].token_id,
            transfers[i].block_height,
            transfers[i].tx_type,
            (long long)transfers[i].amount,
            transfers[i].vout,
            transfers[i].to_addr_hex[0] ? ",\"to_addr_hex\":\"" : "",
            transfers[i].to_addr_hex,
            transfers[i].to_addr_hex[0] ? "\"" : "");
    }
    w += (size_t)snprintf((char *)response + w, response_max - w, "]}");
    return w < response_max ? w : response_max;
}

size_t api_serve_onion_announcements(const char *path,
                                            uint8_t *response,
                                            size_t response_max)
{
    struct node_db *ndb = api_node_db();
    struct db_onion_announcement rows[32];
    size_t limit = 16;
    int count;
    size_t w = 0;

    if (!ndb || !ndb->db)
        return api_json_error(response, response_max, JSON_500_HEADERS, "No database");
    if (!api_parse_collection_limit(path, "limit", 16, 32, &limit))
        return api_json_error(response, response_max, JSON_404_HEADERS,
                          "Invalid limit parameter");
    memset(rows, 0, sizeof(rows));
    count = db_onion_announcement_recent(ndb, rows, limit);
    w += (size_t)snprintf((char *)response + w, response_max - w,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n"
        "{\"announcements\":[");
    for (int i = 0; i < count && w + 256 < response_max; i++) {
        w += (size_t)snprintf((char *)response + w, response_max - w,
            "%s{\"onion_address\":\"%s\",\"announced_at\":%" PRId64
            ",\"script_hex\":\"%s\"}",
            i > 0 ? "," : "",
            rows[i].onion_address,
            rows[i].announced_at,
            rows[i].script_hex);
    }
    w += (size_t)snprintf((char *)response + w, response_max - w, "]}");
    return w < response_max ? w : response_max;
}

size_t api_serve_file_services(const char *path,
                                      uint8_t *response,
                                      size_t response_max)
{
    struct node_db *ndb = api_node_db();
    struct db_file_service rows[32];
    size_t limit = 16;
    int count;
    size_t w = 0;

    if (!ndb || !ndb->db)
        return api_json_error(response, response_max, JSON_500_HEADERS, "No database");
    if (!api_parse_collection_limit(path, "limit", 16, 32, &limit))
        return api_json_error(response, response_max, JSON_404_HEADERS,
                          "Invalid limit parameter");
    memset(rows, 0, sizeof(rows));
    count = db_file_service_recent(ndb, rows, limit);
    w += (size_t)snprintf((char *)response + w, response_max - w,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n"
        "{\"file_services\":[");
    for (int i = 0; i < count && w + 256 < response_max; i++) {
        char ip_hex[33];
        HexStr(rows[i].ip, 16, false, ip_hex, sizeof(ip_hex));
        w += (size_t)snprintf((char *)response + w, response_max - w,
            "%s{\"ip\":\"%s\",\"port\":%u,\"p2p_port\":%u,"
            "\"last_seen\":%" PRId64 ",\"is_zcl23\":%s}",
            i > 0 ? "," : "",
            ip_hex,
            (unsigned)rows[i].port,
            (unsigned)rows[i].p2p_port,
            rows[i].last_seen,
            rows[i].is_zcl23 ? "true" : "false");
    }
    w += (size_t)snprintf((char *)response + w, response_max - w, "]}");
    return w < response_max ? w : response_max;
}

size_t api_serve_peers(const char *path,
                              uint8_t *response,
                              size_t response_max)
{
    struct node_db *ndb = api_node_db();
    struct db_peer rows[32];
    size_t limit = 16;
    int count;
    size_t w = 0;

    if (!ndb || !ndb->db)
        return api_json_error(response, response_max, JSON_500_HEADERS, "No database");
    if (!api_parse_collection_limit(path, "limit", 16, 32, &limit))
        return api_json_error(response, response_max, JSON_404_HEADERS,
                          "Invalid limit parameter");
    memset(rows, 0, sizeof(rows));
    count = db_peer_recent(ndb, rows, limit);
    w += (size_t)snprintf((char *)response + w, response_max - w,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n"
        "{\"peers\":[");
    for (int i = 0; i < count && w + 320 < response_max; i++) {
        char ip_hex[33];
        char src_hex[33];
        for (int j = 0; j < 16; j++) {
            snprintf(ip_hex + j * 2, 3, "%02x", rows[i].ip[j]);
            snprintf(src_hex + j * 2, 3, "%02x", rows[i].source[j]);
        }
        w += (size_t)snprintf((char *)response + w, response_max - w,
            "%s{\"ip\":\"%s\",\"port\":%u,\"services\":%llu,"
            "\"last_seen\":%" PRId64 ",\"last_try\":%" PRId64
            ",\"attempts\":%d,\"bandwidth_score\":%u,"
            "\"is_zcl23\":%s%s%s%s}",
            i > 0 ? "," : "",
            ip_hex,
            (unsigned)rows[i].port,
            (unsigned long long)rows[i].services,
            rows[i].last_seen,
            rows[i].last_try,
            rows[i].attempts,
            (unsigned)rows[i].bandwidth_score,
            rows[i].is_zcl23 ? "true" : "false",
            rows[i].has_source ? ",\"source\":\"" : "",
            rows[i].has_source ? src_hex : "",
            rows[i].has_source ? "\"" : "");
    }
    w += (size_t)snprintf((char *)response + w, response_max - w, "]}");
    return w < response_max ? w : response_max;
}
