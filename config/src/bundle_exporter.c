/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * bundle_exporter — the STANDING live consensus-state bundle exporter (lane C1
 * of the Instant-Sync program). See config/include/config/bundle_exporter.h for
 * the contract and the precise provenance claim this proves.
 *
 * Structure mirrors app/services/src/wallet_backup_service.c: a dedicated worker
 * pthread runs the heavy job (the export walks millions of rows, so it must NOT
 * run on the supervisor's on_tick thread), and a supervised liveness_contract
 * heartbeats via supervisor_tick from the worker loop. The contract uses
 * period_secs=0 / deadline_secs=0 / progress_max_quiet_us=0: best-effort, no
 * stall — a degraded exporter is a named dumpstate degradation, never a boot
 * blocker.
 *
 * FAIL-SAFE: bundle_exporter_start NEVER returns false for a qualification or
 * producer-session failure; it records a dumpstate-visible degradation reason
 * and still arms the worker. It returns false only on a hard wiring error (NULL
 * progress handle / datadir), which the boot caller ignores anyway.
 */

#include "config/bundle_exporter.h"

#include "config/consensus_state_producer_receipt.h"
#include "config/consensus_state_snapshot_export.h"
#include "config/consensus_state_bundle_validate.h"

#include "storage/progress_store.h"
#include "storage/coins_kv.h"
#include "storage/coins_ram.h"
#include "storage/consensus_state_bundle_codec.h"

#include "jobs/tip_finalize_stage.h"

#include "services/sync_trust_policy.h"

#include "kernel/service_kernel.h"
#include "util/supervisor.h"
#include "supervisors/domains.h"
#include "util/thread_registry.h"
#include "util/clientversion.h"
#include "util/log_macros.h"

#include "json/json.h"
#include "core/utiltime.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── Filename convention ────────────────────────────────────────── */

#define BX_BUNDLE_PREFIX "consensus-state-bundle-"
#define BX_BUNDLE_SUFFIX ".sqlite"
#define BX_MAX_GENERATIONS 256

/* ── Module state ───────────────────────────────────────────────── */

static struct {
    pthread_mutex_t lock;

    sqlite3 *pdb;                 /* owned progress.kv handle (borrowed) */
    char     datadir[1024];
    char     bundles_dir[1100];   /* "<datadir>/bundles" */

    bool     session_open;        /* producer receipt session is held open */
    bool     qualified;           /* provenance qualified at start */
    char     degradation_reason[256];
    char     last_refusal[256];

    int64_t  every_blocks;        /* ZCL_BUNDLE_EXPORT_EVERY_BLOCKS (>=1) */
    int      keep;                /* ZCL_BUNDLE_EXPORT_KEEP (>=1) */
    int64_t  tick_secs;           /* ZCL_BUNDLE_EXPORT_TICK_SECS worker poll */

    pthread_t worker;
    bool      worker_running;     /* true iff `worker` is joinable */
} g_bx = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static _Atomic int32_t g_bx_last_export_height   = -1;
static _Atomic int64_t g_bx_last_export_time_us  = 0;
static _Atomic int64_t g_bx_last_export_duration_us = 0;
static _Atomic int64_t g_bx_exports_ok           = 0;
static _Atomic int64_t g_bx_exports_failed       = 0;

static atomic_bool g_bx_running = false;
static _Atomic supervisor_child_id g_bx_supervisor_id = SUPERVISOR_INVALID_ID;
static struct liveness_contract g_bx_contract;

/* ── Small helpers ──────────────────────────────────────────────── */

static void bx_note_refusal(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_bx.lock);
    snprintf(g_bx.last_refusal, sizeof g_bx.last_refusal, "%s", buf);
    pthread_mutex_unlock(&g_bx.lock);
}

static int64_t bx_env_i64(const char *name, int64_t dflt)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return dflt;
    errno = 0;
    char *end = NULL;
    long long parsed = strtoll(v, &end, 10);
    if (errno != 0 || end == v || (end && *end))
        return dflt;
    return (int64_t)parsed;
}

/* True iff `s` is the exact lowercase SHA-256 source identity baked by the
 * canonical build. Git object IDs intentionally stay outside the sovereign
 * executable (clientversion.c); producer receipts bind this same 32-byte
 * source identity, so the exporter must gate on it rather than on the legacy
 * external-only Git trace string. */
static bool bx_is_exact_source_id(const char *s)
{
    if (!s)
        return false;
    size_t n = strlen(s);
    if (n != 64)
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

/* Parse "consensus-state-bundle-<N>.sqlite" -> height. Returns false for any
 * other name. */
static bool bx_parse_bundle_height(const char *name, long *out)
{
    size_t plen = strlen(BX_BUNDLE_PREFIX);
    size_t slen = strlen(BX_BUNDLE_SUFFIX);
    size_t nlen = strlen(name);
    if (nlen <= plen + slen)
        return false;
    if (strncmp(name, BX_BUNDLE_PREFIX, plen) != 0)
        return false;
    if (strcmp(name + nlen - slen, BX_BUNDLE_SUFFIX) != 0)
        return false;
    const char *digits = name + plen;
    size_t dcount = nlen - plen - slen;
    if (dcount == 0 || dcount >= 32)
        return false;
    for (size_t i = 0; i < dcount; i++)
        if (!isdigit((unsigned char)digits[i]))
            return false;
    char buf[32];
    memcpy(buf, digits, dcount);
    buf[dcount] = '\0';
    errno = 0;
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if (errno != 0 || (end && *end) || v < 0)
        return false;
    *out = v;
    return true;
}

/* Highest bundle height present in `dir`, or -1 when none / dir unreadable. */
static int32_t bx_scan_max_height(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    long max = -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        long h;
        if (bx_parse_bundle_height(e->d_name, &h) && h > max)
            max = h;
    }
    closedir(d);
    return (int32_t)max;
}

/* The qualification gate — ALL must hold or the producer session stays closed
 * and `reason` names the first failing rung. The coins predicates read the
 * shared singleton progress.kv handle, which is only safe under
 * progress_store_tx_lock (SQLite statements on one connection must be
 * serialized across threads; coins_kv_is_proven_authority does NOT self-lock).
 * The lock is recursive, so coins_kv_contains_refold_marker's internal acquire
 * nests cleanly. Callers must NOT hold g_bx.lock here — this module keeps
 * g_bx.lock and progress_store_tx_lock strictly disjoint. */
static bool bx_qualified(sqlite3 *pdb, char *reason, size_t cap)
{
    if (coins_ram_active()) {
        snprintf(reason, cap, "in-RAM coins overlay active");
        return false;
    }
    progress_store_tx_lock();
    bool proven = coins_kv_is_proven_authority(pdb, NULL);
    bool refolded = proven && coins_kv_contains_refold_marker(pdb);
    progress_store_tx_unlock();
    /* Route ONLY the provenance-bit portion of the gate through the central
     * trust table (services/sync_trust_policy.h). EXPORT_BUNDLE is granted
     * exactly in the X states (HEADERS_VERIFIED, SOVEREIGN), i.e. iff
     * (proven && refold); it is independent of the self_derived bit, so that
     * input is immaterial here and passed false. X = proven && refolded
     * (refolded already carries proven), so the derived answer is identical to
     * the old `!proven || !refolded` gate. Every other rung (coins_ram above,
     * exact source-identity rung below) stays exactly where it is and is never
     * weakened. */
    if (!sync_trust_cap_allowed(
            sync_trust_derive(proven, refolded, /*self_derived=*/false),
            SYNC_CAP_EXPORT_BUNDLE)) {
        snprintf(reason, cap, "%s",
                 !proven ? "coins not proven authority"
                         : "coins lacks self-folded refold marker");
        return false;
    }
    if (!bx_is_exact_source_id(zcl_build_source_id_sha256())) {
        snprintf(reason, cap, "build has no exact source identity; unstamped");
        return false;
    }
    if (cap)
        reason[0] = '\0';
    return true;
}

#ifdef ZCL_TESTING
bool bundle_exporter_source_identity_is_exact_for_test(const char *source_id)
{
    return bx_is_exact_source_id(source_id);
}
#endif

/* Belt+braces re-validation of a sealed bundle by path (read-only, immutable).
 * The export path already reopens+validates before linking; rotation only ever
 * deletes an OLDER generation after confirming the NEWEST still validates. */
static bool bx_validate_bundle_path(const char *path)
{
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db,
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL)
        != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return false;
    }
    (void)sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL);
    (void)sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, NULL);
    (void)sqlite3_exec(db, "PRAGMA query_only=ON", NULL, NULL, NULL);

    struct consensus_state_bundle_manifest manifest;
    struct consensus_state_install_result validation;
    memset(&manifest, 0, sizeof manifest);
    memset(&validation, 0, sizeof validation);
    bool ok = consensus_state_bundle_validate(db, &manifest, &validation);
    sqlite3_close(db);
    return ok;
}

/* ── Rotation ───────────────────────────────────────────────────── */

struct bx_gen {
    long h;
    char name[128];
};

static int bx_gen_cmp_desc(const void *a, const void *b)
{
    const struct bx_gen *x = a;
    const struct bx_gen *y = b;
    if (x->h < y->h)
        return 1;
    if (x->h > y->h)
        return -1;
    return 0;
}

/* Keep the `keep` newest bundles; delete older ones — but only after the newest
 * bundle independently re-validates, and delete nothing if it does not. */
static void bx_rotate(const char *dir, int keep)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct bx_gen gens[BX_MAX_GENERATIONS];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < BX_MAX_GENERATIONS) {
        long h;
        if (bx_parse_bundle_height(e->d_name, &h)) {
            gens[n].h = h;
            snprintf(gens[n].name, sizeof gens[n].name, "%s", e->d_name);
            n++;
        }
    }
    closedir(d);
    if (n <= keep)
        return;
    qsort(gens, (size_t)n, sizeof gens[0], bx_gen_cmp_desc);

    char newest_path[1300];
    snprintf(newest_path, sizeof newest_path, "%s/%s", dir, gens[0].name);
    if (!bx_validate_bundle_path(newest_path)) {
        LOG_WARN("bundle_exporter",
                 "rotation: newest bundle %s failed re-validation; "
                 "deleting nothing", gens[0].name);
        return;
    }

    int dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) {
        LOG_WARN("bundle_exporter", "rotation: open bundles dir failed: %s",
                 strerror(errno));
        return;
    }
    for (int i = keep; i < n; i++) {
        if (unlinkat(dir_fd, gens[i].name, 0) != 0)
            LOG_WARN("bundle_exporter", "rotation: unlink %s failed: %s",
                     gens[i].name, strerror(errno));
    }
    close(dir_fd);
}

/* ── One export attempt ─────────────────────────────────────────── */

static void bx_try_export_once(void)
{
    pthread_mutex_lock(&g_bx.lock);
    sqlite3 *pdb = g_bx.pdb;
    int64_t every = g_bx.every_blocks;
    int keep = g_bx.keep;
    char bundles_dir[1100];
    snprintf(bundles_dir, sizeof bundles_dir, "%s", g_bx.bundles_dir);
    pthread_mutex_unlock(&g_bx.lock);

    /* Belt+braces: the proof independently refuses while the overlay is live,
     * but never even finalize the receipt against a mutable in-RAM view. */
    if (coins_ram_active()) {
        bx_note_refusal("in-RAM overlay active");
        return;
    }

    /* The durable-tip read touches the shared singleton handle and does NOT
     * self-lock; serialize it with the reducer's batches. Brief: two indexed
     * lookups. (The receipt finalize + the snapshot pin below each take the
     * lock themselves.) */
    int h = -1;
    uint8_t hash[32];
    progress_store_tx_lock();
    bool tip_ok = tip_finalize_stage_resolve_durable_tip(pdb, &h, hash);
    progress_store_tx_unlock();
    if (!tip_ok || h < 0) {
        bx_note_refusal("durable tip unavailable");
        return;
    }

    int32_t last = atomic_load(&g_bx_last_export_height);
    if ((int64_t)h - (int64_t)last < every)
        return; /* not due yet */

    char name[128];
    snprintf(name, sizeof name, BX_BUNDLE_PREFIX "%d" BX_BUNDLE_SUFFIX, h);

    /* Already exported this generation? Treat as done. */
    char full[1300];
    snprintf(full, sizeof full, "%s/%s", bundles_dir, name);
    struct stat stx;
    if (stat(full, &stx) == 0) {
        atomic_store(&g_bx_last_export_height, h);
        return;
    }

    /* Roll the source receipt forward to the current durable tip. Monotonic:
     * an equal height is idempotent, a lower one is refused inside finalize. */
    char err[256] = "";
    if (!consensus_state_producer_receipt_finalize(pdb, h, hash,
                                                   err, sizeof err)) {
        bx_note_refusal("%s", err[0] ? err : "receipt finalize failed");
        atomic_fetch_add(&g_bx_exports_failed, 1);
        return;
    }

    int dir_fd = open(bundles_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) {
        bx_note_refusal("open bundles dir failed: %s", strerror(errno));
        atomic_fetch_add(&g_bx_exports_failed, 1);
        return;
    }

    struct consensus_state_snapshot_export_request req;
    memset(&req, 0, sizeof req);
    req.output_dir_fd = dir_fd;
    req.output_name = name;
    req.expected_height = h;
    memcpy(req.expected_block_hash, hash, 32);

    struct consensus_state_export_result res;
    memset(&res, 0, sizeof res);
    int64_t t0 = GetTimeMicros();
    bool ok = consensus_state_snapshot_export_from_progress_snapshot(&req, &res);
    close(dir_fd);
    int64_t dur = GetTimeMicros() - t0;

    if (ok && res.status == CONSENSUS_EXPORT_EXPORTED) {
        atomic_store(&g_bx_last_export_height, h);
        atomic_store(&g_bx_last_export_time_us, GetTimeMicros());
        atomic_store(&g_bx_last_export_duration_us, dur);
        atomic_fetch_add(&g_bx_exports_ok, 1);
        pthread_mutex_lock(&g_bx.lock);
        g_bx.last_refusal[0] = '\0';
        pthread_mutex_unlock(&g_bx.lock);
        LOG_INFO("bundle_exporter",
                 "exported %s height=%d duration_us=%lld",
                 name, h, (long long)dur);
        bx_rotate(bundles_dir, keep);
    } else {
        bx_note_refusal("%s", res.reason[0] ? res.reason : "export refused");
        atomic_fetch_add(&g_bx_exports_failed, 1);
        LOG_WARN("bundle_exporter",
                 "export refused height=%d status=%d reason=%s",
                 h, (int)res.status,
                 res.reason[0] ? res.reason : "(none)");
    }
}

/* ── Worker loop ────────────────────────────────────────────────── */

static void *bx_worker_main(void *arg)
{
    (void)arg;
    supervisor_child_id id = atomic_load(&g_bx_supervisor_id);

    while (atomic_load(&g_bx_running)) {
        if (id != SUPERVISOR_INVALID_ID)
            supervisor_tick(id); /* heartbeat */

        pthread_mutex_lock(&g_bx.lock);
        int64_t tick = g_bx.tick_secs;
        pthread_mutex_unlock(&g_bx.lock);
        if (tick < 1)
            tick = 1;

        /* Sleep in <=1s increments so _stop stays responsive. */
        for (int64_t i = 0; i < tick && atomic_load(&g_bx_running); i++) {
            struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
            nanosleep(&ts, NULL);
        }
        if (!atomic_load(&g_bx_running))
            break;

        pthread_mutex_lock(&g_bx.lock);
        bool session = g_bx.session_open;
        pthread_mutex_unlock(&g_bx.lock);
        if (!session)
            continue; /* degraded — keep heartbeating, never export */

        bx_try_export_once();
    }
    return NULL;
}

/* ── Lifecycle ──────────────────────────────────────────────────── */

bool bundle_exporter_start(sqlite3 *pdb, const char *datadir)
{
    if (!pdb || !datadir || !*datadir)
        LOG_FAIL("bundle_exporter",
                 "start: NULL progress handle or datadir — not armed");

    pthread_mutex_lock(&g_bx.lock);
    if (atomic_load(&g_bx_running)) {
        pthread_mutex_unlock(&g_bx.lock);
        return true; /* idempotent */
    }

    g_bx.pdb = pdb;
    snprintf(g_bx.datadir, sizeof g_bx.datadir, "%s", datadir);
    snprintf(g_bx.bundles_dir, sizeof g_bx.bundles_dir, "%s/bundles", datadir);

    g_bx.every_blocks = bx_env_i64("ZCL_BUNDLE_EXPORT_EVERY_BLOCKS", 5000);
    if (g_bx.every_blocks < 1)
        g_bx.every_blocks = 1;
    g_bx.keep = (int)bx_env_i64("ZCL_BUNDLE_EXPORT_KEEP", 3);
    if (g_bx.keep < 1)
        g_bx.keep = 1;
    g_bx.tick_secs = bx_env_i64("ZCL_BUNDLE_EXPORT_TICK_SECS", 30);
    if (g_bx.tick_secs < 1)
        g_bx.tick_secs = 1;

    if (mkdir(g_bx.bundles_dir, 0700) != 0 && errno != EEXIST)
        LOG_WARN("bundle_exporter", "mkdir %s failed: %s",
                 g_bx.bundles_dir, strerror(errno));

    char bundles_dir[1100];
    int64_t every = g_bx.every_blocks;
    int keep = g_bx.keep;
    int64_t tick = g_bx.tick_secs;
    snprintf(bundles_dir, sizeof bundles_dir, "%s", g_bx.bundles_dir);
    pthread_mutex_unlock(&g_bx.lock);

    /* Qualify + (maybe) open the producer session — OUTSIDE g_bx.lock: both
     * bx_qualified and receipt_begin take progress_store_tx_lock, and this
     * module keeps the two locks strictly disjoint (no nesting order exists,
     * so no lock-order edge). Single-threaded here: the worker is not spawned
     * yet and g_bx_running is still false, so the fields written below cannot
     * be observed concurrently. NEVER fail here. */
    bool session_open = false;
    bool qualified = false;
    char degradation[256] = "";
    char reason[256] = "";
    if (bx_qualified(pdb, reason, sizeof reason)) {
        char err[256] = "";
        if (consensus_state_producer_receipt_begin(
                pdb, CONSENSUS_STATE_VALIDATION_FULL, err, sizeof err)) {
            session_open = true;
            qualified = true;
            LOG_INFO("bundle_exporter",
                     "producer session opened (qualified); standing exporter "
                     "armed every=%lld keep=%d tick=%llds",
                     (long long)every, keep, (long long)tick);
        } else {
            snprintf(degradation, sizeof degradation, "%s",
                     err[0] ? err : "producer session refused");
            LOG_WARN("bundle_exporter",
                     "producer session refused: %s (degraded, still armed)",
                     degradation);
        }
    } else {
        snprintf(degradation, sizeof degradation, "%s", reason);
        LOG_WARN("bundle_exporter",
                 "not qualified: %s (degraded, still armed)", reason);
    }

    pthread_mutex_lock(&g_bx.lock);
    g_bx.session_open = session_open;
    g_bx.qualified = qualified;
    snprintf(g_bx.degradation_reason, sizeof g_bx.degradation_reason, "%s",
             degradation);
    pthread_mutex_unlock(&g_bx.lock);

    atomic_store(&g_bx_last_export_height, bx_scan_max_height(bundles_dir));

    /* Register the supervised (best-effort, no-stall) contract. */
    if (supervisor_start()) {
        liveness_contract_init(&g_bx_contract, "ops.bundle_exporter");
        atomic_store(&g_bx_contract.period_secs, 0);
        atomic_store(&g_bx_contract.deadline_secs, 0);
        atomic_store(&g_bx_contract.progress_max_quiet_us, 0);
        supervisor_domains_init();
        supervisor_child_id id =
            supervisor_register_in_domain(g_op_sup, &g_bx_contract);
        atomic_store(&g_bx_supervisor_id, id);
        if (id != SUPERVISOR_INVALID_ID)
            supervisor_tick(id);
    } else {
        LOG_WARN("bundle_exporter",
                 "supervisor_start failed; exporter runs unsupervised");
    }

    atomic_store(&g_bx_running, true);
    int rc = thread_registry_spawn("zcl_bundle_exp", bx_worker_main, NULL,
                                   &g_bx.worker);
    if (rc != 0) {
        atomic_store(&g_bx_running, false);
        pthread_mutex_lock(&g_bx.lock);
        g_bx.worker_running = false;
        pthread_mutex_unlock(&g_bx.lock);
        LOG_WARN("bundle_exporter",
                 "worker spawn failed (%d); exporter not running", rc);
        return true; /* fail-safe: never block boot */
    }
    pthread_mutex_lock(&g_bx.lock);
    g_bx.worker_running = true;
    pthread_mutex_unlock(&g_bx.lock);
    return true;
}

void bundle_exporter_stop(void)
{
    atomic_store(&g_bx_running, false);

    pthread_t th;
    bool joinable = false;
    pthread_mutex_lock(&g_bx.lock);
    if (g_bx.worker_running) {
        th = g_bx.worker;
        joinable = true;
        g_bx.worker_running = false;
    }
    pthread_mutex_unlock(&g_bx.lock);
    if (joinable)
        pthread_join(th, NULL);

    atomic_store(&g_bx_contract.completed, true);
    supervisor_child_id id = atomic_load(&g_bx_supervisor_id);
    if (id != SUPERVISOR_INVALID_ID)
        supervisor_child_complete(id);
}

/* ── Boot service registration ──────────────────────────────────── */

static bool bx_service_start(void *ctx)
{
    const char *datadir = ctx;
    if (!datadir)
        return true;
    (void)bundle_exporter_start(progress_store_db(), datadir);
    return true;
}

static void bx_service_stop(void *ctx)
{
    (void)ctx;
    bundle_exporter_stop();
}

bool bundle_exporter_register_service(struct zcl_service_kernel *kernel,
                                      const char *datadir)
{
    const struct zcl_service_spec spec = {
        .name = "bundle_exporter",
        .start = bx_service_start,
        .stop = bx_service_stop,
        .ctx = (void *)datadir,
        .flags = ZCL_SERVICE_OPTIONAL,
    };
    return zcl_service_kernel_register(kernel, &spec);
}

/* ── `zclassic23 dumpstate bundle_exporter` ────────────────────── */

bool bundle_exporter_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;
    json_set_object(out);

    /* Atomics read directly; string fields under a brief lock. */
    char degradation_reason[256];
    char last_refusal[256];
    char bundles_dir[1100];
    bool session_open, qualified;
    int64_t every_blocks;
    int keep;
    pthread_mutex_lock(&g_bx.lock);
    session_open = g_bx.session_open;
    qualified = g_bx.qualified;
    every_blocks = g_bx.every_blocks;
    keep = g_bx.keep;
    snprintf(degradation_reason, sizeof degradation_reason, "%s",
             g_bx.degradation_reason);
    snprintf(last_refusal, sizeof last_refusal, "%s", g_bx.last_refusal);
    snprintf(bundles_dir, sizeof bundles_dir, "%s", g_bx.bundles_dir);
    pthread_mutex_unlock(&g_bx.lock);

    json_push_kv_bool(out, "session_open", session_open);
    json_push_kv_bool(out, "qualified", qualified);
    json_push_kv_str(out, "degradation_reason", degradation_reason);
    json_push_kv_str(out, "last_refusal", last_refusal);
    json_push_kv_int(out, "last_export_height",
                     atomic_load(&g_bx_last_export_height));
    json_push_kv_int(out, "last_export_time_us",
                     atomic_load(&g_bx_last_export_time_us));
    json_push_kv_int(out, "last_export_duration_us",
                     atomic_load(&g_bx_last_export_duration_us));
    json_push_kv_int(out, "exports_ok", atomic_load(&g_bx_exports_ok));
    json_push_kv_int(out, "exports_failed", atomic_load(&g_bx_exports_failed));
    json_push_kv_int(out, "every_blocks", every_blocks);
    json_push_kv_int(out, "keep", keep);
    json_push_kv_str(out, "bundles_dir", bundles_dir);

    /* Generations currently on disk (heights present), newest first. */
    struct bx_gen gens[BX_MAX_GENERATIONS];
    int n = 0;
    DIR *d = opendir(bundles_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && n < BX_MAX_GENERATIONS) {
            long h;
            if (bx_parse_bundle_height(e->d_name, &h)) {
                gens[n].h = h;
                gens[n].name[0] = '\0';
                n++;
            }
        }
        closedir(d);
    }
    qsort(gens, (size_t)n, sizeof gens[0], bx_gen_cmp_desc);

    struct json_value arr;
    json_init(&arr);
    json_set_array(&arr);
    for (int i = 0; i < n; i++) {
        struct json_value item;
        json_init(&item);
        json_set_int(&item, gens[i].h);
        json_push_back(&arr, &item);
        json_free(&item);
    }
    json_push_kv(out, "generations", &arr);
    json_free(&arr);

    return true;
}
