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
#include "jobs/reducer_frontier.h"
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
#include "platform/time_compat.h"

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

/* Cached endpoints: blocks, stats, supply, hodl, deep_stats. Called only from background refresh thread. */

static int64_t api_hodl_cap_to_served_tip(int64_t index_tip)
{
    if (index_tip < 0)
        return index_tip;
    if (!reducer_frontier_provable_tip_is_published())
        return 0;

    int32_t served_tip = reducer_frontier_provable_tip_cached();
    if (served_tip >= 0 && index_tip > served_tip)
        return served_tip;
    return index_tip;
}

static int64_t api_hodl_tip_from_db(sqlite3 *db, int64_t *block_tip_out,
                                    int64_t *utxo_tip_out)
{
    int64_t block_tip;
    int64_t utxo_tip;

    if (!db) {
        if (block_tip_out)
            *block_tip_out = -1;
        if (utxo_tip_out)
            *utxo_tip_out = -1;
        LOG_RETURN(-1, "api", "api_hodl_tip_from_db: NULL db");
    }

    block_tip = sql_query_i64(db, "SELECT COALESCE(MAX(height),0) FROM blocks");
    utxo_tip = sql_query_i64(db, "SELECT COALESCE(MAX(height),0) FROM utxos");
    if (block_tip_out)
        *block_tip_out = block_tip;
    if (utxo_tip_out)
        *utxo_tip_out = utxo_tip;
    return block_tip > utxo_tip ? block_tip : utxo_tip;
}

int64_t api_hodl_index_tip_height(void)
{
    char db_path[1024];
    sqlite3 *db = NULL;
    int64_t tip;

    if (!g_api_ctx.datadir)
        LOG_RETURN(-1, "api", "api_hodl_index_tip_height: no datadir");

    snprintf(db_path, sizeof(db_path), "%s/node.db", g_api_ctx.datadir);
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        LOG_RETURN(-1, "api", "api_hodl_index_tip_height: open %s failed",
                   db_path);
    }
    tip = api_hodl_tip_from_db(db, NULL, NULL);
    sqlite3_close(db);
    return tip;
}

int64_t api_hodl_current_tip_height(void)
{
    int64_t index_tip = api_hodl_index_tip_height();
    return api_hodl_cap_to_served_tip(index_tip);
}

bool api_hodl_index_ahead_of_served(int64_t *index_tip_out,
                                    int64_t *served_tip_out)
{
    char db_path[1024];
    sqlite3 *db = NULL;
    int64_t index_tip;
    int64_t served_tip;

    if (!g_api_ctx.datadir)
        LOG_RETURN(false, "api", "api_hodl_index_ahead_of_served: no datadir");

    snprintf(db_path, sizeof(db_path), "%s/node.db", g_api_ctx.datadir);
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        LOG_RETURN(false, "api", "api_hodl_index_ahead_of_served: open %s failed",
                   db_path);
    }
    index_tip = api_hodl_tip_from_db(db, NULL, NULL);
    sqlite3_close(db);
    served_tip = api_hodl_cap_to_served_tip(index_tip);
    if (index_tip_out)
        *index_tip_out = index_tip;
    if (served_tip_out)
        *served_tip_out = served_tip;
    return index_tip >= 0 && served_tip >= 0 && index_tip > served_tip;
}

size_t compute_blocks(uint8_t *r, size_t max)
{
    char buf[65536];
    size_t off = 0;

    /* Get current height */
    if (api_rpc_call("getblockcount", "[]", buf, sizeof(buf)) <= 0)
        return api_json_error(r, max, JSON_500_HEADERS, "RPC unavailable");

    int64_t height = zcl_json_int(buf, "result");
    if (height < 0)
        return api_json_error(r, max, JSON_500_HEADERS, "Cannot get block count");

    off += (size_t)snprintf((char *)r + off, max - off, "%s[", JSON_HEADERS);

    int count = 25;
    if (height < count) count = (int)height + 1;

    for (int i = 0; i < count && off + 512 < max; i++) {
        int64_t h = height - i;
        char params[64];
        snprintf(params, sizeof(params), "[%" PRId64 "]", h);
        if (api_rpc_call("getblockhash", params, buf, sizeof(buf)) <= 0)
            continue;

        char hash[65] = "";
        zcl_json_extract_str(buf, "result", hash, sizeof(hash));
        if (!hash[0]) continue;

        char params2[128];
        snprintf(params2, sizeof(params2), "[\"%s\", true]", hash);
        if (api_rpc_call("getblock", params2, buf, sizeof(buf)) <= 0)
            continue;

        int64_t blk_time = zcl_json_int(buf, "time");
        double diff = zcl_json_real(buf, "difficulty");

        /* Count transactions */
        int tx_count = explorer_count_json_tx_array(buf);

        if (i > 0) off += (size_t)snprintf((char *)r + off, max - off, ",");
        off += (size_t)snprintf((char *)r + off, max - off,
            "{\"height\":%" PRId64
            ",\"hash\":\"%s\""
            ",\"time\":%" PRId64
            ",\"num_tx\":%d"
            ",\"difficulty\":%.8f}",
            h, hash, blk_time, tx_count, diff);
    }

    off += (size_t)snprintf((char *)r + off, max - off, "]");
    return off;
}
size_t compute_stats(uint8_t *r, size_t max)
{
    char buf[65536];

    /* Get blockchain info */
    if (api_rpc_call("getblockchaininfo", "[]", buf, sizeof(buf)) <= 0)
        return api_json_error(r, max, JSON_500_HEADERS, "RPC unavailable");

    int64_t height = zcl_json_int(buf, "blocks");
    double diff = zcl_json_real(buf, "difficulty");

    char chain[32] = "";
    zcl_json_extract_str(buf, "chain", chain, sizeof(chain));

    /* Get mining info for hashrate */
    char mbuf[8192];
    double hashrate = 0.0;
    if (api_rpc_call("getmininginfo", "[]", mbuf, sizeof(mbuf)) > 0)
        hashrate = zcl_json_real(mbuf, "networkhashps");

    double supply = (double)zcl_total_supply_zatoshi(height) / (double)ZATOSHI_PER_ZCL;

    /* UTXO count via gettxoutsetinfo — expensive, skip if too slow */
    int64_t utxo_count = -1;
    char ubuf[8192];
    if (api_rpc_call("gettxoutsetinfo", "[]", ubuf, sizeof(ubuf)) > 0)
        utxo_count = zcl_json_int(ubuf, "txouts");

    size_t off = 0;
    off += (size_t)snprintf((char *)r + off, max - off,
        "%s{"
        "\"height\":%" PRId64
        ",\"difficulty\":%.8f"
        ",\"networkhashps\":%.2f"
        ",\"supply\":%.8f"
        ",\"chain\":\"%s\"",
        JSON_HEADERS,
        height, diff, hashrate, supply, chain);

    if (utxo_count >= 0)
        off += (size_t)snprintf((char *)r + off, max - off,
            ",\"utxo_count\":%" PRId64, utxo_count);

    off += (size_t)snprintf((char *)r + off, max - off, "}");
    return off;
}
size_t compute_supply(uint8_t *r, size_t max)
{
    char buf[8192];

    if (api_rpc_call("getblockcount", "[]", buf, sizeof(buf)) <= 0)
        return api_json_error(r, max, JSON_500_HEADERS, "RPC unavailable");

    int64_t height = zcl_json_int(buf, "result");
    if (height < 0)
        return api_json_error(r, max, JSON_500_HEADERS, "Cannot get height");

    double supply = (double)zcl_total_supply_zatoshi(height) / (double)ZATOSHI_PER_ZCL;

    /* Plain number -- CoinGecko expects just a number */
    return (size_t)snprintf((char *)r, max,
        "%s%.8f", JSON_HEADERS, supply);
}
size_t compute_hodl(uint8_t *r, size_t max)
{
    if (!g_api_ctx.datadir)
        return api_json_error(r, max, JSON_503_HEADERS, "Data loading, please retry in a few seconds");

    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", g_api_ctx.datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return api_json_error(r, max, JSON_500_HEADERS, "Database unavailable");
    }

    int64_t block_tip = 0;
    int64_t utxo_tip = 0;
    int64_t index_tip = api_hodl_tip_from_db(db, &block_tip, &utxo_tip);
    int64_t tip = api_hodl_cap_to_served_tip(index_tip);

    struct hodl_wave_snapshot hodl;
    if (!hodl_wave_scan_current_utxos(db, tip, &hodl)) {
        sqlite3_close(db);
        return api_json_error(r, max, JSON_503_HEADERS, hodl.status);
    }
    sqlite3_close(db);

    double over_1y_pct = hodl_wave_older_than_1y_percent(&hodl);

    size_t off = 0;
    off += (size_t)snprintf((char *)r + off, max - off,
        "%s{"
        "\"schema\":\"zcl.hodl_wave.v1\""
        ",\"generated_at\":%" PRId64
        ",\"fresh\":true"
        ",\"source\":\"%s\""
        ",\"metric\":\"%s\""
        ",\"status\":\"%s\""
        ",\"height\":%" PRId64
        ",\"served_tip_height\":%" PRId64
        ",\"indexed_tip_height\":%" PRId64
        ",\"block_tip_height\":%" PRId64
        ",\"utxo_tip_height\":%" PRId64
        ",\"total_utxos\":%" PRId64
        ",\"total_value\":%.8f"
        ",\"older_than_1y\":{\"value\":%.8f,\"percent\":%.6f}"
        ",\"skipped_rows\":%" PRId64
        ",\"buckets\":[",
        JSON_HEADERS,
        (int64_t)platform_time_wall_time_t(),
        hodl.source, hodl.metric, hodl.status,
        hodl.tip_height, tip, index_tip, block_tip, utxo_tip, hodl.total_count,
        (double)hodl.total_value / (double)ZATOSHI_PER_ZCL,
        (double)hodl.older_than_1y_value / (double)ZATOSHI_PER_ZCL,
        over_1y_pct, hodl.skipped_rows);

    for (int i = 0; i < HODL_WAVE_BUCKETS && off + 256 < max; i++) {
        double value_zcl = (double)hodl.buckets[i].value / (double)ZATOSHI_PER_ZCL;
        double pct = hodl.total_value > 0
            ? (double)hodl.buckets[i].value / (double)hodl.total_value * 100.0 : 0.0;
        if (i > 0)
            off += (size_t)snprintf((char *)r + off, max - off, ",");
        off += (size_t)snprintf((char *)r + off, max - off,
            "{\"label\":\"%s\",\"utxos\":%" PRId64
            ",\"value\":%.8f,\"percent\":%.6f}",
            hodl.buckets[i].label, hodl.buckets[i].count, value_zcl, pct);
    }

    off += (size_t)snprintf((char *)r + off, max - off, "]}");
    return off;
}
size_t compute_deep_stats(uint8_t *r, size_t max)
{
    if (!g_api_ctx.datadir)
        return api_json_error(r, max, JSON_500_HEADERS, "No datadir");

    sqlite3 *db = NULL;
    if (!explorer_open_readonly_db(g_api_ctx.datadir, &db)) {
        return api_json_error(r, max, JSON_500_HEADERS, "Cannot open database");
    }

    struct explorer_chain_stats chain_stats = {0};
    explorer_query_chain_stats(db, &chain_stats);
    int64_t current_height = sql_query_i64(db,
        "SELECT COALESCE(MAX(height),0) FROM utxos");
    if (current_height < chain_stats.height)
        current_height = chain_stats.height;
    struct explorer_history_validation history;
    explorer_validate_block_history(db, current_height, &history);
    if (!history.usable) {
        int64_t block_rows = sql_query_i64(db, "SELECT count(*) FROM blocks");
        int64_t tx_rows = sql_query_i64(db, "SELECT count(*) FROM transactions");
        int64_t utxo_count = sql_query_i64(db, "SELECT count(*) FROM utxos");
        int64_t utxo_value = sql_query_i64(db,
            "SELECT COALESCE(SUM(value),0) FROM utxos");
        int64_t supply_sat = zcl_total_supply_zatoshi(current_height);
        sqlite3_close(db);
        return (size_t)snprintf((char *)r, max,
            "%s{"
            "\"history_index_usable\":false,"
            "\"unsafe_sections_suppressed\":true,"
            "\"reason\":\"%s\","
            "\"height\":%" PRId64 ","
            "\"supply\":%.8f,"
            "\"utxo\":{\"count\":%" PRId64 ",\"value\":%.8f},"
            "\"index\":{\"blocks\":%" PRId64 ",\"transactions\":%" PRId64 "}"
            "}",
            JSON_HEADERS,
            history.reason,
            current_height,
            (double)supply_sat / (double)ZATOSHI_PER_ZCL,
            utxo_count, (double)utxo_value / (double)ZATOSHI_PER_ZCL,
            block_rows, tx_rows);
    }
    struct explorer_transaction_stats transaction_stats = {0};
    explorer_query_transaction_stats(db, &transaction_stats);
    struct explorer_utxo_stats utxo_stats = {0};
    explorer_query_utxo_stats(db, &utxo_stats);
    struct explorer_address_stats address_stats = {0};
    explorer_query_address_stats(db, &address_stats);

    /* Sprout stats */
    struct explorer_privacy_stats privacy_stats = {0};
    explorer_query_privacy_stats(db, &privacy_stats);
    struct explorer_first_privacy_heights first_privacy = {0};
    explorer_query_first_privacy_heights(db, &first_privacy);

    /* Sapling stats */
    /* ZSLP stats */
    struct explorer_token_stats token_stats = {0};
    explorer_query_token_stats(db, &token_stats);

    int64_t supply_sat = zcl_total_supply_zatoshi(chain_stats.height);

    /* Integrity: checkpoint count and latest block hash */
    char latest_hash[128] = "";
    sql_query_text(db,
        "SELECT hex(hash) FROM blocks WHERE height = (SELECT MAX(height) FROM blocks)",
        latest_hash, sizeof(latest_hash));

    sqlite3_close(db);

    size_t off = 0;
    off += (size_t)snprintf((char *)r + off, max - off,
        "%s{"
        "\"height\":%" PRId64
        ",\"blocks\":%" PRId64
        ",\"transactions\":%" PRId64
        ",\"supply\":%.8f"
        ",\"shielded_net\":%.8f"
        ",\"sprout\":{\"joinsplits\":%" PRId64 ",\"first_height\":%" PRId64 "}"
        ",\"sapling\":{\"spends\":%" PRId64 ",\"outputs\":%" PRId64
            ",\"first_height\":%" PRId64 "}"
        ",\"utxo\":{\"count\":%" PRId64 ",\"dust_under_0001\":%" PRId64 "}"
        ",\"addresses\":{\"total\":%" PRId64 ",\"nonzero\":%" PRId64 "}"
        ",\"zslp\":{\"tokens\":%" PRId64 ",\"transfers\":%" PRId64 "}"
        ",\"integrity\":{\"indexed_blocks\":%" PRId64
            ",\"latest_hash\":\"%s\"}"
        "}",
        JSON_HEADERS,
        chain_stats.height, chain_stats.blocks, transaction_stats.total,
        (double)supply_sat / (double)ZATOSHI_PER_ZCL,
        (double)privacy_stats.net_shielded_sat / (double)ZATOSHI_PER_ZCL,
        privacy_stats.joinsplits, first_privacy.joinsplit_height,
        privacy_stats.sapling_spends, privacy_stats.sapling_outputs,
        first_privacy.sapling_height,
        utxo_stats.count, utxo_stats.dust_under_0001,
        address_stats.total, address_stats.nonzero,
        token_stats.token_count, token_stats.transfer_count,
        chain_stats.blocks, latest_hash);

    return off;
}
