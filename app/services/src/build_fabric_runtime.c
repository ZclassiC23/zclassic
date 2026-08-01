/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Fail-loud liveness contracts for build scheduling and workers. */

#include "services/build_fabric_runtime.h"

#include "base/hex.h"
#include "services/build_fabric_service.h"
#include "services/build_fabric_worker.h"

#include "config/runtime.h"
#include "json/json.h"
#include "models/build_fabric.h"
#include "platform/time_compat.h"
#include "crypto/random_secret.h"
#include "supervisors/domains.h"
#include "util/log_macros.h"
#include "util/supervisor.h"
#include "util/thread_registry.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { BF_RUNTIME_JOB_LIMIT = 100 };
#define BF_RUNTIME_PERIOD_SECS 2
#define BF_RUNTIME_MAX_QUIET_US ((int64_t)120 * 1000 * 1000)

static struct liveness_contract g_coordinator_contract;
static struct liveness_contract g_worker_contract;
static _Atomic supervisor_child_id g_coordinator_id = SUPERVISOR_INVALID_ID;
static _Atomic supervisor_child_id g_worker_id = SUPERVISOR_INVALID_ID;
static _Atomic bool g_worker_enabled;
static _Atomic uint64_t g_coordinator_ticks;
static _Atomic uint64_t g_worker_ticks;
static _Atomic uint64_t g_jobs_active;
static _Atomic uint64_t g_actions_active;
static _Atomic uint64_t g_jobs_terminal;
static _Atomic uint64_t g_accepted_or_cache;
static _Atomic uint64_t g_leases_recovered;
static _Atomic uint64_t g_recovery_failures;
static _Atomic uint64_t g_worker_dispatches;
static _Atomic uint64_t g_worker_failures;
static struct db_build_worker g_local_worker;
static uint8_t g_local_secret[32];
static uint8_t g_local_pubkey[32];
static char g_worker_workspace[4096];
static char g_worker_db_path[4096];
static pthread_t g_worker_thread;
static _Atomic bool g_worker_started;

extern volatile sig_atomic_t g_shutdown_requested;

static bool bf_runtime_state_active(const char *state)
{
    return state && (strcmp(state, "QUEUED") == 0 ||
                     strcmp(state, "CLAIMED") == 0 ||
                     strcmp(state, "RUNNING") == 0 ||
                     strcmp(state, "VERIFYING") == 0);
}

static bool bf_runtime_state_terminal(const char *state)
{
    return state && (strcmp(state, "ACCEPTED") == 0 ||
                     strcmp(state, "CACHE_HIT") == 0 ||
                     strcmp(state, "LOCAL_FALLBACK") == 0 ||
                     strcmp(state, "DISPUTED") == 0 ||
                     strcmp(state, "CANCELLED") == 0 ||
                     strcmp(state, "FAILED") == 0);
}

static void bf_runtime_snapshot(uint64_t *active_jobs,
                                uint64_t *active_actions,
                                uint64_t *terminal_jobs,
                                uint64_t *accepted)
{
    *active_jobs = *active_actions = *terminal_jobs = *accepted = 0;
    struct node_db *ndb = app_runtime_node_db();
    if (!ndb || !ndb->open) return;
    struct db_build_job jobs[BF_RUNTIME_JOB_LIMIT];
    int count = db_build_jobs_recent(ndb, jobs, BF_RUNTIME_JOB_LIMIT);
    for (int i = 0; i < count; i++) {
        if (bf_runtime_state_active(jobs[i].state)) (*active_jobs)++;
        if (bf_runtime_state_terminal(jobs[i].state)) (*terminal_jobs)++;
        if (strcmp(jobs[i].state, "ACCEPTED") == 0 ||
            strcmp(jobs[i].state, "CACHE_HIT") == 0) (*accepted)++;
        struct db_build_action actions[16];
        int action_count = db_build_job_actions(ndb, jobs[i].job_id,
                                                 actions, 16);
        for (int j = 0; j < action_count; j++)
            if (bf_runtime_state_active(actions[j].state)) (*active_actions)++;
    }
    atomic_store(&g_jobs_active, *active_jobs);
    atomic_store(&g_actions_active, *active_actions);
    atomic_store(&g_jobs_terminal, *terminal_jobs);
    atomic_store(&g_accepted_or_cache, *accepted);
}

static void bf_coordinator_tick(struct liveness_contract *contract)
{
    (void)contract;
    struct node_db *ndb = app_runtime_node_db();
    if (ndb && ndb->open) {
        size_t recovered = 0;
        struct zcl_result recovery = build_fabric_recover_expired(
            ndb, (int64_t)platform_time_wall_unix(), &recovered);
        if (recovery.ok)
            atomic_fetch_add(&g_leases_recovered, recovered);
        else {
            atomic_fetch_add(&g_recovery_failures, 1);
            LOG_WARN("build_fabric", "expired lease recovery failed: %s",
                     recovery.message);
        }
    }
    uint64_t active_jobs, active_actions, terminal_jobs, accepted;
    bf_runtime_snapshot(&active_jobs, &active_actions, &terminal_jobs, &accepted);
    supervisor_child_id id = atomic_load(&g_coordinator_id);
    if (active_jobs == 0) supervisor_progress_idle(id);
    else supervisor_progress(id, (int64_t)terminal_jobs);
    atomic_fetch_add(&g_coordinator_ticks, 1);
    supervisor_tick(id);
}

static void bf_worker_tick(struct liveness_contract *contract)
{
    (void)contract;
    uint64_t active_jobs, active_actions, terminal_jobs, accepted;
    bf_runtime_snapshot(&active_jobs, &active_actions, &terminal_jobs, &accepted);
    supervisor_child_id id = atomic_load(&g_worker_id);
    if (atomic_load(&g_worker_enabled)) {
        if (active_actions == 0) supervisor_progress_idle(id);
        else supervisor_progress(id, (int64_t)accepted);
    }
    atomic_fetch_add(&g_worker_ticks, 1);
    supervisor_tick(id);
}

static void *bf_worker_loop(void *arg)
{
    (void)arg;
    struct node_db worker_db;
    memset(&worker_db, 0, sizeof(worker_db));
    if (!node_db_open(&worker_db, g_worker_db_path)) {
        atomic_fetch_add(&g_worker_failures, 1);
        return NULL;
    }
    uint64_t completed = 0;
    while (!g_shutdown_requested) {
        struct node_db *ndb = &worker_db;
        supervisor_child_id id = atomic_load(&g_worker_id);
        uint8_t lease_raw[32];
        char lease_id[65];
        if (!zcl_random_secret_bytes(lease_raw, sizeof(lease_raw),
                                     "zbuild_lease")) {
            atomic_fetch_add(&g_worker_failures, 1);
            supervisor_tick(id);
            platform_sleep_ms(1000);
            continue;
        }
        zcl_hex_encode(lease_raw, sizeof(lease_raw), lease_id);
        memset(lease_raw, 0, sizeof(lease_raw));
        struct db_build_action action;
        bool claimed = false;
        struct zcl_result claim = build_fabric_claim(
            ndb, g_local_worker.worker_id, lease_id,
            (int64_t)platform_time_wall_unix(),
            BUILD_FABRIC_LEASE_SECONDS_MAX, &action, &claimed);
        if (!claim.ok) {
            atomic_fetch_add(&g_worker_failures, 1);
            supervisor_tick(id);
            platform_sleep_ms(1000);
            continue;
        }
        if (!claimed) {
            supervisor_progress_idle(id);
            supervisor_tick(id);
            platform_sleep_ms(250);
            continue;
        }
        atomic_fetch_add(&g_worker_dispatches, 1);
        struct db_build_receipt receipt;
        struct zcl_result run = build_fabric_worker_execute(
            ndb, g_worker_workspace, action.action_id, lease_id,
            g_local_secret, g_local_pubkey, &receipt);
        if (run.ok)
            supervisor_progress(id, (int64_t)++completed);
        else
            atomic_fetch_add(&g_worker_failures, 1);
        supervisor_tick(id);
    }
    node_db_close(&worker_db);
    return NULL;
}

static supervisor_child_id bf_runtime_child(
    struct liveness_contract *contract, const char *name,
    void (*tick)(struct liveness_contract *))
{
    liveness_contract_init(contract, name);
    atomic_store(&contract->period_secs, BF_RUNTIME_PERIOD_SECS);
    atomic_store(&contract->deadline_secs, 10);
    contract->on_tick = tick;
    return supervisor_register_in_domain(g_op_sup, contract);
}

struct zcl_result build_fabric_runtime_register(bool worker_enabled,
                                                const char *datadir)
{
    supervisor_domains_init();
    atomic_store(&g_worker_enabled, worker_enabled);
    if (worker_enabled && !atomic_load(&g_worker_started)) {
        if (!datadir || !getcwd(g_worker_workspace,
                                sizeof(g_worker_workspace)))
            return ZCL_ERR(-1, "build worker cannot resolve its workspace");
        int dbn = snprintf(g_worker_db_path, sizeof(g_worker_db_path),
                           "%s/node.db", datadir);
        if (dbn <= 0 || (size_t)dbn >= sizeof(g_worker_db_path))
            return ZCL_ERR(-1, "build worker database path is too long");
        ZCL_CHECK(build_fabric_worker_identity_load(
            datadir, &g_local_worker, g_local_secret, g_local_pubkey));
        struct node_db *ndb = app_runtime_node_db();
        if (!ndb || !ndb->open)
            return ZCL_ERR(-1, "build worker database is unavailable");
        int64_t now = (int64_t)platform_time_wall_unix();
        g_local_worker.approved_at = now;
        g_local_worker.last_seen_at = now;
        ZCL_CHECK(build_fabric_worker_approve(ndb, &g_local_worker, now));
    }
    if (atomic_load(&g_coordinator_id) == SUPERVISOR_INVALID_ID) {
        supervisor_child_id id = bf_runtime_child(
            &g_coordinator_contract, "build.coordinator", bf_coordinator_tick);
        atomic_store(&g_coordinator_id, id);
        if (id == SUPERVISOR_INVALID_ID)
            LOG_WARN("build_fabric", "coordinator supervisor registration failed");
        else
            supervisor_set_progress_max_quiet(id, BF_RUNTIME_MAX_QUIET_US);
    }
    if (atomic_load(&g_worker_id) == SUPERVISOR_INVALID_ID) {
        supervisor_child_id id = bf_runtime_child(
            &g_worker_contract, "build.worker",
            worker_enabled ? NULL : bf_worker_tick);
        atomic_store(&g_worker_id, id);
        if (id == SUPERVISOR_INVALID_ID)
            LOG_WARN("build_fabric", "worker supervisor registration failed");
        else if (worker_enabled) {
            atomic_store(&g_worker_contract.period_secs, 0);
            atomic_store(&g_worker_contract.deadline_secs, 600);
            supervisor_set_progress_max_quiet(
                id, (int64_t)610 * 1000 * 1000);
        }
        else
            supervisor_set_progress_exempt(id, "-buildworker not enabled");
    }
    if (atomic_load(&g_coordinator_id) == SUPERVISOR_INVALID_ID ||
        atomic_load(&g_worker_id) == SUPERVISOR_INVALID_ID)
        return ZCL_ERR(-1, "build runtime supervisor registration failed");
    if (worker_enabled && !atomic_exchange(&g_worker_started, true)) {
        // supervised:build.worker (g_worker_contract registered above)
        int rc = thread_registry_spawn("zcl_build_worker", bf_worker_loop,
                                       NULL, &g_worker_thread);
        if (rc != 0) {
            atomic_store(&g_worker_started, false);
            return ZCL_ERR(-1, "build worker thread spawn failed: %d", rc);
        }
    }
    return ZCL_OK;
}

bool build_fabric_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);
    (void)json_push_kv_str(out, "schema", "zcl.build_fabric_state.v1");
    (void)json_push_kv_bool(out, "worker_enabled", atomic_load(&g_worker_enabled));
    (void)json_push_kv_int(out, "coordinator_ticks", (int64_t)atomic_load(&g_coordinator_ticks));
    (void)json_push_kv_int(out, "worker_ticks", (int64_t)atomic_load(&g_worker_ticks));
    (void)json_push_kv_int(out, "jobs_active", (int64_t)atomic_load(&g_jobs_active));
    (void)json_push_kv_int(out, "actions_active", (int64_t)atomic_load(&g_actions_active));
    (void)json_push_kv_int(out, "jobs_terminal", (int64_t)atomic_load(&g_jobs_terminal));
    (void)json_push_kv_int(out, "accepted_or_cache", (int64_t)atomic_load(&g_accepted_or_cache));
    (void)json_push_kv_int(out, "leases_recovered", (int64_t)atomic_load(&g_leases_recovered));
    (void)json_push_kv_int(out, "recovery_failures", (int64_t)atomic_load(&g_recovery_failures));
    (void)json_push_kv_int(out, "worker_dispatches", (int64_t)atomic_load(&g_worker_dispatches));
    (void)json_push_kv_int(out, "worker_failures", (int64_t)atomic_load(&g_worker_failures));
    (void)json_push_kv_bool(out, "worker_thread_started", atomic_load(&g_worker_started));
    (void)json_push_kv_int(out, "coordinator_deadline_s", 10);
    (void)json_push_kv_int(out, "worker_deadline_s", 10);
    (void)json_push_kv_int(out, "max_quiet_s", 120);
    (void)json_push_kv_int(out, "max_actions_per_job", 256);
    (void)json_push_kv_int(out, "worker_cpu_limit", 1);
    (void)json_push_kv_int(out, "worker_memory_mb", 2048);
    (void)json_push_kv_int(out, "worker_timeout_s", 120);
    (void)json_push_kv_bool(out, "worker_network_allowed", false);
    (void)json_push_kv_int(out, "supervisor_child_headroom",
                           supervisor_child_headroom());
    bool supervised = atomic_load(&g_coordinator_id) != SUPERVISOR_INVALID_ID &&
                      atomic_load(&g_worker_id) != SUPERVISOR_INVALID_ID;
    diag_push_health(out, supervised, supervised ? "supervised" : "not_registered");
    return true;
}
