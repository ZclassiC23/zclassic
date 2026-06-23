/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Always-on legacy mirror MONITOR. The staged sync pipeline is the
 * authoritative block writer, so this service no longer applies blocks.
 * It only observes lag against a sibling
 * zclassicd: fetch chain-info, compute lag, evaluate the lag-SLO,
 * verify anchor/tip hashes agree, cache hashes, and surface a blocker
 * when the local tip is behind. The supervisor tick, lag-SLO monitor,
 * and stats/introspection feed health, metrics, conditions, and the
 * liveness tree.
 */

#include "platform/time_compat.h"
#include "services/legacy_mirror_sync_service.h"
#include "legacy_mirror_sync_internal.h"

#include "services/header_probe.h"
#include "services/mirror_divergence_locator.h"
#include "services/oracle_policy.h"
#include "services/sync_monitor.h"

#include "supervisors/legacy_mirror_supervisor.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "validation/mirror_consensus.h"
#include "chain/chain.h"
#include "core/uint256.h"
#include "json/json.h"
#include "rpc/legacy_rpc_client.h"
#include "util/log_macros.h"
#include "event/event.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

struct legacy_mirror_sync_runtime g_lms = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .flight = PTHREAD_MUTEX_INITIALIZER,
};

#ifdef ZCL_TESTING
bool g_lms_test_fake_running;
_Atomic int g_lms_test_catchup_enabled;
_Atomic int g_lms_test_catchup_result;
_Atomic int g_lms_test_catchup_clear_stuck;
_Atomic int g_lms_test_catchup_calls;
#endif

void lms_set_error(const char *msg)
{
    pthread_mutex_lock(&g_lms.lock);
    snprintf(g_lms.last_error, sizeof(g_lms.last_error), "%s",
             msg ? msg : "");
    pthread_mutex_unlock(&g_lms.lock);
}

static void lms_record_blocker(const char *code, const char *msg)
{
    pthread_mutex_lock(&g_lms.lock);
    g_lms.last_blocker_class =
        mirror_consensus_classify_blocker_reason(code);
    snprintf(g_lms.last_blocker_id, sizeof(g_lms.last_blocker_id),
             "%s", code ? code : "");
    snprintf(g_lms.last_error, sizeof(g_lms.last_error), "%s",
             msg ? msg : "");
    pthread_mutex_unlock(&g_lms.lock);
    if (code && code[0])
        mirror_consensus_record_blocker(code);
}

static void lms_clear_csr_failure(void)
{
    atomic_store(&g_lms.csr_sqlite_rc, 0);
    pthread_mutex_lock(&g_lms.lock);
    g_lms.csr_failure_reason[0] = '\0';
    pthread_mutex_unlock(&g_lms.lock);
}

static void lms_set_stuck_reason(const char *reason)
{
    bool count_stall = reason && reason[0];
    pthread_mutex_lock(&g_lms.lock);
    if (count_stall && strcmp(g_lms.stuck_reason, reason) == 0)
        count_stall = false;
    snprintf(g_lms.stuck_reason, sizeof(g_lms.stuck_reason),
             "%s", reason ? reason : "");
    pthread_mutex_unlock(&g_lms.lock);
    if (count_stall)
        atomic_fetch_add(&g_lms.stalls_total, 1);
}

static void lms_clear_blocker(void)
{
    pthread_mutex_lock(&g_lms.lock);
    g_lms.last_blocker_class = BLOCKER_TRANSIENT;
    g_lms.last_blocker_id[0] = '\0';
    g_lms.last_error[0] = '\0';
    pthread_mutex_unlock(&g_lms.lock);
    /* mirror_consensus_clear_blocker was deleted in F-1e — the
     * mirror.* blocker IDs that wired to the typed blocker registry
     * via mirror_consensus_record_blocker auto-expire via
     * BLOCKER_TRANSIENT classification. The cryptographic-mismatch
     * (PERMANENT) blockers were never intended to auto-clear. */
}

int lms_env_int(const char *name, int fallback, int min, int max)
{
    const char *s = getenv(name);
    if (!s || !s[0]) return fallback;
    int n = atoi(s);
    if (n < min) return fallback;
    if (n > max) return max;
    return n;
}

bool lms_env_disabled(void)
{
    const char *s = getenv("ZCL_MIRROR_SYNC");
    return s && (!strcmp(s, "0") || !strcasecmp(s, "false") ||
                 !strcasecmp(s, "off") || !strcasecmp(s, "no"));
}

static bool lms_parse_int_result(const char *raw, const char *key,
                                 int *out, char *err, size_t err_sz)
{
    const char *body = legacy_rpc_http_body(raw);
    if (!body) {
        snprintf(err, err_sz, "no http body separator");
        return false;
    }
    struct json_value v = {0};
    if (!json_read(&v, body, strlen(body))) {
        snprintf(err, err_sz, "json parse failed");
        json_free(&v);
        return false;
    }
    const struct json_value *result = json_get(&v, "result");
    const struct json_value *field = key && result ? json_get(result, key)
                                                   : result;
    if (!field || field->type != JSON_INT) {
        const struct json_value *jerr = json_get(&v, "error");
        if (jerr && jerr->type == JSON_OBJ) {
            const struct json_value *code = json_get(jerr, "code");
            const struct json_value *msg = json_get(jerr, "message");
            if (code && code->type == JSON_INT &&
                msg && msg->type == JSON_STR) {
                snprintf(err, err_sz, "rpc error %lld: %s",
                         (long long)json_get_int(code), json_get_str(msg));
                json_free(&v);
                return false;
            }
            if (msg && msg->type == JSON_STR) {
                snprintf(err, err_sz, "rpc error: %s", json_get_str(msg));
                json_free(&v);
                return false;
            }
            if (code && code->type == JSON_INT) {
                snprintf(err, err_sz, "rpc error %lld",
                         (long long)json_get_int(code));
                json_free(&v);
                return false;
            }
        }
        snprintf(err, err_sz, "missing int result%s%s",
                 key ? "." : "", key ? key : "");
        json_free(&v);
        return false;
    }
    int64_t n = json_get_int(field);
    if (n < 0 || n > 0x7fffffff) {
        snprintf(err, err_sz, "height out of range");
        json_free(&v);
        return false;
    }
    *out = (int)n;
    json_free(&v);
    return true;
}

/* Parse a JSON-RPC `.result` string and require exactly 64 hex chars
 * (a block hash). Thin wrapper over the shared transport parser so the
 * "getblockhash result + 64-hex validation" idiom is not reimplemented
 * here. */
static bool lms_parse_hash_result(const char *raw, char out_hex[65],
                                  char *err, size_t err_sz)
{
    if (!legacy_rpc_parse_result_string(raw, out_hex, 65, err, err_sz))
        return false;
    if (strlen(out_hex) != 64) {
        snprintf(err, err_sz, "hash result is not 64 hex chars");
        return false;
    }
    return true;
}

static bool lms_rpc_call(const char *method_params, char **out_resp,
                         char *err, size_t err_sz)
{
    char host[64], user[64], pass[128];
    int port;
    pthread_mutex_lock(&g_lms.lock);
    snprintf(host, sizeof(host), "%s", g_lms.rpc_host);
    port = g_lms.rpc_port;
    snprintf(user, sizeof(user), "%s", g_lms.rpc_user);
    snprintf(pass, sizeof(pass), "%s", g_lms.rpc_password);
    pthread_mutex_unlock(&g_lms.lock);

    return legacy_rpc_call(host, port, user, pass, method_params,
                           out_resp, err, err_sz);
}

static bool lms_fetch_chain_info(int *out_blocks, int *out_headers,
                                 char *err, size_t err_sz)
{
    char *resp = NULL;
    const char *body =
        "{\"jsonrpc\":\"1.0\",\"id\":\"zcl-mirror\","
        "\"method\":\"getblockchaininfo\",\"params\":[]}";
    if (!lms_rpc_call(body, &resp, err, err_sz))
        return false;
    int blocks = -1, headers = -1;
    bool ok_b = lms_parse_int_result(resp, "blocks", &blocks, err, err_sz);
    if (!ok_b) {
        free(resp);
        return false;
    }
    bool ok_h = lms_parse_int_result(resp, "headers", &headers, err, err_sz);
    free(resp);
    if (!ok_h) {
        headers = blocks;
        if (err && err_sz)
            err[0] = '\0';
    }
    *out_blocks = blocks;
    *out_headers = headers;
    return true;
}

static bool lms_fetch_hash(int height, char out_hex[65],
                           char *err, size_t err_sz)
{
    char body[160];
    snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"1.0\",\"id\":\"zcl-mirror\","
        "\"method\":\"getblockhash\",\"params\":[%d]}", height);
    char *resp = NULL;
    if (!lms_rpc_call(body, &resp, err, err_sz))
        return false;
    bool ok = lms_parse_hash_result(resp, out_hex, err, err_sz);
    free(resp);
    return ok;
}

bool lms_remote_hash_at(int height, char out_hex[65])
{
    char err[160] = {0};
    if (!lms_fetch_hash(height, out_hex, err, sizeof(err))) {
        atomic_fetch_add(&g_lms.rpc_errors, 1);
        return false;
    }
    return true;
}

bool lms_local_hash_at(int height, char out_hex[65])
{
    out_hex[0] = '\0';
    struct main_state *ms = g_lms.ms;
    if (!ms || height < 0) return false;
    zcl_mutex_lock(&ms->cs_main);
    struct block_index *bi = active_chain_at(&ms->chain_active, height);
    if (!bi) {
        bi = active_chain_tip(&ms->chain_active);
        int steps = 0;
        while (bi && bi->nHeight > height && steps <= 2050) {
            bi = bi->pprev;
            steps++;
        }
        if (bi && bi->nHeight != height)
            bi = NULL;
    }
    if (bi && bi->phashBlock)
        uint256_get_hex(bi->phashBlock, out_hex);
    zcl_mutex_unlock(&ms->cs_main);
    return out_hex[0] != '\0';
}

static bool lms_verify_anchor(int height)
{
    if (height < 0) return true;
    char local[65], remote[65], err[160] = {0};
    if (!lms_local_hash_at(height, local))
        return true;
    if (!lms_fetch_hash(height, remote, err, sizeof(err))) {
        atomic_fetch_add(&g_lms.rpc_errors, 1);
        lms_record_blocker("rpc-unreachable", err);
        return false;
    }
    if (strcasecmp(local, remote) != 0) {
        oracle_policy_record_disagreement(height, local, remote);
        lms_record_blocker("hash-disagreement", "legacy hash disagreement");
        /* Validation pack check 6: locate the FIRST diverging height and
         * page once with it — never 279 identical quiet warnings again.
         * Rate-limited inside; aborts silently on RPC errors; a tip-window
         * divergence (healthy transient fork) is NOT escalated until it
         * confirms at depth or persists. */
        (void)mirror_divergence_locate(height);
        return false;
    }
    /* Agreement self-clears any pending/located divergence at or below
     * this height (the fork resolved / the mirror reorged to us). */
    mirror_divergence_note_agreement(height);
    return true;
}

static bool lms_verify_after_tip(int height)
{
    char local[65], remote[65], err[160] = {0};
    if (!lms_local_hash_at(height, local)) {
        lms_set_error("local tip hash unavailable after catchup");
        return false;
    }
    if (!lms_fetch_hash(height, remote, err, sizeof(err))) {
        atomic_fetch_add(&g_lms.rpc_errors, 1);
        lms_record_blocker("rpc-unreachable", err);
        return false;
    }
    if (strcasecmp(local, remote) != 0) {
        oracle_policy_record_disagreement(height, local, remote);
        lms_record_blocker("hash-disagreement", "post-catchup tip disagreement");
        /* Validation pack check 6 — see lms_verify_anchor. */
        (void)mirror_divergence_locate(height);
        return false;
    }
    mirror_divergence_note_agreement(height);
    return true;
}

static void lms_cache_hashes(const char *local, const char *remote)
{
    pthread_mutex_lock(&g_lms.lock);
    if (local)
        snprintf(g_lms.zclassic23_hash, sizeof(g_lms.zclassic23_hash),
                 "%s", local);
    if (remote)
        snprintf(g_lms.zclassicd_hash, sizeof(g_lms.zclassicd_hash),
                 "%s", remote);
    pthread_mutex_unlock(&g_lms.lock);
}

static int lms_header_probe_last_local_height(void)
{
    struct json_value dump;
    json_init(&dump);
    int h = -1;
    if (header_probe_dump_state_json(&dump, NULL)) {
        const struct json_value *v = json_get(&dump, "last_local_height");
        if (v && v->type == JSON_INT)
            h = (int)json_get_int(v);
    }
    json_free(&dump);
    return h;
}

void lms_refresh_local_heights(int *out_local, int *out_header)
{
    int local = -1, hdr = -1;
    struct main_state *ms = g_lms.ms;
    if (ms) {
        zcl_mutex_lock(&ms->cs_main);
        local = active_chain_height(&ms->chain_active);
        hdr = ms->pindex_best_header ? ms->pindex_best_header->nHeight
                                      : local;
        zcl_mutex_unlock(&ms->cs_main);
    }
    {
        int hp_height = lms_header_probe_last_local_height();
        if (hp_height > hdr)
            hdr = hp_height;
    }
    atomic_store(&g_lms.local_height, local);
    atomic_store(&g_lms.best_header_height, hdr);
    if (out_local) *out_local = local;
    if (out_header) *out_header = hdr;
}

static void lms_record_stuck_status(int height)
{
    unsigned int flags = 0;
    if (height >= 0 && g_lms.ms) {
        char remote[65] = {0};
        char err[160] = {0};
        if (lms_fetch_hash(height, remote, err, sizeof(err))) {
            struct uint256 hash;
            uint256_set_hex(&hash, remote);
            zcl_mutex_lock(&g_lms.ms->cs_main);
            struct block_index *bi =
                block_map_find(&g_lms.ms->map_block_index, &hash);
            if (!bi) {
                lms_set_stuck_reason("no-authorized-child");
            } else {
                flags = bi->nStatus;
                if (!(bi->nStatus & BLOCK_HAVE_DATA))
                    lms_set_stuck_reason("missing-have-data");
                else if (bi->nStatus & BLOCK_FAILED_MASK)
                    lms_set_stuck_reason("failed-mask");
                else
                    lms_set_stuck_reason("activation-state");
            }
            zcl_mutex_unlock(&g_lms.ms->cs_main);
        }
    }
    atomic_store(&g_lms.stuck_height, height);
    atomic_store(&g_lms.stuck_status_flags, flags);
}

/* Track lag-SLO breach episodes and emit EV_LAG_SLO_BREACH at most
 * once per (episode, severity). Called from the catchup tick after
 * each fresh lag reading; safe under the single-flight lock.
 *
 * Severity ladder:
 *   under breach              → severity=none, clear timers
 *   lag ≥ breach_blocks       → start/continue breach timer
 *      ≥ breach_secs sustained → severity=critical (one emit)
 *   lag ≥ critical_blocks     → start/continue critical timer
 *      ≥ critical_secs sustained → severity=fatal (one emit; node_health flips) */
static void lms_evaluate_lag_slo(int lag, int legacy_height, int local_height,
                                 int64_t now)
{
    int breach_blocks   = atomic_load(&g_lms.lag_sla_breach_blocks);
    int breach_secs     = atomic_load(&g_lms.lag_sla_breach_secs);
    int critical_blocks = atomic_load(&g_lms.lag_sla_critical_blocks);
    int critical_secs   = atomic_load(&g_lms.lag_sla_critical_secs);

    if (breach_blocks <= 0) {
        atomic_store(&g_lms.lag_breach_since, 0);
        atomic_store(&g_lms.lag_critical_since, 0);
        atomic_store(&g_lms.lag_breach_emitted, 0);
        atomic_store(&g_lms.lag_critical_emitted, 0);
        return;
    }

    if (lag < breach_blocks) {
        /* Recovered. Reset both timers + emission latches. */
        atomic_store(&g_lms.lag_breach_since, 0);
        atomic_store(&g_lms.lag_critical_since, 0);
        atomic_store(&g_lms.lag_breach_emitted, 0);
        atomic_store(&g_lms.lag_critical_emitted, 0);
        return;
    }

    int64_t since = atomic_load(&g_lms.lag_breach_since);
    if (since == 0) {
        atomic_store(&g_lms.lag_breach_since, now);
        since = now;
    }
    int64_t breach_for = now - since;

    if (breach_for >= breach_secs &&
        !atomic_exchange(&g_lms.lag_breach_emitted, 1)) {
        event_emitf(EV_LAG_SLO_BREACH, 0,
                    "lag=%d legacy_height=%d local_height=%d "
                    "since=%llds severity=critical",
                    lag, legacy_height, local_height,
                    (long long)breach_for);
    }

    if (critical_blocks > 0 && lag >= critical_blocks) {
        int64_t csince = atomic_load(&g_lms.lag_critical_since);
        if (csince == 0) {
            atomic_store(&g_lms.lag_critical_since, now);
            csince = now;
        }
        int64_t crit_for = now - csince;
        if (crit_for >= critical_secs &&
            !atomic_exchange(&g_lms.lag_critical_emitted, 1)) {
            event_emitf(EV_LAG_SLO_BREACH, 0,
                        "lag=%d legacy_height=%d local_height=%d "
                        "since=%llds severity=fatal",
                        lag, legacy_height, local_height,
                        (long long)crit_for);
        }
    } else {
        atomic_store(&g_lms.lag_critical_since, 0);
        atomic_store(&g_lms.lag_critical_emitted, 0);
    }
}

static void lms_observe_local_primary(int local, int legacy_blocks)
{
    atomic_store(&g_lms.target_height, legacy_blocks);
    atomic_store(&g_lms.last_progress_blocks, 0);
    atomic_store(&g_lms.last_advanced_height, local);
    lms_set_error("local sync primary; mirror observing");
}

static void lms_mark_success(int local, int progress)
{
    if (progress < 0)
        progress = 0;
    atomic_store(&g_lms.last_progress_blocks, progress);
    atomic_store(&g_lms.last_catchup, (int64_t)platform_time_wall_time_t());
    atomic_store(&g_lms.last_advanced_height, local);
    atomic_store(&g_lms.stuck_height, 0);
    atomic_store(&g_lms.stuck_status_flags, 0);
    lms_set_stuck_reason("");
    lms_clear_csr_failure();
    lms_clear_blocker();
}

bool legacy_mirror_sync_request_catchup(const char *reason)
{
    (void)reason;
#ifdef ZCL_TESTING
    if (atomic_load(&g_lms_test_catchup_enabled)) {
        atomic_fetch_add(&g_lms_test_catchup_calls, 1);
        if (atomic_load(&g_lms_test_catchup_clear_stuck)) {
            atomic_store(&g_lms.stuck_height, 0);
            atomic_store(&g_lms.stuck_status_flags, 0);
            pthread_mutex_lock(&g_lms.lock);
            g_lms.stuck_reason[0] = '\0';
            pthread_mutex_unlock(&g_lms.lock);
        }
        return atomic_load(&g_lms_test_catchup_result) != 0;
    }
#endif
    if (!g_lms.initialized || !g_lms.enabled)
        return true;
    if (pthread_mutex_trylock(&g_lms.flight) != 0)
        return true;

    atomic_store(&g_lms.in_flight, 1);
    atomic_store(&g_lms.last_attempt, (int64_t)platform_time_wall_time_t());

    bool ok = true;
    int legacy_blocks = -1, legacy_headers = -1;
    char err[160] = {0};
    if (!lms_fetch_chain_info(&legacy_blocks, &legacy_headers,
                              err, sizeof(err))) {
        atomic_store(&g_lms.reachable, 0);
        atomic_fetch_add(&g_lms.rpc_errors, 1);
        lms_record_blocker("rpc-unreachable", err);
        ok = false;
        goto out;
    }
    atomic_store(&g_lms.reachable, 1);
    atomic_store(&g_lms.legacy_height, legacy_blocks);
    atomic_store(&g_lms.legacy_headers, legacy_headers);

    int local = -1, hdr = -1;
    lms_refresh_local_heights(&local, &hdr);
    int lag = legacy_blocks - local;

    /* SLO evaluation runs every tick regardless of gating — the loud
     * half of the redundancy guarantee. Severity is emitted once per
     * episode (latched), cleared when lag drops back below threshold. */
    lms_evaluate_lag_slo(lag, legacy_blocks, local, (int64_t)platform_time_wall_time_t());

    {
        char local_hash[65] = {0}, remote_hash[65] = {0};
        char hash_err[160] = {0};
        if (lms_local_hash_at(local, local_hash))
            lms_cache_hashes(local_hash, NULL);
        if (legacy_blocks >= 0 &&
            lms_fetch_hash(legacy_blocks, remote_hash,
                           hash_err, sizeof(hash_err))) {
            lms_cache_hashes(NULL, remote_hash);
        }
    }

    if (legacy_blocks < local) {
        atomic_store(&g_lms.target_height, legacy_blocks);
        lms_mark_success(local, 0);
        lms_set_error("mirror behind local; observing");
        ok = true;
        goto out;
    }

    if (!lms_verify_anchor(local)) {
        ok = false;
        goto out;
    }
    if (lag <= g_lms.lag_sla) {
        atomic_store(&g_lms.target_height, legacy_blocks);
        if (local == legacy_blocks && !lms_verify_after_tip(local)) {
            ok = false;
            goto out;
        }
        int prev_advanced = atomic_load(&g_lms.last_advanced_height);
        if (local >= prev_advanced)
            atomic_fetch_add(&g_lms.catchups_total, 1);
        lms_mark_success(local, local - prev_advanced);
        ok = true;
        goto out;
    }
    if (lag > 4) {
        int sample = local + lag / 2;
        if (sample < legacy_blocks && !lms_verify_anchor(sample)) {
            ok = false;
            goto out;
        }
    }

    /* Monitor-only: the stage pipeline is the authoritative
     * block writer. The mirror no longer applies blocks; it observes
     * the lag, records the stuck status when behind, and lets the
     * native pipeline advance the tip. */
    atomic_store(&g_lms.target_height, legacy_blocks);
    lms_record_stuck_status(local + 1);
    lms_observe_local_primary(local, legacy_blocks);
    ok = true;
    goto out;

out:
    lms_refresh_local_heights(NULL, NULL);
    atomic_store(&g_lms.in_flight, 0);
    pthread_mutex_unlock(&g_lms.flight);
    return ok;
}

struct zcl_result lms_request_catchup_result_internal(const char *reason)
{
    if (legacy_mirror_sync_request_catchup(reason))
        return ZCL_OK;
    struct legacy_mirror_sync_stats s;
    legacy_mirror_sync_stats_snapshot(&s);
    struct zcl_result r = ZCL_ERR(-1,
        "legacy_mirror catchup failed reason=%s blocker=%s error=%s",
        reason ? reason : "", s.last_blocker_id[0] ? s.last_blocker_id : "",
        s.last_error[0] ? s.last_error : "");
    LOG_WARN("legacy_mirror", "%s", r.message);
    return r;
}
