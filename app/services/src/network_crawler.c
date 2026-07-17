/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * network_crawler service — see services/network_crawler.h. A supervised worker
 * walks the full local address table (addrman_get_addr), dials a bounded,
 * rate-limited batch of addresses per round through SHORT-LIVED measurement
 * sockets OUTSIDE the node's connman (the injectable probe_fn seam), records a
 * bounded/pruned census, and folds it into a whole-network view: reachable
 * count, version histogram, height distribution, onion/clearnet split, and an
 * eclipse signal (our connected-peer modal height vs the crawled network's
 * modal height). OPT-IN — off unless enabled; the worker still registers and
 * idles when disabled (named degradation). Observational only; never relays,
 * syncs, or touches chain selection.
 */

// one-result-type-ok:network-crawler-query-accessors — the fallible service
// surface (network_crawler_start) returns struct zcl_result; the remaining
// bool exports (network_census_compute is void, get_view/dump/default_probe +
// test helpers are pure predicates) are not fallible operations.

#include "services/network_crawler.h"
#include "services/network_monitor.h"

#include "net/addrman.h"
#include "storage/peers_projection.h"
#include "storage/event_log_payloads.h"

#include "json/json.h"
#include "platform/time_compat.h"
#include "util/log_macros.h"
#include "util/supervisor.h"
#include "util/sync.h"
#include "util/thread_registry.h"
#include "supervisors/domains.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NCRAWL_SUPERVISOR_DEADLINE_SEC 300

static struct {
    zcl_mutex_t lock;
    bool started;
    bool enabled;

    struct addr_man *addrman;
    ncrawl_probe_fn probe_fn;
    int round_interval_secs;
    int max_per_round;
    int max_concurrent;
    int connect_timeout_ms;
    int handshake_timeout_ms;

    /* bounded/pruned census (source of truth for the fold + dumper) */
    struct ncrawl_probe_result census[NCRAWL_MAX_CENSUS];
    int census_count;
    struct network_census_view view;

    /* round stats */
    int64_t rounds_run;
    int probed_last_round;
    int64_t addresses_known;
    int64_t last_round_unix;

    /* supervised worker */
    pthread_t thread;
    _Atomic bool stop_requested;
    bool thread_running;
    _Atomic int64_t loop_ticks;
    _Atomic supervisor_child_id supervisor_id;
#ifdef ZCL_TESTING
    _Atomic int64_t test_own_modal;   /* INT64_MIN = unset */
#endif
} g_ncrawl = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .probe_fn = network_crawler_default_probe,
};

static struct liveness_contract g_ncrawl_contract;

/* ── config ──────────────────────────────────────────────────────────── */

void network_crawler_config_defaults(struct network_crawler_config *cfg)
{
    if (!cfg)
        return;
    /* NET-2: ON by default — the eclipse blocker (net_eclipse_suspected) needs
     * the wider-network census to detect that our connected peers are a small
     * minority. The existing rate limits (bounded probe batches on short-lived
     * measurement sockets outside connman) are unchanged. Opt OUT with
     * ZCL_NETWORK_CRAWLER=0. */
    cfg->enabled = true;
    cfg->round_interval_secs = NCRAWL_ROUND_INTERVAL_SECS_DEFAULT;
    cfg->max_per_round = NCRAWL_MAX_PER_ROUND;
    cfg->max_concurrent = NCRAWL_MAX_CONCURRENT;
    cfg->connect_timeout_ms = NCRAWL_CONNECT_TIMEOUT_MS_DEFAULT;
    cfg->handshake_timeout_ms = NCRAWL_HANDSHAKE_TIMEOUT_MS_DEFAULT;
}

static bool ncrawl_env_truthy(const char *e)
{
    return e && (e[0] == '1' || e[0] == 't' || e[0] == 'T' ||
                 e[0] == 'y' || e[0] == 'Y');
}

/* Explicit opt-out truthiness: 0/f/F/n/N disables. */
static bool ncrawl_env_falsy(const char *e)
{
    return e && (e[0] == '0' || e[0] == 'f' || e[0] == 'F' ||
                 e[0] == 'n' || e[0] == 'N');
}

static void ncrawl_config_from_env(struct network_crawler_config *cfg)
{
    /* ON by default; ZCL_NETWORK_CRAWLER is now the opt-OUT knob. An explicit
     * falsy value disables; any truthy value re-enables (idempotent). */
    const char *en = getenv("ZCL_NETWORK_CRAWLER");
    if (ncrawl_env_falsy(en))
        cfg->enabled = false;
    else if (ncrawl_env_truthy(en))
        cfg->enabled = true;
    const char *iv = getenv("ZCL_NETCRAWL_INTERVAL_SECS");
    if (iv && iv[0]) {
        int v = atoi(iv);
        if (v >= 5 && v <= 86400)
            cfg->round_interval_secs = v;
    }
    const char *pr = getenv("ZCL_NETCRAWL_MAX_PER_ROUND");
    if (pr && pr[0]) {
        int v = atoi(pr);
        if (v >= 1 && v <= NCRAWL_MAX_PER_ROUND)
            cfg->max_per_round = v;
    }
    const char *mc = getenv("ZCL_NETCRAWL_MAX_CONCURRENT");
    if (mc && mc[0]) {
        int v = atoi(mc);
        if (v >= 1 && v <= NCRAWL_MAX_CONCURRENT)
            cfg->max_concurrent = v;
    }
}

/* Clamp every knob into its hard bound (defensive: env + cfg both untrusted). */
static void ncrawl_clamp(struct network_crawler_config *c)
{
    if (c->round_interval_secs < 5) c->round_interval_secs = 5;
    if (c->round_interval_secs > 86400) c->round_interval_secs = 86400;
    if (c->max_per_round < 1) c->max_per_round = 1;
    if (c->max_per_round > NCRAWL_MAX_PER_ROUND) c->max_per_round = NCRAWL_MAX_PER_ROUND;
    if (c->max_concurrent < 1) c->max_concurrent = 1;
    if (c->max_concurrent > NCRAWL_MAX_CONCURRENT) c->max_concurrent = NCRAWL_MAX_CONCURRENT;
    if (c->connect_timeout_ms < 100) c->connect_timeout_ms = 100;
    if (c->connect_timeout_ms > 60000) c->connect_timeout_ms = 60000;
    if (c->handshake_timeout_ms < 100) c->handshake_timeout_ms = 100;
    if (c->handshake_timeout_ms > 60000) c->handshake_timeout_ms = 60000;
}

/* ── pure census fold ────────────────────────────────────────────────── */

void network_census_compute(const struct ncrawl_probe_result *r, int n,
                            int64_t own_modal_height, int64_t now_unix,
                            struct network_census_view *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->ready = true;
    out->computed_at = now_unix;
    out->modal_height = -1;
    out->max_height = -1;
    out->min_height = -1;
    out->own_modal_height = own_modal_height;
    out->network_modal_height = -1;
    if (!r || n <= 0)
        return;
    if (n > NCRAWL_MAX_CENSUS)
        n = NCRAWL_MAX_CENSUS;
    out->probed = n;

    /* version histogram + onion/clearnet split + height extremes (reachable) */
    for (int i = 0; i < n; i++) {
        if (!r[i].reachable)
            continue;
        out->reachable_count++;
        if (r[i].is_onion)
            out->onion_count++;
        else
            out->clearnet_count++;

        const char *sv = r[i].subver[0] ? r[i].subver : "(unknown)";
        int found = -1;
        for (int k = 0; k < out->num_versions; k++)
            if (strcmp(out->versions[k].subver, sv) == 0) {
                found = k;
                break;
            }
        if (found >= 0) {
            out->versions[found].count++;
        } else if (out->num_versions < NCRAWL_MAX_VERSIONS) {
            struct ncrawl_version_bucket *b =
                &out->versions[out->num_versions++];
            snprintf(b->subver, sizeof(b->subver), "%s", sv);
            b->count = 1;
        }

        if (r[i].best_height >= 0) {
            out->heights_known++;
            if (out->max_height < 0 || r[i].best_height > out->max_height)
                out->max_height = r[i].best_height;
            if (out->min_height < 0 || r[i].best_height < out->min_height)
                out->min_height = r[i].best_height;
        }
    }
    if (out->max_height >= 0 && out->min_height >= 0)
        out->height_spread = out->max_height - out->min_height;

    /* modal advertised height over reachable-with-height (bounded O(n^2)) */
    int best_count = 0;
    int64_t best_h = -1;
    for (int i = 0; i < n; i++) {
        if (!r[i].reachable || r[i].best_height < 0)
            continue;
        int c = 0;
        for (int j = 0; j < n; j++)
            if (r[j].reachable && r[j].best_height == r[i].best_height)
                c++;
        if (c > best_count ||
            (c == best_count && r[i].best_height > best_h)) {
            best_count = c;
            best_h = r[i].best_height;
        }
    }
    out->modal_height = best_h;
    out->modal_height_count = best_count;
    out->network_modal_height = best_h;

    /* sort version histogram descending by count (bounded selection sort) */
    for (int i = 0; i < out->num_versions; i++)
        for (int j = i + 1; j < out->num_versions; j++)
            if (out->versions[j].count > out->versions[i].count) {
                struct ncrawl_version_bucket t = out->versions[i];
                out->versions[i] = out->versions[j];
                out->versions[j] = t;
            }

    /* whole-network eclipse signal: our connected peers cluster on a height
     * that is a small minority (<1/3) of the wider crawled network. */
    if (own_modal_height >= 0 && best_h >= 0) {
        int at_own = 0;
        for (int i = 0; i < n; i++)
            if (r[i].reachable && r[i].best_height == own_modal_height)
                at_own++;
        out->network_count_at_own_modal = at_own;
        out->eclipse_suspected =
            out->reachable_count >= NCRAWL_ECLIPSE_MIN &&
            own_modal_height != best_h &&
            (int64_t)at_own * 3 < (int64_t)out->reachable_count;
    }
}

/* ── bounded census table ────────────────────────────────────────────── */

/* Insert-or-update by addr; evict the oldest (smallest last_probe_us) row when
 * full. Caller holds g_ncrawl.lock. */
static void ncrawl_census_ingest_locked(const struct ncrawl_probe_result *pr)
{
    if (!pr || !pr->addr[0])
        return;
    for (int i = 0; i < g_ncrawl.census_count; i++) {
        if (strcmp(g_ncrawl.census[i].addr, pr->addr) == 0) {
            g_ncrawl.census[i] = *pr;
            return;
        }
    }
    if (g_ncrawl.census_count < NCRAWL_MAX_CENSUS) {
        g_ncrawl.census[g_ncrawl.census_count++] = *pr;
        return;
    }
    int oldest = 0;
    for (int i = 1; i < g_ncrawl.census_count; i++)
        if (g_ncrawl.census[i].last_probe_us <
            g_ncrawl.census[oldest].last_probe_us)
            oldest = i;
    g_ncrawl.census[oldest] = *pr;
}

static void ncrawl_refold_locked(int64_t own_modal, int64_t now)
{
    network_census_compute(g_ncrawl.census, g_ncrawl.census_count,
                           own_modal, now, &g_ncrawl.view);
}

static int64_t ncrawl_own_modal(void)
{
#ifdef ZCL_TESTING
    int64_t o = atomic_load(&g_ncrawl.test_own_modal);
    if (o != INT64_MIN)
        return o;
#endif
    struct network_consensus_view v;
    if (network_monitor_get_view(&v))
        return v.modal_height;
    return -1; // raw-return-ok:sentinel-own-modal-unknown
}

/* ── bounded-concurrency probe wave ──────────────────────────────────── */

struct ncrawl_work {
    ncrawl_probe_fn fn;
    const struct net_address *addr;
    int connect_timeout_ms;
    int handshake_timeout_ms;
    struct ncrawl_probe_result out;
    bool ok;
};

static void *ncrawl_worker_fn(void *arg)
{
    struct ncrawl_work *w = arg;
    memset(&w->out, 0, sizeof(w->out));
    w->ok = w->fn && w->fn(w->addr, w->connect_timeout_ms,
                           w->handshake_timeout_ms, &w->out);
    return NULL;
}

/* Dial addrs[0..n) through fn with at most `concurrent` (<=NCRAWL_MAX_CONCURRENT)
 * in-flight at once; ingest each recordable result into the bounded census.
 * Returns the count of recorded probes. */
static int ncrawl_run_round(const struct net_address *addrs, int n,
                            ncrawl_probe_fn fn, int concurrent,
                            int ct_ms, int ht_ms)
{
    if (!addrs || n <= 0 || !fn)
        return 0;
    if (n > NCRAWL_MAX_PER_ROUND)
        n = NCRAWL_MAX_PER_ROUND;
    if (concurrent < 1)
        concurrent = 1;
    if (concurrent > NCRAWL_MAX_CONCURRENT)
        concurrent = NCRAWL_MAX_CONCURRENT;

    int probed = 0;
    for (int base = 0; base < n; base += concurrent) {
        int wave = n - base;
        if (wave > concurrent)
            wave = concurrent;

        struct ncrawl_work items[NCRAWL_MAX_CONCURRENT];
        pthread_t th[NCRAWL_MAX_CONCURRENT];
        bool spawned[NCRAWL_MAX_CONCURRENT];

        for (int t = 0; t < wave; t++) {
            items[t].fn = fn;
            items[t].addr = &addrs[base + t];
            items[t].connect_timeout_ms = ct_ms;
            items[t].handshake_timeout_ms = ht_ms;
            memset(&items[t].out, 0, sizeof(items[t].out));
            items[t].ok = false;
            spawned[t] = false;
            /* Bounded (<=NCRAWL_MAX_CONCURRENT) short-lived probe workers, all
             * joined before this wave returns — not a long-running thread. */
            // raw-pthread-ok: bounded, joined-per-wave short-lived crawler probe worker
            if (pthread_create(&th[t], NULL, ncrawl_worker_fn, &items[t]) == 0)
                spawned[t] = true;
            else
                ncrawl_worker_fn(&items[t]); /* inline fallback on spawn fail */
        }
        for (int t = 0; t < wave; t++)
            if (spawned[t])
                pthread_join(th[t], NULL);

        for (int t = 0; t < wave; t++) {
            if (items[t].ok && items[t].out.addr[0]) {
                zcl_mutex_lock(&g_ncrawl.lock);
                ncrawl_census_ingest_locked(&items[t].out);
                zcl_mutex_unlock(&g_ncrawl.lock);

                /* Bank the durable node-identity census from this crawler
                 * contact (source = crawler), OUTSIDE the crawler lock (the
                 * emit does event-log I/O). A reachable contact carries an
                 * identity (success upsert); an unreachable one only bumps an
                 * existing node's dial_fail_count. The emit fails closed on a
                 * malformed user-agent and no-ops if no event log is wired. */
                const struct net_addr *na = &items[t].addr->svc.addr;
                uint8_t census_key[16];
                if (na->has_torv3)
                    memcpy(census_key, na->torv3, 16);
                else
                    memcpy(census_key, na->ip, 16);
                int64_t obs = items[t].out.last_probe_us > 0
                                  ? items[t].out.last_probe_us
                                  : platform_time_wall_unix();
                (void)peers_projection_emit_census_observed(
                    census_key, items[t].addr->svc.port,
                    EV_CENSUS_SOURCE_CRAWLER, items[t].out.reachable,
                    items[t].out.subver, items[t].out.version,
                    items[t].out.services, items[t].out.best_height, obs);
                probed++;
            }
        }
    }
    return probed;
}

/* One full crawl round: pull a bounded address batch from addrman, probe it,
 * refold. Worker-thread-only (the batch buffer is thread-owned). */
static void ncrawl_do_round(void)
{
    struct addr_man *am = g_ncrawl.addrman;
    if (!am)
        return;
    static struct net_address batch[NCRAWL_MAX_PER_ROUND]; /* worker-thread-owned */
    size_t want = (size_t)g_ncrawl.max_per_round;
    if (want > NCRAWL_MAX_PER_ROUND)
        want = NCRAWL_MAX_PER_ROUND;
    size_t got = addrman_get_addr(am, batch, want);
    int n = (int)(got > NCRAWL_MAX_PER_ROUND ? NCRAWL_MAX_PER_ROUND : got);

    int probed = ncrawl_run_round(batch, n, g_ncrawl.probe_fn,
                                  g_ncrawl.max_concurrent,
                                  g_ncrawl.connect_timeout_ms,
                                  g_ncrawl.handshake_timeout_ms);

    int64_t now = platform_time_wall_unix();
    int64_t own = ncrawl_own_modal();
    zcl_mutex_lock(&g_ncrawl.lock);
    g_ncrawl.rounds_run++;
    g_ncrawl.probed_last_round = probed;
    g_ncrawl.addresses_known = (int64_t)addrman_size(am);
    g_ncrawl.last_round_unix = now;
    ncrawl_refold_locked(own, now);
    zcl_mutex_unlock(&g_ncrawl.lock);
}

/* ── supervised worker thread ────────────────────────────────────────── */

static void ncrawl_heartbeat(void)
{
    supervisor_child_id id = atomic_load(&g_ncrawl.supervisor_id);
    if (id == SUPERVISOR_INVALID_ID)
        return;
    supervisor_tick(id);
    supervisor_progress(id, atomic_load(&g_ncrawl.loop_ticks));
}

static void ncrawl_on_stall(struct liveness_contract *c)
{
    (void)c;
    LOG_WARN("network_crawler",
             "crawler heartbeat lapsed (ticks=%lld) — worker may be wedged",
             (long long)atomic_load(&g_ncrawl.loop_ticks));
}

static void *ncrawl_thread_fn(void *arg)
{
    (void)arg;
    int64_t next_round_at = 0; /* first round immediately when enabled */
    while (!atomic_load(&g_ncrawl.stop_requested)) {
        atomic_fetch_add(&g_ncrawl.loop_ticks, 1);
        ncrawl_heartbeat();

        int64_t now = platform_time_wall_unix();
        if (g_ncrawl.enabled && now >= next_round_at) {
            ncrawl_do_round();
            next_round_at = now + g_ncrawl.round_interval_secs;
        }
        platform_sleep_ms(200); /* responsive stop between rounds */
    }
    return NULL;
}

static struct zcl_result ncrawl_register_supervisor(void)
{
    if (!supervisor_start())
        return ZCL_ERR(-5, "network_crawler: supervisor_start failed");

    liveness_contract_init(&g_ncrawl_contract, "net.network_crawler");
    atomic_store(&g_ncrawl_contract.period_secs, 0); /* self-heartbeats */
    atomic_store(&g_ncrawl_contract.deadline_secs, NCRAWL_SUPERVISOR_DEADLINE_SEC);
    atomic_store(&g_ncrawl_contract.progress_max_quiet_us, 0);
    g_ncrawl_contract.on_stall = ncrawl_on_stall;

    supervisor_domains_init();
    supervisor_child_id id =
        supervisor_register_in_domain(g_net_sup, &g_ncrawl_contract);
    if (id == SUPERVISOR_INVALID_ID)
        return ZCL_ERR(-6, "network_crawler: supervisor_register failed");
    atomic_store(&g_ncrawl.supervisor_id, id);
    return ZCL_OK;
}

struct zcl_result network_crawler_start(const struct network_crawler_config *cfg,
                                        struct addr_man *addrman)
{
    if (g_ncrawl.started)
        return ZCL_OK;

    struct network_crawler_config local;
    network_crawler_config_defaults(&local);
    if (cfg)
        local = *cfg;
    ncrawl_config_from_env(&local);
    ncrawl_clamp(&local);

    g_ncrawl.enabled = local.enabled;
    g_ncrawl.addrman = addrman;
    g_ncrawl.round_interval_secs = local.round_interval_secs;
    g_ncrawl.max_per_round = local.max_per_round;
    g_ncrawl.max_concurrent = local.max_concurrent;
    g_ncrawl.connect_timeout_ms = local.connect_timeout_ms;
    g_ncrawl.handshake_timeout_ms = local.handshake_timeout_ms;
    if (!g_ncrawl.probe_fn)
        g_ncrawl.probe_fn = network_crawler_default_probe;
    atomic_store(&g_ncrawl.stop_requested, false);
    atomic_store(&g_ncrawl.loop_ticks, 0);
    atomic_store(&g_ncrawl.supervisor_id, SUPERVISOR_INVALID_ID);

    g_ncrawl.thread_running = true;
    int rc = thread_registry_spawn("zcl_network_crawler", ncrawl_thread_fn,
                                   NULL, &g_ncrawl.thread);
    if (rc != 0) {
        g_ncrawl.thread_running = false;
        return ZCL_ERR(-4,
                       "network_crawler: thread_registry_spawn failed (%d)", rc);
    }

    struct zcl_result sup = ncrawl_register_supervisor();
    if (!sup.ok) {
        /* worker still runs; supervision is a liveness contract, not a hard
         * dependency. Name the gap loudly and continue. */
        LOG_WARN("network_crawler", "supervisor registration failed: %s",
                 sup.message);
    }

    g_ncrawl.started = true;
    if (!g_ncrawl.enabled)
        LOG_WARN("network_crawler",
                 "crawler registered but IDLE (disabled via ZCL_NETWORK_CRAWLER=0; "
                 "it is ON by default — the eclipse census is off while disabled)");
    return ZCL_OK;
}

void network_crawler_stop(void)
{
    if (!g_ncrawl.started)
        return;
    atomic_store(&g_ncrawl.stop_requested, true);
    atomic_store(&g_ncrawl_contract.deadline_secs, 0); /* silence stall on stop */
    if (g_ncrawl.thread_running) {
        pthread_join(g_ncrawl.thread, NULL);
        g_ncrawl.thread_running = false;
    }
    g_ncrawl.started = false;
}

bool network_crawler_get_view(struct network_census_view *out)
{
    if (!out)
        return false;
    zcl_mutex_lock(&g_ncrawl.lock);
    bool ready = g_ncrawl.view.ready;
    if (ready)
        *out = g_ncrawl.view;
    zcl_mutex_unlock(&g_ncrawl.lock);
    return ready;
}

/* ── introspection (network_census dumper) ───────────────────────────── */

bool network_crawler_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;
    json_set_object(out);

    zcl_mutex_lock(&g_ncrawl.lock);
    bool started = g_ncrawl.started;
    bool enabled = g_ncrawl.enabled;
    struct network_census_view v = g_ncrawl.view;
    int census_count = g_ncrawl.census_count;
    int64_t rounds = g_ncrawl.rounds_run;
    int probed_last = g_ncrawl.probed_last_round;
    int64_t known = g_ncrawl.addresses_known;
    int64_t last_round = g_ncrawl.last_round_unix;
    int interval = g_ncrawl.round_interval_secs;
    int per_round = g_ncrawl.max_per_round;
    int concurrent = g_ncrawl.max_concurrent;
    int ct_ms = g_ncrawl.connect_timeout_ms;
    int ht_ms = g_ncrawl.handshake_timeout_ms;
    zcl_mutex_unlock(&g_ncrawl.lock);

    json_push_kv_bool(out, "started", started);
    json_push_kv_bool(out, "enabled", enabled);
    json_push_kv_bool(out, "running", started && enabled);
    json_push_kv_int(out, "round_interval_secs", interval);
    json_push_kv_int(out, "max_per_round", per_round);
    json_push_kv_int(out, "max_concurrent", concurrent);
    json_push_kv_int(out, "connect_timeout_ms", ct_ms);
    json_push_kv_int(out, "handshake_timeout_ms", ht_ms);
    json_push_kv_int(out, "loop_ticks", atomic_load(&g_ncrawl.loop_ticks));
    json_push_kv_int(out, "rounds_run", rounds);
    json_push_kv_int(out, "addresses_known", known);
    json_push_kv_int(out, "census_rows", census_count);
    json_push_kv_int(out, "probed_last_round", probed_last);
    json_push_kv_int(out, "last_round_unix", last_round);

    if (!v.ready) {
        diag_push_health(out, true,
                         enabled ? "no census yet"
                                 : "crawler disabled (opt-out: ZCL_NETWORK_CRAWLER=0)");
        return true;
    }

    json_push_kv_int(out, "computed_at", v.computed_at);
    json_push_kv_int(out, "reachable_count", v.reachable_count);
    json_push_kv_int(out, "onion_count", v.onion_count);
    json_push_kv_int(out, "clearnet_count", v.clearnet_count);
    json_push_kv_int(out, "heights_known", v.heights_known);
    json_push_kv_int(out, "modal_height", v.modal_height);
    json_push_kv_int(out, "modal_height_count", v.modal_height_count);
    json_push_kv_int(out, "max_height", v.max_height);
    json_push_kv_int(out, "min_height", v.min_height);
    json_push_kv_int(out, "height_spread", v.height_spread);

    struct json_value versions;
    json_init(&versions);
    json_set_array(&versions);
    int top = v.num_versions;
    if (top > NCRAWL_TOPN_VERSIONS)
        top = NCRAWL_TOPN_VERSIONS;
    for (int i = 0; i < top; i++) {
        struct json_value b;
        json_init(&b);
        json_set_object(&b);
        json_push_kv_str(&b, "subver", v.versions[i].subver);
        json_push_kv_int(&b, "count", v.versions[i].count);
        (void)json_push_back(&versions, &b);
        json_free(&b);
    }
    (void)json_push_kv(out, "version_histogram", &versions);
    json_free(&versions);

    json_push_kv_bool(out, "eclipse_suspected", v.eclipse_suspected);
    struct json_value ev;
    json_init(&ev);
    json_set_object(&ev);
    json_push_kv_int(&ev, "own_modal_height", v.own_modal_height);
    json_push_kv_int(&ev, "network_modal_height", v.network_modal_height);
    json_push_kv_int(&ev, "network_count_at_own_modal",
                     v.network_count_at_own_modal);
    json_push_kv_int(&ev, "reachable_count", v.reachable_count);
    (void)json_push_kv(out, "eclipse_evidence", &ev);
    json_free(&ev);

    char reason[192];
    if (v.eclipse_suspected)
        snprintf(reason, sizeof(reason),
                 "eclipse: our peers modal=%lld is a %d/%d minority vs network "
                 "modal=%lld",
                 (long long)v.own_modal_height, v.network_count_at_own_modal,
                 v.reachable_count, (long long)v.network_modal_height);
    else
        snprintf(reason, sizeof(reason),
                 "reachable=%d modal_height=%lld spread=%lld",
                 v.reachable_count, (long long)v.modal_height,
                 (long long)v.height_spread);
    diag_push_health(out, !v.eclipse_suspected, reason);
    return true;
}

#ifdef ZCL_TESTING
void network_crawler_test_reset(void)
{
    zcl_mutex_lock(&g_ncrawl.lock);
    g_ncrawl.census_count = 0;
    memset(g_ncrawl.census, 0, sizeof(g_ncrawl.census));
    memset(&g_ncrawl.view, 0, sizeof(g_ncrawl.view));
    g_ncrawl.rounds_run = 0;
    g_ncrawl.probed_last_round = 0;
    g_ncrawl.addresses_known = 0;
    g_ncrawl.last_round_unix = 0;
    g_ncrawl.probe_fn = network_crawler_default_probe;
    g_ncrawl.max_per_round = NCRAWL_MAX_PER_ROUND;
    g_ncrawl.max_concurrent = NCRAWL_MAX_CONCURRENT;
    g_ncrawl.connect_timeout_ms = NCRAWL_CONNECT_TIMEOUT_MS_DEFAULT;
    g_ncrawl.handshake_timeout_ms = NCRAWL_HANDSHAKE_TIMEOUT_MS_DEFAULT;
    atomic_store(&g_ncrawl.test_own_modal, INT64_MIN);
    zcl_mutex_unlock(&g_ncrawl.lock);
}

void network_crawler_test_set_probe_fn(ncrawl_probe_fn fn)
{
    zcl_mutex_lock(&g_ncrawl.lock);
    g_ncrawl.probe_fn = fn ? fn : network_crawler_default_probe;
    zcl_mutex_unlock(&g_ncrawl.lock);
}

void network_crawler_test_set_own_modal(int64_t h)
{
    atomic_store(&g_ncrawl.test_own_modal, h);
}

void network_crawler_test_set_view(const struct network_census_view *v)
{
    if (!v)
        return;
    zcl_mutex_lock(&g_ncrawl.lock);
    g_ncrawl.view = *v;
    g_ncrawl.view.ready = true;
    zcl_mutex_unlock(&g_ncrawl.lock);
}

int network_crawler_test_probe_round(const struct net_address *addrs, int n)
{
    int probed = ncrawl_run_round(addrs, n, g_ncrawl.probe_fn,
                                  g_ncrawl.max_concurrent,
                                  g_ncrawl.connect_timeout_ms,
                                  g_ncrawl.handshake_timeout_ms);
    int64_t now = platform_time_wall_unix();
    int64_t own = ncrawl_own_modal();
    zcl_mutex_lock(&g_ncrawl.lock);
    g_ncrawl.rounds_run++;
    g_ncrawl.probed_last_round = probed;
    ncrawl_refold_locked(own, now);
    zcl_mutex_unlock(&g_ncrawl.lock);
    return probed;
}

int network_crawler_test_census_count(void)
{
    zcl_mutex_lock(&g_ncrawl.lock);
    int c = g_ncrawl.census_count;
    zcl_mutex_unlock(&g_ncrawl.lock);
    return c;
}
#endif
