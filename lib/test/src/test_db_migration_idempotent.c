/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Regression tests for database.c migration machinery.
 *
 * These tests guard the class of silent-failure bugs that the v2→v18
 * migration block previously hid: if a CREATE TABLE or ALTER TABLE
 * failed inside `node_db_migrate`, the schema_version counter still
 * advanced (or, worse, failed to persist and quietly re-applied the
 * same migration on every boot).
 */

#include "test/test_helpers.h"
#include "models/database.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int mkdir_p(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

/* cwd-relative tmpdir to comply with the "no /tmp" project convention. */
static void db_mig_path(char *buf, size_t n, const char *tag)
{
    snprintf(buf, n, "./test-tmp/db_mig_%d_%s", (int)getpid(), tag);
}

static bool db_mig_stamp_schema(sqlite3 *db, int32_t version)
{
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "UPDATE node_state SET value=? WHERE key='schema_version'",
        -1, &st, NULL);
    if (rc != SQLITE_OK)
        return false;
    rc = sqlite3_bind_blob(st, 1, &version, sizeof(version),
                           SQLITE_TRANSIENT);
    bool ok = rc == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE &&
              sqlite3_changes(db) == 1;
    sqlite3_finalize(st);
    return ok;
}

static int db_mig_count(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK)
        return -1;
    rc = sqlite3_step(st);
    int out = rc == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    return out;
}

static bool db_mig_exec_raw(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK)
        fprintf(stderr, "db_mig raw exec failed: %s sql=%s\n",
                err ? err : "(no errmsg)", sql ? sql : "(null)");
    sqlite3_free(err);
    return rc == SQLITE_OK;
}

static bool db_mig_column_exists(sqlite3 *db, const char *table,
                                 const char *column)
{
    char sql[160];
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    bool found = false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(st, 1);
        if (name && strcmp((const char *)name, column) == 0) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(st);
    return found;
}

static bool db_mig_seed_v20_wallet_notes_db(const char *dbpath)
{
    sqlite3 *raw = NULL;
    if (sqlite3_open(dbpath, &raw) != SQLITE_OK)
        return false;

    bool ok = true;
    ok = ok && db_mig_exec_raw(raw,
        "CREATE TABLE node_state (key TEXT PRIMARY KEY,value BLOB)");
    ok = ok && db_mig_exec_raw(raw,
        "INSERT INTO node_state(key,value) "
        "VALUES('schema_version',X'14000000')");
    ok = ok && db_mig_exec_raw(raw,
        "CREATE TABLE wallet_sapling_notes ("
        "txid BLOB NOT NULL,output_index INTEGER NOT NULL,"
        "value INTEGER NOT NULL,rcm BLOB NOT NULL,memo BLOB,"
        "ivk BLOB NOT NULL,diversifier BLOB NOT NULL,"
        "pk_d BLOB NOT NULL,cm BLOB NOT NULL,"
        "nullifier BLOB NOT NULL UNIQUE,"
        "block_height INTEGER,spent_txid BLOB,address TEXT,"
        "witness_data BLOB,witness_height INTEGER DEFAULT 0,"
        "PRIMARY KEY (txid,output_index))");
    ok = ok && db_mig_exec_raw(raw,
        "INSERT INTO wallet_sapling_notes"
        "(txid,output_index,value,rcm,ivk,diversifier,pk_d,cm,"
        "nullifier,block_height,address,witness_height) "
        "VALUES(X'01',0,42,X'02',X'03',X'04',X'05',X'06',"
        "X'07',100,'zs-v20-note',100)");
    sqlite3_close(raw);
    return ok;
}

static int t_fresh_reaches_latest(void)
{
    int failures = 0;
    char dir[256];
    db_mig_path(dir, sizeof(dir), "fresh");
    mkdir_p(dir);
    char dbpath[512];
    snprintf(dbpath, sizeof(dbpath), "%s/node.db", dir);

    struct node_db ndb;
    TEST("db_mig: fresh open reaches latest built-in schema version") {
        ASSERT(node_db_open(&ndb, dbpath));
        int v = node_db_schema_version(&ndb);
        ASSERT(v >= 18);
        node_db_close(&ndb);
        PASS();
    } _test_next:;
    test_cleanup_tmpdir(dir);
    return failures;
}

static int t_v20_wallet_notes_upgrade_adds_source(void)
{
    int failures = 0;
    char dir[256];
    db_mig_path(dir, sizeof(dir), "v20_wallet_notes");
    mkdir_p(dir);
    char dbpath[512];
    snprintf(dbpath, sizeof(dbpath), "%s/node.db", dir);

    TEST("db_mig: v20 wallet notes upgrade adds source after schema create") {
        ASSERT(db_mig_seed_v20_wallet_notes_db(dbpath));

        struct node_db ndb;
        ASSERT(node_db_open(&ndb, dbpath));
        ASSERT_EQ(node_db_schema_version(&ndb), NODE_DB_SCHEMA_LATEST);
        node_db_close(&ndb);

        sqlite3 *raw = NULL;
        ASSERT(sqlite3_open(dbpath, &raw) == SQLITE_OK);
        ASSERT(db_mig_column_exists(raw, "wallet_sapling_notes", "source"));
        ASSERT(db_mig_count(raw,
            "SELECT count(*) FROM sqlite_master "
            "WHERE type='index' AND name='idx_snote_view_address'") == 1);
        ASSERT(db_mig_count(raw,
            "SELECT count(*) FROM schema_migrations "
            "WHERE version='021'") == 1);
        ASSERT(db_mig_count(raw,
            "SELECT count(*) FROM schema_migrations "
            "WHERE version='022'") == 1);
        ASSERT(db_mig_count(raw,
            "SELECT count(*) FROM sqlite_master "
            "WHERE type='index' AND name='idx_txo_hodl_scan'") == 1);
        ASSERT(db_mig_count(raw,
            "SELECT count(*) FROM sqlite_master "
            "WHERE type='index' AND name='idx_txi_prev_height'") == 1);
        ASSERT(db_mig_count(raw,
            "SELECT count(*) FROM wallet_sapling_notes "
            "WHERE address='zs-v20-note' AND source='local'") == 1);
        sqlite3_close(raw);
        PASS();
    } _test_next:;
    test_cleanup_tmpdir(dir);
    return failures;
}

static int t_reopen_is_idempotent(void)
{
    int failures = 0;
    char dir[256];
    db_mig_path(dir, sizeof(dir), "reopen");
    mkdir_p(dir);
    char dbpath[512];
    snprintf(dbpath, sizeof(dbpath), "%s/node.db", dir);

    TEST("db_mig: reopen does not re-apply migrations") {
        struct node_db ndb1;
        ASSERT(node_db_open(&ndb1, dbpath));
        int v1 = node_db_schema_version(&ndb1);
        ASSERT(v1 >= 18);
        node_db_close(&ndb1);

        struct node_db ndb2;
        ASSERT(node_db_open(&ndb2, dbpath));
        int v2 = node_db_schema_version(&ndb2);
        ASSERT_EQ(v1, v2);
        node_db_close(&ndb2);
        PASS();
    } _test_next:;
    test_cleanup_tmpdir(dir);
    return failures;
}

static int t_memory_open(void)
{
    int failures = 0;
    TEST("db_mig: :memory: open succeeds with schema migrations") {
        struct node_db mem;
        ASSERT(node_db_open(&mem, ":memory:"));
        int v = node_db_schema_version(&mem);
        ASSERT(v >= 18);
        node_db_close(&mem);
        PASS();
    } _test_next:;
    return failures;
}

static int t_turbo_mode_roundtrip(void)
{
    int failures = 0;
    char dir[256];
    db_mig_path(dir, sizeof(dir), "turbo");
    mkdir_p(dir);
    char dbpath[512];
    snprintf(dbpath, sizeof(dbpath), "%s/node.db", dir);

    TEST("db_mig: turbo->normal mode roundtrip leaves state consistent") {
        struct node_db ndb;
        ASSERT(node_db_open(&ndb, dbpath));
        ASSERT(node_db_ibd_turbo_mode(&ndb));

        struct node_db_status st;
        node_db_get_status(&ndb, &st);
        ASSERT(st.turbo_mode);

        ASSERT(node_db_normal_mode(&ndb));
        node_db_get_status(&ndb, &st);
        ASSERT(!st.turbo_mode);

        node_db_close(&ndb);
        PASS();
    } _test_next:;
    test_cleanup_tmpdir(dir);
    return failures;
}

static int t_newer_schema_refuses_open_before_staging_cleanup(void)
{
    int failures = 0;
    char dir[256];
    db_mig_path(dir, sizeof(dir), "newer");
    mkdir_p(dir);
    char dbpath[512];
    snprintf(dbpath, sizeof(dbpath), "%s/node.db", dir);

    TEST("db_mig: newer schema fails closed before staging cleanup") {
        struct node_db seed;
        ASSERT(node_db_open(&seed, dbpath));
        ASSERT(node_db_exec(&seed,
            "INSERT INTO snapshot_staging_utxos"
            "(txid,vout,value,script,script_type,height,is_coinbase)"
            " VALUES(X'4200000000000000000000000000000000000000000000000000000000000000',0,1,X'51',0,1,0)"));
        ASSERT(node_db_state_set(&seed, "snapshot_staging_phase",
                                 "chunk_receive", strlen("chunk_receive")));
        node_db_close(&seed);

        sqlite3 *raw = NULL;
        ASSERT(sqlite3_open(dbpath, &raw) == SQLITE_OK);
        ASSERT(db_mig_stamp_schema(raw, NODE_DB_MAX_SCHEMA + 1));
        sqlite3_close(raw);
        raw = NULL;

        struct node_db newer;
        bool opened = node_db_open(&newer, dbpath);
        ASSERT(!opened);
        ASSERT(!newer.open);
        ASSERT(newer.db == NULL);

        ASSERT(sqlite3_open(dbpath, &raw) == SQLITE_OK);
        ASSERT(db_mig_count(raw,
            "SELECT count(*) FROM snapshot_staging_utxos") == 1);
        ASSERT(db_mig_count(raw,
            "SELECT count(*) FROM node_state "
            "WHERE key='snapshot_staging_phase'") == 1);
        ASSERT(db_mig_count(raw,
            "SELECT count(*) FROM node_state "
            "WHERE key='schema_version'") == 1);
        sqlite3_close(raw);
        PASS();
    } _test_next:;
    test_cleanup_tmpdir(dir);
    return failures;
}

int test_db_migration_idempotent(void);

int test_db_migration_idempotent(void)
{
    printf("\n=== db_migration_idempotent tests ===\n");
    int failures = 0;
    mkdir_p("./test-tmp");
    failures += t_fresh_reaches_latest();
    failures += t_v20_wallet_notes_upgrade_adds_source();
    failures += t_reopen_is_idempotent();
    failures += t_memory_open();
    failures += t_turbo_mode_roundtrip();
    failures += t_newer_schema_refuses_open_before_staging_cleanup();
    return failures;
}
