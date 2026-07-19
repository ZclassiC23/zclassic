/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_consensus_db_flip — the Wave A3 physical flip: migrate the reducer kernel
 * (Class A fingerprint set + Class B/D tables) out of progress.kv into
 * consensus.db, drop the migrated tables from progress.kv leaving only the Class
 * C projections, and stamp the schema-version marker.
 *
 * Load-bearing assertions:
 *   1. FRESH datadir: migrate is a clean no-op (no consensus.db minted).
 *   2. POPULATED progress.kv: migrate copies the kernel AND the Class B/D tables
 *      (stage *_log journals, utxo_apply_delta, producer session) into
 *      consensus.db but NOT the Class C projections; the drop then removes the
 *      migrated tables from progress.kv and keeps ONLY the projections.
 *   3. IDEMPOTENT: a second migrate + drop is a stable no-op.
 *   4. KILL-9 at the migrate boundary (fork + _exit after migrate, before drop)
 *      and at the drop boundary re-run clean.
 *   5. FOLD-CONTINUES: a stage_cursor migrated into consensus.db is still
 *      readable and advanceable after the flip. */

#include "test/test_helpers.h"

#include "storage/anchor_kv.h"
#include "storage/consensus_db.h"
#include "storage/coins_kv.h"
#include "storage/nullifier_kv.h"
#include "storage/progress_store.h"
#include "util/stage.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CKF(name, expr) do {                                               \
    if (expr) { printf("  consensus_db_flip: %s... OK\n", (name)); }        \
    else { printf("  consensus_db_flip: %s... FAIL\n", (name)); failures++; }\
} while (0)

static bool flip_exec(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK && err)
        printf("    (exec failed: %s)\n", err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

static bool flip_table_exists(const char *path, const char *name)
{
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_stmt *st = NULL;
    bool found = false;
    if (sqlite3_prepare_v2(
            db, "SELECT 1 FROM sqlite_schema WHERE type='table' AND name=?1",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        found = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return found;
}

static int64_t flip_count(const char *path, const char *name)
{
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM \"%s\"", name);
    sqlite3_stmt *st = NULL;
    int64_t n = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            n = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return n;
}

/* Read a named stage cursor directly from an open handle (no getter in the
 * public stage API). Returns -1 when absent. */
static int64_t flip_named_cursor(sqlite3 *db, const char *name)
{
    if (!db) return -1;
    sqlite3_stmt *st = NULL;
    int64_t v = -1;
    if (sqlite3_prepare_v2(db, "SELECT cursor FROM stage_cursor WHERE name=?1",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW)
            v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return v;
}

/* Build a populated progress.kv: the 7 fingerprint kernel tables, a
 * representative Class B set (utxo_apply_log, validate_headers_log,
 * utxo_apply_delta), a Class D producer session, and Class C projections
 * (address_index, txindex, created_outputs). */
static bool flip_build_source(const char *dir)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/progress.kv", dir);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    bool ok = coins_kv_ensure_schema(db) && anchor_kv_ensure_schema(db) &&
              nullifier_kv_ensure_schema(db) && progress_meta_table_ensure(db) &&
              stage_table_ensure(db);

    for (int i = 0; ok && i < 3; i++) {
        uint8_t txid[32];
        memset(txid, 0, 32);
        txid[0] = (uint8_t)(0x40 + i);
        txid[31] = 0xC0;
        uint8_t script[4] = {0x76, 0xa9, 0x14, (uint8_t)i};
        ok = coins_kv_add(db, txid, (uint32_t)i, 9000 + i, 200 + i, i == 0,
                          script, sizeof(script));
    }

    ok = ok &&
        flip_exec(db, "INSERT INTO sprout_anchors(anchor,height,tree) "
                      "VALUES(x'dd01',201,x'01')") &&
        flip_exec(db, "INSERT INTO sapling_anchors(anchor,height,tree) "
                      "VALUES(x'ee01',202,x'02')") &&
        flip_exec(db, "INSERT INTO anchor_state(pool,activation_cursor) "
                      "VALUES(0,0)") &&
        flip_exec(db, "INSERT INTO nullifiers(nf,pool,height) "
                      "VALUES(x'ff01',0,60)") &&
        flip_exec(db, "INSERT INTO stage_cursor(name,cursor,updated_at) "
                      "VALUES('utxo_apply',200,1)") &&
        flip_exec(db, "INSERT INTO progress_meta(key,value) "
                      "VALUES('coins_applied_height',x'c8')") &&
        /* Class B — stage journals + delta */
        flip_exec(db, "CREATE TABLE utxo_apply_log(height INTEGER PRIMARY KEY,"
                      "status TEXT NOT NULL)") &&
        flip_exec(db, "INSERT INTO utxo_apply_log VALUES(200,'ok')") &&
        flip_exec(db, "CREATE TABLE validate_headers_log(height INTEGER "
                      "PRIMARY KEY, ok INTEGER NOT NULL)") &&
        flip_exec(db, "INSERT INTO validate_headers_log VALUES(200,1)") &&
        flip_exec(db, "CREATE TABLE utxo_apply_delta(height INTEGER PRIMARY KEY,"
                      "delta BLOB NOT NULL)") &&
        flip_exec(db, "INSERT INTO utxo_apply_delta VALUES(200,x'00')") &&
        /* Class D — producer session */
        flip_exec(db, "CREATE TABLE consensus_state_producer_session("
                      "singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
                      "epoch BLOB NOT NULL)") &&
        flip_exec(db, "INSERT INTO consensus_state_producer_session "
                      "VALUES(1,x'abcd')") &&
        /* Class C — projections that MUST stay in progress.kv */
        flip_exec(db, "CREATE TABLE address_index(addr BLOB, height INTEGER)") &&
        flip_exec(db, "INSERT INTO address_index VALUES(x'01',1)") &&
        flip_exec(db, "CREATE TABLE txindex(txid BLOB PRIMARY KEY, off INTEGER)") &&
        flip_exec(db, "INSERT INTO txindex VALUES(x'02',1)") &&
        flip_exec(db, "CREATE TABLE created_outputs(txid BLOB, vout INTEGER)") &&
        flip_exec(db, "INSERT INTO created_outputs VALUES(x'03',0)");

    sqlite3_close(db);
    return ok;
}

int test_consensus_db_flip(void);
int test_consensus_db_flip(void)
{
    int failures = 0;
    char err[256];

    /* ── 1. FRESH datadir: no progress.kv → clean no-op ───────────────── */
    {
        char fresh[256];
        test_make_tmpdir(fresh, sizeof(fresh), "consensus_db_flip", "fresh");
        char fc[300];
        snprintf(fc, sizeof(fc), "%s/consensus.db", fresh);
        CKF("fresh migrate is a no-op success",
            consensus_db_migrate_from_progress(fresh, err, sizeof(err)));
        CKF("fresh mints no consensus.db", access(fc, F_OK) != 0);
        CKF("fresh drop is a no-op success",
            consensus_db_drop_migrated_from_progress(fresh, err, sizeof(err)));
        test_cleanup_tmpdir(fresh);
    }

    /* ── 2. POPULATED progress.kv: migrate + drop split ───────────────── */
    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "consensus_db_flip", "split");
        char cpath[300], ppath[300];
        snprintf(cpath, sizeof(cpath), "%s/consensus.db", dir);
        snprintf(ppath, sizeof(ppath), "%s/progress.kv", dir);

        CKF("source builds", flip_build_source(dir));
        CKF("migrate succeeds",
            consensus_db_migrate_from_progress(dir, err, sizeof(err)));
        CKF("consensus.db minted", access(cpath, F_OK) == 0);

        /* consensus.db has the kernel + Class B/D, NOT the Class C projections */
        CKF("consensus.db has coins", flip_count(cpath, "coins") == 3);
        CKF("consensus.db has utxo_apply_log",
            flip_count(cpath, "utxo_apply_log") == 1);
        CKF("consensus.db has validate_headers_log",
            flip_count(cpath, "validate_headers_log") == 1);
        CKF("consensus.db has utxo_apply_delta",
            flip_count(cpath, "utxo_apply_delta") == 1);
        CKF("consensus.db has producer session",
            flip_count(cpath, "consensus_state_producer_session") == 1);
        CKF("consensus.db does NOT have address_index (projection)",
            !flip_table_exists(cpath, "address_index"));
        CKF("consensus.db does NOT have txindex (projection)",
            !flip_table_exists(cpath, "txindex"));
        /* created_outputs is a kernel-store table (kernel-handle read/write), so
         * it MOVES to consensus.db despite the retention prune being decoupled. */
        CKF("consensus.db HAS created_outputs (kernel-read table)",
            flip_count(cpath, "created_outputs") == 1);

        /* progress.kv still intact pre-drop (kernel duplicates + projections) */
        CKF("progress.kv still has coins pre-drop",
            flip_table_exists(ppath, "coins"));

        CKF("drop succeeds",
            consensus_db_drop_migrated_from_progress(dir, err, sizeof(err)));

        /* progress.kv keeps ONLY the projections now */
        CKF("progress.kv keeps address_index",
            flip_count(ppath, "address_index") == 1);
        CKF("progress.kv keeps txindex", flip_count(ppath, "txindex") == 1);
        CKF("progress.kv dropped created_outputs (moved to consensus.db)",
            !flip_table_exists(ppath, "created_outputs"));
        CKF("progress.kv dropped coins", !flip_table_exists(ppath, "coins"));
        CKF("progress.kv dropped utxo_apply_log",
            !flip_table_exists(ppath, "utxo_apply_log"));
        CKF("progress.kv dropped producer session",
            !flip_table_exists(ppath, "consensus_state_producer_session"));

        /* Schema marker into consensus.db */
        {
            sqlite3 *cdb = NULL;
            bool opened = sqlite3_open_v2(cpath, &cdb,
                SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK;
            CKF("consensus.db opens for marker", opened);
            CKF("schema marker writes",
                cdb && consensus_db_write_schema_marker(cdb, err, sizeof(err)));
            if (cdb) sqlite3_close(cdb);
            uint8_t got[8];
            size_t glen = 0;
            bool found = false;
            sqlite3 *rc = NULL;
            if (sqlite3_open_v2(cpath, &rc, SQLITE_OPEN_READONLY, NULL) ==
                SQLITE_OK) {
                progress_meta_get(rc, CONSENSUS_DB_SCHEMA_VERSION_KEY, got,
                                  sizeof(got), &glen, &found);
                sqlite3_close(rc);
            }
            CKF("schema marker present", found && glen == 4 &&
                got[0] == (uint8_t)CONSENSUS_DB_SCHEMA_VERSION);
        }

        /* ── 3. IDEMPOTENT re-run ─────────────────────────────────────── */
        CKF("migrate idempotent no-op",
            consensus_db_migrate_from_progress(dir, err, sizeof(err)));
        CKF("drop idempotent no-op",
            consensus_db_drop_migrated_from_progress(dir, err, sizeof(err)));
        CKF("projections survive idempotent re-run",
            flip_count(ppath, "address_index") == 1);

        /* ── 5. FOLD-CONTINUES: cursor readable + advanceable post-flip ── */
        {
            sqlite3 *cdb = NULL;
            bool opened = sqlite3_open_v2(cpath, &cdb,
                SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK;
            CKF("consensus.db opens for fold", opened);
            CKF("migrated cursor reads back",
                flip_named_cursor(cdb, "utxo_apply") == 200);
            CKF("cursor advances post-flip",
                cdb && stage_set_named_cursor(cdb, "utxo_apply", 201));
            CKF("advanced cursor persists",
                flip_named_cursor(cdb, "utxo_apply") == 201);
            if (cdb) sqlite3_close(cdb);
        }

        test_cleanup_tmpdir(dir);
    }

    /* ── 4a. KILL-9 at migrate boundary (child migrates, _exit before drop) */
    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "consensus_db_flip", "kill_migrate");
        char cpath[300], ppath[300];
        snprintf(cpath, sizeof(cpath), "%s/consensus.db", dir);
        snprintf(ppath, sizeof(ppath), "%s/progress.kv", dir);
        CKF("kill-migrate source builds", flip_build_source(dir));

        pid_t pid = fork();
        if (pid == 0) {
            char cerr[256];
            (void)consensus_db_migrate_from_progress(dir, cerr, sizeof(cerr));
            _exit(0); /* crash right after migrate, before the drop */
        }
        int status = 0;
        (void)waitpid(pid, &status, 0);

        CKF("consensus.db durable across crash", access(cpath, F_OK) == 0);
        CKF("crash left progress.kv kernel intact (drop not yet run)",
            flip_table_exists(ppath, "coins"));
        /* Parent resumes the flip: drop completes idempotently. */
        CKF("post-crash drop completes",
            consensus_db_drop_migrated_from_progress(dir, err, sizeof(err)));
        CKF("post-crash progress.kv keeps projections",
            flip_count(ppath, "address_index") == 1);
        CKF("post-crash progress.kv kernel dropped",
            !flip_table_exists(ppath, "coins"));
        CKF("post-crash consensus.db kernel intact",
            flip_count(cpath, "coins") == 3);
        test_cleanup_tmpdir(dir);
    }

    /* ── 4b. KILL-9 at drop boundary (child migrates+drops, _exit) ─────── */
    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "consensus_db_flip", "kill_drop");
        char cpath[300], ppath[300];
        snprintf(cpath, sizeof(cpath), "%s/consensus.db", dir);
        snprintf(ppath, sizeof(ppath), "%s/progress.kv", dir);
        CKF("kill-drop source builds", flip_build_source(dir));

        pid_t pid = fork();
        if (pid == 0) {
            char cerr[256];
            (void)consensus_db_migrate_from_progress(dir, cerr, sizeof(cerr));
            (void)consensus_db_drop_migrated_from_progress(dir, cerr,
                                                           sizeof(cerr));
            _exit(0);
        }
        int status = 0;
        (void)waitpid(pid, &status, 0);

        /* Re-run the whole flip in the parent: everything is a stable no-op. */
        CKF("post-drop-crash migrate idempotent",
            consensus_db_migrate_from_progress(dir, err, sizeof(err)));
        CKF("post-drop-crash drop idempotent",
            consensus_db_drop_migrated_from_progress(dir, err, sizeof(err)));
        CKF("post-drop-crash consensus.db kernel intact",
            flip_count(cpath, "coins") == 3);
        CKF("post-drop-crash progress.kv keeps only projections",
            flip_table_exists(ppath, "address_index") &&
            !flip_table_exists(ppath, "coins"));
        test_cleanup_tmpdir(dir);
    }

    /* ── kernel_store_path shim: consensus.db when present, else progress.kv */
    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "consensus_db_flip", "path");
        char out[512];
        /* Neither file: falls back to the legacy progress.kv name. */
        CKF("kernel path falls back to progress.kv",
            consensus_db_kernel_store_path(dir, out, sizeof(out)) &&
            strstr(out, "progress.kv") != NULL);
        /* Mint a consensus.db → path resolves to it. */
        char cpath[300];
        snprintf(cpath, sizeof(cpath), "%s/consensus.db", dir);
        FILE *f = fopen(cpath, "w");
        if (f) fclose(f);
        CKF("kernel path prefers consensus.db when present",
            consensus_db_kernel_store_path(dir, out, sizeof(out)) &&
            strstr(out, "consensus.db") != NULL);
        test_cleanup_tmpdir(dir);
    }

    printf("consensus_db_flip: %d failures\n", failures);
    return failures;
}
