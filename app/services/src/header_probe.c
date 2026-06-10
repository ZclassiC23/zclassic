/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Header Probe. See header for the high-level rationale.
 *
 * Layout:
 *   1. Config + creds
 *   2. header_probe_pull_range() — fetch + validate + insert
 *   3. header_probe_tick_once() — scheduler-independent Job body
 *   4. init + dump_state_json
 *
 * Threading: this module creates no background work. The supervised
 * header_probe_poll Job owns cadence and calls header_probe_tick_once().
 */

#include "services/header_probe.h"

#include "platform/clock.h"
#include "services/header_admit_inbox.h"
#include "jobs/stage_repair.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "validation/process_block.h"
#include "consensus/validation.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "core/uint256.h"
#include "primitives/block.h"
#include "json/json.h"
#include "rpc/legacy_header_client.h"
#include "rpc/legacy_rpc_client.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/result.h"
#include "util/safe_alloc.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ── Constants ─────────────────────────────────────────────────── */

#define HP_DEFAULT_HOST          "127.0.0.1"
#define HP_DEFAULT_PORT          8232
#define HP_DEFAULT_BATCH         2000
#define HP_DEFAULT_LAG           100
#define HP_MAX_BATCH             5000

/* ── Global state ──────────────────────────────────────────────── */

static struct {
    pthread_mutex_t lock;       /* guards config + non-atomic fields */
    bool   initialized;
    char   rpc_host[64];
    int    rpc_port;
    char   rpc_user[64];
    char   rpc_password[128];
    int    batch_size;
    int    lag_threshold;
    struct main_state *ms;
    const struct chain_params *params;

    /* Stats */
    _Atomic int64_t calls_total;
    _Atomic int64_t headers_added;
    _Atomic int64_t headers_rejected;
    _Atomic int64_t rpc_errors;
    _Atomic int     last_remote_height;
    _Atomic int     last_local_height;
} g_hp = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static void hp_publish_header_admit(const struct block_index *pindex,
                                    const struct block_header *header)
{
    if (!pindex || !pindex->phashBlock)
        return;

    struct header_admit_msg msg = {
        .height = pindex->nHeight,
        .hash = *pindex->phashBlock,
        .peer_id = 0,
        .observed_unix = clock_now_wall_ms() / 1000,
    };
    if (header) {
        struct uint256 header_hash;
        block_header_get_hash(header, &header_hash);
        if (uint256_eq(&header_hash, &msg.hash)) {
            msg.has_header = true;
            msg.header = *header;
        }
    }
    if (!mailbox_header_admit_push(&msg)) {
        LOG_INFO("header_probe", "[header_probe] header_admit inbox full; drop height=%d", pindex->nHeight);
    }
}

static void hp_save_repair_header(const struct block_index *pindex,
                                  const struct block_header *header)
{
    if (!pindex || !pindex->phashBlock || !header ||
        header->nSolutionSize == 0)
        return;
    sqlite3 *db = progress_store_db();
    if (!db)
        return;
    if (!stage_repair_header_solution_save(db, pindex->nHeight,
                                           pindex->phashBlock, header)) {
        LOG_WARN("header_probe",
                 "[header_probe] repair-header save failed height=%d",
                 pindex->nHeight);
    }
}

/* ── Public pull-range ─────────────────────────────────────────── */

struct zcl_result header_probe_pull_range(int start_height, int max_headers,
                                          int *out_added)
{
    if (out_added) *out_added = 0;
    if (start_height < 0) {
        return ZCL_ERR(-1, "pull_range: bad start_height=%d", start_height);
    }

    pthread_mutex_lock(&g_hp.lock);
    if (!g_hp.initialized || !g_hp.ms || !g_hp.params) {
        pthread_mutex_unlock(&g_hp.lock);
        return ZCL_ERR(-2, "pull_range: not initialized");
    }
    char host[64], user[64], pass[128];
    int port;
    snprintf(host, sizeof(host), "%s",
             g_hp.rpc_host[0] ? g_hp.rpc_host : HP_DEFAULT_HOST);
    port = g_hp.rpc_port ? g_hp.rpc_port : HP_DEFAULT_PORT;
    snprintf(user, sizeof(user), "%s", g_hp.rpc_user);
    snprintf(pass, sizeof(pass), "%s", g_hp.rpc_password);
    struct main_state *ms = g_hp.ms;
    const struct chain_params *params = g_hp.params;
    pthread_mutex_unlock(&g_hp.lock);

    atomic_fetch_add(&g_hp.calls_total, 1);

    /* Clamp batch size. */
    if (max_headers <= 0) max_headers = HP_DEFAULT_BATCH;
    if (max_headers > HP_MAX_BATCH) max_headers = HP_MAX_BATCH;

    /* Discover remote tip — bounds the loop and updates last_remote. */
    int remote_tip = -1;
    char err[160] = {0};
    if (!legacy_header_rpc_fetch_remote_tip(host, port, user, pass,
                                            &remote_tip, err, sizeof(err))) {
        atomic_fetch_add(&g_hp.rpc_errors, 1);
        /* Not a fatal logic failure — return OK with 0 added so the
         * MCP/test callers can distinguish "RPC unreachable" via state. */
        return ZCL_OK;
    }
    atomic_store(&g_hp.last_remote_height, remote_tip);

    /* Local tip (header tip is the high-water mark for headers). */
    int local_tip = 0;
    if (ms->pindex_best_header)
        local_tip = ms->pindex_best_header->nHeight;
    else
        local_tip = active_chain_height(&ms->chain_active);
    if (local_tip < 0) local_tip = 0;
    atomic_store(&g_hp.last_local_height, local_tip);

    int end_height = start_height + max_headers - 1;
    if (end_height > remote_tip) end_height = remote_tip;
    if (end_height < start_height) return ZCL_OK;  /* nothing to do */

    int added = 0;
    int h = start_height;
    /* Batched fast path: fetch headers via JSON-RPC arrays. Per-item accept
     * still validates PoW + chain link locally after the RPC helper
     * deserializes wire headers. */
    struct block_header *hbuf =
        zcl_malloc(sizeof(*hbuf) * LEGACY_HEADER_RPC_BATCH_MAX,
                   "hp_pullrange_hbuf");
    if (!hbuf) {
        return ZCL_ERR(-3, "pull_range: oom hbuf");
    }

    while (h <= end_height) {
        int n = end_height - h + 1;
        if (n > LEGACY_HEADER_RPC_BATCH_MAX)
            n = LEGACY_HEADER_RPC_BATCH_MAX;

        int parsed = 0;
        if (!legacy_header_rpc_fetch_batch(host, port, user, pass,
                                           h, n, hbuf, &parsed,
                                           err, sizeof(err))) {
            atomic_fetch_add(&g_hp.rpc_errors, 1);
            /* Batch failed — fall back to single-call for this one
             * header so we still make some progress and surface a
             * precise error message. */
            struct block_header hdr;
            if (!legacy_header_rpc_fetch_one(host, port, user, pass, h,
                                             &hdr, err, sizeof(err))) {
                atomic_fetch_add(&g_hp.rpc_errors, 1);
                break;
            }
            struct validation_state vs;
            validation_state_init(&vs);
            struct block_index *pindex = NULL;
            if (accept_block_header(&hdr, &vs, ms, params, &pindex)) {
                atomic_fetch_add(&g_hp.headers_added, 1);
                added++;
                hp_save_repair_header(pindex, &hdr);
                hp_publish_header_admit(pindex, &hdr);
                if (pindex && pindex->nHeight > 0)
                    atomic_store(&g_hp.last_local_height,
                                 pindex->nHeight);
                h++;
                continue;
            }
            atomic_fetch_add(&g_hp.headers_rejected, 1);
            break;
        }

        bool reject = false;
        for (int i = 0; i < parsed; i++) {
            struct validation_state vs;
            validation_state_init(&vs);
            struct block_index *pindex = NULL;
            if (accept_block_header(&hbuf[i], &vs, ms, params, &pindex)) {
                atomic_fetch_add(&g_hp.headers_added, 1);
                added++;
                hp_save_repair_header(pindex, &hbuf[i]);
                hp_publish_header_admit(pindex, &hbuf[i]);
                if (pindex && pindex->nHeight > 0)
                    atomic_store(&g_hp.last_local_height,
                                 pindex->nHeight);
            } else {
                atomic_fetch_add(&g_hp.headers_rejected, 1);
                reject = true;
                break;
            }
        }
        if (reject) break;
        if (parsed < n) {
            /* Partial batch — surface per-item decode/deserialize
             * failures as RPC errors so callers see the same
             * "something went wrong" signal as the single-call path. */
            atomic_fetch_add(&g_hp.rpc_errors, (int64_t)(n - parsed));
            break;
        }
        h += parsed;
    }
    free(hbuf);

    if (out_added) *out_added = added;
    return ZCL_OK;
}

/* ── Poll tick body used by the supervised header_probe_poll Job ── */

void header_probe_tick_once(void)
{
    pthread_mutex_lock(&g_hp.lock);
    bool inited = g_hp.initialized;
    struct main_state *ms = g_hp.ms;
    int lag_thresh = g_hp.lag_threshold > 0
                         ? g_hp.lag_threshold : HP_DEFAULT_LAG;
    int batch = g_hp.batch_size > 0 ? g_hp.batch_size : HP_DEFAULT_BATCH;
    char host[64], user[64], pass[128];
    int port;
    snprintf(host, sizeof(host), "%s",
             g_hp.rpc_host[0] ? g_hp.rpc_host : HP_DEFAULT_HOST);
    port = g_hp.rpc_port ? g_hp.rpc_port : HP_DEFAULT_PORT;
    snprintf(user, sizeof(user), "%s", g_hp.rpc_user);
    snprintf(pass, sizeof(pass), "%s", g_hp.rpc_password);
    pthread_mutex_unlock(&g_hp.lock);
    if (!inited || !ms) return;

    int local_tip = 0;
    if (ms->pindex_best_header)
        local_tip = ms->pindex_best_header->nHeight;
    else
        local_tip = active_chain_height(&ms->chain_active);
    if (local_tip < 0) local_tip = 0;

    /* Cheap getblockcount to decide whether to pull. */
    int remote_tip = -1;
    char err[160] = {0};
    if (!legacy_header_rpc_fetch_remote_tip(host, port, user, pass,
                                            &remote_tip, err, sizeof(err))) {
        atomic_fetch_add(&g_hp.rpc_errors, 1);
        return;
    }
    atomic_store(&g_hp.last_remote_height, remote_tip);
    atomic_store(&g_hp.last_local_height, local_tip);

    if (remote_tip <= local_tip + lag_thresh) return;  /* under-lag */

    int added = 0;
    (void)header_probe_pull_range(local_tip + 1, batch, &added);  /* result ignored — RPC errors surface via state counters */
}

/* ── init ──────────────────────────────────────────────────────── */

struct zcl_result header_probe_init(const struct header_probe_config *cfg,
                                    struct main_state *ms,
                                    const struct chain_params *params)
{
    pthread_mutex_lock(&g_hp.lock);

    snprintf(g_hp.rpc_host, sizeof(g_hp.rpc_host), "%s",
             (cfg && cfg->rpc_host) ? cfg->rpc_host : HP_DEFAULT_HOST);
    g_hp.rpc_port = (cfg && cfg->rpc_port > 0)
                        ? cfg->rpc_port : HP_DEFAULT_PORT;
    g_hp.batch_size = (cfg && cfg->batch_size > 0)
                        ? cfg->batch_size : HP_DEFAULT_BATCH;
    if (g_hp.batch_size > HP_MAX_BATCH) g_hp.batch_size = HP_MAX_BATCH;
    g_hp.lag_threshold = (cfg && cfg->lag_threshold > 0)
                        ? cfg->lag_threshold : HP_DEFAULT_LAG;
    g_hp.ms = ms;
    g_hp.params = params;

    if (cfg && cfg->rpc_user && cfg->rpc_user[0]) {
        snprintf(g_hp.rpc_user, sizeof(g_hp.rpc_user),
                 "%s", cfg->rpc_user);
    }
    if (cfg && cfg->rpc_password && cfg->rpc_password[0]) {
        snprintf(g_hp.rpc_password, sizeof(g_hp.rpc_password),
                 "%s", cfg->rpc_password);
    }

    bool need_user = (g_hp.rpc_user[0] == '\0');
    bool need_pass = (g_hp.rpc_password[0] == '\0');
    if (need_user || need_pass) {
        int port_from_conf = g_hp.rpc_port;
        char u[64] = {0}, p[128] = {0};
        if (legacy_rpc_parse_conf(u, sizeof(u), p, sizeof(p),
                                  &port_from_conf)) {
            if (need_user)
                snprintf(g_hp.rpc_user, sizeof(g_hp.rpc_user), "%s", u);
            if (need_pass)
                snprintf(g_hp.rpc_password, sizeof(g_hp.rpc_password),
                         "%s", p);
            if (!cfg || cfg->rpc_port <= 0)
                g_hp.rpc_port = port_from_conf;
        } else if (need_user || need_pass) {
            pthread_mutex_unlock(&g_hp.lock);
            return ZCL_ERR(-1,
                "no RPC credentials: pass via config or ~/.zclassic/zclassic.conf");
        }
    }

    g_hp.initialized = true;
    pthread_mutex_unlock(&g_hp.lock);
    return ZCL_OK;
}

void header_probe_reset_for_test(void)
{
    pthread_mutex_lock(&g_hp.lock);
    g_hp.initialized = false;
    g_hp.rpc_host[0] = '\0';
    g_hp.rpc_port = 0;
    g_hp.rpc_user[0] = '\0';
    g_hp.rpc_password[0] = '\0';
    g_hp.batch_size = 0;
    g_hp.lag_threshold = 0;
    g_hp.ms = NULL;
    g_hp.params = NULL;
    atomic_store(&g_hp.calls_total, 0);
    atomic_store(&g_hp.headers_added, 0);
    atomic_store(&g_hp.headers_rejected, 0);
    atomic_store(&g_hp.rpc_errors, 0);
    atomic_store(&g_hp.last_remote_height, 0);
    atomic_store(&g_hp.last_local_height, 0);
    pthread_mutex_unlock(&g_hp.lock);
}

/* ── State dump (see CLAUDE.md "Adding state introspection") ───── */

bool header_probe_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    pthread_mutex_lock(&g_hp.lock);
    bool initialized = g_hp.initialized;
    pthread_mutex_unlock(&g_hp.lock);

    json_push_kv_bool(out, "initialized",        initialized);
    json_push_kv_int (out, "calls_total",
                      atomic_load(&g_hp.calls_total));
    json_push_kv_int (out, "headers_added",
                      atomic_load(&g_hp.headers_added));
    json_push_kv_int (out, "headers_rejected",
                      atomic_load(&g_hp.headers_rejected));
    json_push_kv_int (out, "rpc_errors",
                      atomic_load(&g_hp.rpc_errors));
    json_push_kv_int (out, "last_remote_height",
                      atomic_load(&g_hp.last_remote_height));
    json_push_kv_int (out, "last_local_height",
                      atomic_load(&g_hp.last_local_height));
    return true;
}
