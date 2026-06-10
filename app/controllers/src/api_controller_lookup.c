/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "platform/time_compat.h"
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
#include "views/explorer_factoids_view.h"
#include "net/download.h"
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

/* Lookup queue and parameterized handlers: /api/block/:id, /tx/:txid, /address/:addr. Single-slot work queue, served via background thread. */

size_t compute_block(const char *param, uint8_t *r, size_t max)
{
    if (!param || !*param)
        return api_json_error(r, max, JSON_404_HEADERS, "Missing block identifier");

    char buf[262144];
    char hash[65] = "";

    /* Resolve height to hash if needed */
    if (zcl_is_all_digits(param)) {
        char params[64];
        snprintf(params, sizeof(params), "[%s]", param);
        if (api_rpc_call("getblockhash", params, buf, sizeof(buf)) <= 0)
            return api_json_error(r, max, JSON_500_HEADERS, "RPC unavailable");
        zcl_json_extract_str(buf, "result", hash, sizeof(hash));
    } else if (zcl_is_hex_string(param, 64)) {
        snprintf(hash, sizeof(hash), "%s", param);
    }

    if (!hash[0])
        return api_json_error(r, max, JSON_404_HEADERS, "Block not found");

    /* Get full block */
    char params2[128];
    snprintf(params2, sizeof(params2), "[\"%s\", true]", hash);
    if (api_rpc_call("getblock", params2, buf, sizeof(buf)) <= 0)
        return api_json_error(r, max, JSON_500_HEADERS, "RPC unavailable");

    if (strstr(buf, "\"error\":null") == NULL)
        return api_json_error(r, max, JSON_404_HEADERS, "Block not found");

    int64_t height = zcl_json_int(buf, "height");
    int64_t blk_time = zcl_json_int(buf, "time");
    int64_t blk_size = zcl_json_int(buf, "size");
    double diff = zcl_json_real(buf, "difficulty");

    char merkle[65] = "", prev[65] = "", next_hash[65] = "", nonce[65] = "";
    zcl_json_extract_str(buf, "merkleroot", merkle, sizeof(merkle));
    zcl_json_extract_str(buf, "previousblockhash", prev, sizeof(prev));
    zcl_json_extract_str(buf, "nextblockhash", next_hash, sizeof(next_hash));
    zcl_json_extract_str(buf, "nonce", nonce, sizeof(nonce));

    int64_t confirmations = zcl_json_int(buf, "confirmations");

    /* Count and collect transaction IDs */
    int tx_count = 0;
    const char *txarr = strstr(buf, "\"tx\":[");
    if (txarr) {
        const char *end = strchr(txarr, ']');
        tx_count = 1;
        if (end) for (const char *p = txarr; p < end; p++)
            if (*p == ',') tx_count++;
    }

    size_t off = 0;
    off += (size_t)snprintf((char *)r + off, max - off,
        "%s{"
        "\"hash\":\"%s\""
        ",\"height\":%" PRId64
        ",\"time\":%" PRId64
        ",\"size\":%" PRId64
        ",\"difficulty\":%.8f"
        ",\"confirmations\":%" PRId64
        ",\"num_tx\":%d"
        ",\"merkleroot\":\"%s\""
        ",\"nonce\":\"%s\"",
        JSON_HEADERS,
        hash, height, blk_time, blk_size, diff,
        confirmations, tx_count, merkle, nonce);

    if (prev[0])
        off += (size_t)snprintf((char *)r + off, max - off,
            ",\"previousblockhash\":\"%s\"", prev);
    if (next_hash[0])
        off += (size_t)snprintf((char *)r + off, max - off,
            ",\"nextblockhash\":\"%s\"", next_hash);

    /* Transaction ID array */
    off += (size_t)snprintf((char *)r + off, max - off, ",\"tx\":[");
    if (txarr) {
        const char *p = txarr + 6; /* skip "tx":[ */
        int idx = 0;
        while (p && idx < 200 && off + 128 < max) {
            if (*p == '"') {
                p++;
                const char *end = strchr(p, '"');
                if (!end) break;
                size_t tlen = (size_t)(end - p);
                if (tlen > 64) tlen = 64;
                if (idx > 0)
                    off += (size_t)snprintf((char *)r + off, max - off, ",");
                off += (size_t)snprintf((char *)r + off, max - off,
                    "\"%.*s\"", (int)tlen, p);
                idx++;
                p = end + 1;
            } else if (*p == ']') {
                break;
            } else {
                p++;
            }
        }
    }
    off += (size_t)snprintf((char *)r + off, max - off, "]}");
    return off;
}
size_t compute_tx(const char *param, uint8_t *r, size_t max)
{
    if (!zcl_is_hex_string(param, 64))
        return api_json_error(r, max, JSON_404_HEADERS, "Invalid transaction ID");

    char buf[262144];
    char params[128];
    snprintf(params, sizeof(params), "[\"%s\", 1]", param);
    int n = api_rpc_call("getrawtransaction", params, buf, sizeof(buf));
    if (n <= 0 || strstr(buf, "\"error\":null") == NULL)
        return api_json_error(r, max, JSON_404_HEADERS, "Transaction not found");

    /* Extract the result object */
    const char *result = strstr(buf, "\"result\":{");
    if (!result) result = buf;

    int64_t confirmations = zcl_json_int(result, "confirmations");
    int64_t blk_height = zcl_json_int(result, "height");
    int64_t tx_size = zcl_json_int(result, "size");
    int64_t version = zcl_json_int(result, "version");
    int64_t locktime = zcl_json_int(result, "locktime");
    double value_balance = zcl_json_real(result, "valuebalance");

    char blockhash[65] = "";
    zcl_json_extract_str(result, "blockhash", blockhash, sizeof(blockhash));

    size_t off = 0;
    off += (size_t)snprintf((char *)r + off, max - off,
        "%s{"
        "\"txid\":\"%s\""
        ",\"version\":%" PRId64
        ",\"size\":%" PRId64
        ",\"locktime\":%" PRId64
        ",\"confirmations\":%" PRId64
        ",\"blockhash\":\"%s\""
        ",\"blockheight\":%" PRId64
        ",\"valuebalance\":%.8f",
        JSON_HEADERS,
        param, version, tx_size, locktime,
        confirmations, blockhash, blk_height, value_balance);

    /* Parse vout array */
    off += (size_t)snprintf((char *)r + off, max - off, ",\"vout\":[");
    const char *vout = strstr(result, "\"vout\":[");
    if (vout) {
        const char *p = vout + 7;
        int brace = 0, idx = 0;
        while (*p && off + 512 < max) {
            if (*p == '{') {
                brace++;
                if (brace == 1) {
                    const char *entry = p;
                    int depth = 0;
                    const char *entry_end = NULL;
                    for (const char *q = p; *q; q++) {
                        if (*q == '{') depth++;
                        if (*q == '}') { depth--; if (depth == 0) { entry_end = q + 1; break; } }
                    }
                    if (!entry_end) break;

                    double val = zcl_json_real(entry, "value");
                    int64_t vn = zcl_json_int(entry, "n");

                    char addr[64] = "";
                    const char *addrs = strstr(entry, "\"addresses\":[\"");
                    if (addrs && addrs < entry_end) {
                        addrs += 14;
                        size_t ai = 0;
                        while (addrs[ai] && addrs[ai] != '"' && ai < sizeof(addr) - 1) {
                            addr[ai] = addrs[ai]; ai++;
                        }
                        addr[ai] = '\0';
                    }

                    if (idx > 0)
                        off += (size_t)snprintf((char *)r + off, max - off, ",");
                    off += (size_t)snprintf((char *)r + off, max - off,
                        "{\"n\":%" PRId64 ",\"value\":%.8f", vn, val);
                    if (addr[0])
                        off += (size_t)snprintf((char *)r + off, max - off,
                            ",\"address\":\"%s\"", addr);
                    off += (size_t)snprintf((char *)r + off, max - off, "}");
                    idx++;

                    p = entry_end;
                    brace = 0;
                    continue;
                }
            }
            if (*p == ']' && brace == 0) break;
            p++;
        }
    }
    off += (size_t)snprintf((char *)r + off, max - off, "]");

    /* Parse vin array */
    off += (size_t)snprintf((char *)r + off, max - off, ",\"vin\":[");
    const char *vin = strstr(result, "\"vin\":[");
    if (vin) {
        const char *p = vin + 6;
        int brace = 0, idx = 0;
        while (*p && off + 256 < max) {
            if (*p == '{') {
                brace++;
                if (brace == 1) {
                    const char *entry = p;
                    int depth = 0;
                    const char *entry_end = NULL;
                    for (const char *q = p; *q; q++) {
                        if (*q == '{') depth++;
                        if (*q == '}') { depth--; if (depth == 0) { entry_end = q + 1; break; } }
                    }
                    if (!entry_end) break;

                    char prev_txid[65] = "";
                    zcl_json_extract_str(entry, "txid", prev_txid, sizeof(prev_txid));
                    int64_t vout_n = zcl_json_int(entry, "vout");

                    if (idx > 0)
                        off += (size_t)snprintf((char *)r + off, max - off, ",");

                    /* Check for coinbase */
                    if (strstr(entry, "\"coinbase\"") && (const char*)strstr(entry, "\"coinbase\"") < entry_end) {
                        off += (size_t)snprintf((char *)r + off, max - off,
                            "{\"coinbase\":true}");
                    } else {
                        off += (size_t)snprintf((char *)r + off, max - off,
                            "{\"txid\":\"%s\",\"vout\":%" PRId64 "}",
                            prev_txid, vout_n);
                    }
                    idx++;
                    p = entry_end;
                    brace = 0;
                    continue;
                }
            }
            if (*p == ']' && brace == 0) break;
            p++;
        }
    }
    off += (size_t)snprintf((char *)r + off, max - off, "]}");
    return off;
}
size_t compute_address(const char *param, uint8_t *r, size_t max)
{
    if (!param || !*param)
        return api_json_error(r, max, JSON_404_HEADERS, "Missing address");

    size_t alen = strlen(param);
    if (alen < 25 || alen > 95)
        return api_json_error(r, max, JSON_404_HEADERS, "Invalid address");

    /* Ensure address is safe to embed in JSON/RPC params */
    if (!api_is_json_safe_param(param, alen))
        return api_json_error(r, max, JSON_404_HEADERS, "Invalid address characters");

    char buf[262144];
    size_t off = 0;

    /* Try getaddressbalance (addressindex RPC) */
    char params[256];
    snprintf(params, sizeof(params),
        "[{\"addresses\":[\"%s\"]}]", param);
    int n = api_rpc_call("getaddressbalance", params, buf, sizeof(buf));

    int64_t balance_sat = 0;
    bool got_balance = false;

    if (n > 0 && strstr(buf, "\"error\":null")) {
        balance_sat = zcl_json_int(buf, "balance");
        got_balance = true;
    }

    /* Try getaddressutxos */
    char ubuf[262144];
    n = api_rpc_call("getaddressutxos", params, ubuf, sizeof(ubuf));

    off += (size_t)snprintf((char *)r + off, max - off,
        "%s{\"address\":\"%s\"", JSON_HEADERS, param);

    if (got_balance) {
        off += (size_t)snprintf((char *)r + off, max - off,
            ",\"balance_sat\":%" PRId64
            ",\"balance\":%.8f",
            balance_sat, (double)balance_sat / (double)ZATOSHI_PER_ZCL);
    }

    /* Parse UTXOs from result array */
    off += (size_t)snprintf((char *)r + off, max - off, ",\"utxos\":[");

    if (n > 0 && strstr(ubuf, "\"error\":null")) {
        const char *result = strstr(ubuf, "\"result\":[");
        if (result) {
            const char *p = result + 10;
            int brace = 0, idx = 0;
            while (*p && off + 512 < max) {
                if (*p == '{') {
                    brace++;
                    if (brace == 1) {
                        const char *entry = p;
                        int depth = 0;
                        const char *entry_end = NULL;
                        for (const char *q = p; *q; q++) {
                            if (*q == '{') depth++;
                            if (*q == '}') { depth--; if (depth == 0) { entry_end = q + 1; break; } }
                        }
                        if (!entry_end) break;

                        char txid[65] = "";
                        zcl_json_extract_str(entry, "txid", txid, sizeof(txid));
                        int64_t output_idx = zcl_json_int(entry, "outputIndex");
                        int64_t satoshis = zcl_json_int(entry, "satoshis");
                        int64_t utxo_height = zcl_json_int(entry, "height");

                        if (idx > 0)
                            off += (size_t)snprintf((char *)r + off, max - off, ",");
                        off += (size_t)snprintf((char *)r + off, max - off,
                            "{\"txid\":\"%s\""
                            ",\"vout\":%" PRId64
                            ",\"satoshis\":%" PRId64
                            ",\"value\":%.8f"
                            ",\"height\":%" PRId64 "}",
                            txid, output_idx, satoshis,
                            (double)satoshis / (double)ZATOSHI_PER_ZCL, utxo_height);
                        idx++;

                        p = entry_end;
                        brace = 0;
                        continue;
                    }
                }
                if (*p == ']' && brace == 0) break;
                p++;
            }
        }
    }

    off += (size_t)snprintf((char *)r + off, max - off, "]}");
    return off;
}
/* ── Parameterized endpoint request queue ────────────────── */
/* For /api/block/:id, /api/tx/:txid, /api/address/:addr we
 * submit the request to the background thread and serve from
 * a small per-request cache. Uses a simple single-slot queue. */

static pthread_mutex_t g_lookup_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_lookup_request_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_lookup_done_cond = PTHREAD_COND_INITIALIZER;

/* enum lookup_type defined in api_controller_internal.h */

static _Atomic enum lookup_type g_lookup_type = LOOKUP_NONE;
static char    g_lookup_param[512];
static uint8_t g_lookup_result[262144];
static size_t  g_lookup_result_len = 0;
static _Atomic int g_lookup_thread_running = 0;

/* Background thread that processes lookup requests one at a time */
static void *api_lookup_thread(void *arg)
{
    (void)arg;
    printf("API lookup: background thread started\n");
    fflush(stdout);

    while (g_lookup_thread_running) {
        pthread_mutex_lock(&g_lookup_mutex);
        while (g_lookup_type == LOOKUP_NONE && g_lookup_thread_running) {
            struct timespec ts;
            platform_time_realtime_timespec(&ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&g_lookup_request_cond, &g_lookup_mutex, &ts);
        }
        if (!g_lookup_thread_running) {
            pthread_mutex_unlock(&g_lookup_mutex);
            break;
        }

        enum lookup_type type = g_lookup_type;
        char param[512];
        snprintf(param, sizeof(param), "%s", g_lookup_param);
        pthread_mutex_unlock(&g_lookup_mutex);

        size_t len = 0;
        switch (type) {
        case LOOKUP_BLOCK:
            len = compute_block(param, g_lookup_result, sizeof(g_lookup_result));
            break;
        case LOOKUP_TX:
            len = compute_tx(param, g_lookup_result, sizeof(g_lookup_result));
            break;
        case LOOKUP_ADDRESS:
            len = compute_address(param, g_lookup_result, sizeof(g_lookup_result));
            break;
        default:
            break;
        }

        pthread_mutex_lock(&g_lookup_mutex);
        g_lookup_result_len = len;
        g_lookup_type = LOOKUP_NONE;
        pthread_cond_broadcast(&g_lookup_done_cond);
        pthread_mutex_unlock(&g_lookup_mutex);
    }

    printf("API lookup: background thread stopped\n");
    fflush(stdout);
    return NULL;
}

static bool ensure_lookup_thread(void)
{
    int expected = 0;
    pthread_t t;

    if (!atomic_compare_exchange_strong(&g_lookup_thread_running,
                                        &expected, 1))
        return expected == 1;
    if (!api_start_detached_thread(&t, api_lookup_thread, NULL)) {
        atomic_store(&g_lookup_thread_running, 0);
        LOG_FAIL("api", "ensure_lookup_thread: failed to start lookup thread");
    }
    return true;
}

/* Submit a lookup request and wait for result (with timeout) */
size_t do_lookup(enum lookup_type type, const char *param,
                         uint8_t *response, size_t response_max)
{
    if (!ensure_lookup_thread()) {
        return api_json_error(response, response_max, JSON_503_HEADERS,
                          "Lookup unavailable, please retry");
    }

    pthread_mutex_lock(&g_lookup_mutex);

    /* If another request is pending, return 503 */
    if (g_lookup_type != LOOKUP_NONE) {
        pthread_mutex_unlock(&g_lookup_mutex);
        return api_json_error(response, response_max, JSON_503_HEADERS,
                          "Server busy, please retry");
    }

    snprintf(g_lookup_param, sizeof(g_lookup_param), "%s", param);
    g_lookup_result_len = 0;
    g_lookup_type = type;
    pthread_cond_signal(&g_lookup_request_cond);

    /* Wait up to 15 seconds for result */
    struct timespec ts;
    platform_time_realtime_timespec(&ts);
    ts.tv_sec += 15;

    while (g_lookup_type != LOOKUP_NONE) {
        int rc = pthread_cond_timedwait(&g_lookup_done_cond, &g_lookup_mutex, &ts);
        if (rc != 0) {
            /* Timeout */
            pthread_mutex_unlock(&g_lookup_mutex);
            return api_json_error(response, response_max, JSON_503_HEADERS,
                              "Request timed out");
        }
    }

    size_t len = g_lookup_result_len;
    if (len > 0) {
        size_t copy = len < response_max ? len : response_max;
        memcpy(response, g_lookup_result, copy);
        pthread_mutex_unlock(&g_lookup_mutex);
        return copy;
    }

    pthread_mutex_unlock(&g_lookup_mutex);
    return api_json_error(response, response_max, JSON_500_HEADERS, "RPC unavailable");
}
