/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_node_utilities.c — node operator utility entry points.
 *
 * Part of the boot composition root (extracted from boot_services.c). This
 * unit holds the small operator-facing utilities that hang off the live boot
 * context: -addnode connection setup (app_add_node), the metrics thread
 * lifecycle (app_start_metrics / app_stop_metrics) with its injected external
 * gauge callback (boot_metrics_external_gauges), the Prometheus RPC-counter
 * source (app_wire_metrics_sources), the async sync-state observer that logs
 * sync-pipeline transitions (boot_sync_state_logger), and the single advisory
 * peer-selection weight feed (boot_seed_addrman_reputation), which merges
 * banked bandwidth reputation with on-chain seniority into ONE bounded,
 * never-exclusive call to addrman_set_reputation_weight.
 *
 * This TU is the single place that reads live subsystem state on behalf of
 * lib/metrics. lib/metrics deliberately links against neither lib/net,
 * lib/validation nor lib/rpc; every number it publishes about the chain tip,
 * the peer count, or the RPC middleware arrives through one of the two
 * callbacks defined here.
 *
 * Owns no file-statics. The public app_* entry points reach the live context
 * through boot_active_svc() (declared in boot_internal.h, called from main.c);
 * boot_metrics_external_gauges is private here (only app_start_metrics injects
 * it). boot_sync_state_logger is wired by app_init_services in boot_services.c,
 * so its prototype lives in config/boot_internal.h; app_wire_metrics_sources is
 * called unconditionally from main() and is declared in config/boot.h. */

#include "config/boot_internal.h"
#include "services/node_health_service.h"
#include "services/legacy_mirror_sync_service.h"
#include "services/sync_monitor.h"
#include "services/chain_state_service.h"
#include "jobs/reducer_frontier.h"
#include "jobs/header_admit_stage.h"
#include "jobs/validate_headers_stage.h"
#include "jobs/body_fetch_stage.h"
#include "jobs/body_persist_stage.h"
#include "jobs/script_validate_stage.h"
#include "jobs/proof_validate_stage.h"
#include "jobs/utxo_apply_stage.h"
#include "jobs/tip_finalize_stage.h"
#include "chain/chainparams.h"
#include "metrics/prometheus_metrics.h"
#include "rpc/http_middleware.h"
#include "validation/chainstate.h"
#include "sync/sync_state.h"
#include "event/event.h"
#include "net/connman.h"
#include "net/addrman.h"
#include "net/zdir_selection.h"
#include "net/addnode_file.h"
#include "storage/peers_projection.h"
#include "models/zid_identity.h"
#include "zid/zid_seniority.h"
#include "crypto/sha3.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

/* External-gauge callback injected into the metrics thread: snapshots
 * sync state, UTXO count, tip-advance age, mirror lag, and peer counts. */
static void boot_metrics_external_gauges(
    struct metrics_external_gauges *out,
    void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    enum sync_state state;
    struct legacy_mirror_sync_stats lms = {0};
    struct node_health_snapshot nhs = {0};

    if (!out)
        return;

    state = sync_get_state();
    out->sync_state = (int)state;
    snprintf(out->sync_state_name, sizeof(out->sync_state_name), "%s",
             sync_state_name(state));

    if (svc && svc->node_db && svc->node_db->open)
        out->utxo_count = node_db_utxo_count(svc->node_db);

    out->tip_advance_age_seconds = sync_monitor_tip_advance_age();

    legacy_mirror_sync_stats_snapshot(&lms);
    out->mirror_lag_blocks = lms.enabled && lms.lag_known
                                  ? (int64_t)lms.lag : -1;
    out->mirror_lag_breach_seconds = lms.lag_breach_seconds;
    out->mirror_lag_critical_seconds = lms.lag_critical_seconds;

    if (svc && svc->state) {
        node_health_collect(&nhs, svc->node_db, svc->state);
        out->magicbean_peer_count = (int64_t)nhs.magicbean_peer_count;
        out->zclassic_c23_peer_count = (int64_t)nhs.zclassic_c23_peer_count;
    }

    /* header_gap_growing input: best-known header height minus served
     * height H*. csr_header_height() is a cheap in-memory, lock-guarded
     * read (see its doc comment — deliberately not csr_snapshot(), which
     * also runs SQLite queries); reducer_frontier_provable_tip_cached()
     * is a lock-free atomic. Both are safe to call from the metrics
     * thread. -1 until the chain-state repository has a header tip. */
    out->header_gap_blocks = -1;
    {
        int64_t hh = csr_header_height(csr_instance());
        if (hh >= 0) {
            int64_t served = (int64_t)reducer_frontier_provable_tip_cached();
            out->header_gap_blocks = hh - served;
        }
    }

    /* Per-reducer-stage telemetry (Phase E4): fixed 8-stage cursor +
     * step_us_ewma snapshot, forwarded to lib/metrics/src/stage_metrics.c
     * by the metrics tick (lib/metrics/src/metrics.c) since lib/ cannot
     * include these app/jobs headers directly. Order MUST match
     * metrics_stage_name()'s canonical order in
     * lib/metrics/include/metrics/stage_metrics.h — mirrors the same
     * fixed-order rows[] pattern diag_profile_push_stage_ewma() uses in
     * app/controllers/src/diagnostics_registry.c for the `profile`
     * command's stage_ewma block. */
    out->stage_cursor[0]       = (int64_t)header_admit_stage_cursor();
    out->stage_step_us_ewma[0] = header_admit_stage_step_us_ewma();
    out->stage_cursor[1]       = (int64_t)validate_headers_stage_cursor();
    out->stage_step_us_ewma[1] = validate_headers_stage_step_us_ewma();
    out->stage_cursor[2]       = (int64_t)body_fetch_stage_cursor();
    out->stage_step_us_ewma[2] = body_fetch_stage_step_us_ewma();
    out->stage_cursor[3]       = (int64_t)body_persist_stage_cursor();
    out->stage_step_us_ewma[3] = body_persist_stage_step_us_ewma();
    out->stage_cursor[4]       = (int64_t)script_validate_stage_cursor();
    out->stage_step_us_ewma[4] = script_validate_stage_step_us_ewma();
    out->stage_cursor[5]       = (int64_t)proof_validate_stage_cursor();
    out->stage_step_us_ewma[5] = proof_validate_stage_step_us_ewma();
    out->stage_cursor[6]       = (int64_t)utxo_apply_stage_cursor();
    out->stage_step_us_ewma[6] = utxo_apply_stage_step_us_ewma();
    out->stage_cursor[7]       = (int64_t)tip_finalize_stage_cursor();
    out->stage_step_us_ewma[7] = tip_finalize_stage_step_us_ewma();

    /* Active-chain tip + peer count. lib/metrics used to call
     * active_chain_tip() (lib/validation) and connman_get_node_count()
     * (lib/net) straight from its tick, bypassing this seam; both reads
     * belong here, where the boot context already owns cs_main and the
     * connman. The tip read is under cs_main exactly as it was. */
    if (svc && svc->state) {
        zcl_mutex_lock(&svc->state->cs_main);
        struct block_index *tip = active_chain_tip(&svc->state->chain_active);
        out->tip_height = tip ? (int64_t)tip->nHeight : 0;
        out->tip_time   = tip ? (int64_t)tip->nTime : 0;
        zcl_mutex_unlock(&svc->state->cs_main);
    }
    if (svc && svc->connman)
        out->connection_count = (int64_t)connman_get_node_count(svc->connman);
}

/* Prometheus `zcl_rpc_*` counter source. lib/metrics must not link against
 * lib/rpc, so the renderer pulls through this callback (registered by
 * app_wire_metrics_sources below) instead of calling
 * rpc_http_middleware_get_global()/stats_snapshot() itself. Called with the
 * renderer's lock held — do nothing here but snapshot and copy. */
static void boot_metrics_rpc_http_gauges(struct metrics_rpc_http_gauges *out,
                                         void *ctx)
{
    (void)ctx;
    if (!out)
        return;

    struct rpc_http_stats_snapshot snap;
    rpc_http_middleware_stats_snapshot(rpc_http_middleware_get_global(), &snap);

    out->allowed             = snap.allowed;
    out->rate_limited_global = snap.rate_limited_global;
    out->rate_limited_per_ip = snap.rate_limited_per_ip;
    out->banned_rejected     = snap.banned_rejected;
    out->bans_issued         = snap.bans_issued;
    out->auth_failures       = snap.auth_failures;
    out->tracked_ips         = (uint64_t)snap.tracked_ips;
    out->active_bans         = (uint64_t)snap.active_bans;
}

/* Wire the metrics sources that are NOT tied to the metrics thread's
 * lifetime. The Prometheus dump is served on demand (native `meta`
 * handler, HTTPS, and the RPC HTTP server), including on a node started
 * with -showmetrics=0 where app_start_metrics() never runs — so main()
 * calls this unconditionally rather than folding it into app_start_metrics. */
void app_wire_metrics_sources(void)
{
    metrics_prometheus_set_rpc_http_source(boot_metrics_rpc_http_gauges, NULL);
}

/* ── Utility functions ─────────────────────────────────────── */

/* Resolve and open an outbound connection to a -addnode host[:port]. */
void app_add_node(const char *host, int port)
{
    struct boot_svc_ctx *svc = boot_active_svc();
    char hostbuf[256];
    snprintf(hostbuf, sizeof(hostbuf), "%s", host);

    if (port <= 0) {
        char *colon = strrchr(hostbuf, ':');
        if (colon && colon != hostbuf) {
            int p = atoi(colon + 1);
            if (p > 0 && p < 65536) {
                port = p;
                *colon = '\0';
            }
        }
    }

    uint16_t use_port = port > 0 ? (uint16_t)port
                                 : svc->connman->manager.default_port;

    struct net_address addr;
    net_address_init(&addr);
    addr.svc.port = use_port;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(hostbuf, NULL, &hints, &res) == 0 && res) {
        if (res->ai_family == AF_INET) {
            struct sockaddr_in *s4 = (struct sockaddr_in *)res->ai_addr;
            memset(addr.svc.addr.ip, 0, 10);
            addr.svc.addr.ip[10] = 0xff;
            addr.svc.addr.ip[11] = 0xff;
            memcpy(addr.svc.addr.ip + 12, &s4->sin_addr, 4);
        } else if (res->ai_family == AF_INET6) {
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)res->ai_addr;
            memcpy(addr.svc.addr.ip, &s6->sin6_addr, 16);
        }
        freeaddrinfo(res);

        printf("Connecting to addnode %s:%u\n", hostbuf, use_port);
        connman_open_connection(svc->connman, &addr);
    } else {
        printf("Failed to resolve addnode %s\n", hostbuf);
    }
}

static void app_add_node_from_file_cb(const char *host, uint16_t port, void *ctx)
{
    (void)ctx;
    app_add_node(host, (int)port);
}

void app_add_nodes_from_file(const char *path)
{
    if (!path || !*path)
        return;
    addnode_file_load(path, app_add_node_from_file_cb, NULL);
}

/* ── The ONE advisory influence path into peer selection ───────────
 *
 * Everything below feeds exactly one function: addrman_set_reputation_weight()
 * (lib/net/include/net/addrman.h), which clamps to [1.0,
 * ADDRMAN_REPUTATION_MAX_MULT] and STRUCTURALLY CANNOT exclude a peer — the
 * bound is a property of the API, not of the caller's discipline. Hardcoded
 * seeds, address gossip and -addnode stay independent discovery roots that
 * nothing here can narrow.
 *
 * Two opinions arrive about the same address: banked bandwidth reputation
 * (NET-2) and on-chain seniority (T5.2). They are merged into ONE value by
 * zid_seniority_combine() and issued as ONE call per address. This is
 * deliberate and load-bearing: two calls would be two influence paths in
 * everything but name, with the later one silently clobbering the earlier.
 * If a third signal ever appears, it joins the combine — it does not get its
 * own call. */

/* Per-client draw context: the epoch seed T5.1's derivation produced. */
struct boot_seniority_draw_ctx {
    uint8_t seed[32];
};

/* The per-client selection derivation is T5.1's (net/zdir_selection.h) and is
 * NOT reimplemented here — this is the adapter that plugs it into the
 * seniority weighting's zid_seniority_draw_fn seam. lib/zid sits below
 * lib/net in the module order and cannot call into it, so the boot
 * composition root is where the two meet.
 *
 * zdir_candidate_score is SHA3-256(0x02 ‖ "ZDIR" ‖ seed ‖ candidate_id) with
 * the seed already bound to this node's client key and the epoch's block
 * hash; the low 8 bytes are a uniform draw. */
static bool boot_seniority_client_draw(void *vctx, const uint8_t relay_id[32],
                                       uint64_t *draw_out)
{
    const struct boot_seniority_draw_ctx *ctx = vctx;
    if (!ctx || !relay_id || !draw_out)
        LOG_FAIL("boot_seniority", "draw: NULL ctx/relay/out");

    uint8_t score[32];
    if (!zdir_candidate_score(score, ctx->seed, relay_id))
        LOG_FAIL("boot_seniority", "draw: zdir_candidate_score failed");

    uint64_t d = 0;
    for (int i = 0; i < 8; i++)
        d |= (uint64_t)score[i] << (8 * i);
    *draw_out = d;
    return true;
}

/* Build the per-client, per-epoch seed the draw above is keyed on, entirely
 * through T5.1's primitives: client_key from this node's addrman salt (durable
 * across restarts, never on the wire), epoch seed from that key plus the block
 * hash at the RANKING EPOCH height rather than at the tip — that is the update
 * rate limit, so the favourite set rotates about every 6 hours instead of every
 * block, and an attacker gets one grinding attempt per epoch instead of one per
 * 150 seconds.
 *
 * When the epoch height is outside the in-memory active chain window (early
 * boot, or a node still folding) the block hash stays all-zero. The seed is
 * still per-client, because client_key alone already makes it so; the only
 * thing missing is chain-binding of the rotation. Never an error, never a
 * fallback to a shared ranking. */
static bool boot_seniority_epoch_seed(const struct addr_man *am,
                                      int32_t epoch_height, uint8_t seed[32])
{
    if (!am || !seed)
        LOG_FAIL("boot_seniority", "epoch seed: NULL addrman/out");

    uint8_t client_key[32];
    if (!zdir_client_key(client_key, am->nKey.data))
        LOG_FAIL("boot_seniority", "epoch seed: client key derivation failed");

    uint8_t block_hash[32];
    memset(block_hash, 0, sizeof(block_hash));

    struct boot_svc_ctx *svc = boot_active_svc();
    if (svc && svc->state && epoch_height > 0) {
        zcl_mutex_lock(&svc->state->cs_main);
        struct block_index *bi =
            active_chain_at(&svc->state->chain_active, epoch_height);
        if (bi)
            memcpy(block_hash, bi->hashBlock.data, 32);
        zcl_mutex_unlock(&svc->state->cs_main);
    }

    if (!zdir_epoch_seed(seed, block_hash, client_key))
        LOG_FAIL("boot_seniority", "epoch seed: zdir_epoch_seed failed");
    return true;
}

/* SEAM — resolve a dialable peer address to the anchored relay identity that
 * registered it.
 *
 * DELIBERATELY UNWIRED, AND SAYING SO RATHER THAN FAKING IT. The ZID overlay
 * anchors a master key to the chain (zid/zid_anchor.h) and the projection
 * records who anchored it and at what height (models/zid_identity.h), but
 * NOTHING on-chain yet binds that key to an IP or an onion — no ZID command
 * publishes a relay's contact address. Inventing a binding here (matching a
 * text record, or trusting a peer's self-reported identity in its version
 * handshake) would be exactly the unauthenticated claim this layer exists to
 * remove, and it would be worse than no binding at all: it would let anyone
 * borrow someone else's seniority by asserting their key.
 *
 * So this returns false for every address today. Consequence, stated
 * plainly: every address currently gets seniority multiplier 1.0, so the
 * combined weight is identical to the bandwidth-only weight. The table above
 * it is still built, scored, owner-capped and drawn — only this last hop is
 * missing, and this function is the ONE place that changes when the on-chain
 * address binding lands. */
static bool boot_seniority_relay_for_addr(const uint8_t ip[16], uint16_t port,
                                          uint8_t relay_id[32])
{
    (void)ip;
    (void)port;
    (void)relay_id;
    return false;
}

/* Everything one address needs to be weighted. */
struct boot_addrman_seed_ctx {
    struct addr_man *am;
    const struct zid_seniority_weight *seniority;  /* sorted by relay_id */
    size_t seniority_n;
};

/* Feed-read: fold one address's banked bandwidth reputation and its on-chain
 * seniority into a single bounded dial-preference weight. Neither signal can
 * lower the other and neither can drop a peer below the unweighted baseline.
 * Fail-open throughout. */
static void boot_addrman_reputation_cb(const uint8_t ip[16], uint16_t port,
                                       const struct peer_reputation *rep,
                                       void *ctx)
{
    struct boot_addrman_seed_ctx *sc = ctx;
    if (!sc || !sc->am || !rep)
        return;

    /* NET-2: bandwidth_score 0..255 → bounded [1, MAX] multiplier. */
    double bandwidth_mult = 1.0;
    if (rep->bandwidth_score > 0) {
        double frac = (double)rep->bandwidth_score / 255.0;
        if (frac > 1.0) frac = 1.0;
        bandwidth_mult = 1.0 + (ADDRMAN_REPUTATION_MAX_MULT - 1.0) * frac;
    }

    /* T5.2: on-chain seniority, per-client and owner-capped. */
    double seniority_mult = 1.0;
    uint8_t relay_id[32];
    if (sc->seniority_n &&
        boot_seniority_relay_for_addr(ip, port, relay_id)) {
        const struct zid_seniority_weight *w =
            zid_seniority_find(sc->seniority, sc->seniority_n, relay_id);
        if (w)
            seniority_mult = w->multiplier;
    }

    double weight = zid_seniority_combine(bandwidth_mult, seniority_mult);
    if (weight <= 1.0)
        return;   /* no opinion — leave the address on the plain baseline */

    struct net_addr na;
    memset(&na, 0, sizeof(na));
    memcpy(na.ip, ip, 16);
    (void)addrman_set_reputation_weight(sc->am, &na, weight);
}

/* Build this node's seniority table from the on-chain identity projection.
 *
 * Reads anchor_height (how long the identity has been anchored) and
 * owner_address (the P2PKH signer that anchored it, the same owner
 * convention ZNAM uses) out of zid_identities, hashes the owner address into
 * an opaque grouping key, and hands the set to zid_seniority_rank() — which
 * applies the anti-Sybil age floor, the per-owner influence cap, and this
 * node's own draw.
 *
 * Returns the entry count and sets *out_table to a heap table the caller
 * frees, or 0 with *out_table NULL (no rows, no db, or allocation failure —
 * all of which degrade to bandwidth-only weighting, never to an error). */
static size_t boot_build_seniority_table(struct zid_seniority_weight **out_table,
                                         const struct addr_man *am)
{
    if (!out_table)
        return 0;
    *out_table = NULL;

    struct boot_svc_ctx *svc = boot_active_svc();
    if (!svc || !svc->node_db || !svc->node_db->open)
        return 0;

    int64_t total = db_zid_identity_count(svc->node_db);
    if (total <= 0)
        return 0;   /* no anchored identities: nothing to weigh */
    if (total > ZID_SENIORITY_MAX_RELAYS)
        total = ZID_SENIORITY_MAX_RELAYS;   /* projection quota */

    size_t n = (size_t)total;
    struct zid_relay_registration *regs =
        zcl_malloc(n * sizeof(*regs), "seniority_registrations");
    struct zid_seniority_weight *tbl =
        zcl_malloc(n * sizeof(*tbl), "seniority_weights");
    if (!regs || !tbl) {
        free(regs);
        free(tbl);
        return 0;
    }

    /* Page the projection in bounded chunks — the row struct is large and
     * this runs on the boot thread's stack. */
    struct zid_identity page[32];
    size_t filled = 0;
    int offset = 0;
    while (filled < n) {
        int got = db_zid_identity_list(svc->node_db, page,
                                       (int)(sizeof(page) / sizeof(page[0])),
                                       offset);
        if (got <= 0)
            break;
        for (int i = 0; i < got && filled < n; i++) {
            memset(&regs[filled], 0, sizeof(regs[filled]));
            memcpy(regs[filled].relay_id, page[i].master_pubkey, 32);
            regs[filled].registration_height = page[i].anchor_height;
            /* owner_id is an opaque grouping key; an empty owner_address
             * stays all-zero, which zid_seniority treats as "unknown owner"
             * and pointedly does NOT pool with other unknowns. */
            if (page[i].owner_address[0])
                sha3_256((const unsigned char *)page[i].owner_address,
                         strlen(page[i].owner_address), regs[filled].owner_id);
            filled++;
        }
        offset += got;
    }

    size_t count = 0;
    if (filled > 0) {
        int32_t tip = reducer_frontier_provable_tip_cached();
        struct boot_seniority_draw_ctx dctx;
        memset(&dctx, 0, sizeof(dctx));
        if (boot_seniority_epoch_seed(am, zid_seniority_epoch_height(tip),
                                      dctx.seed)) {
            int ranked = zid_seniority_rank(regs, filled, tip,
                                            boot_seniority_client_draw, &dctx,
                                            tbl, n);
            if (ranked > 0)
                count = (size_t)ranked;
        }
    }

    free(regs);
    if (count == 0) {
        free(tbl);
        return 0;
    }
    *out_table = tbl;
    return count;
}

/* Seed addrman dial preference from banked bandwidth reputation (the peers
 * projection is opened + caught up earlier in boot) combined with on-chain
 * seniority. Bounded, one-shot, fail-open. */
static void boot_seed_addrman_reputation(struct addr_man *am)
{
    if (!am)
        return;

    /* The table is derived per-client (through zdir_client_key over this
     * node's durable addrman salt), never globally — see rule (1): a single
     * deterministic ranking every client shares would be an anonymity
     * monoculture, one ranking with one top relay for everyone to attack. */
    struct zid_seniority_weight *table = NULL;
    size_t table_n = boot_build_seniority_table(&table, am);

    struct boot_addrman_seed_ctx sc;
    memset(&sc, 0, sizeof(sc));
    sc.am = am;
    sc.seniority = table;
    sc.seniority_n = table_n;

    (void)peers_projection_for_each_reputation_global(
        4096, boot_addrman_reputation_cb, &sc);

    if (table_n > 0)
        LOG_INFO("boot_seniority",
                 "ranked %zu anchored relay identities (epoch %d); no on-chain "
                 "address binding yet, so no address consumes a seniority "
                 "boost this boot",
                 table_n, zid_seniority_epoch_height(
                              reducer_frontier_provable_tip_cached()));

    free(table);
}

void app_log_bootstrap_sources(const struct chain_params *params,
                                struct connman *cm)
{
    if (!params || !cm)
        return;

    const char *home = getenv("HOME");
    bool operator_onion_seed_file = false;
    if (home) {
        char p[512];
        snprintf(p, sizeof(p), "%s/.config/zclassic23/onion-seeds", home);
        operator_onion_seed_file = access(p, R_OK) == 0;
    }
    boot_seed_addrman_reputation(&cm->manager.addrman);
    size_t addrman_loaded = addrman_size(&cm->manager.addrman);
    size_t total_sources = params->nSeeds + params->nFixedSeeds +
                            params->nOnionSeeds +
                            (operator_onion_seed_file ? 1 : 0) +
                            (addrman_loaded > 0 ? 1 : 0);
    printf("[net] bootstrap sources: dns_seeds=%zu fixed_seeds=%zu "
           "onion_seeds=%zu operator_onion_seed_file=%d "
           "addrman_loaded_peers=%zu total_sources=%zu\n",
           params->nSeeds, params->nFixedSeeds, params->nOnionSeeds,
           operator_onion_seed_file ? 1 : 0, addrman_loaded, total_sources);
}

/* Start the Prometheus metrics thread with injected external gauges. */
void app_start_metrics(bool mining)
{
    struct boot_svc_ctx *svc = boot_active_svc();
    svc->metrics->ms = svc->state;
    svc->metrics->params = chain_params_get();
    svc->metrics->mining = mining;
    svc->metrics->external_gauges = boot_metrics_external_gauges;
    svc->metrics->external_gauges_ctx = svc;
    if (!metrics_start(svc->metrics))
        fprintf(stderr, "WARNING: failed to start metrics thread\n");
}

/* Stop the metrics thread. */
void app_stop_metrics(void)
{
    struct boot_svc_ctx *svc = boot_active_svc();
    metrics_stop(svc->metrics);
}

/* ── Sync state observer ──────────────────────────────────────── *
 * Async observer that logs sync state transitions, tip updates,
 * block connections, and reorgs. Provides high-level observability
 * of the sync pipeline without blocking any P2P or validation thread.
 *
 * Registered at boot via event_observe_async() for:
 *   EV_SYNC_STATE_CHANGE — sync FSM transitions
 *   EV_TIP_UPDATED       — chain tip advances
 *   EV_BLOCK_CONNECTED    — individual blocks connected
 *   EV_REORG_START        — chain reorganization begins */
void boot_sync_state_logger(enum event_type type, uint32_t peer_id,
                               const void *payload, uint32_t payload_len,
                               void *ctx)
{
    (void)ctx;
    const char *msg = (payload_len > 0 && payload) ? (const char *)payload : "";

    switch (type) {
    case EV_SYNC_STATE_CHANGE:
        printf("[observer] sync state → %s\n", msg);
        break;
    case EV_TIP_UPDATED:
        /* Only log major milestones to avoid flooding */
        break;
    case EV_BLOCK_CONNECTED:
        break; /* too noisy for printf, event log captures it */
    case EV_REORG_START:
        fprintf(stderr, "[observer] REORG: %s (peer=%u)\n", msg, peer_id);
        break;
    default:
        break;
    }
    fflush(stdout);
}
