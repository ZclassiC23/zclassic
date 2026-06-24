/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Runtime state snapshots, lifecycle wiring, and test hooks for the legacy
 * mirror monitor. The catchup tick logic lives in legacy_mirror_sync_service.c. */

#include "services/legacy_mirror_sync_service.h"
#include "legacy_mirror_sync_internal.h"

#include "json/json.h"
#include "platform/time_compat.h"
#include "rpc/legacy_rpc_client.h"
#include "services/mirror_divergence_locator.h"
#include "services/sync_monitor.h"
#include "supervisors/legacy_mirror_supervisor.h"
#include "util/blocker.h"
#include "util/log_macros.h"
#include "validation/mirror_consensus.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const char *lms_state_name(const struct legacy_mirror_sync_stats *s)
{
    if (!s || !s->enabled || !s->running)
        return "blocked";
    if (s->activation_blocker_reason[0] || s->last_blocker_id[0] ||
        s->csr_sqlite_rc != 0)
        return "blocked";
    if (s->lag_known &&
        s->lag_sla_breach_blocks > 0 &&
        s->lag >= s->lag_sla_breach_blocks)
        return "concurrent_catchup";
    if (s->mirror_repair_gated_by_local_retries)
        return "gated_by_local_retries";
    if (s->lag_known && s->lag < 0)
        return "observing";
    if (s->in_flight || s->last_progress_blocks > 0 ||
        (s->lag_known && s->lag > 1))
        return "catching_up";
    if (s->lag_known && s->lag <= 1)
        return "healthy";
    return "observing";
}

static bool lms_blocker_cleared_by_catchup(const char *code,
                                           bool lag_known,
                                           int lag)
{
    return code && lag_known &&
           strcmp(code, "activation-no-progress") == 0 && lag <= 0;
}

int legacy_mirror_sync_reported_lag(
    const struct legacy_mirror_sync_stats *s)
{
    return (s && s->lag_known) ? s->lag : 0;
}

void legacy_mirror_sync_push_observed_lag_json(
    struct json_value *out,
    const char *key,
    const struct legacy_mirror_sync_stats *s)
{
    if (!out || !key)
        return;
    if (s && s->lag_known) {
        json_push_kv_int(out, key, s->lag);
        return;
    }
    struct json_value nullv;
    json_init(&nullv);
    json_set_null(&nullv);
    json_push_kv(out, key, &nullv);
    json_free(&nullv);
}

struct zcl_result legacy_mirror_sync_request_catchup_result(const char *reason)
{
    return lms_request_catchup_result_internal(reason);
}

/* zcl_result error band for this file: -200..-209. The -1 code is owned
 * by lms_request_catchup_result_internal in legacy_mirror_sync_service.c. */
struct zcl_result
legacy_mirror_sync_init(const struct legacy_mirror_sync_config *cfg,
                        struct main_state *ms,
                        struct coins_view_cache *coins_tip,
                        const struct chain_params *params,
                        const char *datadir)
{
    pthread_mutex_lock(&g_lms.lock);

    snprintf(g_lms.rpc_host, sizeof(g_lms.rpc_host), "%s",
             (cfg && cfg->rpc_host) ? cfg->rpc_host : LMS_DEFAULT_HOST);
    g_lms.rpc_port = (cfg && cfg->rpc_port > 0)
                         ? cfg->rpc_port : LMS_DEFAULT_PORT;
    g_lms.cadence_secs = (cfg && cfg->cadence_secs > 0)
                         ? cfg->cadence_secs : LMS_DEFAULT_CADENCE;
    g_lms.max_blocks_tick = (cfg && cfg->max_blocks_tick > 0)
                         ? cfg->max_blocks_tick : LMS_DEFAULT_MAX_BLOCKS;
    g_lms.lag_sla = (cfg && cfg->lag_sla >= 0)
                         ? cfg->lag_sla : LMS_DEFAULT_LAG_SLA;
    g_lms.cadence_secs = lms_env_int("ZCL_MIRROR_CADENCE_SECS",
                                     g_lms.cadence_secs, 1, 300);
    g_lms.max_blocks_tick = lms_env_int("ZCL_MIRROR_MAX_BLOCKS_PER_TICK",
                                        g_lms.max_blocks_tick, 1, 20000);
    g_lms.lag_sla = lms_env_int("ZCL_MIRROR_LAG_SLA",
                                g_lms.lag_sla, 0, 10000);

    atomic_store(&g_lms.lag_sla_breach_blocks,
        (cfg && cfg->lag_sla_breach_blocks > 0)
        ? cfg->lag_sla_breach_blocks
        : LMS_DEFAULT_LAG_SLA_BREACH_BLOCKS);
    atomic_store(&g_lms.lag_sla_breach_blocks,
        lms_env_int("ZCL_MIRROR_LAG_SLA_BREACH_BLOCKS",
                    atomic_load(&g_lms.lag_sla_breach_blocks), 1, 100000));

    atomic_store(&g_lms.lag_sla_breach_secs,
        (cfg && cfg->lag_sla_breach_secs > 0)
        ? cfg->lag_sla_breach_secs
        : LMS_DEFAULT_LAG_SLA_BREACH_SECS);
    atomic_store(&g_lms.lag_sla_breach_secs,
        lms_env_int("ZCL_MIRROR_LAG_SLA_BREACH_SECS",
                    atomic_load(&g_lms.lag_sla_breach_secs), 1, 86400));

    atomic_store(&g_lms.lag_sla_critical_blocks,
        (cfg && cfg->lag_sla_critical_blocks > 0)
        ? cfg->lag_sla_critical_blocks
        : LMS_DEFAULT_LAG_SLA_CRITICAL_BLOCKS);
    atomic_store(&g_lms.lag_sla_critical_blocks,
        lms_env_int("ZCL_MIRROR_LAG_SLA_CRITICAL_BLOCKS",
                    atomic_load(&g_lms.lag_sla_critical_blocks), 1, 1000000));

    atomic_store(&g_lms.lag_sla_critical_secs,
        (cfg && cfg->lag_sla_critical_secs > 0)
        ? cfg->lag_sla_critical_secs
        : LMS_DEFAULT_LAG_SLA_CRITICAL_SECS);
    atomic_store(&g_lms.lag_sla_critical_secs,
        lms_env_int("ZCL_MIRROR_LAG_SLA_CRITICAL_SECS",
                    atomic_load(&g_lms.lag_sla_critical_secs), 1, 86400));

    g_lms.ms = ms;
    g_lms.coins_tip = coins_tip;
    g_lms.params = params;
    snprintf(g_lms.datadir, sizeof(g_lms.datadir), "%s",
             datadir ? datadir : "");

    if (cfg && cfg->rpc_user && cfg->rpc_user[0])
        snprintf(g_lms.rpc_user, sizeof(g_lms.rpc_user), "%s",
                 cfg->rpc_user);
    if (cfg && cfg->rpc_password && cfg->rpc_password[0])
        snprintf(g_lms.rpc_password, sizeof(g_lms.rpc_password), "%s",
                 cfg->rpc_password);

    /* Fill any missing credential from zclassic.conf; an explicit port
     * (cfg->rpc_port > 0) is never overridden. A missing set is not
     * fatal here — the have-creds check below decides enablement. */
    bool have_creds = legacy_rpc_fill_missing_creds(
        g_lms.rpc_user, sizeof(g_lms.rpc_user),
        g_lms.rpc_password, sizeof(g_lms.rpc_password),
        &g_lms.rpc_port, cfg && cfg->rpc_port > 0);
    g_lms.enabled = (cfg ? cfg->enabled : true) && have_creds &&
                    !lms_env_disabled();
    mirror_consensus_set_enabled(g_lms.enabled);
    g_lms.initialized = true;
    pthread_mutex_unlock(&g_lms.lock);

    if (!have_creds) {
        lms_set_error("no zclassicd RPC credentials");
        return ZCL_ERR(-200, "no zclassicd RPC credentials");
    }
    return ZCL_OK;
}

void legacy_mirror_sync_reload_from_env(void)
{
    if (!g_lms.initialized)
        return;
    pthread_mutex_lock(&g_lms.lock);
    g_lms.cadence_secs = lms_env_int("ZCL_MIRROR_CADENCE_SECS",
                                     g_lms.cadence_secs, 1, 300);
    g_lms.max_blocks_tick = lms_env_int("ZCL_MIRROR_MAX_BLOCKS_PER_TICK",
                                        g_lms.max_blocks_tick, 1, 20000);
    g_lms.lag_sla = lms_env_int("ZCL_MIRROR_LAG_SLA",
                                g_lms.lag_sla, 0, 10000);
    atomic_store(&g_lms.lag_sla_breach_blocks,
        lms_env_int("ZCL_MIRROR_LAG_SLA_BREACH_BLOCKS",
                    atomic_load(&g_lms.lag_sla_breach_blocks), 1, 100000));
    atomic_store(&g_lms.lag_sla_breach_secs,
        lms_env_int("ZCL_MIRROR_LAG_SLA_BREACH_SECS",
                    atomic_load(&g_lms.lag_sla_breach_secs), 1, 86400));
    atomic_store(&g_lms.lag_sla_critical_blocks,
        lms_env_int("ZCL_MIRROR_LAG_SLA_CRITICAL_BLOCKS",
                    atomic_load(&g_lms.lag_sla_critical_blocks), 1, 1000000));
    atomic_store(&g_lms.lag_sla_critical_secs,
        lms_env_int("ZCL_MIRROR_LAG_SLA_CRITICAL_SECS",
                    atomic_load(&g_lms.lag_sla_critical_secs), 1, 86400));
    pthread_mutex_unlock(&g_lms.lock);
}

struct zcl_result legacy_mirror_sync_start(void)
{
    if (!g_lms.initialized) {
        fprintf(stderr, "[legacy_mirror] start before init\n");
        return ZCL_ERR(-201, "legacy_mirror start before init");
    }
    if (!g_lms.enabled)
        return ZCL_OK;
    int cad = g_lms.cadence_secs > 0 ? g_lms.cadence_secs
                                     : LMS_DEFAULT_CADENCE;
    if (!legacy_mirror_supervisor_start(cad))
        return ZCL_ERR(-202,
                       "legacy_mirror supervisor start failed cadence=%d",
                       cad);
    return ZCL_OK;
}

void legacy_mirror_sync_stop(void)
{
    legacy_mirror_supervisor_stop();
}

void legacy_mirror_sync_stats_snapshot(
    struct legacy_mirror_sync_stats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    lms_refresh_local_heights(NULL, NULL);
    pthread_mutex_lock(&g_lms.lock);
    out->enabled = g_lms.enabled;
#ifdef ZCL_TESTING
    out->running = g_lms_test_fake_running ||
                   legacy_mirror_supervisor_running();
#else
    out->running = legacy_mirror_supervisor_running();
#endif
    snprintf(out->zclassicd_hash, sizeof(out->zclassicd_hash), "%s",
             g_lms.zclassicd_hash);
    snprintf(out->last_error, sizeof(out->last_error), "%s",
             g_lms.last_error);
    snprintf(out->last_blocker_id, sizeof(out->last_blocker_id), "%s",
             g_lms.last_blocker_id);
    out->last_blocker_class = out->last_blocker_id[0]
                                  ? g_lms.last_blocker_class
                                  : BLOCKER_TRANSIENT;
    snprintf(out->csr_failure_reason, sizeof(out->csr_failure_reason), "%s",
             g_lms.csr_failure_reason);
    snprintf(out->stuck_reason, sizeof(out->stuck_reason), "%s",
             g_lms.stuck_reason);
    pthread_mutex_unlock(&g_lms.lock);
    out->reachable = atomic_load(&g_lms.reachable) != 0;
    out->in_flight = atomic_load(&g_lms.in_flight) != 0;
    out->legacy_height = atomic_load(&g_lms.legacy_height);
    out->legacy_headers = atomic_load(&g_lms.legacy_headers);
    out->local_height = atomic_load(&g_lms.local_height);
    out->best_header_height = atomic_load(&g_lms.best_header_height);
    out->legacy_advisory_height_known =
        out->enabled && out->reachable && out->legacy_height >= 0;
    out->target_height_known = out->legacy_advisory_height_known;
    out->lag_known = out->legacy_advisory_height_known &&
                     out->local_height >= 0;
    out->lag_valid = out->lag_known;
    if (!lms_local_hash_at(out->local_height, out->zclassic23_hash)) {
        pthread_mutex_lock(&g_lms.lock);
        snprintf(out->zclassic23_hash, sizeof(out->zclassic23_hash), "%s",
                 g_lms.zclassic23_hash);
        pthread_mutex_unlock(&g_lms.lock);
    }
    out->lag = out->lag_known ? out->legacy_height - out->local_height : 0;
    out->target_height = atomic_load(&g_lms.target_height);
    out->authority_rewind_target =
        atomic_load(&g_lms.authority_rewind_target);
    out->csr_sqlite_rc = atomic_load(&g_lms.csr_sqlite_rc);
    out->last_advanced_height = atomic_load(&g_lms.last_advanced_height);
    out->last_progress_blocks = atomic_load(&g_lms.last_progress_blocks);
    {
        struct watchdog_local_recovery_stats lr;
        sync_monitor_get_local_recovery_stats(&lr);
        out->local_recovery_active = lr.active;
        out->mirror_repair_gated_by_local_retries =
            lr.mirror_repair_gated;
        out->local_retries_exhausted = lr.retries_exhausted;
        out->local_missing_height = lr.missing_height;
        out->local_retry_count = lr.retry_count;
        out->local_distinct_peer_count = lr.distinct_peer_count;
        out->local_peer_rotation_count = lr.peer_rotation_count;
    }
    out->stuck_height = atomic_load(&g_lms.stuck_height);
    out->stuck_status_flags = atomic_load(&g_lms.stuck_status_flags);
    out->stalls_total = atomic_load(&g_lms.stalls_total);
    out->last_catchup = atomic_load(&g_lms.last_catchup);
    out->last_attempt = atomic_load(&g_lms.last_attempt);
    out->catchups_total = atomic_load(&g_lms.catchups_total);
    out->rpc_errors = atomic_load(&g_lms.rpc_errors);
    out->blocks_applied = atomic_load(&g_lms.blocks_applied);
    out->headers_added = atomic_load(&g_lms.headers_added);
    {
        struct mirror_consensus_stats mcs;
        mirror_consensus_stats_snapshot(&mcs);
        snprintf(out->consensus_authority,
                 sizeof(out->consensus_authority), "%s",
                 "local_consensus_validation");
        snprintf(out->candidate_trust,
                 sizeof(out->candidate_trust), "%s",
                 "bounded_advisory_fallback");
        out->override_active = mcs.override_active;
        out->overrides_total = mcs.overrides_total;
        out->unsafe_overrides_total = mcs.unsafe_overrides_total;
        out->blockers_total = mcs.blockers_total;
        out->last_override_height = mcs.last_override_height;
        out->last_override_safe = mcs.last_override_safe;
        out->activation_blocker_class = mcs.activation_blocker_class;
        snprintf(out->last_override_reason,
                 sizeof(out->last_override_reason), "%s",
                 mcs.last_override_reason);
        snprintf(out->last_override_scope,
                 sizeof(out->last_override_scope), "%s",
                 mcs.last_override_scope);
        snprintf(out->activation_blocker_reason,
                 sizeof(out->activation_blocker_reason), "%s",
                 mcs.activation_blocker_reason);
    }
    if (lms_blocker_cleared_by_catchup(out->activation_blocker_reason,
                                       out->lag_known,
                                       out->lag))
        out->activation_blocker_reason[0] = '\0';
    if (lms_blocker_cleared_by_catchup(out->last_blocker_id,
                                       out->lag_known,
                                       out->lag))
        out->last_blocker_id[0] = '\0';
    if (!out->activation_blocker_reason[0])
        out->activation_blocker_class = BLOCKER_TRANSIENT;
    if (!out->last_blocker_id[0])
        out->last_blocker_class = BLOCKER_TRANSIENT;
    out->lag_sla_breach_blocks   = atomic_load(&g_lms.lag_sla_breach_blocks);
    out->lag_sla_breach_secs     = atomic_load(&g_lms.lag_sla_breach_secs);
    out->lag_sla_critical_blocks = atomic_load(&g_lms.lag_sla_critical_blocks);
    out->lag_sla_critical_secs   = atomic_load(&g_lms.lag_sla_critical_secs);
    out->lag_breach_since        = atomic_load(&g_lms.lag_breach_since);
    out->lag_critical_since      = atomic_load(&g_lms.lag_critical_since);
    {
        int64_t now = (int64_t)platform_time_wall_time_t();
        out->lag_breach_seconds =
            out->lag_known &&
            out->lag_breach_since > 0 && now >= out->lag_breach_since
                ? now - out->lag_breach_since : 0;
        out->lag_critical_seconds =
            out->lag_known &&
            out->lag_critical_since > 0 && now >= out->lag_critical_since
                ? now - out->lag_critical_since : 0;
    }
    const char *sev = "none";
    if (out->lag_known &&
        out->lag_critical_since > 0 &&
        out->lag_critical_seconds >= out->lag_sla_critical_secs)
        sev = "fatal";
    else if (out->lag_known && out->lag_critical_since > 0)
        sev = "critical";
    else if (out->lag_known &&
             out->lag_breach_since > 0 &&
             out->lag_breach_seconds >= out->lag_sla_breach_secs)
        sev = "critical";
    else if (out->lag_known && out->lag_breach_since > 0)
        sev = "warn";
    snprintf(out->lag_breach_severity, sizeof(out->lag_breach_severity),
             "%s", sev);
    snprintf(out->state, sizeof(out->state), "%s", lms_state_name(out));
}

bool legacy_mirror_sync_dump_state_json(struct json_value *out,
                                        const char *key)
{
    (void)key;
    if (!out) return false;
    struct legacy_mirror_sync_stats s;
    legacy_mirror_sync_stats_snapshot(&s);
    json_push_kv_bool(out, "mirror_enabled", s.enabled);
    json_push_kv_str(out, "state", s.state);
    json_push_kv_bool(out, "mirror_running", s.running);
    json_push_kv_bool(out, "running", s.running);
    json_push_kv_bool(out, "reachable", s.reachable);
    json_push_kv_bool(out, "mirror_reachable", s.reachable);
    json_push_kv_bool(out, "in_flight", s.in_flight);
    json_push_kv_int(out, "zclassic23_height", s.local_height);
    json_push_kv_str(out, "zclassic23_hash", s.zclassic23_hash);
    json_push_kv_int(out, "zclassicd_height", s.legacy_height);
    json_push_kv_str(out, "zclassicd_hash", s.zclassicd_hash);
    json_push_kv_int(out, "legacy_height", s.legacy_height);
    json_push_kv_int(out, "legacy_headers", s.legacy_headers);
    json_push_kv_int(out, "local_height", s.local_height);
    json_push_kv_int(out, "best_header_height", s.best_header_height);
    json_push_kv_bool(out, "legacy_advisory_height_known",
                      s.legacy_advisory_height_known);
    json_push_kv_bool(out, "target_height_known", s.target_height_known);
    json_push_kv_bool(out, "lag_known", s.lag_known);
    json_push_kv_bool(out, "lag_valid", s.lag_valid);
    json_push_kv_int(out, "lag", legacy_mirror_sync_reported_lag(&s));
    legacy_mirror_sync_push_observed_lag_json(out, "lag_observed", &s);
    json_push_kv_str(out, "candidate_source", "legacy_advisory");
    json_push_kv_str(out, "candidate_trust", s.candidate_trust);
    json_push_kv_bool(out, "candidate_lag_known", s.lag_known);
    json_push_kv_bool(out, "candidate_lag_valid", s.lag_valid);
    json_push_kv_int(out, "candidate_lag",
                     legacy_mirror_sync_reported_lag(&s));
    legacy_mirror_sync_push_observed_lag_json(out,
                                              "candidate_lag_observed", &s);
    json_push_kv_str(out, "candidate_blocker",
                     s.activation_blocker_reason[0] ? s.activation_blocker_reason
                                             : s.last_blocker_id);
    json_push_kv_int(out, "target_height", s.target_height);
    json_push_kv_int(out, "authority_rewind_target",
                     s.authority_rewind_target);
    json_push_kv_int(out, "last_advanced_height", s.last_advanced_height);
    json_push_kv_int(out, "last_progress_blocks", s.last_progress_blocks);
    json_push_kv_bool(out, "local_recovery_active",
                      s.local_recovery_active);
    json_push_kv_bool(out, "legacy_advisory_gated_by_native_retries",
                      s.mirror_repair_gated_by_local_retries);
    json_push_kv_bool(out, "mirror_repair_gated_by_local_retries",
                      s.mirror_repair_gated_by_local_retries);
    json_push_kv_bool(out, "local_retries_exhausted",
                      s.local_retries_exhausted);
    json_push_kv_int(out, "local_missing_height", s.local_missing_height);
    json_push_kv_int(out, "local_retry_count", s.local_retry_count);
    json_push_kv_int(out, "local_distinct_peer_count",
                     s.local_distinct_peer_count);
    json_push_kv_int(out, "local_peer_rotation_count",
                     s.local_peer_rotation_count);
    json_push_kv_int(out, "stuck_height", s.stuck_height);
    json_push_kv_int(out, "stuck_status_flags", s.stuck_status_flags);
    json_push_kv_str(out, "stuck_reason", s.stuck_reason);
    json_push_kv_int(out, "stalls_total", s.stalls_total);
    json_push_kv_int(out, "last_catchup", s.last_catchup);
    json_push_kv_int(out, "last_attempt", s.last_attempt);
    json_push_kv_int(out, "catchups_total", s.catchups_total);
    json_push_kv_int(out, "rpc_errors", s.rpc_errors);
    json_push_kv_int(out, "blocks_applied", s.blocks_applied);
    json_push_kv_int(out, "headers_added", s.headers_added);
    json_push_kv_str(out, "consensus_authority", s.consensus_authority);
    json_push_kv_bool(out, "override_active", s.override_active);
    json_push_kv_int(out, "overrides_total", s.overrides_total);
    json_push_kv_int(out, "unsafe_overrides_total",
                     s.unsafe_overrides_total);
    json_push_kv_int(out, "blockers_total", s.blockers_total);
    json_push_kv_int(out, "last_override_height", s.last_override_height);
    json_push_kv_bool(out, "last_override_safe", s.last_override_safe);
    json_push_kv_str(out, "last_override_reason", s.last_override_reason);
    json_push_kv_str(out, "last_override_scope", s.last_override_scope);
    json_push_kv_str(out, "activation_blocker_class",
                     blocker_class_name(s.activation_blocker_class));
    json_push_kv_str(out, "last_blocker_class",
                     blocker_class_name(s.last_blocker_class));
    json_push_kv_str(out, "activation_blocker", s.activation_blocker_reason);
    json_push_kv_str(out, "last_blocker_code", s.last_blocker_id);
    json_push_kv_str(out, "active_error_code",
                     s.activation_blocker_reason[0] ? s.activation_blocker_reason
                                             : s.last_blocker_id);
    json_push_kv_str(out, "active_error_detail",
                     (s.activation_blocker_reason[0] || s.last_blocker_id[0])
                         ? s.last_error : "");
    json_push_kv_int(out, "csr_sqlite_rc", s.csr_sqlite_rc);
    json_push_kv_str(out, "csr_failure_reason", s.csr_failure_reason);
    json_push_kv_str(out, "last_error", s.last_error);
    json_push_kv_int(out, "lag_sla_breach_blocks", s.lag_sla_breach_blocks);
    json_push_kv_int(out, "lag_sla_breach_secs", s.lag_sla_breach_secs);
    json_push_kv_int(out, "lag_sla_critical_blocks",
                     s.lag_sla_critical_blocks);
    json_push_kv_int(out, "lag_sla_critical_secs", s.lag_sla_critical_secs);
    json_push_kv_int(out, "lag_breach_since", s.lag_breach_since);
    json_push_kv_int(out, "lag_breach_seconds", s.lag_breach_seconds);
    json_push_kv_int(out, "lag_critical_since", s.lag_critical_since);
    json_push_kv_int(out, "lag_critical_seconds", s.lag_critical_seconds);
    json_push_kv_str(out, "lag_breach_severity", s.lag_breach_severity);
    return true;
}

void legacy_mirror_sync_reset_for_test(void)
{
#ifdef ZCL_TESTING
    g_lms_test_fake_running = false;
#endif
    legacy_mirror_sync_stop();
    pthread_mutex_lock(&g_lms.lock);
    g_lms.initialized = false;
    g_lms.enabled = false;
    mirror_consensus_set_enabled(false);
    g_lms.rpc_host[0] = '\0';
    g_lms.rpc_port = 0;
    g_lms.rpc_user[0] = '\0';
    g_lms.rpc_password[0] = '\0';
    g_lms.datadir[0] = '\0';
    g_lms.zclassic23_hash[0] = '\0';
    g_lms.zclassicd_hash[0] = '\0';
    g_lms.stuck_reason[0] = '\0';
    g_lms.last_blocker_class = BLOCKER_TRANSIENT;
    g_lms.last_blocker_id[0] = '\0';
    g_lms.csr_failure_reason[0] = '\0';
    g_lms.ms = NULL;
    g_lms.coins_tip = NULL;
    g_lms.params = NULL;
    g_lms.last_error[0] = '\0';
    pthread_mutex_unlock(&g_lms.lock);
    atomic_store(&g_lms.reachable, 0);
    atomic_store(&g_lms.in_flight, 0);
    atomic_store(&g_lms.legacy_height, 0);
    atomic_store(&g_lms.legacy_headers, 0);
    atomic_store(&g_lms.local_height, 0);
    atomic_store(&g_lms.best_header_height, 0);
    atomic_store(&g_lms.target_height, 0);
    atomic_store(&g_lms.authority_rewind_target, 0);
    atomic_store(&g_lms.csr_sqlite_rc, 0);
    atomic_store(&g_lms.last_advanced_height, 0);
    atomic_store(&g_lms.last_progress_blocks, 0);
    atomic_store(&g_lms.stuck_height, 0);
    atomic_store(&g_lms.stuck_status_flags, 0);
    atomic_store(&g_lms.stalls_total, 0);
    atomic_store(&g_lms.last_catchup, 0);
    atomic_store(&g_lms.last_attempt, 0);
    atomic_store(&g_lms.catchups_total, 0);
    atomic_store(&g_lms.rpc_errors, 0);
    atomic_store(&g_lms.blocks_applied, 0);
    atomic_store(&g_lms.headers_added, 0);
    atomic_store(&g_lms.lag_breach_since, 0);
    atomic_store(&g_lms.lag_critical_since, 0);
    atomic_store(&g_lms.lag_breach_emitted, 0);
    atomic_store(&g_lms.lag_critical_emitted, 0);
#ifdef ZCL_TESTING
    atomic_store(&g_lms_test_catchup_enabled, 0);
    atomic_store(&g_lms_test_catchup_result, 0);
    atomic_store(&g_lms_test_catchup_clear_stuck, 0);
    atomic_store(&g_lms_test_catchup_calls, 0);
    /* The divergence locator (check 6) fires from the lms verify path;
     * its blocker/HOLD/rate-limit must not leak across test cases. */
    mirror_divergence_reset_for_testing();
#endif
    mirror_consensus_reset_for_test();
}

#ifdef ZCL_TESTING
void legacy_mirror_sync_test_set_stats(
    const struct legacy_mirror_sync_stats *stats,
    struct main_state *ms)
{
    if (!stats)
        return;

    pthread_mutex_lock(&g_lms.lock);
    g_lms.initialized = true;
    g_lms.enabled = stats->enabled;
    g_lms_test_fake_running = stats->running;
    g_lms.ms = ms;
    snprintf(g_lms.zclassic23_hash, sizeof(g_lms.zclassic23_hash), "%s",
             stats->zclassic23_hash);
    snprintf(g_lms.zclassicd_hash, sizeof(g_lms.zclassicd_hash), "%s",
             stats->zclassicd_hash);
    snprintf(g_lms.stuck_reason, sizeof(g_lms.stuck_reason), "%s",
             stats->stuck_reason);
    snprintf(g_lms.last_blocker_id, sizeof(g_lms.last_blocker_id),
             "%s", stats->last_blocker_id);
    g_lms.last_blocker_class = stats->last_blocker_class;
    snprintf(g_lms.csr_failure_reason, sizeof(g_lms.csr_failure_reason),
             "%s", stats->csr_failure_reason);
    snprintf(g_lms.last_error, sizeof(g_lms.last_error), "%s",
             stats->last_error);
    pthread_mutex_unlock(&g_lms.lock);

    atomic_store(&g_lms.reachable, stats->reachable ? 1 : 0);
    atomic_store(&g_lms.in_flight, stats->in_flight ? 1 : 0);
    atomic_store(&g_lms.legacy_height, stats->legacy_height);
    atomic_store(&g_lms.legacy_headers, stats->legacy_headers);
    atomic_store(&g_lms.local_height, stats->local_height);
    atomic_store(&g_lms.best_header_height, stats->best_header_height);
    atomic_store(&g_lms.target_height, stats->target_height);
    atomic_store(&g_lms.authority_rewind_target,
                 stats->authority_rewind_target);
    atomic_store(&g_lms.csr_sqlite_rc, stats->csr_sqlite_rc);
    atomic_store(&g_lms.last_advanced_height,
                 stats->last_advanced_height);
    atomic_store(&g_lms.last_progress_blocks,
                 stats->last_progress_blocks);
    atomic_store(&g_lms.stuck_height, stats->stuck_height);
    atomic_store(&g_lms.stuck_status_flags, stats->stuck_status_flags);
    atomic_store(&g_lms.stalls_total, stats->stalls_total);
    atomic_store(&g_lms.last_catchup, stats->last_catchup);
    atomic_store(&g_lms.last_attempt, stats->last_attempt);
    atomic_store(&g_lms.catchups_total, stats->catchups_total);
    atomic_store(&g_lms.rpc_errors, stats->rpc_errors);
    atomic_store(&g_lms.blocks_applied, stats->blocks_applied);
    atomic_store(&g_lms.headers_added, stats->headers_added);
}
#endif
