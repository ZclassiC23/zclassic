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

#include "controllers/diagnostics_controller.h"
#include "jobs/reducer_frontier.h"
#include "jobs/refold_progress.h"
#include "json/json.h"
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

    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/node.db", datadir);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID,
                               "DATADIR_PATH_TOO_LONG", "normalize", false,
                               false, "datadir path too long", datadir);
        return;
    }

    /* SQLITE_OPEN_READONLY (no CREATE): a missing node.db fails closed with
     * SQLITE_CANTOPEN rather than silently creating one — same open mode
     * tools/sqlq.c uses for this exact "copied fixture datadir" story. */
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        char evidence[512];
        snprintf(evidence, sizeof(evidence), "%s: %s", path,
                 db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db)
            sqlite3_close(db);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED,
                               "NODE_DB_UNAVAILABLE", "execute", true, false,
                               "node.db not found or unreadable at datadir",
                               evidence);
        return;
    }
    /* Belt-and-braces: refuse any accidental write on this handle (same
     * PRAGMA tools/sqlq.c's sibling xck_open_ro() sets). */
    (void)sqlite3_exec(db, "PRAGMA query_only=ON", NULL, NULL, NULL);
    sqlite3_busy_timeout(db, 2000);

    struct json_value result;
    json_init(&result);
    bool ok = dbquery_execute(db, sql, limit, &result);
    sqlite3_close(db);

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

    char kernel_path[PROGRESS_STORE_PATH_MAX];
    if (!consensus_db_kernel_store_path(datadir, kernel_path,
                                        sizeof(kernel_path))) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID,
                               "DATADIR_PATH_TOO_LONG", "normalize", false,
                               false, "datadir path too long", datadir);
        return;
    }

    /* Fail closed BEFORE progress_store_open(): that call opens
     * consensus.db READWRITE|CREATE, so pointed at a directory with no
     * kernel store it would silently CREATE a fresh empty one and report a
     * meaningless H*=0 instead of "wrong path". consensus_db_kernel_store_
     * path() above resolves to consensus.db OR the legacy progress.kv
     * *name*, whichever exists — but doesn't itself require existence, so
     * confirm the file is actually there first. */
    if (access(kernel_path, F_OK) != 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED,
                               "KERNEL_STORE_NOT_FOUND", "execute", true,
                               false,
                               "no consensus.db/progress.kv at this datadir",
                               kernel_path);
        return;
    }

    if (!progress_store_open(datadir)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED,
                               "KERNEL_STORE_UNAVAILABLE", "execute", true,
                               false,
                               "failed to open the kernel store at datadir",
                               kernel_path);
        return;
    }
    sqlite3 *db = progress_store_db();
    if (!db) {
        progress_store_close();
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL,
                               "KERNEL_STORE_UNAVAILABLE", "execute", false,
                               false, "kernel store opened but handle is NULL",
                               kernel_path);
        return;
    }

    /* Refresh the process-wide refold-in-progress cache from THIS datadir's
     * OWN persisted progress_meta before folding — without this the cache
     * defaults conservatively to "not refolding", which would misreport a
     * copied mid-refold datadir's H* floor. See refold_progress.h. */
    (void)refold_progress_refresh(db);

    int32_t hstar = 0, served_floor = 0;
    progress_store_tx_lock();
    bool ok = reducer_frontier_compute_hstar(db, &hstar, &served_floor);
    progress_store_tx_unlock();

    if (!ok) {
        progress_store_close();
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
    progress_store_close();
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
}
