/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit test for boot_flight_recorder — the boot-phase timing memory:
 *   - marks buffered during a boot persist into node.db on finish();
 *   - the diagnostics dumper ('boot_timings') reports the most recent
 *     boot's per-stage ms next to the recorded median;
 *   - a stage that blows past max(5s, 4x its recorded median) raises the
 *     named blocker boot.stage_regression — but ONLY once enough history
 *     exists (>=3 samples) to trust a median, and never on the boot that
 *     first establishes it;
 *   - the durable table is pruned to the last BOOT_FLIGHT_RECORDER_MAX_BOOTS
 *     distinct boot_epochs.
 */

#include "test/test_helpers.h"

#include "config/boot_flight_recorder.h"
#include "config/db_service.h"
#include "config/runtime.h"
#include "json/json.h"
#include "models/database.h"
#include "util/blocker.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#define BFR_CHECK(name, expr) do {                                        \
    if (expr) { printf("  boot_flight_recorder: %s... OK\n", (name)); }    \
    else { printf("  boot_flight_recorder: %s... FAIL\n", (name)); failures++; } \
} while (0)

/* Fabricate one prior-boot history row directly (bypassing finish()'s
 * wall-clock boot_epoch, so a test can build precise multi-boot history
 * without depending on real time granularity between calls). */
static bool bfr_insert_history_row(sqlite3 *db, int64_t boot_epoch,
                                   const char *stage, int64_t ms)
{
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO boot_stage_timings"
            "(boot_epoch,stage,ms,ts) VALUES (?,?,?,?)",
            -1, &s, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(s, 1, boot_epoch);
    sqlite3_bind_text(s, 2, stage, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 3, ms);
    sqlite3_bind_int64(s, 4, boot_epoch);
    bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}

static int64_t bfr_distinct_epoch_count(sqlite3 *db)
{
    sqlite3_stmt *s = NULL;
    int64_t n = -1;
    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(DISTINCT boot_epoch) FROM boot_stage_timings",
            -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
        n = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return n;
}

int test_boot_flight_recorder(void);
int test_boot_flight_recorder(void)
{
    printf("\n=== boot_flight_recorder tests ===\n");
    int failures = 0;

    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "boot_flight_recorder", "main");
    char ndb_path[600];
    snprintf(ndb_path, sizeof(ndb_path), "%s/node.db", dir);

    struct node_db ndb;
    bool db_ok = node_db_open(&ndb, ndb_path);
    BFR_CHECK("node.db opens", db_ok);
    if (!db_ok) { test_cleanup_tmpdir(dir); return failures; }

    struct db_service dbsvc;
    struct app_runtime_context runtime;
    memset(&runtime, 0, sizeof(runtime));
    db_service_init(&dbsvc);
    BFR_CHECK("db service attaches", db_service_attach(&dbsvc, &ndb));
    BFR_CHECK("db service starts", db_service_start(&dbsvc));
    runtime.db_service = &dbsvc;
    app_runtime_set_current(&runtime);

    boot_flight_recorder_reset_buffer_for_testing();
    blocker_clear("boot.stage_regression");

    /* ── finish() with no marks is a logged no-op — no schema, no rows. */
    boot_flight_recorder_finish(&ndb);
    BFR_CHECK("finish() with zero marks creates no rows",
              bfr_distinct_epoch_count(ndb.db) <= 0);

    /* ── finish(NULL/closed) is a logged no-op, never a crash. */
    boot_flight_recorder_mark("throwaway", 1);
    boot_flight_recorder_finish(NULL);
    boot_flight_recorder_reset_buffer_for_testing();

    /* ── mark() + finish() persists; the dumper reflects the latest boot. */
    boot_flight_recorder_mark("phase_a", 111);
    boot_flight_recorder_mark("phase_b", 222);
    boot_flight_recorder_finish(&ndb);
    {
        struct json_value v = {0};
        json_set_object(&v);
        bool ok = boot_flight_recorder_dump_state_json(&v, NULL);
        const struct json_value *epoch = json_get(&v, "last_boot_epoch");
        const struct json_value *stages = json_get(&v, "stages");
        bool shape_ok = ok && epoch && json_get_int(epoch) > 0 &&
                        stages && json_size(stages) == 2;
        BFR_CHECK("dump_state_json reports last_boot_epoch + 2 stages", shape_ok);
        json_free(&v);
    }
    BFR_CHECK("first-ever boot for a stage never raises a regression "
              "(no history to compare against)",
              !blocker_exists("boot.stage_regression"));

    /* ── Regression check: needs >=3 PRIOR samples before it trusts a
     * median — a stage with only 1-2 history rows never fires even on a
     * wildly high ms, so a fresh datadir's first few boots can't false-fire. */
    BFR_CHECK("seed history epoch 1001", bfr_insert_history_row(ndb.db, 1001, "phase_c", 100));
    BFR_CHECK("seed history epoch 1002", bfr_insert_history_row(ndb.db, 1002, "phase_c", 100));
    boot_flight_recorder_mark("phase_c", 50000);
    boot_flight_recorder_finish(&ndb);
    BFR_CHECK("no regression with only 2 prior samples (insufficient history)",
              !blocker_exists("boot.stage_regression"));

    /* ── Third prior sample establishes a trustworthy median (100ms) ->
     * threshold = max(5000, 4*100) = 5000ms. A stage at 6000ms breaches it. */
    BFR_CHECK("seed history epoch 1003", bfr_insert_history_row(ndb.db, 1003, "phase_c", 100));
    boot_flight_recorder_mark("phase_c", 6000);
    boot_flight_recorder_finish(&ndb);
    BFR_CHECK("stage_regression blocker raised on a real breach",
              blocker_exists("boot.stage_regression"));
    {
        struct blocker_snapshot snaps[8];
        int n = blocker_snapshot_all(snaps, 8);
        bool found = false;
        for (int i = 0; i < n; i++) {
            if (strcmp(snaps[i].id, "boot.stage_regression") == 0) {
                found = strstr(snaps[i].reason, "phase_c") != NULL &&
                        strstr(snaps[i].reason, "6000") != NULL;
            }
        }
        BFR_CHECK("blocker reason names the stage + ms", found);
    }
    BFR_CHECK("boot still proceeds past a regression (fail-LOUD not fail-stop "
              "-- finish() returned, this line runs)", true);
    blocker_clear("boot.stage_regression");

    /* ── Below max(5s, 4x median): no regression. median now includes the
     * 6000ms outlier row too (persisted by the previous finish()), so seed a
     * FRESH stage name with a clean, larger history to isolate this check. */
    BFR_CHECK("seed phase_d epoch 2001", bfr_insert_history_row(ndb.db, 2001, "phase_d", 2000));
    BFR_CHECK("seed phase_d epoch 2002", bfr_insert_history_row(ndb.db, 2002, "phase_d", 2000));
    BFR_CHECK("seed phase_d epoch 2003", bfr_insert_history_row(ndb.db, 2003, "phase_d", 2000));
    /* threshold = max(5000, 4*2000=8000) = 8000; 7000 < 8000 -> no fire. */
    boot_flight_recorder_mark("phase_d", 7000);
    boot_flight_recorder_finish(&ndb);
    BFR_CHECK("no regression strictly under threshold",
              !blocker_exists("boot.stage_regression"));
    /* 9000 > 8000 -> fires. */
    boot_flight_recorder_mark("phase_d", 9000);
    boot_flight_recorder_finish(&ndb);
    BFR_CHECK("regression fires once over threshold", blocker_exists("boot.stage_regression"));
    blocker_clear("boot.stage_regression");

    /* ── Retention: prune to the last BOOT_FLIGHT_RECORDER_MAX_BOOTS distinct
     * boot_epochs. Seed well past the cap with distinct FUTURE epochs (well
     * beyond any real wall-clock value finish() would generate earlier in
     * this test), so "the 30 largest boot_epochs survive" is unambiguous
     * regardless of when this test actually runs. */
    const int64_t PRUNE_EPOCH_BASE = 9000000000LL; /* year ~2255 */
    int prune_rows = BOOT_FLIGHT_RECORDER_MAX_BOOTS + 10;
    for (int i = 0; i < prune_rows; i++) {
        BFR_CHECK("seed prune-fixture row",
                  bfr_insert_history_row(ndb.db, PRUNE_EPOCH_BASE + i,
                                         "prune_stage", 42));
    }
    boot_flight_recorder_mark("prune_stage", 43);
    boot_flight_recorder_finish(&ndb);
    int64_t epochs_after = bfr_distinct_epoch_count(ndb.db);
    BFR_CHECK("retention caps distinct boot_epochs at BOOT_FLIGHT_RECORDER_MAX_BOOTS",
              epochs_after == BOOT_FLIGHT_RECORDER_MAX_BOOTS);

    /* The MOST RECENT (largest boot_epoch) boots survive pruning, not the
     * oldest — the real finish() call's (small, real-wall-clock) epoch is
     * itself pruned away here, which is exactly the intended "keep the
     * newest N" behavior applied to this test's deliberately-future fixture
     * epochs. */
    {
        sqlite3_stmt *s = NULL;
        int64_t max_epoch = -1;
        if (sqlite3_prepare_v2(ndb.db,
                "SELECT MAX(boot_epoch) FROM boot_stage_timings",
                -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
            max_epoch = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
        BFR_CHECK("newest boot_epoch survives pruning",
                  max_epoch == PRUNE_EPOCH_BASE + prune_rows - 1);
    }

    app_runtime_set_current(NULL);
    db_service_stop(&dbsvc);
    node_db_close(&ndb);
    test_cleanup_tmpdir(dir);
    return failures;
}
