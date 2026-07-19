/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * projection_store — implementation. See storage/projection_store.h.
 *
 * The owner of the progress.kv projection file (the kernel moved to consensus.db
 * in the A3 flip), behind an atomic pointer with a one-shot init/close mutex.
 * Projection co-writers use this handle + their OWN recursive tx mutex so their
 * BEGIN IMMEDIATE never serialises on the reducer drive's kernel tx lock — and,
 * post-flip, never shares the kernel's WAL journal either.
 *
 * Raw sqlite3_exec/step here carry the projection-store marker: like
 * progress_store this module sits below the AR lifecycle (the projection
 * tables it fronts are not models). */

#include "platform/time_compat.h"
#include "storage/projection_store.h"
#include "storage/progress_store.h"

#include "sqlite_integrity_gate.h"
#include "event/event.h"
#include "json/json.h"
#include "util/hw_profile.h"
#include "util/log_macros.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PROJECTION_STORE_FILENAME "progress.kv"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_tx_lock;
static pthread_once_t g_tx_lock_once = PTHREAD_ONCE_INIT;
static _Atomic(sqlite3 *) g_db = NULL;
static char g_path[PROJECTION_STORE_PATH_MAX];
static char g_display_path[PROJECTION_STORE_PATH_MAX];
static int g_dir_fd = -1;
static int64_t g_opened_at;

static void projection_store_tx_lock_init(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_tx_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

static int64_t wall_now_s(void)
{
    struct timespec ts;
    platform_time_realtime_timespec(&ts);
    return (int64_t)ts.tv_sec;
}

/* The projection handle is a SECONDARY connection: it shares the WAL the
 * kernel connection scaled, so it takes modest fixed page-cache / mmap
 * windows rather than doubling the kernel's RAM budget. WAL/synchronous/
 * foreign_keys/busy_timeout mirror progress_store's per-connection settings so
 * both handles honour the same durability + contention discipline on the same
 * file. */
#define PROJECTION_STORE_CACHE_KIB   (64 * 1024)             /* 64 MiB */
#define PROJECTION_STORE_MMAP_BYTES  (256LL * 1024 * 1024)   /* 256 MiB */

static bool apply_pragmas(sqlite3 *db)
{
    char cache_pragma[64], mmap_pragma[64];
    snprintf(cache_pragma, sizeof(cache_pragma), "PRAGMA cache_size=-%lld",
             (long long)PROJECTION_STORE_CACHE_KIB);
    snprintf(mmap_pragma, sizeof(mmap_pragma), "PRAGMA mmap_size=%lld",
             (long long)PROJECTION_STORE_MMAP_BYTES);

    const char *const pragmas[] = {
        "PRAGMA journal_mode=WAL",
        "PRAGMA synchronous=NORMAL",
        "PRAGMA foreign_keys=ON",
        "PRAGMA busy_timeout=5000",
        cache_pragma,
        mmap_pragma,
        NULL,
    };
    for (size_t i = 0; pragmas[i]; i++) {
        char *err = NULL;
        if (sqlite3_exec(db, pragmas[i], NULL, NULL, &err) != SQLITE_OK) {
            fprintf(stderr,  // obs-ok:projection-store-open-failure
                    "[projection_store] pragma failed (%s): %s\n",
                    pragmas[i], err ? err : "(no message)");
            if (err) sqlite3_free(err);
            return false;
        }
    }
    return true;
}

bool projection_store_open(const char *datadir)
{
    if (!datadir || !datadir[0]) LOG_FAIL("projection_store",
        "open: empty datadir");

    char display_path[PROJECTION_STORE_PATH_MAX];
    int n = snprintf(display_path, sizeof(display_path), "%s/%s",
                     datadir, PROJECTION_STORE_FILENAME);
    if (n <= 0 || (size_t)n >= sizeof(display_path))
        LOG_FAIL("projection_store", "open: datadir path too long");

    int opened_dir_fd = open(datadir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (opened_dir_fd < 0)
        LOG_FAIL("projection_store", "open: datadir capability failed: %s",
                 strerror(errno));
    char path[PROJECTION_STORE_PATH_MAX];
    n = snprintf(path, sizeof(path), "/proc/self/fd/%d/%s", opened_dir_fd,
                 PROJECTION_STORE_FILENAME);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        (void)close(opened_dir_fd);
        LOG_FAIL("projection_store", "open: capability path too long");
    }

    pthread_mutex_lock(&g_lock);

    if (atomic_load_explicit(&g_db, memory_order_relaxed) != NULL) {
        struct stat have;
        struct stat want;
        bool same = g_dir_fd >= 0 && fstat(g_dir_fd, &have) == 0 &&
                    fstat(opened_dir_fd, &want) == 0 &&
                    have.st_dev == want.st_dev && have.st_ino == want.st_ino;
        (void)close(opened_dir_fd);
        pthread_mutex_unlock(&g_lock);
        if (!same)
            LOG_FAIL("projection_store",
                "open: already opened at a different directory (%s vs %s)",
                g_display_path, display_path);
        return true;
    }

    /* CREATE: after the Wave A3 consensus.db flip the kernel handle
     * (progress_store) opens consensus.db, NOT progress.kv — so progress_store
     * no longer creates progress.kv. projection_store now OWNS progress.kv as
     * the dedicated projection file: on a fresh node it must mint it here (the
     * Class C address_index / txindex projections are fully rebuildable, so a
     * fresh or re-derived projection file is always safe; created_outputs is
     * a KERNEL table written through the consensus.db handle, not here — see
     * consensus_db.c's projection-stay exclusion list). */
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:projection-store-open-failure
                "[projection_store] sqlite3_open_v2(%s) failed: %s\n",
                path, db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db) sqlite3_close(db);
        (void)close(opened_dir_fd);
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    /* Integrity gate. progress.kv's projection tables (address_index / txindex
     * / created_outputs and kin) are fully rebuildable, but a corrupt file
     * left in place would otherwise surface as a mid-fold SQLITE_CORRUPT deep
     * inside a projection job — a JOB_FATAL with no named blocker. On a
     * non-"ok" quick_check, quarantine the file aside and reopen a FRESH one;
     * whichever projection job runs next re-creates its schema (CREATE TABLE
     * IF NOT EXISTS) and re-derives its rows from the kernel, same as a
     * brand-new node. AUTO-TERMINATING + idempotent: a fresh, just-created
     * store that ALSO fails quick_check is a disk/fs fault, not corrupt
     * derived state — fail the open instead of quarantine-looping. */
    if (!sqlite_integrity_quick_check_ok(db, "projection_store")) {
        fprintf(stderr,  // obs-ok:projection-store-open-failure
                "[projection_store] %s failed integrity quick_check; "
                "quarantining + re-deriving\n", path);
        sqlite3_close(db);
        db = NULL;
        sqlite_integrity_quarantine_corrupt(path, "projection_store",
                                            "projection_store_quarantine");

        rc = sqlite3_open_v2(path, &db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr,  // obs-ok:projection-store-open-failure
                    "[projection_store] reopen after quarantine of %s failed: "
                    "%s — disk/fs fault\n",
                    path, db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
            if (db) sqlite3_close(db);
            (void)close(opened_dir_fd);
            event_emitf(EV_RECOVERY_ACTION, 0,
                        "action=projection_store_reopen_failed "
                        "reason=disk_fault path=%s", path);
            pthread_mutex_unlock(&g_lock);
            return false;
        }
        if (!sqlite_integrity_quick_check_ok(db, "projection_store")) {
            /* A freshly-created, empty DB that fails quick_check cannot be
             * derived-state corruption — the underlying storage is broken.
             * Do NOT quarantine again (that would loop); fail terminally. */
            fprintf(stderr,  // obs-ok:projection-store-open-failure
                    "[projection_store] FRESH %s still fails quick_check — "
                    "terminal disk/fs fault, refusing to loop\n", path);
            sqlite3_close(db);
            (void)close(opened_dir_fd);
            event_emitf(EV_RECOVERY_ACTION, 0,
                        "action=projection_store_fresh_corrupt "
                        "reason=disk_fault path=%s", path);
            pthread_mutex_unlock(&g_lock);
            return false;
        }
        fprintf(stderr,  // obs-ok:projection-store-lifecycle
                "[projection_store] fresh %s opened after quarantine "
                "(projections re-derive on next fold)\n", path);
    }

    if (!apply_pragmas(db)) {
        sqlite3_close(db);
        (void)close(opened_dir_fd);
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    snprintf(g_path, sizeof(g_path), "%s", path);
    snprintf(g_display_path, sizeof(g_display_path), "%s", display_path);
    g_dir_fd = opened_dir_fd;
    g_opened_at = wall_now_s();
    atomic_store_explicit(&g_db, db, memory_order_release);

    pthread_mutex_unlock(&g_lock);

    fprintf(stderr,  // obs-ok:projection-store-lifecycle
            "[projection_store] opened %s (WAL, secondary handle)\n",
            display_path);
    return true;
}

sqlite3 *projection_store_db(void)
{
    return atomic_load_explicit(&g_db, memory_order_acquire);
}

bool projection_store_path(char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    pthread_mutex_lock(&g_lock);
    bool ok = g_path[0] != '\0' && strlen(g_path) < cap;
    if (ok) snprintf(out, cap, "%s", g_path);
    else    out[0] = '\0';
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void projection_store_tx_lock(void)
{
    pthread_once(&g_tx_lock_once, projection_store_tx_lock_init);
    pthread_mutex_lock(&g_tx_lock);
}

bool projection_store_tx_trylock(void)
{
    pthread_once(&g_tx_lock_once, projection_store_tx_lock_init);
    return pthread_mutex_trylock(&g_tx_lock) == 0;
}

void projection_store_tx_unlock(void)
{
    pthread_mutex_unlock(&g_tx_lock);
}

bool projection_store_run_in_tx(bool (*op)(sqlite3 *db, void *arg), void *arg)
{
    if (!op) return false;
    projection_store_tx_lock();
    sqlite3 *db = projection_store_db();
    if (!db) {
        projection_store_tx_unlock();
        return false;
    }
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:projection-store-lifecycle
                "[projection_store] BEGIN IMMEDIATE failed: %s\n",
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        projection_store_tx_unlock();
        return false;
    }
    bool ok = op(db, arg);
    const char *fini = ok ? "COMMIT" : "ROLLBACK";
    if (sqlite3_exec(db, fini, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:projection-store-lifecycle
                "[projection_store] %s failed: %s\n",
                fini, err ? err : "(no message)");
        if (err) sqlite3_free(err);
        /* A failed COMMIT leaves the tx open — force a rollback so the handle
         * is usable for the next batch. */
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        projection_store_tx_unlock();
        return false;
    }
    projection_store_tx_unlock();
    return ok;
}

void projection_store_close(void)
{
    pthread_mutex_lock(&g_lock);
    projection_store_tx_lock();
    sqlite3 *db = atomic_exchange_explicit(&g_db, NULL,
                                            memory_order_acq_rel);
    if (!db) {
        projection_store_tx_unlock();
        pthread_mutex_unlock(&g_lock);
        return;
    }

    int rc = sqlite3_close(db);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:projection-store-lifecycle
                "[projection_store] sqlite3_close: rc=%d (%s)\n",
                rc, sqlite3_errstr(rc));
    } else {
        fprintf(stderr,  // obs-ok:projection-store-lifecycle
                "[projection_store] closed %s\n", g_display_path);
    }

    if (g_dir_fd >= 0)
        (void)close(g_dir_fd);
    g_dir_fd = -1;
    g_path[0] = '\0';
    g_display_path[0] = '\0';
    g_opened_at = 0;
    projection_store_tx_unlock();
    pthread_mutex_unlock(&g_lock);
}

bool projection_store_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    sqlite3 *db = projection_store_db();
    json_push_kv_bool(out, "open", db != NULL);
    pthread_mutex_lock(&g_lock);
    char path_snap[PROJECTION_STORE_PATH_MAX];
    snprintf(path_snap, sizeof(path_snap), "%s", g_display_path);
    int64_t opened_at_snap = g_opened_at;
    pthread_mutex_unlock(&g_lock);
    json_push_kv_str(out, "path", path_snap);
    json_push_kv_int(out, "opened_at", opened_at_snap);
    /* Prove the split: this handle is a distinct sqlite3 connection from the
     * kernel's progress_store handle (both fronting the same physical file). */
    json_push_kv_bool(out, "independent_of_kernel",
                      db != NULL && db != progress_store_db());
    return true;
}
