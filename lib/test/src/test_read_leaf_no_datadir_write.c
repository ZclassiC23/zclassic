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
 * explicit `datadir` input) in four states, and the only thing checked is
 * what the leaf left behind on disk:
 *
 *   absent    — an empty datadir. Afterwards NO file whose name begins
 *               "node.db" may exist. (Catches OPEN_CREATE.)
 *   present   — a migrated node.db carrying seeded snapshot_staging rows.
 *               Afterwards the file's byte length and FNV-1a content hash
 *               must be identical, both staging row counts must be
 *               identical, and no node.db.corrupt-* may exist. (Catches
 *               node_db_migrate and the two DELETEs.)
 *   foreign   — a real SQLite database that is NOT a node database: an
 *               operator's own file that happens to sit at node.db.
 *               Afterwards it must hold the same tables. (Catches
 *               create_schema, which the present case CANNOT see: forty
 *               CREATE TABLE IF NOT EXISTS statements against an
 *               already-migrated database change no bytes, which is why
 *               app.store.products reached node_db_open_runtime for months
 *               with the present case green.)
 *   garbage   — a node.db that is not a SQLite file at all. Afterwards it
 *               must still be there, byte-identical, under its own name.
 *               (Catches db_quarantine_files' rename.)
 *   walset    — both stores in WAL mode, which is what the node actually
 *               writes. Afterwards the directory's FILE SET must be exactly
 *               the set that went in. (Catches the WAL sidecars: a read-only
 *               connection to a WAL database materializes <db>-shm and
 *               <db>-wal and cannot unlink them on close, so a "read" leaves
 *               two files behind and voids any copy-proof taken beforehand.
 *               Invisible to every case above, which hash files that were
 *               already there and so cannot see one APPEAR.)
 *   walopen   — the same, with a live writer attached, where the fix must
 *               NOT be "assume the database is immutable": that read returns
 *               the database's pre-log past. The set must still be unchanged
 *               and the writer must still be able to commit.
 *
 * node.db is not the only database a read leaf can be pointed at, so the
 * absent/present/garbage states are asserted for the KERNEL store too —
 * <datadir>/consensus.db, the append-only fact log that is the authority
 * for every stage cursor. progress_store_open() opens it READWRITE|CREATE, runs a
 * rename migration, ensures its schema, and on a failed integrity check
 * rename()s it aside to consensus.db.corrupt-<ts> and installs a fresh
 * empty one. core.sync.frontier.offline reached exactly that, and while
 * this file watched only node.db* it would not have seen the damage.
 *
 * The reply is deliberately NOT asserted on: a read leaf is free to answer
 * "blocked, no node.db here" or to answer with data. The contract under
 * test is that pointing a read leaf at a directory never changes it.
 *
 * WHO IS UNDER TEST — derived, not remembered. The first version of this
 * file listed its six subjects as string literals, and the very next read
 * leaf to take a `datadir` (core.wallet.recovery.status) was added, called
 * node_db_open(), renamed a user's node.db to node.db.corrupt-<ts> while
 * answering "ok": true, and this file said nothing. A hand list cannot
 * catch the leaf nobody remembered to add to it. So case 6 walks the
 * compiled command registry (zcl_command_catalog()) and requires that
 * EVERY READY, non-branch, READ-effect leaf whose declared input keys
 * include `datadir` is either exercised here or named in g_rlw_uncovered
 * with a reason. Neither list may contain a leaf the registry does not,
 * the uncovered list is shrink-only, and the derived population is
 * floor-gated so an unlinked catalog cannot pass by proving nothing.
 *
 * Sibling: test_offline_datadir_query.c covers the two SCOPE_OFFLINE_COPY
 * leaves' answers; this file covers every read leaf's SIDE EFFECTS. */

#define _POSIX_C_SOURCE 200809L

#include "test/test_core.h"

#include "command/native_command.h"
#include "config/command_catalog.h"
#include "json/json.h"
#include "kernel/command_registry.h"
#include "models/database.h"
#include "storage/consensus_db.h"

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
 * datadir and the case would prove nothing.
 *
 * THIS TABLE IS NOT THE POPULATION. It used to be: six string literals,
 * hand-maintained, and the SEVENTH read leaf that took a `datadir`
 * (core.wallet.recovery.status) walked straight past it and shipped
 * calling node_db_open() — the exact defect this file exists to catch,
 * missed by this file. The population now comes from the compiled command
 * registry (t_registry_coverage below): every READY, non-branch,
 * READ-effect leaf whose declared input keys include `datadir`. Each one
 * must appear either here or in g_rlw_uncovered with a stated reason, so
 * a new read leaf can no longer be silently absent. */

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

/* core.identity.resolve wants a bare 32-byte key (64 hex, not all-zero),
 * not the 33-byte compressed form above. */
#define RLW_ZID_PUBKEY \
    "b4632d08485ff1df2db55b9dafd23347d1c47a457072a1e87be26896549a8737"

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
    /* The seventh. Declared READ, `datadir` defaults to the operator's LIVE
     * one, and it opened node.db with node_db_open() — so pointed at a
     * damaged database it renamed the user's wallet to
     * node.db.corrupt-<ts>, installed a fresh empty one, and answered
     * "ok": true. Data destruction reported as success, from a command the
     * catalog advertises as a read. It now opens through
     * zcl_native_node_db_open_readonly(). */
    { "core.wallet.recovery.status",
      zcl_native_handle_wallet_recovery_status,
      NULL, NULL,               NULL, NULL },
    /* The two leaves this file used to only NAME as open defects.
     *
     * app.store.products reached node_db_open_runtime — READWRITE|CREATE,
     * create_schema(), node_db_migrate() — so pointing this read leaf at a
     * datadir rewrote that datadir's schema. It now opens through
     * zcl_native_node_db_require_readonly().
     *
     * core.sync.frontier.offline reached progress_store_open, which opens
     * <datadir>/consensus.db READWRITE|CREATE, runs a rename migration,
     * ensures its schema, and QUARANTINES it — rename()s the append-only
     * fact log aside and installs a fresh empty one — on a failed integrity
     * check. It now opens through zcl_native_kernel_store_open_readonly(),
     * and the kernel-store observations below watch consensus.db the same
     * way the node.db ones watch node.db. */
    { "app.store.products",     zcl_native_handle_store_products,
      NULL, NULL,               NULL, NULL },
    { "core.sync.frontier.offline",
      zcl_native_handle_core_sync_frontier_offline,
      NULL, NULL,               NULL, NULL },
    /* Six of the pre-existing gaps, moved off the uncovered list because
     * they now have an on-disk proof rather than a promise. All six already
     * opened correctly; what was missing was anyone checking. They are
     * exercised with real inputs on purpose — a leaf that refuses before it
     * reaches the datadir proves nothing about the datadir, so
     * core.identity.resolve gets a selector and core.storage.query.offline
     * gets a statement it will actually run. */
    { "core.epoch.status",      zcl_native_handle_core_epoch_status,
      NULL, NULL,               NULL, NULL },
    { "core.epoch.verify",      zcl_native_handle_core_epoch_verify,
      NULL, NULL,               NULL, NULL },
    { "core.identity.list",     zcl_native_handle_core_identity_list,
      NULL, NULL,               NULL, NULL },
    { "core.identity.resolve",  zcl_native_handle_core_identity_resolve,
      "pubkey", RLW_ZID_PUBKEY, NULL, NULL },
    { "core.storage.query.offline",
      zcl_native_handle_core_storage_query_offline,
      "sql", "SELECT 1",        NULL, NULL },
    /* Narrower than the five above, and stated so: bootstatus reads
     * <datadir>/boot_status.json and never opens a database at all, so what
     * is proven here is only that it creates nothing and quarantines
     * nothing. That is the whole of its exposure, but do not read its
     * presence in this table as database coverage. */
    { "core.node.bootstatus",   zcl_native_handle_core_node_bootstatus,
      NULL, NULL,               NULL, NULL },
};

#define RLW_LEAF_COUNT ((int)(sizeof(g_rlw_leaves) / sizeof(g_rlw_leaves[0])))

/* ── the read leaves this file does NOT exercise, and why ──────────────
 *
 * Every entry is a READ leaf that takes a `datadir` and is not in the
 * table above. Being on this list is a STATED GAP, never an exemption:
 * t_registry_coverage refuses any derived leaf that is on neither list, so
 * the only way a new read leaf gets past this file is by someone writing
 * a line here and saying why. The count is ceilinged (RLW_UNCOVERED_MAX)
 * and shrink-only — this list can get shorter, never longer.
 *
 * These are the pre-existing gap: the hand table only ever named six
 * leaves, so the rest of the derived population was absent and nothing said
 * so. Deriving the population from the registry is what made them visible;
 * covering them is follow-on work, one entry deleted per leaf exercised. */
#define RLW_UNCOVERED_REASON_PREEXISTING                                 \
    "pre-existing gap: declared READ, takes datadir, never exercised "   \
    "here. Made visible by the registry-derived coverage check; delete "  \
    "this line by adding the leaf to g_rlw_leaves"

struct rlw_uncovered {
    const char *path;
    const char *why;
};

static const struct rlw_uncovered g_rlw_uncovered[] = {
    { "core.node.bootwait",           RLW_UNCOVERED_REASON_PREEXISTING },
    { "ops.debug.producer",           RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.package.publish.plan",   RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.package.search",         RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.package.show",           RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.package.recipe",         RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.package.verify",         RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.package.peers",          RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.contributor.packages",   RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.contributor.badges",     RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.reward.score",           RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.reward.eligible",        RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.reward.queue",           RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.reward.receipt",         RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.leaderboard.daily",      RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.leaderboard.weekly",     RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.leaderboard.monthly",    RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.leaderboard.all",        RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.badge.eligible",         RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.seed.status",            RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.seed.ratio",             RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.storage.status",         RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.release.verify",         RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.proof.walk",             RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.desc.resolve",           RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.endpoint.verify",        RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.endpoint.resolve",       RLW_UNCOVERED_REASON_PREEXISTING },
    { "zcode.endpoint.list",          RLW_UNCOVERED_REASON_PREEXISTING },
};

#define RLW_UNCOVERED_COUNT \
    ((int)(sizeof(g_rlw_uncovered) / sizeof(g_rlw_uncovered[0])))

/* SHRINK-ONLY ceiling. Raising it is the one edit that would turn this
 * whole coverage check back into the hand list it replaced. */
#define RLW_UNCOVERED_MAX 28

/* Anti-vacuous floor on the derived population itself: a coverage check
 * over an empty registry passes every assertion and proves nothing. Sits
 * below the live count (43) with headroom for ordinary removals. */
#define RLW_DERIVED_FLOOR 35

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

/* The directory's FILE SET, as a sorted newline-joined list of names. The
 * per-file hashes above cannot see a file APPEARING, and appearing is the
 * whole of the WAL-sidecar defect: a read-only connection to a WAL database
 * materializes <db>-shm and <db>-wal to read consistently and then cannot
 * unlink them on close, because unlinking needs the write lock it does not
 * hold. Every byte the reader was asked about is still there and correct, and
 * the directory is not the one the operator hashed. False on overflow or an
 * unreadable directory, which callers treat as a failed observation. */
#define RLW_SET_MAX_ENTRIES 64
#define RLW_SET_NAME_MAX 256

static bool rlw_dir_set(const char *dir, char *out, size_t out_size)
{
    if (!out || !out_size)
        return false;
    out[0] = '\0';
    DIR *d = opendir(dir);
    if (!d)
        return false;
    static char names[RLW_SET_MAX_ENTRIES][RLW_SET_NAME_MAX];
    int n = 0;
    bool overflow = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (n >= RLW_SET_MAX_ENTRIES ||
            strlen(e->d_name) >= RLW_SET_NAME_MAX) {
            overflow = true;
            break;
        }
        /* Insertion sort, so the list is order-independent: readdir order is
         * not stable and an unordered join would compare unequal for reasons
         * that have nothing to do with the contract. */
        int i = n++;
        while (i > 0 && strcmp(names[i - 1], e->d_name) > 0) {
            memcpy(names[i], names[i - 1], RLW_SET_NAME_MAX);
            i--;
        }
        snprintf(names[i], RLW_SET_NAME_MAX, "%s", e->d_name);
    }
    closedir(d);
    if (overflow)
        return false;
    for (int i = 0; i < n; i++) {
        size_t used = strlen(out);
        int wrote = snprintf(out + used, out_size - used, "%s\n", names[i]);
        if (wrote <= 0 || (size_t)wrote >= out_size - used)
            return false;
    }
    return true;
}

/* Is the database at `path` in WAL mode? Header byte 18 is the "file format
 * write version": 2 for WAL. Read straight out of the file rather than by
 * asking the code under test, so the anti-vacuous check below cannot be
 * satisfied by the same bug it is guarding. */
static bool rlw_is_wal(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    unsigned char h[20] = { 0 };
    size_t got = fread(h, 1, sizeof(h), f);
    fclose(f);
    return got == sizeof(h) && memcmp(h, "SQLite format 3", 16) == 0 &&
           h[18] == 2;
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

/* The byte pattern used for every "this is not a database" fixture. Chosen
 * to be something an operator could plausibly have left in the datadir, so
 * the failure reads as data loss rather than as a fuzz artefact. */
static const char *const g_rlw_junk =
    "this is an operator file, not a SQLite database\n";

static bool rlw_write_junk(const char *path)
{
    size_t len = strlen(g_rlw_junk);
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(g_rlw_junk, 1, len, f) == len;
    if (f)
        fclose(f);
    return ok;
}

/* ── kernel-store (consensus.db) fixture ───────────────────────────────
 *
 * A VALID but otherwise empty SQLite file at <datadir>/consensus.db. Empty
 * on purpose: there is nothing to migrate and nothing to read, so any byte
 * that changes afterwards is DDL the leaf itself wrote (progress_store_open
 * ensures the kernel schema). One real table, not a zero-byte placeholder,
 * so the file has a genuine SQLite header and passes an integrity check —
 * a quarantine after this would be unambiguous damage, not a rescue. */
static bool rlw_seed_kernel_store(const char *dir, char *path_out,
                                  size_t path_size)
{
    snprintf(path_out, path_size, "%s/%s", dir, CONSENSUS_DB_FILENAME);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path_out, &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL)
        != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return false;
    }
    bool ok = sqlite3_exec(db, "CREATE TABLE rlw_fixture(x INTEGER)", NULL,
                           NULL, NULL) == SQLITE_OK;
    sqlite3_close(db);
    return ok;
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

/* Same call, but hands back whether the leaf DISCLOSED that the read did not
 * happen. Not writing to a corrupt database is only half the contract; the
 * other half is that the caller is told. A leaf that opens a 47-byte text
 * file, gets nothing back, and answers "no results" has damaged nothing and
 * still told a lie — and two fee-spending pre-flights key on exactly that
 * distinction, so the lie costs money.
 *
 * There are two honest shapes, and which one is right depends on whether the
 * chain read is the leaf's payload or an optional enrichment:
 *   - refuse outright (error code set), for the leaves whose whole answer
 *     comes out of node.db; or
 *   - answer the rest and mark the section "read": false with a reason, for
 *     zcode.contributor.show, where the package index is the real payload
 *     and the ZNAM pointer is a garnish. Degrading is fine. Degrading
 *     silently is not: "read": true with "found": false over a database
 *     nobody could open is indistinguishable from "nobody claims this key".
 * Both count as disclosure. A bare empty answer counts as neither. */
static bool rlw_invoke_refused(const struct rlw_leaf *lf, const char *datadir,
                               char *code_out, size_t code_size)
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
    bool refused = reply.exit_code != 0 || reply.error.code[0] != '\0';
    /* The degrade-and-disclose shape: any section that carries an explicit
     * "read": false is a leaf saying, in the reply body, that it did not
     * look. Walk the top-level sections rather than naming one, so a leaf
     * that grows a second optional chain read is covered without editing
     * this test. */
    if (!refused && reply.data.type == JSON_OBJ) {
        for (size_t i = 0; i < reply.data.num_children && !refused; i++) {
            const struct json_value *sec = &reply.data.children[i];
            if (sec->type != JSON_OBJ)
                continue;
            const struct json_value *r = json_get(sec, "read");
            if (r && r->type == JSON_BOOL && !json_get_bool(r) &&
                json_get(sec, "reason"))
                refused = true;
        }
    }
    if (code_out && code_size)
        snprintf(code_out, code_size, "%s",
                 reply.error.code[0]      ? reply.error.code
                 : refused                ? "read:false+reason"
                                          : "-");
    printf("    [%s] status=%d exit=%d code=%s\n", lf->path, (int)reply.status,
           (int)reply.exit_code, reply.error.code[0] ? reply.error.code : "-");
    zcl_command_reply_free(&reply);
    json_free(&input);
    return refused;
}

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
        /* Same question for the kernel store, under both the current name
         * and the legacy one consensus_db_kernel_store_path() falls back
         * to: an empty datadir must not gain a fact log either. */
        int kleft = rlw_count_entries(dir, CONSENSUS_DB_FILENAME);
        int pleft = rlw_count_entries(dir, CONSENSUS_DB_LEGACY_KERNEL_FILENAME);
        if (left != 0 || kleft != 0 || pleft != 0)
            rlw_list_dir(lf->path, dir);
        snprintf(name, sizeof(name),
                 "%s: read leaf created no node.db* in an empty datadir",
                 lf->path);
        RLW_CHECK(name, left == 0);
        snprintf(name, sizeof(name),
                 "%s: read leaf created no kernel store in an empty datadir",
                 lf->path);
        RLW_CHECK(name, kleft == 0 && pleft == 0);

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

    char kernel_path[1200];
    RLW_CHECK("present: fixture consensus.db seeded",
              rlw_seed_kernel_store(dir, kernel_path, sizeof(kernel_path)));
    int64_t kernel_size_before = -1;
    uint64_t kernel_hash_before = rlw_file_hash(kernel_path,
                                               &kernel_size_before);
    RLW_CHECK("present: consensus.db readable before the calls",
              kernel_hash_before != 0 && kernel_size_before > 0);

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

    int64_t kernel_size_after = -1;
    uint64_t kernel_hash_after = rlw_file_hash(kernel_path,
                                              &kernel_size_after);
    int kernel_quarantined = rlw_count_entries(dir, "consensus.db.corrupt");
    if (kernel_hash_after != kernel_hash_before || kernel_quarantined != 0) {
        printf("    consensus.db size %lld -> %lld, hash %016llx -> %016llx\n",
               (long long)kernel_size_before, (long long)kernel_size_after,
               (unsigned long long)kernel_hash_before,
               (unsigned long long)kernel_hash_after);
        rlw_list_dir("present", dir);
    }
    RLW_CHECK("present: consensus.db byte length unchanged",
              kernel_size_after == kernel_size_before &&
              kernel_size_after > 0);
    RLW_CHECK("present: consensus.db content hash unchanged",
              kernel_hash_after == kernel_hash_before &&
              kernel_hash_after != 0);
    RLW_CHECK("present: nothing was quarantined to consensus.db.corrupt-*",
              kernel_quarantined == 0);

    test_rm_rf(dir);
    return failures;
}

/* ── case 3: a valid database that is not a NODE database ───────────────
 *
 * The byte-identity case above cannot see the smaller half of the ceremony.
 * create_schema() is `CREATE TABLE IF NOT EXISTS` forty times over and
 * node_db_migrate() is a no-op at the current version, so re-running both
 * against an already-migrated node.db and closing it leaves the file
 * byte-identical — app.store.products reached node_db_open_runtime for
 * months and the present case stayed green throughout.
 *
 * What that path actually does is INSTALL A SCHEMA into whatever file it
 * was handed. So point every read leaf at a real SQLite database that is
 * not a node database — an operator's own file that happens to sit at
 * <datadir>/node.db — and require that it comes back with the same tables
 * it went in with. A read leaf may find nothing there; it may not fix
 * that by writing the tables it wanted to read. */

static int64_t rlw_table_count(const char *db_path)
{
    return rlw_scalar(db_path,
                      "SELECT COUNT(*) FROM sqlite_master WHERE type='table'");
}

static int t_foreign_node_db_gets_no_schema(void)
{
    int failures = 0;
    char dir[256];
    rlw_mkfixture(dir, sizeof(dir), "foreign");
    char db_path[1200];
    snprintf(db_path, sizeof(db_path), "%s/node.db", dir);

    sqlite3 *seed = NULL;
    bool made = sqlite3_open_v2(db_path, &seed,
                                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                                NULL) == SQLITE_OK &&
                sqlite3_exec(seed,
                             "CREATE TABLE operator_notes(note TEXT);"
                             "INSERT INTO operator_notes VALUES('mine');",
                             NULL, NULL, NULL) == SQLITE_OK;
    if (seed)
        sqlite3_close(seed);
    RLW_CHECK("foreign: fixture is a real SQLite file, not a node database",
              made);

    int64_t tables_before = rlw_table_count(db_path);
    /* Anti-vacuous: one table, so any schema install is a visible jump. */
    RLW_CHECK("foreign: fixture holds exactly its own 1 table",
              tables_before == 1);
    int64_t size_before = -1;
    uint64_t hash_before = rlw_file_hash(db_path, &size_before);
    RLW_CHECK("foreign: fixture readable before the calls",
              hash_before != 0 && size_before > 0);

    for (int i = 0; i < RLW_LEAF_COUNT; i++)
        rlw_invoke(&g_rlw_leaves[i], dir);

    int64_t tables_after = rlw_table_count(db_path);
    int64_t size_after = -1;
    uint64_t hash_after = rlw_file_hash(db_path, &size_after);
    int quarantined = rlw_count_entries(dir, "node.db.corrupt");

    if (tables_after != tables_before || hash_after != hash_before ||
        quarantined != 0) {
        printf("    tables %lld -> %lld, size %lld -> %lld, "
               "hash %016llx -> %016llx\n",
               (long long)tables_before, (long long)tables_after,
               (long long)size_before, (long long)size_after,
               (unsigned long long)hash_before,
               (unsigned long long)hash_after);
        rlw_list_dir("foreign", dir);
    }

    RLW_CHECK("foreign: no schema was installed into the operator's file",
              tables_after == tables_before && tables_after == 1);
    RLW_CHECK("foreign: the file is still byte-identical",
              size_after == size_before && hash_after == hash_before &&
              hash_after != 0);
    RLW_CHECK("foreign: nothing was quarantined to node.db.corrupt-*",
              quarantined == 0);

    test_rm_rf(dir);
    return failures;
}

/* ── case 4: a node.db that is not a database must not be renamed ──── */

static int t_garbage_node_db_is_not_quarantined(void)
{
    int failures = 0;
    char dir[256];
    rlw_mkfixture(dir, sizeof(dir), "garbage");
    char db_path[1200];
    snprintf(db_path, sizeof(db_path), "%s/node.db", dir);

    RLW_CHECK("garbage: fixture node.db written", rlw_write_junk(db_path));

    /* Same fixture for the kernel store. This is the one that used to be
     * silently destroyed: progress_store_open's integrity check fails on a
     * non-database, and its recovery is to rename the file to
     * consensus.db.corrupt-<ts> and install a fresh empty fact log. */
    char kernel_path[1200];
    snprintf(kernel_path, sizeof(kernel_path), "%s/%s", dir,
             CONSENSUS_DB_FILENAME);
    RLW_CHECK("garbage: fixture consensus.db written",
              rlw_write_junk(kernel_path));

    int64_t size_before = -1;
    uint64_t hash_before = rlw_file_hash(db_path, &size_before);
    RLW_CHECK("garbage: fixture readable before the calls",
              hash_before != 0 &&
              size_before == (int64_t)strlen(g_rlw_junk));
    int64_t kernel_size_before = -1;
    uint64_t kernel_hash_before = rlw_file_hash(kernel_path,
                                               &kernel_size_before);
    RLW_CHECK("garbage: consensus.db fixture readable before the calls",
              kernel_hash_before != 0 &&
              kernel_size_before == (int64_t)strlen(g_rlw_junk));

    for (int i = 0; i < RLW_LEAF_COUNT; i++)
        rlw_invoke(&g_rlw_leaves[i], dir);

    int64_t size_after = -1;
    uint64_t hash_after = rlw_file_hash(db_path, &size_after);
    int quarantined = rlw_count_entries(dir, "node.db.corrupt");
    int64_t kernel_size_after = -1;
    uint64_t kernel_hash_after = rlw_file_hash(kernel_path,
                                              &kernel_size_after);
    int kernel_quarantined = rlw_count_entries(dir, "consensus.db.corrupt");

    if (hash_after != hash_before || quarantined != 0 ||
        kernel_hash_after != kernel_hash_before || kernel_quarantined != 0)
        rlw_list_dir("garbage", dir);

    RLW_CHECK("garbage: node.db still exists under its own name",
              access(db_path, F_OK) == 0);
    RLW_CHECK("garbage: node.db still byte-identical",
              size_after == size_before && hash_after == hash_before &&
              hash_after != 0);
    RLW_CHECK("garbage: no node.db.corrupt-* rename happened",
              quarantined == 0);
    RLW_CHECK("garbage: consensus.db still exists under its own name",
              access(kernel_path, F_OK) == 0);
    RLW_CHECK("garbage: consensus.db still byte-identical",
              kernel_size_after == kernel_size_before &&
              kernel_hash_after == kernel_hash_before &&
              kernel_hash_after != 0);
    RLW_CHECK("garbage: no consensus.db.corrupt-* rename happened",
              kernel_quarantined == 0);

    test_rm_rf(dir);
    return failures;
}

/* ── case 5: a corrupt node.db must REFUSE, not answer empty ───────── */

/* sqlite3_open_v2 is lazy — it does not read the header, so a non-database
 * opens with SQLITE_OK and only fails at the first statement. A helper that
 * returned OK there would hand every caller a live-looking handle over a
 * file it had never read, and each caller's own "no rows" path would then
 * report an empty answer. Absent and unreadable would be the same answer
 * again, which is the entire thing this module exists to prevent. */
static int t_garbage_node_db_is_refused_not_empty(void)
{
    int failures = 0;
    char dir[256];
    rlw_mkfixture(dir, sizeof(dir), "refuse");
    char db_path[1200];
    snprintf(db_path, sizeof(db_path), "%s/node.db", dir);

    RLW_CHECK("refuse: fixture node.db written", rlw_write_junk(db_path));

    /* The kernel store is unreadable here too, so a leaf whose payload
     * comes out of consensus.db has the same duty to say so. */
    char kernel_path[1200];
    snprintf(kernel_path, sizeof(kernel_path), "%s/%s", dir,
             CONSENSUS_DB_FILENAME);
    RLW_CHECK("refuse: fixture consensus.db written",
              rlw_write_junk(kernel_path));

    for (int i = 0; i < RLW_LEAF_COUNT; i++) {
        char code[64] = { 0 };
        bool refused = rlw_invoke_refused(&g_rlw_leaves[i], dir, code,
                                          sizeof(code));
        char what[192];
        snprintf(what, sizeof(what),
                 "refuse: %s answers a refusal over an unreadable node.db "
                 "(got code=%s)", g_rlw_leaves[i].path, code);
        RLW_CHECK(what, refused);
    }

    test_rm_rf(dir);
    return failures;
}

/* ── case 6: a WAL datadir must come back with the same FILE SET ────────
 *
 * THE BUG THIS CASE EXISTS TO CATCH (reproduced against the vendored sqlite
 * 3.49.0, 2026-07-30): SQLITE_OPEN_READONLY is not enough to leave a WAL
 * database alone. A read-only connection still has to materialize the
 * wal-index before it can read consistently, so sqlite CREATES <db>-shm and
 * <db>-wal beside the database — and it cannot unlink them again on close,
 * because that needs the write lock a read-only connection does not hold. A
 * directory holding one 8192-byte node.db came out of the helper's own
 * sequence (open_v2 READONLY, PRAGMA query_only, PRAGMA schema_version,
 * close) holding node.db, node.db-shm (32768 bytes) and node.db-wal.
 *
 * Both stores under a datadir are WAL — app/models/src/database.c sets
 * journal_mode=WAL for node.db, lib/storage/src/progress_store.c for
 * consensus.db — so this was the ORDINARY case, not an edge.
 *
 * Every case above was structurally blind to it. They hash the main database
 * file and count a `.corrupt` prefix; nothing enumerated the directory, and a
 * file APPEARING changes no hash of a file that was already there. The
 * `present` case in particular has been running read leaves against a WAL
 * node.db all along (its fixture goes through node_db_open, which sets
 * journal_mode=WAL) and stayed green while the sidecars piled up beside it.
 *
 * Why the file set and not just "no -wal": the harm is to a copy-proof.
 * The recovery doctrine is copy the datadir, hash it, ask a read leaf about
 * the copy, and trust the hash still describes the disk. Two files appearing
 * voids that silently. And a read running as a different uid than the node —
 * an operator or a CI job inspecting a copy as root — leaves the sidecars
 * owned by the wrong user, in the way of the node's own later open. So what
 * is asserted is the property the proof depends on: the set of names in that
 * directory is the set that went in. */

/* How many of the leaves must still ANSWER over a healthy WAL datadir. Eight
 * do today (the rest refuse for their own reasons — no releases seeded, no
 * such domain, an unanchored key); the floor sits below that with room for
 * ordinary churn, and is the guard against a "fix" that keeps the directory
 * clean by refusing to open a WAL database at all. */
#define RLW_WAL_ANSWER_FLOOR 5

/* A kernel store in WAL mode, matching what progress_store_open() would leave
 * on disk. rlw_seed_kernel_store above is left on sqlite's default rollback
 * journal, so the cases that use it cannot see a sidecar; this one is the
 * production shape, and this case needs it or it would only be testing
 * node.db. */
static bool rlw_seed_kernel_store_wal(const char *dir, char *path_out,
                                      size_t path_size)
{
    snprintf(path_out, path_size, "%s/%s", dir, CONSENSUS_DB_FILENAME);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path_out, &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL)
        != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return false;
    }
    bool ok = sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL)
                  == SQLITE_OK &&
              sqlite3_exec(db, "CREATE TABLE rlw_fixture(x INTEGER)", NULL,
                           NULL, NULL) == SQLITE_OK;
    /* A clean close checkpoints and unlinks the sidecars, so the fixture
     * directory holds exactly the two database files — which is what a
     * cleanly-shut-down node's datadir, and any copy of one, looks like. */
    sqlite3_close(db);
    return ok;
}

static int t_wal_datadir_file_set_is_unchanged(void)
{
    int failures = 0;
    char dir[256];
    rlw_mkfixture(dir, sizeof(dir), "walset");
    char db_path[1200];
    snprintf(db_path, sizeof(db_path), "%s/node.db", dir);

    RLW_CHECK("walset: fixture node.db seeded",
              rlw_seed_node_db(dir, db_path));
    char kernel_path[1200];
    RLW_CHECK("walset: fixture consensus.db seeded",
              rlw_seed_kernel_store_wal(dir, kernel_path,
                                        sizeof(kernel_path)));

    /* ANTI-VACUOUS. If either fixture stopped being a WAL database this whole
     * case would pass while proving nothing about the defect it exists for —
     * a rollback-journal database needs no wal-index and grows no sidecars. */
    RLW_CHECK("walset: node.db really is in WAL mode (header byte 18 == 2)",
              rlw_is_wal(db_path));
    RLW_CHECK("walset: consensus.db really is in WAL mode",
              rlw_is_wal(kernel_path));

    /* And the fixture must START clean, or "no sidecars afterwards" would be
     * measuring a directory that never had a chance to be dirty. */
    char before[4096];
    bool got_before = rlw_dir_set(dir, before, sizeof(before));
    RLW_CHECK("walset: fixture file set observed before the calls", got_before);
    RLW_CHECK("walset: fixture starts with no node.db-wal/-shm",
              rlw_count_entries(dir, "node.db-") == 0);
    RLW_CHECK("walset: fixture starts with no consensus.db-wal/-shm",
              rlw_count_entries(dir, "consensus.db-") == 0);

    int64_t size_before = -1;
    uint64_t hash_before = rlw_file_hash(db_path, &size_before);
    int64_t kernel_size_before = -1;
    uint64_t kernel_hash_before = rlw_file_hash(kernel_path,
                                                &kernel_size_before);

    /* ANTI-VACUOUS, and the one that matters most here. "No sidecars were
     * created" is trivially satisfiable by an open that FAILS: refuse every
     * leaf and the directory is certainly untouched. So count the leaves that
     * answered, and require that a healthy WAL datadir still gets read. This
     * is what stops the fix from being "stop opening WAL databases". */
    int answered = 0;
    for (int i = 0; i < RLW_LEAF_COUNT; i++) {
        char code[64] = { 0 };
        if (!rlw_invoke_refused(&g_rlw_leaves[i], dir, code, sizeof(code)))
            answered++;
    }
    {
        char what[192];
        snprintf(what, sizeof(what),
                 "walset: %d leaves still READ the WAL datadir (floor %d) — "
                 "the clean directory is not just a failed open",
                 answered, RLW_WAL_ANSWER_FLOOR);
        RLW_CHECK(what, answered >= RLW_WAL_ANSWER_FLOOR);
    }

    char after[4096];
    bool got_after = rlw_dir_set(dir, after, sizeof(after));
    RLW_CHECK("walset: fixture file set observed after the calls", got_after);

    bool same_set = got_before && got_after && strcmp(before, after) == 0;
    if (!same_set) {
        printf("    file set BEFORE:\n%s", before);
        printf("    file set AFTER:\n%s", after);
        rlw_list_dir("walset", dir);
    }
    RLW_CHECK("walset: the datadir's file set is exactly what went in",
              same_set);

    /* Named separately from the set comparison so a failure says WHICH files
     * arrived rather than only that the set differs. */
    int node_sidecars = rlw_count_entries(dir, "node.db-");
    int kernel_sidecars = rlw_count_entries(dir, "consensus.db-");
    if (node_sidecars != 0 || kernel_sidecars != 0)
        rlw_list_dir("walset", dir);
    RLW_CHECK("walset: no node.db-wal/-shm was created by a read",
              node_sidecars == 0);
    RLW_CHECK("walset: no consensus.db-wal/-shm was created by a read",
              kernel_sidecars == 0);

    /* The old assertions still have to hold on a WAL fixture: not creating
     * sidecars is worthless if the fix got there by rewriting the database. */
    int64_t size_after = -1;
    uint64_t hash_after = rlw_file_hash(db_path, &size_after);
    int64_t kernel_size_after = -1;
    uint64_t kernel_hash_after = rlw_file_hash(kernel_path,
                                               &kernel_size_after);
    RLW_CHECK("walset: node.db is still byte-identical",
              size_after == size_before && hash_after == hash_before &&
              hash_after != 0);
    RLW_CHECK("walset: consensus.db is still byte-identical",
              kernel_size_after == kernel_size_before &&
              kernel_hash_after == kernel_hash_before &&
              kernel_hash_after != 0);
    RLW_CHECK("walset: nothing was quarantined",
              rlw_count_entries(dir, "node.db.corrupt") == 0 &&
              rlw_count_entries(dir, "consensus.db.corrupt") == 0);

    test_rm_rf(dir);
    return failures;
}

/* ── case 7: a WAL datadir a LIVE writer is attached to ─────────────────
 *
 * The fix cannot be "always use the immutable open". Measured on the same
 * sqlite: pointed at a WAL database a writer was attached to, an
 * immutable=1 read returned 1 row where the truth was 2 — it does not consult
 * the log. Trading two stray files for silently stale answers would be the
 * worse bug on a project whose read leaves default to the operator's LIVE
 * node, so this case pins the other half of the contract.
 *
 * With a writer attached the wal-index already exists, so there is nothing
 * left for a read to create — the file set must STILL come back unchanged,
 * and the writer must still be able to write afterwards. */
static int t_wal_datadir_with_live_writer(void)
{
    int failures = 0;
    char dir[256];
    rlw_mkfixture(dir, sizeof(dir), "walopen");
    char db_path[1200];
    snprintf(db_path, sizeof(db_path), "%s/node.db", dir);

    RLW_CHECK("walopen: fixture node.db seeded",
              rlw_seed_node_db(dir, db_path));
    char kernel_path[1200];
    RLW_CHECK("walopen: fixture consensus.db seeded",
              rlw_seed_kernel_store_wal(dir, kernel_path,
                                        sizeof(kernel_path)));

    /* The stand-in for the running node: a READWRITE connection that stays
     * open across every read below, with an uncommitted-to-the-main-file
     * INSERT sitting in its log. */
    struct node_db live;
    memset(&live, 0, sizeof(live));
    bool live_open = node_db_open(&live, db_path) && live.open;
    RLW_CHECK("walopen: a live writer holds node.db open", live_open);
    if (live_open)
        RLW_CHECK("walopen: the live writer has a row in its log",
                  sqlite3_exec(live.db,
                               "INSERT OR REPLACE INTO node_state(key,value)"
                               " VALUES('rlw_live','1')",
                               NULL, NULL, NULL) == SQLITE_OK);

    /* Its own sidecars are legitimately there, and are NOT this test's to
     * object to — what must not change is the set. */
    RLW_CHECK("walopen: the live writer's wal-index exists",
              rlw_count_entries(dir, "node.db-") > 0);

    char before[4096];
    bool got_before = rlw_dir_set(dir, before, sizeof(before));
    RLW_CHECK("walopen: file set observed before the calls", got_before);

    int answered = 0;
    for (int i = 0; i < RLW_LEAF_COUNT; i++) {
        char code[64] = { 0 };
        if (!rlw_invoke_refused(&g_rlw_leaves[i], dir, code, sizeof(code)))
            answered++;
    }
    {
        char what[192];
        snprintf(what, sizeof(what),
                 "walopen: %d leaves still READ through the live wal-index "
                 "(floor %d)", answered, RLW_WAL_ANSWER_FLOOR);
        RLW_CHECK(what, answered >= RLW_WAL_ANSWER_FLOOR);
    }

    char after[4096];
    bool got_after = rlw_dir_set(dir, after, sizeof(after));
    RLW_CHECK("walopen: file set observed after the calls", got_after);
    bool same_set = got_before && got_after && strcmp(before, after) == 0;
    if (!same_set) {
        printf("    file set BEFORE:\n%s", before);
        printf("    file set AFTER:\n%s", after);
    }
    RLW_CHECK("walopen: the file set is unchanged with a writer attached",
              same_set);
    RLW_CHECK("walopen: nothing was quarantined",
              rlw_count_entries(dir, "node.db.corrupt") == 0 &&
              rlw_count_entries(dir, "consensus.db.corrupt") == 0);

    /* The read must not have wedged the writer — a read leaf that leaves the
     * live node unable to commit has done damage of a different kind. */
    if (live_open) {
        RLW_CHECK("walopen: the live writer can still write afterwards",
                  sqlite3_exec(live.db,
                               "INSERT OR REPLACE INTO node_state(key,value)"
                               " VALUES('rlw_live_after','1')",
                               NULL, NULL, NULL) == SQLITE_OK);
        node_db_close(&live);
    }

    test_rm_rf(dir);
    return failures;
}

/* ── case 8: the population comes from the registry, not from memory ──
 *
 * The defect this case exists to catch is the one that hit this very file:
 * a seventh read leaf was added, took a `datadir`, opened it with the boot
 * ceremony, and this test — which enumerated its subjects as six string
 * literals — never noticed. The list is derived now. */

/* Is `key` one comma-separated token of `csv`? (Substring matching would
 * accept "datadirs" and "no_datadir".) */
static bool rlw_csv_has(const char *csv, const char *key)
{
    if (!csv || !key || !key[0])
        return false;
    size_t klen = strlen(key);
    for (const char *p = csv; *p;) {
        while (*p == ' ' || *p == ',')
            p++;
        const char *start = p;
        while (*p && *p != ',')
            p++;
        size_t len = (size_t)(p - start);
        while (len && start[len - 1] == ' ')
            len--;
        if (len == klen && strncmp(start, key, klen) == 0)
            return true;
    }
    return false;
}

/* A leaf this file is responsible for: dispatchable, declared READ, and
 * pointable at a caller-named datadir. */
static bool rlw_is_datadir_read_leaf(const struct zcl_command_spec *s)
{
    return s && s->path && s->path[0] &&
           s->availability == ZCL_COMMAND_READY &&
           s->mode != ZCL_COMMAND_MODE_BRANCH &&
           s->effect == ZCL_COMMAND_EFFECT_READ &&
           s->handler != NULL &&
           rlw_csv_has(s->input_keys, "datadir");
}

static int t_registry_coverage(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();

    RLW_CHECK("coverage: the command catalog is readable",
              reg != NULL && reg->commands != NULL && reg->count > 0);
    if (!reg || !reg->commands)
        return failures;   /* RLW_CHECK above already counted this */

    int derived = 0, uncovered_seen = 0;
    for (size_t i = 0; i < reg->count; i++) {
        const struct zcl_command_spec *s = &reg->commands[i];
        if (!rlw_is_datadir_read_leaf(s))
            continue;
        derived++;

        const struct rlw_leaf *exercised = NULL;
        for (int j = 0; j < RLW_LEAF_COUNT && !exercised; j++)
            if (strcmp(g_rlw_leaves[j].path, s->path) == 0)
                exercised = &g_rlw_leaves[j];

        const struct rlw_uncovered *listed = NULL;
        for (int j = 0; j < RLW_UNCOVERED_COUNT && !listed; j++)
            if (strcmp(g_rlw_uncovered[j].path, s->path) == 0)
                listed = &g_rlw_uncovered[j];

        char what[224];
        if (exercised && listed) {
            snprintf(what, sizeof(what),
                     "coverage: %s is exercised AND listed uncovered — pick "
                     "one", s->path);
            RLW_CHECK(what, false);
            continue;
        }
        if (!exercised && !listed) {
            printf("    [%s] declared READ, takes `datadir`, and is in "
                   "neither g_rlw_leaves nor g_rlw_uncovered\n", s->path);
            snprintf(what, sizeof(what),
                     "coverage: %s is accounted for (exercised or listed "
                     "with a reason)", s->path);
            RLW_CHECK(what, false);
            continue;
        }
        if (listed) {
            uncovered_seen++;
            /* An entry with no reason is silent absence with extra steps. */
            snprintf(what, sizeof(what),
                     "coverage: %s is listed uncovered WITH a stated reason",
                     s->path);
            RLW_CHECK(what, listed->why != NULL && listed->why[0] != '\0');
            continue;
        }
        /* Exercised: the pointer this file calls must be the pointer the
         * registry dispatches, or the case below proves a different leaf. */
        snprintf(what, sizeof(what),
                 "coverage: %s is exercised through the registry's own "
                 "handler pointer", s->path);
        RLW_CHECK(what, exercised->fn == s->handler);
    }

    printf("    derived=%d exercised_table=%d uncovered_table=%d "
           "uncovered_matched=%d\n",
           derived, RLW_LEAF_COUNT, RLW_UNCOVERED_COUNT, uncovered_seen);

    /* Anti-vacuous: every assertion above is over the derived set, so an
     * empty or unlinked catalog would pass them all. */
    {
        char what[192];
        snprintf(what, sizeof(what),
                 "coverage: %d READ leaves take a datadir (floor %d) — the "
                 "population is real", derived, RLW_DERIVED_FLOOR);
        RLW_CHECK(what, derived >= RLW_DERIVED_FLOOR);
    }

    /* Every table entry must name a leaf that still exists and still is a
     * datadir READ leaf. A stale line is how a list starts drifting back
     * into decoration. */
    for (int j = 0; j < RLW_LEAF_COUNT; j++) {
        bool found = false;
        for (size_t i = 0; i < reg->count && !found; i++)
            found = rlw_is_datadir_read_leaf(&reg->commands[i]) &&
                    strcmp(reg->commands[i].path, g_rlw_leaves[j].path) == 0;
        char what[224];
        snprintf(what, sizeof(what),
                 "coverage: exercised leaf %s is still a READ leaf taking a "
                 "datadir", g_rlw_leaves[j].path);
        RLW_CHECK(what, found);
    }
    {
        char what[192];
        snprintf(what, sizeof(what),
                 "coverage: every uncovered line matched a live leaf "
                 "(%d/%d)", uncovered_seen, RLW_UNCOVERED_COUNT);
        RLW_CHECK(what, uncovered_seen == RLW_UNCOVERED_COUNT);
    }

    /* SHRINK-ONLY. Covering a leaf deletes a line; nothing may add one. */
    {
        char what[192];
        snprintf(what, sizeof(what),
                 "coverage: uncovered list is %d, at or under its "
                 "shrink-only ceiling of %d",
                 RLW_UNCOVERED_COUNT, RLW_UNCOVERED_MAX);
        RLW_CHECK(what, RLW_UNCOVERED_COUNT <= RLW_UNCOVERED_MAX);
    }

    return failures;
}

int test_read_leaf_no_datadir_write(void)
{
    printf("\n=== read leaf writes nothing to the datadir ===\n");
    int failures = 0;

    failures += t_registry_coverage();
    failures += t_absent_node_db_is_not_created();
    failures += t_present_node_db_is_not_mutated();
    failures += t_foreign_node_db_gets_no_schema();
    failures += t_garbage_node_db_is_not_quarantined();
    failures += t_garbage_node_db_is_refused_not_empty();
    failures += t_wal_datadir_file_set_is_unchanged();
    failures += t_wal_datadir_with_live_writer();

    return failures;
}
