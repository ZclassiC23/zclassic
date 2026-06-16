/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * wallet_backup_store_sqlite — sqlite implementation of
 * wallet_backup_store_port.
 *
 * The four methods below are the raw sqlite ops behind the port: EXACT same
 * SQL text, per-table existence probe, open flags, and AR step macros, so
 * the produced backup file and its verification are bit-for-bit identical.
 *
 * Writes: the CREATE TABLE AS SELECT copies go through sqlite3_exec — the
 * AR-compatible exec path (the AR lint gate forbids raw sqlite3_step in app
 * code; sqlite3_exec is the blessed statement-less write path, and the
 * ATTACH step uses the AR_STEP macro). No raw sqlite3_step appears here.
 */

#include "adapters/outbound/persistence/wallet_backup_store_sqlite.h"

#include "util/ar_step_readonly.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static inline struct wallet_backup_store_sqlite_ctx *ctx_of(void *self)
{
    return (struct wallet_backup_store_sqlite_ctx *)self;
}

/* sqlite3_db_filename(src,"main") — absolute on-disk path of the source.
 * Empty for an in-memory DB. */
static bool wbs_store_source_path(void *self, char *out, size_t cap)
{
    struct wallet_backup_store_sqlite_ctx *c = ctx_of(self);
    if (!c || !c->src_db || !out || cap == 0)
        return false;
    const char *p = sqlite3_db_filename(c->src_db, "main");
    if (!p || !*p)
        return false;
    snprintf(out, cap, "%s", p);
    return true;
}

/* "SELECT count(*) FROM <table>" over the bound source connection. */
static bool wbs_store_count_rows(void *self, const char *table, int64_t *out)
{
    struct wallet_backup_store_sqlite_ctx *c = ctx_of(self);
    if (!c || !c->src_db || !table || !out)
        return false;
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM %s", table);
    sqlite3_stmt *st = NULL;
    bool ok = false;
    if (sqlite3_prepare_v2(c->src_db, sql, -1, &st, NULL) == SQLITE_OK && st) {
        if (AR_STEP_ROW_READONLY(st) == SQLITE_ROW) {
            *out = sqlite3_column_int64(st, 0);
            ok = true;
        }
        sqlite3_finalize(st);
    }
    return ok;
}

/* Open dst, ATTACH source by path, CREATE TABLE AS SELECT per existing
 * table, DETACH, close. */
static enum wallet_backup_store_status wbs_store_write_snapshot(
    void *self,
    const char *dst_path,
    const char *src_path,
    const char *const *tables,
    size_t n_tables,
    char *out_copy_err,
    size_t copy_err_cap)
{
    struct wallet_backup_store_sqlite_ctx *c = ctx_of(self);
    if (out_copy_err && copy_err_cap)
        out_copy_err[0] = '\0';
    if (!c || !dst_path || !src_path || (!tables && n_tables > 0))
        return WB_STORE_OPEN_DST_FAILED;

    /* Open the destination as a fresh empty db. */
    sqlite3 *dst = NULL;
    int rc = sqlite3_open_v2(dst_path, &dst,
        SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE, NULL);
    if (rc != SQLITE_OK) {
        if (dst) sqlite3_close(dst);
        unlink(dst_path);
        return WB_STORE_OPEN_DST_FAILED;
    }

    /* ATTACH the source by absolute path under alias "src". */
    {
        sqlite3_stmt *att = NULL;
        rc = sqlite3_prepare_v2(dst,
            "ATTACH DATABASE ? AS src", -1, &att, NULL);
        if (rc != SQLITE_OK || !att) {
            if (att) sqlite3_finalize(att);
            sqlite3_close(dst);
            unlink(dst_path);
            return WB_STORE_ATTACH_FAILED;
        }
        sqlite3_bind_text(att, 1, src_path, -1, SQLITE_STATIC);
        if (AR_STEP_ROW_READONLY(att) != SQLITE_DONE) {
            sqlite3_finalize(att);
            sqlite3_close(dst);
            unlink(dst_path);
            return WB_STORE_ATTACH_FAILED;
        }
        sqlite3_finalize(att);
    }

    /* For each wallet table, run CREATE TABLE IF NOT EXISTS t AS
     * SELECT ... The AS SELECT form copies both schema and rows
     * in one statement; if the source table is missing we just
     * skip it (older databases may not have every table). */
    char *errmsg = NULL;
    bool all_ok = true;
    for (size_t i = 0; i < n_tables; i++) {
        const char *table = tables[i];
        /* Check the source even has this table. */
        char exists_sql[256];
        snprintf(exists_sql, sizeof(exists_sql),
            "SELECT name FROM src.sqlite_master "
            "WHERE type='table' AND name='%s'", table);
        sqlite3_stmt *chk = NULL;
        bool src_has = false;
        if (sqlite3_prepare_v2(dst, exists_sql, -1, &chk, NULL) == SQLITE_OK && chk) {
            src_has = AR_STEP_ROW_READONLY(chk) == SQLITE_ROW;
            sqlite3_finalize(chk);
        }
        if (!src_has) continue;

        char sql[256];
        snprintf(sql, sizeof(sql),
            "CREATE TABLE %s AS SELECT * FROM src.%s", table, table);
        rc = sqlite3_exec(dst, sql, NULL, NULL, &errmsg);
        if (rc != SQLITE_OK) {
            if (out_copy_err && copy_err_cap)
                snprintf(out_copy_err, copy_err_cap,
                        "copy %s: %s", table, errmsg ? errmsg : "?");
            sqlite3_free(errmsg);
            errmsg = NULL;
            all_ok = false;
            break;
        }
    }

    /* Detach + close. */
    (void)sqlite3_exec(dst, "DETACH DATABASE src", NULL, NULL, NULL);
    sqlite3_close(dst);

    return all_ok ? WB_STORE_OK : WB_STORE_COPY_FAILED;
}

/* Reopen a backup file READ-ONLY and count rows in a table; -1 on miss. */
static int64_t wbs_store_count_rows_in_file(void *self,
                                            const char *file_path,
                                            const char *table)
{
    (void)self;
    if (!file_path || !table)
        return -1;
    int64_t n = -1;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(file_path, &db, SQLITE_OPEN_READONLY, NULL)
            == SQLITE_OK) {
        char sql[128];
        snprintf(sql, sizeof(sql), "SELECT count(*) FROM %s", table);
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && st) {
            if (AR_STEP_ROW_READONLY(st) == SQLITE_ROW)
                n = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
        sqlite3_close(db);
    } else if (db) {
        sqlite3_close(db);
    }
    return n;
}

bool wallet_backup_store_sqlite_bind(struct wallet_backup_store_sqlite_ctx *ctx,
                                     sqlite3 *src_db,
                                     struct wallet_backup_store_port *out_port)
{
    if (!ctx || !out_port)
        return false;
    ctx->src_db = src_db;
    *out_port = (struct wallet_backup_store_port){
        .self               = ctx,
        .source_path        = wbs_store_source_path,
        .count_rows         = wbs_store_count_rows,
        .write_snapshot     = wbs_store_write_snapshot,
        .count_rows_in_file = wbs_store_count_rows_in_file,
    };
    return true;
}
