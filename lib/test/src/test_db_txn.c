/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the db_txn scoped transaction wrapper.
 *
 * Each test opens an in-memory node_db, drives db_txn through a
 * specific state, and asserts both the behaviour and the events
 * emitted on the bus. A local sync observer counts each event type
 * so assertions can be made against the exact lifecycle ("one begin
 * and one commit" vs "one begin, one rollback, one leaked").
 */

#include "test/test_helpers.h"
#include "models/db_txn.h"
#include "models/database.h"
#include "event/event.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* ── Event counters ─────────────────────────────────────────── */

static _Atomic int g_ev_begin;
static _Atomic int g_ev_commit;
static _Atomic int g_ev_rollback;
static _Atomic int g_ev_rejected;
static _Atomic int g_ev_leaked;

static void dt_observer(enum event_type type, uint32_t peer_id,
                        const void *payload, uint32_t payload_len, void *ctx)
{
    (void)peer_id; (void)payload; (void)payload_len; (void)ctx;
    switch (type) {
    case EV_DB_TXN_BEGIN:    atomic_fetch_add(&g_ev_begin,    1); break;
    case EV_DB_TXN_COMMIT:   atomic_fetch_add(&g_ev_commit,   1); break;
    case EV_DB_TXN_ROLLBACK: atomic_fetch_add(&g_ev_rollback, 1); break;
    case EV_DB_TXN_REJECTED: atomic_fetch_add(&g_ev_rejected, 1); break;
    case EV_DB_TXN_LEAKED:   atomic_fetch_add(&g_ev_leaked,   1); break;
    default: break;
    }
}

static void dt_install_observer(void)
{
    event_clear_observers(EV_DB_TXN_BEGIN);
    event_clear_observers(EV_DB_TXN_COMMIT);
    event_clear_observers(EV_DB_TXN_ROLLBACK);
    event_clear_observers(EV_DB_TXN_REJECTED);
    event_clear_observers(EV_DB_TXN_LEAKED);
    atomic_store(&g_ev_begin,    0);
    atomic_store(&g_ev_commit,   0);
    atomic_store(&g_ev_rollback, 0);
    atomic_store(&g_ev_rejected, 0);
    atomic_store(&g_ev_leaked,   0);
    event_observe(EV_DB_TXN_BEGIN,    dt_observer, NULL);
    event_observe(EV_DB_TXN_COMMIT,   dt_observer, NULL);
    event_observe(EV_DB_TXN_ROLLBACK, dt_observer, NULL);
    event_observe(EV_DB_TXN_REJECTED, dt_observer, NULL);
    event_observe(EV_DB_TXN_LEAKED,   dt_observer, NULL);
}

#define DT_RUN(name, expr) do { \
    printf("%s... ", (name));   \
    bool _ok = (expr);          \
    if (_ok) printf("OK\n");    \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* ── 1. Commit path: begin + commit emits exactly 1 of each ── */

static int t_commit_path(void)
{
    int failures = 0;
    dt_install_observer();

    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    bool opened = node_db_open(&ndb, ":memory:");
    if (!opened) { printf("dt: open failed\n"); return 1; }

    struct db_txn *txn = db_txn_begin(&ndb, "test.commit");
    bool begin_ok = txn != NULL;
    bool commit_ok = db_txn_commit(txn);
    db_txn_auto_rollback(&txn);  /* releases handle */

    bool ok = begin_ok && commit_ok &&
              atomic_load(&g_ev_begin)    == 1 &&
              atomic_load(&g_ev_commit)   == 1 &&
              atomic_load(&g_ev_rollback) == 0 &&
              atomic_load(&g_ev_leaked)   == 0;
    DT_RUN("dt: commit path emits begin+commit only", ok);

    node_db_close(&ndb);
    return failures;
}

/* ── 2. Explicit rollback: begin + rollback, no leak ───────── */

static int t_explicit_rollback(void)
{
    int failures = 0;
    dt_install_observer();

    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    node_db_open(&ndb, ":memory:");

    struct db_txn *txn = db_txn_begin(&ndb, "test.rollback");
    db_txn_rollback(txn);
    db_txn_auto_rollback(&txn);

    bool ok = atomic_load(&g_ev_begin)    == 1 &&
              atomic_load(&g_ev_rollback) == 1 &&  /* explicit */
              atomic_load(&g_ev_leaked)   == 0 &&
              atomic_load(&g_ev_commit)   == 0;
    DT_RUN("dt: explicit rollback emits begin+rollback, no leak", ok);

    node_db_close(&ndb);
    return failures;
}

/* ── 3. Leak detection: scope exits without commit/rollback ─ */

static void leak_scope(struct node_db *ndb)
{
    DB_TXN_SCOPE(txn, ndb, "test.leak");
    (void)txn;
    /* Fall out of scope without committing — auto_rollback fires. */
}

static int t_leak_detection(void)
{
    int failures = 0;
    dt_install_observer();

    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    node_db_open(&ndb, ":memory:");

    leak_scope(&ndb);

    bool ok = atomic_load(&g_ev_begin)    == 1 &&
              atomic_load(&g_ev_leaked)   == 1 &&
              /* auto_rollback emits rollback(leaked) in addition to
               * the LEAKED marker so the db actually rolls back. */
              atomic_load(&g_ev_rollback) == 1 &&
              atomic_load(&g_ev_commit)   == 0;
    DT_RUN("dt: scope exit without commit fires LEAKED + rollback", ok);

    node_db_close(&ndb);
    return failures;
}

/* ── 4. Scope + explicit commit: no leak event ─────────────── */

static int t_scope_with_commit(void)
{
    int failures = 0;
    dt_install_observer();

    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    node_db_open(&ndb, ":memory:");

    bool commit_ok = false;
    {
        DB_TXN_SCOPE(txn, &ndb, "test.scope_commit");
        commit_ok = db_txn_commit(txn);
    }  /* auto_rollback fires here; sees committed, just frees */

    bool ok = commit_ok &&
              atomic_load(&g_ev_begin)    == 1 &&
              atomic_load(&g_ev_commit)   == 1 &&
              atomic_load(&g_ev_leaked)   == 0 &&
              atomic_load(&g_ev_rollback) == 0;
    DT_RUN("dt: scope + explicit commit emits no leak event", ok);

    node_db_close(&ndb);
    return failures;
}

/* ── 5. Nesting is rejected ─────────────────────────────────── */

static int t_nesting_rejected(void)
{
    int failures = 0;
    dt_install_observer();

    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    node_db_open(&ndb, ":memory:");

    struct db_txn *outer = db_txn_begin(&ndb, "test.nest_outer");
    bool outer_ok = (outer != NULL);

    struct db_txn *inner = db_txn_begin(&ndb, "test.nest_inner");
    bool inner_rejected = (inner == NULL);

    db_txn_rollback(outer);
    db_txn_auto_rollback(&outer);

    bool ok = outer_ok && inner_rejected &&
              atomic_load(&g_ev_begin)    == 1 &&  /* outer only */
              atomic_load(&g_ev_rejected) == 1 &&  /* inner */
              atomic_load(&g_ev_rollback) == 1 &&
              atomic_load(&g_ev_leaked)   == 0;
    DT_RUN("dt: nested db_txn_begin is REJECTED", ok);

    node_db_close(&ndb);
    return failures;
}

/* ── 6. NULL db is rejected ─────────────────────────────────── */

static int t_null_db_rejected(void)
{
    int failures = 0;
    dt_install_observer();
    struct db_txn *txn = db_txn_begin(NULL, "test.null_db");
    bool ok = txn == NULL && atomic_load(&g_ev_rejected) == 1;
    DT_RUN("dt: NULL db is REJECTED", ok);
    return failures;
}

/* ── 7. NULL/empty label is rejected ────────────────────────── */

static int t_null_label_rejected(void)
{
    int failures = 0;
    dt_install_observer();

    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    node_db_open(&ndb, ":memory:");

    struct db_txn *t1 = db_txn_begin(&ndb, NULL);
    struct db_txn *t2 = db_txn_begin(&ndb, "");
    bool ok = t1 == NULL && t2 == NULL &&
              atomic_load(&g_ev_rejected) == 2;
    DT_RUN("dt: NULL / empty label is REJECTED", ok);

    node_db_close(&ndb);
    return failures;
}

/* ── 8. Closed db is rejected ───────────────────────────────── */

static int t_closed_db_rejected(void)
{
    int failures = 0;
    dt_install_observer();

    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    /* Never opened — open flag is false */

    struct db_txn *txn = db_txn_begin(&ndb, "test.closed");
    bool ok = txn == NULL && atomic_load(&g_ev_rejected) == 1;
    DT_RUN("dt: closed db is REJECTED", ok);
    return failures;
}

/* ── 9. Idempotent rollback (calling twice is fine) ─────────── */

static int t_idempotent_rollback(void)
{
    int failures = 0;
    dt_install_observer();

    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    node_db_open(&ndb, ":memory:");

    struct db_txn *txn = db_txn_begin(&ndb, "test.idem");
    db_txn_rollback(txn);
    db_txn_rollback(txn);  /* second call: no-op */
    db_txn_rollback(txn);  /* third call: no-op */
    db_txn_auto_rollback(&txn);

    bool ok = atomic_load(&g_ev_begin) == 1 &&
              atomic_load(&g_ev_rollback) == 1 &&  /* exactly one */
              atomic_load(&g_ev_leaked) == 0;
    DT_RUN("dt: multiple rollbacks emit exactly one rollback event", ok);

    node_db_close(&ndb);
    return failures;
}

/* ── 10. Rollback-after-commit is a harmless no-op ─────────── */

static int t_rollback_after_commit(void)
{
    int failures = 0;
    dt_install_observer();

    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    node_db_open(&ndb, ":memory:");

    struct db_txn *txn = db_txn_begin(&ndb, "test.rollback_after_commit");
    db_txn_commit(txn);
    db_txn_rollback(txn);  /* no-op */
    db_txn_auto_rollback(&txn);

    bool ok = atomic_load(&g_ev_begin)    == 1 &&
              atomic_load(&g_ev_commit)   == 1 &&
              atomic_load(&g_ev_rollback) == 0 &&
              atomic_load(&g_ev_leaked)   == 0;
    DT_RUN("dt: rollback after commit is a silent no-op", ok);

    node_db_close(&ndb);
    return failures;
}

/* ── 11. Double commit is a bug → LEAKED event ─────────────── */

static int t_double_commit(void)
{
    int failures = 0;
    dt_install_observer();

    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    node_db_open(&ndb, ":memory:");

    struct db_txn *txn = db_txn_begin(&ndb, "test.double_commit");
    bool c1 = db_txn_commit(txn);
    bool c2 = db_txn_commit(txn);  /* should fail + emit LEAKED */
    db_txn_auto_rollback(&txn);

    bool ok = c1 && !c2 &&
              atomic_load(&g_ev_commit) == 1 &&
              atomic_load(&g_ev_leaked) == 1;
    DT_RUN("dt: double commit returns false and emits LEAKED", ok);

    node_db_close(&ndb);
    return failures;
}

/* ── 12. Long label is truncated, not rejected ─────────────── */

static int t_label_truncation(void)
{
    int failures = 0;
    dt_install_observer();

    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    node_db_open(&ndb, ":memory:");

    /* Deliberately over DB_TXN_LABEL_MAX */
    char big[DB_TXN_LABEL_MAX + 40];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    struct db_txn *txn = db_txn_begin(&ndb, big);
    bool truncated = txn != NULL &&
                     strlen(txn->label) == (size_t)(DB_TXN_LABEL_MAX - 1);
    db_txn_commit(txn);
    db_txn_auto_rollback(&txn);

    bool ok = truncated &&
              atomic_load(&g_ev_begin) == 1 &&
              atomic_load(&g_ev_commit) == 1;
    DT_RUN("dt: long label is truncated, not rejected", ok);

    node_db_close(&ndb);
    return failures;
}

/* ── 13. Auto-rollback no-op on NULL pointer ──────────────── */

static int t_auto_rollback_null(void)
{
    int failures = 0;
    dt_install_observer();
    struct db_txn *txn = NULL;
    db_txn_auto_rollback(&txn);  /* must not crash */
    bool ok = atomic_load(&g_ev_begin)  == 0 &&
              atomic_load(&g_ev_leaked) == 0;
    DT_RUN("dt: auto_rollback on NULL is a safe no-op", ok);
    return failures;
}

/* ── 14. Concurrent txns on different databases ────────────── */

struct dt_thread_args {
    struct node_db *ndb;
    int iterations;
    _Atomic int ok_count;
};

static void *dt_thread_commit_loop(void *p)
{
    struct dt_thread_args *a = p;
    for (int i = 0; i < a->iterations; i++) {
        struct db_txn *txn = db_txn_begin(a->ndb, "test.concurrent");
        if (txn && db_txn_commit(txn)) atomic_fetch_add(&a->ok_count, 1);
        db_txn_auto_rollback(&txn);
    }
    return NULL;
}

static int t_concurrent_different_dbs(void)
{
    int failures = 0;
    dt_install_observer();

    struct node_db ndb_a, ndb_b;
    memset(&ndb_a, 0, sizeof(ndb_a));
    memset(&ndb_b, 0, sizeof(ndb_b));
    node_db_open(&ndb_a, ":memory:");
    node_db_open(&ndb_b, ":memory:");

    const int iters = 50;
    struct dt_thread_args args_a = { .ndb = &ndb_a, .iterations = iters };
    struct dt_thread_args args_b = { .ndb = &ndb_b, .iterations = iters };
    atomic_store(&args_a.ok_count, 0);
    atomic_store(&args_b.ok_count, 0);

    pthread_t ta, tb;
    pthread_create(&ta, NULL, dt_thread_commit_loop, &args_a);
    pthread_create(&tb, NULL, dt_thread_commit_loop, &args_b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    bool ok = atomic_load(&args_a.ok_count) == iters &&
              atomic_load(&args_b.ok_count) == iters &&
              atomic_load(&g_ev_begin)  == 2 * iters &&
              atomic_load(&g_ev_commit) == 2 * iters &&
              atomic_load(&g_ev_leaked) == 0;
    DT_RUN("dt: concurrent txns on different dbs succeed", ok);

    node_db_close(&ndb_a);
    node_db_close(&ndb_b);
    return failures;
}

/* ── 15. Induced mid-sequence failure rolls rows back ──────── */

/* This is the test that backs the wave 3 wiring: we write several
 * rows inside a DB_TXN_SCOPE, "abort" by returning before the
 * explicit commit, and then assert that none of the writes are
 * visible to a subsequent read. It is the end-to-end check that the
 * scope actually rolls back real data, not just the event ledger.
 *
 * The helper writes three keys, commits zero of them, and falls out
 * of scope so auto_rollback fires. The caller then verifies every
 * key is absent. */
static void induced_failure_scope(struct node_db *ndb)
{
    DB_TXN_SCOPE(txn, ndb, "test.induced_failure");
    if (!txn) return;

    /* Write three rows. All should disappear on scope exit. */
    (void)node_db_state_set(ndb, "induced.k1", "v1", 2);
    (void)node_db_state_set(ndb, "induced.k2", "v2-longer", 9);
    (void)node_db_state_set(ndb, "induced.k3", "v3", 2);

    /* Simulate a mid-sequence failure: early return WITHOUT
     * committing. The cleanup attribute runs auto_rollback and the
     * three rows above should never reach the final table. */
    return;
}

static int t_rollback_on_induced_failure(void)
{
    int failures = 0;
    dt_install_observer();

    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    bool opened = node_db_open(&ndb, ":memory:");
    if (!opened) {
        printf("dt: open failed for induced_failure test\n");
        return 1;
    }

    /* Sanity: none of the keys exist yet. */
    char buf[64];
    size_t got = 0;
    bool absent_before =
        !node_db_state_get(&ndb, "induced.k1", buf, sizeof(buf), &got) &&
        !node_db_state_get(&ndb, "induced.k2", buf, sizeof(buf), &got) &&
        !node_db_state_get(&ndb, "induced.k3", buf, sizeof(buf), &got);

    /* Run the aborted scope. */
    induced_failure_scope(&ndb);

    /* All three keys must still be absent — auto_rollback dropped
     * the partial writes. */
    bool absent_after =
        !node_db_state_get(&ndb, "induced.k1", buf, sizeof(buf), &got) &&
        !node_db_state_get(&ndb, "induced.k2", buf, sizeof(buf), &got) &&
        !node_db_state_get(&ndb, "induced.k3", buf, sizeof(buf), &got);

    /* And the event trail shows exactly the leak pattern: one begin,
     * one LEAKED, one rollback(leaked), zero commits. */
    bool events_ok = atomic_load(&g_ev_begin)    == 1 &&
                     atomic_load(&g_ev_leaked)   == 1 &&
                     atomic_load(&g_ev_rollback) == 1 &&
                     atomic_load(&g_ev_commit)   == 0;

    /* Cross-check: a follow-up committed write IS visible, proving
     * the db itself is still healthy after the rollback. */
    struct db_txn *good = db_txn_begin(&ndb, "test.induced_followup");
    bool follow_ok = good != NULL &&
                     node_db_state_set(&ndb, "induced.k4", "v4", 2) &&
                     db_txn_commit(good);
    db_txn_auto_rollback(&good);
    bool follow_visible =
        node_db_state_get(&ndb, "induced.k4", buf, sizeof(buf), &got) &&
        got == 2 && memcmp(buf, "v4", 2) == 0;

    bool ok = absent_before && absent_after && events_ok &&
              follow_ok && follow_visible;
    DT_RUN("dt: induced mid-sequence failure rolls written rows back", ok);

    node_db_close(&ndb);
    return failures;
}

/* ── 16. Scoped multi-row wipe rollback (recovery path shape) ── */

/* Mirrors the shape of snapsync_begin_receive after wave-3 wiring:
 * a scoped wipe of an existing set of rows, followed by a simulated
 * mid-sequence abort before commit. The expected behaviour is that
 * the pre-existing rows are still there after rollback — the DELETE
 * never landed. */
static int t_rollback_preserves_pre_existing_rows(void)
{
    int failures = 0;
    dt_install_observer();

    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    node_db_open(&ndb, ":memory:");

    /* Seed two rows that must survive the aborted wipe. */
    (void)node_db_state_set(&ndb, "preexisting.a", "alpha", 5);
    (void)node_db_state_set(&ndb, "preexisting.b", "bravo", 5);

    /* Scoped wipe that fails halfway — scope exits without
     * committing, auto_rollback restores the rows. */
    {
        DB_TXN_SCOPE(txn, &ndb, "test.wipe_abort");
        if (!txn) {
            node_db_close(&ndb);
            return 1;
        }
        (void)node_db_exec(&ndb, "DELETE FROM node_state");
        /* Fall out of scope without db_txn_commit. */
    }

    char buf[64];
    size_t got = 0;
    bool a_present =
        node_db_state_get(&ndb, "preexisting.a", buf, sizeof(buf), &got) &&
        got == 5 && memcmp(buf, "alpha", 5) == 0;
    bool b_present =
        node_db_state_get(&ndb, "preexisting.b", buf, sizeof(buf), &got) &&
        got == 5 && memcmp(buf, "bravo", 5) == 0;

    bool ok = a_present && b_present &&
              atomic_load(&g_ev_leaked) == 1;
    DT_RUN("dt: aborted scoped wipe preserves pre-existing rows", ok);

    node_db_close(&ndb);
    return failures;
}

/* ── 17. Failed COMMIT explicitly unwinds SQLite state ───────── */

static int t_failed_commit_rolls_back(void)
{
    int failures = 0;
    dt_install_observer();

    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    bool opened = node_db_open(&ndb, ":memory:");
    bool schema_ok = opened && node_db_exec(&ndb,
        "CREATE TABLE dt_parent(id INTEGER PRIMARY KEY);"
        "CREATE TABLE dt_child(parent_id INTEGER NOT NULL,"
        "FOREIGN KEY(parent_id) REFERENCES dt_parent(id) "
        "DEFERRABLE INITIALLY DEFERRED)");

    struct db_txn *txn = schema_ok
        ? db_txn_begin(&ndb, "test.failed_commit") : NULL;
    bool inserted = txn && node_db_exec(
        &ndb, "INSERT INTO dt_child(parent_id) VALUES(7)");
    bool commit_failed = inserted && !db_txn_commit(txn);
    db_txn_auto_rollback(&txn);

    struct node_db_status status = {0};
    node_db_get_status(&ndb, &status);
    sqlite3_stmt *st = NULL;
    bool count_ok = opened && sqlite3_prepare_v2(ndb.db,
        "SELECT COUNT(*) FROM dt_child", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 0;
    sqlite3_finalize(st);

    struct db_txn *followup = opened
        ? db_txn_begin(&ndb, "test.failed_followup") : NULL;
    bool followup_ok = followup && db_txn_commit(followup);
    db_txn_auto_rollback(&followup);
    bool ok = opened && schema_ok && inserted && commit_failed && count_ok &&
              followup_ok && !status.tx_open &&
              atomic_load(&g_ev_rollback) == 1 &&
              atomic_load(&g_ev_leaked) == 0;
    DT_RUN("dt: failed COMMIT rolls back before ownership is released", ok);
    if (opened)
        node_db_close(&ndb);
    return failures;
}

/* ── Aggregator ─────────────────────────────────────────────── */

int test_db_txn(void)
{
    printf("\n=== db_txn tests ===\n");
    int failures = 0;
    failures += t_commit_path();
    failures += t_explicit_rollback();
    failures += t_leak_detection();
    failures += t_scope_with_commit();
    failures += t_nesting_rejected();
    failures += t_null_db_rejected();
    failures += t_null_label_rejected();
    failures += t_closed_db_rejected();
    failures += t_idempotent_rollback();
    failures += t_rollback_after_commit();
    failures += t_double_commit();
    failures += t_label_truncation();
    failures += t_auto_rollback_null();
    failures += t_concurrent_different_dbs();
    failures += t_rollback_on_induced_failure();
    failures += t_rollback_preserves_pre_existing_rows();
    failures += t_failed_commit_rolls_back();

    event_clear_observers(EV_DB_TXN_BEGIN);
    event_clear_observers(EV_DB_TXN_COMMIT);
    event_clear_observers(EV_DB_TXN_ROLLBACK);
    event_clear_observers(EV_DB_TXN_REJECTED);
    event_clear_observers(EV_DB_TXN_LEAKED);
    return failures;
}
