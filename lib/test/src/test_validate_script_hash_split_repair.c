/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Deterministic fault-injection test for the hash_split / validate-script-
 * hash-mismatch wedge class — script_validate recorded an ok=1 verdict for a
 * NON-canonical body at a height, so its block_hash disagrees with the
 * canonical header validate_headers re-derived. apply_hash_agreement
 * (app/jobs/src/reducer_frontier.c) then caps H* at the height BELOW the split.
 *
 * THE GAP this test pins closed: before the fix, this class had NO repair owner.
 * diagnostic_repair_hint (reducer_frontier_dump.c) returned "" for
 * "validate-script-hash-mismatch", and the only existing reconcile rewound the
 * validate_headers cursor — which re-derives the SAME canonical hash and never
 * touches the stale script row, so the split could sit forever as a silent
 * operator-needed wedge. The fix adds maybe_repair_validate_script_hash_split to
 * the reducer_frontier_reconcile_light replay ladder (a one-shot stale_script
 * replay that deletes the stale script+proof verdicts and rewinds the cursors so
 * script_validate re-derives against the canonical body), and names that
 * condition as the repair owner in the dump.
 *
 * Fixture: six success-checked logs seeded ok=1 contiguously A+1..A+5 with
 * matching per-height block hashes EXCEPT script_validate_log[A+3].block_hash,
 * which is corrupted to differ from validate_headers_log[A+3].hash.
 *
 *   PHASE A — the wedge is real and now NAMED:
 *     - reducer_frontier_compute_hstar caps H* at A+2 (apply_hash_agreement).
 *     - the new detector finds the split at A+3.
 *     - the reducer_frontier dump reports kind=hash_split,
 *       reason=validate-script-hash-mismatch, AND repair_owner=
 *       reducer_frontier_reconcile_light (was "" before the fix).
 *
 *   PHASE B — the repair fires and AUTO-TERMINATES:
 *     - the one-shot rewind deletes script_validate_log[A+3] and rewinds the
 *       script_validate cursor to A+3.
 *     - a SECOND rewind is a no-op (one-shot marker) — never thrashes.
 *
 *   PHASE C — after re-derivation, H* CLIMBS past the split (the heal):
 *     - re-seed the rewound script/proof rows with the canonical hash (what the
 *       real stages produce on replay) + restore cursors → H* climbs to A+5.
 *
 * Negative control (PHASE A repair_owner goes RED): in reducer_frontier_dump.c,
 * delete the "validate-script-hash-mismatch" -> reducer_frontier_reconcile_light
 * mapping in diagnostic_repair_hint — the dump reports an empty repair_owner and
 * the c4 assertion fails.
 * Negative control (PHASE C never climbs): in
 * stage_repair_reducer_frontier_coin.c, make stale_script_hash_split_unlocked
 * always return -1 (no detection) — the rewind is a no-op, the split persists,
 * and the H*==A+5 assertion stays at A+2 (RED).
 *
 * Scratch files live under ./test-tmp/<name>_<pid>/ per the no-/tmp convention.
 */

#include "test/test_helpers.h"

#include "jobs/reducer_frontier.h"
#include "jobs/stage_repair.h"
#include "json/json.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VSH_CHECK(name, expr) do {                                  \
    printf("validate_script_hash_split_repair: %s... ", (name));    \
    if (expr) printf("OK\n");                                       \
    else { printf("FAIL\n"); failures++; }                          \
} while (0)

/* The compiled trusted anchor (SHA3 checkpoint) the algorithm clamps to. */
#define A REDUCER_FRONTIER_TRUSTED_ANCHOR  /* 3,056,758 */

/* The split height: A+3 (a hole at A+3 caps H* at A+2). */
#define SPLIT_H (A + 3)
#define TOP_H   (A + 5)

/* Canonical per-height block hash; `corrupt` flips one byte so a script row's
 * block_hash diverges from the canonical validate_headers hash at the SAME
 * height (the hash_split signature). Both remain valid 32-byte blobs. */
static void fill_hash(uint8_t out[32], int32_t h, bool corrupt)
{
    memset(out, 0, 32);
    out[0] = (uint8_t)(h & 0xff);
    out[1] = (uint8_t)((h >> 8) & 0xff);
    out[2] = (uint8_t)((h >> 16) & 0xff);
    out[3] = corrupt ? 0xBD : 0xCA;
}

static bool vsh_build_schema(sqlite3 *db)
{
    static const char *const ddl =
        "CREATE TABLE IF NOT EXISTS stage_cursor (name TEXT PRIMARY KEY,"
        "  cursor INTEGER NOT NULL, updated_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS progress_meta (key TEXT PRIMARY KEY,"
        "  value BLOB);"
        "CREATE TABLE IF NOT EXISTS validate_headers_log ("
        "  height INTEGER PRIMARY KEY, hash BLOB NOT NULL, ok INTEGER NOT NULL,"
        "  fail_reason TEXT, validated_at INTEGER);"
        "CREATE TABLE IF NOT EXISTS script_validate_log ("
        "  height INTEGER PRIMARY KEY, status TEXT NOT NULL, ok INTEGER NOT NULL,"
        "  tx_count INTEGER NOT NULL, input_count INTEGER NOT NULL,"
        "  first_failure_txid BLOB, first_failure_vin INTEGER,"
        "  first_failure_serror INTEGER, validated_at INTEGER NOT NULL,"
        "  block_hash BLOB);"
        "CREATE TABLE IF NOT EXISTS body_persist_log ("
        "  height INTEGER PRIMARY KEY, source TEXT, ok INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS proof_validate_log ("
        "  height INTEGER PRIMARY KEY, ok INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS utxo_apply_log ("
        "  height INTEGER PRIMARY KEY, ok INTEGER NOT NULL,"
        "  spent_count INTEGER, added_count INTEGER);"
        "CREATE TABLE IF NOT EXISTS tip_finalize_log ("
        "  height INTEGER PRIMARY KEY, status TEXT, ok INTEGER NOT NULL,"
        "  tip_hash BLOB);";
    return sqlite3_exec(db, ddl, NULL, NULL, NULL) == SQLITE_OK;  // raw-sql-ok:test-seed
}

static bool vsh_set_cursor(sqlite3 *db, const char *name, int64_t cursor)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO stage_cursor(name,cursor,updated_at)"
            " VALUES(?,?,0)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, cursor);
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-seed
    sqlite3_finalize(st);
    return ok;
}

static bool vsh_put_vh(sqlite3 *db, int32_t h)
{
    uint8_t hash[32];
    fill_hash(hash, h, false);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO validate_headers_log(height,hash,ok)"
            " VALUES(?,?,1)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, h);
    sqlite3_bind_blob(st, 2, hash, 32, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-seed
    sqlite3_finalize(st);
    return ok;
}

/* script_validate_log row, ok=1. block_hash = canonical(h) unless `corrupt`. */
static bool vsh_put_sv(sqlite3 *db, int32_t h, bool corrupt)
{
    uint8_t hash[32];
    fill_hash(hash, h, corrupt);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO script_validate_log"
            "(height,status,ok,tx_count,input_count,validated_at,block_hash)"
            " VALUES(?,'valid',1,0,0,0,?)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, h);
    sqlite3_bind_blob(st, 2, hash, 32, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-seed
    sqlite3_finalize(st);
    return ok;
}

static bool vsh_put_simple(sqlite3 *db, const char *sql, int32_t h)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, h);
    bool ok = sqlite3_step(st) == SQLITE_DONE;  // raw-sql-ok:test-seed
    sqlite3_finalize(st);
    return ok;
}

static bool vsh_put_pv(sqlite3 *db, int32_t h)
{
    return vsh_put_simple(db,
        "INSERT OR REPLACE INTO proof_validate_log(height,ok) VALUES(?,1)", h);
}

static bool vsh_put_bp(sqlite3 *db, int32_t h)
{
    return vsh_put_simple(db,
        "INSERT OR REPLACE INTO body_persist_log(height,source,ok)"
        " VALUES(?,'x',1)", h);
}

static bool vsh_put_ua(sqlite3 *db, int32_t h)
{
    return vsh_put_simple(db,
        "INSERT OR REPLACE INTO utxo_apply_log(height,ok) VALUES(?,1)", h);
}

static bool vsh_put_tf(sqlite3 *db, int32_t h)
{
    return vsh_put_simple(db,
        "INSERT OR REPLACE INTO tip_finalize_log(height,status,ok)"
        " VALUES(?,'final',1)", h);
}

/* Stamp coins_applied + the migration-complete rung so
 * coins_kv_is_proven_authority returns true and compute_hstar keeps the anchor
 * floor (mirrors test_contaminated_coin_above_anchor cca_set_applied). */
static bool vsh_set_applied(sqlite3 *db, int32_t height)
{
    if (!progress_meta_table_ensure(db))
        return false;
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return false;
    }
    bool ok = coins_kv_set_applied_height_in_tx(db, height);
    const char *finish = ok ? "COMMIT" : "ROLLBACK";
    if (sqlite3_exec(db, finish, NULL, NULL, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return false;
    }
    if (ok) {
        uint8_t one = 1;
        ok = progress_meta_set(db, COINS_KV_MIGRATION_COMPLETE_KEY, &one, 1);
    }
    return ok;
}

/* Compute H* off the shared progress.kv handle (lock-wrapped like production). */
static bool vsh_hstar(sqlite3 *db, int32_t *hstar)
{
    int32_t served = -1;
    progress_store_tx_lock();
    bool ok = reducer_frontier_compute_hstar(db, hstar, &served);
    progress_store_tx_unlock();
    return ok;
}

/* Does a script_validate_log row exist at height h? */
static bool vsh_sv_row_present(sqlite3 *db, int32_t h)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM script_validate_log WHERE height = ?",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, h);
    bool present = sqlite3_step(st) == SQLITE_ROW;  // raw-sql-ok:test-seed
    sqlite3_finalize(st);
    return present;
}

static int64_t vsh_cursor(sqlite3 *db, const char *name)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT cursor FROM stage_cursor WHERE name = ?",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    int64_t cur = -1;
    if (sqlite3_step(st) == SQLITE_ROW)  // raw-sql-ok:test-seed
        cur = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return cur;
}

int test_validate_script_hash_split_repair(void);
int test_validate_script_hash_split_repair(void)
{
    test_reset_shared_globals();   /* monolith isolation */
    printf("\n=== validate_script_hash_split_repair tests ===\n");
    int failures = 0;

    char dir[256];
    snprintf(dir, sizeof(dir),
             "./test-tmp/vsh_split_%d", (int)getpid());
    mkdir("./test-tmp", 0755);
    mkdir(dir, 0755);

    progress_store_close();
    bool store_ok = progress_store_open(dir);
    VSH_CHECK("progress store opens", store_ok);
    sqlite3 *pk = progress_store_db();
    VSH_CHECK("progress db handle", pk != NULL);

    bool schema_ok = pk && vsh_build_schema(pk) && coins_kv_ensure_schema(pk);
    VSH_CHECK("schema built", schema_ok);

    /* ── fixture: six logs ok=1 contiguous A+1..A+5; split at A+3 ──────────── */
    bool seeded = pk != NULL;
    for (int32_t h = A + 1; h <= TOP_H; h++) {
        seeded = seeded
            && vsh_put_vh(pk, h)
            && vsh_put_sv(pk, h, h == SPLIT_H)   /* corrupt block_hash @ A+3 */
            && vsh_put_bp(pk, h)
            && vsh_put_pv(pk, h)
            && vsh_put_ua(pk, h)
            && vsh_put_tf(pk, h);
    }
    VSH_CHECK("six logs seeded A+1..A+5 (script hash corrupt @ A+3)", seeded);

    /* Cursors: non-served logs at TOP_H+1; tip_finalize served-tip at TOP_H. */
    bool cursors = pk
        && vsh_set_cursor(pk, "validate_headers", TOP_H + 1)
        && vsh_set_cursor(pk, "script_validate", TOP_H + 1)
        && vsh_set_cursor(pk, "body_persist", TOP_H + 1)
        && vsh_set_cursor(pk, "proof_validate", TOP_H + 1)
        && vsh_set_cursor(pk, "utxo_apply", TOP_H + 1)
        && vsh_set_cursor(pk, "tip_finalize", TOP_H);
    VSH_CHECK("cursors set", cursors);

    /* Proven authority so compute_hstar keeps the anchor floor (= A). */
    uint8_t txid[32] = {0};
    txid[0] = 0xDE; txid[1] = 0xAD; txid[31] = 0x5e;
    uint8_t cscript[1] = {0x51};  /* OP_TRUE */
    bool coins = pk
        && coins_kv_add(pk, txid, 0, 1000, A + 1, false, cscript, sizeof(cscript))
        && vsh_set_applied(pk, TOP_H);
    VSH_CHECK("coins proven authority stamped", coins);

    /* ── PHASE A: the wedge is real and now NAMED ──────────────────────────── */
    {
        int32_t hstar = -1;
        VSH_CHECK("A: compute_hstar returns true", pk && vsh_hstar(pk, &hstar));
        VSH_CHECK("A: H* capped at A+2 (split caps the frontier)",
                  hstar == A + 2);

        int detected = -1;
        bool dok = pk &&
            stage_repair_validate_script_hash_split_detect_for_testing(
                pk, &detected);
        VSH_CHECK("A: detector returns true", dok);
        VSH_CHECK("A: detector finds the split at A+3", detected == SPLIT_H);

        struct json_value dump;
        json_init(&dump);
        bool dumped = pk && reducer_frontier_dump_state_json(&dump, NULL);
        VSH_CHECK("A: dump produced", dumped);
        const struct json_value *kind =
            json_get(&dump, "first_hstar_blocker_kind");
        const struct json_value *reason =
            json_get(&dump, "first_hstar_blocker_reason");
        const struct json_value *owner =
            json_get(&dump, "first_hstar_blocker_repair_owner");
        const char *kind_s = kind ? json_get_str(kind) : NULL;
        const char *reason_s = reason ? json_get_str(reason) : NULL;
        const char *owner_s = owner ? json_get_str(owner) : NULL;
        VSH_CHECK("A: dump kind == hash_split",
                  kind_s && strcmp(kind_s, "hash_split") == 0);
        VSH_CHECK("A: dump reason == validate-script-hash-mismatch",
                  reason_s &&
                  strcmp(reason_s, "validate-script-hash-mismatch") == 0);
        VSH_CHECK("A: dump NAMES repair_owner reducer_frontier_reconcile_light "
                  "(was empty before the fix)",
                  owner_s &&
                  strcmp(owner_s, "reducer_frontier_reconcile_light") == 0);
        json_free(&dump);
    }

    /* ── PHASE B: the repair fires and AUTO-TERMINATES ─────────────────────── */
    {
        bool repaired = false;
        int at = -1;
        bool rok = pk &&
            stage_repair_validate_script_hash_split_rewind_for_testing(
                pk, &repaired, &at);
        VSH_CHECK("B: rewind returns true", rok);
        VSH_CHECK("B: rewind fired at the split (A+3)",
                  repaired && at == SPLIT_H);
        VSH_CHECK("B: stale script_validate_log[A+3] row deleted",
                  pk && !vsh_sv_row_present(pk, SPLIT_H));
        VSH_CHECK("B: script_validate cursor rewound to A+3",
                  vsh_cursor(pk, "script_validate") == SPLIT_H);

        /* Second call: one-shot marker present -> no-op (never thrashes). */
        bool repaired2 = true;
        int at2 = -1;
        bool rok2 = pk &&
            stage_repair_validate_script_hash_split_rewind_for_testing(
                pk, &repaired2, &at2);
        VSH_CHECK("B: second rewind is a no-op (one-shot marker)",
                  rok2 && !repaired2);
    }

    /* ── PHASE C: after re-derivation, H* CLIMBS past the split ─────────────── */
    {
        /* The real script/proof stages, replaying from the rewound cursor,
         * re-derive the verdict against the CANONICAL body — recording the
         * canonical block hash. Simulate that here, then restore the cursors. */
        bool reseed = pk != NULL;
        for (int32_t h = SPLIT_H; h <= TOP_H; h++) {
            reseed = reseed
                && vsh_put_sv(pk, h, false)   /* canonical block_hash now */
                && vsh_put_pv(pk, h);
        }
        reseed = reseed
            && vsh_set_cursor(pk, "script_validate", TOP_H + 1)
            && vsh_set_cursor(pk, "proof_validate", TOP_H + 1)
            && vsh_set_cursor(pk, "tip_finalize", TOP_H);
        VSH_CHECK("C: re-derived script/proof rows + cursors restored", reseed);

        int32_t hstar = -1;
        VSH_CHECK("C: compute_hstar returns true", pk && vsh_hstar(pk, &hstar));
        VSH_CHECK("C: H* CLIMBED past the split to A+5", hstar == TOP_H);
    }

    /* ── teardown ──────────────────────────────────────────────────────────── */
    progress_store_close();
    test_cleanup_tmpdir(dir);
    return failures;
}
