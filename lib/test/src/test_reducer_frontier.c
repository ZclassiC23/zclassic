/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Regression tests for the L0 authority reducer_frontier_compute_hstar.
 *
 * Each case builds a throwaway in-memory progress.kv with the REAL stage-log
 * schema (the same CREATE TABLE text the production *_log_store.c modules
 * emit), populates it for a specific tear topology, then asserts the exact
 * (hstar, served_floor) the algorithm must return. Assertions check exact
 * equality so they fail if compute_hstar drifts by even one height —
 * mutation-sensitive by construction.
 *
 * The fixture writes rows with plain sqlite3_exec/INSERT — this is TEST
 * scaffolding building the durable image, not production reducer code, so it
 * does not route through the AR lifecycle (no model, no progress.kv handle).
 * compute_hstar itself is the SELECT-only unit under test. */

#include "test/test_helpers.h"

#include "coins/utxo_commitment.h"
#include "config/db_service.h"
#include "config/runtime.h"
#include "jobs/reducer_frontier.h"
#include "models/database.h"
#include "storage/progress_store.h"
#include "storage/seal_kv.h"
#include "util/blocker.h"

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RF_CHECK(name, expr) do {                                  \
    printf("reducer_frontier: %s... ", (name));                    \
    if (expr) { printf("OK\n"); }                                  \
    else { printf("FAIL\n"); failures++; }                         \
} while (0)

/* reducer_frontier_nearest_self_verified_base() and its result struct are
 * declared only in the PRIVATE header app/jobs/src/reducer_frontier_rewind_
 * bases.h (deliberately not on the public app/jobs/include/jobs/ path — see
 * that file's own PURPOSE comment). This is the driver-facing selector the
 * sovereignty invariant under test (case_sovereign_base_ignores_borrowed_
 * higher_stamp below) exists to pin, so the test calls the REAL linked
 * symbol directly rather than only exercising it indirectly through the JSON
 * dump's own separately-implemented nearest-tracking. Mirrors the private
 * declaration byte-for-byte (same idiom as e.g. test_always_sync_selfheal.c's
 * local `extern bool stage_rederive_range(...)` and test_stage_repair_
 * script_refill.c's `extern bool stage_reducer_frontier_try_unapplied_hole_
 * clamp(...)` — a test-only extern of a private production symbol, no
 * production header/Makefile include-path change). */
struct reducer_frontier_rewind_base {
    int32_t height;
    bool    self_derived;
    bool    ratified;
    char    kind[32];
    char    commitment_sha3[65];
};
extern bool reducer_frontier_nearest_self_verified_base(
    int32_t at_or_below, struct reducer_frontier_rewind_base *out);

/* The anchor the production algorithm clamps to. Fixtures sit just above it
 * so the contiguous-prefix walk has something to traverse without building
 * three million rows. */
#define A REDUCER_FRONTIER_TRUSTED_ANCHOR  /* 3056758 */

/* ── fixture builder ─────────────────────────────────────────────────── */

/* Create the per-stage log tables and the stage_cursor / progress_meta
 * tables exactly as production does. Returns false on any SQLite error. */
static bool build_schema(sqlite3 *db)
{
    static const char *const ddl =
        "CREATE TABLE IF NOT EXISTS stage_cursor ("
        "  name TEXT PRIMARY KEY,"
        "  cursor INTEGER NOT NULL,"
        "  updated_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS progress_meta ("
        "  key TEXT PRIMARY KEY, value BLOB);"
        "CREATE TABLE IF NOT EXISTS header_admit_log ("
        "  height INTEGER PRIMARY KEY, hash BLOB NOT NULL,"
        "  parent_hash BLOB, admitted_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS validate_headers_log ("
        "  height INTEGER PRIMARY KEY, hash BLOB NOT NULL, ok INTEGER NOT NULL,"
        "  fail_reason TEXT, validated_at INTEGER);"
        "CREATE TABLE IF NOT EXISTS script_validate_log ("
        "  height INTEGER PRIMARY KEY, status TEXT, ok INTEGER NOT NULL,"
        "  block_hash BLOB);"
        "CREATE TABLE IF NOT EXISTS body_persist_log ("
        "  height INTEGER PRIMARY KEY, source TEXT, ok INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS proof_validate_log ("
        "  height INTEGER PRIMARY KEY, status TEXT, ok INTEGER NOT NULL,"
        "  block_hash BLOB);"
        "CREATE TABLE IF NOT EXISTS utxo_apply_log ("
        "  height INTEGER PRIMARY KEY, status TEXT, ok INTEGER NOT NULL,"
        "  spent_count INTEGER, added_count INTEGER);"
        "CREATE TABLE IF NOT EXISTS utxo_apply_delta ("
        "  height INTEGER PRIMARY KEY, branch_hash BLOB NOT NULL,"
        "  spent_blob BLOB NOT NULL, added_blob BLOB NOT NULL);"
        "CREATE TABLE IF NOT EXISTS tip_finalize_log ("
        "  height INTEGER PRIMARY KEY, status TEXT, ok INTEGER NOT NULL,"
        "  tip_hash BLOB);";
    char *err = NULL;
    if (sqlite3_exec(db, ddl, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "[test_reducer_frontier] schema: %s\n",
                err ? err : "(null)");
        sqlite3_free(err);
        return false;
    }
    return true;
}

/* Stamp coins_kv proven-authority on the fixture db so compute_hstar treats
 * the baked TRUSTED_ANCHOR as a REAL finality floor (the production path on any
 * seeded/migrated datadir). Without this, compute_hstar's phantom-anchor guard
 * lowers the floor to 0 — correct for a fresh datadir, but the cases below
 * model a real anchored datadir and assert the anchor-floor semantics. The
 * three rungs mirror coins_kv_is_proven_authority: an 8-byte LE
 * coins_applied_height, the 1-byte migration-complete stamp, and a non-empty
 * `coins` table. Returns false on any SQLite error. */
static bool stamp_proven_authority(sqlite3 *db, int64_t applied_height)
{
    char *err = NULL;
    if (sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS coins(k BLOB PRIMARY KEY, v BLOB);"
            "INSERT OR IGNORE INTO coins(k,v) VALUES(x'00', x'00');",
            NULL, NULL, &err) != SQLITE_OK) {
        sqlite3_free(err);
        return false;
    }
    uint8_t ah[8];
    for (int i = 0; i < 8; i++)
        ah[i] = (uint8_t)((uint64_t)applied_height >> (8 * i));
    uint8_t one = 1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO progress_meta(key,value) VALUES(?,?)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, "coins_applied_height", -1, SQLITE_STATIC);
    sqlite3_bind_blob(st, 2, ah, 8, SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) return false;
    st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO progress_meta(key,value) VALUES(?,?)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, "coins_kv_migration_complete", -1, SQLITE_STATIC);
    sqlite3_bind_blob(st, 2, &one, 1, SQLITE_STATIC);
    ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool set_cursor(sqlite3 *db, const char *name, int64_t cursor)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO stage_cursor(name,cursor,updated_at) VALUES(?,?,0) "
            "ON CONFLICT(name) DO UPDATE SET "
            "cursor=excluded.cursor, updated_at=excluded.updated_at",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, cursor);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

/* Insert an ok row into a *_log table that has a (height, ok) shape plus an
 * optional 32-byte hash blob in `hash_col` (NULL hash_col => no hash). For
 * validate_headers_log the hash column is NOT NULL, so a hash is always
 * supplied there via hbyte. */
static bool put_log_row(sqlite3 *db, const char *table, const char *hash_col,
                        int32_t height, int ok, const uint8_t hash[32],
                        const char *status)
{
    char sql[256];
    bool profile_bound = strcmp(table, "script_validate_log") == 0 ||
                         strcmp(table, "proof_validate_log") == 0 ||
                         strcmp(table, "utxo_apply_log") == 0;
    /* Successful consensus-validation rows must carry the exact production
     * evidence label. Failure rows retain their caller-supplied diagnosis. */
    const char *row_status = profile_bound && ok == 1 ? "verified" : status;
    if (hash_col && row_status)
        snprintf(sql, sizeof(sql),
                 "INSERT INTO %s(height,status,ok,%s) VALUES(?,?,?,?)",
                 table, hash_col);
    else if (hash_col)
        snprintf(sql, sizeof(sql),
                 "INSERT INTO %s(height,ok,%s) VALUES(?,?,?)",
                 table, hash_col);
    else if (row_status)
        snprintf(sql, sizeof(sql),
                 "INSERT INTO %s(height,status,ok) VALUES(?,?,?)", table);
    else
        snprintf(sql, sizeof(sql),
                 "INSERT INTO %s(height,ok) VALUES(?,?)", table);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "[test_reducer_frontier] prepare %s: %s\n",
                table, sqlite3_errmsg(db));
        return false;
    }
    int col = 1;
    sqlite3_bind_int64(st, col++, height);
    if (row_status)
        sqlite3_bind_text(st, col++, row_status, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, col++, ok);
    if (hash_col) {
        if (hash) sqlite3_bind_blob(st, col++, hash, 32, SQLITE_STATIC);
        else      sqlite3_bind_null(st, col++);
    }
    bool done = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!done)
        fprintf(stderr, "[test_reducer_frontier] step %s h=%d: %s\n",
                table, height, sqlite3_errmsg(db));
    return done;
}

/* A deterministic 32-byte hash keyed by height + a tag byte, so we can make
 * two logs agree (same tag) or disagree (different tag) at a height. */
static void synth_hash(uint8_t out[32], int32_t h, uint8_t tag)
{
    memset(out, 0, 32);
    out[0] = (uint8_t)(h & 0xff);
    out[1] = (uint8_t)((h >> 8) & 0xff);
    out[2] = (uint8_t)((h >> 16) & 0xff);
    out[31] = tag;
}

static bool put_header_admit(sqlite3 *db, int32_t h,
                             const uint8_t hash[32])
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO header_admit_log(height,hash,admitted_at) "
            "VALUES(?,?,0)", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, h);
    sqlite3_bind_blob(st, 2, hash, 32, SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static bool put_utxo_delta(sqlite3 *db, int32_t h,
                           const uint8_t hash[32])
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO utxo_apply_delta"
            "(height,branch_hash,spent_blob,added_blob) "
            "VALUES(?,?,x'',x'')", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, h);
    sqlite3_bind_blob(st, 2, hash, 32, SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

/* Write a full consistent ok=1 row across ALL stage logs at height h, with
 * validate_headers.hash and script_validate.block_hash AGREEING (tag 0). */
static bool put_consistent_height(sqlite3 *db, int32_t h)
{
    uint8_t hh[32];
    synth_hash(hh, h, 0);
    return put_header_admit(db, h, hh)
        && put_log_row(db, "validate_headers_log", "hash", h, 1, hh, NULL)
        && put_log_row(db, "script_validate_log", "block_hash", h, 1, hh,
                       "ok")
        && put_log_row(db, "body_persist_log", NULL, h, 1, NULL, NULL)
        && put_log_row(db, "proof_validate_log", "block_hash", h, 1, hh,
                       NULL)
        && put_log_row(db, "utxo_apply_log", NULL, h, 1, NULL, NULL)
        && put_utxo_delta(db, h, hh)
        && put_log_row(db, "tip_finalize_log", NULL, h, 1, NULL, "ok");
}

static bool put_validate_failure(sqlite3 *db, int32_t h, const char *reason)
{
    uint8_t hh[32];
    synth_hash(hh, h, 0);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO validate_headers_log(height,hash,ok,fail_reason) "
            "VALUES(?,?,0,?)",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "[test_reducer_frontier] prepare validate fail: %s\n",
                sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int64(st, 1, h);
    sqlite3_bind_blob(st, 2, hh, 32, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, reason ? reason : "", -1, SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok)
        fprintf(stderr, "[test_reducer_frontier] step validate fail h=%d: %s\n",
                h, sqlite3_errmsg(db));
    return ok;
}

static bool put_tip_anchor(sqlite3 *db, int32_t h)
{
    return put_log_row(db, "tip_finalize_log", NULL, h, 1, NULL, "anchor");
}

static bool set_log_status(sqlite3 *db, const char *table, int32_t height,
                           const char *status)
{
    char sql[128];
    int n = snprintf(sql, sizeof(sql),
                     "UPDATE %s SET status=? WHERE height=?", table);
    if (n < 0 || n >= (int)sizeof(sql))
        return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, status, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, height);
    bool ok = sqlite3_step(st) == SQLITE_DONE &&
              sqlite3_changes(db) == 1;
    sqlite3_finalize(st);
    return ok;
}

static bool set_hash_value(sqlite3 *db, const char *table,
                           const char *column, int32_t height,
                           const void *value, int length, bool as_text)
{
    char sql[160];
    int n = snprintf(sql, sizeof(sql), "UPDATE %s SET %s=? WHERE height=?",
                     table, column);
    if (n <= 0 || n >= (int)sizeof(sql))
        return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    int rc = as_text
        ? sqlite3_bind_text(st, 1, value, length, SQLITE_STATIC)
        : sqlite3_bind_blob(st, 1, value, length, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, height);
    bool ok = rc == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE &&
              sqlite3_changes(db) == 1;
    sqlite3_finalize(st);
    return ok;
}

/* Set every reducer cursor to `c` (the next-height frame == tip+1 in these
 * fixtures). tip_finalize is given the SAME value: under the served-tip
 * convention (task #31) its real cursor would be `c-1`, but
 * reducer_frontier_compute_hstar / reducer_anchor_candidate_ok normalize
 * tip_finalize's cursor to the next-height frame (cursor+1) before scanning,
 * so a tip_finalize cursor of either `c` (legacy +1 lattice) or `c-1` (new
 * served-tip value) yields the SAME H* here — these cases pin H* identically
 * across the convention change. */
static bool set_all_cursors(sqlite3 *db, int64_t c)
{
    return set_cursor(db, "validate_headers", c)
        && set_cursor(db, "body_fetch", c)
        && set_cursor(db, "body_persist", c)
        && set_cursor(db, "proof_validate", c)
        && set_cursor(db, "script_validate", c)
        && set_cursor(db, "utxo_apply", c)
        && set_cursor(db, "tip_finalize", c);
}

/* ── cases ───────────────────────────────────────────────────────────── */

/* (a) Fully-consistent multi-row fixture: every log ok=1 and hashes agree
 *     over [A+1 .. A+5]. H* must reach the tip A+5; served_floor == A+5. */
static int case_consistent(void)
{
    int failures = 0;
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) { return 1; }
    RF_CHECK("consistent: schema", build_schema(db));
    RF_CHECK("consistent: proven authority", stamp_proven_authority(db, A));

    const int32_t tip = A + 5;
    bool built = true;
    for (int32_t h = A + 1; h <= tip; h++)
        built = built && put_consistent_height(db, h);
    RF_CHECK("consistent: rows built", built);
    /* cursor names the NEXT height to process == tip+1. */
    RF_CHECK("consistent: cursors", set_all_cursors(db, tip + 1));

    int32_t hstar = -1, served = -1;
    bool ok = reducer_frontier_compute_hstar(db, &hstar, &served);
    RF_CHECK("consistent: returns true", ok);
    /* Mutation-sensitive: if the prefix walk stops early or off-by-one,
     * hstar != tip and this fails. */
    RF_CHECK("consistent: hstar == tip", hstar == tip);
    RF_CHECK("consistent: served_floor == tip", served == tip);

    sqlite3_close(db);
    return failures;
}

/* Every successful stage receipt above a trusted base must bind the same
 * selected-chain hash.  A proof receipt from fork B beside headers/scripts
 * from fork A, or a UTXO delta from B, cannot advance the serving frontier. */
static int case_proof_and_utxo_fork_split(void)
{
    int failures = 0;
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) return 1;
    RF_CHECK("proof-utxo-split: schema", build_schema(db));
    RF_CHECK("proof-utxo-split: proven authority", stamp_proven_authority(db,A));
    bool built = true;
    for (int32_t h = A + 1; h <= A + 3; h++)
        built = built && put_consistent_height(db, h);
    RF_CHECK("proof-utxo-split: rows", built);
    RF_CHECK("proof-utxo-split: cursors", set_all_cursors(db, A + 4));

    uint8_t canonical[32], forked[32];
    synth_hash(canonical, A + 2, 0);
    synth_hash(forked, A + 2, 9);
    RF_CHECK("proof-utxo-split: proof fork fixture",
             set_hash_value(db, "proof_validate_log", "block_hash", A + 2,
                            forked, 32, false));
    int32_t hstar = -1, served = -1;
    RF_CHECK("proof-utxo-split: proof computation",
             reducer_frontier_compute_hstar(db, &hstar, &served));
    RF_CHECK("proof-utxo-split: proof fork caps H*", hstar == A + 1);
    RF_CHECK("proof-utxo-split: proof restore",
             set_hash_value(db, "proof_validate_log", "block_hash", A + 2,
                            canonical, 32, false));

    RF_CHECK("proof-utxo-split: UTXO fork fixture",
             set_hash_value(db, "utxo_apply_delta", "branch_hash", A + 2,
                            forked, 32, false));
    RF_CHECK("proof-utxo-split: UTXO computation",
             reducer_frontier_compute_hstar(db, &hstar, &served));
    RF_CHECK("proof-utxo-split: UTXO fork caps H*", hstar == A + 1);
    static const char text_hash[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    RF_CHECK("proof-utxo-split: text UTXO hash fixture",
             set_hash_value(db, "utxo_apply_delta", "branch_hash", A + 2,
                            text_hash, 32, true));
    RF_CHECK("proof-utxo-split: text UTXO computation",
             reducer_frontier_compute_hstar(db, &hstar, &served));
    RF_CHECK("proof-utxo-split: text UTXO hash caps H*", hstar == A + 1);

    sqlite3_close(db);
    return failures;
}

/* Checkpoint-fold rows are transparent state-production evidence, never a
 * serving validation receipt. Every consensus-validation column must cap H*
 * below the first such row. A text-typed hash is likewise not a hash proof. */
static int case_validation_evidence_contained(void)
{
    int failures = 0;
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) { return 1; }
    RF_CHECK("evidence: schema", build_schema(db));
    RF_CHECK("evidence: proven authority", stamp_proven_authority(db, A));

    const int32_t tip = A + 3;
    bool built = true;
    for (int32_t h = A + 1; h <= tip; h++)
        built = built && put_consistent_height(db, h);
    RF_CHECK("evidence: rows built", built);
    RF_CHECK("evidence: cursors", set_all_cursors(db, tip + 1));

    static const char *const tables[] = {
        "script_validate_log", "proof_validate_log", "utxo_apply_log",
    };
    for (size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
        RF_CHECK("evidence: checkpoint row fixture",
                 set_log_status(db, tables[i], A + 2, "checkpoint_fold"));
        int32_t hstar = -1, served = -1;
        RF_CHECK("evidence: checkpoint computation succeeds",
                 reducer_frontier_compute_hstar(db, &hstar, &served));
        RF_CHECK("evidence: checkpoint row caps H*", hstar == A + 1);
        RF_CHECK("evidence: restore verified row",
                 set_log_status(db, tables[i], A + 2, "verified"));
    }

    sqlite3_stmt *st = NULL;
    bool text_hash = sqlite3_prepare_v2(db,
        "UPDATE script_validate_log SET block_hash=? WHERE height=?",
        -1, &st, NULL) == SQLITE_OK;
    if (text_hash) {
        static const char thirty_two_a[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        sqlite3_bind_text(st, 1, thirty_two_a, 32, SQLITE_STATIC);
        sqlite3_bind_int(st, 2, A + 2);
        text_hash = sqlite3_step(st) == SQLITE_DONE;
    }
    if (st)
        sqlite3_finalize(st);
    RF_CHECK("evidence: text-typed hash fixture", text_hash);
    int32_t hstar = -1, served = -1;
    RF_CHECK("evidence: malformed hash computation succeeds",
             reducer_frontier_compute_hstar(db, &hstar, &served));
    RF_CHECK("evidence: text-typed hash caps H*", hstar == A + 1);

    sqlite3_close(db);
    return failures;
}

/* (b) Torn fixture mirroring the live tear: utxo_apply has run forward (high
 *     cursor + ok=1 coin rows), but script_validate hit a not_script_valid
 *     ok=0 laggard at A+4. The contiguous-prefix MIN across logs caps at the
 *     block BEFORE that failure (A+3), even though utxo_apply/coins are
 *     applied much further. tip_finalize has stale ok=0 debris above A+3 AND
 *     a fresh ok=1 at A+3, so served_floor == A+3. */
static int case_torn(void)
{
    int failures = 0;
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) { return 1; }
    RF_CHECK("torn: schema", build_schema(db));
    RF_CHECK("torn: proven authority", stamp_proven_authority(db, A));

    bool built = true;
    /* A+1..A+3 fully consistent and finalized. */
    for (int32_t h = A + 1; h <= A + 3; h++)
        built = built && put_consistent_height(db, h);

    /* script_validate FAILS at A+4 (not_script_valid). validate_headers is
     * authoritative ahead (ok=1) and body/proof are ok=1 too, but the MIN
     * over logs is bounded by script_validate's break at A+4 => prefix A+3. */
    uint8_t h4[32]; synth_hash(h4, A + 4, 0);
    built = built
        && put_log_row(db, "validate_headers_log", "hash", A + 4, 1, h4, NULL)
        && put_log_row(db, "script_validate_log", "block_hash", A + 4, 0, NULL,
                       "not_script_valid")
        && put_log_row(db, "body_persist_log", NULL, A + 4, 1, NULL, NULL)
        && put_log_row(db, "proof_validate_log", NULL, A + 4, 1, NULL, NULL)
        /* utxo_apply forged forward (ok=1) far past the finalize laggard. */
        && put_log_row(db, "utxo_apply_log", NULL, A + 4, 1, NULL, NULL)
        && put_log_row(db, "utxo_apply_log", NULL, A + 5, 1, NULL, NULL)
        && put_log_row(db, "utxo_apply_log", NULL, A + 6, 1, NULL, NULL)
        /* tip_finalize: stale ok=0 debris above A+3 (the laggard never
         * advanced) — must NOT raise served_floor above the real ok=1. */
        && put_log_row(db, "tip_finalize_log", NULL, A + 4, 0, NULL, "stale")
        && put_log_row(db, "tip_finalize_log", NULL, A + 5, 0, NULL, "stale");
    RF_CHECK("torn: rows built", built);
    RF_CHECK("torn: vh hash", true);

    /* Cursors mirror the live drift: validate_headers authoritative far
     * ahead, utxo_apply ahead, tip_finalize lagging at the failure. */
    bool cur = set_cursor(db, "validate_headers", A + 7)
            && set_cursor(db, "body_fetch", A + 7)
            && set_cursor(db, "body_persist", A + 7)
            && set_cursor(db, "proof_validate", A + 7)
            && set_cursor(db, "script_validate", A + 5)
            && set_cursor(db, "utxo_apply", A + 7)
            && set_cursor(db, "tip_finalize", A + 5);
    RF_CHECK("torn: cursors", cur);

    /* coins_applied ahead of H* (the live "coins consistent, flag drift"
     * case): an 8-byte LE int64 blob, like coins_kv writes. */
    int64_t applied = A + 6;
    uint8_t blob[8];
    for (int i = 0; i < 8; i++) blob[i] = (uint8_t)((uint64_t)applied >> (8*i));
    sqlite3_stmt *st = NULL;
    bool meta_ok =
        sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO progress_meta(key,value) "
            "VALUES('coins_applied_height',?)", -1, &st, NULL) == SQLITE_OK;
    if (meta_ok) {
        sqlite3_bind_blob(st, 1, blob, 8, SQLITE_STATIC);
        meta_ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    RF_CHECK("torn: coins_applied meta", meta_ok);

    int32_t hstar = -1, served = -1;
    bool ok = reducer_frontier_compute_hstar(db, &hstar, &served);
    RF_CHECK("torn: returns true", ok);
    /* The prefix caps at A+3 (block before script_validate's ok=0). If the
     * algorithm wrongly trusted the forward utxo_apply cursor or ignored the
     * script_validate hole, hstar would be A+6 and this fails. */
    RF_CHECK("torn: hstar == A+3", hstar == A + 3);
    /* served_floor is the deepest ok=1 finalize (A+3) — the stale ok=0 debris
     * at A+4/A+5 must NOT raise it. */
    RF_CHECK("torn: served_floor == A+3", served == A + 3);
    /* H* must never exceed served_floor in a torn view (invariant). */
    RF_CHECK("torn: hstar <= served_floor", hstar <= served);

    sqlite3_close(db);
    return failures;
}

/* (b2) Sparse imported base: the reducer logs are intentionally absent across
 *      the imported/checkpointed middle, then a seed-anchor row marks a later
 *      trusted base and dense rows continue above it. H* must start from that
 *      valid seed anchor, not the compiled SHA3 checkpoint, and must ignore a
 *      stale higher active-tip anchor whose upstream cursors never reached it. */
static int case_sparse_seed_anchor(void)
{
    int failures = 0;
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) { return 1; }
    RF_CHECK("sparse-anchor: schema", build_schema(db));
    RF_CHECK("sparse-anchor: proven authority", stamp_proven_authority(db, A));

    const int32_t base = A + 100;
    const int32_t stale_high = A + 200;
    const int32_t tip = base + 5;
    bool built = put_tip_anchor(db, stale_high) && put_tip_anchor(db, base);
    for (int32_t h = base + 1; h <= tip; h++)
        built = built && put_consistent_height(db, h);
    RF_CHECK("sparse-anchor: rows built", built);
    RF_CHECK("sparse-anchor: cursors", set_all_cursors(db, tip + 1));

    int32_t hstar = -1, served = -1;
    bool ok = reducer_frontier_compute_hstar(db, &hstar, &served);
    RF_CHECK("sparse-anchor: returns true", ok);
    RF_CHECK("sparse-anchor: hstar reaches dense tip", hstar == tip);
    RF_CHECK("sparse-anchor: stale high anchor ignored", hstar < stale_high);
    RF_CHECK("sparse-anchor: served_floor sees stale public anchor",
             served == stale_high);

    sqlite3_close(db);
    return failures;
}

static bool put_int64_le_meta(sqlite3 *db, const char *key, int64_t v);

/* (b2.1) Blocks-less verified bundle base: the loader writes BOTH the
 * tip_finalize anchor row and the durable trusted-base declaration at `base`,
 * then sets reducer cursors to at least base+1. On a fresh bundle datadir the
 * first post-base reducer rows may be ABSENT until P2P body fetch catches up.
 * That rowless state must keep H* at the trusted base, not collapse to the
 * compiled checkpoint. A real ok=0 row remains covered by the collapse case
 * below. */
static int case_durable_base_accepts_rowless_first(void)
{
    int failures = 0;
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) { return 1; }
    RF_CHECK("rowless-base: schema", build_schema(db));

    const int32_t base = A + 100;
    RF_CHECK("rowless-base: proven authority",
             stamp_proven_authority(db, base + 1));
    RF_CHECK("rowless-base: seed anchor row", put_tip_anchor(db, base));
    RF_CHECK("rowless-base: durable trusted base key",
             put_int64_le_meta(db, REDUCER_TRUSTED_BASE_HEIGHT_KEY, base));

    /* No rows at base+1 in any reducer log. Cursors past base+1 model the
     * loader's block-index-derived stage cursors before those rows are
     * re-derived. */
    RF_CHECK("rowless-base: cursors", set_all_cursors(db, base + 2));

    int32_t hstar = -1, served = -1;
    bool ok = reducer_frontier_compute_hstar(db, &hstar, &served);
    RF_CHECK("rowless-base: returns true", ok);
    RF_CHECK("rowless-base: hstar == durable base", hstar == base);
    RF_CHECK("rowless-base: served_floor == seed anchor", served == base);

    RF_CHECK("rowless-base: exact-length TEXT authority planted",
             sqlite3_exec(db,
                 "UPDATE progress_meta SET value=CAST('12345678' AS TEXT) "
                 "WHERE key='" REDUCER_TRUSTED_BASE_HEIGHT_KEY "'",
                 NULL, NULL, NULL) == SQLITE_OK);
    RF_CHECK("rowless-base: TEXT authority fails closed",
             !reducer_frontier_compute_hstar(db, &hstar, &served));
    RF_CHECK("rowless-base: high-bit BLOB authority planted",
             put_int64_le_meta(db, REDUCER_TRUSTED_BASE_HEIGHT_KEY,
                               INT64_MIN));
    RF_CHECK("rowless-base: high-bit BLOB authority fails closed",
             !reducer_frontier_compute_hstar(db, &hstar, &served));

    sqlite3_close(db);
    return failures;
}

static int case_durable_base_survives_header_failure(void)
{
    int failures = 0;
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) { return 1; }
    RF_CHECK("header-fail-base: schema", build_schema(db));

    const int32_t base = A + 100;
    RF_CHECK("header-fail-base: proven authority",
             stamp_proven_authority(db, base + 1));
    RF_CHECK("header-fail-base: seed anchor row", put_tip_anchor(db, base));
    RF_CHECK("header-fail-base: durable trusted base key",
             put_int64_le_meta(db, REDUCER_TRUSTED_BASE_HEIGHT_KEY, base));
    RF_CHECK("header-fail-base: validate failure row",
             put_validate_failure(db, base + 1,
                                  "no-header-solution-backfill-required"));
    RF_CHECK("header-fail-base: cursors", set_all_cursors(db, base + 2));

    int32_t hstar = -1, served = -1;
    bool ok = reducer_frontier_compute_hstar(db, &hstar, &served);
    RF_CHECK("header-fail-base: returns true", ok);
    RF_CHECK("header-fail-base: hstar stays at durable base", hstar == base);
    RF_CHECK("header-fail-base: served_floor == seed anchor", served == base);

    sqlite3_close(db);
    return failures;
}

/* (b3) RECURRING POST-COLD-IMPORT WEDGE GUARD — the 2026-06-13 anchor-collapse.
 *
 * Models the live tear class exactly: a cold import seeded a trusted base at
 * `base` (declared BOTH as a tip_finalize status='anchor' row AND the durable
 * REDUCER_TRUSTED_BASE_HEIGHT_KEY, the way the import path writes it) over a
 * LOG-LESS imported region [A+1 .. base-1] — a terminal UTXO snapshot carries
 * no per-height reducer rows. Forward progress then reached base+2, but the
 * canonical block at base+1 legitimately spends a coin the (orphan-seeded)
 * import never installed, so script_validate HONESTLY recorded ok=0
 * (prevout_unresolved) there while every other stage at base+1 is ok=1.
 *
 * reducer_anchor_candidate_ok(base) probes the first row above the base
 * (base+1), finds script_validate ok=0, and REJECTS the base — both via the
 * tip_finalize anchor-row scan and via the durable-base-key raise — so the
 * trusted anchor collapses to the compiled SHA3 checkpoint A. The imported span
 * being log-less, H* then falls all the way to A while served_floor still
 * reports the imported tip. That ~88k-height gap is the wedge the I4.3 sweep
 * latches into operator_needed.
 *
 * This case PINS that hstar == A is the CORRECT, consensus-safe answer: H* must
 * NEVER float up to the trusted base / served_floor over a REAL ok=0 — that was
 * adversarially refuted as consensus-UNSAFE (it would seal a torn coin set as
 * finalized-by-construction). The durable remedy is upstream (refuse the torn
 * import at write time) plus making the I4.3 *verdict* honest in
 * invariant_sentinel; neither changes this value. A future "unwedge by raising
 * H*" regression fails here, loudly. */
static bool put_int64_le_meta(sqlite3 *db, const char *key, int64_t v)
{
    uint8_t blob[8];
    for (int i = 0; i < 8; i++)
        blob[i] = (uint8_t)((uint64_t)v >> (8 * i));
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO progress_meta(key,value) VALUES(?,?)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_blob(st, 2, blob, 8, SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static int case_anchor_collapse_after_forward_ok0(void)
{
    int failures = 0;
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) { return 1; }
    RF_CHECK("collapse: schema", build_schema(db));
    RF_CHECK("collapse: proven authority", stamp_proven_authority(db, A));

    const int32_t base = A + 100;   /* the cold-import terminal tip */

    /* Trusted base declared the way the import path writes it: a seed-anchor
     * row AND the durable height key. */
    RF_CHECK("collapse: seed anchor row", put_tip_anchor(db, base));
    RF_CHECK("collapse: durable trusted base key",
             put_int64_le_meta(db, REDUCER_TRUSTED_BASE_HEIGHT_KEY, base));

    /* [A+1 .. base-1] is intentionally LOG-LESS (no rows) — the imported span.
     * base+1 = the canonical spend block: every stage ok=1 EXCEPT script_validate,
     * which honestly recorded ok=0 (prevout_unresolved on the missing coin). No
     * tip_finalize row at base+1 (the block is block-not-finalized-by-reducer). */
    uint8_t h1[32]; synth_hash(h1, base + 1, 0);
    bool row =
        put_log_row(db, "validate_headers_log", "hash", base + 1, 1, h1, NULL)
        && put_log_row(db, "script_validate_log", "block_hash", base + 1, 0,
                       NULL, "prevout_unresolved")
        && put_log_row(db, "body_persist_log", NULL, base + 1, 1, NULL, NULL)
        && put_log_row(db, "proof_validate_log", NULL, base + 1, 1, NULL, NULL)
        && put_log_row(db, "utxo_apply_log", NULL, base + 1, 1, NULL, NULL);
    RF_CHECK("collapse: spend-block rows", row);

    /* Forward progress reached base+2 across every stage (coins forged ahead,
     * the live drift) so the candidate gate PROBES the ok=0 at base+1 rather
     * than stopping short of it. */
    RF_CHECK("collapse: cursors", set_all_cursors(db, base + 2));
    /* coins_applied forged forward to base+1 — the live coins-ahead tear. */
    RF_CHECK("collapse: coins_applied meta",
             put_int64_le_meta(db, "coins_applied_height", base + 1));

    int32_t hstar = -1, served = -1;
    bool ok = reducer_frontier_compute_hstar(db, &hstar, &served);
    RF_CHECK("collapse: returns true", ok);
    /* The trusted base is rejected over the real ok=0; H* collapses to the
     * compiled SHA3 checkpoint. This MUST stay A — raising it would seal a torn
     * coin set as final (refuted consensus-unsafe). */
    RF_CHECK("collapse: hstar == anchor (refuses to float over real ok=0)",
             hstar == A);
    /* served_floor still reports the imported tip's seed anchor — the wedge is
     * precisely H* << served_floor (the log-less span read as an ~88k hole). */
    RF_CHECK("collapse: served_floor == base (imported tip)", served == base);
    RF_CHECK("collapse: hstar < served_floor (the torn-view gap)",
             hstar < served);

    sqlite3_close(db);
    return failures;
}

/* (c) Clamp-up: the only logged rows are an ok=0 failure just ABOVE the
 *     anchor, so the contiguous prefix would compute to (anchor) and a hash
 *     split below the anchor must never pull it lower. Even with an empty
 *     finalize log (served_floor 0), H* is clamped UP to the trusted anchor,
 *     never below it. */
static int case_clamp_up(void)
{
    int failures = 0;
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) { return 1; }
    RF_CHECK("clamp: schema", build_schema(db));
    RF_CHECK("clamp: proven authority", stamp_proven_authority(db, A));

    /* script_validate fails immediately at anchor+1 -> contiguous prefix is
     * exactly the anchor. No tip_finalize ok=1 rows at all. */
    /* validate_headers_log has no `status` column (its hash is NOT NULL), so
     * supply status=NULL there; script_validate_log carries the status text. */
    uint8_t zero[32] = {0};
    bool built =
        put_log_row(db, "script_validate_log", "block_hash", A + 1, 0, NULL,
                    "not_script_valid")
        && put_log_row(db, "validate_headers_log", "hash", A + 1, 0,
                       zero, NULL);
    RF_CHECK("clamp: rows built", built);
    RF_CHECK("clamp: cursors", set_all_cursors(db, A + 2));

    int32_t hstar = -1, served = -1;
    bool ok = reducer_frontier_compute_hstar(db, &hstar, &served);
    RF_CHECK("clamp: returns true", ok);
    /* The hard guard raises H* to the anchor; it must NEVER be below it. */
    RF_CHECK("clamp: hstar == anchor", hstar == A);
    RF_CHECK("clamp: hstar >= TRUSTED_ANCHOR", hstar >= A);
    RF_CHECK("clamp: served_floor == 0 (no ok=1 finalize)", served == 0);

    sqlite3_close(db);
    return failures;
}

/* (d) Hash split: validate_headers and script_validate both present with
 *     non-NULL hashes that DISAGREE at A+3. H* must cap at A+2 even though
 *     every log shows ok=1 through A+5. Guards C3. */
static int case_hash_split(void)
{
    int failures = 0;
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) { return 1; }
    RF_CHECK("split: schema", build_schema(db));
    RF_CHECK("split: proven authority", stamp_proven_authority(db, A));

    bool built = true;
    for (int32_t h = A + 1; h <= A + 2; h++)
        built = built && put_consistent_height(db, h);

    /* A+3: both ok=1 in every log, but the two hash columns DISAGREE
     * (tag 0 vs tag 9). */
    uint8_t hv[32], hs[32];
    synth_hash(hv, A + 3, 0);
    synth_hash(hs, A + 3, 9);
    built = built
        && put_log_row(db, "validate_headers_log", "hash", A + 3, 1, hv, NULL)
        && put_log_row(db, "script_validate_log", "block_hash", A + 3, 1, hs,
                       "ok")
        && put_log_row(db, "body_persist_log", NULL, A + 3, 1, NULL, NULL)
        && put_log_row(db, "proof_validate_log", NULL, A + 3, 1, NULL, NULL)
        && put_log_row(db, "utxo_apply_log", NULL, A + 3, 1, NULL, NULL)
        && put_log_row(db, "tip_finalize_log", NULL, A + 3, 1, NULL, "ok");
    for (int32_t h = A + 4; h <= A + 5; h++)
        built = built && put_consistent_height(db, h);
    RF_CHECK("split: rows built", built);
    RF_CHECK("split: cursors", set_all_cursors(db, A + 6));

    int32_t hstar = -1, served = -1;
    bool ok = reducer_frontier_compute_hstar(db, &hstar, &served);
    RF_CHECK("split: returns true", ok);
    /* H* caps at A+2 (block before the split). If C3 were skipped hstar would
     * be A+5 and this fails. */
    RF_CHECK("split: hstar == A+2", hstar == A + 2);
    /* served_floor still reaches the deepest ok=1 finalize (A+5). */
    RF_CHECK("split: served_floor == A+5", served == A + 5);

    sqlite3_close(db);
    return failures;
}

/* (e) Hash split at the VERY FIRST height above the anchor (A+1): every log
 *     is ok=1 through A+3 so the contiguous prefix would reach A+3, but the
 *     two hashes disagree at A+1. C3 must cap H* at A (anchor) — exercising
 *     its "never below the anchor" lower clamp, h-1 == anchor here. This is
 *     the only fixture that drives H* down onto the anchor floor via C3, so a
 *     regression that drops the lower clamp is caught. */
static int case_split_at_floor(void)
{
    int failures = 0;
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) { return 1; }
    RF_CHECK("floor: schema", build_schema(db));
    RF_CHECK("floor: proven authority", stamp_proven_authority(db, A));

    /* A+1: ok=1 everywhere but hashes disagree (tag 0 vs tag 7). */
    uint8_t hv[32], hs[32];
    synth_hash(hv, A + 1, 0);
    synth_hash(hs, A + 1, 7);
    bool built =
        put_log_row(db, "validate_headers_log", "hash", A + 1, 1, hv, NULL)
        && put_log_row(db, "script_validate_log", "block_hash", A + 1, 1, hs,
                       "ok")
        && put_log_row(db, "body_persist_log", NULL, A + 1, 1, NULL, NULL)
        && put_log_row(db, "proof_validate_log", NULL, A + 1, 1, NULL, NULL)
        && put_log_row(db, "utxo_apply_log", NULL, A + 1, 1, NULL, NULL)
        && put_log_row(db, "tip_finalize_log", NULL, A + 1, 1, NULL, "ok");
    for (int32_t h = A + 2; h <= A + 3; h++)
        built = built && put_consistent_height(db, h);
    RF_CHECK("floor: rows built", built);
    RF_CHECK("floor: cursors", set_all_cursors(db, A + 4));

    int32_t hstar = -1, served = -1;
    bool ok = reducer_frontier_compute_hstar(db, &hstar, &served);
    RF_CHECK("floor: returns true", ok);
    /* The split is at the first height above the anchor; H* must clamp to the
     * anchor itself, never A (=A+1-1) which already equals the anchor — and
     * NEVER below it. */
    RF_CHECK("floor: hstar == anchor", hstar == A);
    RF_CHECK("floor: hstar >= TRUSTED_ANCHOR", hstar >= A);

    sqlite3_close(db);
    return failures;
}

static int case_dump_reports_validate_failure_owner(void)
{
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "reducer_frontier", "dump");

    progress_store_close();
    bool opened = progress_store_open(dir);
    RF_CHECK("dump: progress_store opens", opened);
    if (!opened) {
        test_cleanup_tmpdir(dir);
        return failures;
    }

    sqlite3 *db = progress_store_db();
    RF_CHECK("dump: schema", db && build_schema(db));
    RF_CHECK("dump: proven authority", db && stamp_proven_authority(db, A));

    const int32_t fail_h = A + 3;
    bool built = put_consistent_height(db, A + 1)
              && put_consistent_height(db, A + 2)
              && put_validate_failure(db, fail_h,
                                      "header-source-hash-mismatch")
              && put_log_row(db, "script_validate_log", "block_hash",
                             fail_h, 0, NULL, "upstream_failed")
              && put_log_row(db, "body_persist_log", NULL,
                             fail_h, 1, NULL, NULL)
              && put_log_row(db, "proof_validate_log", NULL,
                             fail_h, 1, NULL, NULL)
              && put_log_row(db, "utxo_apply_log", NULL,
                             fail_h, 1, NULL, NULL);
    RF_CHECK("dump: rows built", built);
    RF_CHECK("dump: cursors", set_all_cursors(db, fail_h + 1));

    struct json_value out;
    json_init(&out);
    bool dumped = reducer_frontier_dump_state_json(&out, NULL);
    RF_CHECK("dump: returns true", dumped);
    if (dumped) {
        RF_CHECK("dump: hstar reaches block before validate failure",
                 json_get_int(json_get(&out, "hstar")) == fail_h - 1);
        RF_CHECK("dump: first validate failure found",
                 json_get_bool(json_get(&out,
                                        "first_validate_failure_found")));
        RF_CHECK("dump: first validate failure height",
                 json_get_int(json_get(&out,
                                       "first_validate_failure_height"))
                     == fail_h);
        RF_CHECK("dump: first validate failure reason",
                 strcmp(json_get_str(json_get(&out,
                           "first_validate_failure_reason")),
                        "header-source-hash-mismatch") == 0);
        RF_CHECK("dump: first validate failure owner",
                 strcmp(json_get_str(json_get(&out,
                           "first_validate_failure_repair_owner")),
                        "stale_validate_headers_repair") == 0);
        RF_CHECK("dump: hstar blocker found",
                 json_get_bool(json_get(&out,
                                        "first_hstar_blocker_found")));
        RF_CHECK("dump: hstar blocker stage",
                 strcmp(json_get_str(json_get(&out,
                           "first_hstar_blocker_stage")),
                        "validate_headers") == 0);
        RF_CHECK("dump: hstar blocker height",
                 json_get_int(json_get(&out,
                                       "first_hstar_blocker_height"))
                     == fail_h);
        RF_CHECK("dump: hstar blocker kind",
                 strcmp(json_get_str(json_get(&out,
                           "first_hstar_blocker_kind")),
                        "ok0_failure") == 0);
        RF_CHECK("dump: hstar blocker reason",
                 strcmp(json_get_str(json_get(&out,
                           "first_hstar_blocker_reason")),
                        "header-source-hash-mismatch") == 0);
        RF_CHECK("dump: hstar blocker owner",
                 strcmp(json_get_str(json_get(&out,
                           "first_hstar_blocker_repair_owner")),
                        "stale_validate_headers_repair") == 0);
        RF_CHECK("dump: hstar next height",
                 json_get_int(json_get(&out, "hstar_next_height")) == fail_h);
        RF_CHECK("dump: hstar next blocked",
                 json_get_bool(json_get(&out, "hstar_next_blocked")));
        RF_CHECK("dump: hstar next primary kind",
                 strcmp(json_get_str(json_get(&out,
                           "hstar_next_primary_kind")),
                        "ok0_failure") == 0);
        RF_CHECK("dump: hstar next primary table",
                 strcmp(json_get_str(json_get(&out,
                           "hstar_next_primary_log_table")),
                        "validate_headers_log") == 0);
    }
    json_free(&out);

    progress_store_close();
    test_cleanup_tmpdir(dir);
    return failures;
}

static int case_dump_reports_unavailable_store(void)
{
    int failures = 0;

    progress_store_close();
    struct json_value closed;
    json_init(&closed);
    bool dumped_closed = reducer_frontier_dump_state_json(&closed, NULL);
    RF_CHECK("dump-unavailable: closed store returns true", dumped_closed);
    if (dumped_closed) {
        RF_CHECK("dump-unavailable: closed store open=false",
                 !json_get_bool(json_get(&closed, "open")));
        RF_CHECK("dump-unavailable: closed store schema_ready=false",
                 !json_get_bool(json_get(&closed, "schema_ready")));
        RF_CHECK("dump-unavailable: closed store missing=progress_store",
                 strcmp(json_get_str(json_get(&closed, "schema_missing")),
                        "progress_store") == 0);
    }
    json_free(&closed);

    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "reducer_frontier", "dump_empty");
    bool opened = progress_store_open(dir);
    RF_CHECK("dump-unavailable: empty store opens", opened);
    if (opened) {
        struct json_value empty;
        json_init(&empty);
        bool dumped_empty = reducer_frontier_dump_state_json(&empty, NULL);
        RF_CHECK("dump-unavailable: empty store returns true", dumped_empty);
        if (dumped_empty) {
            RF_CHECK("dump-unavailable: empty store open=true",
                     json_get_bool(json_get(&empty, "open")));
            RF_CHECK("dump-unavailable: empty store schema_ready=false",
                     !json_get_bool(json_get(&empty, "schema_ready")));
            RF_CHECK("dump-unavailable: empty store missing=validate log",
                     strcmp(json_get_str(json_get(&empty, "schema_missing")),
                            "validate_headers_log") == 0);
        }
        json_free(&empty);
    }

    progress_store_close();
    test_cleanup_tmpdir(dir);
    return failures;
}

static int case_dump_reports_hstar_log_hole(void)
{
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "reducer_frontier", "dump_hole");

    progress_store_close();
    bool opened = progress_store_open(dir);
    RF_CHECK("dump-hole: progress_store opens", opened);
    if (!opened) {
        test_cleanup_tmpdir(dir);
        return failures;
    }

    sqlite3 *db = progress_store_db();
    RF_CHECK("dump-hole: schema", db && build_schema(db));
    RF_CHECK("dump-hole: proven authority", db && stamp_proven_authority(db, A));

    const int32_t hole_h = A + 3;
    bool built = put_consistent_height(db, A + 1)
              && put_consistent_height(db, A + 2);
    uint8_t hh[32];
    synth_hash(hh, hole_h, 0);
    built = built
        && put_log_row(db, "validate_headers_log", "hash", hole_h, 1,
                       hh, NULL)
        && put_log_row(db, "script_validate_log", "block_hash", hole_h, 1,
                       hh, "ok")
        /* body_persist_log intentionally has NO row at hole_h. */
        && put_log_row(db, "proof_validate_log", NULL, hole_h, 1, NULL, NULL)
        && put_log_row(db, "utxo_apply_log", NULL, hole_h, 1, NULL, NULL)
        && put_log_row(db, "tip_finalize_log", NULL, hole_h, 1, NULL, "ok");
    RF_CHECK("dump-hole: rows built", built);
    RF_CHECK("dump-hole: cursors", set_all_cursors(db, hole_h + 1));

    struct json_value out;
    json_init(&out);
    bool dumped = reducer_frontier_dump_state_json(&out, NULL);
    RF_CHECK("dump-hole: returns true", dumped);
    if (dumped) {
        RF_CHECK("dump-hole: hstar before hole",
                 json_get_int(json_get(&out, "hstar")) == hole_h - 1);
        RF_CHECK("dump-hole: blocker found",
                 json_get_bool(json_get(&out,
                                        "first_hstar_blocker_found")));
        RF_CHECK("dump-hole: blocker stage",
                 strcmp(json_get_str(json_get(&out,
                           "first_hstar_blocker_stage")),
                        "body_persist") == 0);
        RF_CHECK("dump-hole: blocker height",
                 json_get_int(json_get(&out,
                                       "first_hstar_blocker_height"))
                     == hole_h);
        RF_CHECK("dump-hole: blocker kind",
                 strcmp(json_get_str(json_get(&out,
                           "first_hstar_blocker_kind")),
                        "log_hole") == 0);
        RF_CHECK("dump-hole: blocker reason",
                 strcmp(json_get_str(json_get(&out,
                           "first_hstar_blocker_reason")),
                        "missing-success-row") == 0);
        /* kind=log_hole names its repair owner — repair_owner="" here is the
         * 3166989 regression (a rowless hole stalled 3 h with no named
         * owner). */
        RF_CHECK("dump-hole: blocker repair owner",
                 strcmp(json_get_str(json_get(&out,
                           "first_hstar_blocker_repair_owner")),
                        "reducer_frontier_reconcile_light") == 0);
        RF_CHECK("dump-hole: hstar next repair owner",
                 strcmp(json_get_str(json_get(&out,
                           "hstar_next_primary_repair_owner")),
                        "reducer_frontier_reconcile_light") == 0);
        RF_CHECK("dump-hole: hstar next kind",
                 strcmp(json_get_str(json_get(&out,
                           "hstar_next_primary_kind")),
                        "log_hole") == 0);
        RF_CHECK("dump-hole: hstar next table",
                 strcmp(json_get_str(json_get(&out,
                           "hstar_next_primary_log_table")),
                        "body_persist_log") == 0);
    }
    json_free(&out);

    progress_store_close();
    test_cleanup_tmpdir(dir);
    return failures;
}

static int case_dump_reports_hstar_hash_split(void)
{
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "reducer_frontier", "dump_split");

    progress_store_close();
    bool opened = progress_store_open(dir);
    RF_CHECK("dump-split: progress_store opens", opened);
    if (!opened) {
        test_cleanup_tmpdir(dir);
        return failures;
    }

    sqlite3 *db = progress_store_db();
    RF_CHECK("dump-split: schema", db && build_schema(db));
    RF_CHECK("dump-split: proven authority", db && stamp_proven_authority(db, A));

    bool built = true;
    for (int32_t h = A + 1; h <= A + 2; h++)
        built = built && put_consistent_height(db, h);

    const int32_t split_h = A + 3;
    uint8_t hv[32], hs[32];
    synth_hash(hv, split_h, 0);
    synth_hash(hs, split_h, 9);
    built = built
        && put_log_row(db, "validate_headers_log", "hash", split_h, 1,
                       hv, NULL)
        && put_log_row(db, "script_validate_log", "block_hash", split_h, 1,
                       hs, "ok")
        && put_log_row(db, "body_persist_log", NULL, split_h, 1, NULL, NULL)
        && put_log_row(db, "proof_validate_log", NULL, split_h, 1, NULL, NULL)
        && put_log_row(db, "utxo_apply_log", NULL, split_h, 1, NULL, NULL)
        && put_log_row(db, "tip_finalize_log", NULL, split_h, 1, NULL, "ok");
    RF_CHECK("dump-split: rows built", built);
    RF_CHECK("dump-split: cursors", set_all_cursors(db, split_h + 1));

    struct json_value out;
    json_init(&out);
    bool dumped = reducer_frontier_dump_state_json(&out, NULL);
    RF_CHECK("dump-split: returns true", dumped);
    if (dumped) {
        RF_CHECK("dump-split: hstar before split",
                 json_get_int(json_get(&out, "hstar")) == split_h - 1);
        RF_CHECK("dump-split: blocker found",
                 json_get_bool(json_get(&out,
                                        "first_hstar_blocker_found")));
        RF_CHECK("dump-split: blocker stage",
                 strcmp(json_get_str(json_get(&out,
                           "first_hstar_blocker_stage")),
                        "script_validate") == 0);
        RF_CHECK("dump-split: blocker height",
                 json_get_int(json_get(&out,
                                       "first_hstar_blocker_height"))
                     == split_h);
        RF_CHECK("dump-split: blocker kind",
                 strcmp(json_get_str(json_get(&out,
                           "first_hstar_blocker_kind")),
                        "hash_split") == 0);
        RF_CHECK("dump-split: blocker reason",
                 strcmp(json_get_str(json_get(&out,
                           "first_hstar_blocker_reason")),
                        "validate-script-hash-mismatch") == 0);
        RF_CHECK("dump-split: blocker repair owner",
                 strcmp(json_get_str(json_get(&out,
                           "first_hstar_blocker_repair_owner")),
                        "reducer_frontier_reconcile_light") == 0);
        RF_CHECK("dump-split: hstar next repair owner",
                 strcmp(json_get_str(json_get(&out,
                           "hstar_next_primary_repair_owner")),
                        "reducer_frontier_reconcile_light") == 0);
        RF_CHECK("dump-split: hstar next kind",
                 strcmp(json_get_str(json_get(&out,
                           "hstar_next_primary_kind")),
                        "hash_split") == 0);
        RF_CHECK("dump-split: hstar next table",
                 strcmp(json_get_str(json_get(&out,
                           "hstar_next_primary_log_table")),
                        "script_validate_log") == 0);
    }
    json_free(&out);

    progress_store_close();
    test_cleanup_tmpdir(dir);
    return failures;
}

static int case_dump_ignores_tip_finalize_pending_edge(void)
{
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "reducer_frontier", "dump_tipfin_edge");

    progress_store_close();
    bool opened = progress_store_open(dir);
    RF_CHECK("dump-tipfin-edge: progress_store opens", opened);
    if (!opened) {
        test_cleanup_tmpdir(dir);
        return failures;
    }

    sqlite3 *db = progress_store_db();
    RF_CHECK("dump-tipfin-edge: schema", db && build_schema(db));
    RF_CHECK("dump-tipfin-edge: proven authority",
             db && stamp_proven_authority(db, A + 4));

    const int32_t pending_h = A + 3;
    bool built = put_consistent_height(db, A + 1)
              && put_consistent_height(db, A + 2);
    uint8_t hh[32];
    synth_hash(hh, pending_h, 0);
    built = built
        && put_log_row(db, "validate_headers_log", "hash", pending_h, 1,
                       hh, NULL)
        && put_log_row(db, "script_validate_log", "block_hash", pending_h, 1,
                       hh, "ok")
        && put_log_row(db, "body_persist_log", NULL, pending_h, 1, NULL, NULL)
        && put_log_row(db, "proof_validate_log", NULL, pending_h, 1, NULL, NULL)
        && put_log_row(db, "utxo_apply_log", NULL, pending_h, 1, NULL, NULL);
    RF_CHECK("dump-tipfin-edge: rows built", built);
    RF_CHECK("dump-tipfin-edge: upstream cursors",
             set_cursor(db, "validate_headers", pending_h + 1) &&
             set_cursor(db, "body_fetch", pending_h + 1) &&
             set_cursor(db, "body_persist", pending_h + 1) &&
             set_cursor(db, "script_validate", pending_h + 1) &&
             set_cursor(db, "proof_validate", pending_h + 1) &&
             set_cursor(db, "utxo_apply", pending_h + 1));
    RF_CHECK("dump-tipfin-edge: tip_finalize cursor",
             set_cursor(db, "tip_finalize", pending_h));

    struct json_value out;
    json_init(&out);
    bool dumped = reducer_frontier_dump_state_json(&out, NULL);
    RF_CHECK("dump-tipfin-edge: returns true", dumped);
    if (dumped) {
        RF_CHECK("dump-tipfin-edge: hstar is finalized row frontier",
                 json_get_int(json_get(&out, "hstar")) == pending_h - 1);
        RF_CHECK("dump-tipfin-edge: next height is served tip edge",
                 json_get_int(json_get(&out, "hstar_next_height")) ==
                     pending_h);
        RF_CHECK("dump-tipfin-edge: no false blocker",
                 !json_get_bool(json_get(&out, "hstar_next_blocked")));
        RF_CHECK("dump-tipfin-edge: no false repair owner",
                 strcmp(json_get_str(json_get(&out,
                           "hstar_next_primary_repair_owner")), "") == 0);
        RF_CHECK("dump-tipfin-edge: pending edge named",
                 json_get_bool(json_get(&out,
                           "hstar_next_pending_edge")));
        RF_CHECK("dump-tipfin-edge: pending edge stage",
                 strcmp(json_get_str(json_get(&out,
                           "hstar_next_pending_stage")),
                        "tip_finalize") == 0);
        RF_CHECK("dump-tipfin-edge: pending edge table",
                 strcmp(json_get_str(json_get(&out,
                           "hstar_next_pending_log_table")),
                        "tip_finalize_log") == 0);
        RF_CHECK("dump-tipfin-edge: pending edge detail",
                 strcmp(json_get_str(json_get(&out,
                           "hstar_next_pending_detail")),
                        "tip-finalize-edge-pending") == 0);
        RF_CHECK("dump-tipfin-edge: first blocker absent",
                 !json_get_bool(json_get(&out,
                           "first_hstar_blocker_found")));
    }
    json_free(&out);

    progress_store_close();
    test_cleanup_tmpdir(dir);
    return failures;
}

static int case_dump_reports_tip_finalize_real_hole(void)
{
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "reducer_frontier",
                     "dump_tipfin_hole");

    progress_store_close();
    bool opened = progress_store_open(dir);
    RF_CHECK("dump-tipfin-hole: progress_store opens", opened);
    if (!opened) {
        test_cleanup_tmpdir(dir);
        return failures;
    }

    sqlite3 *db = progress_store_db();
    RF_CHECK("dump-tipfin-hole: schema", db && build_schema(db));
    RF_CHECK("dump-tipfin-hole: proven authority",
             db && stamp_proven_authority(db, A + 4));

    const int32_t hole_h = A + 3;
    bool built = put_consistent_height(db, A + 1)
              && put_consistent_height(db, A + 2);
    uint8_t hh[32];
    synth_hash(hh, hole_h, 0);
    built = built
        && put_log_row(db, "validate_headers_log", "hash", hole_h, 1,
                       hh, NULL)
        && put_log_row(db, "script_validate_log", "block_hash", hole_h, 1,
                       hh, "ok")
        && put_log_row(db, "body_persist_log", NULL, hole_h, 1, NULL, NULL)
        && put_log_row(db, "proof_validate_log", NULL, hole_h, 1, NULL, NULL)
        && put_log_row(db, "utxo_apply_log", NULL, hole_h, 1, NULL, NULL);
    RF_CHECK("dump-tipfin-hole: rows built", built);
    RF_CHECK("dump-tipfin-hole: upstream cursors",
             set_cursor(db, "validate_headers", hole_h + 1) &&
             set_cursor(db, "body_fetch", hole_h + 1) &&
             set_cursor(db, "body_persist", hole_h + 1) &&
             set_cursor(db, "script_validate", hole_h + 1) &&
             set_cursor(db, "proof_validate", hole_h + 1) &&
             set_cursor(db, "utxo_apply", hole_h + 1));
    RF_CHECK("dump-tipfin-hole: tip_finalize cursor beyond hole",
             set_cursor(db, "tip_finalize", hole_h + 1));

    struct json_value out;
    json_init(&out);
    bool dumped = reducer_frontier_dump_state_json(&out, NULL);
    RF_CHECK("dump-tipfin-hole: returns true", dumped);
    if (dumped) {
        RF_CHECK("dump-tipfin-hole: hstar before hole",
                 json_get_int(json_get(&out, "hstar")) == hole_h - 1);
        RF_CHECK("dump-tipfin-hole: next height is hole",
                 json_get_int(json_get(&out, "hstar_next_height")) ==
                     hole_h);
        RF_CHECK("dump-tipfin-hole: not a pending edge",
                 !json_get_bool(json_get(&out,
                           "hstar_next_pending_edge")));
        RF_CHECK("dump-tipfin-hole: blocker found",
                 json_get_bool(json_get(&out,
                           "first_hstar_blocker_found")));
        RF_CHECK("dump-tipfin-hole: blocker stage",
                 strcmp(json_get_str(json_get(&out,
                           "first_hstar_blocker_stage")),
                        "tip_finalize") == 0);
        RF_CHECK("dump-tipfin-hole: blocker table",
                 strcmp(json_get_str(json_get(&out,
                           "first_hstar_blocker_log_table")),
                        "tip_finalize_log") == 0);
        RF_CHECK("dump-tipfin-hole: blocker height",
                 json_get_int(json_get(&out,
                           "first_hstar_blocker_height")) == hole_h);
        RF_CHECK("dump-tipfin-hole: blocker kind",
                 strcmp(json_get_str(json_get(&out,
                           "first_hstar_blocker_kind")),
                        "log_hole") == 0);
        RF_CHECK("dump-tipfin-hole: blocker reason",
                 strcmp(json_get_str(json_get(&out,
                           "first_hstar_blocker_reason")),
                        "missing-success-row") == 0);
        RF_CHECK("dump-tipfin-hole: blocker repair owner",
                 strcmp(json_get_str(json_get(&out,
                           "first_hstar_blocker_repair_owner")),
                        "reducer_frontier_reconcile_light") == 0);
        RF_CHECK("dump-tipfin-hole: hstar next blocked",
                 json_get_bool(json_get(&out, "hstar_next_blocked")));
        RF_CHECK("dump-tipfin-hole: hstar next primary table",
                 strcmp(json_get_str(json_get(&out,
                           "hstar_next_primary_log_table")),
                        "tip_finalize_log") == 0);
        RF_CHECK("dump-tipfin-hole: hstar next repair owner",
                 strcmp(json_get_str(json_get(&out,
                           "hstar_next_primary_repair_owner")),
                        "reducer_frontier_reconcile_light") == 0);
    }
    json_free(&out);

    progress_store_close();
    test_cleanup_tmpdir(dir);
    return failures;
}

static int case_log_frontier_above_tip_finalize_cursor(void)
{
    int failures = 0;
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) { return 1; }
    RF_CHECK("tipfin-above: schema", build_schema(db));

    bool built = true;
    for (int32_t h = A + 1; h <= A + 3; h++)
        built = built &&
            put_log_row(db, "tip_finalize_log", NULL, h, 1, NULL, "ok");
    RF_CHECK("tipfin-above: rows built", built);
    RF_CHECK("tipfin-above: served-height cursor",
             set_cursor(db, "tip_finalize", A + 3));

    int32_t frontier = -1;
    bool ok = reducer_frontier_log_frontier_above(db, "tip_finalize_log",
                                                  "tip_finalize", A,
                                                  &frontier);
    RF_CHECK("tipfin-above: returns true", ok);
    RF_CHECK("tipfin-above: frontier includes served cursor height",
             frontier == A + 3);

    sqlite3_close(db);
    return failures;
}

/* Scan a JSON array for the first object whose "kind" field equals `kind`
 * and whose "height" field equals `height`. Returns NULL if not found. */
static const struct json_value *find_rewind_base(const struct json_value *arr,
                                                  const char *kind,
                                                  int32_t height)
{
    size_t n = arr ? json_size(arr) : 0;
    for (size_t i = 0; i < n; i++) {
        const struct json_value *item = json_at(arr, i);
        if (!item)
            continue;
        const char *k = json_get_str(json_get(item, "kind"));
        if (k && strcmp(k, kind) == 0 &&
            json_get_int(json_get(item, "height")) == height)
            return item;
    }
    return NULL;
}

/* Pillar 3 observability: the reducer_frontier dump lists every currently
 * available self-verified rewind base (the compiled checkpoint + every
 * self-valid seal_kv ring slot + the optional finalized utxo_sha3 stamp) and
 * reports the O(delta) distance from H* to the nearest one. */
static int case_dump_reports_rewind_bases(void)
{
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "reducer_frontier", "rewind_bases");

    progress_store_close();
    bool opened = progress_store_open(dir);
    RF_CHECK("rewind: progress_store opens", opened);
    if (!opened) {
        test_cleanup_tmpdir(dir);
        return failures;
    }

    sqlite3 *db = progress_store_db();
    RF_CHECK("rewind: schema", db && build_schema(db));
    RF_CHECK("rewind: proven authority", db && stamp_proven_authority(db, A));
    RF_CHECK("rewind: seal schema", seal_kv_ensure_schema(db));

    bool built = true;
    for (int32_t h = A + 1; h <= A + 5; h++)
        built = built && put_consistent_height(db, h);
    RF_CHECK("rewind: rows built", built);
    RF_CHECK("rewind: cursors", set_all_cursors(db, A + 6));

    /* One self-derived sealed candidate at A+2 — closer to the tip (A+5) than
     * the compiled checkpoint (at A). */
    struct seal_record r;
    memset(&r, 0, sizeof(r));
    r.height = A + 2;
    memset(r.coins_sha3, 0xAB, sizeof(r.coins_sha3));
    r.utxo_count = 3;
    r.supply = 12345;
    r.sealed_at = 1;
    progress_store_tx_lock();
    bool tx_ok = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL)
                     == SQLITE_OK;
    bool inserted = tx_ok && seal_kv_insert_candidate_in_tx(db, &r);
    sqlite3_exec(db, inserted ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    progress_store_tx_unlock();
    RF_CHECK("rewind: seal candidate inserted", inserted);

    struct json_value out;
    json_init(&out);
    bool dumped = reducer_frontier_dump_state_json(&out, NULL);
    RF_CHECK("rewind: dump returns true", dumped);
    if (dumped) {
        RF_CHECK("rewind: hstar reaches last consistent height",
                 json_get_int(json_get(&out, "hstar")) == A + 5);

        const struct json_value *bases = json_get(&out, "rewind_bases");
        RF_CHECK("rewind: rewind_bases is present", bases != NULL);
        RF_CHECK("rewind: rewind_bases_count >= 2",
                 json_get_int(json_get(&out, "rewind_bases_count")) >= 2);

        const struct json_value *compiled =
            find_rewind_base(bases, "compiled_checkpoint", A);
        RF_CHECK("rewind: compiled checkpoint entry present",
                 compiled != NULL);
        if (compiled) {
            RF_CHECK("rewind: compiled checkpoint self_derived",
                     json_get_bool(json_get(compiled, "self_derived")));
            RF_CHECK("rewind: compiled checkpoint distance_from_tip",
                     json_get_int(json_get(compiled, "distance_from_tip"))
                         == 5);
        }

        const struct json_value *sealed =
            find_rewind_base(bases, "sealed_coins_sha3", A + 2);
        RF_CHECK("rewind: sealed candidate entry present", sealed != NULL);
        if (sealed) {
            RF_CHECK("rewind: sealed candidate self_derived",
                     json_get_bool(json_get(sealed, "self_derived")));
            RF_CHECK("rewind: sealed candidate not yet ratified",
                     !json_get_bool(json_get(sealed, "ratified")));
            RF_CHECK("rewind: sealed candidate distance_from_tip",
                     json_get_int(json_get(sealed, "distance_from_tip"))
                         == 3);
            const char *sha3_hex = json_get_str(json_get(sealed,
                                                          "commitment_sha3"));
            RF_CHECK("rewind: sealed candidate commitment hex present",
                     sha3_hex && strlen(sha3_hex) == 64);
        }

        /* The seal candidate at A+2 is strictly closer to H*=A+5 than the
         * compiled checkpoint at A, so it MUST be the nearest — regardless
         * of whether an optional finalized_utxo_sha3 entry is also present
         * (it is best-effort / process-global and not under this test's
         * control). Bound the distance both ways instead of asserting an
         * exact kind match. */
        int64_t nearest_distance =
            json_get_int(json_get(&out, "nearest_rewind_distance"));
        RF_CHECK("rewind: nearest distance is non-negative",
                 nearest_distance >= 0);
        RF_CHECK("rewind: nearest distance at most the sealed candidate's",
                 nearest_distance <= 3);
    }
    json_free(&out);

    progress_store_close();
    test_cleanup_tmpdir(dir);
    return failures;
}

/* THE sovereignty invariant (docs/work/self-verified-tip-plan.md): a rewind
 * base is only trustworthy if THIS node self-derived it — a borrowed
 * finalized_utxo_sha3 stamp (self_derived=false, written once at snapshot
 * import, never re-derived by forward fold) must NEVER win over a genuinely
 * self-verified rung (self_derived=true — here the compiled SHA3 checkpoint
 * at the fixed anchor A), even when the borrowed stamp sits at a HIGHER
 * height (closer to H*, i.e. a smaller/cheaper rewind under naive
 * height-only selection).
 *
 * Fixture: rows [A+1..A+5] fully consistent (H*=A+5), a borrowed
 * finalized_utxo_sha3 stamped at A+4 (self_derived=false, HIGHER than the
 * compiled checkpoint at A), and NO self-verified candidate closer than the
 * compiled checkpoint (no seal_kv slot seeded). Proves BOTH halves at once:
 *   (1) reducer_frontier_nearest_self_verified_base() — the function the
 *       generic recovery driver (rewind_driver.c) actually calls — returns
 *       the LOWER self-verified height A, never the higher borrowed A+4.
 *   (2) the LEGACY height-only nearest_rewind_base_* JSON keys (unchanged,
 *       still height-first over any provenance) DO pick the higher borrowed
 *       A+4 — proving the two selectors now genuinely disagree, which is
 *       exactly the bug this fix closes. */
static int case_sovereign_base_ignores_borrowed_higher_stamp(void)
{
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "reducer_frontier", "sovereign_base");

    progress_store_close();
    bool opened = progress_store_open(dir);
    RF_CHECK("sovereign-base: progress_store opens", opened);
    if (!opened) {
        test_cleanup_tmpdir(dir);
        return failures;
    }

    sqlite3 *db = progress_store_db();
    RF_CHECK("sovereign-base: schema", db && build_schema(db));
    RF_CHECK("sovereign-base: proven authority",
             db && stamp_proven_authority(db, A));

    bool built = true;
    for (int32_t h = A + 1; h <= A + 5; h++)
        built = built && put_consistent_height(db, h);
    RF_CHECK("sovereign-base: rows built", built);
    RF_CHECK("sovereign-base: cursors", set_all_cursors(db, A + 6));

    /* Wire a real node_db + db_service into app_runtime so enumerate_rewind_
     * bases()'s app_runtime_node_db() resolves to a live handle (same seam
     * test_dbquery_secret_denylist.c proves against: node_db_open ->
     * db_service_init/attach/start -> app_runtime_set_current) and stamp a
     * BORROWED finalized_utxo_sha3 at A+4 — strictly HIGHER than the
     * self-verified compiled checkpoint at A. */
    struct node_db ndb;
    struct db_service dbsvc;
    struct app_runtime_context runtime;
    memset(&ndb, 0, sizeof(ndb));
    memset(&dbsvc, 0, sizeof(dbsvc));
    memset(&runtime, 0, sizeof(runtime));
    bool ndb_ok = node_db_open(&ndb, ":memory:");
    RF_CHECK("sovereign-base: node_db opens", ndb_ok);
    db_service_init(&dbsvc);
    RF_CHECK("sovereign-base: db_service attaches",
             ndb_ok && db_service_attach(&dbsvc, &ndb));
    RF_CHECK("sovereign-base: db_service starts",
             ndb_ok && db_service_start(&dbsvc));
    runtime.db_service = &dbsvc;
    app_runtime_set_current(&runtime);

    uint8_t borrowed_hash[32];
    memset(borrowed_hash, 0xCD, sizeof(borrowed_hash));
    RF_CHECK("sovereign-base: borrowed utxo_sha3 stamp saved",
             utxo_commitment_sha3_save(ndb.db, borrowed_hash, A + 4, 7));

    /* (1) The driver-facing selector: must return the LOWER self-verified
     * compiled checkpoint (A), never the higher borrowed stamp (A+4). */
    struct reducer_frontier_rewind_base base;
    memset(&base, 0, sizeof(base));
    bool found = reducer_frontier_nearest_self_verified_base(A + 5, &base);
    RF_CHECK("sovereign-base: selector finds a base", found);
    if (found) {
        RF_CHECK("sovereign-base: selector picks the LOWER self-verified "
                 "height, not the higher borrowed one",
                 base.height == A);
        RF_CHECK("sovereign-base: selector's pick is self_derived",
                 base.self_derived);
        RF_CHECK("sovereign-base: selector's pick is compiled_checkpoint",
                 strcmp(base.kind, "compiled_checkpoint") == 0);
        RF_CHECK("sovereign-base: selector never returns the borrowed height",
                 base.height != A + 4);
    }

    /* (2) The legacy height-only JSON keys (nearest_rewind_base_*, unchanged
     * "nearest by height, any provenance" semantics): they DO pick the
     * higher borrowed stamp — proving the fix is a genuine behavior change,
     * not a no-op relabeling. */
    struct json_value out;
    json_init(&out);
    bool dumped = reducer_frontier_dump_state_json(&out, NULL);
    RF_CHECK("sovereign-base: dump returns true", dumped);
    if (dumped) {
        RF_CHECK("sovereign-base: hstar reaches the built tip",
                 json_get_int(json_get(&out, "hstar")) == A + 5);
        RF_CHECK("sovereign-base: legacy nearest picks the HIGHER borrowed "
                 "height",
                 json_get_int(json_get(&out, "nearest_rewind_base_height"))
                     == A + 4);
        RF_CHECK("sovereign-base: legacy nearest is NOT self_derived (it is "
                 "the borrowed stamp)",
                 !json_get_bool(json_get(&out,
                           "nearest_rewind_base_self_derived")));
        RF_CHECK("sovereign-base: new self-verified key picks the LOWER "
                 "compiled checkpoint",
                 json_get_int(json_get(&out,
                           "nearest_self_verified_base_height")) == A);
        RF_CHECK("sovereign-base: new self-verified key names "
                 "compiled_checkpoint",
                 strcmp(json_get_str(json_get(&out,
                           "nearest_self_verified_base_kind")),
                        "compiled_checkpoint") == 0);
        /* The two keys now genuinely disagree — the exact bug this fix
         * closes (a height-first selector would have rewound to the
         * borrowed A+4 instead of the sovereign A). */
        RF_CHECK("sovereign-base: legacy and self-verified nearest heights "
                 "now DIFFER (the fix)",
                 json_get_int(json_get(&out, "nearest_rewind_base_height"))
                     != json_get_int(json_get(&out,
                           "nearest_self_verified_base_height")));
    }
    json_free(&out);

    app_runtime_set_current(NULL);
    db_service_stop(&dbsvc);
    node_db_close(&ndb);

    progress_store_close();
    test_cleanup_tmpdir(dir);
    return failures;
}

/* Lane E3: the body torn-read repair note + quarantine + typed blocker that
 * stage_repair_read_active_block_checked records when a HAVE_DATA body cannot
 * be read (torn bytes / wrong block). Proves the note/quarantine/blocker logic
 * and the clear-on-successful-read (revalidation) path in isolation; the
 * end-to-end HAVE_DATA-drop + refetch chain is proven in
 * test_have_data_unreadable.c. */
static int case_body_read_repair_note(void)
{
    int failures = 0;
    blocker_module_init();
    blocker_reset_for_testing();
    blocker_set_rate_limit_ms_for_testing(0); /* re-sets always replace */
    reducer_frontier_body_read_note_reset_for_testing();

    RF_CHECK("body_read_note: inactive at start",
             !reducer_frontier_body_read_note_active() &&
             reducer_frontier_body_read_note_height() == -1);

    /* Below the quarantine bound: note recorded, no blocker yet. */
    reducer_frontier_body_read_note_record(
        3143721, 49, 129998574, REDUCER_FRONTIER_BODY_READ_DISK);
    reducer_frontier_body_read_note_record(
        3143721, 49, 129998574, REDUCER_FRONTIER_BODY_READ_DISK);
    RF_CHECK("body_read_note: height/file/pos exposed",
             reducer_frontier_body_read_note_active() &&
             reducer_frontier_body_read_note_height() == 3143721 &&
             reducer_frontier_body_read_note_file() == 49 &&
             reducer_frontier_body_read_note_pos() == 129998574);
    RF_CHECK("body_read_note: count == 2 (below bound)",
             reducer_frontier_body_read_note_count_for_testing() == 2);
    RF_CHECK("body_read_note: no blocker below the quarantine bound",
             !blocker_exists("reducer_frontier.body_read_torn"));

    /* Crossing REDUCER_FRONTIER_BODY_READ_QUARANTINE_MAX raises ONE typed
     * TRANSIENT blocker naming height/nFile/reason — a NAMED blocker, never a
     * silent defer. */
    reducer_frontier_body_read_note_record(
        3143721, 49, 129998574, REDUCER_FRONTIER_BODY_READ_DISK);
    RF_CHECK("body_read_note: quarantine raises typed blocker",
             blocker_exists("reducer_frontier.body_read_torn"));
    {
        struct blocker_snapshot snaps[16];
        int n = blocker_snapshot_all(snaps, 16);
        bool named = false;
        for (int i = 0; i < n; i++) {
            if (strcmp(snaps[i].id, "reducer_frontier.body_read_torn") != 0)
                continue;
            named = snaps[i].class == BLOCKER_TRANSIENT &&
                    strstr(snaps[i].reason, "height=3143721") != NULL &&
                    strstr(snaps[i].reason, "nFile=49") != NULL &&
                    strstr(snaps[i].reason, "disk_read_failed") != NULL;
        }
        RF_CHECK("body_read_note: blocker names height+file+reason, TRANSIENT",
                 named);
    }

    /* Revalidation flow: a successful read of the noted height retires the
     * note AND its blocker (this is exactly the clear_at() call the
     * read_active_block_checked success path makes). */
    reducer_frontier_body_read_note_clear_at(3143721);
    RF_CHECK("body_read_note: matching clear retires note + blocker",
             !reducer_frontier_body_read_note_active() &&
             !blocker_exists("reducer_frontier.body_read_torn"));

    /* Lowest-height-first: a LOWER failing height supersedes and restarts the
     * count (it must heal first so the frontier climbs); a HIGHER failing
     * height never displaces a lower pending one. */
    reducer_frontier_body_read_note_record(
        3100000, 7, 42, REDUCER_FRONTIER_BODY_READ_WRONG);
    RF_CHECK("body_read_note: fresh lower note counts from 1",
             reducer_frontier_body_read_note_height() == 3100000 &&
             reducer_frontier_body_read_note_count_for_testing() == 1);
    reducer_frontier_body_read_note_record(
        3300000, 9, 9, REDUCER_FRONTIER_BODY_READ_DISK);
    RF_CHECK("body_read_note: higher height does not displace the lower one",
             reducer_frontier_body_read_note_height() == 3100000 &&
             reducer_frontier_body_read_note_count_for_testing() == 1);

    /* clear_at only fires for the currently-noted height. */
    reducer_frontier_body_read_note_clear_at(3300000);
    RF_CHECK("body_read_note: clear_at(non-noted height) is a no-op",
             reducer_frontier_body_read_note_height() == 3100000);

    reducer_frontier_body_read_note_reset_for_testing();
    blocker_reset_for_testing();
    blocker_set_rate_limit_ms_for_testing(BLOCKER_DEFAULT_RATE_LIMIT_MS);
    return failures;
}

int test_reducer_frontier(void)
{
    int failures = 0;
    printf("\n--- reducer_frontier (L0 H* authority) ---\n");
    failures += case_body_read_repair_note();
    failures += case_consistent();
    failures += case_proof_and_utxo_fork_split();
    failures += case_validation_evidence_contained();
    failures += case_torn();
    failures += case_sparse_seed_anchor();
    failures += case_durable_base_accepts_rowless_first();
    failures += case_durable_base_survives_header_failure();
    failures += case_anchor_collapse_after_forward_ok0();
    failures += case_clamp_up();
    failures += case_hash_split();
    failures += case_split_at_floor();
    failures += case_dump_reports_validate_failure_owner();
    failures += case_dump_reports_unavailable_store();
    failures += case_dump_reports_hstar_log_hole();
    failures += case_dump_reports_hstar_hash_split();
    failures += case_dump_ignores_tip_finalize_pending_edge();
    failures += case_dump_reports_tip_finalize_real_hole();
    failures += case_log_frontier_above_tip_finalize_cursor();
    failures += case_dump_reports_rewind_bases();
    failures += case_sovereign_base_ignores_borrowed_higher_stamp();
    if (failures == 0)
        printf("reducer_frontier: all cases passed\n");
    else
        printf("reducer_frontier: %d failure(s)\n", failures);
    return failures;
}
