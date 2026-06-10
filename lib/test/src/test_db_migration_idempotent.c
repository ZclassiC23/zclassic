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

int test_db_migration_idempotent(void);

int test_db_migration_idempotent(void)
{
    printf("\n=== db_migration_idempotent tests ===\n");
    int failures = 0;
    mkdir_p("./test-tmp");
    failures += t_fresh_reaches_latest();
    failures += t_reopen_is_idempotent();
    failures += t_memory_open();
    failures += t_turbo_mode_roundtrip();
    return failures;
}
