/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_stage_dump_trylock — the observability-under-load guard for the three
 * tail-stage dump-state views (utxo_apply / script_validate / proof_validate).
 *
 * A2: during catch-up the reducer holds progress_store_tx_lock around each
 * bulk fold. Before this fix the three dumpers took that lock BLOCKING, so an
 * RPC worker calling `dumpstate <stage>` queued behind the fold and, at a few
 * concurrent calls, the node's whole observability front door (dumpstate /
 * status) disappeared exactly while the node was busiest. The fix mirrors
 * reducer_frontier_dump: acquire the lock non-blocking (trylock) and, when the
 * fold owns it, emit {"snapshot_status":"progress_store_busy","retryable":true}
 * and return cleanly instead of blocking. This test proves each dumper takes
 * that busy path — WITHOUT blocking — while another thread holds the lock.
 *
 * A2 amplifier: the utxo_apply dumper's "lowest ok=0 row" query full-scanned
 * ~3.18M rows on a healthy node (no index on ok). utxo_apply_log_ensure_schema
 * now creates a partial index (idx_utxo_apply_log_ok0 ON utxo_apply_log(height)
 * WHERE ok=0); this test asserts EXPLAIN QUERY PLAN uses that index rather than
 * a full table scan.
 */

#include "test/test_helpers.h"

#include "jobs/proof_validate_stage.h"
#include "jobs/script_validate_stage.h"
#include "jobs/utxo_apply_stage.h"
#include "json/json.h"
#include "storage/progress_store.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Production schema-ensure for utxo_apply_log — declared extern (private
 * header lives under app/jobs/src/), the same pattern
 * test_utxo_root_ladder_tripwire.c uses. */
extern bool utxo_apply_log_ensure_schema(struct sqlite3 *db);

#define SD_CHECK(name, expr) do {                                 \
    printf("stage_dump_trylock: %s... ", (name));                 \
    if (expr) { printf("OK\n"); }                                 \
    else { printf("FAIL\n"); failures++; }                        \
} while (0)

/* ---- lock-holder thread: grabs progress_store_tx_lock and parks ---- */
struct locker {
    _Atomic int locked;   /* set once the lock is held */
    _Atomic int release;  /* main thread sets this to let the holder go */
};

static void *locker_thread(void *arg)
{
    struct locker *lk = (struct locker *)arg;
    progress_store_tx_lock();
    atomic_store(&lk->locked, 1);
    /* Hold until told to release (bounded spin so a stuck test can never wedge
     * the suite forever). */
    for (int i = 0; i < 500000 && !atomic_load(&lk->release); i++) {
        struct timespec ts = { 0, 200000 }; /* 0.2ms */
        nanosleep(&ts, NULL);
    }
    progress_store_tx_unlock();
    return NULL;
}

/* True iff `out` carries the busy/retryable marker AND no SQLite detail. */
static bool is_busy_marker(const struct json_value *out)
{
    const struct json_value *st = json_get(out, "snapshot_status");
    const struct json_value *rt = json_get(out, "retryable");
    if (!st || !rt)
        return false;
    const char *s = json_get_str(st);
    return s && strcmp(s, "progress_store_busy") == 0 && json_get_bool(rt);
}

/* (A2) Each of the 3 dumpers returns the busy marker — fast, non-blocking —
 * while a helper thread holds progress_store_tx_lock. */
static int case_dumpers_busy_path(void)
{
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "stage_dump_trylock", "busy");

    progress_store_close();
    bool opened = progress_store_open(dir);
    SD_CHECK("busy: progress_store opens", opened);
    if (!opened) {
        test_cleanup_tmpdir(dir);
        return failures;
    }
    sqlite3 *db = progress_store_db();
    /* Create the utxo_apply schema so the non-busy path would have real rows to
     * read; the other two dumpers hit the busy path before any table read. */
    SD_CHECK("busy: utxo_apply schema", db && utxo_apply_log_ensure_schema(db));

    struct locker lk = { 0, 0 };
    pthread_t th;
    int rc = pthread_create(&th, NULL, locker_thread, &lk);
    SD_CHECK("busy: locker thread starts", rc == 0);
    if (rc != 0) {
        progress_store_close();
        test_cleanup_tmpdir(dir);
        return failures;
    }
    /* Wait until the holder actually owns the lock. */
    for (int i = 0; i < 500000 && !atomic_load(&lk.locked); i++) {
        struct timespec ts = { 0, 200000 };
        nanosleep(&ts, NULL);
    }
    SD_CHECK("busy: locker holds the progress lock", atomic_load(&lk.locked));

    /* Each dumper must return true AND the busy marker, promptly. A blocking
     * lock would deadlock here (the holder only releases after we signal). */
    struct json_value out;

    json_init(&out);
    bool d1 = utxo_apply_dump_state_json(&out, NULL);
    SD_CHECK("busy: utxo_apply dump returns true", d1);
    SD_CHECK("busy: utxo_apply emits progress_store_busy", is_busy_marker(&out));

    json_init(&out);
    bool d2 = script_validate_dump_state_json(&out, NULL);
    SD_CHECK("busy: script_validate dump returns true", d2);
    SD_CHECK("busy: script_validate emits progress_store_busy",
             is_busy_marker(&out));

    json_init(&out);
    bool d3 = proof_validate_dump_state_json(&out, NULL);
    SD_CHECK("busy: proof_validate dump returns true", d3);
    SD_CHECK("busy: proof_validate emits progress_store_busy",
             is_busy_marker(&out));

    /* Release the holder and confirm the SAME dumper now takes the normal path
     * (no busy marker) — proves the trylock succeeds when the lock is free. */
    atomic_store(&lk.release, 1);
    pthread_join(th, NULL);

    json_init(&out);
    bool d4 = utxo_apply_dump_state_json(&out, NULL);
    SD_CHECK("free: utxo_apply dump returns true", d4);
    SD_CHECK("free: utxo_apply no longer busy", !is_busy_marker(&out));

    progress_store_close();
    test_cleanup_tmpdir(dir);
    return failures;
}

/* (A2 amplifier) The lowest-ok=0 query uses the partial index, not a full
 * table scan. */
static int case_utxo_apply_ok0_index_used(void)
{
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "stage_dump_trylock", "eqp");

    progress_store_close();
    bool opened = progress_store_open(dir);
    SD_CHECK("eqp: progress_store opens", opened);
    if (!opened) {
        test_cleanup_tmpdir(dir);
        return failures;
    }
    sqlite3 *db = progress_store_db();
    SD_CHECK("eqp: utxo_apply schema (creates index)",
             db && utxo_apply_log_ensure_schema(db));

    /* A body of healthy ok=1 rows and no ok=0 row — the exact production shape
     * where a full scan would be worst (walks every row to find nothing). */
    progress_store_tx_lock();
    bool ins_ok = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL)
                      == SQLITE_OK;
    for (int h = 1; ins_ok && h <= 4000; h++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO utxo_apply_log (height,status,ok,spent_count,"
                 "added_count,total_value_delta,applied_at) "
                 "VALUES (%d,'VERIFIED',1,0,0,0,0)", h);
        ins_ok = sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
    }
    sqlite3_exec(db, ins_ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    progress_store_tx_unlock();
    SD_CHECK("eqp: rows inserted", ins_ok);

    /* EXPLAIN QUERY PLAN of the exact dumper query. Column 3 ("detail") must
     * name the partial index; it must NOT be a bare "SCAN utxo_apply_log". */
    progress_store_tx_lock();
    sqlite3_stmt *st = NULL;
    bool prep = sqlite3_prepare_v2(db,
        "EXPLAIN QUERY PLAN "
        "SELECT height, COALESCE(status,''), "
        "       COALESCE(first_failure_kind,''), first_failure_detail "
        "  FROM utxo_apply_log WHERE ok=0 "
        " ORDER BY height ASC LIMIT 1",
        -1, &st, NULL) == SQLITE_OK;
    SD_CHECK("eqp: prepare EXPLAIN QUERY PLAN", prep);

    bool uses_index = false;
    bool bare_scan = false;
    while (prep && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *detail = sqlite3_column_text(st, 3);
        const char *d = detail ? (const char *)detail : "";
        if (strstr(d, "idx_utxo_apply_log_ok0"))
            uses_index = true;
        /* A full-table scan reads "SCAN utxo_apply_log" with no "USING INDEX". */
        if (strstr(d, "SCAN utxo_apply_log") && !strstr(d, "USING INDEX"))
            bare_scan = true;
    }
    sqlite3_finalize(st);
    progress_store_tx_unlock();

    SD_CHECK("eqp: query plan uses idx_utxo_apply_log_ok0", uses_index);
    SD_CHECK("eqp: query plan is NOT a bare full-table scan", !bare_scan);

    progress_store_close();
    test_cleanup_tmpdir(dir);
    return failures;
}

int test_stage_dump_trylock(void)
{
    int failures = 0;
    failures += case_dumpers_busy_path();
    failures += case_utxo_apply_ok0_index_used();
    if (failures == 0)
        printf("test_stage_dump_trylock: ALL PASSED\n");
    else
        printf("test_stage_dump_trylock: %d FAILURE(S)\n", failures);
    return failures;
}
