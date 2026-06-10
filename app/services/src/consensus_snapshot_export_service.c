/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "services/consensus_snapshot_export_service.h"

#include "util/ar_step_readonly.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

static struct zcl_result export_exec_checked(sqlite3 *db, const char *sql,
                                             const char *label)
{
    if (!db || !sql) {
        return ZCL_ERR(-1, "export exec %s: NULL %s",
                       label ? label : "(unknown)",
                       !db ? "db" : "sql");
    }

    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        return ZCL_ERR(-2, "export exec %s failed: %s",
                       label ? label : "(unknown)",
                       sqlite3_errmsg(db));
    }
    return ZCL_OK;
}

static struct zcl_result export_prepare_checked(sqlite3 *db, const char *sql,
                                                sqlite3_stmt **stmt,
                                                const char *label)
{
    if (!db || !sql || !stmt) {
        return ZCL_ERR(-1, "export prepare %s: NULL %s",
                       label ? label : "(unknown)",
                       !db ? "db" : !sql ? "sql" : "stmt");
    }

    if (sqlite3_prepare_v2(db, sql, -1, stmt, NULL) != SQLITE_OK || !*stmt) {
        return ZCL_ERR(-2, "export prepare %s failed: %s",
                       label ? label : "(unknown)",
                       sqlite3_errmsg(db));
    }
    return ZCL_OK;
}

static struct zcl_result export_step_checked(sqlite3_stmt *stmt, sqlite3 *db,
                                             const char *label)
{
    if (!stmt || !db) {
        return ZCL_ERR(-1, "export step %s: NULL %s",
                       label ? label : "(unknown)",
                       !stmt ? "stmt" : "db");
    }

    /* Snapshot export writes only to a side database owned by this service.
     * AR-managed model writes still go through the normal AR lifecycle. */
    int rc = AR_STEP_ROW_READONLY(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        return ZCL_ERR(-2, "export step %s failed: rc=%d err=%s",
                       label ? label : "(unknown)", rc, sqlite3_errmsg(db));
    }
    return ZCL_OK;
}

struct zcl_result consensus_snapshot_export_service_run(const char *datadir)
{
    if (!datadir)
        return ZCL_ERR(-1, "export_snapshot: NULL datadir");

    char src_path[576], dst_path[576];
    snprintf(src_path, sizeof(src_path), "%s/node.db", datadir);
    snprintf(dst_path, sizeof(dst_path), "%s/consensus_snapshot.db", datadir);

    struct stat src_st;
    if (stat(src_path, &src_st) != 0 || src_st.st_size < 1000000) {
        return ZCL_ERR(-2, "export_snapshot: %s missing or too small",
                       src_path);
    }

    /* Refuse to clobber a downloaded snapshot with an empty rebuild.
     * On a fresh node, node.db has only the genesis-era UTXOs that
     * block-by-block IBD has produced so far. Exporting that would
     * also unlink the downloaded consensus_snapshot.db that the next
     * boot needs to import, destroying the secure-snapshot fast path
     * for any node that runs file_service and then restarts before
     * full chain catchup. */
    {
        sqlite3 *probe = NULL;
        int64_t src_utxos = 0;
        if (sqlite3_open_v2(src_path, &probe,
                            SQLITE_OPEN_READONLY, NULL) == SQLITE_OK
            && probe) {
            sqlite3_stmt *q = NULL;
            if (sqlite3_prepare_v2(probe,
                    "SELECT COUNT(*) FROM utxos",
                    -1, &q, NULL) == SQLITE_OK && q) {
                if (sqlite3_step(q) == SQLITE_ROW)  // raw-sql-ok:read-only-probe
                    src_utxos = sqlite3_column_int64(q, 0);
                sqlite3_finalize(q);
            }
            sqlite3_close(probe);
        }
        if (src_utxos < 1000) {
            return ZCL_ERR(
                -3,
                "export_snapshot: source utxos=%lld is below the 1000-row "
                "threshold; preserving any downloaded consensus_snapshot.db "
                "so the next boot can import it",
                (long long)src_utxos);
        }
    }

    unlink(dst_path);

    sqlite3 *src_db = NULL;
    sqlite3 *dst_db = NULL;
    bool src_db_opened = false;
    bool dst_db_opened = false;
    bool dst_txn_open = false;
    bool src_attached = false;
    struct zcl_result result = ZCL_OK;

    if (sqlite3_open_v2(src_path, &src_db, SQLITE_OPEN_READONLY, NULL)
        != SQLITE_OK || !src_db) {
        result = ZCL_ERR(-4, "export_snapshot: cannot open source db %s",
                         src_path);
        goto export_cleanup;
    }
    src_db_opened = true;

    if (sqlite3_open_v2(dst_path, &dst_db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK ||
        !dst_db) {
        result = ZCL_ERR(-5, "export_snapshot: cannot create destination db %s",
                         dst_path);
        goto export_cleanup;
    }
    dst_db_opened = true;

    struct zcl_result step = export_exec_checked(dst_db,
        "PRAGMA journal_mode=WAL", "set journal_mode WAL");
    if (!step.ok) { result = step; goto export_cleanup; }
    step = export_exec_checked(dst_db, "PRAGMA synchronous=OFF",
                               "set synchronous OFF");
    if (!step.ok) { result = step; goto export_cleanup; }
    step = export_exec_checked(dst_db, "PRAGMA cache_size=-65536",
                               "set cache_size");
    if (!step.ok) { result = step; goto export_cleanup; }
    step = export_exec_checked(dst_db, "PRAGMA temp_store=FILE",
                               "set temp_store FILE");
    if (!step.ok) { result = step; goto export_cleanup; }

    char *attach_sql = sqlite3_mprintf("ATTACH DATABASE '%q' AS src",
                                       src_path);
    if (!attach_sql) {
        result = ZCL_ERR(-12, "export_snapshot: out of memory building ATTACH");
        goto export_cleanup;
    }
    step = export_exec_checked(dst_db, attach_sql, "attach source db");
    sqlite3_free(attach_sql);
    if (!step.ok) { result = step; goto export_cleanup; }
    src_attached = true;

    static const char *safe_tables[] = {
        "blocks", "transactions", "utxos", "addresses",
        "chain_stats", "zslp_tokens", "zslp_balances",
        NULL
    };

    step = export_exec_checked(dst_db, "BEGIN",
                               "begin snapshot transaction");
    if (!step.ok) { result = step; goto export_cleanup; }
    dst_txn_open = true;

    int tables_copied = 0;
    for (int i = 0; safe_tables[i]; i++) {
        char create_sql[512];
        snprintf(create_sql, sizeof(create_sql),
            "CREATE TABLE IF NOT EXISTS %s AS SELECT * FROM src.%s",
            safe_tables[i], safe_tables[i]);
        step = export_exec_checked(dst_db, create_sql,
                                   "copy consensus table");
        if (!step.ok) { result = step; goto export_cleanup; }
        tables_copied++;
    }

    step = export_exec_checked(dst_db,
        "CREATE TABLE IF NOT EXISTS _snapshot_meta "
        "(key TEXT PRIMARY KEY, value TEXT)",
        "create metadata table");
    if (!step.ok) { result = step; goto export_cleanup; }

    sqlite3_stmt *sel = NULL;
    step = export_prepare_checked(dst_db,
        "SELECT MAX(height) FROM blocks",
        &sel, "read snapshot height");
    if (!step.ok) { result = step; goto export_cleanup; }

    int snap_height = 0;
    step = export_step_checked(sel, dst_db, "read snapshot height");
    if (!step.ok) {
        result = step;
    } else if (sqlite3_column_type(sel, 0) != SQLITE_NULL) {
        snap_height = sqlite3_column_int(sel, 0);
    }
    sqlite3_finalize(sel);
    sel = NULL;
    if (!result.ok) goto export_cleanup;

    sqlite3_stmt *meta = NULL;
    step = export_prepare_checked(
        dst_db,
        "INSERT INTO _snapshot_meta(key,value) VALUES(?,?)",
        &meta, "prepare metadata insert");
    if (!step.ok) { result = step; goto export_cleanup; }

    char value_buf[32];
    snprintf(value_buf, sizeof(value_buf), "%d", snap_height);
    if (sqlite3_bind_text(meta, 1, "height", -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_text(meta, 2, value_buf, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        result = ZCL_ERR(-6, "export_snapshot: bind metadata height failed: %s",
                         sqlite3_errmsg(dst_db));
        sqlite3_finalize(meta);
        goto export_cleanup;
    }
    step = export_step_checked(meta, dst_db, "insert metadata height");
    if (!step.ok) {
        result = step;
        sqlite3_finalize(meta);
        goto export_cleanup;
    }

    sqlite3_reset(meta);
    sqlite3_clear_bindings(meta);

    snprintf(value_buf, sizeof(value_buf), "%d", tables_copied);
    if (sqlite3_bind_text(meta, 1, "tables", -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_text(meta, 2, value_buf, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        result = ZCL_ERR(-7, "export_snapshot: bind metadata tables failed: %s",
                         sqlite3_errmsg(dst_db));
        sqlite3_finalize(meta);
        goto export_cleanup;
    }
    step = export_step_checked(meta, dst_db, "insert metadata table count");
    if (!step.ok) {
        result = step;
        sqlite3_finalize(meta);
        goto export_cleanup;
    }
    sqlite3_finalize(meta);
    meta = NULL;

    step = export_exec_checked(dst_db, "COMMIT",
                               "commit snapshot transaction");
    if (!step.ok) { result = step; goto export_cleanup; }
    dst_txn_open = false;
    src_attached = false;
    step = export_exec_checked(dst_db, "DETACH DATABASE src",
                               "detach source db");
    if (!step.ok) { result = step; goto export_cleanup; }

    step = export_exec_checked(dst_db, "PRAGMA synchronous=NORMAL",
                               "restore sync NORMAL");
    if (!step.ok) { result = step; goto export_cleanup; }
    step = export_exec_checked(dst_db, "VACUUM", "vacuum snapshot");
    if (!step.ok) { result = step; goto export_cleanup; }

    struct stat dst_st;
    if (stat(dst_path, &dst_st) != 0) {
        result = ZCL_ERR(-8, "export_snapshot: destination stat failed: %s",
                         dst_path);
        goto export_cleanup;
    }

    printf("Consensus snapshot: %d tables, height %d, %.0f MB\n",
           tables_copied, snap_height,
           (double)dst_st.st_size / (1024.0 * 1024.0));
    if (tables_copied == 0) {
        result = ZCL_ERR(-9, "export_snapshot: no tables exported");
        goto export_cleanup;
    }

export_cleanup:
    if (dst_db_opened && dst_db) {
        if (dst_txn_open && !sqlite3_get_autocommit(dst_db)) {
            step = export_exec_checked(dst_db, "ROLLBACK",
                                       "rollback snapshot tx");
            if (!step.ok && result.ok)
                result = step;
        }
        if (src_attached) {
            step = export_exec_checked(dst_db, "DETACH DATABASE src",
                                       "detach source db");
            if (!step.ok && result.ok)
                result = step;
        }
        sqlite3_close(dst_db);
    }
    if (src_db_opened && src_db)
        sqlite3_close(src_db);

    if (!result.ok) {
        LOG_WARN("consensus_snapshot_export", "%s", result.message);
        unlink(dst_path);
    }
#ifdef __GLIBC__
    malloc_trim(0);
#endif

    return result;
}
