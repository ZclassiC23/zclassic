// wallet_backup_dump_state_json, implements the diagnostics_dump_fn typedef
// (CLAUDE.md "Adding state introspection": `bool <name>_dump_state_json(...)`)
// mandated by the g_dumpers[] dispatch table in
// app/controllers/src/diagnostics_registry.c; every other dumper in the
// codebase has the same bool signature for the same reason, so this is not
// a candidate for struct zcl_result conversion.

/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Wallet Backup Service — LIFECYCLE half: config, the background thread,
 * the status snapshot, the diagnostics dumper, and the supervisor liveness
 * contract. See the header for rationale.
 *
 * The one-shot snapshot primitive and its all-seven-tables verification
 * live in wallet_backup_run.c (declared in wallet_backup_internal.h);
 * rotation/listing in wallet_backup_rotation.c; the WBE1 crypto in
 * wallet_backup_crypto.c. The split happened when verification grew from
 * one table to seven and this file passed the 800-line shape ceiling.
 */

#include "platform/time_compat.h"
#include "services/wallet_backup_internal.h"
#include "services/wallet_backup_service.h"

#include "event/event.h"
#include "json/json.h"
#include "supervisors/domains.h"

#include <errno.h>
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

#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/supervisor.h"
#include "util/thread_registry.h"

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
    int     last_tables_verified;
    char    last_missing_tables[WBS_MISSING_TABLES_MAX];
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
    out->last_tables_verified = g_wbs.last_tables_verified;
    size_t n_tables = 0;
    (void)wallet_backup_tables(&n_tables);
    out->wallet_table_count = (int)n_tables;
    snprintf(out->last_missing_tables, sizeof(out->last_missing_tables), "%s",
             g_wbs.last_missing_tables);
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
    /* Verification breadth. last_tables_verified counts the wallet tables
     * whose backup row count matched the source on the last run; the rest
     * are named in last_missing_tables (absent from the SOURCE, so nothing
     * to copy — not a failure, but the operator is told rather than left to
     * discover it at restore time). */
    json_push_kv_int(out, "last_tables_verified", st.last_tables_verified);
    json_push_kv_int(out, "wallet_table_count", st.wallet_table_count);
    json_push_kv_str(out, "last_missing_tables", st.last_missing_tables);
    json_push_kv_str(out, "last_path", st.last_path);
    json_push_kv_str(out, "last_error", st.last_error);
    return true;
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
    struct wbs_verify_out vout;
    struct zcl_result res = wbs_run_once_impl(g_wbs.cfg.backup_dir, g_wbs.db,
                                      path, sizeof(path),
                                      &key_count,
                                      err, sizeof(err), &vout);
    bool ok = res.ok;
    int64_t elapsed = platform_time_monotonic_ms() - started_ms;

    g_wbs.last_tables_verified = vout.tables_verified;
    snprintf(g_wbs.last_missing_tables, sizeof(g_wbs.last_missing_tables),
             "%s", vout.missing);

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
    if (wbs_source_path(db, src_path, sizeof(src_path)).ok) {
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

    struct zcl_result dir_r = wbs_ensure_backup_dir(cfg->backup_dir);
    if (!dir_r.ok) {
        struct zcl_result r = ZCL_ERR(-22, "start: %s", dir_r.message);
        pthread_mutex_unlock(&g_wbs.lock);
        return r;
    }

    g_wbs.cfg = *cfg;
    g_wbs.db = db;
    g_wbs.stop_requested = false;
    g_wbs.thread_running = true;

    int rc = thread_registry_spawn("zcl_wallet_bk", wbs_thread_fn, NULL,
                                       &g_wbs.thread);
    if (rc != 0) {
        g_wbs.thread_running = false;
        struct zcl_result r = ZCL_ERR(-23,
                "start: thread_registry_spawn failed (%d)", rc);
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
