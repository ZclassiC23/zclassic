/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the OBSERVE-ONLY golden UTXO-root ladder tripwire
 * (app/jobs/src/utxo_root_ladder_tripwire.c). Mirrors
 * test_sha3_windows.c's structure for its sibling tripwire: SKIP/silent on
 * a healthy verdict, FIRES a typed PERMANENT blocker naming the divergent
 * height on a mismatch, respects the operator kill-switch, and — the hard
 * parity guard — never moves chain_linkage_hold's accept/reject gate either
 * way (see test_sha3_windows.c:173-207 for the sibling proof this mirrors).
 *
 * The tripwire's header is internal to app/jobs/src (not on the test
 * include path by design — same convention as test_sha3_windows.c), so
 * mirror the enum + declare the entry points directly. Kept in lockstep
 * with utxo_root_ladder_tripwire.h. */

#include "test/test_helpers.h"

#include "chain/utxo_root_ladder.h"
#include "jobs/reducer_frontier.h"
#include "models/utxo_root_ladder_verify.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "validation/chain_linkage_check.h"

#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum utxo_root_ladder_tripwire_result {
    UTXO_ROOT_LADDER_TRIPWIRE_HEALTHY  = 0,
    UTXO_ROOT_LADDER_TRIPWIRE_MISMATCH = 1,
};
extern enum utxo_root_ladder_tripwire_result
utxo_root_ladder_tripwire_report(struct sqlite3 *db,
                                 const struct utxo_root_ladder_verify_result *results,
                                 size_t n);
extern void utxo_root_ladder_tripwire_at_boundary(int height);

/* Internal seams reused by the fail-closed cap sections (redeclared here, same
 * convention as the tripwire entry points above). */
void reducer_frontier_test_set_compiled_anchor(int32_t height);
extern bool utxo_apply_log_ensure_schema(struct sqlite3 *db);
extern bool utxo_apply_log_insert(struct sqlite3 *db, int height,
                                  const char *status, bool ok,
                                  size_t spent_count, size_t added_count,
                                  int64_t total_value_delta,
                                  const char *failure_kind,
                                  const uint8_t failure_detail[36]);

#define TRIPWIRE_BLOCKER_ID "utxo_ladder_mismatch"

/* Create the minimal reducer tables the cap proof reads (tip_finalize_log for
 * the trusted-anchor scan, stage_cursor for the utxo_apply cursor) plus the
 * production utxo_apply_log schema. Returns false on any SQLite error. */
static bool tw_build_cap_schema(sqlite3 *db)
{
    static const char *const ddl =
        "CREATE TABLE IF NOT EXISTS stage_cursor ("
        "  name TEXT PRIMARY KEY, cursor INTEGER NOT NULL, updated_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS tip_finalize_log ("
        "  height INTEGER PRIMARY KEY, status TEXT, ok INTEGER NOT NULL, tip_hash BLOB);";
    return sqlite3_exec(db, ddl, NULL, NULL, NULL) == SQLITE_OK &&
           utxo_apply_log_ensure_schema(db);
}

static bool tw_set_cursor(sqlite3 *db, const char *name, int64_t cursor)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO stage_cursor(name,cursor,updated_at) VALUES(?,?,0) "
            "ON CONFLICT(name) DO UPDATE SET cursor=excluded.cursor",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, cursor);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

/* Read a utxo_apply_log row's ok flag. *found=false when no row at `h`. */
static bool tw_utxo_apply_ok_at(sqlite3 *db, int32_t h, int *ok_out, bool *found)
{
    *found = false; *ok_out = -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT ok FROM utxo_apply_log WHERE height=?",
                           -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, h);
    if (sqlite3_step(st) == SQLITE_ROW) {
        *found = true;
        *ok_out = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return true;
}

/* Seed utxo_apply_log with ok=1 "verified" rows for [anchor+1 .. rh+1] and set
 * the utxo_apply cursor to rh+2, so WITHOUT a cap the contiguous prefix is rh+1.
 * A later ok=0 verdict planted at rh caps that prefix to rh-1. */
static bool tw_seed_applied_column(sqlite3 *db, int32_t anchor, int32_t rh)
{
    for (int32_t h = anchor + 1; h <= rh + 1; h++)
        if (!utxo_apply_log_insert(db, h, "verified", true, 0, 0, 0, NULL, NULL))
            return false;
    return tw_set_cursor(db, "utxo_apply", (int64_t)rh + 2);
}

/* End-to-end proof that the fail-closed default caps H* (via the untouched
 * reducer_frontier fold) below the first divergent rung and refuses fold above
 * it, that the observe-only hatch does not, and that the happy path is
 * unchanged. Runs each case in its own datadir; restores the production floor
 * on exit. Returns the failure count. */
static int tw_run_cap_sections(void)
{
    int failures = 0;
    if (g_utxo_root_ladder_count == 0) {
        printf("utxo_root_ladder_tripwire: cap sections SKIPPED (no compiled "
              "ladder rungs)\n");
        return 0;
    }
    const struct utxo_root_ladder_entry *rung = &g_utxo_root_ladder[0];
    const int32_t rh = rung->height;
    const int32_t anchor = rh - 5;   /* below the rung so the cap is observable */

    /* Lower the fixture finality floor below the rung: at the real compiled
     * floor the rung IS the floor and H* cannot cap below it. */
    reducer_frontier_test_set_compiled_anchor(anchor);

    /* ── Section A: fail-closed (default) caps H* below the divergent rung. ── */
    printf("utxo_root_ladder_tripwire: fail-closed default caps H* below the "
          "divergent rung + sets a DEPENDENCY blocker... ");
    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "utxo_ladder_cap_failclose", "main");
        bool ok = progress_store_open(dir);
        sqlite3 *db = ok ? progress_store_db() : NULL;
        ok = ok && db && coins_kv_ensure_schema(db) && tw_build_cap_schema(db) &&
             tw_seed_applied_column(db, anchor, rh);

        int32_t base_h = -1;   /* baseline: contiguous ok=1 through rh+1 */
        ok = ok && reducer_frontier_log_frontier(db, "utxo_apply_log",
                                                 "utxo_apply", &base_h) &&
             base_h == rh + 1;

        uint8_t corrupted[32];
        memcpy(corrupted, rung->utxo_root, 32);
        corrupted[0] ^= 0xff;
        ok = ok && coins_kv_boundary_root_set(db, rh, corrupted);

        blocker_clear(TRIPWIRE_BLOCKER_ID);
        unsetenv("ZCL_UTXO_LADDER_OBSERVE_ONLY");
        utxo_root_ladder_tripwire_at_boundary(0);   /* 0 % 100 == 0 */

        bool blk = blocker_exists(TRIPWIRE_BLOCKER_ID) &&
                   blocker_class_for(TRIPWIRE_BLOCKER_ID) == BLOCKER_DEPENDENCY;
        int row_ok = -1; bool found = false;
        bool cap_row = tw_utxo_apply_ok_at(db, rh, &row_ok, &found) &&
                       found && row_ok == 0;
        int32_t capped_h = -1;
        bool capped = reducer_frontier_log_frontier(db, "utxo_apply_log",
                                                   "utxo_apply", &capped_h) &&
                      capped_h == rh - 1;

        if (ok && blk && cap_row && capped) printf("OK\n");
        else { printf("FAIL (ok=%d blk=%d cap_row=%d capped=%d base=%d capped_h=%d)\n",
                      (int)ok, (int)blk, (int)cap_row, (int)capped,
                      base_h, capped_h); failures++; }

        blocker_clear(TRIPWIRE_BLOCKER_ID);
        progress_store_close();
        test_rm_rf_recursive(dir);
    }

    /* ── Section B: the observe-only escape hatch does NOT cap H*. ── */
    printf("utxo_root_ladder_tripwire: ZCL_UTXO_LADDER_OBSERVE_ONLY keeps a "
          "PERMANENT blocker and does NOT cap H*... ");
    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "utxo_ladder_cap_observe", "main");
        bool ok = progress_store_open(dir);
        sqlite3 *db = ok ? progress_store_db() : NULL;
        ok = ok && db && coins_kv_ensure_schema(db) && tw_build_cap_schema(db) &&
             tw_seed_applied_column(db, anchor, rh);

        uint8_t corrupted[32];
        memcpy(corrupted, rung->utxo_root, 32);
        corrupted[0] ^= 0xff;
        ok = ok && coins_kv_boundary_root_set(db, rh, corrupted);

        blocker_clear(TRIPWIRE_BLOCKER_ID);
        setenv("ZCL_UTXO_LADDER_OBSERVE_ONLY", "1", 1);
        utxo_root_ladder_tripwire_at_boundary(0);
        unsetenv("ZCL_UTXO_LADDER_OBSERVE_ONLY");

        bool blk = blocker_exists(TRIPWIRE_BLOCKER_ID) &&
                   blocker_class_for(TRIPWIRE_BLOCKER_ID) == BLOCKER_PERMANENT;
        int row_ok = -1; bool found = false;
        bool row_untouched = tw_utxo_apply_ok_at(db, rh, &row_ok, &found) &&
                             found && row_ok == 1;   /* seeded ok=1, no cap */
        int32_t h = -1;
        bool uncapped = reducer_frontier_log_frontier(db, "utxo_apply_log",
                                                     "utxo_apply", &h) &&
                        h == rh + 1;

        if (ok && blk && row_untouched && uncapped) printf("OK\n");
        else { printf("FAIL (ok=%d blk=%d row_untouched=%d uncapped=%d h=%d)\n",
                      (int)ok, (int)blk, (int)row_untouched, (int)uncapped, h);
               failures++; }

        blocker_clear(TRIPWIRE_BLOCKER_ID);
        progress_store_close();
        test_rm_rf_recursive(dir);
    }

    /* ── Section C: happy path — a MATCHING rung sets no blocker, no cap. ── */
    printf("utxo_root_ladder_tripwire: happy path (matching rung) sets no "
          "blocker and does NOT cap H*... ");
    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "utxo_ladder_cap_happy", "main");
        bool ok = progress_store_open(dir);
        sqlite3 *db = ok ? progress_store_db() : NULL;
        ok = ok && db && coins_kv_ensure_schema(db) && tw_build_cap_schema(db) &&
             tw_seed_applied_column(db, anchor, rh) &&
             coins_kv_boundary_root_set(db, rh, rung->utxo_root);  /* CORRECT root */

        blocker_clear(TRIPWIRE_BLOCKER_ID);
        unsetenv("ZCL_UTXO_LADDER_OBSERVE_ONLY");
        utxo_root_ladder_tripwire_at_boundary(0);

        bool no_blk = !blocker_exists(TRIPWIRE_BLOCKER_ID);
        int32_t h = -1;
        bool uncapped = reducer_frontier_log_frontier(db, "utxo_apply_log",
                                                     "utxo_apply", &h) &&
                        h == rh + 1;

        if (ok && no_blk && uncapped) printf("OK\n");
        else { printf("FAIL (ok=%d no_blk=%d uncapped=%d h=%d)\n",
                      (int)ok, (int)no_blk, (int)uncapped, h); failures++; }

        blocker_clear(TRIPWIRE_BLOCKER_ID);
        progress_store_close();
        test_rm_rf_recursive(dir);
    }

    reducer_frontier_test_set_compiled_anchor(-1);   /* restore production floor */
    return failures;
}

int test_utxo_root_ladder_tripwire(void)
{
    int failures = 0;

    printf("\n=== test_utxo_root_ladder_tripwire ===\n");

    /* report() on an empty/NULL result set: HEALTHY, no blocker. */
    printf("utxo_root_ladder_tripwire: report() is silent on an empty result "
          "set... ");
    {
        blocker_clear(TRIPWIRE_BLOCKER_ID);
        enum utxo_root_ladder_tripwire_result rc =
            utxo_root_ladder_tripwire_report(NULL, NULL, 0);
        if (rc == UTXO_ROOT_LADDER_TRIPWIRE_HEALTHY &&
            !blocker_exists(TRIPWIRE_BLOCKER_ID)) printf("OK\n");
        else { printf("FAIL (rc=%d)\n", (int)rc); failures++; }
    }

    /* Every rung MATCH or NOT_YET_REACHED: SILENT — normal daily operation
     * for most nodes most of the time. */
    printf("utxo_root_ladder_tripwire: report() is SILENT when healthy "
          "(MATCH/NOT_YET_REACHED only)... ");
    {
        blocker_clear(TRIPWIRE_BLOCKER_ID);
        struct utxo_root_ladder_verify_result results[2] = {
            { 100000, UTXO_ROOT_LADDER_VERIFY_MATCH },
            { 200000, UTXO_ROOT_LADDER_VERIFY_NOT_YET_REACHED },
        };
        enum utxo_root_ladder_tripwire_result rc =
            utxo_root_ladder_tripwire_report(NULL, results, 2);
        if (rc == UTXO_ROOT_LADDER_TRIPWIRE_HEALTHY &&
            !blocker_exists(TRIPWIRE_BLOCKER_ID)) printf("OK\n");
        else { printf("FAIL (rc=%d blocker=%d)\n", (int)rc,
                      (int)blocker_exists(TRIPWIRE_BLOCKER_ID)); failures++; }
    }

    /* A synthetic DIVERGENT rung FIRES: by DEFAULT (fail-closed) an ESCALATING
     * BLOCKER_DEPENDENCY, and the reason names the exact divergent height. (db
     * NULL here: this exercises the blocker/verdict logic only; the H* cap is
     * proven end-to-end in the cap sections below.) */
    printf("utxo_root_ladder_tripwire: report() FIRES on a synthetic "
          "divergent boundary-root (fail-closed DEPENDENCY, names the height)... ");
    {
        blocker_clear(TRIPWIRE_BLOCKER_ID);
        struct utxo_root_ladder_verify_result results[2] = {
            { 100000, UTXO_ROOT_LADDER_VERIFY_MATCH },
            { 3056758, UTXO_ROOT_LADDER_VERIFY_DIVERGENT },
        };
        enum utxo_root_ladder_tripwire_result rc =
            utxo_root_ladder_tripwire_report(NULL, results, 2);
        int cls = blocker_class_for(TRIPWIRE_BLOCKER_ID);
        if (rc == UTXO_ROOT_LADDER_TRIPWIRE_MISMATCH &&
            blocker_exists(TRIPWIRE_BLOCKER_ID) &&
            cls == BLOCKER_DEPENDENCY) printf("OK\n");
        else { printf("FAIL (rc=%d blocker=%d class=%d)\n", (int)rc,
                      (int)blocker_exists(TRIPWIRE_BLOCKER_ID), cls);
               failures++; }
    }
    blocker_clear(TRIPWIRE_BLOCKER_ID);

    /* at_boundary(): the kill switch fully suppresses the check, even at a
     * real %100 boundary height against a genuinely corrupted store. */
    printf("utxo_root_ladder_tripwire: at_boundary() respects "
          "ZCL_DISABLE_UTXO_LADDER_TRIPWIRE... ");
    if (g_utxo_root_ladder_count > 0) {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "utxo_ladder_tripwire_kill", "main");
        ASSERT(progress_store_open(dir));
        sqlite3 *db = progress_store_db();
        ASSERT(db != NULL);
        ASSERT(coins_kv_ensure_schema(db));

        const struct utxo_root_ladder_entry *rung = &g_utxo_root_ladder[0];
        uint8_t corrupted[32];
        memcpy(corrupted, rung->utxo_root, 32);
        corrupted[0] ^= 0xff;
        ASSERT(coins_kv_boundary_root_set(db, rung->height, corrupted));

        blocker_clear(TRIPWIRE_BLOCKER_ID);
        setenv("ZCL_DISABLE_UTXO_LADDER_TRIPWIRE", "1", 1);
        utxo_root_ladder_tripwire_at_boundary(0);   /* 0 % 100 == 0 */
        unsetenv("ZCL_DISABLE_UTXO_LADDER_TRIPWIRE");
        bool ok = !blocker_exists(TRIPWIRE_BLOCKER_ID);

        progress_store_close();
        test_rm_rf_recursive(dir);
        if (ok) printf("OK\n");
        else { printf("FAIL (blocker fired despite kill switch)\n"); failures++; }
    } else {
        printf("OK (no compiled ladder rungs to corrupt)\n");
    }

    /* at_boundary(): with the kill switch OFF, a genuinely corrupted store
     * FIRES for real (end-to-end path, not just report()'s unit API). */
    printf("utxo_root_ladder_tripwire: at_boundary() FIRES against a "
          "corrupted store (kill switch off)... ");
    if (g_utxo_root_ladder_count > 0) {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "utxo_ladder_tripwire_fire", "main");
        ASSERT(progress_store_open(dir));
        sqlite3 *db = progress_store_db();
        ASSERT(db != NULL);
        ASSERT(coins_kv_ensure_schema(db));

        const struct utxo_root_ladder_entry *rung = &g_utxo_root_ladder[0];
        uint8_t corrupted[32];
        memcpy(corrupted, rung->utxo_root, 32);
        corrupted[0] ^= 0xff;
        ASSERT(coins_kv_boundary_root_set(db, rung->height, corrupted));

        blocker_clear(TRIPWIRE_BLOCKER_ID);
        utxo_root_ladder_tripwire_at_boundary(0);
        bool fired = blocker_exists(TRIPWIRE_BLOCKER_ID);

        progress_store_close();
        test_rm_rf_recursive(dir);
        if (fired) printf("OK\n");
        else { printf("FAIL (tripwire stayed silent against a corrupted "
                      "store)\n"); failures++; }
    } else {
        printf("OK (no compiled ladder rungs to corrupt)\n");
    }

    /* at_boundary(): off-boundary heights are a pure no-op — no db touch,
     * no evidence — even against the same corrupted store. */
    printf("utxo_root_ladder_tripwire: at_boundary() SKIPs an off-boundary "
          "height... ");
    if (g_utxo_root_ladder_count > 0) {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "utxo_ladder_tripwire_offb", "main");
        ASSERT(progress_store_open(dir));
        sqlite3 *db = progress_store_db();
        ASSERT(db != NULL);
        ASSERT(coins_kv_ensure_schema(db));

        const struct utxo_root_ladder_entry *rung = &g_utxo_root_ladder[0];
        uint8_t corrupted[32];
        memcpy(corrupted, rung->utxo_root, 32);
        corrupted[0] ^= 0xff;
        ASSERT(coins_kv_boundary_root_set(db, rung->height, corrupted));

        blocker_clear(TRIPWIRE_BLOCKER_ID);
        utxo_root_ladder_tripwire_at_boundary(101);   /* 101 % 100 != 0 */
        bool ok = !blocker_exists(TRIPWIRE_BLOCKER_ID);

        progress_store_close();
        test_rm_rf_recursive(dir);
        if (ok) printf("OK\n");
        else { printf("FAIL (fired on an off-boundary height)\n"); failures++; }
    } else {
        printf("OK (no compiled ladder rungs)\n");
    }

    /* HARD PARITY GUARD: the fail-close is enacted through the provable-tip
     * fold (an ok=0 utxo_apply_log verdict), NEVER through the chain_linkage
     * accept/reject latch — the latch that would refuse a tip move. Assert it
     * is byte-identical (no HOLD, refuse_from == -1) whether report() saw a
     * healthy or a divergent verdict, so consensus accept/reject stays
     * bit-identical to zclassicd — mirrors test_sha3_windows.c:173-207's proof
     * for the sibling tripwire. (db NULL: no cap write; the blocker still fires
     * to prove it is the ONLY consensus-visible side effect here — a plain
     * blocker, not a linkage HOLD.) */
    printf("utxo_root_ladder_tripwire: does NOT touch the chain_linkage "
          "accept/reject latch (consensus parity)... ");
    {
        bool ok = true;
        bool hold_before = chain_linkage_hold_active();
        int  refuse_before = chain_linkage_hold_refuse_from();

        struct utxo_root_ladder_verify_result healthy[1] = {
            { 100000, UTXO_ROOT_LADDER_VERIFY_MATCH },
        };
        blocker_clear(TRIPWIRE_BLOCKER_ID);
        (void)utxo_root_ladder_tripwire_report(NULL, healthy, 1);
        ok = ok && (chain_linkage_hold_active() == hold_before);
        ok = ok && (chain_linkage_hold_refuse_from() == refuse_before);

        struct utxo_root_ladder_verify_result divergent[1] = {
            { 100000, UTXO_ROOT_LADDER_VERIFY_DIVERGENT },
        };
        blocker_clear(TRIPWIRE_BLOCKER_ID);
        (void)utxo_root_ladder_tripwire_report(NULL, divergent, 1);
        ok = ok && (chain_linkage_hold_active() == hold_before);
        ok = ok && (chain_linkage_hold_refuse_from() == refuse_before);
        /* Evidence exists, but it is a plain blocker — NOT a linkage HOLD. */
        ok = ok && blocker_exists(TRIPWIRE_BLOCKER_ID);

        if (ok) printf("OK\n");
        else { printf("FAIL (hold_before=%d refuse_before=%d hold_now=%d "
                      "refuse_now=%d)\n",
                      (int)hold_before, refuse_before,
                      (int)chain_linkage_hold_active(),
                      chain_linkage_hold_refuse_from()); failures++; }
    }

    /* Fail-closed cap: prove the default stops advance (caps H*), the
     * observe-only hatch does not, and the happy path is unchanged. */
    failures += tw_run_cap_sections();

    /* Leave no evidence blocker behind for other groups. */
    blocker_clear(TRIPWIRE_BLOCKER_ID);

    goto _done;
_test_next:
    /* ASSERT() jumps here on a hard failure inside one of the sections
     * above (mirrors test_utxo_root_ladder.c's convention) — failures was
     * already incremented at the ASSERT site. */
    printf("(section aborted)\n");
_done:
    if (failures == 0)
        printf("=== test_utxo_root_ladder_tripwire: all cases passed ===\n");
    else
        printf("=== test_utxo_root_ladder_tripwire: %d failure(s) ===\n",
              failures);
    return failures;
}
