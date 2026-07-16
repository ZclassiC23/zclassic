/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Fault-injection proof for the unified recovery_coordinator: for each injected
 * corruption class the coordinator must select the CHEAPEST sufficient rung and
 * drive the fixture back to a consistent state.
 *
 *   class: stale tip_finalize cursor  -> rung 1 (cursor warm restart) REAL e2e
 *   class: corrupt sealed segment     -> rung 3 (segment refetch)     REAL e2e
 *   class: stale cursor AND corrupt seg -> rung 1 wins (cost ordering) REAL
 *   class: suspect range              -> rung 2 (range re-derive)      selection
 *   class: none of the above          -> rung 4 (name a typed blocker) REAL
 *
 * Rungs 1/3/4 are proven end-to-end against the real healers. Rung 2's real
 * remedy (stage_reducer_frontier_reconcile_light) is a heavy main_state fixture
 * covered by test_reducer_frontier_reconcile_light / test_reducer_reconcile_
 * witness; here we prove the SELECTOR reaches it (cheapest-first ordering,
 * cheaper rungs declined) via the rung-fn seam. */

#include "test/test_helpers.h"

#include "services/recovery_coordinator.h"
#include "conditions/segment_corruption.h"
#include "storage/chain_segment.h"
#include "storage/progress_store.h"
#include "util/blocker.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define RC_CHECK(name, expr) do { \
    printf("recovery_coordinator: %s... ", (name)); \
    if (expr) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* ── progress-store stale-cursor fixture (mirrors test_stage_reducer_unwedge) */

static void rc_exec(sqlite3 *db, const char *sql)
{ sqlite3_exec(db, sql, NULL, NULL, NULL); }

static int rc_cursor(sqlite3 *db, const char *name)
{
    sqlite3_stmt *st = NULL;
    int v = -999;
    if (sqlite3_prepare_v2(db, "SELECT cursor FROM stage_cursor WHERE name=?",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW)
            v = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return v;
}

/* coins_best = APPLIED; tip_finalize cursor drifted to CURSOR (> APPLIED). */
static void seed_stale_cursor(sqlite3 *db, int applied, int stale, int cursor)
{
    rc_exec(db,
        "CREATE TABLE IF NOT EXISTS stage_cursor("
        "name TEXT PRIMARY KEY, cursor INTEGER, updated_at INTEGER)");
    rc_exec(db,
        "CREATE TABLE IF NOT EXISTS tip_finalize_log("
        "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER,"
        "work_delta_high INTEGER, work_delta_low INTEGER,"
        "utxo_size_after INTEGER, reorg_depth INTEGER, finalized_at INTEGER,"
        "tip_hash BLOB)");
    char sql[256];
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO stage_cursor VALUES('tip_finalize',%d,1)", cursor);
    rc_exec(db, sql);
    for (int h = 0; h <= stale; h++) {
        snprintf(sql, sizeof(sql),
            "INSERT OR REPLACE INTO tip_finalize_log"
            "(height,status,ok,work_delta_high,work_delta_low,utxo_size_after,"
            "reorg_depth,finalized_at) VALUES(%d,'finalized',1,0,1,0,0,1)", h);
        rc_exec(db, sql);
    }
    (void)applied;
}

/* ── segment fixture (mirrors test_segment_corruption) ─────────────── */

static bool tiny_body(void *user, uint32_t h, uint8_t **bytes, size_t *len)
{
    (void)user;
    size_t n = 24;
    uint8_t *b = malloc(n); // raw-alloc-ok:test
    if (!b) return false;
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(h * 7u + i);
    *bytes = b; *len = n;
    return true;
}

/* Seal ONE segment seg-0-10 and flip a byte so index 0 verifies corrupt. */
static bool make_corrupt_segment(const char *dir, char *seg_path, size_t plen)
{
    char err[256];
    chain_segment_seal_range(dir, tiny_body, NULL, 0, 10, err, sizeof(err));
    snprintf(seg_path, plen, "%s/seg-0-10.dat", dir);
    chmod(seg_path, 0644);
    FILE *f = fopen(seg_path, "r+b");
    if (!f) return false;
    long off = 32 + 10 * 48 + 3; /* header + index(10*48) + into first body */
    fseek(f, off, SEEK_SET); int c = fgetc(f);
    fseek(f, off, SEEK_SET); fputc(c ^ 0xff, f);
    fclose(f);
    return true;
}

/* ── rung-fn spies (rung 2 selection proof) ────────────────────────── */

static int g_order;
static int g_rung1_at = -1, g_rung2_at = -1, g_rung3_at = -1;
static int g_fixture_hstar;

static bool spy_rung1_decline(struct recovery_ctx *ctx,
                              enum recovery_outcome *o)
{ (void)ctx; (void)o; g_rung1_at = ++g_order; return false; }

static bool spy_rung2_recover(struct recovery_ctx *ctx,
                              enum recovery_outcome *o)
{
    (void)ctx;
    g_rung2_at = ++g_order;
    g_fixture_hstar = 7; /* the suspect range re-derived; H* recovers */
    *o = RECOVERY_OUTCOME_RECOVERED;
    return true;
}

static bool spy_rung3_decline(struct recovery_ctx *ctx,
                              enum recovery_outcome *o)
{ (void)ctx; (void)o; g_rung3_at = ++g_order; return false; }

int test_recovery_coordinator(void);
int test_recovery_coordinator(void)
{
    printf("\n=== recovery_coordinator (fault-injection rung selection) ===\n");
    int failures = 0;
    const int APPLIED = 3, STALE = 6, CURSOR = 8;

    /* ── 1. stale cursor -> rung 1, REAL clamp, cursor recovers ──────── */
    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "recovery_rung1", "main");
        RC_CHECK("rung1: progress_store opens", progress_store_open(dir));
        sqlite3 *db = progress_store_db();
        seed_stale_cursor(db, APPLIED, STALE, CURSOR);
        recovery_coordinator_test_reset();

        struct recovery_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.db = db;
        ctx.coins_best = APPLIED; /* durably-applied coins frontier */

        enum recovery_outcome out = RECOVERY_OUTCOME_NONE;
        enum recovery_rung r = recovery_coordinator_run(&ctx, &out);
        RC_CHECK("rung1: selected cursor_warm_restart",
                 r == RECOVERY_RUNG_CURSOR_WARM_RESTART);
        RC_CHECK("rung1: outcome progressing",
                 out == RECOVERY_OUTCOME_PROGRESSING);
        RC_CHECK("rung1: tip_finalize cursor clamped to coins frontier (H* recovers)",
                 rc_cursor(db, "tip_finalize") == APPLIED);
        RC_CHECK("rung1: recorded last_rung",
                 recovery_coordinator_test_last_rung() ==
                     RECOVERY_RUNG_CURSOR_WARM_RESTART);

        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── 2. stale cursor AND corrupt segment -> rung 1 wins (cost order) ─ */
    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "recovery_costorder", "main");
        RC_CHECK("costorder: progress_store opens", progress_store_open(dir));
        sqlite3 *db = progress_store_db();
        seed_stale_cursor(db, APPLIED, STALE, CURSOR);

        char segdir[300];
        snprintf(segdir, sizeof(segdir), "%s/segments", dir);
        mkdir(segdir, 0700);
        char seg_path[400];
        RC_CHECK("costorder: corrupt segment injected",
                 make_corrupt_segment(segdir, seg_path, sizeof(seg_path)));

        recovery_coordinator_test_reset();
        struct recovery_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.db = db;
        ctx.coins_best = APPLIED;
        ctx.segments_dir = segdir; /* BOTH classes present */

        enum recovery_rung r = recovery_coordinator_run(&ctx, NULL);
        RC_CHECK("costorder: cheapest rung (cursor) chosen over segment",
                 r == RECOVERY_RUNG_CURSOR_WARM_RESTART);
        struct stat sb;
        RC_CHECK("costorder: corrupt segment left untouched (rung3 not reached)",
                 stat(seg_path, &sb) == 0);

        progress_store_close();
        test_rm_rf_recursive(dir);
    }

    /* ── 3. corrupt segment (no progress-store class) -> rung 3 REAL ──── */
    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "recovery_rung3", "seg");
        char seg_path[400];
        RC_CHECK("rung3: corrupt segment injected",
                 make_corrupt_segment(dir, seg_path, sizeof(seg_path)));

        recovery_coordinator_test_reset();
        uint32_t scan_cursor = 0;
        struct recovery_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.db = NULL;              /* skip rungs 1 & 2 */
        ctx.coins_best = -1;
        ctx.segments_dir = dir;
        ctx.segment_scan_cursor = &scan_cursor;

        enum recovery_outcome out = RECOVERY_OUTCOME_NONE;
        enum recovery_rung r = recovery_coordinator_run(&ctx, &out);
        RC_CHECK("rung3: selected segment_refetch",
                 r == RECOVERY_RUNG_SEGMENT_REFETCH);
        RC_CHECK("rung3: outcome recovered", out == RECOVERY_OUTCOME_RECOVERED);
        struct stat sb;
        RC_CHECK("rung3: corrupt segment removed (range no longer served corrupt)",
                 stat(seg_path, &sb) != 0);

        test_rm_rf_recursive(dir);
    }

    /* ── 4. suspect range -> rung 2 selected (cheapest-first ordering) ── */
    {
        g_order = 0; g_rung1_at = g_rung2_at = g_rung3_at = -1;
        g_fixture_hstar = 5;
        recovery_coordinator_test_reset();
        recovery_coordinator_test_set_rung_fn(RECOVERY_RUNG_CURSOR_WARM_RESTART,
                                              spy_rung1_decline);
        recovery_coordinator_test_set_rung_fn(RECOVERY_RUNG_REDERIVE_RANGE,
                                              spy_rung2_recover);
        recovery_coordinator_test_set_rung_fn(RECOVERY_RUNG_SEGMENT_REFETCH,
                                              spy_rung3_decline);

        struct recovery_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));
        enum recovery_outcome out = RECOVERY_OUTCOME_NONE;
        enum recovery_rung r = recovery_coordinator_run(&ctx, &out);
        RC_CHECK("rung2: selected rederive_range",
                 r == RECOVERY_RUNG_REDERIVE_RANGE);
        RC_CHECK("rung2: outcome recovered", out == RECOVERY_OUTCOME_RECOVERED);
        RC_CHECK("rung2: fixture H* recovered (5 -> 7)", g_fixture_hstar == 7);
        RC_CHECK("rung2: rung1 (cheaper) probed FIRST", g_rung1_at == 1);
        RC_CHECK("rung2: rung2 probed after rung1", g_rung2_at == 2);
        RC_CHECK("rung2: rung3 (costlier) NOT reached", g_rung3_at == -1);
        recovery_coordinator_test_reset();
    }

    /* ── 5. no applicable rung -> rung 4 names a typed blocker ────────── */
    {
        blocker_module_init();
        blocker_reset_for_testing();
        recovery_coordinator_test_reset();

        struct recovery_ctx ctx;
        memset(&ctx, 0, sizeof(ctx)); /* db/ms/segments all NULL */
        ctx.coins_best = -1;

        enum recovery_outcome out = RECOVERY_OUTCOME_NONE;
        enum recovery_rung r = recovery_coordinator_run(&ctx, &out);
        RC_CHECK("rung4: selected blocker", r == RECOVERY_RUNG_BLOCKER);
        RC_CHECK("rung4: outcome blocked", out == RECOVERY_OUTCOME_BLOCKED);
        RC_CHECK("rung4: typed blocker named (no silent halt)",
                 blocker_fire_count_for_testing(
                     "recovery_coordinator.no_applicable_rung") > 0);
        blocker_reset_for_testing();
    }

    printf("recovery_coordinator: %d failures\n", failures);
    return failures;
}
