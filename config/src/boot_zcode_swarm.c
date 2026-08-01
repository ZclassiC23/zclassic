/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_zcode_swarm — see config/include/config/boot_zcode_swarm.h. This
 * unit is the ONLY place where lib/net peer facts (node id, host key,
 * peer_state, peer_scoring) meet the swarm engine's contract; both sides
 * stay pure of each other. */

#include "config/boot_zcode_swarm.h"

#include "config/boot_internal.h"

#include "vcs/package_reward.h"
#include "vcs/package_service.h"
#include "vcs/package_store.h"
#include "vcs/package_swarm_node.h"
#include "vcs/build_action.h"
#include "vcs/build_artifact_manifest.h"
#include "vcs/zcode_work_node.h"

#include "crypto/sha3.h"
#include "event/event.h"
#include "net/fast_sync.h"
#include "net/net.h"
#include "net/peer_identity.h"
#include "net/peer_scoring.h"
#include "platform/time_compat.h"
#include "util/log_macros.h"
#include "util/sync.h"
#include "util/util.h"
#include "services/build_fabric_worker.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Session pseudo-key domain (0x02 || SHA3-256(domain || host)). The host
 * identity comes from zcl_peer_host_key (onion hostname or IP), so the
 * key scopes the service book to a transport endpoint, survives a
 * reconnect of the same host, and is never a contributor identity. */
#define ZCODE_SWARM_KEY_DOMAIN "zcl.zcode_swarm_peer.v1"

/* Membership sync / scheduler-tick throttle (seconds). The tick hook
 * fires once per peer message cycle — far too hot for an O(peers x
 * nodes) membership sweep or a scheduler pass; both are second-scale. */
#define ZCODE_SWARM_SYNC_PERIOD_SEC 15
#define ZCODE_SWARM_TICK_PERIOD_SEC 1

static zcl_mutex_t s_lock;
static bool s_lock_init;
static struct vcs_swarm_engine *s_engine;   /* owned here */
static struct vcs_zcode_work_node *s_work;  /* owned work adapter */
static struct vcs_service_book *s_book;     /* owned here */
static struct vcs_reward_ledger *s_ledger;  /* owned here; may be NULL */
static char s_zcode_dir[4400];
static int64_t s_last_sync;                 /* wall seconds */
static int64_t s_last_tick;
static uint8_t s_work_secret[32];
static uint8_t s_work_pubkey[32];
static bool s_work_key_ready;

static bool boot_zcode_work_refresh(struct boot_svc_ctx *svc, int64_t wall)
{
    if (!s_work || !GetBoolArg("-buildworker", false)) return true;
    if (!s_work_key_ready) {
        struct db_build_worker worker;
        struct zcl_result loaded = build_fabric_worker_identity_load(
            svc->datadir, &worker, s_work_secret, s_work_pubkey);
        if (!loaded.ok)
            LOG_FAIL("net.zcode_swarm", "work identity: %s", loaded.message);
        s_work_key_ready = true;
    }
    struct vcs_toolchain_capsule_v1 capsule;
    struct vcs_zcode_work_capability_v1 capability = {0};
    if (!vcs_toolchain_capsule_v1_capture_gcc(&capsule) ||
        !vcs_toolchain_capsule_v1_root(
            &capsule, capability.toolchain_capsule_root))
        LOG_FAIL("net.zcode_swarm", "work toolchain capture failed");
    capability.work_kinds = UINT32_C(1) << VCS_ZCODE_WORK_BUILD;
    capability.target = VCS_ZCODE_WORK_TARGET_LINUX_X86_64_V3;
    capability.confinement = VCS_ZCODE_WORK_CONFINEMENT_V1_MASK;
    capability.max_cpu_seconds = 120;
    capability.max_memory_bytes = UINT64_C(2) * 1024u * 1024u * 1024u;
    capability.max_output_bytes = VCS_BUILD_ARTIFACT_MAX_BYTES;
    capability.max_lease_seconds = 120;
    capability.slots = 1;
    capability.queue_headroom = 1;
    capability.expires_unix = wall + 600;
    if (!vcs_zcode_work_capability_seal(
            &capability, s_work_secret, s_work_pubkey) ||
        !vcs_zcode_work_node_set_local_capability(s_work, &capability))
        LOG_FAIL("net.zcode_swarm", "work capability signing failed");
    return true;
}

static void boot_zcode_swarm_lock(void)
{
    if (!s_lock_init) {
        /* First touch happens on the single message-handler thread
         * before any command thread can observe the engine global. */
        zcl_mutex_init(&s_lock);
        s_lock_init = true;
    }
    zcl_mutex_lock(&s_lock);
}

/* score_fn for the engine: earned score from the reward ledger. Pseudo-
 * keys never appear in the ledger (transport scope, not identity), so
 * this resolves to zero in practice today; it exists so a future slice
 * that swaps in authenticated peer keys needs no engine change. */
static uint64_t boot_zcode_swarm_score(const uint8_t contributor[33],
                                       void *ctx)
{
    (void)ctx;
    if (!s_ledger)
        return 0;
    struct vcs_reward_contributor_totals t;
    memset(&t, 0, sizeof(t));
    vcs_reward_contributor_totals(s_ledger, contributor, &t);
    return t.earned_score;
}

/* Engine peer handle for a node. node->id starts at 0 on a fresh
 * net_manager (nm->last_node_id++), and the engine reserves peer id 0
 * (the next_outbound no-filter convention), so translate by +1. */
static uint64_t boot_zcode_swarm_peer_id(const struct p2p_node *node)
{
    return (uint64_t)node->id + 1u;
}

static bool boot_zcode_swarm_peer_key(const struct p2p_node *node,
                                      uint8_t out[33])
{
    char host[ZCL_PEER_HOST_KEY_MAX];
    if (!zcl_peer_host_key(node, host, sizeof(host)))
        return false;
    out[0] = 0x02;
    struct sha3_256_ctx h;
    sha3_256_init(&h);
    sha3_256_write(&h, (const unsigned char *)ZCODE_SWARM_KEY_DOMAIN,
                   sizeof(ZCODE_SWARM_KEY_DOMAIN) - 1);
    sha3_256_write(&h, (const unsigned char *)host, strlen(host));
    sha3_256_finalize(&h, out + 1);
    return true;
}

/* Lazily create the node-global engine. Caller holds s_lock. Returns
 * NULL (named, logged) when hosting is off, the store is not open, the
 * datadir is missing, or allocation fails — the swarm simply stays off
 * for that frame/tick; nothing here is fatal. */
static struct vcs_swarm_engine *boot_zcode_swarm_ensure(
    struct boot_svc_ctx *svc)
{
    if (s_engine)
        return s_engine;
    if (!svc || !svc->datadir)
        return NULL;
    if (!GetBoolArg("-packagehost", false))
        return NULL;
    struct vcs_package_store *store = vcs_package_store_global();
    if (!store)
        return NULL;
    int n = snprintf(s_zcode_dir, sizeof(s_zcode_dir), "%s/zcode",
                     svc->datadir);
    if (n < 0 || (size_t)n >= sizeof(s_zcode_dir))
        LOG_NULL("net.zcode_swarm", "datadir path too long");
    s_book = vcs_service_book_load(s_zcode_dir);
    if (!s_book)
        LOG_NULL("net.zcode_swarm",
                 "service book unavailable; swarm off");
    /* The ledger is optional: tier resolution degrades to NEW_USER. */
    s_ledger = vcs_reward_ledger_load(s_zcode_dir);
    s_engine = vcs_swarm_engine_create(store, s_book, s_zcode_dir,
                                       boot_zcode_swarm_score, NULL);
    if (!s_engine) {
        vcs_reward_ledger_free(s_ledger);
        s_ledger = NULL;
        vcs_service_book_free(s_book);
        s_book = NULL;
        LOG_NULL("net.zcode_swarm", "engine create failed; swarm off");
    }
    vcs_swarm_engine_set_global(s_engine);
    s_work = vcs_zcode_work_node_create();
    if (!s_work) {
        vcs_swarm_engine_set_global(NULL);
        vcs_swarm_engine_free(s_engine); s_engine = NULL;
        vcs_reward_ledger_free(s_ledger); s_ledger = NULL;
        vcs_service_book_free(s_book); s_book = NULL;
        LOG_NULL("net.zcode_swarm", "work adapter create failed; swarm off");
    }
    vcs_zcode_work_node_set_global(s_work);
    if (!boot_zcode_work_refresh(svc,
            (int64_t)platform_time_wall_time_t())) {
        vcs_zcode_work_node_set_global(NULL);
        vcs_zcode_work_node_free(s_work); s_work = NULL;
        vcs_swarm_engine_set_global(NULL);
        vcs_swarm_engine_free(s_engine); s_engine = NULL;
        vcs_reward_ledger_free(s_ledger); s_ledger = NULL;
        vcs_service_book_free(s_book); s_book = NULL;
        LOG_NULL("net.zcode_swarm", "work capability failed; swarm off");
    }
    return s_engine;
}

/* A node is swarm-eligible once the version handshake completed, it
 * speaks the zclassic23 service bit, and it is not being torn down. */
static bool boot_zcode_swarm_eligible(const struct p2p_node *node)
{
    enum peer_state st = atomic_load(&node->state);
    if (st < PEER_HANDSHAKE_COMPLETE || st > PEER_SNAPSHOT_RECEIVING)
        return false;
    if (atomic_load(&node->disconnect))
        return false;
    if (node->is_feeler || node->one_shot)
        return false;
    return peer_supports_fast_sync(node->services);
}

static enum peer_offence boot_zcode_swarm_offence(
    enum vcs_swarm_penalty penalty)
{
    switch (penalty) {
    case VCS_SWARM_PENALTY_MALFORMED:
        return PEER_OFFENCE_INVALID_MESSAGE;
    case VCS_SWARM_PENALTY_ANNOUNCE_FLOOD:
    case VCS_SWARM_PENALTY_REQUEST_FLOOD:
        return PEER_OFFENCE_FLOOD;
    case VCS_SWARM_PENALTY_REPLAYED_REQUEST:
    case VCS_SWARM_PENALTY_REPLAYED_DATA:
        return PEER_OFFENCE_INVALID_PAYLOAD;
    case VCS_SWARM_PENALTY_UNREQUESTED_DATA:
        return PEER_OFFENCE_UNREQUESTED;
    case VCS_SWARM_PENALTY_INVALID_DATA:
        return PEER_OFFENCE_INVALID_CHUNK;
    case VCS_SWARM_PENALTY_NONE:
    default:
        return PEER_OFFENCE_NONE;
    }
}

static void boot_zcode_swarm_send(struct msg_processor *mp,
                                  struct p2p_node *node,
                                  const uint8_t *frame, size_t frame_len)
{
    if (!p2p_node_begin_message(node, "zpkgswm",
                                mp->params->pchMessageStart)) {
        LOG_ERROR("net.zcode_swarm", "begin_message failed for peer %lld",
                  (long long)node->id);
        return;
    }
    p2p_node_write_message_data(node, frame, frame_len);
    if (!p2p_node_end_message(node))
        LOG_ERROR("net.zcode_swarm", "end_message failed for peer %lld",
                  (long long)node->id);
}

bool boot_zcode_swarm_frame(struct msg_processor *mp, struct p2p_node *node,
                            const uint8_t *payload, size_t payload_len,
                            void *ctx)
{
    if (!mp || !node || !payload)
        LOG_FAIL("net.zcode_swarm", "null mp/node/payload");
    boot_zcode_swarm_lock();
    struct vcs_swarm_engine *engine =
        boot_zcode_swarm_ensure((struct boot_svc_ctx *)ctx);
    if (!engine) {
        zcl_mutex_unlock(&s_lock);
        return true; /* hosting off: drop quietly (never an offence) */
    }
    uint8_t key[33];
    if (!boot_zcode_swarm_peer_key(node, key)) {
        zcl_mutex_unlock(&s_lock);
        return true; /* no usable host identity: drop, never an offence */
    }
    (void)vcs_swarm_engine_peer_add(engine, boot_zcode_swarm_peer_id(node),
                                    key);
    int64_t day = (int64_t)platform_time_wall_time_t() / 86400;
    uint64_t now = (uint64_t)platform_time_wall_time_t();
    uint64_t peer_id = boot_zcode_swarm_peer_id(node);
    (void)vcs_zcode_work_node_peer_add(s_work, peer_id);
    if (payload_len >= 4 && memcmp(payload, "ZCWS", 4) == 0) {
        enum vcs_zcode_work_node_result wr =
            vcs_zcode_work_node_handle_frame(s_work, peer_id, payload,
                                              payload_len, (int64_t)now);
        if (wr == VCS_ZCODE_WORK_NODE_OK) {
            struct vcs_zcode_work_swarm_message message;
            if (vcs_zcode_work_swarm_parse(payload, payload_len, &message) &&
                message.type == VCS_ZCODE_WORK_SWARM_REQUEST)
                (void)vcs_swarm_engine_fetch(
                    engine, message.body.request.context_root, day, now);
        }
        zcl_mutex_unlock(&s_lock);
        if (wr == VCS_ZCODE_WORK_NODE_MALFORMED ||
            wr == VCS_ZCODE_WORK_NODE_REPLAY ||
            wr == VCS_ZCODE_WORK_NODE_UNREQUESTED ||
            wr == VCS_ZCODE_WORK_NODE_BINDING) {
            char context[96];
            (void)snprintf(context, sizeof(context), "zcode work: %s",
                           vcs_zcode_work_node_result_string(wr));
            if (mp->net_mgr)
                peer_scoring_record(mp->net_mgr, node,
                                    PEER_OFFENCE_INVALID_PAYLOAD, context);
        }
        return true;
    }
    struct vcs_swarm_frame_result ev = vcs_swarm_engine_handle_frame(
        engine, boot_zcode_swarm_peer_id(node), payload, payload_len, day,
        now);
    zcl_mutex_unlock(&s_lock);

    if (ev.penalty != VCS_SWARM_PENALTY_NONE && mp->net_mgr) {
        char context[96];
        (void)snprintf(context, sizeof(context), "zcode swarm: %s",
                       ev.rule ? ev.rule : "penalty");
        peer_scoring_record(mp->net_mgr, node,
                            boot_zcode_swarm_offence(ev.penalty), context);
    }
    if (ev.reply) {
        if (ev.reply_len > 0 && !atomic_load(&node->disconnect))
            boot_zcode_swarm_send(mp, node, ev.reply, ev.reply_len);
        free(ev.reply);
    }
    if (ev.disconnect_peer)
        atomic_store(&node->disconnect, true);
    return true;
}

/* Membership sync: under cs_nodes, add every eligible node (announcing
 * tracked packages to newly known peers) and drop engine peers whose
 * node is gone or no longer eligible. announce_to only queues frames —
 * no network I/O under cs_nodes. Caller holds s_lock. */
static void boot_zcode_swarm_sync_membership(struct msg_processor *mp,
                                             struct vcs_swarm_engine *engine)
{
    struct net_manager *nm = mp->net_mgr;
    if (!nm)
        return;
    zcl_mutex_lock(&nm->cs_nodes);
    for (size_t i = 0; i < nm->num_nodes; i++) {
        struct p2p_node *node = nm->nodes[i];
        if (!node || !boot_zcode_swarm_eligible(node))
            continue;
        uint8_t key[33];
        if (!boot_zcode_swarm_peer_key(node, key))
            continue;
        bool known = vcs_swarm_engine_peer_known(
            engine, boot_zcode_swarm_peer_id(node));
        if (vcs_swarm_engine_peer_add(engine,
                                      boot_zcode_swarm_peer_id(node),
                                      key) &&
            !known)
            (void)vcs_swarm_engine_announce_to(
                engine, boot_zcode_swarm_peer_id(node));
        (void)vcs_zcode_work_node_peer_add(
            s_work, boot_zcode_swarm_peer_id(node));
    }
    uint64_t ids[VCS_SWARM_MAX_PEERS];
    size_t count = vcs_swarm_engine_peer_ids(engine, ids,
                                             VCS_SWARM_MAX_PEERS);
    for (size_t i = 0; i < count; i++) {
        bool live = false;
        for (size_t j = 0; j < nm->num_nodes; j++) {
            struct p2p_node *node = nm->nodes[j];
            if (node && boot_zcode_swarm_peer_id(node) == ids[i] &&
                boot_zcode_swarm_eligible(node)) {
                live = true;
                break;
            }
        }
        if (!live)
            vcs_swarm_engine_peer_drop(engine, ids[i]);
        if (!live)
            vcs_zcode_work_node_peer_drop(s_work, ids[i]);
    }
    zcl_mutex_unlock(&nm->cs_nodes);
}

void boot_zcode_swarm_tick(struct msg_processor *mp, struct p2p_node *node,
                           void *ctx)
{
    if (!mp || !node)
        return;
    int64_t wall = (int64_t)platform_time_wall_time_t();
    boot_zcode_swarm_lock();
    struct vcs_swarm_engine *engine =
        boot_zcode_swarm_ensure((struct boot_svc_ctx *)ctx);
    if (!engine) {
        zcl_mutex_unlock(&s_lock);
        return;
    }
    if (wall - s_last_sync >= ZCODE_SWARM_SYNC_PERIOD_SEC) {
        s_last_sync = wall;
        boot_zcode_swarm_sync_membership(mp, engine);
    }
    if (wall - s_last_tick >= ZCODE_SWARM_TICK_PERIOD_SEC) {
        s_last_tick = wall;
        vcs_swarm_engine_tick(engine, wall / 86400, (uint64_t)wall);
        vcs_zcode_work_node_tick(s_work, wall);
        if (GetBoolArg("-buildworker", false) && wall % 300 == 0)
            (void)boot_zcode_work_refresh((struct boot_svc_ctx *)ctx, wall);
    }
    /* Drain queued frames for THIS node (bounded by the engine's
     * outbound queue; WANT/CANCEL/ANNOUNCE frames only — DATA replies
     * go out synchronously from the frame hook). */
    if (boot_zcode_swarm_eligible(node)) {
        uint8_t frame[VCS_SWARM_OUTBOUND_FRAME_MAX];
        for (;;) {
            uint64_t peer_out = 0;
            size_t frame_len = 0;
            uint64_t peer_id = boot_zcode_swarm_peer_id(node);
            if (!vcs_swarm_engine_next_outbound(engine, peer_id,
                                                &peer_out, frame,
                                                &frame_len))
                break;
            if (peer_out != peer_id || frame_len == 0)
                break; /* defensive: filter contract violated */
            boot_zcode_swarm_send(mp, node, frame, frame_len);
        }
        uint8_t work_frame[VCS_ZCODE_WORK_SWARM_MAX_WIRE_BYTES];
        for (;;) {
            uint64_t peer_out = 0; size_t frame_len = 0;
            uint64_t peer_id = boot_zcode_swarm_peer_id(node);
            if (!vcs_zcode_work_node_next_outbound(
                    s_work, peer_id, &peer_out, work_frame, &frame_len))
                break;
            if (peer_out != peer_id || frame_len == 0) break;
            boot_zcode_swarm_send(mp, node, work_frame, frame_len);
        }
    }
    zcl_mutex_unlock(&s_lock);
}

void boot_zcode_swarm_wire(struct boot_svc_ctx *svc)
{
    if (!svc || !svc->msg_processor) {
        LOG_ERROR("net.zcode_swarm", "wire: null svc/msg_processor");
        return;
    }
    msg_processor_set_zcode_swarm(svc->msg_processor,
                                  boot_zcode_swarm_frame,
                                  boot_zcode_swarm_tick, svc);
}

void boot_zcode_swarm_shutdown(void)
{
    boot_zcode_swarm_lock();
    vcs_swarm_engine_set_global(NULL);
    vcs_zcode_work_node_set_global(NULL);
    if (s_work)
        vcs_zcode_work_node_free(s_work);
    s_work = NULL;
    if (s_engine)
        vcs_swarm_engine_free(s_engine);
    s_engine = NULL;
    if (s_book)
        vcs_service_book_free(s_book);
    s_book = NULL;
    if (s_ledger)
        vcs_reward_ledger_free(s_ledger);
    s_ledger = NULL;
    s_last_sync = 0;
    s_last_tick = 0;
    memset(s_work_secret, 0, sizeof(s_work_secret));
    memset(s_work_pubkey, 0, sizeof(s_work_pubkey));
    s_work_key_ready = false;
    zcl_mutex_unlock(&s_lock);
}
