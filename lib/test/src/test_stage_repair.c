/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "jobs/stage_repair.h"
#include "primitives/block.h"
#include "storage/progress_store.h"
#include "util/stage.h"

#include <sqlite3.h>
#include <stdint.h>
#include <string.h>

/* These tests pin the destructive poison-rewind safety guards in
 * app/jobs/src/stage_repair.c. A prior unguarded rewind collapsed the
 * public tip from ~3.13M to ~47279, so each guard below is load-bearing:
 *   - a rewind is refused when any ok=1 row sits at/above the frontier
 *     (the regression guard that protects the Tier-2 public-tip floor);
 *   - a non-frontier rewind (height != active_tip+1) is refused;
 *   - a POISON_NONE height is a safe no-op (returns true, repaired=false);
 *   - header save/load round-trips, and rejects a hash mismatch and an
 *     oversized solution. */

#define STR_CHECK(name, expr) do { \
    printf("stage_repair: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

static bool exec_sql(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        printf("SQL failed: %s\n", err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

static bool seed_schema(sqlite3 *db)
{
    return
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS validate_headers_log ("
            "height INTEGER PRIMARY KEY, hash BLOB NOT NULL, ok INTEGER NOT NULL,"
            "fail_reason TEXT, validated_at INTEGER NOT NULL)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS body_fetch_log ("
            "height INTEGER PRIMARY KEY, hash BLOB NOT NULL, source TEXT NOT NULL,"
            "bytes INTEGER NOT NULL DEFAULT 0, fetched_at INTEGER NOT NULL,"
            "ok INTEGER NOT NULL, fail_reason TEXT)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS body_persist_log ("
            "height INTEGER PRIMARY KEY, source TEXT, ok INTEGER,"
            "persisted_at INTEGER)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS script_validate_log ("
            "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS proof_validate_log ("
            "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS utxo_apply_log ("
            "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS utxo_apply_delta ("
            "height INTEGER PRIMARY KEY)") &&
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS tip_finalize_log ("
            "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER)");
}

/* Seed every stage cursor. The poison-rewind has a downstream success-check
 * over the *_log tables, not the cursors, but the rewind also rewinds the
 * cursors so they must exist. */
static bool seed_cursors(sqlite3 *db, int validate_cursor,
                         int downstream_cursor)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO stage_cursor(name,cursor,updated_at) "
            "VALUES(?,?,1)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    static const char *const names[] = {
        "validate_headers",
        "body_fetch",
        "body_persist",
        "script_validate",
        "proof_validate",
        "utxo_apply",
        "tip_finalize",
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_text(st, 1, names[i], -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 2, i == 0 ? validate_cursor : downstream_cursor);
        if (sqlite3_step(st) != SQLITE_DONE) {
            ok = false;
            break;
        }
    }
    sqlite3_finalize(st);
    return ok;
}

/* Seed the DOWNSTREAM-STALE poison shape at `height`: validate_headers ok=1,
 * body_fetch skipped_invalid/header_validation_failed, and ok=0 downstream
 * rows. This is the shape stage_repair_header_solution_poison_mode classifies
 * as STAGE_REPAIR_POISON_DOWNSTREAM_STALE. */
static bool seed_downstream_poison(sqlite3 *db, int height)
{
    char sql[4096];
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO validate_headers_log"
        "(height,hash,ok,fail_reason,validated_at) "
        "VALUES(%d,zeroblob(32),1,NULL,1);"
        "INSERT OR REPLACE INTO body_fetch_log"
        "(height,hash,source,bytes,fetched_at,ok,fail_reason) "
        "VALUES(%d,zeroblob(32),'skipped_invalid',0,1,0,"
        "'header_validation_failed');"
        "INSERT OR REPLACE INTO body_persist_log"
        "(height,source,ok,persisted_at) VALUES(%d,'upstream_failed',0,1);"
        "INSERT OR REPLACE INTO script_validate_log"
        "(height,status,ok) VALUES(%d,'upstream_failed',0);"
        "INSERT OR REPLACE INTO proof_validate_log"
        "(height,status,ok) VALUES(%d,'upstream_failed',0);"
        "INSERT OR REPLACE INTO utxo_apply_log"
        "(height,status,ok) VALUES(%d,'upstream_failed',0);"
        "INSERT OR REPLACE INTO utxo_apply_delta(height) VALUES(%d);"
        "INSERT OR REPLACE INTO tip_finalize_log"
        "(height,status,ok) VALUES(%d,'upstream_failed',0);",
        height, height, height, height, height, height, height, height);
    return exec_sql(db, sql);
}

static bool seed_validate_poison(sqlite3 *db, int height, const char *reason)
{
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO validate_headers_log"
        "(height,hash,ok,fail_reason,validated_at) "
        "VALUES(%d,zeroblob(32),0,%s,1);"
        "INSERT OR REPLACE INTO body_fetch_log"
        "(height,hash,source,bytes,fetched_at,ok,fail_reason) "
        "VALUES(%d,zeroblob(32),'skipped_invalid',0,1,0,"
        "'header_validation_failed');",
        height, reason ? reason : "NULL", height);
    return exec_sql(db, sql);
}

static int cursor_for(sqlite3 *db, const char *name)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT cursor FROM stage_cursor WHERE name=?",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    int out = -1;
    if (sqlite3_step(st) == SQLITE_ROW)
        out = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return out;
}

static bool row_exists(sqlite3 *db, const char *table, int height)
{
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT 1 FROM %s WHERE height=?", table);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    bool found = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return found;
}

/* Build a deterministic, self-consistent header (hash matches the bytes) at
 * `height`. */
static void make_header(struct block_header *h, int height)
{
    block_header_init(h);
    h->nVersion = 4;
    h->hashPrevBlock.data[0] = (uint8_t)(height - 1);
    h->hashPrevBlock.data[1] = 0xA7;
    h->hashMerkleRoot.data[0] = (uint8_t)height;
    h->hashMerkleRoot.data[1] = 0xB8;
    h->hashFinalSaplingRoot.data[0] = (uint8_t)height;
    h->hashFinalSaplingRoot.data[1] = 0xC9;
    h->nTime = 1700000000u + (uint32_t)height;
    h->nBits = 0x1f07ffff;
    h->nNonce.data[0] = (uint8_t)height;
    h->nNonce.data[1] = 0xDA;
    h->nSolutionSize = 32;
    for (size_t i = 0; i < h->nSolutionSize; i++)
        h->nSolution[i] = (uint8_t)(height + (int)i);
}

/* Open a fresh progress_store in a tmpdir and seed schema + stage_cursor. */
static bool setup_case(const char *tag, char *dir, size_t dir_n, sqlite3 **db)
{
    test_make_tmpdir(dir, dir_n, "stage_repair", tag);
    if (!progress_store_open(dir))
        return false;
    *db = progress_store_db();
    /* stage_table_ensure creates the stage_cursor table the rewind rewinds. */
    return seed_schema(*db) && stage_table_ensure(*db);
}

static void teardown_case(const char *dir)
{
    progress_store_close();
    test_cleanup_tmpdir(dir);
}

int test_stage_repair(void)
{
    printf("\n=== stage_repair poison-rewind safety guard tests ===\n");
    int failures = 0;

    /* --- Guard 1: ok=1 row AT the frontier refuses the whole rewind. ---
     * This is the regression guard: a finalized (ok=1) row at/above the
     * frontier means the public tip is anchored there; the rewind must
     * refuse so the Tier-2 floor is never disturbed. */
    {
        char dir[256];
        sqlite3 *db = NULL;
        bool ok = setup_case("ok1_at_frontier", dir, sizeof(dir), &db);
        ok = ok && seed_cursors(db, 5, 5);
        ok = ok && seed_downstream_poison(db, 2);
        /* Poison the height we want to rewind (2), but plant a finalized
         * ok=1 row exactly AT the frontier (height 2). */
        ok = ok && exec_sql(db,
            "INSERT OR REPLACE INTO tip_finalize_log(height,status,ok) "
            "VALUES(2,'finalized',1)");

        struct stage_repair_header_solution_result res;
        /* frontier = active_tip+1 = 2 */
        bool rv = stage_repair_header_solution_poison_rewind(db, 2, 1, &res);

        ok = ok && rv == false;            /* refused */
        /* Nothing was deleted: the poisoned rows survive the refusal. */
        ok = ok && row_exists(db, "body_fetch_log", 2);
        ok = ok && row_exists(db, "validate_headers_log", 2);
        ok = ok && row_exists(db, "tip_finalize_log", 2);
        ok = ok && res.repaired == false;
        /* Cursors untouched. */
        ok = ok && cursor_for(db, "body_fetch") == 5;
        STR_CHECK("ok=1 row at frontier refuses the destructive rewind", ok);
        teardown_case(dir);
    }

    /* --- Guard 1b: ok=1 row ABOVE the frontier also refuses. --- */
    {
        char dir[256];
        sqlite3 *db = NULL;
        bool ok = setup_case("ok1_above_frontier", dir, sizeof(dir), &db);
        ok = ok && seed_cursors(db, 5, 5);
        ok = ok && seed_downstream_poison(db, 2);
        /* Finalized ok=1 row strictly ABOVE the frontier (height 4 > 2). */
        ok = ok && exec_sql(db,
            "INSERT OR REPLACE INTO utxo_apply_log(height,status,ok) "
            "VALUES(4,'applied',1)");

        struct stage_repair_header_solution_result res;
        bool rv = stage_repair_header_solution_poison_rewind(db, 2, 1, &res);

        ok = ok && rv == false;
        ok = ok && row_exists(db, "body_fetch_log", 2);
        ok = ok && res.repaired == false;
        ok = ok && cursor_for(db, "body_fetch") == 5;
        STR_CHECK("ok=1 row above frontier refuses the destructive rewind", ok);
        teardown_case(dir);
    }

    /* --- Guard 2: non-frontier rewind (height != active_tip+1) refused. --- */
    {
        char dir[256];
        sqlite3 *db = NULL;
        bool ok = setup_case("non_frontier", dir, sizeof(dir), &db);
        ok = ok && seed_cursors(db, 5, 5);
        ok = ok && seed_downstream_poison(db, 2);

        struct stage_repair_header_solution_result res;
        /* height=2 but active_tip=10 => frontier should be 11, not 2. */
        bool rv = stage_repair_header_solution_poison_rewind(db, 2, 10, &res);

        ok = ok && rv == false;
        /* Refused before any mutation: rows and cursors intact. */
        ok = ok && row_exists(db, "body_fetch_log", 2);
        ok = ok && cursor_for(db, "body_fetch") == 5;
        ok = ok && res.repaired == false;
        STR_CHECK("non-frontier rewind is refused", ok);
        teardown_case(dir);
    }

    /* --- Guard 3: POISON_NONE is a safe no-op (returns true, repaired=false).
     * A clean frontier with no poison shape must not delete anything. --- */
    {
        char dir[256];
        sqlite3 *db = NULL;
        bool ok = setup_case("poison_none", dir, sizeof(dir), &db);
        ok = ok && seed_cursors(db, 5, 5);
        /* No poison rows seeded at the frontier height 2. */

        struct stage_repair_header_solution_result res;
        bool rv = stage_repair_header_solution_poison_rewind(db, 2, 1, &res);

        ok = ok && rv == true;             /* no-op succeeds */
        ok = ok && res.repaired == false;
        ok = ok && res.mode == STAGE_REPAIR_POISON_NONE;
        /* Cursors untouched by the no-op. */
        ok = ok && cursor_for(db, "body_fetch") == 5;
        STR_CHECK("POISON_NONE frontier is a safe no-op", ok);
        teardown_case(dir);
    }

    /* --- Sanity: a clean DOWNSTREAM_STALE rewind (no ok=1 at/above frontier)
     * succeeds, deletes the downstream poison rows, rewinds the cursors, and
     * preserves the (ok=0) tip_finalize_log row (doctrine forbids deleting
     * tip_finalize_log rows). This proves the guards above reject the
     * dangerous case rather than the whole code path. --- */
    {
        char dir[256];
        sqlite3 *db = NULL;
        bool ok = setup_case("downstream_rewind", dir, sizeof(dir), &db);
        ok = ok && seed_cursors(db, 5, 5);
        ok = ok && seed_downstream_poison(db, 2);

        struct stage_repair_header_solution_result res;
        bool rv = stage_repair_header_solution_poison_rewind(db, 2, 1, &res);

        ok = ok && rv == true;
        ok = ok && res.repaired == true;
        ok = ok && res.mode == STAGE_REPAIR_POISON_DOWNSTREAM_STALE;
        /* Downstream rows deleted; validate row (ok=1) preserved. */
        ok = ok && !row_exists(db, "body_fetch_log", 2);
        ok = ok && row_exists(db, "validate_headers_log", 2);
        /* tip_finalize_log ok=0 row MUST survive (deletion banned). */
        ok = ok && row_exists(db, "tip_finalize_log", 2);
        /* Downstream cursors rewound to the frontier. */
        ok = ok && cursor_for(db, "body_fetch") == 2;
        ok = ok && cursor_for(db, "tip_finalize") == 2;
        /* validate_headers cursor untouched on a downstream rewind. */
        ok = ok && cursor_for(db, "validate_headers") == 5;
        STR_CHECK("downstream rewind deletes downstream, keeps tip_finalize_log",
                  ok);
        teardown_case(dir);
    }

    /* --- Classifier: source-hash mismatch is repairable; invalid is not. --- */
    {
        char dir[256];
        sqlite3 *db = NULL;
        bool ok = setup_case("validate_source_mismatch", dir, sizeof(dir), &db);
        ok = ok && seed_cursors(db, 5, 5);
        ok = ok && seed_validate_poison(
            db, 2, "'header-source-hash-mismatch'");

        ok = ok && stage_repair_header_solution_poison_mode(db, 2) ==
                   STAGE_REPAIR_POISON_VALIDATE_HASH_MISMATCH;

        struct stage_repair_header_solution_result res;
        bool rv = stage_repair_header_solution_poison_rewind(db, 2, 1, &res);
        ok = ok && rv == true;
        ok = ok && res.repaired == false;
        ok = ok && row_exists(db, "validate_headers_log", 2);
        ok = ok && row_exists(db, "body_fetch_log", 2);
        STR_CHECK("header-source hash mismatch is non-destructive repairable "
                  "validate poison",
                  ok);
        teardown_case(dir);
    }

    {
        char dir[256];
        sqlite3 *db = NULL;
        bool ok = setup_case("validate_invalid_solution", dir, sizeof(dir), &db);
        ok = ok && seed_cursors(db, 5, 5);
        ok = ok && seed_validate_poison(db, 2, "'invalid-solution'");

        ok = ok && stage_repair_header_solution_poison_mode(db, 2) ==
                   STAGE_REPAIR_POISON_NONE;
        STR_CHECK("invalid validate failure stays non-repairable", ok);
        teardown_case(dir);
    }

    /* --- Guard 4: header save/load round-trip. --- */
    {
        char dir[256];
        sqlite3 *db = NULL;
        bool ok = setup_case("header_roundtrip", dir, sizeof(dir), &db);

        struct block_header h;
        make_header(&h, 100);
        struct uint256 hash;
        block_header_get_hash(&h, &hash);

        ok = ok && stage_repair_header_solution_save(db, 100, &hash, &h);

        struct block_header loaded;
        ok = ok && stage_repair_header_solution_load(db, 100, &hash, &loaded);
        ok = ok && loaded.nVersion == h.nVersion;
        ok = ok && loaded.nTime == h.nTime;
        ok = ok && loaded.nBits == h.nBits;
        ok = ok && loaded.nSolutionSize == h.nSolutionSize;
        ok = ok && memcmp(loaded.nSolution, h.nSolution, h.nSolutionSize) == 0;
        ok = ok && uint256_eq(&loaded.hashPrevBlock, &h.hashPrevBlock);
        ok = ok && uint256_eq(&loaded.hashMerkleRoot, &h.hashMerkleRoot);
        /* The loaded header must hash to the stored hash. */
        struct uint256 rehash;
        block_header_get_hash(&loaded, &rehash);
        ok = ok && uint256_eq(&rehash, &hash);
        ok = ok && stage_repair_header_solution_available(db, 100, NULL);
        STR_CHECK("header solution save/load round-trips", ok);
        teardown_case(dir);
    }

    /* --- Guard 5a: save refuses a hash that does not match the header. --- */
    {
        char dir[256];
        sqlite3 *db = NULL;
        bool ok = setup_case("hash_mismatch", dir, sizeof(dir), &db);

        struct block_header h;
        make_header(&h, 200);
        struct uint256 wrong;
        memset(&wrong, 0, sizeof(wrong));
        wrong.data[0] = 0xFF;   /* deliberately not block_header_get_hash(h) */

        bool rv = stage_repair_header_solution_save(db, 200, &wrong, &h);
        ok = ok && rv == false;                 /* refused */
        /* Nothing persisted, so it is not available. */
        ok = ok && !stage_repair_header_solution_available(db, 200, NULL);
        STR_CHECK("save refuses a hash that mismatches the header bytes", ok);
        teardown_case(dir);
    }

    /* --- Guard 5b: save refuses an oversized solution (> MAX_SOLUTION_SIZE
     * via nSolutionSize past the backing array). --- */
    {
        char dir[256];
        sqlite3 *db = NULL;
        bool ok = setup_case("oversized_solution", dir, sizeof(dir), &db);

        struct block_header h;
        make_header(&h, 300);
        struct uint256 hash;
        block_header_get_hash(&h, &hash);
        /* Claim a solution length larger than the backing buffer. */
        h.nSolutionSize = sizeof(h.nSolution) + 1;

        bool rv = stage_repair_header_solution_save(db, 300, &hash, &h);
        ok = ok && rv == false;                 /* refused */
        ok = ok && !stage_repair_header_solution_available(db, 300, NULL);
        STR_CHECK("save refuses an oversized solution", ok);
        teardown_case(dir);
    }

    /* --- Guard 5c: save refuses a zero-length solution. --- */
    {
        char dir[256];
        sqlite3 *db = NULL;
        bool ok = setup_case("zero_solution", dir, sizeof(dir), &db);

        struct block_header h;
        make_header(&h, 400);
        struct uint256 hash;
        block_header_get_hash(&h, &hash);
        h.nSolutionSize = 0;

        bool rv = stage_repair_header_solution_save(db, 400, &hash, &h);
        ok = ok && rv == false;
        STR_CHECK("save refuses a zero-length solution", ok);
        teardown_case(dir);
    }

    /* --- Load round-trip rejection: a stored row whose expected_hash differs
     * is rejected by load. --- */
    {
        char dir[256];
        sqlite3 *db = NULL;
        bool ok = setup_case("load_expected_mismatch", dir, sizeof(dir), &db);

        struct block_header h;
        make_header(&h, 500);
        struct uint256 hash;
        block_header_get_hash(&h, &hash);
        ok = ok && stage_repair_header_solution_save(db, 500, &hash, &h);

        struct uint256 expected_wrong;
        memset(&expected_wrong, 0, sizeof(expected_wrong));
        expected_wrong.data[0] = 0xEE;
        struct block_header loaded;
        bool rv = stage_repair_header_solution_load(db, 500, &expected_wrong,
                                                    &loaded);
        ok = ok && rv == false;                 /* expected-hash guard rejects */
        /* But loading with the correct expected hash succeeds. */
        ok = ok && stage_repair_header_solution_load(db, 500, &hash, &loaded);
        STR_CHECK("load rejects a mismatched expected hash", ok);
        teardown_case(dir);
    }

    return failures;
}
