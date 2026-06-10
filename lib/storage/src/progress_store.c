/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * progress_store — implementation. See storage/progress_store.h.
 *
 * One handle, one path, opened at boot once. The handle lives behind
 * an atomic pointer so `progress_store_db()` is a relaxed-atomic load
 * with no mutex; opens and closes serialise on a one-shot init mutex.
 *
 * Direct sqlite3_exec / sqlite3_step calls here carry the kernel-
 * primitive marker because, like the stage primitive itself, this
 * module sits below the AR lifecycle — the stage_cursor row is not a
 * model. */

#include "platform/time_compat.h"
#include "storage/progress_store.h"

#include "json/json.h"
#include "util/log_macros.h"
#include "util/stage.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define PROGRESS_STORE_FILENAME  "progress.kv"
#define PROGRESS_STORE_PATH_MAX  1024

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_tx_lock;
static pthread_once_t g_tx_lock_once = PTHREAD_ONCE_INIT;
static _Atomic(sqlite3 *) g_db = NULL;
static char g_path[PROGRESS_STORE_PATH_MAX];
static int64_t g_opened_at;

static void progress_store_tx_lock_init(void)
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

/* Apply WAL + reasonable durability/recovery pragmas. Errors here are
 * fatal for the open (caller will close & fail). */
static bool apply_pragmas(sqlite3 *db)
{
    static const char *const pragmas[] = {
        "PRAGMA journal_mode=WAL",
        "PRAGMA synchronous=NORMAL",
        "PRAGMA foreign_keys=ON",
        "PRAGMA busy_timeout=5000",
        NULL,
    };
    for (size_t i = 0; pragmas[i]; i++) {
        char *err = NULL;
        if (sqlite3_exec(db, pragmas[i], NULL, NULL, &err) != SQLITE_OK) {
            fprintf(stderr,  // obs-ok:progress-store-open-failure
                    "[progress_store] pragma failed (%s): %s\n",
                    pragmas[i], err ? err : "(no message)");
            if (err) sqlite3_free(err);
            return false;
        }
    }
    return true;
}

bool progress_store_open(const char *datadir)
{
    if (!datadir || !datadir[0]) LOG_FAIL("progress_store",
        "open: empty datadir");

    char path[PROGRESS_STORE_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s",
                     datadir, PROGRESS_STORE_FILENAME);
    if (n <= 0 || (size_t)n >= sizeof(path))
        LOG_FAIL("progress_store", "open: datadir path too long");

    pthread_mutex_lock(&g_lock);

    /* Already open with this path → idempotent success. */
    if (atomic_load_explicit(&g_db, memory_order_relaxed) != NULL) {
        bool same = (strcmp(g_path, path) == 0);
        pthread_mutex_unlock(&g_lock);
        if (!same)
            LOG_FAIL("progress_store",
                "open: already opened at a different path (%s vs %s)",
                g_path, path);
        return true;
    }

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:progress-store-open-failure
                "[progress_store] sqlite3_open_v2(%s) failed: %s\n",
                path, db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db) sqlite3_close(db);
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    if (!apply_pragmas(db) || !stage_table_ensure(db) ||
        !progress_meta_table_ensure(db)) {
        sqlite3_close(db);
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    snprintf(g_path, sizeof(g_path), "%s", path);
    g_opened_at = wall_now_s();
    atomic_store_explicit(&g_db, db, memory_order_release);

    pthread_mutex_unlock(&g_lock);

    fprintf(stderr,  // obs-ok:progress-store-lifecycle
            "[progress_store] opened %s (WAL)\n", path);
    return true;
}

sqlite3 *progress_store_db(void)
{
    return atomic_load_explicit(&g_db, memory_order_acquire);
}

void progress_store_tx_lock(void)
{
    pthread_once(&g_tx_lock_once, progress_store_tx_lock_init);
    pthread_mutex_lock(&g_tx_lock);
}

void progress_store_tx_unlock(void)
{
    pthread_mutex_unlock(&g_tx_lock);
}

void progress_store_close(void)
{
    pthread_mutex_lock(&g_lock);
    progress_store_tx_lock();
    sqlite3 *db = atomic_exchange_explicit(&g_db, NULL,
                                            memory_order_acq_rel);
    if (!db) {
        progress_store_tx_unlock();
        pthread_mutex_unlock(&g_lock);
        return;
    }

    /* Best-effort checkpoint so the WAL truncates cleanly. Failures
     * here are not fatal — sqlite3_close still runs. */
    char *err = NULL;
    if (sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE)",
                     NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:progress-store-lifecycle
                "[progress_store] checkpoint on close: %s\n",
                err ? err : "(no message)");
    }
    if (err) sqlite3_free(err);

    int rc = sqlite3_close(db);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:progress-store-lifecycle
                "[progress_store] sqlite3_close: rc=%d (%s)\n",
                rc, sqlite3_errstr(rc));
    } else {
        fprintf(stderr,  // obs-ok:progress-store-lifecycle
                "[progress_store] closed %s\n", g_path);
    }

    g_path[0] = '\0';
    g_opened_at = 0;
    progress_store_tx_unlock();
    pthread_mutex_unlock(&g_lock);
}

/* ── progress_meta ─────────────────────────────────────────────────────
 *
 * Tiny k/v table colocated with stage_cursor. See header for purpose.
 * Same kernel-primitive justification as stage_cursor — raw sqlite3_step
 * carries the `// raw-sql-ok:kernel-primitive` marker. */

bool progress_meta_table_ensure(sqlite3 *db)
{
    if (!db) return false;
    char *err = NULL;
    int rc = sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS progress_meta ("
        "  key   TEXT PRIMARY KEY,"
        "  value BLOB NOT NULL"
        ")",
        NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:progress-store-open-failure
                "[progress_store] progress_meta CREATE failed: %s\n",
                err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

static bool progress_meta_set_stmt(sqlite3 *db, const char *key,
                                   const void *value, size_t value_len)
{
    if (!db || !key || !key[0]) return false;
    if (value_len > 0 && !value) return false;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO progress_meta(key, value) VALUES(?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 2, value ? value : "", (int)value_len,
                      SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);  // raw-sql-ok:kernel-primitive
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

static bool progress_meta_delete_stmt(sqlite3 *db, const char *key)
{
    if (!db || !key || !key[0]) return false;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "DELETE FROM progress_meta WHERE key = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);  // raw-sql-ok:kernel-primitive
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool progress_meta_set_in_tx(sqlite3 *db, const char *key,
                             const void *value, size_t value_len)
{
    return progress_meta_set_stmt(db, key, value, value_len);
}

bool progress_meta_delete_in_tx(sqlite3 *db, const char *key)
{
    return progress_meta_delete_stmt(db, key);
}

/* Single-source the transaction discipline shared by progress_meta_set and
 * progress_meta_delete: lock, BEGIN IMMEDIATE, run op, COMMIT on success /
 * ROLLBACK on failure, unlock. op binds and steps its own statement. */
static bool progress_meta_run_in_tx(sqlite3 *db,
                                    bool (*op)(sqlite3 *, void *), void *arg)
{
    if (!db) return false;
    progress_store_tx_lock();
    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        progress_store_tx_unlock();
        return false;
    }
    bool ok = op(db, arg);
    const char *fini = ok ? "COMMIT" : "ROLLBACK";
    if (sqlite3_exec(db, fini, NULL, NULL, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        progress_store_tx_unlock();
        return false;
    }
    progress_store_tx_unlock();
    return ok;
}

struct progress_meta_set_args {
    const char *key;
    const void *value;
    size_t value_len;
};

static bool progress_meta_set_op(sqlite3 *db, void *arg)
{
    struct progress_meta_set_args *a = arg;
    return progress_meta_set_stmt(db, a->key, a->value, a->value_len);
}

static bool progress_meta_delete_op(sqlite3 *db, void *arg)
{
    return progress_meta_delete_stmt(db, (const char *)arg);
}

bool progress_meta_set(sqlite3 *db, const char *key,
                       const void *value, size_t value_len)
{
    struct progress_meta_set_args a = { key, value, value_len };
    return progress_meta_run_in_tx(db, progress_meta_set_op, &a);
}

bool progress_meta_delete(sqlite3 *db, const char *key)
{
    return progress_meta_run_in_tx(db, progress_meta_delete_op, (void *)key);
}

bool progress_meta_get(sqlite3 *db, const char *key,
                       void *out_buf, size_t out_cap,
                       size_t *out_len, bool *out_found)
{
    if (out_found) *out_found = false;
    if (out_len) *out_len = 0;
    if (!db || !key || !key[0]) return false;

    progress_store_tx_lock();
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT value FROM progress_meta WHERE key = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        progress_store_tx_unlock();
        return false;
    }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);  // raw-sql-ok:kernel-primitive
    bool ok = true;
    if (rc == SQLITE_ROW) {
        if (out_found) *out_found = true;
        int n = sqlite3_column_bytes(stmt, 0);
        const void *blob = sqlite3_column_blob(stmt, 0);
        if (out_len) *out_len = (size_t)n;
        if (out_buf && out_cap > 0) {
            size_t copy = (size_t)n < out_cap ? (size_t)n : out_cap;
            if (blob && copy > 0) memcpy(out_buf, blob, copy);
        }
    } else if (rc != SQLITE_DONE) {
        ok = false;
    }
    sqlite3_finalize(stmt);
    progress_store_tx_unlock();
    return ok;
}

static int64_t stage_cursor_count(sqlite3 *db)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM stage_cursor",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    int64_t n = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)  // raw-sql-ok:kernel-primitive
        n = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

bool progress_store_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    sqlite3 *db = progress_store_db();
    json_push_kv_bool(out, "open", db != NULL);
    /* g_path/g_opened_at are written under g_lock in progress_store_open;
     * snapshot both under the same lock so the dump never reads a torn or
     * mid-update value. (g_lock is not held on this diagnostic path.) */
    pthread_mutex_lock(&g_lock);
    char path_snap[PROGRESS_STORE_PATH_MAX];
    snprintf(path_snap, sizeof(path_snap), "%s", g_path);
    int64_t opened_at_snap = g_opened_at;
    pthread_mutex_unlock(&g_lock);
    json_push_kv_str (out, "path", path_snap);
    json_push_kv_int (out, "opened_at", opened_at_snap);

    if (db) {
        progress_store_tx_lock();
        json_push_kv_int(out, "stage_cursor_rows",
                         stage_cursor_count(db));
        progress_store_tx_unlock();
    } else {
        json_push_kv_int(out, "stage_cursor_rows", 0);
    }
    return true;
}
