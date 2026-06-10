/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * chainstate-backed RPCs must fail cleanly when the active
 * chain height points at a missing block-index entry. */

#include "test/test_helpers.h"
#include "controllers/blockchain_controller.h"
#include "controllers/chain_inspect_controller.h"
#include "controllers/repair_controller.h"
#include "controllers/transaction_controller.h"
#include "controllers/wallet_diagnostic_controller.h"
#include "controllers/wallet_helpers.h"
#include "controllers/wallet_rescan_controller.h"
#include "json/json.h"
#include "models/database.h"
#include "rpc/server.h"
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
