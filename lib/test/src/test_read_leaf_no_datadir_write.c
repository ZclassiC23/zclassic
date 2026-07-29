/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * A leaf declared READ must not WRITE the datadir it is pointed at.
 *
 * THE BUG THIS EXISTS TO CATCH (reproduced live, 2026-07-29):
 *
 *   $ zclassic23 app service access --input='{"service":"reference"}'
 *   [boot] sqlite.quick_check ...
 *   db: applied 35 migration(s), now at version 36
 *
 * `app.service.access` is declared ZCL_COMMAND_READY_READ /
 * ZCL_COMMAND_AUTH_PUBLIC / ZCL_COMMAND_TRAIT_IDEMPOTENT, and its handler
 * called node_db_open() — the BOOT ceremony. That opens
 * <datadir>/node.db with SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE and then,
 * in order: runs PRAGMA quick_check and on failure rename()s node.db,
 * node.db-wal and node.db-shm aside to node.db.corrupt-<ts>; runs
 * create_schema(); runs node_db_migrate(); and finally executes
 *   DELETE FROM snapshot_staging_utxos
 *   DELETE FROM node_state WHERE key LIKE 'snapshot_staging_%'
 * (app/models/src/database.c, node_db_open_impl(boot_ceremony=true)).
 *
 * Because `datadir` falls back to zcl_native_command_datadir() when the
 * caller passes none, a bare invocation of a *read* leaf did all of that to
 * the operator's LIVE node database. node.db is WAL, so the running node
 * holding it open blocks none of it. Five more leaves had the same defect:
 * zcode.release.prove, zcode.domain.list, zcode.domain.status,
 * zcode.contributor.show, zcode.package.resolve.
 *
 * WHAT IS ASSERTED HERE — the property, not the implementation. Each leaf
 * is invoked against a FIXTURE datadir (never a live one; always an
 * explicit `datadir` input) in three states, and the only thing checked is
 * what the leaf left behind on disk:
 *
 *   absent    — an empty datadir. Afterwards NO file whose name begins
 *               "node.db" may exist. (Catches OPEN_CREATE.)
 *   present   — a migrated node.db carrying seeded snapshot_staging rows.
 *               Afterwards the file's byte length and FNV-1a content hash
 *               must be identical, both staging row counts must be
 *               identical, and no node.db.corrupt-* may exist. (Catches
 *               create_schema, node_db_migrate, and the two DELETEs.)
 *   garbage   — a node.db that is not a SQLite file at all. Afterwards it
 *               must still be there, byte-identical, under its own name.
 *               (Catches db_quarantine_files' rename.)
 *
 * The reply is deliberately NOT asserted on: a read leaf is free to answer
 * "blocked, no node.db here" or to answer with data. The contract under
 * test is that pointing a read leaf at a directory never changes it.
 *
 * Sibling: test_offline_datadir_query.c covers the two SCOPE_OFFLINE_COPY
 * leaves' answers; this file covers every read leaf's SIDE EFFECTS. */

#define _POSIX_C_SOURCE 200809L

#include "test/test_core.h"

#include "command/native_command.h"
#include "json/json.h"
#include "models/database.h"

#include <dirent.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RLW_CHECK(name, expr) do {                                     \
    printf("read_leaf_no_datadir_write: %s... ", (name));              \
    if (expr) { printf("OK\n"); }                                      \
    else { printf("FAIL\n"); failures++; }                             \
} while (0)

/* ── the leaves under test ─────────────────────────────────────────────
 *
 * Every one of these is declared with a READ macro in a config/commands
 * .def file and takes a caller-supplied `datadir`. k1/k2 are the other
 * inputs, without which the handler returns before it ever reaches the
 * datadir and the case would prove nothing. */

typedef void (*rlw_handler_fn)(const struct zcl_command_request *,
                               struct zcl_command_reply *);

struct rlw_leaf {
    const char *path;
    rlw_handler_fn fn;
    const char *k1, *v1;
    const char *k2, *v2;
};

/* A syntactically valid compressed secp256k1 key; it needs to parse, not to
 * exist. */
#define RLW_PUBKEY \
    "02b4632d08485ff1df2db55b9dafd23347d1c47a457072a1e87be26896549a8737"

static const struct rlw_leaf g_rlw_leaves[] = {
    { "app.service.access",     zcl_native_handle_service_access,
      "service", "reference",   NULL, NULL },
    { "zcode.release.prove",    zcl_native_handle_zcode_release_prove,
      "name", "demo",           "version", "0.1.0" },
    { "zcode.domain.list",      zcl_native_handle_zcode_domain_list,
      NULL, NULL,               NULL, NULL },
    { "zcode.domain.status",    zcl_native_handle_zcode_domain_status,
      "domain", "zcode",        NULL, NULL },
    { "zcode.contributor.show", zcl_native_handle_zcode_contributor_show,
      "pubkey", RLW_PUBKEY,     NULL, NULL },
    { "zcode.package.resolve",  zcl_native_handle_zcode_package_resolve,
      "name", "ringbuffer",     NULL, NULL },
};

#define RLW_LEAF_COUNT ((int)(sizeof(g_rlw_leaves) / sizeof(g_rlw_leaves[0])))

/* ── on-disk observation helpers ───────────────────────────────────── */

/* FNV-1a over the whole file. 0 means "could not read" (callers treat an
 * unreadable snapshot as a failed observation, never as a match). */
static uint64_t rlw_file_hash(const char *path, int64_t *size_out)
{
    if (size_out)
        *size_out = -1;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    uint64_t h = 1469598103934665603ULL;
    int64_t total = 0;
    unsigned char buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        total += (int64_t)got;
        for (size_t i = 0; i < got; i++) {
            h ^= buf[i];
            h *= 1099511628211ULL;
        }
    }
    fclose(f);
    if (size_out)
        *size_out = total;
    /* Never hand back the "unreadable" sentinel for a real read. */
    return h ? h : 1;
}

/* Count directory entries whose name starts with `prefix`. -1 on a
 * directory that cannot be opened. */
static int rlw_count_entries(const char *dir, const char *prefix)
{
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    int n = 0;
    size_t plen = strlen(prefix);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (plen == 0 || strncmp(e->d_name, prefix, plen) == 0)
            n++;
    }
    closedir(d);
    return n;
}

/* Print every entry under `dir` — the evidence line for a failed case. */
static void rlw_list_dir(const char *tag, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        printf("    [%s] %s: unreadable\n", tag, dir);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char p[1200];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0)
            printf("    [%s] %s (%lld bytes)\n", tag, e->d_name,
                   (long long)st.st_size);
        else
            printf("    [%s] %s\n", tag, e->d_name);
    }
    closedir(d);
}

/* SELECT one integer, read-only. -1 on any failure (an unreadable database
 * is never silently reported as a count of zero). */
static int64_t rlw_scalar(const char *db_path, const char *sql)
{
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt *st = NULL;
    int64_t v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return v;
}

/* ── fixture builders ──────────────────────────────────────────────── */

static void rlw_mkfixture(char *dir, size_t n, const char *tag)
{
    test_fmt_tmpdir(dir, n, "read_leaf_no_write", tag);
    mkdir("./test-tmp", 0700);
    test_rm_rf(dir);
    mkdir(dir, 0700);
    /* zcode.contributor.show and zcode.package.resolve rebuild a package
     * index over <datadir>/zcode before they touch node.db; without the
     * directory they return early and the case proves nothing. */
    char zdir[1200];
    snprintf(zdir, sizeof(zdir), "%s/zcode", dir);
    mkdir(zdir, 0700);
}

/* A migrated node.db carrying rows in exactly the two places the boot
 * ceremony deletes from. Returns false if the fixture did not come out the
 * way the assertions below assume. */
static bool rlw_seed_node_db(const char *dir, const char *db_path)
{
    (void)dir;
    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    if (!node_db_open(&ndb, db_path) || !ndb.open) {
        fprintf(stderr, "[read_leaf_no_datadir_write] fixture: node_db_open "
                        "failed for %s\n", db_path);
        return false;
    }
    static const char *const seed =
        "INSERT OR REPLACE INTO snapshot_staging_utxos"
        "(txid,vout,value,script,script_type,address_hash,height,is_coinbase)"
        " VALUES(x'11', 0, 5000, x'76a914', 0, NULL, 700000, 0);"
        "INSERT OR REPLACE INTO snapshot_staging_utxos"
        "(txid,vout,value,script,script_type,address_hash,height,is_coinbase)"
        " VALUES(x'22', 1, 6000, x'76a914', 0, NULL, 700001, 0);"
        "INSERT OR REPLACE INTO node_state(key,value)"
        " VALUES('snapshot_staging_height','700001');"
        "INSERT OR REPLACE INTO node_state(key,value)"
        " VALUES('snapshot_staging_root','deadbeef');";
    char *err = NULL;
    bool ok = sqlite3_exec(ndb.db, seed, NULL, NULL, &err) == SQLITE_OK;
    if (!ok) {
        fprintf(stderr, "[read_leaf_no_datadir_write] fixture seed: %s\n",
                err ? err : "(null)");
        sqlite3_free(err);
    }
    node_db_close(&ndb);
    return ok;
}

/* ── leaf invocation ───────────────────────────────────────────────── */

static void rlw_invoke(const struct rlw_leaf *lf, const char *datadir)
{
    struct json_value input;
    json_init(&input);
    json_set_object(&input);
    (void)json_push_kv_str(&input, "datadir", datadir);
    if (lf->k1)
        (void)json_push_kv_str(&input, lf->k1, lf->v1);
    if (lf->k2)
        (void)json_push_kv_str(&input, lf->k2, lf->v2);

    struct zcl_command_request request = { .input = &input };
    struct zcl_command_reply reply;
    zcl_command_reply_init(&reply, "zcl.read_leaf_probe.v1");
    lf->fn(&request, &reply);
    printf("    [%s] status=%d exit=%d code=%s\n", lf->path, (int)reply.status,
           (int)reply.exit_code, reply.error.code[0] ? reply.error.code : "-");
    zcl_command_reply_free(&reply);
    json_free(&input);
}

/* ── case 1: an empty datadir must stay empty of node.db ───────────── */

static int t_absent_node_db_is_not_created(void)
{
    int failures = 0;

    for (int i = 0; i < RLW_LEAF_COUNT; i++) {
        const struct rlw_leaf *lf = &g_rlw_leaves[i];
        char dir[256];
        char tag[64];
        snprintf(tag, sizeof(tag), "absent%d", i);
        rlw_mkfixture(dir, sizeof(dir), tag);

        char db_path[1200];
        snprintf(db_path, sizeof(db_path), "%s/node.db", dir);
        char name[160];

        snprintf(name, sizeof(name),
                 "%s: fixture starts with no node.db", lf->path);
        RLW_CHECK(name, access(db_path, F_OK) != 0);

        rlw_invoke(lf, dir);

        int left = rlw_count_entries(dir, "node.db");
        if (left != 0)
            rlw_list_dir(lf->path, dir);
        snprintf(name, sizeof(name),
                 "%s: read leaf created no node.db* in an empty datadir",
                 lf->path);
        RLW_CHECK(name, left == 0);

        test_rm_rf(dir);
    }
    return failures;
}

/* ── case 2: a real node.db must come out byte-identical ───────────── */

static int t_present_node_db_is_not_mutated(void)
{
    int failures = 0;
    char dir[256];
    rlw_mkfixture(dir, sizeof(dir), "present");
    char db_path[1200];
    snprintf(db_path, sizeof(db_path), "%s/node.db", dir);

    RLW_CHECK("present: fixture node.db seeded",
              rlw_seed_node_db(dir, db_path));

    static const char *const q_utxos =
        "SELECT COUNT(*) FROM snapshot_staging_utxos";
    static const char *const q_state =
        "SELECT COUNT(*) FROM node_state WHERE key LIKE 'snapshot_staging_%'";

    int64_t utxos_before = rlw_scalar(db_path, q_utxos);
    int64_t state_before = rlw_scalar(db_path, q_state);
    /* Anti-vacuous: with zero seeded rows the DELETEs would be invisible and
     * this whole case would pass on a database the boot ceremony wiped. */
    RLW_CHECK("present: 2 snapshot_staging_utxos rows to lose",
              utxos_before == 2);
    RLW_CHECK("present: 2 snapshot_staging_% node_state rows to lose",
              state_before == 2);

    int64_t size_before = -1;
    uint64_t hash_before = rlw_file_hash(db_path, &size_before);
    RLW_CHECK("present: node.db readable before the calls",
              hash_before != 0 && size_before > 0);

    for (int i = 0; i < RLW_LEAF_COUNT; i++)
        rlw_invoke(&g_rlw_leaves[i], dir);

    int64_t size_after = -1;
    uint64_t hash_after = rlw_file_hash(db_path, &size_after);
    int64_t utxos_after = rlw_scalar(db_path, q_utxos);
    int64_t state_after = rlw_scalar(db_path, q_state);
    int quarantined = rlw_count_entries(dir, "node.db.corrupt");

    if (hash_after != hash_before || utxos_after != utxos_before ||
        state_after != state_before || quarantined != 0) {
        printf("    size %lld -> %lld, hash %016llx -> %016llx\n",
               (long long)size_before, (long long)size_after,
               (unsigned long long)hash_before,
               (unsigned long long)hash_after);
        printf("    snapshot_staging_utxos %lld -> %lld, "
               "node_state snapshot_staging_%% %lld -> %lld\n",
               (long long)utxos_before, (long long)utxos_after,
               (long long)state_before, (long long)state_after);
        rlw_list_dir("present", dir);
    }

    RLW_CHECK("present: node.db byte length unchanged",
              size_after == size_before && size_after > 0);
    RLW_CHECK("present: node.db content hash unchanged",
              hash_after == hash_before && hash_after != 0);
    RLW_CHECK("present: snapshot_staging_utxos rows survived",
              utxos_after == utxos_before);
    RLW_CHECK("present: node_state snapshot_staging_% rows survived",
              state_after == state_before);
    RLW_CHECK("present: nothing was quarantined to node.db.corrupt-*",
              quarantined == 0);

    test_rm_rf(dir);
    return failures;
}

/* ── case 3: a node.db that is not a database must not be renamed ──── */

static int t_garbage_node_db_is_not_quarantined(void)
{
    int failures = 0;
    char dir[256];
    rlw_mkfixture(dir, sizeof(dir), "garbage");
    char db_path[1200];
    snprintf(db_path, sizeof(db_path), "%s/node.db", dir);

    static const char *const junk =
        "this is an operator file, not a SQLite database\n";
    FILE *f = fopen(db_path, "wb");
    bool wrote = f && fwrite(junk, 1, strlen(junk), f) == strlen(junk);
    if (f)
        fclose(f);
    RLW_CHECK("garbage: fixture node.db written", wrote);

    int64_t size_before = -1;
    uint64_t hash_before = rlw_file_hash(db_path, &size_before);
    RLW_CHECK("garbage: fixture readable before the calls",
              hash_before != 0 && size_before == (int64_t)strlen(junk));

    for (int i = 0; i < RLW_LEAF_COUNT; i++)
        rlw_invoke(&g_rlw_leaves[i], dir);

    int64_t size_after = -1;
    uint64_t hash_after = rlw_file_hash(db_path, &size_after);
    int quarantined = rlw_count_entries(dir, "node.db.corrupt");

    if (hash_after != hash_before || quarantined != 0)
        rlw_list_dir("garbage", dir);

    RLW_CHECK("garbage: node.db still exists under its own name",
              access(db_path, F_OK) == 0);
    RLW_CHECK("garbage: node.db still byte-identical",
              size_after == size_before && hash_after == hash_before &&
              hash_after != 0);
    RLW_CHECK("garbage: no node.db.corrupt-* rename happened",
              quarantined == 0);

    test_rm_rf(dir);
    return failures;
}

int test_read_leaf_no_datadir_write(void)
{
    printf("\n=== read leaf writes nothing to the datadir ===\n");
    int failures = 0;

    failures += t_absent_node_db_is_not_created();
    failures += t_present_node_db_is_not_mutated();
    failures += t_garbage_node_db_is_not_quarantined();

    return failures;
}
