/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Shared snapshot import implementation for the pre-restore probe in boot.c
 * and the post-services receive path in boot_services.c. The function takes
 * `struct node_db *` directly (not boot_svc_ctx) so it can run before
 * services have been composed. See config/include/config/boot_snapshot_import.h.
 */

#include "config/boot_snapshot_import.h"
#include "models/database.h"
#include "util/boot_progress.h"
#include "util/log_macros.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

/* Pump thread that ticks boot_progress every second during the bulk
 * INSERT. The INSERT can take tens of seconds on slower disks; without
 * this, systemd WatchdogSec=120 would fire SIGABRT mid-write. The
 * thread exits as soon as the import path sets the stop flag. */
struct progress_pump {
    _Atomic bool stop;
    pthread_t tid;
};

static void *progress_pump_run(void *arg)
{
    struct progress_pump *p = arg;
    while (!atomic_load_explicit(&p->stop, memory_order_acquire)) {
        boot_progress_tick("snapshot_import_bulk_insert");
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void progress_pump_start(struct progress_pump *p)
{
    atomic_store_explicit(&p->stop, false, memory_order_release);
    if (pthread_create(&p->tid, NULL, progress_pump_run, p) != 0)  // raw-pthread-ok:boot-time-pump-no-thread-registry-dep
        p->tid = 0;
}

static void progress_pump_stop(struct progress_pump *p)
{
    atomic_store_explicit(&p->stop, true, memory_order_release);
    if (p->tid)
        pthread_join(p->tid, NULL);
}

bool boot_import_snapshot_db(struct node_db *ndb,
                              const char *snapshot_path,
                              int64_t *out_utxo_count,
                              int64_t *out_snap_height,
                              uint8_t out_best_hash[32])
{
    if (!ndb || !ndb->open || !ndb->db || !snapshot_path)
        LOG_FAIL("boot_snapshot_import", "null inputs");

    struct stat st;
    if (stat(snapshot_path, &st) != 0)
        LOG_FAIL("boot_snapshot_import", "stat %s: %s",
                 snapshot_path, strerror(errno));
    if (st.st_size < (off_t)(10 * 1024 * 1024))
        LOG_FAIL("boot_snapshot_import",
                 "snapshot too small (%lld bytes) — likely truncated",
                 (long long)st.st_size);

    sqlite3 *src = NULL;
    if (sqlite3_open_v2(snapshot_path, &src,
                        SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        const char *m = src ? sqlite3_errmsg(src) : "n/a";
        if (src) sqlite3_close(src);
        LOG_FAIL("boot_snapshot_import", "open ro %s: %s",
                 snapshot_path, m);
    }

    bool integrity_ok = false;
    {
        sqlite3_stmt *ck = NULL;
        if (sqlite3_prepare_v2(src, "PRAGMA integrity_check",
                               -1, &ck, NULL) == SQLITE_OK && ck) {
            if (sqlite3_step(ck) == SQLITE_ROW) {  // raw-sql-ok:integrity-pragma
                const unsigned char *r = sqlite3_column_text(ck, 0);
                integrity_ok = r && strcmp((const char *)r, "ok") == 0;
            }
            sqlite3_finalize(ck);
        }
    }
    if (!integrity_ok) {
        sqlite3_close(src);
        LOG_FAIL("boot_snapshot_import",
                 "integrity_check failed for %s", snapshot_path);
    }

    int64_t snap_height = -1;
    {
        sqlite3_stmt *q = NULL;
        if (sqlite3_prepare_v2(src,
                "SELECT value FROM _snapshot_meta WHERE key='height'",
                -1, &q, NULL) == SQLITE_OK && q) {
            if (sqlite3_step(q) == SQLITE_ROW) {  // raw-sql-ok:read-only-snapshot
                const unsigned char *v = sqlite3_column_text(q, 0);
                if (v) snap_height = strtoll((const char *)v, NULL, 10);
            }
            sqlite3_finalize(q);
        }
    }
    if (snap_height < 1) {
        sqlite3_close(src);
        LOG_FAIL("boot_snapshot_import",
                 "missing/invalid _snapshot_meta.height");
    }

    uint8_t best_hash[32] = {0};
    bool best_found = false;
    {
        sqlite3_stmt *q = NULL;
        if (sqlite3_prepare_v2(src,
                "SELECT hash FROM blocks WHERE height=?",
                -1, &q, NULL) == SQLITE_OK && q) {
            sqlite3_bind_int64(q, 1, snap_height);
            if (sqlite3_step(q) == SQLITE_ROW) {  // raw-sql-ok:read-only-snapshot
                const void *b = sqlite3_column_blob(q, 0);
                int n = sqlite3_column_bytes(q, 0);
                if (b && n == 32) {
                    memcpy(best_hash, b, 32);
                    best_found = true;
                }
            }
            sqlite3_finalize(q);
        }
    }
    if (!best_found) {
        sqlite3_close(src);
        LOG_FAIL("boot_snapshot_import",
                 "no blocks row at h=%lld", (long long)snap_height);
    }

    int64_t snap_utxos = 0;
    {
        sqlite3_stmt *q = NULL;
        if (sqlite3_prepare_v2(src,
                "SELECT COUNT(*) FROM utxos",
                -1, &q, NULL) == SQLITE_OK && q) {
            if (sqlite3_step(q) == SQLITE_ROW)  // raw-sql-ok:read-only-snapshot
                snap_utxos = sqlite3_column_int64(q, 0);
            sqlite3_finalize(q);
        }
    }
    sqlite3_close(src);
    if (snap_utxos < 1000)
        LOG_FAIL("boot_snapshot_import",
                 "implausible utxo count %lld", (long long)snap_utxos);

    /* Stash prior coins_best_block so we can restore on failure
     * after the bulk-copy has committed. */
    uint8_t prior_cb[32] = {0};
    size_t prior_cb_len = 0;
    bool prior_cb_present = node_db_state_get(ndb, "coins_best_block",
                                              prior_cb, sizeof(prior_cb),
                                              &prior_cb_len);

    char attach_sql[640];
    snprintf(attach_sql, sizeof(attach_sql),
             "ATTACH DATABASE '%s' AS snapsrc", snapshot_path);
    char *err = NULL;
    if (sqlite3_exec(ndb->db, attach_sql, NULL, NULL, &err) != SQLITE_OK) {
        char msg[256] = "?";
        if (err) { snprintf(msg, sizeof(msg), "%s", err); sqlite3_free(err); }
        LOG_FAIL("boot_snapshot_import", "ATTACH failed: %s", msg);
    }

    bool ok = true;
    if (sqlite3_exec(ndb->db, "BEGIN IMMEDIATE", NULL, NULL, &err)
        != SQLITE_OK) {
        char msg[256] = "?";
        if (err) { snprintf(msg, sizeof(msg), "%s", err); sqlite3_free(err); }
        sqlite3_exec(ndb->db, "DETACH DATABASE snapsrc", NULL, NULL, NULL);
        LOG_FAIL("boot_snapshot_import", "BEGIN failed: %s", msg);
    }
    /* Pump systemd watchdog liveness while the bulk copy runs.
     * The single INSERT statement holds the main thread for tens of
     * seconds on snapshots with millions of UTXOs; without this pump
     * the unit's WatchdogSec=120 timer would expire and SIGABRT the
     * process mid-write. */
    struct progress_pump pump = {0};
    progress_pump_start(&pump);
    if (ok && sqlite3_exec(ndb->db, "DELETE FROM main.utxos",
                           NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "[boot_snapshot_import] clear utxos: %s\n",  // obs-ok:bulk-import-failure
                err ? err : "n/a");
        if (err) { sqlite3_free(err); err = NULL; }
        ok = false;
    }
    if (ok && sqlite3_exec(ndb->db,
            "INSERT INTO main.utxos SELECT * FROM snapsrc.utxos",
            NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "[boot_snapshot_import] copy utxos: %s\n",  // obs-ok:bulk-import-failure
                err ? err : "n/a");
        if (err) { sqlite3_free(err); err = NULL; }
        ok = false;
    }
    if (ok)
        sqlite3_exec(ndb->db, "COMMIT", NULL, NULL, NULL);
    else
        sqlite3_exec(ndb->db, "ROLLBACK", NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "DETACH DATABASE snapsrc", NULL, NULL, NULL);
    progress_pump_stop(&pump);

    if (!ok)
        LOG_FAIL("boot_snapshot_import",
                 "bulk copy failed; node.db rolled back");

    if (!node_db_state_set(ndb, "coins_best_block",
                           best_hash, sizeof(best_hash))) {
        /* utxos already committed; restore prior anchor so the next
         * boot doesn't try to CSR-commit to a snapshot we lost. */
        if (prior_cb_present && prior_cb_len == 32)
            node_db_state_set(ndb, "coins_best_block",
                              prior_cb, prior_cb_len);
        LOG_FAIL("boot_snapshot_import", "set coins_best_block failed");
    }

    if (out_utxo_count)  *out_utxo_count  = snap_utxos;
    if (out_snap_height) *out_snap_height = snap_height;
    if (out_best_hash)   memcpy(out_best_hash, best_hash, 32);
    return true;
}
