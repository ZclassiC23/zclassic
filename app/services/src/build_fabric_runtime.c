/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Fail-loud liveness contracts for build scheduling and workers. */

#include "services/build_fabric_runtime.h"

#include "config/runtime.h"
#include "json/json.h"
#include "models/build_fabric.h"
#include "supervisors/domains.h"
#include "util/log_macros.h"
#include "util/supervisor.h"

#include <stdatomic.h>
#include <string.h>

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

struct zcl_result build_fabric_runtime_register(bool worker_enabled)
{
    supervisor_domains_init();
    atomic_store(&g_worker_enabled, worker_enabled);
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
            &g_worker_contract, "build.worker", bf_worker_tick);
        atomic_store(&g_worker_id, id);
        if (id == SUPERVISOR_INVALID_ID)
            LOG_WARN("build_fabric", "worker supervisor registration failed");
        else if (worker_enabled)
            supervisor_set_progress_max_quiet(id, BF_RUNTIME_MAX_QUIET_US);
        else
            supervisor_set_progress_exempt(id, "-buildworker not enabled");
    }
    if (atomic_load(&g_coordinator_id) == SUPERVISOR_INVALID_ID ||
        atomic_load(&g_worker_id) == SUPERVISOR_INVALID_ID)
        return ZCL_ERR(-1, "build runtime supervisor registration failed");
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
