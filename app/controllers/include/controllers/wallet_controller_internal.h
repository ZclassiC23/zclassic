/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Internal shared includes + helpers for the transparent wallet RPC
 * controller. Included by wallet_controller*.c files only — not part
 * of the public API. The public entry points stay in
 * controllers/wallet_controller.h. */

#ifndef ZCL_CONTROLLERS_WALLET_CONTROLLER_INTERNAL_H
#define ZCL_CONTROLLERS_WALLET_CONTROLLER_INTERNAL_H

#include "controllers/wallet_controller.h"
#include "rpc/client.h"
#include "controllers/rpc_chainstate_guard.h"
#include "controllers/wallet_helpers.h"
#include "controllers/wallet_shielded_controller.h"
#include "controllers/wallet_diagnostic_controller.h"
#include "controllers/wallet_rescan_controller.h"
#include "controllers/strong_params.h"
#include "config/runtime.h"
#include "util/log_macros.h"
#include "wallet/wallet.h"
#include "chain/chainparams.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "keys/key_io.h"
#include "script/standard.h"
#include "support/cleanse.h"
#include "core/utiltime.h"
#include "validation/main_state.h"
#include "validation/txmempool.h"
#include "wallet/wallet_sqlite.h"
#include "net/connman.h"
#include "core/hash.h"
#include "models/database.h"
#include "models/utxo.h"
#include "models/wallet_tx.h"
#include "controllers/sync_controller.h"
#include "controllers/wallet_scan.h"
#include "coins/coins_view.h"
#include "core/serialize.h"
#include "domain/encoding/base58.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "wallet/wallet_canary.h"
#include "services/wallet_backup_service.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Shared accessor for the current wallet RPC context. */
static inline struct wallet_rpc_context *wallet_ctx(void)
{
    return wallet_rpc_context_current();
}

struct wallet_flush_lane_ctx {
    struct wallet_sqlite *wallet_db;
    struct wallet *wallet;
    struct zcl_result result;
};

static inline bool wallet_controller_flush_lane_write(struct node_db *ndb,
                                                      void *ctx)
{
    struct wallet_flush_lane_ctx *flush = ctx;

    (void)ndb;
    if (!flush || !flush->wallet_db || !flush->wallet) {
        if (flush)
            flush->result = ZCL_ERR(-1, "wallet flush lane: invalid ctx");
        return false;
    }
    if (ndb && ndb->sync_in_batch && !node_db_sync_flush(ndb)) {
        flush->result = ZCL_ERR(-1, "wallet flush lane: node.db batch flush failed");
        return false;
    }
    flush->result = wallet_sqlite_flush_r(flush->wallet_db, flush->wallet);
    return flush->result.ok;
}

static inline struct zcl_result
wallet_controller_flush_r(struct wallet_rpc_context *ctx)
{
    if (!ctx || !ctx->wallet_db || !ctx->wallet)
        return ZCL_ERR(-1, "wallet flush: invalid ctx");

    struct wallet_flush_lane_ctx flush = {
        .wallet_db = ctx->wallet_db,
        .wallet = ctx->wallet,
        .result = ZCL_ERR(-1, "wallet flush lane did not run"),
    };
    struct db_service *dbsvc = app_runtime_db_service();
    if (dbsvc && db_service_is_started(dbsvc) && ctx->node_db &&
        db_service_node_db(dbsvc) == ctx->node_db) {
        if (!db_service_run_write(dbsvc, wallet_controller_flush_lane_write,
                                  &flush) && flush.result.ok) {
            LOG_WARN("wallet", "wallet flush lane failed without detail");
            flush.result = ZCL_ERR(-1, "wallet flush lane failed");
        }
        return flush.result;
    }
    return wallet_sqlite_flush_r(ctx->wallet_db, ctx->wallet);
}

/* ── Handlers (grouped into sibling .c files) ─────────────── */

/* wallet_controller_keys.c — key/address import-export */
bool rpc_dumpprivkey(const struct json_value *params, bool help,
                     struct json_value *result);
bool rpc_importprivkey(const struct json_value *params, bool help,
                       struct json_value *result);
bool rpc_importaddress(const struct json_value *params, bool help,
                       struct json_value *result);

/* wallet_controller_history.c — transaction listing */
bool rpc_listtransactions(const struct json_value *params, bool help,
                          struct json_value *result);
bool rpc_gettransaction(const struct json_value *params, bool help,
                        struct json_value *result);

/* wallet_controller_multisig.c — multisig + sendmany */
bool rpc_createmultisig(const struct json_value *params, bool help,
                        struct json_value *result);
bool rpc_sendmany(const struct json_value *params, bool help,
                  struct json_value *result);
bool rpc_addmultisigaddress(const struct json_value *params, bool help,
                            struct json_value *result);

#endif /* ZCL_CONTROLLERS_WALLET_CONTROLLER_INTERNAL_H */
