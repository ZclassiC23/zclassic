/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * OFFLINE_COPY native leaves: inspect a STOPPED or COPIED datadir's SQLite
 * stores directly, with NO node contact and NO RPC.
 *
 * The gap this closes: `core.storage.query` (dbquery_controller.c) is
 * scoped to a RUNNING node's RPC only, and `dumpstate reducer_frontier`
 * likewise answers only for the live process. Neither can answer "what's
 * H* in this datadir I just copied off a stalled node?" without booting a
 * full second node against it. `tools/sqlq.c` exists precisely for this
 * ("cannot reach a copied fixture datadir") but is an unregistered raw
 * binary requiring hand-known table/column names — the enum value
 * ZCL_COMMAND_SCOPE_OFFLINE_COPY (lib/kernel/include/kernel/command_
 * registry.h) has existed with zero leaves using it until this file.
 *
 * Both leaves below open an AD HOC handle straight at the caller-supplied
 * `--datadir=<path>` and run the SAME SELECT-only production primitive a
 * live node would use — dbquery_execute() for storage.query.offline,
 * reducer_frontier_compute_hstar() for sync.frontier.offline — so the
 * safety envelope (SELECT-only, no secrets, budget/row caps for the
 * former; the pure L0 H* fold for the latter) is identical to the
 * RPC-backed leaves, just without requiring a booted node. */

#include "command/native_command.h"

#include "chain/chainparams.h"
#include "controllers/diagnostics_controller.h"
#include "jobs/reducer_frontier.h"
#include "jobs/refold_progress.h"
#include "json/json.h"
#include "models/database.h"
#include "storage/consensus_db.h"
#include "storage/progress_store.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ── core.storage.query.offline ──────────────────────────────────────── */

void zcl_native_handle_core_storage_query_offline(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;

    const char *datadir = json_get_str(json_get(request->input, "datadir"));
    if (!datadir || !datadir[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_DATADIR",
                               "normalize", false, false,
                               "no datadir given", "core.storage.query.offline");
        return;
    }
    const char *sql = json_get_str(json_get(request->input, "sql"));
    int64_t limit = json_get_int_or(request->input, "limit", 10);

    /* The shared read-only open (command/native_command.h): READONLY, no
     * CREATE, so a missing node.db fails closed rather than silently
     * creating one, plus PRAGMA query_only as a second refusal of any
     * write — the same story tools/sqlq.c's xck_open_ro() serves. */
    sqlite3 *db = NULL;
    struct node_db ndb;
    if (!zcl_native_node_db_require_readonly(datadir, reply,
                                             "this datadir's node.db",
                                             &db, &ndb))
        return;
    /* Copied BEFORE the close: the shim's path field is cleared with it. */
    char path[sizeof(ndb.path)];
    snprintf(path, sizeof(path), "%s", ndb.path);

    struct json_value result;
    json_init(&result);
    bool ok = dbquery_execute(db, sql, limit, &result);
    zcl_native_node_db_close_readonly(&db, &ndb);

    if (!ok) {
        const char *msg = json_get_str(&result);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "QUERY_REJECTED",
                               "execute", false, false,
                               msg && msg[0] ? msg : "query rejected", path);
        json_free(&result);
        return;
    }

    (void)json_push_kv_str(&result, "datadir", datadir);
    json_copy(&reply->data, &result);
    json_free(&result);
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
}

/* ── core.sync.frontier.offline ──────────────────────────────────────── */

void zcl_native_handle_core_sync_frontier_offline(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;

    const char *datadir = json_get_str(json_get(request->input, "datadir"));
    if (!datadir || !datadir[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_DATADIR",
                               "normalize", false, false,
                               "no datadir given", "core.sync.frontier.offline");
        return;
    }

    /* The shared read-only kernel-store open (command/native_command.h).
     *
     * This leaf used to call progress_store_open(datadir), which is
     * READWRITE|CREATE, runs the progress.kv rename migration, ensures the
     * kernel schema, and — on a failed integrity check — rename()s
     * consensus.db aside to consensus.db.corrupt-<ts> and installs a fresh
     * empty one. So a question about a copied datadir ("what is H* here?")
     * could answer by DESTROYING the append-only fact log it was asked
     * about, and an operator file that merely happened to sit at that path
     * was quarantined outright. Read-only closes all of it: no CREATE (a
     * mistyped datadir fails instead of minting an empty store and reporting
     * a meaningless H*=0), no migration, no schema ensure, no quarantine —
     * and no claim on the process singleton either. */
    sqlite3 *db = NULL;
    char kernel_path[1200];
    enum zcl_node_db_ro_status ro_st = zcl_native_kernel_store_open_readonly(
        datadir, &db, kernel_path, sizeof(kernel_path));
    if (ro_st != ZCL_NODE_DB_RO_OK) {
        switch (ro_st) {
        case ZCL_NODE_DB_RO_PATH_TOO_LONG:
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                   ZCL_COMMAND_EXIT_INVALID,
                                   "DATADIR_PATH_TOO_LONG", "normalize", false,
                                   false, "datadir path too long", datadir);
            return;
        case ZCL_NODE_DB_RO_ABSENT:
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                                   ZCL_COMMAND_EXIT_BLOCKED,
                                   "KERNEL_STORE_NOT_FOUND", "execute", true,
                                   false,
                                   "no consensus.db/progress.kv at this "
                                   "datadir", kernel_path);
            return;
        case ZCL_NODE_DB_RO_UNREADABLE:
        case ZCL_NODE_DB_RO_NO_DATADIR:
        default:
            /* Distinct from NOT_FOUND on purpose: the file IS there and
             * would not open read-only, which is never the same answer as
             * "this datadir has no kernel store". */
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                                   ZCL_COMMAND_EXIT_BLOCKED,
                                   "KERNEL_STORE_UNAVAILABLE", "execute", true,
                                   false,
                                   "the kernel store at this datadir exists "
                                   "but would not open read-only", kernel_path);
            return;
        }
    }

    /* A one-shot native CLI process has no app_init(): reducer_frontier_
     * compute_hstar() reads the compiled anchor via chain_params_get(),
     * which asserts pCurrentParams non-NULL — fatal if nothing ever called
     * chain_params_select() (the RPC bridge path selects it in
     * bridge_ensure_rpc_client(); this OFFLINE_COPY path never goes through
     * the bridge, so it must select for itself). Idempotent — safe even if
     * something upstream already selected. Mainnet-only: the offline-copy
     * story targets mainnet datadirs; a testnet/regtest copy would need a
     * network hint this leaf does not yet accept. */
    chain_params_select(CHAIN_MAIN);

    /* Refresh the process-wide refold-in-progress cache from THIS datadir's
     * OWN persisted progress_meta before folding — without this the cache
     * defaults conservatively to "not refolding", which would misreport a
     * copied mid-refold datadir's H* floor. See refold_progress.h. */
    (void)refold_progress_refresh(db);

    int32_t hstar = 0, served_floor = 0;
    /* reducer_frontier_compute_hstar's documented contract is that the caller
     * holds this lock; honoured even though `db` is this call's own private
     * handle and not the singleton, so the contract cannot rot if the fold
     * ever reaches a helper that assumes it. */
    progress_store_tx_lock();
    bool ok = reducer_frontier_compute_hstar(db, &hstar, &served_floor);
    progress_store_tx_unlock();

    if (!ok) {
        sqlite3_close(db);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL,
                               "HSTAR_COMPUTE_FAILED", "execute", false,
                               false,
                               "reducer_frontier_compute_hstar failed",
                               kernel_path);
        return;
    }

    (void)json_push_kv_str(&reply->data, "datadir", datadir);
    (void)json_push_kv_str(&reply->data, "kernel_store", kernel_path);
    (void)json_push_kv_int(&reply->data, "hstar", hstar);
    (void)json_push_kv_int(&reply->data, "served_floor", served_floor);
    (void)json_push_kv_int(&reply->data, "compiled_anchor",
                           REDUCER_FRONTIER_TRUSTED_ANCHOR);
    (void)json_push_kv_bool(&reply->data, "refold_in_progress",
                            refold_in_progress());
    sqlite3_close(db);
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
}
