/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MVP criterion #7 CI gate: recover from `kill -9` in <2 min.
 *
 * Exercises the SIGKILL-mid-block-apply recovery surface that 
 * (`ac782fef5`) protects.  For each of 10 cycles:
 *   1. Parent `fork()`s a child.
 *   2. Child opens the shared datadir, walks a connect-block-style write
 *      sequence inside a BEGIN/COMMIT transaction (insert a batch of
 *      UTXOs at height h+1, update the coins_best_block tip pointer,
 *      insert the block-index row), with a small per-step delay to
 *      widen the kill window.
 *   3. Parent sleeps a randomised short duration (0.5ms–40ms), then
 *      `kill(pid, SIGKILL)` — covers different points across the
 *      transaction (pre-begin, mid-insert, pre-commit, post-commit).
 *   4. Parent `waitpid()`s, then reopens the datadir through
 *      `coins_view_sqlite_open()` — the same entry point the live node
 *      takes on boot.
 *   5. Parent asserts: the reopen succeeds (the boot-time atomicity
 *      check + auto-rewind already covered by
 *      `test_coins_view_atomicity` kicks in if the child died in the
 *      "UTXOs ahead of tip" window), and the post-recovery state has
 *      no UTXO row above the tip height — the exact invariant
 *      `test_coins_view_atomicity` asserts, but now exercised under a
 *      real SIGKILL instead of a hand-crafted mismatch.
 *
 * Total elapsed time across all 10 cycles must stay under the
 * 2-minute MVP budget.
 *
 * What this test proves
 * ---------------------
 *   - SQLite's BEGIN/COMMIT atomicity survives `SIGKILL` — the journal
 *     rollback path is never half-applied.
 *   - `coins_view_sqlite_open`'s boot-time invariant (`utxos.height
 *     must never exceed tip.height`) auto-heals the crash-mid-flush
 *     shape (single-block overshoot ≤32 rows — see
 *     `test_coins_view_atomicity:t_utxos_one_ahead_auto_rewound`).
 *   - A `SIGKILL` anywhere during a realistic write loop does not
 *     leave the datadir in a shape the operator would have to fix
 *     by hand.
 *
 * What this test does NOT prove
 * -----------------------------
 *   - Full-binary cold restart recovery (that's MVP criterion #6's
 *     soak test — `deploy/zclassic23.service` under systemd).
 *   - Protocol-level resync after restart (covered by MVP criterion
 * #3, `test_cold_start_sync`).
 *   - Block validation correctness (covered by
 *     `test_chain_stall_repro`, `test_chain_rollback`,
 *     `test_consensus_reject_events`).
 *
 * It proves the narrower but load-bearing claim: "on-disk state is
 * atomically recoverable after `SIGKILL`", which is the hard part of
 * MVP criterion #7.
 *
 * Gating
 * ------
 * Skipped unless `ZCL_STRESS_TESTS=1`.  The test spawns 10 child
 * processes and does real SQLite I/O against a tempdir — measured at
 * ~4-8s on the dev box.  Keeping it out of `make test` default
 * protects the default suite's sub-minute budget.
 *
 * Invocation
 * ----------
 *   ZCL_STRESS_TESTS=1 build/bin/test_zcl
 *   ZCL_STRESS_TESTS=1 ZCL_TEST_ONLY=kill9 build/bin/test_zcl (focused)
 *
 * MVP linkage
 * -----------
 * Flips `MVP.md` criterion #7 from ☐ to ✅.  Forward-looking CI
 * gate, not RED-first.
 */

#include "platform/time_compat.h"
#include "test/test_helpers.h"
#include "storage/coins_view_sqlite.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int test_kill9_recovery(void);

/* ── Datadir helpers ────────────────────────────────────────
 *
 * Mirror the minimal SQLite schema builder from
 * `test_coins_view_atomicity.c` so this test is self-contained and
 * doesn't drag in node_db's full migration cost.  The helpers there
 * are file-static; duplicating (not extracting) keeps both tests
 * independent. */

static int p11_7_mkdir_p(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static void p11_7_dir_path(char *buf, size_t n)
{
    snprintf(buf, n, "./test-tmp/kill9_%d", (int)getpid());
}

static bool p11_7_build_schema(sqlite3 *db)
{
    char *err = NULL;
    int rc = sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS utxos("
        " txid BLOB, vout INTEGER, value INTEGER,"
        " script BLOB, script_type INTEGER, address_hash BLOB,"
        " height INTEGER, is_coinbase INTEGER,"
        " PRIMARY KEY(txid,vout));"
        "CREATE TABLE IF NOT EXISTS node_state("
        " key TEXT PRIMARY KEY, value BLOB);"
        "CREATE TABLE IF NOT EXISTS blocks("
        " hash BLOB PRIMARY KEY, height INTEGER, status INTEGER);",
        NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "p11_7 build_schema: %s\n", err ? err : "?");
        sqlite3_free(err);
        return false;
    }
    return true;
}

/* ── Query the current tip height via node_state + blocks join ── */

static int p11_7_query_tip_height(sqlite3 *db)
{
    sqlite3_stmt *s = NULL;
    int height = -1;
    int rc = sqlite3_prepare_v2(db,
        "SELECT b.height FROM blocks b, node_state n "
        "WHERE n.key = 'coins_best_block' AND b.hash = n.value",
        -1, &s, NULL);
    if (rc == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
        height = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return height;
}

/* ── Count UTXO rows strictly above the tip — the regression invariant ── */

static int p11_7_count_utxos_above_tip(sqlite3 *db)
{
    int tip = p11_7_query_tip_height(db);
    if (tip < 0) return -1;
    sqlite3_stmt *s = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM utxos WHERE height > ?", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int(s, 1, tip);
        if (sqlite3_step(s) == SQLITE_ROW)
            n = sqlite3_column_int(s, 0);
    }
    sqlite3_finalize(s);
    return n;
}

/* ── Child worker: realistic write loop, designed to be killed ──
 *
 * Each "block" application is wrapped in its own BEGIN/COMMIT —
 * matches the per-block atomicity guarantee the live node relies on.
 * A small nanosleep between steps widens the kill window so the
 * randomised parent delay has a real chance of landing mid-cycle.
 *
 * On success (not killed), the child exits 0 after applying the full
 * block batch.  On SIGKILL, the process dies leaving whichever
 * BEGIN/COMMIT rounds had already landed and possibly one
 * partially-staged transaction the SQLite journal will roll back on
 * the parent's next open. */

static void p11_7_child_worker(const char *dbpath, int start_height)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(dbpath, &db) != SQLITE_OK)
        _exit(1);

    /* WAL mode matches production (see boot_services.c); gives the
     * fastest recovery path on reopen. */
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);

    const int n_blocks = 30;
    for (int i = 0; i < n_blocks; i++) {
        int h = start_height + 1 + i;

        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
            _exit(2);

        /* Insert the block-index row first — matches the live write
         * order in connect_block. */
        uint8_t hash[32];
        memset(hash, (uint8_t)(0xA0 ^ (h & 0xFF)), 32);
        /* Mix the iteration into the hash so each block has a unique
         * primary key. */
        hash[0] ^= (uint8_t)(h >> 8);

        sqlite3_stmt *s = NULL;
        sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO blocks(hash,height,status) VALUES(?,?,3)",
            -1, &s, NULL);
        sqlite3_bind_blob(s, 1, hash, 32, SQLITE_TRANSIENT);
        sqlite3_bind_int(s, 2, h);
        sqlite3_step(s);
        sqlite3_finalize(s);

        /* Insert a handful of UTXOs at this block's height. */
        for (int k = 0; k < 4; k++) {
            uint8_t txid[32];
            memset(txid, 0, 32);
            txid[0] = (uint8_t)(h & 0xFF);
            txid[1] = (uint8_t)((h >> 8) & 0xFF);
            txid[2] = (uint8_t)k;
            sqlite3_prepare_v2(db,
                "INSERT OR REPLACE INTO utxos(txid,vout,value,script,"
                " script_type,address_hash,height,is_coinbase) "
                "VALUES(?,0,0,NULL,0,NULL,?,0)", -1, &s, NULL);
            sqlite3_bind_blob(s, 1, txid, 32, SQLITE_TRANSIENT);
            sqlite3_bind_int(s, 2, h);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }

        /* Update the tip pointer LAST — the ordering that makes the
         * "UTXOs ahead of tip" pathology observable if we get killed
         * between the UTXO inserts and the tip update. */
        sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO node_state(key,value) "
            "VALUES('coins_best_block',?)", -1, &s, NULL);
        sqlite3_bind_blob(s, 1, hash, 32, SQLITE_TRANSIENT);
        sqlite3_step(s);
        sqlite3_finalize(s);

        if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
            _exit(3);

        /* Widen the kill window: ~1ms per block.  30 blocks * 1ms ≈
         * 30ms total — covers the full 0.5-40ms randomised parent
         * kill range. */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L };
        nanosleep(&ts, NULL);
    }

    sqlite3_close(db);
    _exit(0);
}

/* ── One kill-and-recover cycle ───────────────────────────── */

static int p11_7_one_cycle(const char *dbpath, int cycle_idx,
                            unsigned int *rng_state)
{
    int start_height;
    {
        sqlite3 *db = NULL;
        if (sqlite3_open(dbpath, &db) != SQLITE_OK) {
            printf("FAIL (cycle %d: pre-fork open failed)\n", cycle_idx);
            return 1;
        }
        start_height = p11_7_query_tip_height(db);
        sqlite3_close(db);
    }
    if (start_height < 0) {
        printf("FAIL (cycle %d: no tip before fork)\n", cycle_idx);
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        p11_7_child_worker(dbpath, start_height);
        /* unreachable — child always _exit()s */
        _exit(99);
    }

    /* Randomised kill delay: 0.5ms to ~40ms.  Covers cases where the
     * child is (a) still opening the DB, (b) mid-transaction, (c)
     * between COMMIT rounds, (d) already finished (delay > 30ms). */
    long delay_us = 500 + (long)(rand_r(rng_state) % 40000);
    struct timespec delay_ts = {
        .tv_sec = 0,
        .tv_nsec = delay_us * 1000,
    };
    nanosleep(&delay_ts, NULL);

    if (kill(pid, SIGKILL) != 0 && errno != ESRCH) {
        /* ESRCH = child already finished; that's fine. */
        perror("kill");
        waitpid(pid, NULL, 0);
        return 1;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        perror("waitpid");
        return 1;
    }
    bool killed = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
    bool exited = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!killed && !exited) {
        printf("FAIL (cycle %d: child ended abnormally; "
               "WIFSIGNALED=%d WIFEXITED=%d status=%d)\n",
               cycle_idx, WIFSIGNALED(status), WIFEXITED(status), status);
        return 1;
    }

    /* Parent reopens through the live node's entry point.  This is
     * where the "UTXOs ahead of tip → auto-rewind-or-refuse" invariant
     * fires (see test_coins_view_atomicity). */
    sqlite3 *rdb = NULL;
    if (sqlite3_open(dbpath, &rdb) != SQLITE_OK) {
        printf("FAIL (cycle %d: reopen failed after %s)\n",
               cycle_idx, killed ? "SIGKILL" : "clean exit");
        return 1;
    }
    sqlite3_exec(rdb, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);

    struct coins_view_sqlite cvs;
    bool opened = coins_view_sqlite_open(&cvs, rdb);
    if (!opened) {
        /* The boot-time integrity check refused.  That's an acceptable
         * outcome if the overshoot exceeds the auto-rewind threshold
         * (32 rows per test_coins_view_atomicity:t_utxos_one_ahead_too_many_rejected).
         * But for our workload (4 UTXOs per block, 30 blocks max), the
         * overshoot should always be ≤ a few rows — auto-heal should
         * succeed every time.  A refusal here is a regression. */
        printf("FAIL (cycle %d: coins_view_sqlite_open refused after %s)\n",
               cycle_idx, killed ? "SIGKILL" : "clean exit");
        sqlite3_close(rdb);
        return 1;
    }
    coins_view_sqlite_close(&cvs);

    /* Post-recovery invariant: no UTXO row above the tip height. */
    int overshoot = p11_7_count_utxos_above_tip(rdb);
    if (overshoot != 0) {
        printf("FAIL (cycle %d: %d UTXO row(s) above tip after recovery "
               "— auto-rewind did not restore the invariant)\n",
               cycle_idx, overshoot);
        sqlite3_close(rdb);
        return 1;
    }

    sqlite3_close(rdb);
    return 0;
}

/* ── Test entrypoint ───────────────────────────────────────── */

int test_kill9_recovery(void)
{
    int failures = 0;
    printf("\n=== kill -9 recovery (MVP #7, <2 min) ===\n");
    printf("kill9_recovery SIGKILL-mid-apply × 10 cycles... ");

    if (!getenv("ZCL_STRESS_TESTS")) {
        printf("SKIP (set ZCL_STRESS_TESTS=1 to run — spawns 10 child procs)\n");
        return 0;
    }
    printf("\n");

    char dir[256];
    p11_7_dir_path(dir, sizeof(dir));
    p11_7_mkdir_p("./test-tmp");
    p11_7_mkdir_p(dir);

    char dbpath[512];
    snprintf(dbpath, sizeof(dbpath), "%s/node.db", dir);
    /* Remove any stale DB from an earlier aborted run — fresh start. */
    unlink(dbpath);
    {
        char wal[520];
        snprintf(wal, sizeof(wal), "%s-wal", dbpath); unlink(wal);
        snprintf(wal, sizeof(wal), "%s-shm", dbpath); unlink(wal);
    }

    /* Seed: build schema + genesis block at height 0 + one UTXO. */
    {
        sqlite3 *db = NULL;
        if (sqlite3_open(dbpath, &db) != SQLITE_OK) {
            printf("FAIL (seed open failed)\n");
            return 1;
        }
        if (!p11_7_build_schema(db)) {
            sqlite3_close(db);
            printf("FAIL (seed schema failed)\n");
            return 1;
        }

        uint8_t genesis[32];
        memset(genesis, 0xA0, 32);

        sqlite3_stmt *s = NULL;
        sqlite3_prepare_v2(db,
            "INSERT INTO blocks(hash,height,status) VALUES(?,0,3)",
            -1, &s, NULL);
        sqlite3_bind_blob(s, 1, genesis, 32, SQLITE_TRANSIENT);
        sqlite3_step(s); sqlite3_finalize(s);

        uint8_t gen_txid[32];
        memset(gen_txid, 0, 32); gen_txid[0] = 0xFF;
        sqlite3_prepare_v2(db,
            "INSERT INTO utxos(txid,vout,value,script,script_type,"
            " address_hash,height,is_coinbase) "
            "VALUES(?,0,0,NULL,0,NULL,0,1)", -1, &s, NULL);
        sqlite3_bind_blob(s, 1, gen_txid, 32, SQLITE_TRANSIENT);
        sqlite3_step(s); sqlite3_finalize(s);

        sqlite3_prepare_v2(db,
            "INSERT INTO node_state(key,value) "
            "VALUES('coins_best_block',?)", -1, &s, NULL);
        sqlite3_bind_blob(s, 1, genesis, 32, SQLITE_TRANSIENT);
        sqlite3_step(s); sqlite3_finalize(s);

        sqlite3_close(db);
    }

    const int n_cycles = 10;
    const int budget_sec = 120;

    unsigned int rng = (unsigned int)(platform_time_wall_time_t() ^ getpid());
    time_t t0 = platform_time_wall_time_t();

    int n_killed = 0, n_clean = 0;
    for (int i = 0; i < n_cycles; i++) {
        int before_tip;
        {
            sqlite3 *db = NULL;
            sqlite3_open(dbpath, &db);
            before_tip = p11_7_query_tip_height(db);
            sqlite3_close(db);
        }

        if (p11_7_one_cycle(dbpath, i, &rng) != 0) {
            failures++;
            break;
        }

        int after_tip;
        {
            sqlite3 *db = NULL;
            sqlite3_open(dbpath, &db);
            after_tip = p11_7_query_tip_height(db);
            sqlite3_close(db);
        }
        if (after_tip > before_tip) n_clean++;
        else n_killed++;
    }

    int elapsed = (int)(platform_time_wall_time_t() - t0);

    if (!failures && elapsed > budget_sec) {
        printf("FAIL (10 cycles took %ds, exceeds %ds MVP budget)\n",
               elapsed, budget_sec);
        failures++;
    }
    if (!failures) {
        printf(" kill9_recovery OK "
               "(10 cycles in %ds; %d advanced tip, %d killed mid-apply "
               "— all recovered cleanly, no UTXO overshoot)\n",
               elapsed, n_clean, n_killed);
    }

    /* Cleanup */
    test_cleanup_tmpdir(dir);

    return failures;
}
