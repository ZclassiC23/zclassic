/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * consensus_db — one-time, integrity-verified migration of the reducer's
 * consensus kernel atomic set out of progress.kv into a dedicated
 * consensus.db. See storage/consensus_db.h for the contract and the
 * writer-contention rationale.
 *
 * This module sits BELOW the AR lifecycle for the same reason progress_store
 * and coins_kv do: it manipulates the kernel primitive store itself (schema
 * creation + a raw table-to-table copy), not a model. Its direct sqlite3_*
 * calls carry the kernel-store marker used by progress_store.c / coins_kv.c. */

#include "storage/consensus_db.h"

#include "storage/anchor_kv.h"
#include "storage/coins_kv.h"
#include "storage/nullifier_kv.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/stage.h"

#include <sqlite3.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The kernel atomic set, in the stable order the fingerprint indexes.  These
 * are exactly the tables committed together on the reducer batch-commit path
 * (see utxo_apply_stage.c: coins/anchors/nullifiers authored inside the same
 * BEGIN IMMEDIATE as the stage_cursor advance and the progress_meta writes). */
static const char *const KERNEL_TABLES[CONSENSUS_DB_KERNEL_TABLE_COUNT] = {
    "coins",           /* 0 — carries the SHA3 UTXO commitment */
    "sprout_anchors",  /* 1 */
    "sapling_anchors", /* 2 */
    "anchor_state",    /* 3 */
    "nullifiers",      /* 4 */
    "progress_meta",   /* 5 */
    "stage_cursor",    /* 6 */
};

/* Optional message helper — errbuf may be NULL. */
static void cdb_set_err(char *errbuf, size_t errcap, const char *fmt, ...)
{
    if (!errbuf || errcap == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, errcap, fmt, ap);
    va_end(ap);
}

static bool cdb_table_exists(sqlite3 *db, const char *name)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(
            db,
            "SELECT 1 FROM sqlite_schema WHERE type='table' AND name=?1",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    bool found = sqlite3_step(st) == SQLITE_ROW; // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(st);
    return found;
}

/* Durable row count of `table`. A missing table (or a prepare error) yields 0
 * so a source that never created an optional kernel table compares equal to a
 * freshly schema-ensured, empty destination table. */
static int64_t cdb_table_count(sqlite3 *db, const char *table)
{
    if (!cdb_table_exists(db, table))
        return 0;
    char sql[96];
    /* `table` is only ever from the fixed KERNEL_TABLES list — no injection. */
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return 0;
    int64_t n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) // raw-sql-ok:progress-kv-kernel-store
        n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

bool consensus_db_read_kernel_stats(sqlite3 *db,
                                    struct consensus_db_kernel_stats *out,
                                    char *errbuf, size_t errcap)
{
    if (!db || !out)
        LOG_FAIL("consensus_db", "read_kernel_stats: NULL db/out");

    memset(out, 0, sizeof(*out));

    if (!cdb_table_exists(db, "coins")) {
        cdb_set_err(errbuf, errcap,
                    "consensus_db: no coins table (uninitialised kernel)");
        return false;
    }

    /* Canonical SHA3-256 UTXO commitment over `coins`.  Reads the on-disk
     * table directly (this runs before the coins_ram overlay is bound); if the
     * overlay WERE active coins_kv_commitment would answer from RAM, so this is
     * documented as an overlay-absent primitive. */
    if (coins_kv_commitment(db, out->coins_commit) != 0) {
        cdb_set_err(errbuf, errcap,
                    "consensus_db: coins sha3 commitment read failed");
        return false;
    }

    for (size_t i = 0; i < CONSENSUS_DB_KERNEL_TABLE_COUNT; i++)
        out->table_rows[i] = cdb_table_count(db, KERNEL_TABLES[i]);

    return true;
}

static void cdb_hex32(const uint8_t v[32], char out[65])
{
    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = hex[(v[i] >> 4) & 0xf];
        out[i * 2 + 1] = hex[v[i] & 0xf];
    }
    out[64] = '\0';
}

bool consensus_db_kernel_stats_match(const struct consensus_db_kernel_stats *a,
                                     const struct consensus_db_kernel_stats *b,
                                     char *errbuf, size_t errcap)
{
    if (!a || !b)
        LOG_FAIL("consensus_db", "kernel_stats_match: NULL operand");

    if (memcmp(a->coins_commit, b->coins_commit, 32) != 0) {
        char ha[65], hb[65];
        cdb_hex32(a->coins_commit, ha);
        cdb_hex32(b->coins_commit, hb);
        cdb_set_err(errbuf, errcap,
                    "consensus_db: coins sha3 commitment mismatch src=%s dst=%s",
                    ha, hb);
        return false;
    }

    for (size_t i = 0; i < CONSENSUS_DB_KERNEL_TABLE_COUNT; i++) {
        if (a->table_rows[i] != b->table_rows[i]) {
            cdb_set_err(errbuf, errcap,
                        "consensus_db: %s row count mismatch src=%lld dst=%lld",
                        KERNEL_TABLES[i], (long long)a->table_rows[i],
                        (long long)b->table_rows[i]);
            return false;
        }
    }
    return true;
}

/* Create every kernel table on the destination via the SAME CREATE TABLE text
 * the live store uses, so an `INSERT INTO main.T SELECT * FROM src.T` copy
 * aligns column-for-column. */
static bool cdb_ensure_kernel_schema(sqlite3 *db)
{
    return coins_kv_ensure_schema(db) &&
           anchor_kv_ensure_schema(db) &&
           nullifier_kv_ensure_schema(db) &&
           progress_meta_table_ensure(db) &&
           stage_table_ensure(db);
}

/* Copy one kernel table src.<name> → main.<name> (both already schema-ensured
 * on the dest connection with src ATTACHed).  A source that lacks an optional
 * table is a clean no-op — the empty dest table then matches its 0-row source
 * count during verification. */
static bool cdb_copy_table(sqlite3 *db, const char *name, char *errbuf,
                           size_t errcap)
{
    if (!cdb_table_exists(db, name)) /* checks main.<name> — always present */
        LOG_FAIL("consensus_db", "copy_table: dest %s missing after ensure",
                 name);

    /* Existence of src.<name>: query the src schema explicitly. */
    sqlite3_stmt *chk = NULL;
    if (sqlite3_prepare_v2(
            db,
            "SELECT 1 FROM src.sqlite_schema WHERE type='table' AND name=?1",
            -1, &chk, NULL) != SQLITE_OK) {
        cdb_set_err(errbuf, errcap, "consensus_db: src schema probe failed: %s",
                    sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_text(chk, 1, name, -1, SQLITE_STATIC);
    bool src_has = sqlite3_step(chk) == SQLITE_ROW; // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(chk);
    if (!src_has)
        return true; /* nothing to copy for this table */

    char sql[128];
    snprintf(sql, sizeof(sql), "INSERT INTO main.%s SELECT * FROM src.%s",
             name, name);
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        cdb_set_err(errbuf, errcap, "consensus_db: copy %s failed: %s", name,
                    err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    if (err) sqlite3_free(err);
    return true;
}

static void cdb_unlink_family(const char *base)
{
    char p[1200];
    (void)unlink(base);
    snprintf(p, sizeof(p), "%s-wal", base);
    (void)unlink(p);
    snprintf(p, sizeof(p), "%s-shm", base);
    (void)unlink(p);
    snprintf(p, sizeof(p), "%s-journal", base);
    (void)unlink(p);
}

bool consensus_db_migrate_from_progress(const char *datadir, char *errbuf,
                                        size_t errcap)
{
    if (!datadir || !datadir[0])
        LOG_FAIL("consensus_db", "migrate: empty datadir");

    char cpath[1024];
    char ppath[1024];
    char tpath[1040];
    int n = snprintf(cpath, sizeof(cpath), "%s/%s", datadir,
                     CONSENSUS_DB_FILENAME);
    if (n <= 0 || (size_t)n >= sizeof(cpath))
        LOG_FAIL("consensus_db", "migrate: consensus.db path too long");
    n = snprintf(ppath, sizeof(ppath), "%s/progress.kv", datadir);
    if (n <= 0 || (size_t)n >= sizeof(ppath))
        LOG_FAIL("consensus_db", "migrate: progress.kv path too long");
    n = snprintf(tpath, sizeof(tpath), "%s.tmp", cpath);
    if (n <= 0 || (size_t)n >= sizeof(tpath))
        LOG_FAIL("consensus_db", "migrate: tmp path too long");

    /* Idempotent: already migrated. */
    if (access(cpath, F_OK) == 0)
        return true;

    /* Fresh node: no progress.kv to migrate — boot creates consensus.db. */
    if (access(ppath, F_OK) != 0)
        return true;

    /* 1) Fingerprint the SOURCE, then close it before the ATTACH copy so the
     *    WAL file has a single writer/owner during the copy. */
    struct consensus_db_kernel_stats src_stats;
    {
        sqlite3 *src = NULL;
        if (sqlite3_open_v2(ppath, &src,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                            NULL) != SQLITE_OK) {
            cdb_set_err(errbuf, errcap, "consensus_db: open source %s failed: %s",
                        ppath, src ? sqlite3_errmsg(src) : "(open)");
            if (src) sqlite3_close(src);
            return false;
        }
        if (!cdb_table_exists(src, "coins")) {
            /* progress.kv present but uninitialised — nothing to migrate. */
            sqlite3_close(src);
            return true;
        }
        bool ok = consensus_db_read_kernel_stats(src, &src_stats, errbuf, errcap);
        sqlite3_close(src);
        if (!ok)
            return false;
    }

    /* 2) Build consensus.db.tmp fresh and copy the atomic set into it.  Use a
     *    rollback journal (default) so the finished file is self-contained —
     *    no -wal to carry across the rename; progress_store switches the real
     *    consensus.db to WAL on its first open. */
    cdb_unlink_family(tpath);
    sqlite3 *dst = NULL;
    if (sqlite3_open_v2(tpath, &dst,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                            SQLITE_OPEN_FULLMUTEX,
                        NULL) != SQLITE_OK) {
        cdb_set_err(errbuf, errcap, "consensus_db: create %s failed: %s", tpath,
                    dst ? sqlite3_errmsg(dst) : "(open)");
        if (dst) sqlite3_close(dst);
        cdb_unlink_family(tpath);
        return false;
    }

    bool ok = cdb_ensure_kernel_schema(dst);
    if (!ok)
        cdb_set_err(errbuf, errcap, "consensus_db: dest schema ensure failed");

    if (ok) {
        char *err = NULL;
        char attach[1200];
        snprintf(attach, sizeof(attach), "ATTACH DATABASE '%s' AS src", ppath);
        if (sqlite3_exec(dst, attach, NULL, NULL, &err) != SQLITE_OK) {
            cdb_set_err(errbuf, errcap, "consensus_db: attach source failed: %s",
                        err ? err : "(no message)");
            if (err) sqlite3_free(err);
            ok = false;
        }
        if (err) { sqlite3_free(err); err = NULL; }

        if (ok && sqlite3_exec(dst, "BEGIN IMMEDIATE", NULL, NULL, &err) !=
                      SQLITE_OK) {
            cdb_set_err(errbuf, errcap, "consensus_db: dest BEGIN failed: %s",
                        err ? err : "(no message)");
            if (err) sqlite3_free(err);
            ok = false;
        }
        if (err) { sqlite3_free(err); err = NULL; }

        for (size_t i = 0; ok && i < CONSENSUS_DB_KERNEL_TABLE_COUNT; i++)
            ok = cdb_copy_table(dst, KERNEL_TABLES[i], errbuf, errcap);

        const char *fini = ok ? "COMMIT" : "ROLLBACK";
        if (sqlite3_exec(dst, fini, NULL, NULL, &err) != SQLITE_OK) {
            if (ok)
                cdb_set_err(errbuf, errcap, "consensus_db: dest COMMIT failed: %s",
                            err ? err : "(no message)");
            ok = false;
        }
        if (err) { sqlite3_free(err); err = NULL; }

        (void)sqlite3_exec(dst, "DETACH DATABASE src", NULL, NULL, NULL);
    }

    /* 3) Verify the copy fingerprint EQUALS the source fingerprint. */
    if (ok) {
        struct consensus_db_kernel_stats dst_stats;
        ok = consensus_db_read_kernel_stats(dst, &dst_stats, errbuf, errcap) &&
             consensus_db_kernel_stats_match(&src_stats, &dst_stats, errbuf,
                                             errcap);
    }

    sqlite3_close(dst);

    if (!ok) {
        /* Refuse: never leave a half-migrated consensus.db. progress.kv is
         * untouched, so the caller can retry or fall back. */
        cdb_unlink_family(tpath);
        return false;
    }

    /* 4) Atomically publish. */
    if (rename(tpath, cpath) != 0) {
        cdb_set_err(errbuf, errcap, "consensus_db: rename %s -> %s failed: %s",
                    tpath, cpath, strerror(errno));
        cdb_unlink_family(tpath);
        return false;
    }
    return true;
}
