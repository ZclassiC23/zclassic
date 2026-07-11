// wallet_backup_dump_state_json, implements the diagnostics_dump_fn typedef
// (CLAUDE.md "Adding state introspection": `bool <name>_dump_state_json(...)`)
// mandated by the g_dumpers[] dispatch table in
// app/controllers/src/diagnostics_registry.c; every other dumper in the
// codebase has the same bool signature for the same reason, so this is not
// a candidate for struct zcl_result conversion.

/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Wallet Backup Service — see header for rationale.
 *
 * Implementation strategy
 * -----------------------
 *
 * SQLite's online backup API (sqlite3_backup_init) copies the
 * whole database. We want only the wallet tables, so instead we
 * open the destination file as a fresh DB, ATTACH the source
 * via its on-disk path, and `CREATE TABLE <name> AS SELECT * FROM
 * src.<name>` for each wallet table. That keeps the destination
 * file small (users typically have a handful of wallet rows, not
 * the full 3M-row blocks table) and avoids copying UTXO data
 * that would leak peer-observable chain state to the backup.
 *
 * The ATTACH path must be absolute — sqlite3_db_filename returns
 * it for an opened connection, so we read that off the source
 * handle at backup time instead of asking the caller to thread
 * it through the config.
 *
 * Row-count verification
 * ----------------------
 *
 * After the CREATE TABLE AS SELECT statements run, we reopen the
 * destination in a second connection and count wallet_keys rows.
 * That round-trip proves the file is readable, the schema is as
 * expected, and the same number of keys landed as we thought we
 * wrote. Mismatches set last_error and emit
 * EV_WALLET_BACKUP_FAILED; the file is NOT deleted — operators
 * need the bytes even when verification fails.
 */

#include "platform/time_compat.h"
#include "services/wallet_backup_service.h"

#include "event/event.h"
#include "json/json.h"
#include "supervisors/domains.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "adapters/outbound/persistence/wallet_backup_store_sqlite.h"
#include "ports/wallet_backup_store_port.h"

#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/supervisor.h"
#include "util/thread_registry.h"

/* The wallet-backup sqlite surface lives behind wallet_backup_store_port.
 * This service binds the default sqlite adapter to the source node_db
 * connection per call; sqlite is named only by the adapter. The header
 * comment's "ATTACH + CREATE TABLE AS SELECT" strategy is unchanged — it
 * now executes inside the adapter. */

/* ── Wallet table list ──────────────────────────────────────── */

static const char *const WALLET_TABLES[] = {
    "wallet_keys",
    "wallet_sapling_keys",
    "wallet_seed",
    "wallet_scripts",
    "wallet_transactions",
    "wallet_utxos",
    "wallet_sapling_notes",
};

#define WALLET_TABLE_COUNT (sizeof(WALLET_TABLES) / sizeof(WALLET_TABLES[0]))
#define WALLET_BACKUP_SUPERVISOR_DEADLINE_SEC 60

/* ── Module state ───────────────────────────────────────────── */

struct wallet_backup_service_state {
    pthread_mutex_t lock;
    pthread_t       thread;
    bool            thread_running;
    bool            stop_requested;

    struct wallet_backup_config cfg;
    struct node_db             *db;

    /* Snapshot counters */
    int64_t total_runs;
    int64_t total_failures;
    int64_t last_run_unix;
    int64_t last_size_bytes;
    int64_t last_key_count;
    int64_t last_duration_ms;
    char    last_path[512];
    char    last_error[256];

    /* Debounced event trigger (D4: plan §5.4).
     * Set by wallet_backup_service_on_key_change; cleared by the
     * thread after running a debounce-eligible backup. */
    bool    key_change_pending;
    int64_t total_triggers;     /* total on_key_change calls (all, incl. coalesced) */
    int64_t total_trigger_runs; /* backups that actually ran due to a trigger */
    _Atomic supervisor_child_id supervisor_id;
};

static struct wallet_backup_service_state g_wbs = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .supervisor_id = SUPERVISOR_INVALID_ID,
};

static _Atomic int64_t g_wbs_last_backup_path_us = 0;

static struct liveness_contract g_wbs_contract;

/* ── Helpers ────────────────────────────────────────────────── */

static int64_t wbs_progress_marker(void)
{
    if (pthread_mutex_trylock(&g_wbs.lock) != 0)
        return 0;
    int64_t marker = g_wbs.total_runs + g_wbs.total_failures;
    pthread_mutex_unlock(&g_wbs.lock);
    return marker;
}

static void wbs_supervisor_heartbeat(void)
{
    supervisor_child_id id = atomic_load(&g_wbs.supervisor_id);
    if (id == SUPERVISOR_INVALID_ID)
        return;
    supervisor_tick(id);
    supervisor_progress(id, wbs_progress_marker());
}

static void wbs_on_stall(struct liveness_contract *c)
{
    const char *reason = c
        ? supervisor_stall_reason_name(
              (enum supervisor_stall_reason)atomic_load(&c->stall_reason))
        : "unknown";
    int64_t runs = -1;
    int64_t failures = -1;
    if (pthread_mutex_trylock(&g_wbs.lock) == 0) {
        runs = g_wbs.total_runs;
        failures = g_wbs.total_failures;
        pthread_mutex_unlock(&g_wbs.lock);
    }
    LOG_WARN("wallet_backup",
             "[wallet_backup] supervisor stall reason=%s runs=%lld failures=%lld",
             reason, (long long)runs, (long long)failures);
    event_emitf(EV_WALLET_BACKUP_FAILED, 0,
                "source=wallet_backup decision=worker_stall "
                "reason=%s runs=%lld failures=%lld",
                reason, (long long)runs, (long long)failures);
}

static struct zcl_result wbs_register_supervisor(void)
{
    if (!supervisor_start())
        return ZCL_ERR(-30, "wallet_backup: supervisor_start failed");

    supervisor_child_id id = atomic_load(&g_wbs.supervisor_id);
    if (id != SUPERVISOR_INVALID_ID) {
        supervisor_set_deadline(id, WALLET_BACKUP_SUPERVISOR_DEADLINE_SEC);
        supervisor_progress(id, wbs_progress_marker());
        supervisor_tick(id);
        return ZCL_OK;
    }

    liveness_contract_init(&g_wbs_contract, "wallet.backup");
    atomic_store(&g_wbs_contract.period_secs, 0);
    atomic_store(&g_wbs_contract.deadline_secs,
                 WALLET_BACKUP_SUPERVISOR_DEADLINE_SEC);
    atomic_store(&g_wbs_contract.progress_max_quiet_us, 0);
    g_wbs_contract.on_stall = wbs_on_stall;

    supervisor_domains_init();
    id = supervisor_register_in_domain(g_op_sup, &g_wbs_contract);
    if (id == SUPERVISOR_INVALID_ID)
        return ZCL_ERR(-31, "wallet_backup: supervisor_register failed");
    atomic_store(&g_wbs.supervisor_id, id);
    supervisor_progress(id, wbs_progress_marker());
    supervisor_tick(id);
    return ZCL_OK;
}

/* WALLET_BACKUP_PASSWORD env policy: non-empty => encrypt; absent or
 * empty => plaintext with a one-time warning (the service is the
 * key-loss safety net, so it must not refuse to run). The password is
 * kept as a FULL-LENGTH heap copy: the --decrypt-wallet-backup restore
 * path derives its key from the raw env string, so truncating here
 * (e.g. into a fixed buffer) would encrypt every backup under a key
 * the documented recovery path can never re-derive. The copy is cached
 * and never freed — a running service's shallow config copy may still
 * reference it. */
static void wbs_config_apply_env_password(struct wallet_backup_config *cfg)
{
    static char *cached_pw;
    static bool warned_plaintext;
    const char *env_pw = getenv("WALLET_BACKUP_PASSWORD");
    if (!env_pw || !*env_pw) {
        if (!warned_plaintext) {
            warned_plaintext = true;
            LOG_WARN("wallet_backup",
                     "WALLET_BACKUP_PASSWORD not set — wallet backups will "
                     "be written in cleartext (set it to enable encryption)");
        }
        return;
    }
    if (!cached_pw || strcmp(cached_pw, env_pw) != 0) {
        size_t len = strlen(env_pw) + 1;
        char *copy = zcl_malloc(len, "wallet_backup_env_pw");
        if (!copy) {
            /* encrypt=true with a NULL password makes
             * wallet_backup_start fail loudly (-24) instead of
             * silently writing plaintext against operator intent. */
            LOG_WARN("wallet_backup",
                     "cannot copy WALLET_BACKUP_PASSWORD (OOM) — backup "
                     "start will refuse rather than fall back to plaintext");
            cfg->encrypt = true;
            cfg->encrypt_password = NULL;
            return;
        }
        memcpy(copy, env_pw, len);
        cached_pw = copy;   /* old copy (if any) intentionally leaked */
    }
    cfg->encrypt = true;
    cfg->encrypt_password = cached_pw;
}

void wallet_backup_config_defaults(struct wallet_backup_config *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->interval_seconds = WALLET_BACKUP_DEFAULT_INTERVAL_SEC;
    cfg->max_versions     = WALLET_BACKUP_DEFAULT_MAX_VERSIONS;
    cfg->encrypt          = false;
    /* Fleet-wide encryption policy rides the env var so every
     * config_defaults caller (boot included) inherits it. */
    wbs_config_apply_env_password(cfg);
}

void wallet_backup_status_snapshot(struct wallet_backup_status *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&g_wbs.lock);
    out->running          = g_wbs.thread_running;
    out->total_runs       = g_wbs.total_runs;
    out->total_failures   = g_wbs.total_failures;
    out->last_run_unix    = g_wbs.last_run_unix;
    out->last_size_bytes  = g_wbs.last_size_bytes;
    out->last_key_count   = g_wbs.last_key_count;
    out->last_duration_ms = g_wbs.last_duration_ms;
    snprintf(out->last_path,  sizeof(out->last_path),  "%s", g_wbs.last_path);
    snprintf(out->last_error, sizeof(out->last_error), "%s", g_wbs.last_error);
    pthread_mutex_unlock(&g_wbs.lock);
}

/* See CLAUDE.md "Adding state introspection". Reentrant-safe: reuses the
 * lock-guarded snapshot that RPC/agent callers already read. */
bool wallet_backup_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;
    json_set_object(out);

    struct wallet_backup_status st;
    wallet_backup_status_snapshot(&st);
    json_push_kv_bool(out, "running", st.running);
    json_push_kv_int(out, "total_runs", st.total_runs);
    json_push_kv_int(out, "total_failures", st.total_failures);
    json_push_kv_int(out, "last_run_unix", st.last_run_unix);
    json_push_kv_int(out, "last_size_bytes", st.last_size_bytes);
    json_push_kv_int(out, "last_key_count", st.last_key_count);
    json_push_kv_int(out, "last_duration_ms", st.last_duration_ms);
    json_push_kv_str(out, "last_path", st.last_path);
    json_push_kv_str(out, "last_error", st.last_error);
    return true;
}

/* Create backup_dir with mode 0700 if missing. Returns true if the
 * directory exists on successful return. */
static bool wbs_ensure_backup_dir(const char *dir)
{
    if (!dir || !*dir) LOG_FAIL("wallet_backup", "backup dir is NULL or empty");
    struct stat st;
    if (stat(dir, &st) == 0)
        return S_ISDIR(st.st_mode);
    if (mkdir(dir, 0700) != 0) {
        fprintf(stderr, "wallet_backup: mkdir %s: %s\n",
                dir, strerror(errno));
        return false;
    }
    return true;
}

/* Bind the default sqlite adapter to the source node_db connection.
 * Returns true (filling ctx and port) when db has an open sqlite handle. */
static bool wbs_bind_store(struct node_db *db,
                           struct wallet_backup_store_sqlite_ctx *ctx,
                           struct wallet_backup_store_port *port)
{
    if (!db || !db->open || !db->db || !ctx || !port)
        return false;
    return wallet_backup_store_sqlite_bind(ctx, db->db, port);
}

/* Write the on-disk path backing the source connection into `out`.
 * Returns false for memory databases (out untouched / empty). */
static bool wbs_source_path(struct node_db *db, char *out, size_t cap)
{
    struct wallet_backup_store_sqlite_ctx ctx;
    struct wallet_backup_store_port port = {0};
    if (out && cap) out[0] = '\0';
    if (!wbs_bind_store(db, &ctx, &port))
        LOG_FAIL("wallet_backup", "source_path: NULL/closed db handle");
    if (!port.source_path(port.self, out, cap))
        LOG_FAIL("wallet_backup",
                 "source_path: db has no file path (in-memory?)");
    return true;
}

static int64_t wbs_unique_backup_timestamp_us(void)
{
    int64_t now = platform_time_realtime_us();
    int64_t prev = atomic_load(&g_wbs_last_backup_path_us);
    for (;;) {
        int64_t next = now > prev ? now : prev + 1;
        if (atomic_compare_exchange_weak(&g_wbs_last_backup_path_us,
                                         &prev, next))
            return next;
    }
}

/* SHA-style filename: wallet_backup_<unix_ts>_<usec>.sqlite. The usec
 * component is monotonicized to disambiguate rapid successive runs (tests call
 * run_once several times back-to-back). */
static void wbs_build_backup_path(const char *dir, char *out, size_t cap)
{
    int64_t now_us = wbs_unique_backup_timestamp_us();
    snprintf(out, cap, "%s/%s%lld_%06ld%s",
             dir,
             WALLET_BACKUP_FILENAME_PREFIX,
             (long long)(now_us / 1000000LL),
             (long)(now_us % 1000000LL),
             WALLET_BACKUP_FILENAME_SUFFIX);
}

/* ── Core primitive ─────────────────────────────────────────── */

/* Copy a non-ok result's message into the caller's err_out buffer
 * (the legacy buffer-form diagnostic) and return the result. The
 * zcl_result is the source of truth; err_out is a convenience mirror. */
#define WBS_FAIL(err_out, err_cap, code, ...) do {                       \
    struct zcl_result _wbs_r = ZCL_ERR((code), __VA_ARGS__);             \
    if ((err_out) && (err_cap))                                          \
        snprintf((err_out), (err_cap), "%s", _wbs_r.message);            \
    return _wbs_r;                                                       \
} while (0)

struct zcl_result wallet_backup_run_once(const char *backup_dir,
                             struct node_db *db,
                             char *out_path, size_t out_path_cap,
                             int64_t *out_key_count,
                             char *err_out, size_t err_cap)
{
    if (err_out && err_cap) err_out[0] = '\0';
    if (out_path && out_path_cap) out_path[0] = '\0';
    if (out_key_count) *out_key_count = -1;

    if (!backup_dir || !db || !db->open || !db->db)
        WBS_FAIL(err_out, err_cap, -1, "null arg or db not open");

    if (!wbs_ensure_backup_dir(backup_dir))
        WBS_FAIL(err_out, err_cap, -2, "cannot create backup_dir %s", backup_dir);

    /* Bind the sqlite adapter to the source connection. All sqlite
     * work below goes through the port. */
    struct wallet_backup_store_sqlite_ctx store_ctx;
    struct wallet_backup_store_port store = {0};
    if (!wbs_bind_store(db, &store_ctx, &store))
        WBS_FAIL(err_out, err_cap, -3, "cannot bind wallet backup store");

    char src_path[1024];
    if (!store.source_path(store.self, src_path, sizeof(src_path)))
        WBS_FAIL(err_out, err_cap, -3, "source db has no file path (in-memory?)");

    /* In-memory source is valid for tests: use the ATTACH TO
     * "file::memory:?cache=shared" form only if the caller opened
     * it with a real filename. Here we simply require a disk file
     * — tests that want to exercise the primitive use a tmpdir. */

    char dst_path[640];
    wbs_build_backup_path(backup_dir, dst_path, sizeof(dst_path));

    /* Open dst, ATTACH source, CREATE TABLE AS SELECT per wallet table,
     * DETACH, close — all inside the adapter. The AS SELECT form copies
     * both schema and rows; missing source tables are skipped (older
     * databases may not have every table). */
    char copy_err[ZCL_RESULT_MSG_MAX] = "";
    enum wallet_backup_store_status status =
        store.write_snapshot(store.self, dst_path, src_path,
                             WALLET_TABLES, WALLET_TABLE_COUNT,
                             copy_err, sizeof(copy_err));

    if (status == WB_STORE_OPEN_DST_FAILED)
        WBS_FAIL(err_out, err_cap, -4, "open dst failed: %s", dst_path);
    if (status == WB_STORE_ATTACH_FAILED)
        WBS_FAIL(err_out, err_cap, -5, "attach source failed: %s", src_path);

    if (status == WB_STORE_COPY_FAILED) {
        /* Leave the dst file on disk for forensics, but emit the
         * failure event and bail out. */
        struct stat st;
        int64_t bytes = stat(dst_path, &st) == 0 ? (int64_t)st.st_size : -1;
        struct zcl_result r = ZCL_ERR(-7, "%s", copy_err);
        if (err_out) snprintf(err_out, err_cap, "%s", r.message);
        event_emitf(EV_WALLET_BACKUP_FAILED, 0,
                    "path=%s bytes=%lld reason=%s",
                    dst_path, (long long)bytes, r.message);
        return r;
    }

    /* Round-trip verification: reopen the backup file read-only,
     * count the wallet_keys rows, and compare against the source.
     * If the counts differ the file is left on disk but we return
     * false so the caller knows the output is not usable. */
    int64_t src_key_count = -1;
    (void)store.count_rows(store.self, "wallet_keys", &src_key_count);
    int64_t dst_key_count =
        store.count_rows_in_file(store.self, dst_path, "wallet_keys");

    if (dst_key_count < 0 || dst_key_count != src_key_count) {
        struct zcl_result r = ZCL_ERR(-8,
                "verify row count mismatch src=%lld dst=%lld",
                (long long)src_key_count, (long long)dst_key_count);
        if (err_out) snprintf(err_out, err_cap, "%s", r.message);
        event_emitf(EV_WALLET_BACKUP_FAILED, 0,
                    "path=%s reason=%s", dst_path, r.message);
        return r;
    }

    struct stat st;
    int64_t bytes = stat(dst_path, &st) == 0 ? (int64_t)st.st_size : -1;
    event_emitf(EV_WALLET_BACKUP, 0,
                "path=%s bytes=%lld keys=%lld",
                dst_path, (long long)bytes, (long long)dst_key_count);

    if (out_path) snprintf(out_path, out_path_cap, "%s", dst_path);
    if (out_key_count) *out_key_count = dst_key_count;

    return ZCL_OK;
}

/* Rotation / listing (wallet_backup_list, wallet_backup_rotate) live
 * in wallet_backup_rotation.c. */

/* ── Synchronous entry points ───────────────────────────────── */

static struct zcl_result wbs_run_one_locked(void)
{
    int64_t started_ms = platform_time_monotonic_ms();
    char path[512] = "";
    char err[256]  = "";
    int64_t key_count = -1;
    struct zcl_result res = wallet_backup_run_once(g_wbs.cfg.backup_dir, g_wbs.db,
                                      path, sizeof(path),
                                      &key_count,
                                      err, sizeof(err));
    bool ok = res.ok;
    int64_t elapsed = platform_time_monotonic_ms() - started_ms;

    if (ok) {
        g_wbs.total_runs++;
        g_wbs.last_run_unix    = platform_time_wall_unix();
        g_wbs.last_key_count   = key_count;
        g_wbs.last_duration_ms = elapsed;
        snprintf(g_wbs.last_path, sizeof(g_wbs.last_path), "%s", path);
        g_wbs.last_error[0] = '\0';
        struct stat st;
        g_wbs.last_size_bytes =
            stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
        /* Encryption step. Order: write → verify rowcount (both done
         * inside wallet_backup_run_once, on the plaintext) → encrypt →
         * unlink plaintext → rotate. An encrypt failure KEEPS the
         * verified plaintext — never delete the only fresh backup —
         * and reports loudly instead. */
        if (g_wbs.cfg.encrypt && g_wbs.cfg.encrypt_password &&
            *g_wbs.cfg.encrypt_password) {
            char enc_path[576];
            size_t plen = strlen(path);
            size_t slen = strlen(WALLET_BACKUP_FILENAME_SUFFIX);
            int base = plen >= slen ? (int)(plen - slen) : (int)plen;
            snprintf(enc_path, sizeof(enc_path), "%.*s%s", base, path,
                     WALLET_BACKUP_FILENAME_SUFFIX_ENC);
            struct zcl_result er = wallet_backup_encrypt_file(
                path, enc_path, g_wbs.cfg.encrypt_password);
            if (er.ok) {
                if (unlink(path) != 0)
                    LOG_WARN("wallet_backup",
                             "encrypt: unlink plaintext %s failed: %s",
                             path, strerror(errno));
                snprintf(g_wbs.last_path, sizeof(g_wbs.last_path),
                         "%s", enc_path);
                g_wbs.last_size_bytes =
                    stat(enc_path, &st) == 0 ? (int64_t)st.st_size : -1;
            } else {
                g_wbs.total_failures++;
                snprintf(g_wbs.last_error, sizeof(g_wbs.last_error),
                         "encrypt_failed: %s", er.message);
                LOG_WARN("wallet_backup",
                         "encrypt failed, keeping plaintext %s: %s",
                         path, er.message);
                event_emitf(EV_WALLET_BACKUP_FAILED, 0,
                            "path=%s reason=encrypt_failed detail=%s",
                            path, er.message);
            }
        }
        /* Rotate after success — never lose the newest backup. */
        int max = g_wbs.cfg.max_versions > 0
            ? g_wbs.cfg.max_versions
            : WALLET_BACKUP_DEFAULT_MAX_VERSIONS;
        (void)wallet_backup_rotate(g_wbs.cfg.backup_dir, max);
    } else {
        g_wbs.total_failures++;
        snprintf(g_wbs.last_error, sizeof(g_wbs.last_error), "%s", err);
    }
    return res;
}

struct zcl_result wallet_backup_now(void)
{
    pthread_mutex_lock(&g_wbs.lock);
    if (!g_wbs.db || !g_wbs.cfg.backup_dir) {
        struct zcl_result r = ZCL_ERR(-10,
                "backup_now: service not initialized (db=%p dir=%s)",
                (void *)g_wbs.db, g_wbs.cfg.backup_dir ? g_wbs.cfg.backup_dir : "NULL");
        pthread_mutex_unlock(&g_wbs.lock);
        return r;
    }
    struct zcl_result res = wbs_run_one_locked();
    pthread_mutex_unlock(&g_wbs.lock);
    return res;
}

/* ── Thread loop ────────────────────────────────────────────── */

static void *wbs_thread_fn(void *arg)
{
    (void)arg;
    wbs_supervisor_heartbeat();
    pthread_mutex_lock(&g_wbs.lock);
    int interval = g_wbs.cfg.interval_seconds > 0
        ? g_wbs.cfg.interval_seconds
        : WALLET_BACKUP_DEFAULT_INTERVAL_SEC;
    pthread_mutex_unlock(&g_wbs.lock);

    /* Do one immediate backup on start so the user always has a
     * fresh copy within a few seconds of boot — the worst failure
     * is the boot that hasn't reached its first hourly tick yet. */
    (void)wallet_backup_now();
    wbs_supervisor_heartbeat();

    int64_t next_at_ms = platform_time_monotonic_ms() + (int64_t)interval * 1000;
    while (true) {
        pthread_mutex_lock(&g_wbs.lock);
        bool stop = g_wbs.stop_requested;
        bool pending = g_wbs.key_change_pending;
        int64_t last_ok = g_wbs.last_run_unix;
        pthread_mutex_unlock(&g_wbs.lock);
        if (stop) break;

        bool ran_this_tick = false;
        if (platform_time_monotonic_ms() >= next_at_ms) {
            (void)wallet_backup_now();
            wbs_supervisor_heartbeat();
            ran_this_tick = true;
            /* Re-read interval in case config was updated. */
            pthread_mutex_lock(&g_wbs.lock);
            interval = g_wbs.cfg.interval_seconds > 0
                ? g_wbs.cfg.interval_seconds
                : WALLET_BACKUP_DEFAULT_INTERVAL_SEC;
            pthread_mutex_unlock(&g_wbs.lock);
            next_at_ms = platform_time_monotonic_ms() + (int64_t)interval * 1000;
        } else if (pending) {
            /* Debounced trigger path: fire if the last backup (of any
             * kind) is older than WALLET_BACKUP_TRIGGER_MIN_INTERVAL_SEC.
             * Multiple triggers that arrive inside the window collapse
             * into this single run. */
            int64_t now_s = platform_time_wall_unix();
            if (last_ok == 0 ||
                now_s >= last_ok + WALLET_BACKUP_TRIGGER_MIN_INTERVAL_SEC) {
                (void)wallet_backup_now();
                wbs_supervisor_heartbeat();
                ran_this_tick = true;
                pthread_mutex_lock(&g_wbs.lock);
                g_wbs.total_trigger_runs++;
                pthread_mutex_unlock(&g_wbs.lock);
            }
        }

        if (ran_this_tick) {
            pthread_mutex_lock(&g_wbs.lock);
            g_wbs.key_change_pending = false;
            pthread_mutex_unlock(&g_wbs.lock);
        }

        /* Sleep in small increments so stop_requested is honoured
         * without waiting up to `interval` seconds. */
        wbs_supervisor_heartbeat();
        platform_sleep_ms(200);
    }

    pthread_mutex_lock(&g_wbs.lock);
    g_wbs.thread_running = false;
    pthread_mutex_unlock(&g_wbs.lock);
    return NULL;
}

struct zcl_result wallet_backup_start(const struct wallet_backup_config *cfg,
                          struct node_db *db)
{
    if (!cfg || !db || !cfg->backup_dir)
        return ZCL_ERR(-20, "start: NULL config, db, or backup_dir");

    /* Explicit encrypt without a password must fail loudly here —
     * silently falling back to plaintext would betray the operator's
     * stated intent. (config_defaults sets cfg->encrypt only when
     * WALLET_BACKUP_PASSWORD is non-empty, so this guard fires on
     * misconfigured direct callers — or on the OOM path above that
     * deliberately leaves encrypt=true with no password.
     * ZCL_SERVICE_OPTIONAL keeps it a kernel WARNING, not a boot
     * failure.) */
    if (cfg->encrypt && (!cfg->encrypt_password || !*cfg->encrypt_password)) {
        struct zcl_result r = ZCL_ERR(-24,
            "start: encrypt=true but encrypt_password is empty "
            "(set WALLET_BACKUP_PASSWORD)");
        LOG_WARN("wallet_backup", "%s", r.message);
        return r;
    }

    pthread_mutex_lock(&g_wbs.lock);
    if (g_wbs.thread_running) {
        pthread_mutex_unlock(&g_wbs.lock);
        return ZCL_OK;
    }

    /* Refuse to back up into the same datadir as the source — the
     * whole point is an *external* copy. We detect this by
     * comparing the backup_dir to the directory containing the
     * source db file. */
    char src_path[1024];
    if (wbs_source_path(db, src_path, sizeof(src_path))) {
        char src_dir[1024];
        snprintf(src_dir, sizeof(src_dir), "%s", src_path);
        char *slash = strrchr(src_dir, '/');
        if (slash) *slash = '\0';
        if (strcmp(src_dir, cfg->backup_dir) == 0) {
            struct zcl_result r = ZCL_ERR(-21,
                "start: refusing to back up into source dir %s", src_dir);
            pthread_mutex_unlock(&g_wbs.lock);
            return r;
        }
    }

    if (!wbs_ensure_backup_dir(cfg->backup_dir)) {
        struct zcl_result r = ZCL_ERR(-22,
                "start: cannot create backup dir %s", cfg->backup_dir);
        pthread_mutex_unlock(&g_wbs.lock);
        return r;
    }

    g_wbs.cfg = *cfg;
    g_wbs.db = db;
    g_wbs.stop_requested = false;
    g_wbs.thread_running = true;

    int rc = thread_registry_spawn_ex("zcl_wallet_bk", wbs_thread_fn, NULL,
                                       &g_wbs.thread);
    if (rc != 0) {
        g_wbs.thread_running = false;
        struct zcl_result r = ZCL_ERR(-23,
                "start: thread_registry_spawn_ex failed (%d)", rc);
        pthread_mutex_unlock(&g_wbs.lock);
        return r;
    }
    pthread_mutex_unlock(&g_wbs.lock);

    struct zcl_result sup_r = wbs_register_supervisor();
    if (!sup_r.ok) {
        wallet_backup_stop();
        return sup_r;
    }
    return ZCL_OK;
}

void wallet_backup_stop(void)
{
    pthread_t th;
    bool joinable = false;
    supervisor_child_id id = atomic_load(&g_wbs.supervisor_id);
    if (id != SUPERVISOR_INVALID_ID)
        supervisor_set_deadline(id, 0);
    pthread_mutex_lock(&g_wbs.lock);
    if (g_wbs.thread_running) {
        g_wbs.stop_requested = true;
        th = g_wbs.thread;
        joinable = true;
    }
    pthread_mutex_unlock(&g_wbs.lock);

    if (joinable) {
        pthread_join(th, NULL);
        pthread_mutex_lock(&g_wbs.lock);
        g_wbs.thread_running = false;
        g_wbs.stop_requested = false;
        g_wbs.db = NULL;
        g_wbs.key_change_pending = false;
        pthread_mutex_unlock(&g_wbs.lock);
    }
#ifdef ZCL_TESTING
    id = atomic_exchange(&g_wbs.supervisor_id, SUPERVISOR_INVALID_ID);
    if (id != SUPERVISOR_INVALID_ID)
        supervisor_unregister(id);
#endif
}

/* ── Event triggers (D4: plan §5.4) ─────────────────────────── */

void wallet_backup_service_on_key_change(void)
{
    pthread_mutex_lock(&g_wbs.lock);
    /* Count every call, even coalesced ones, for debugging /
     * test visibility. Only set the pending flag if the thread is
     * running — otherwise the next wallet_backup_start() will do a
     * first-run immediately and pick up the state anyway. */
    g_wbs.total_triggers++;
    if (g_wbs.thread_running) {
        g_wbs.key_change_pending = true;
    }
    pthread_mutex_unlock(&g_wbs.lock);
}

void wallet_backup_service_on_keypool_topup(void)
{
    wallet_backup_service_on_key_change();
}
