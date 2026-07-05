/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * chainstate-backed RPCs must fail cleanly when the active
 * chain height points at a missing block-index entry. */

#include "test/test_helpers.h"
#include "controllers/blockchain_controller.h"
#include "controllers/chain_inspect_controller.h"
#include "controllers/health_controller.h"
#include "controllers/mining_controller.h"
#include "controllers/misc_controller.h"
#include "controllers/repair_controller.h"
#include "controllers/transaction_controller.h"
#include "controllers/wallet_diagnostic_controller.h"
#include "controllers/wallet_helpers.h"
#include "controllers/wallet_rescan_controller.h"
#include "jobs/reducer_frontier.h"
#include "json/json.h"
#include "models/block.h"
#include "models/database.h"
#include "rpc/server.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "validation/main_state.h"
#include "wallet/wallet.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ensure_rpc_warmup_finished_once(void)
{
    char status[32];
    if (rpc_is_in_warmup(status, sizeof(status)))
        set_rpc_warmup_finished();
}

static void build_unresolved_tip_state(struct main_state *ms, int tip_height)
{
    main_state_init(ms);
    ms->chain_active.height = tip_height;
    ms->chain_active.capacity = tip_height + 1;
    ms->chain_active.chain = calloc((size_t)(tip_height + 1),
                                    sizeof(*ms->chain_active.chain));
}

static void init_single_str_param(struct json_value *params, const char *s)
{
    struct json_value v;
    json_init(params);
    json_set_array(params);
    json_init(&v);
    json_set_str(&v, s);
    json_push_back(params, &v);
    json_free(&v);
}

static void init_single_int_param(struct json_value *params, int64_t n)
{
    struct json_value v;
    json_init(params);
    json_set_array(params);
    json_init(&v);
    json_set_int(&v, n);
    json_push_back(params, &v);
    json_free(&v);
}

static bool result_is_chainstate_guard_error(const struct json_value *result,
                                             const char *method)
{
    const struct json_value *code = json_get(result, "code");
    const struct json_value *msg = json_get(result, "message");
    const struct json_value *got_method = json_get(result, "method");
    return result->type == JSON_OBJ &&
           code && code->type == JSON_INT &&
           code->val.i == RPC_INTERNAL_ERROR &&
           msg && msg->type == JSON_STR &&
           strstr(json_get_str(msg), "active chain tip height") != NULL &&
           got_method && got_method->type == JSON_STR &&
           strcmp(json_get_str(got_method), method) == 0;
}

static bool result_is_retired_reindex_error(const struct json_value *result)
{
    return result->type == JSON_STR &&
           strstr(json_get_str(result), "Runtime reindexchainstate is retired")
               != NULL &&
           strstr(json_get_str(result), "-reindex-chainstate") != NULL;
}

static struct block_index *rpc_safety_insert_block(struct main_state *ms,
                                                   struct uint256 *hash,
                                                   int height,
                                                   struct block_index *prev)
{
    memset(hash, 0, sizeof(*hash));
    hash->data[0] = (uint8_t)(height & 0xff);
    hash->data[1] = (uint8_t)((height >> 8) & 0xff);
    hash->data[2] = 0x52;

    struct block_index *bi =
        chainstate_insert_block_index((struct chainstate *)ms, hash);
    if (!bi)
        return NULL;
    bi->nHeight = height;
    bi->nBits = 0x1f07ffff;
    bi->nTime = 1000000 + (uint32_t)height * 150;
    bi->nVersion = 4;
    bi->nStatus = BLOCK_HAVE_DATA | BLOCK_VALID_SCRIPTS;
    bi->nTx = 1;
    bi->nChainTx = (uint32_t)(height + 1);
    arith_uint256_set_u64(&bi->nChainWork, (uint64_t)(height + 1));
    bi->pprev = prev;
    return bi;
}

static bool rpc_safety_build_chain(struct main_state *ms,
                                   struct block_index **out,
                                   int count)
{
    static struct uint256 hashes[16];
    if (count <= 0 || count > (int)(sizeof(hashes) / sizeof(hashes[0])))
        return false;

    main_state_init(ms);
    struct block_index *prev = NULL;
    for (int h = 0; h < count; h++) {
        out[h] = rpc_safety_insert_block(ms, &hashes[h], h, prev);
        if (!out[h])
            return false;
        prev = out[h];
    }
    ms->pindex_best_header = out[count - 1];
    return active_chain_move_window_tip(&ms->chain_active, out[count - 1]);
}

static bool rpc_safety_exec(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

static bool rpc_safety_set_applied(sqlite3 *db, int32_t height)
{
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return false;
    }
    bool ok = coins_kv_set_applied_height_in_tx(db, height);
    const char *finish = ok ? "COMMIT" : "ROLLBACK";
    if (sqlite3_exec(db, finish, NULL, NULL, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return false;
    }
    return ok;
}

static bool rpc_safety_seed_coin_frontier(sqlite3 *db,
                                          const struct block_index *coin_tip)
{
    if (!db || !coin_tip || !coin_tip->phashBlock)
        return false;
    if (!coins_kv_ensure_schema(db))
        return false;

    uint8_t txid[32] = {0};
    txid[0] = 0x52;
    txid[1] = 0x50;
    if (!coins_kv_add(db, txid, 0, 1000LL, coin_tip->nHeight, false,
                      NULL, 0))
        return false;

    uint8_t one = 1;
    if (!progress_meta_set(db, COINS_KV_MIGRATION_COMPLETE_KEY, &one, 1))
        return false;
    if (!rpc_safety_set_applied(db, coin_tip->nHeight + 1))
        return false;

    if (!rpc_safety_exec(db,
            "CREATE TABLE IF NOT EXISTS validate_headers_log ("
            "height INTEGER PRIMARY KEY, hash BLOB NOT NULL, ok INTEGER NOT NULL,"
            "fail_reason TEXT, validated_at INTEGER NOT NULL)"))
        return false;
    if (!rpc_safety_exec(db,
            "CREATE TABLE IF NOT EXISTS tip_finalize_log ("
            "height INTEGER PRIMARY KEY, status TEXT NOT NULL, "
            "ok INTEGER NOT NULL, tip_hash BLOB)"))
        return false;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO validate_headers_log"
            "(height,hash,ok,fail_reason,validated_at) VALUES(?,?,1,NULL,1)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, coin_tip->nHeight);
    sqlite3_bind_blob(st, 2, coin_tip->phashBlock->data, 32,
                      SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-fixture
    sqlite3_finalize(st);
    return ok;
}

static bool rpc_safety_save_projected_block(struct node_db *ndb,
                                            const struct block_index *bi)
{
    if (!ndb || !bi || !bi->phashBlock)
        return false;

    uint8_t solution[1] = {0x51};
    struct db_block b;
    memset(&b, 0, sizeof(b));
    memcpy(b.hash, bi->phashBlock->data, sizeof(b.hash));
    if (bi->pprev && bi->pprev->phashBlock)
        memcpy(b.prev_hash, bi->pprev->phashBlock->data, sizeof(b.prev_hash));
    b.height = bi->nHeight;
    b.version = bi->nVersion;
    memcpy(b.merkle_root, bi->hashMerkleRoot.data, sizeof(b.merkle_root));
    if (memcmp(b.merkle_root, (uint8_t[32]){0}, sizeof(b.merkle_root)) == 0)
        b.merkle_root[0] = 0x4d;
    b.time = bi->nTime;
    b.bits = bi->nBits;
    memcpy(b.nonce, bi->nNonce.data, sizeof(b.nonce));
    if (memcmp(b.nonce, (uint8_t[32]){0}, sizeof(b.nonce)) == 0)
        b.nonce[0] = 0x4e;
    b.solution = solution;
    b.solution_len = sizeof(solution);
    b.chain_work[0] = (uint8_t)(bi->nHeight + 1);
    b.status = (int)bi->nStatus;
    b.num_tx = (int)bi->nTx;
    return db_block_save(ndb, &b);
}

int test_rpc_safety(void)
{
    int failures = 0;

    printf("rpc_safety: reindexchainstate rejects runtime replay... ");
    {
        ensure_rpc_warmup_finished_once();

        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_blockchain_rpc_commands(&tbl);

        struct json_value params = {0};
        struct json_value result = {0};
        json_init(&params);
        json_set_array(&params);
        json_init(&result);

        bool ok = !rpc_table_execute(&tbl, "reindexchainstate", &params,
                                     &result) &&
                  result_is_retired_reindex_error(&result);

        json_free(&params);
        json_free(&result);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("rpc_safety: getbestblockhash follows provable tip... ");
    {
        test_reset_shared_globals();
        ensure_rpc_warmup_finished_once();

        struct main_state ms;
        struct block_index *blocks[4] = {0};
        bool ok = rpc_safety_build_chain(&ms, blocks, 4);
        rpc_blockchain_set_state(&ms, NULL, "/tmp");
        reducer_frontier_provable_tip_set(1);

        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_blockchain_rpc_commands(&tbl);

        struct rpc_table health_tbl;
        rpc_table_init(&health_tbl);
        rpc_health_set_state(&ms, NULL, NULL, NULL);
        register_health_rpc_commands(&health_tbl);

        struct json_value params = {0};
        struct json_value result = {0};
        json_init(&params);
        json_set_array(&params);
        json_init(&result);

        ok = ok && rpc_table_execute(&tbl, "getbestblockhash", &params,
                                     &result);
        char hstar_hex[65] = {0};
        char active_hex[65] = {0};
        if (blocks[1] && blocks[1]->phashBlock)
            uint256_get_hex(blocks[1]->phashBlock, hstar_hex);
        if (blocks[3] && blocks[3]->phashBlock)
            uint256_get_hex(blocks[3]->phashBlock, active_hex);
        ok = ok && result.type == JSON_STR &&
             strcmp(json_get_str(&result), hstar_hex) == 0 &&
             strcmp(json_get_str(&result), active_hex) != 0;
        json_free(&result);

        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "getchaintip", &params, &result);
        const struct json_value *tip_height = json_get(&result, "height");
        const struct json_value *tip_hash = json_get(&result, "hash");
        ok = ok && tip_height && json_get_int(tip_height) == 1;
        ok = ok && tip_hash && tip_hash->type == JSON_STR &&
             strcmp(json_get_str(tip_hash), hstar_hex) == 0 &&
             strcmp(json_get_str(tip_hash), active_hex) != 0;
        json_free(&result);
        json_free(&params);

        init_single_int_param(&params, 1);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "getblockhash", &params,
                                     &result) &&
             result.type == JSON_STR &&
             strcmp(json_get_str(&result), hstar_hex) == 0;
        json_free(&result);
        json_free(&params);

        init_single_int_param(&params, 2);
        json_init(&result);
        ok = ok && !rpc_table_execute(&tbl, "getblockhash", &params,
                                      &result);
        ok = ok && result.type == JSON_STR &&
             strcmp(json_get_str(&result), active_hex) != 0 &&
             strstr(json_get_str(&result), "out of range") != NULL;
        json_free(&result);
        json_free(&params);

        init_single_str_param(&params, hstar_hex);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "getblockheader", &params,
                                     &result);
        const struct json_value *header_height = json_get(&result, "height");
        ok = ok && header_height && json_get_int(header_height) == 1;
        json_free(&result);
        json_free(&params);

        init_single_str_param(&params, active_hex);
        json_init(&result);
        ok = ok && !rpc_table_execute(&tbl, "getblockheader", &params,
                                      &result);
        ok = ok && result.type == JSON_STR &&
             strstr(json_get_str(&result), "not found") != NULL;
        json_free(&result);
        json_free(&params);

        init_single_str_param(&params, hstar_hex);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "getblock", &params, &result);
        const struct json_value *block_height = json_get(&result, "height");
        ok = ok && block_height && json_get_int(block_height) == 1;
        json_free(&result);
        json_free(&params);

        json_init(&params);
        json_set_array(&params);
        json_init(&result);
        ok = ok && rpc_table_execute(&health_tbl, "getsyncdetail", &params,
                                     &result);
        const struct json_value *chain = json_get(&result, "chain");
        const struct json_value *chain_height = json_get(chain, "height");
        const struct json_value *best_block = json_get(chain, "best_block");
        ok = ok && chain && chain_height && json_get_int(chain_height) == 1;
        ok = ok && best_block && best_block->type == JSON_STR &&
             strcmp(json_get_str(best_block), hstar_hex) == 0 &&
             strcmp(json_get_str(best_block), active_hex) != 0;
        json_free(&result);
        json_free(&params);

        json_init(&params);
        json_set_array(&params);
        json_init(&result);
        ok = ok && rpc_table_execute(&health_tbl, "getsyncdiag", &params,
                                     &result);
        const struct json_value *diag_chain_height =
            json_get(&result, "chain_height");
        const struct json_value *diag_header_height =
            json_get(&result, "best_header_height");
        ok = ok && diag_chain_height &&
             json_get_int(diag_chain_height) == 1;
        ok = ok && diag_header_height &&
             json_get_int(diag_header_height) == 3;
        json_free(&result);
        json_free(&params);

        init_single_str_param(&params, active_hex);
        json_init(&result);
        ok = ok && !rpc_table_execute(&tbl, "getblock", &params, &result);
        ok = ok && result.type == JSON_STR &&
             strstr(json_get_str(&result), "not found") != NULL;

        json_free(&params);
        json_free(&result);
        reducer_frontier_provable_tip_reset();
        rpc_health_set_state(NULL, NULL, NULL, NULL);
        rpc_blockchain_set_state(NULL, NULL, NULL);
        main_state_free(&ms);
        test_reset_shared_globals();

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("rpc_safety: getblockhash resolves sparse H* from node db... ");
    {
        test_reset_shared_globals();
        ensure_rpc_warmup_finished_once();

        struct main_state ms;
        struct block_index *blocks[3] = {0};
        struct node_db ndb;
        bool ndb_open = false;
        bool ok = rpc_safety_build_chain(&ms, blocks, 3);
        if (ok)
            ms.chain_active.chain[1] = NULL;
        ok = ok && node_db_open(&ndb, ":memory:");
        ndb_open = ok;
        ok = ok && rpc_safety_save_projected_block(&ndb, blocks[1]);

        rpc_blockchain_set_state(&ms, NULL, "/tmp");
        rpc_blockchain_set_node_db(&ndb);
        reducer_frontier_provable_tip_set(1);

        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_blockchain_rpc_commands(&tbl);

        char hstar_hex[65] = {0};
        char active_hex[65] = {0};
        if (blocks[1] && blocks[1]->phashBlock)
            uint256_get_hex(blocks[1]->phashBlock, hstar_hex);
        if (blocks[2] && blocks[2]->phashBlock)
            uint256_get_hex(blocks[2]->phashBlock, active_hex);

        struct json_value params = {0};
        struct json_value result = {0};
        json_init(&params);
        json_set_array(&params);
        json_init(&result);

        ok = ok && rpc_table_execute(&tbl, "getbestblockhash", &params,
                                     &result);
        ok = ok && result.type == JSON_STR &&
             strcmp(json_get_str(&result), hstar_hex) == 0 &&
             strcmp(json_get_str(&result), active_hex) != 0;
        json_free(&result);
        json_free(&params);

        init_single_int_param(&params, 1);
        json_init(&result);
        ok = ok && rpc_table_execute(&tbl, "getblockhash", &params,
                                     &result) &&
             result.type == JSON_STR &&
             strcmp(json_get_str(&result), hstar_hex) == 0 &&
             strcmp(json_get_str(&result), active_hex) != 0;
        json_free(&result);
        json_free(&params);

        init_single_int_param(&params, 2);
        json_init(&result);
        ok = ok && !rpc_table_execute(&tbl, "getblockhash", &params,
                                      &result);
        ok = ok && result.type == JSON_STR &&
             strstr(json_get_str(&result), "out of range") != NULL;

        json_free(&params);
        json_free(&result);
        rpc_blockchain_set_state(NULL, NULL, NULL);
        rpc_blockchain_set_node_db(NULL);
        reducer_frontier_provable_tip_reset();
        if (ndb_open)
            node_db_close(&ndb);
        main_state_free(&ms);
        test_reset_shared_globals();

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("rpc_safety: gettxoutsetinfo reports coin frontier... ");
    {
        test_reset_shared_globals();
        ensure_rpc_warmup_finished_once();
        progress_store_close();

        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "rpc_safety", "coin_frontier");

        struct main_state ms;
        struct block_index *blocks[4] = {0};
        bool ok = rpc_safety_build_chain(&ms, blocks, 4);
        ok = ok && progress_store_open(dir);
        sqlite3 *db = progress_store_db();
        ok = ok && db && rpc_safety_seed_coin_frontier(db, blocks[1]);

        rpc_blockchain_set_state(&ms, NULL, "/tmp");

        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_blockchain_rpc_commands(&tbl);

        struct json_value params = {0};
        struct json_value result = {0};
        json_init(&params);
        json_set_array(&params);
        json_init(&result);

        ok = ok && rpc_table_execute(&tbl, "gettxoutsetinfo", &params,
                                     &result);
        const struct json_value *height = json_get(&result, "height");
        const struct json_value *bestblock = json_get(&result, "bestblock");
        const struct json_value *txs = json_get(&result, "transactions");
        const struct json_value *outs = json_get(&result, "txouts");
        char coin_hex[65] = {0};
        char active_hex[65] = {0};
        if (blocks[1] && blocks[1]->phashBlock)
            uint256_get_hex(blocks[1]->phashBlock, coin_hex);
        if (blocks[3] && blocks[3]->phashBlock)
            uint256_get_hex(blocks[3]->phashBlock, active_hex);
        ok = ok && height && json_get_int(height) == 1;
        ok = ok && bestblock && bestblock->type == JSON_STR &&
             strcmp(json_get_str(bestblock), coin_hex) == 0 &&
             strcmp(json_get_str(bestblock), active_hex) != 0;
        ok = ok && txs && json_get_int(txs) == 1;
        ok = ok && outs && json_get_int(outs) == 1;

        json_free(&params);
        json_free(&result);
        rpc_blockchain_set_state(NULL, NULL, NULL);
        progress_store_close();
        test_cleanup_tmpdir(dir);
        main_state_free(&ms);
        test_reset_shared_globals();

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("rpc_safety: summary RPCs do not borrow unresolved active tip... ");
    {
        test_reset_shared_globals();
        ensure_rpc_warmup_finished_once();

        struct main_state ms;
        struct block_index *blocks[4] = {0};
        bool ok = rpc_safety_build_chain(&ms, blocks, 4);
        if (ok)
            ms.chain_active.chain[1] = NULL;
        reducer_frontier_provable_tip_set(1);

        struct rpc_table chain_tbl;
        rpc_table_init(&chain_tbl);
        rpc_blockchain_set_state(&ms, NULL, "/tmp");
        register_blockchain_rpc_commands(&chain_tbl);

        struct rpc_table misc_tbl;
        rpc_table_init(&misc_tbl);
        rpc_misc_set_state(&ms);
        register_misc_rpc_commands(&misc_tbl);

        struct rpc_table mining_tbl;
        rpc_table_init(&mining_tbl);
        rpc_mining_set_state(&ms, NULL, NULL);
        register_mining_rpc_commands(&mining_tbl);

        struct json_value params = {0};
        struct json_value result = {0};
        json_init(&params);
        json_set_array(&params);
        json_init(&result);

        /* getblockchaininfo must NOT borrow the unresolved active tip (height
         * 3). When the H* slot is unresolved it now returns a VALID IBD-shaped
         * object (parseable JSON) rather than the old bare "No provable tip"
         * error string — but it still reports the PROVABLE height (H* == 1),
         * never the active/lookahead tip, with initialblockdownload=true. */
        ok = ok && rpc_table_execute(&chain_tbl, "getblockchaininfo",
                                     &params, &result);
        {
            const struct json_value *gbi_blocks = json_get(&result, "blocks");
            const struct json_value *gbi_ibd =
                json_get(&result, "initialblockdownload");
            ok = ok && gbi_blocks && json_get_int(gbi_blocks) == 1;
            ok = ok && gbi_ibd && json_get_bool(gbi_ibd);
        }
        json_free(&result);

        json_init(&result);
        ok = ok && rpc_table_execute(&misc_tbl, "getinfo", &params,
                                     &result);
        const struct json_value *info_blocks = json_get(&result, "blocks");
        ok = ok && info_blocks && json_get_int(info_blocks) == 1;
        json_free(&result);

        json_init(&result);
        ok = ok && rpc_table_execute(&mining_tbl, "getmininginfo", &params,
                                     &result);
        const struct json_value *mining_blocks = json_get(&result, "blocks");
        ok = ok && mining_blocks && json_get_int(mining_blocks) == 1;

        json_free(&params);
        json_free(&result);
        rpc_blockchain_set_state(NULL, NULL, NULL);
        rpc_misc_set_state(NULL);
        rpc_mining_set_state(NULL, NULL, NULL);
        reducer_frontier_provable_tip_reset();
        main_state_free(&ms);
        test_reset_shared_globals();

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    printf("rpc_safety: chainstate guard rejects unresolved active tip... ");
    {
        ensure_rpc_warmup_finished_once();

        struct main_state *ms = calloc(1, sizeof(*ms));
        struct coins_view_cache *coins_tip = calloc(1, sizeof(*coins_tip));
        struct wallet *wallet = calloc(1, sizeof(*wallet));
        struct node_db *ndb = calloc(1, sizeof(*ndb));
        struct rpc_table *rawtx_tbl = calloc(1, sizeof(*rawtx_tbl));
        struct rpc_table *inspect_tbl = calloc(1, sizeof(*inspect_tbl));
        struct rpc_table *wallet_diag_tbl = calloc(1, sizeof(*wallet_diag_tbl));
        struct rpc_table *wallet_rescan_tbl = calloc(1, sizeof(*wallet_rescan_tbl));
        struct rpc_table *repair_tbl = calloc(1, sizeof(*repair_tbl));

        bool ok = ms && coins_tip && wallet && ndb && rawtx_tbl &&
                  inspect_tbl && wallet_diag_tbl && wallet_rescan_tbl &&
                  repair_tbl;
        if (ok) {
            build_unresolved_tip_state(ms, 100);
            wallet_init(wallet);
            ndb->open = true;
        }

        if (!ok) {
            free(ms);
            free(coins_tip);
            free(wallet);
            free(ndb);
            free(rawtx_tbl);
            free(inspect_tbl);
            free(wallet_diag_tbl);
            free(wallet_rescan_tbl);
            free(repair_tbl);
            printf("FAIL\n");
            return failures + 1;
        }

        rpc_table_init(rawtx_tbl);
        rpc_rawtx_set_state(ms, NULL, coins_tip, "/tmp");
        register_rawtransaction_rpc_commands(rawtx_tbl);

        rpc_table_init(inspect_tbl);
        rpc_chain_inspect_set_state(ms, "/tmp", NULL, coins_tip, NULL);
        register_chain_inspect_rpc_commands(inspect_tbl);

        wallet_rpc_context_set_base(wallet, ms, "/tmp", NULL, NULL, NULL);
        wallet_rpc_context_set_node_db(NULL);
        wallet_rpc_context_set_coins_tip(coins_tip);

        rpc_table_init(wallet_diag_tbl);
        register_wallet_diagnostic_rpc_commands(wallet_diag_tbl);

        wallet_rpc_context_set_node_db(NULL);
        rpc_table_init(wallet_rescan_tbl);
        register_wallet_rescan_rpc_commands(wallet_rescan_tbl);

        rpc_table_init(repair_tbl);
        rpc_repair_set_state(ms, coins_tip, ndb, "/tmp",
                             chain_params_get());
        register_repair_rpc_commands(repair_tbl);
        register_backfill_header_solutions_rpc_commands(repair_tbl);

        struct json_value params = {0};
        struct json_value result = {0};

        init_single_str_param(
            &params,
            "0000000000000000000000000000000000000000000000000000000000000001");
        json_init(&result);
        ok = !rpc_table_execute(rawtx_tbl, "getrawtransaction", &params,
                                &result) &&
             result_is_chainstate_guard_error(&result, "getrawtransaction");
        json_free(&params);
        json_free(&result);

        init_single_str_param(
            &params,
            "0000000000000000000000000000000000000000000000000000000000000002");
        json_init(&result);
        ok = ok && !rpc_table_execute(inspect_tbl, "gettxdetail", &params,
                                      &result) &&
             result_is_chainstate_guard_error(&result, "gettxdetail");
        json_free(&params);
        json_free(&result);

        init_single_str_param(
            &params,
            "0000000000000000000000000000000000000000000000000000000000000003");
        json_init(&result);
        ok = ok && !rpc_table_execute(wallet_diag_tbl, "getchaincoins",
                                      &params, &result) &&
             result_is_chainstate_guard_error(&result, "getchaincoins");
        json_free(&params);
        json_free(&result);

        json_init(&params);
        json_set_array(&params);
        json_init(&result);
        ok = ok && !rpc_table_execute(wallet_rescan_tbl, "syncwalletfromdb",
                                      &params, &result) &&
             result_is_chainstate_guard_error(&result, "syncwalletfromdb");
        json_free(&params);
        json_free(&result);

        json_init(&params);
        json_set_array(&params);
        json_init(&result);
        ok = ok && !rpc_table_execute(repair_tbl, "repairutxos", &params,
                                      &result) &&
             result_is_chainstate_guard_error(&result, "repairutxos");
        json_free(&params);
        json_free(&result);

        main_state_free(ms);
        wallet_free(wallet);
        free(ms);
        free(coins_tip);
        free(wallet);
        free(ndb);
        free(rawtx_tbl);
        free(inspect_tbl);
        free(wallet_diag_tbl);
        free(wallet_rescan_tbl);
        free(repair_tbl);

        if (ok) printf("OK\n");
        else    { printf("FAIL\n"); failures++; }
    }

    return failures;
}
