/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Disk Monitor — see header for rationale.
 *
 * Uses `statvfs` for portable free-space queries. Polls in a
 * background pthread with small sleep increments so stop()
 * returns promptly.
 *
 * State transitions are edge-triggered: we only emit an event
 * when the level *changes*. That keeps EV_DISK_LOW from flooding
 * the log every 60 seconds on a sustained low-disk situation.
 */

#include "platform/time_compat.h"
#include "services/disk_monitor.h"

#include "event/event.h"
#include "json/json.h"
#include "supervisors/domains.h"
#include "util/supervisor.h"
#include "util/thread_registry.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>

#include "util/log_macros.h"

/* Supervisor deadline (sec). The loop polls every poll_seconds (default
 * 60) and heartbeats at the top of each sub-second wake, so 3x the
 * default poll gives ample slack before a genuine wedge fires. */
#define DISK_MONITOR_SUPERVISOR_DEADLINE_SEC 180

/* ── Module state ───────────────────────────────────────────── */

struct disk_monitor_state {
    pthread_mutex_t lock;
    pthread_t       thread;
    bool            thread_running;
    bool            stop_requested;

    struct disk_monitor_config cfg;

    /* Resolved thresholds (after filling in defaults). */
    int64_t warn_free_bytes;
    int64_t refuse_free_bytes;
    int     poll_seconds;

    /* Last poll observation. */
    enum disk_monitor_level last_level;
    int64_t                 last_free_bytes;
    int64_t                 last_poll_unix;

    /* Hot-path lock-free flag mirroring last_level. */
    _Atomic int atomic_level;

    /* Supervisor liveness (Round 5). loop_ticks advances once per
     * outer-loop wake so the supervisor sees forward progress even
     * between the 60 s poll boundaries. */
    _Atomic supervisor_child_id supervisor_id;
    _Atomic int64_t             loop_ticks;
};

static struct disk_monitor_state g_dm = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .last_free_bytes = -1,
    .atomic_level = DISK_MONITOR_OK,
    .supervisor_id = SUPERVISOR_INVALID_ID,
};

static struct liveness_contract g_dm_contract;

/* ── Supervisor liveness ────────────────────────────────────── */

static void dm_supervisor_heartbeat(void)
{
    supervisor_child_id id = atomic_load(&g_dm.supervisor_id);
    if (id == SUPERVISOR_INVALID_ID)
        return;
    supervisor_tick(id);
    supervisor_progress(id, atomic_load(&g_dm.loop_ticks));
}

static void dm_on_stall(struct liveness_contract *c)
{
    const char *reason = c
        ? supervisor_stall_reason_name(
              (enum supervisor_stall_reason)atomic_load(&c->stall_reason))
        : "unknown";
    LOG_WARN("disk_monitor",
             "[disk] supervisor stall reason=%s ticks=%lld level=%d",
             reason, (long long)atomic_load(&g_dm.loop_ticks),
             (int)atomic_load(&g_dm.atomic_level));
}

static struct zcl_result dm_register_supervisor(void)
{
    if (!supervisor_start())
        return ZCL_ERR(-5, "disk_monitor: supervisor_start failed");

    supervisor_child_id id = atomic_load(&g_dm.supervisor_id);
    if (id != SUPERVISOR_INVALID_ID) {
        supervisor_set_deadline(id, DISK_MONITOR_SUPERVISOR_DEADLINE_SEC);
        supervisor_progress(id, atomic_load(&g_dm.loop_ticks));
        supervisor_tick(id);
        return ZCL_OK;
    }

    liveness_contract_init(&g_dm_contract, "op.disk_monitor");
    atomic_store(&g_dm_contract.period_secs, 0);
    atomic_store(&g_dm_contract.deadline_secs,
                 DISK_MONITOR_SUPERVISOR_DEADLINE_SEC);
    atomic_store(&g_dm_contract.progress_max_quiet_us, 0);
    g_dm_contract.on_stall = dm_on_stall;

    supervisor_domains_init();
    id = supervisor_register_in_domain(g_op_sup, &g_dm_contract);
    if (id == SUPERVISOR_INVALID_ID)
        return ZCL_ERR(-6, "disk_monitor: supervisor_register failed");
    atomic_store(&g_dm.supervisor_id, id);
    supervisor_progress(id, atomic_load(&g_dm.loop_ticks));
    supervisor_tick(id);
    return ZCL_OK;
}

/* ── Defaults ───────────────────────────────────────────────── */

void disk_monitor_config_defaults(struct disk_monitor_config *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->warn_free_bytes   = DISK_MONITOR_DEFAULT_WARN_BYTES;
    cfg->refuse_free_bytes = DISK_MONITOR_DEFAULT_REFUSE_BYTES;
    cfg->poll_seconds      = DISK_MONITOR_DEFAULT_POLL_SECONDS;
}

/* ── Primitive ──────────────────────────────────────────────── */

int64_t disk_monitor_free_bytes(const char *path)
{
    if (!path || !*path) {
        fprintf(stderr, "[disk] %s: path is NULL or empty\n", __func__);
        return -1; // raw-return-ok:logged-above
    }
    struct statvfs st;
    if (statvfs(path, &st) != 0) {
        fprintf(stderr, "[disk] %s: statvfs failed for '%s': %s\n",
                __func__, path, strerror(errno));
        return -1; // raw-return-ok:logged-above
    }
    /* `f_bavail` is the blocks available to unprivileged users —
     * the right number to compare against a "should I keep
     * writing?" threshold. Multiplied by `f_frsize` which is the
     * fundamental block size (not `f_bsize` which can differ). */
    return (int64_t)st.f_bavail * (int64_t)st.f_frsize;
}

/* ── Classification ─────────────────────────────────────────── */

static enum disk_monitor_level
dm_classify(int64_t free_bytes, int64_t refuse, int64_t warn)
{
    if (free_bytes < 0)         return DISK_MONITOR_OK; /* unknown */
    if (free_bytes < refuse)    return DISK_MONITOR_CRITICAL;
    if (free_bytes < warn)      return DISK_MONITOR_LOW;
    return DISK_MONITOR_OK;
}

static const char *dm_level_name(enum disk_monitor_level lvl)
{
    switch (lvl) {
        case DISK_MONITOR_OK:       return "ok";
        case DISK_MONITOR_LOW:      return "low";
        case DISK_MONITOR_CRITICAL: return "critical";
        default:                    return "unknown";
    }
}

/* Assumes g_dm.lock is held. */
static void dm_run_one_locked(void)
{
    int64_t free_b = disk_monitor_free_bytes(g_dm.cfg.datadir);
    enum disk_monitor_level new_level =
        dm_classify(free_b, g_dm.refuse_free_bytes, g_dm.warn_free_bytes);

    g_dm.last_free_bytes = free_b;
    g_dm.last_poll_unix  = (int64_t)platform_time_wall_time_t();

    enum disk_monitor_level prev = g_dm.last_level;
    g_dm.last_level = new_level;
    atomic_store(&g_dm.atomic_level, (int)new_level);

    if (new_level == prev) return; /* edge-triggered only */

    switch (new_level) {
        case DISK_MONITOR_OK:
            event_emitf(EV_DISK_OK, 0,
                        "path=%s free=%" PRId64 " prev=%s",
                        g_dm.cfg.datadir ? g_dm.cfg.datadir : "",
                        free_b, dm_level_name(prev));
            break;
        case DISK_MONITOR_LOW:
            event_emitf(EV_DISK_LOW, 0,
                        "path=%s free=%" PRId64 " warn=%" PRId64 " prev=%s",
                        g_dm.cfg.datadir ? g_dm.cfg.datadir : "",
                        free_b, g_dm.warn_free_bytes,
                        dm_level_name(prev));
            break;
        case DISK_MONITOR_CRITICAL:
            event_emitf(EV_DISK_CRITICAL, 0,
                        "path=%s free=%" PRId64 " refuse=%" PRId64 " prev=%s",
                        g_dm.cfg.datadir ? g_dm.cfg.datadir : "",
                        free_b, g_dm.refuse_free_bytes,
                        dm_level_name(prev));
            break;
    }
}

void disk_monitor_poll_now(void)
{
    pthread_mutex_lock(&g_dm.lock);
    dm_run_one_locked();
    pthread_mutex_unlock(&g_dm.lock);
}

/* ── Thread loop ────────────────────────────────────────────── */

static void *dm_thread_fn(void *arg)
{
    (void)arg;
    int64_t next_at_ms;
    int poll_seconds;

    pthread_mutex_lock(&g_dm.lock);
    poll_seconds = g_dm.poll_seconds;
    pthread_mutex_unlock(&g_dm.lock);

    struct timespec now;
    platform_time_monotonic_timespec(&now);
    int64_t now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    next_at_ms = now_ms + (int64_t)poll_seconds * 1000;

    while (true) {
        pthread_mutex_lock(&g_dm.lock);
        bool stop = g_dm.stop_requested;
        pthread_mutex_unlock(&g_dm.lock);
        if (stop) break;

        atomic_fetch_add(&g_dm.loop_ticks, 1);
        dm_supervisor_heartbeat();

        platform_time_monotonic_timespec(&now);
        now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
        if (now_ms >= next_at_ms) {
            pthread_mutex_lock(&g_dm.lock);
            dm_run_one_locked();
            poll_seconds = g_dm.poll_seconds;
            pthread_mutex_unlock(&g_dm.lock);
            next_at_ms = now_ms + (int64_t)poll_seconds * 1000;
        }
        platform_sleep_ms(100);
    }

    pthread_mutex_lock(&g_dm.lock);
    g_dm.thread_running = false;
    pthread_mutex_unlock(&g_dm.lock);
    return NULL;
}

/* ── Lifecycle ──────────────────────────────────────────────── */

struct zcl_result disk_monitor_start(const struct disk_monitor_config *cfg)
{
    if (!cfg || !cfg->datadir)
        return ZCL_ERR(-1, "start called with null config or datadir");

    pthread_mutex_lock(&g_dm.lock);
    if (g_dm.thread_running) {
        pthread_mutex_unlock(&g_dm.lock);
        return ZCL_ERR(-2, "start called but monitor thread already running");
    }

    g_dm.cfg = *cfg;
    g_dm.warn_free_bytes =
        cfg->warn_free_bytes   > 0 ? cfg->warn_free_bytes
                                   : DISK_MONITOR_DEFAULT_WARN_BYTES;
    g_dm.refuse_free_bytes =
        cfg->refuse_free_bytes > 0 ? cfg->refuse_free_bytes
                                   : DISK_MONITOR_DEFAULT_REFUSE_BYTES;
    g_dm.poll_seconds =
        cfg->poll_seconds      > 0 ? cfg->poll_seconds
                                   : DISK_MONITOR_DEFAULT_POLL_SECONDS;

    /* Reject datadir we can't even stat — no point starting a
     * thread that will just emit -1 forever. */
    if (disk_monitor_free_bytes(cfg->datadir) < 0) {
        pthread_mutex_unlock(&g_dm.lock);
        return ZCL_ERR(-3, "cannot stat datadir %s", cfg->datadir);
    }

    /* Synchronous first poll so callers know the level before
     * this function returns (and so tests don't race the thread). */
    dm_run_one_locked();

    g_dm.stop_requested = false;
    g_dm.thread_running = true;
    int rc = thread_registry_spawn_ex("zcl_disk_monitor", dm_thread_fn, NULL,
                                       &g_dm.thread);
    if (rc != 0) {
        g_dm.thread_running = false;
        pthread_mutex_unlock(&g_dm.lock);
        return ZCL_ERR(-4, "thread_registry_spawn_ex failed (%d)", rc);
    }
    pthread_mutex_unlock(&g_dm.lock);

    struct zcl_result sup_r = dm_register_supervisor();
    if (!sup_r.ok) {
        disk_monitor_stop();
        return sup_r;
    }
    return ZCL_OK;
}

void disk_monitor_stop(void)
{
    pthread_t th;
    bool joinable = false;

    supervisor_child_id id = atomic_load(&g_dm.supervisor_id);
    if (id != SUPERVISOR_INVALID_ID)
        supervisor_set_deadline(id, 0);

    pthread_mutex_lock(&g_dm.lock);
    if (g_dm.thread_running) {
        g_dm.stop_requested = true;
        th = g_dm.thread;
        joinable = true;
    }
    pthread_mutex_unlock(&g_dm.lock);

    if (joinable) {
        pthread_join(th, NULL);
        pthread_mutex_lock(&g_dm.lock);
        g_dm.thread_running = false;
        g_dm.stop_requested = false;
        pthread_mutex_unlock(&g_dm.lock);
    }
#ifdef ZCL_TESTING
    id = atomic_exchange(&g_dm.supervisor_id, SUPERVISOR_INVALID_ID);
    if (id != SUPERVISOR_INVALID_ID)
        supervisor_unregister(id);
#endif
}

/* ── Status / queries ───────────────────────────────────────── */

void disk_monitor_status_snapshot(struct disk_monitor_status *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&g_dm.lock);
    out->running          = g_dm.thread_running;
    out->level            = g_dm.last_level;
    out->last_free_bytes  = g_dm.last_free_bytes;
    out->last_poll_unix   = g_dm.last_poll_unix;
    out->warn_free_bytes  = g_dm.warn_free_bytes;
    out->refuse_free_bytes = g_dm.refuse_free_bytes;
    snprintf(out->datadir, sizeof(out->datadir), "%s",
             g_dm.cfg.datadir ? g_dm.cfg.datadir : "");
    pthread_mutex_unlock(&g_dm.lock);
}

bool disk_monitor_is_critical(void)
{
    return (enum disk_monitor_level)atomic_load(&g_dm.atomic_level) ==
           DISK_MONITOR_CRITICAL;
}

enum disk_monitor_level disk_monitor_level(void)
{
    return (enum disk_monitor_level)atomic_load(&g_dm.atomic_level);
}

/* zcl_state subsystem=disk_monitor — free-space watchdog state: running
 * flag, current level (ok/low/critical), last poll's free bytes + time, and
 * the resolved warn/refuse thresholds. See CLAUDE.md "Adding state
 * introspection". Reentrant-safe (the snapshot takes the brief poll lock). */
bool disk_monitor_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;
    json_set_object(out);

    struct disk_monitor_status st;
    disk_monitor_status_snapshot(&st);
    json_push_kv_bool(out, "running", st.running);
    json_push_kv_str (out, "level", dm_level_name(st.level));
    json_push_kv_int (out, "last_free_bytes", st.last_free_bytes);
    json_push_kv_int (out, "last_poll_unix", st.last_poll_unix);
    json_push_kv_int (out, "warn_free_bytes", st.warn_free_bytes);
    json_push_kv_int (out, "refuse_free_bytes", st.refuse_free_bytes);
    json_push_kv_str (out, "datadir", st.datadir);

    /* Reserved `_health` key (see docs/work "Adding state introspection" +
     * app/controllers/src/diagnostics_health_rollup.c): { ok, reason }.
     * Maps the already-computed level above (DISK_MONITOR_OK vs LOW/
     * CRITICAL) — no new health logic. */
    {
        bool ok = st.level == DISK_MONITOR_OK;
        struct json_value health = {0};
        json_set_object(&health);
        json_push_kv_bool(&health, "ok", ok);
        char reason_buf[192] = "";
        if (!ok)
            snprintf(reason_buf, sizeof(reason_buf),
                     "level=%s last_free_bytes=%lld warn_free_bytes=%lld "
                     "refuse_free_bytes=%lld",
                     dm_level_name(st.level), (long long)st.last_free_bytes,
                     (long long)st.warn_free_bytes,
                     (long long)st.refuse_free_bytes);
        json_push_kv_str(&health, "reason", reason_buf);
        json_push_kv(out, "_health", &health);
        json_free(&health);
    }
    return true;
}
