/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * The ONE read-only <datadir>/node.db open shared by native command leaves.
 *
 * Purpose: a leaf whose declared effect is READ must be able to open the
 * node database without any chance of writing to it. Before this file every
 * such leaf hand-rolled the same twenty lines, and six leaves got it wrong
 * in the other direction — they called node_db_open(), the BOOT ceremony,
 * which opens READWRITE|CREATE and then creates schema, migrates,
 * rename()s the file aside on a failed quick_check, and DELETEs the
 * snapshot_staging rows. Because `datadir` defaults to the CLI's resolved
 * datadir, running one of those leaves with no arguments did that to the
 * operator's LIVE node. See the contract comment on
 * zcl_native_node_db_open_readonly in command/native_command.h.
 *
 * Nothing here creates, migrates, schema-initializes, quarantines, renames
 * or deletes. The only sqlite3 calls are open_v2(READONLY), the query_only
 * pragma, busy_timeout, and close. */

#define _GNU_SOURCE
#include "command/native_command.h"

#include "base/log_macros.h"
#include "models/database.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* A locked WAL must give up rather than park a cursor on a connection the
 * live node is writing through (see the wallet write-wedge post-mortem). */
#define ZCL_NODE_DB_RO_BUSY_MS 2000

enum zcl_node_db_ro_status zcl_native_node_db_open_readonly(
    const char *datadir, sqlite3 **db_out, struct node_db *ndb_out,
    char *path_out, size_t path_size)
{
    if (path_out && path_size)
        path_out[0] = '\0';
    if (db_out)
        *db_out = NULL;
    if (ndb_out)
        memset(ndb_out, 0, sizeof(*ndb_out));

    if (!db_out || !ndb_out)
        LOG_RETURN(ZCL_NODE_DB_RO_NO_DATADIR, "cmd",
                   "read-only node.db open called without out-parameters");
    if (!datadir || !datadir[0])
        return ZCL_NODE_DB_RO_NO_DATADIR;

    char path[1200];
    int n = snprintf(path, sizeof(path), "%s/node.db", datadir);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return ZCL_NODE_DB_RO_PATH_TOO_LONG;
    if (path_out && path_size) {
        if (path_size <= (size_t)n)
            return ZCL_NODE_DB_RO_PATH_TOO_LONG;
        memcpy(path_out, path, (size_t)n + 1);
    }

    /* Absent vs unreadable are answered BEFORE the open, because
     * sqlite3_open_v2 collapses both into SQLITE_CANTOPEN and a caller that
     * may proceed on "absent" must never proceed on "unreadable". F_OK only
     * stats; it creates nothing. */
    if (access(path, F_OK) != 0)
        return ZCL_NODE_DB_RO_ABSENT;

    sqlite3 *db = NULL;
    /* READONLY only: no CREATE (a typo'd datadir must fail, never mint a
     * database) and no READWRITE (this handle answers a read leaf). */
    int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR("cmd", "node.db present but not readable: %s: %s", path,
                  db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db)
            sqlite3_close(db);
        return ZCL_NODE_DB_RO_UNREADABLE;
    }
    /* Second, connection-wide refusal of any write that the open flags
     * somehow let through (an ATTACHed db, a temp-table INSERT). */
    (void)sqlite3_exec(db, "PRAGMA query_only=ON", NULL, NULL, NULL);
    sqlite3_busy_timeout(db, ZCL_NODE_DB_RO_BUSY_MS);

    /* Third, TOUCH THE FILE. sqlite3_open_v2 is lazy: it does not read the
     * header, so a 47-byte text file, a truncated database, or any other
     * non-database opens with SQLITE_OK and only fails later, at the first
     * statement — by which point the caller has already been told OK and is
     * inside its own query path, where "it did not open" and "it opened and
     * held nothing" are the same shape. That is the exact confusion this
     * whole helper exists to remove, so pay one page read here.
     *
     * This matters beyond tidiness: zcl_native_node_db_require_readonly's
     * callers include two pre-flights that spend a fee (core.identity.anchor,
     * the zdir register path). They treat ABSENT as "proceed, this is a
     * legitimate first anchor" and anything else as fatal. A corrupt node.db
     * returning OK made the revocation lookup fail, read as "not revoked",
     * and spent the fee on a check that never ran. */
    if (sqlite3_exec(db, "PRAGMA schema_version", NULL, NULL, NULL)
        != SQLITE_OK) {
        LOG_ERROR("cmd", "node.db present but not a readable database: %s: %s",
                  path, sqlite3_errmsg(db));
        sqlite3_close(db);
        return ZCL_NODE_DB_RO_UNREADABLE;
    }

    *db_out = db;
    ndb_out->db = db;
    ndb_out->open = true;
    snprintf(ndb_out->path, sizeof(ndb_out->path), "%s", path);
    return ZCL_NODE_DB_RO_OK;
}

bool zcl_native_node_db_require_readonly(
    const char *datadir, struct zcl_command_reply *reply, const char *what,
    sqlite3 **db_out, struct node_db *ndb_out)
{
    if (!reply)
        LOG_FAIL("cmd", "read-only node.db require called without a reply");
    const char *noun = (what && what[0]) ? what : "the node database";

    char path[1200];
    enum zcl_node_db_ro_status st = zcl_native_node_db_open_readonly(
        datadir, db_out, ndb_out, path, sizeof(path));
    if (st == ZCL_NODE_DB_RO_OK)
        return true;

    char message[512];
    switch (st) {
    case ZCL_NODE_DB_RO_NO_DATADIR:
        snprintf(message, sizeof(message),
                 "no datadir resolved for %s — pass --datadir", noun);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_DATADIR",
                               "normalize", false, false, message, "");
        return false;
    case ZCL_NODE_DB_RO_PATH_TOO_LONG:
        snprintf(message, sizeof(message),
                 "datadir path too long for %s", noun);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID,
                               "DATADIR_PATH_TOO_LONG", "normalize", false,
                               false, message, datadir ? datadir : "");
        return false;
    case ZCL_NODE_DB_RO_ABSENT:
        snprintf(message, sizeof(message),
                 "no node.db at that datadir, so %s cannot be read — check "
                 "--datadir, or boot the node once to create it", noun);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED,
                               "NODE_DB_UNAVAILABLE", "execute", true, false,
                               message, path);
        return false;
    case ZCL_NODE_DB_RO_UNREADABLE:
    default:
        /* Distinct from ABSENT on purpose: the file IS there and we could
         * not read it, which is never the same answer as "empty". */
        snprintf(message, sizeof(message),
                 "node.db exists but would not open read-only, so %s could "
                 "not be read — check permissions and that the path is a "
                 "SQLite database", noun);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED,
                               "NODE_DB_UNREADABLE", "execute", true, false,
                               message, path);
        return false;
    }
}

void zcl_native_node_db_close_readonly(sqlite3 **db, struct node_db *ndb)
{
    sqlite3 *handle = db ? *db : (ndb ? ndb->db : NULL);
    if (handle)
        sqlite3_close(handle);
    if (db)
        *db = NULL;
    /* The shim never owned prepared statements, so there is nothing for
     * node_db_close to finalize — just drop the borrowed pointer. */
    if (ndb)
        memset(ndb, 0, sizeof(*ndb));
}
