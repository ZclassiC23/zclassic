/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_zcode_swarm_net — the slice-12 swarm over the REAL wire seam.
 * test_zcode_swarm.c drives one engine with hand-built frames; this file
 * puts two (and three) independent swarm engines behind two independent
 * msg_processors and lets real "zpkgswm" P2P messages (magic + command +
 * length + checksum, queued by p2p_node_begin/write/end_message, parsed
 * by the real p2p_node_receive_bytes, dispatched by the real
 * msg_process_messages through the dispatch-table row) carry every
 * ANNOUNCE/WANT/DATA/CANCEL between them. Only the socket syscalls are
 * elided (the sentinel technique from test_snapshot_serve_loopback.c).
 *
 * The test glue mirrors config/src/boot_zcode_swarm.c 1:1 — same
 * session pseudo-key derivation (0x02 || SHA3-256(domain || host)),
 * same penalty→peer_offence mapping, same reply/drain sends — with two
 * deliberate, documented substitutions: the score callback returns a
 * fixed contributor score (the production glue reads the reward ledger;
 * a NEW_USER peer has a 0/hour announce rate in the frozen slice-11
 * table, so honest announces would otherwise be flood-penalized), and
 * the tick clock is a deterministic counter instead of wall time.
 *
 * Covered:
 *   1. Golden path: two engines, end-to-end verified fetch into the CAS,
 *      byte-identical chunks, both books credited, zero misbehavior.
 *   2. Malicious server (wrong-hash chunks): PEER_OFFENCE_INVALID_CHUNK
 *      misbehavior on the real peer object, no credit, nothing stored,
 *      download ends in a NAMED failure.
 *   3. Unrequested DATA: PEER_OFFENCE_UNREQUESTED, no credit.
 *   4. Restart mid-download: engine freed and recreated over the same
 *      datadir; the persisted record resumes and the download completes.
 *   5. Disconnect requeue: one of two servers drops mid-download; the
 *      in-flight work moves to the survivor and the download completes. */

#include "test/test_core.h"

#include "chain/chainparams.h"
#include "coins/coins_view.h"
#include "core/serialize.h"
#include "crypto/sha3.h"
#include "event/event.h"
#include "net/fast_sync.h"
#include "net/msgprocessor.h"
#include "net/net.h"
#include "net/peer_identity.h"
#include "net/peer_scoring.h"
#include "util/safe_alloc.h"
#include "validation/main_state.h"
#include "validation/txmempool.h"
#include "vcs/package_service.h"
#include "vcs/package_store.h"
#include "vcs/package_swarm.h"
#include "vcs/package_swarm_node.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZWN_CHECK(name, expr) do {                                       \
    if (expr) { printf("  zcode_swarm_net: %s... OK\n", (name)); }       \
    else { printf("  zcode_swarm_net: %s... FAIL\n", (name)); \
        failures++; }                                                    \
} while (0)

#define ZWN_DAY 20500
#define ZWN_SCORE UINT64_C(100) /* EARNED_CONTRIBUTOR: announces allowed */
#define ZWN_MAX_FILES 13u
#define ZWN_MAX_FILE 512u
#define ZWN_KEY_DOMAIN "zcl.zcode_swarm_peer.v1"

/* ── fixture package (single-chunk files, same shape as the engine gate) */

struct zwn_pkg {
    struct vcs_package_manifest manifest;
    uint8_t *wire;
    size_t wire_len;
    uint8_t root[32];
    size_t count;
    uint8_t contents[ZWN_MAX_FILES][ZWN_MAX_FILE];
    size_t lens[ZWN_MAX_FILES];
    uint64_t total_bytes;
};

static bool zwn_make_package(struct zwn_pkg *p, size_t count, uint8_t seed)
{
    static const char *const k_paths[ZWN_MAX_FILES] = {
        "LICENSE", "include/a.h", "include/b.h", "include/c.h",
        "include/d.h", "src/a.c", "src/b.c", "src/c.c", "src/d.c",
        "src/e.c", "tests/t1.c", "tests/t2.c", "tests/t3.c",
    };
    memset(p, 0, sizeof(*p));
    if (count == 0 || count > ZWN_MAX_FILES)
        return false;
    vcs_package_manifest_init(&p->manifest);
    for (size_t i = 0; i < count; i++) {
        size_t len = 40u + i * 31u + seed;
        for (size_t j = 0; j < len; j++)
            p->contents[i][j] = (uint8_t)(seed + i * 7u + j * 3u);
        p->lens[i] = len;
        p->total_bytes += len;
        uint8_t hash[32];
        if (!vcs_package_chunk_hash(p->contents[i], len, hash))
            return false;
        if (!vcs_package_manifest_add(&p->manifest, k_paths[i],
                                      VCS_PACKAGE_MODE_FILE, len, hash, 1))
            return false;
    }
    p->count = count;
    if (!vcs_package_manifest_serialize(&p->manifest, &p->wire,
                                        &p->wire_len))
        return false;
    return vcs_package_manifest_root(&p->manifest, p->root);
}

static void zwn_free_package(struct zwn_pkg *p)
{
    vcs_package_manifest_free(&p->manifest);
    free(p->wire);
    p->wire = NULL;
}

static bool zwn_store_package(struct vcs_package_store *store,
                              const struct zwn_pkg *p)
{
    uint8_t root[32];
    if (vcs_package_store_put_manifest(store, p->wire, p->wire_len,
                                       root) != VCS_PACKAGE_STORE_OK)
        return false;
    for (size_t i = 0; i < p->count; i++) {
        const char *path = p->manifest.files[i].path;
        if (vcs_package_store_put_chunk(store, root, path, 0,
                                        p->contents[i],
                                        p->lens[i]) != VCS_PACKAGE_STORE_OK)
            return false;
    }
    return true;
}

/* ── the loopback node: msg_processor + engine + store + book ───────── */

struct zwn_node {
    struct main_state ms;
    struct tx_mempool mempool;
    struct coins_view null_view;
    struct coins_view_cache coins;
    struct net_manager nm;
    struct msg_processor mp;
    char datadir[1024];
    char zcode_dir[1100];
    struct vcs_package_store *store;
    struct vcs_service_book *book;
    struct vcs_swarm_engine *engine;
    uint64_t now;
    bool tamper_chunks; /* corrupt DATA replies carrying chunk objects */
};

struct zwn_link {
    struct zwn_node *owner;
    struct p2p_node *node; /* owner's connection object for the remote */
    struct send_segment *sentinel;
};

static uint64_t zwn_score(const uint8_t contributor[33], void *ctx)
{
    (void)contributor;
    (void)ctx;
    return ZWN_SCORE;
}

/* Production-identical session pseudo-key derivation. */
static bool zwn_peer_key(const struct p2p_node *node, uint8_t out[33])
{
    char host[ZCL_PEER_HOST_KEY_MAX];
    if (!zcl_peer_host_key(node, host, sizeof(host)))
        return false;
    out[0] = 0x02;
    struct sha3_256_ctx h;
    sha3_256_init(&h);
    sha3_256_write(&h, (const unsigned char *)ZWN_KEY_DOMAIN,
                   sizeof(ZWN_KEY_DOMAIN) - 1);
    sha3_256_write(&h, (const unsigned char *)host, strlen(host));
    sha3_256_finalize(&h, out + 1);
    return true;
}

static enum peer_offence zwn_offence(enum vcs_swarm_penalty penalty)
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

static void zwn_send(struct zwn_node *z, struct p2p_node *node,
                     const uint8_t *frame, size_t frame_len)
{
    if (atomic_load(&node->disconnect))
        return;
    if (!p2p_node_begin_message(node, "zpkgswm",
                                z->mp.params->pchMessageStart))
        return;
    p2p_node_write_message_data(node, frame, frame_len);
    (void)p2p_node_end_message(node);
}

/* The frame hook: config/src/boot_zcode_swarm.c's boot_zcode_swarm_frame
 * with the deterministic-clock + fixed-score substitutions (file header). */
static bool zwn_frame(struct msg_processor *mp, struct p2p_node *node,
                      const uint8_t *payload, size_t payload_len, void *ctx)
{
    struct zwn_node *z = ctx;
    uint8_t key[33];
    if (!zwn_peer_key(node, key))
        return true;
    (void)vcs_swarm_engine_peer_add(z->engine, (uint64_t)node->id, key);
    struct vcs_swarm_frame_result ev = vcs_swarm_engine_handle_frame(
        z->engine, (uint64_t)node->id, payload, payload_len, ZWN_DAY,
        ++z->now);
    if (ev.penalty != VCS_SWARM_PENALTY_NONE)
        peer_scoring_record(mp->net_mgr, node, zwn_offence(ev.penalty),
                            "zcode swarm test");
    if (ev.reply) {
        if (z->tamper_chunks && ev.reply_len > 0) {
            /* Corrupt only CHUNK data replies: the manifest still
             * verifies, so the download reaches the chunk stage and the
             * bad-hash path is what gets exercised. */
            struct vcs_package_swarm_message msg;
            if (vcs_package_swarm_parse(ev.reply, ev.reply_len, &msg) &&
                msg.type == VCS_PACKAGE_SWARM_DATA &&
                msg.body.data.object.object_kind ==
                    VCS_PACKAGE_SWARM_OBJECT_CHUNK)
                ev.reply[ev.reply_len - 1] ^= 0xff;
        }
        zwn_send(z, node, ev.reply, ev.reply_len);
        free(ev.reply);
    }
    if (ev.disconnect_peer)
        atomic_store(&node->disconnect, true);
    return true;
}

/* The tick hook: scheduler tick + per-peer outbound drain. Called from
 * the real msg_process_messages (proving the wiring) and directly by the
 * drive loop (so the scheduler also advances in quiet cycles). */
static void zwn_tick(struct msg_processor *mp, struct p2p_node *node,
                     void *ctx)
{
    struct zwn_node *z = ctx;
    (void)mp;
    vcs_swarm_engine_tick(z->engine, ZWN_DAY, ++z->now);
    uint8_t frame[VCS_SWARM_OUTBOUND_FRAME_MAX];
    for (;;) {
        uint64_t peer = 0;
        size_t frame_len = 0;
        if (!vcs_swarm_engine_next_outbound(z->engine, (uint64_t)node->id,
                                            &peer, frame, &frame_len))
            break;
        if (peer != (uint64_t)node->id || frame_len == 0)
            break;
        zwn_send(z, node, frame, frame_len);
    }
}

static bool zwn_node_init(struct zwn_node *z, const char *tag,
                          const struct chain_params *params)
{
    memset(&z->ms, 0, sizeof(z->ms));
    memset(&z->mempool, 0, sizeof(z->mempool));
    memset(&z->null_view, 0, sizeof(z->null_view));
    memset(&z->nm, 0, sizeof(z->nm));
    memset(&z->mp, 0, sizeof(z->mp));
    main_state_init(&z->ms);
    tx_mempool_init(&z->mempool, 0);
    coins_view_cache_init(&z->coins, &z->null_view);
    net_manager_init(&z->nm);
    z->mp.main_state = &z->ms;
    z->mp.mempool = &z->mempool;
    z->mp.coins_tip = &z->coins;
    z->mp.params = params;
    z->mp.net_mgr = &z->nm;
    z->now = 0;
    z->tamper_chunks = false;
    test_make_tmpdir(z->datadir, sizeof(z->datadir), "zcode_swarm_net",
                     tag);
    z->mp.datadir = z->datadir;
    snprintf(z->zcode_dir, sizeof(z->zcode_dir), "%s/zcode", z->datadir);
    z->store = vcs_package_store_open(
        z->datadir, VCS_PACKAGE_STORE_DEFAULT_QUOTA_BYTES);
    z->book = vcs_service_book_load(z->zcode_dir);
    if (!z->store || !z->book)
        return false;
    z->engine = vcs_swarm_engine_create(z->store, z->book, z->zcode_dir,
                                        zwn_score, NULL);
    if (!z->engine)
        return false;
    msg_processor_set_zcode_swarm(&z->mp, zwn_frame, zwn_tick, z);
    return true;
}

static void zwn_node_free(struct zwn_node *z)
{
    vcs_swarm_engine_free(z->engine);
    vcs_service_book_free(z->book);
    vcs_package_store_close(z->store);
    z->engine = NULL;
    z->book = NULL;
    z->store = NULL;
    net_manager_free(&z->nm);
    coins_view_cache_free(&z->coins);
    tx_mempool_free(&z->mempool);
    main_state_free(&z->ms);
}

/* A fresh engine over the same datadir (the restart-resume substitution:
 * store + book stay open; only the engine is recreated, exactly like a
 * process restart that keeps the datadir). */
static bool zwn_node_restart_engine(struct zwn_node *z)
{
    vcs_swarm_engine_free(z->engine);
    z->engine = vcs_swarm_engine_create(z->store, z->book, z->zcode_dir,
                                        zwn_score, NULL);
    return z->engine != NULL;
}

/* ── links + pump (the sentinel technique) ──────────────────────────── */

static bool zwn_link(struct zwn_node *owner, struct zwn_link *l,
                     uint8_t ip_a, uint8_t ip_b, uint8_t ip_c, uint8_t ip_d,
                     const char *name)
{
    /* p2p_node_create leaves id 0 on a fresh net_manager; the engine
     * reserves peer id 0 (the next_outbound "no filter" convention), so
     * loopback links take explicit nonzero ids. Distinct per owner. */
    static node_id_t next_id = 100;
    struct net_address addr;
    memset(&addr, 0, sizeof(addr));
    memcpy(addr.svc.addr.ip, pchIPv4Prefix, 12);
    addr.svc.addr.ip[12] = ip_a;
    addr.svc.addr.ip[13] = ip_b;
    addr.svc.addr.ip[14] = ip_c;
    addr.svc.addr.ip[15] = ip_d;
    addr.svc.port = 18033;
    l->owner = owner;
    l->node = p2p_node_create(&owner->nm, ZCL_INVALID_SOCKET, &addr,
                              name, false);
    if (!l->node)
        return false;
    l->node->id = next_id++;
    l->node->version = 1;
    l->node->services = NODE_ZCL23;
    l->node->state = PEER_HANDSHAKE_COMPLETE;
    l->sentinel = zcl_calloc(1, sizeof(*l->sentinel),
                             "zcode_swarm_net_sentinel");
    if (!l->sentinel)
        return false;
    l->node->send_head = l->sentinel;
    l->node->send_tail = l->sentinel;
    l->node->send_offset = 0;
    return true;
}

static void zwn_link_free(struct zwn_link *l)
{
    send_segment_free(l->sentinel);
    l->node->send_head = NULL;
    l->node->send_tail = NULL;
    l->node->recv_msg_count = 0;
    p2p_node_free(l->node);
    l->node = NULL;
    l->sentinel = NULL;
}

/* Drain from_link's queued wire bytes into to_link's owner through the
 * real receive parser + real dispatcher. */
static bool zwn_pump(struct zwn_link *from, struct zwn_link *to,
                     const unsigned char msgstart[MESSAGE_START_SIZE])
{
    bool any = false;
    struct send_segment *sentinel = from->sentinel;
    while (sentinel->next) {
        struct send_segment *seg = sentinel->next;
        sentinel->next = seg->next;
        if (from->node->send_tail == seg)
            from->node->send_tail = sentinel;
        if (from->node->send_size >= seg->size)
            from->node->send_size -= seg->size;
        else
            from->node->send_size = 0;
        if (!p2p_node_receive_bytes(to->node, (const char *)seg->data,
                                    (unsigned int)seg->size, msgstart)) {
            send_segment_free(seg);
            return false;
        }
        any = true;
        send_segment_free(seg);
    }
    from->node->send_head = sentinel;
    from->node->send_offset = 0;
    if (any)
        return msg_process_messages(&to->owner->mp, to->node);
    return true;
}

/* Free every queued segment without delivering it — the loopback
 * emulation of a socket teardown (a real restart/disconnect kills the
 * queued bytes with the fd). */
static void zwn_drain_quiet(struct zwn_link *l)
{
    while (l->sentinel->next) {
        struct send_segment *seg = l->sentinel->next;
        l->sentinel->next = seg->next;
        if (l->node->send_tail == seg)
            l->node->send_tail = l->sentinel;
        if (l->node->send_size >= seg->size)
            l->node->send_size -= seg->size;
        else
            l->node->send_size = 0;
        send_segment_free(seg);
    }
    l->node->send_head = l->sentinel;
    l->node->send_offset = 0;
}

/* Membership, mirroring the boot glue's sync: each side registers the
 * other and announces its tracked packages to a newly known peer. */
static bool zwn_meet_side(struct zwn_node *me, struct zwn_link *my_link)
{
    uint8_t key[33];
    if (!zwn_peer_key(my_link->node, key))
        return false;
    bool known = vcs_swarm_engine_peer_known(me->engine,
                                             (uint64_t)my_link->node->id);
    if (!vcs_swarm_engine_peer_add(me->engine, (uint64_t)my_link->node->id,
                                   key))
        return false;
    if (!known)
        (void)vcs_swarm_engine_announce_to(me->engine,
                                           (uint64_t)my_link->node->id);
    return true;
}

/* One full round: both schedulers advance, then both directions pump. */
static bool zwn_round(struct zwn_link *a_b, struct zwn_link *b_a,
                      const unsigned char msgstart[MESSAGE_START_SIZE])
{
    zwn_tick(&a_b->owner->mp, a_b->node, a_b->owner);
    zwn_tick(&b_a->owner->mp, b_a->node, b_a->owner);
    if (!zwn_pump(a_b, b_a, msgstart))
        return false;
    return zwn_pump(b_a, a_b, msgstart);
}

static bool zwn_download_done(struct zwn_node *b, const uint8_t root[32],
                              enum vcs_swarm_download_state *state_out)
{
    struct vcs_swarm_download_status st;
    memset(&st, 0, sizeof(st));
    if (!vcs_swarm_engine_download_status(b->engine, root, &st))
        return false;
    *state_out = st.state;
    return st.state == VCS_SWARM_DL_COMPLETE ||
           st.state == VCS_SWARM_DL_FAILED;
}

static bool zwn_store_matches(struct vcs_package_store *store,
                              const struct zwn_pkg *p)
{
    for (size_t i = 0; i < p->count; i++) {
        if (!vcs_package_store_chunk_present(store, p->root, (uint32_t)i,
                                             0))
            return false;
        uint8_t *bytes = NULL;
        size_t len = 0;
        if (vcs_package_store_get_chunk_at(store, p->root, (uint32_t)i, 0,
                                           &bytes,
                                           &len) != VCS_PACKAGE_STORE_OK)
            return false;
        bool match = len == p->lens[i] &&
                     memcmp(bytes, p->contents[i], len) == 0;
        free(bytes);
        if (!match)
            return false;
    }
    return true;
}

/* ── 1 + 4 + 5: the golden fetch (with restart / disconnect variants) ── */

enum zwn_golden_mode {
    ZWN_GOLDEN_PLAIN = 0,
    ZWN_GOLDEN_RESTART,
    ZWN_GOLDEN_DISCONNECT,
};

static int zwn_test_golden(enum zwn_golden_mode mode,
                           const struct chain_params *params)
{
    int failures = 0;
    const char *name =
        mode == ZWN_GOLDEN_PLAIN
            ? "golden: end-to-end verified fetch over real zpkgswm frames"
        : mode == ZWN_GOLDEN_RESTART
            ? "restart mid-download: engine recreated over the same "
              "datadir resumes and completes"
            : "disconnect requeue: in-flight work moves to the surviving "
              "server and the download completes";

    TEST(name) {
        /* 12 single-chunk files (restart/disconnect) so the chunk stage
         * spans more than one round (per-peer in-flight is 8) and a
         * mid-download window deterministically exists; 6 for the plain
         * run. */
        size_t file_count = mode == ZWN_GOLDEN_PLAIN ? 6 : 12;
        struct zwn_pkg pkg;
        ASSERT(zwn_make_package(&pkg, file_count, 0x11));

        struct zwn_node a, b, a2;
        ASSERT(zwn_node_init(&a, mode == ZWN_GOLDEN_PLAIN ? "ga" :
                             mode == ZWN_GOLDEN_RESTART ? "ra" : "da",
                             params));
        ASSERT(zwn_node_init(&b, mode == ZWN_GOLDEN_PLAIN ? "gb" :
                             mode == ZWN_GOLDEN_RESTART ? "rb" : "db",
                             params));
        ASSERT(zwn_store_package(a.store, &pkg));

        struct zwn_link a_b, b_a;
        ASSERT(zwn_link(&a, &a_b, 5, 6, 7, 8, "peer-b"));
        ASSERT(zwn_link(&b, &b_a, 1, 2, 3, 4, "peer-a"));
        ASSERT(zwn_meet_side(&a, &a_b));
        ASSERT(zwn_meet_side(&b, &b_a));

        struct zwn_link a2_b, b_a2;
        bool two_servers = mode == ZWN_GOLDEN_DISCONNECT;
        if (two_servers) {
            ASSERT(zwn_node_init(&a2, "da2", params));
            ASSERT(zwn_store_package(a2.store, &pkg));
            ASSERT(zwn_link(&a2, &a2_b, 9, 9, 9, 9, "peer-b"));
            ASSERT(zwn_link(&b, &b_a2, 8, 8, 8, 8, "peer-a2"));
            ASSERT(zwn_meet_side(&a2, &a2_b));
            ASSERT(zwn_meet_side(&b, &b_a2));
        }

        ASSERT(vcs_swarm_engine_fetch(b.engine, pkg.root, ZWN_DAY,
                                      ++b.now) == VCS_SWARM_FETCH_OK);

        bool restarted = false;
        bool dropped = false;
        bool saw_partial = mode != ZWN_GOLDEN_PLAIN ? false : true;
        enum vcs_swarm_download_state state = VCS_SWARM_DL_INACTIVE;
        bool terminal = false;
        for (int i = 0; i < 400 && !terminal; i++) {
            ASSERT(zwn_round(&a_b, &b_a, params->pchMessageStart));
            if (two_servers)
                ASSERT(zwn_round(&a2_b, &b_a2, params->pchMessageStart));

            if (mode == ZWN_GOLDEN_RESTART && !restarted) {
                struct vcs_swarm_download_status st;
                memset(&st, 0, sizeof(st));
                ASSERT(vcs_swarm_engine_download_status(b.engine, pkg.root,
                                                        &st));
                if (st.state == VCS_SWARM_DL_CHUNKS &&
                    st.present_chunks > 0 &&
                    st.present_chunks < st.total_chunks) {
                    saw_partial = true;
                    restarted = true;
                    /* A real restart kills the sockets with their queued
                     * bytes: drop both queues first, or the new engine
                     * would read the pre-restart DATAs as unrequested
                     * (they would be — its outstanding table is gone). */
                    zwn_drain_quiet(&a_b);
                    zwn_drain_quiet(&b_a);
                    ASSERT(zwn_node_restart_engine(&b));
                    /* The restart forgets session peers AND their
                     * advertisements (transport state, never persisted).
                     * A real restart also drops the connections: emulate
                     * the reconnect on both sides — B's new engine
                     * re-registers A, A sees B as a new peer and
                     * re-announces its packages (exactly what the boot
                     * glue's membership sync does for a new node). */
                    ASSERT(zwn_meet_side(&b, &b_a));
                    vcs_swarm_engine_peer_drop(a.engine,
                                               (uint64_t)a_b.node->id);
                    ASSERT(zwn_meet_side(&a, &a_b));
                }
            }
            if (mode == ZWN_GOLDEN_DISCONNECT && !dropped) {
                struct vcs_swarm_download_status st;
                memset(&st, 0, sizeof(st));
                ASSERT(vcs_swarm_engine_download_status(b.engine, pkg.root,
                                                        &st));
                if (st.state == VCS_SWARM_DL_CHUNKS &&
                    st.present_chunks > 0 &&
                    st.present_chunks < st.total_chunks) {
                    saw_partial = true;
                    dropped = true;
                    /* The membership-sync drop: the connection is gone,
                     * so the engine forgets the peer and requeues its
                     * in-flight work onto the surviving advertiser. The
                     * dead connection's queued bytes die with it. */
                    atomic_store(&b_a.node->disconnect, true);
                    zwn_drain_quiet(&a_b);
                    zwn_drain_quiet(&b_a);
                    vcs_swarm_engine_peer_drop(b.engine,
                                               (uint64_t)b_a.node->id);
                }
            }
            terminal = zwn_download_done(&b, pkg.root, &state);
        }

        ASSERT(saw_partial);
        if (mode == ZWN_GOLDEN_RESTART)
            ASSERT(restarted);
        if (mode == ZWN_GOLDEN_DISCONNECT)
            ASSERT(dropped);
        ASSERT(terminal);
        ASSERT(state == VCS_SWARM_DL_COMPLETE);
        ASSERT(zwn_store_matches(b.store, &pkg));
        ASSERT(b_a.node->misbehavior == 0);
        if (!dropped)
            ASSERT(a_b.node->misbehavior == 0);

        /* Accounting: B credited the verified bytes it received under the
         * serving peers' session keys (two keys in the disconnect run —
         * the work moved from A to A2 mid-download); A credited the
         * verified bytes it served to B's session key. The manifest wire
         * is verified+credited too, so the credit sits in [chunks,
         * chunks + manifest wire]. */
        {
            uint64_t credited = 0;
            uint8_t key[33];
            ASSERT(zwn_peer_key(b_a.node, key));
            struct vcs_service_key_totals kt;
            ASSERT(vcs_service_key_totals(b.book, key, ZWN_DAY, &kt));
            credited += kt.verified_bytes_downloaded;
            if (two_servers) {
                ASSERT(zwn_peer_key(b_a2.node, key));
                ASSERT(vcs_service_key_totals(b.book, key, ZWN_DAY, &kt));
                credited += kt.verified_bytes_downloaded;
            }
            ASSERT(credited >= pkg.total_bytes);
            ASSERT(credited <= pkg.total_bytes + pkg.wire_len);
        }
        {
            uint64_t served = 0;
            uint8_t key[33];
            ASSERT(zwn_peer_key(a_b.node, key));
            struct vcs_service_key_totals kt;
            ASSERT(vcs_service_key_totals(a.book, key, ZWN_DAY, &kt));
            served += kt.verified_bytes_uploaded;
            if (two_servers) {
                ASSERT(zwn_peer_key(a2_b.node, key));
                ASSERT(vcs_service_key_totals(a2.book, key, ZWN_DAY, &kt));
                served += kt.verified_bytes_uploaded;
            }
            ASSERT(served >= pkg.total_bytes);
            /* Non-plain runs discard answered-but-undelivered DATAs with
             * the drained socket queues, so the surviving server
             * genuinely re-serves those chunks (upload credit is
             * recorded at serve time, against the request id — that is
             * honest); the slack bound is the worst-case in-flight set
             * at the drain. The plain run keeps the exact bound. */
            uint64_t slack = mode == ZWN_GOLDEN_PLAIN
                                 ? 0
                                 : VCS_SWARM_PEER_INFLIGHT_MAX *
                                       ZWN_MAX_FILE;
            ASSERT(served <= pkg.total_bytes + pkg.wire_len + slack);
        }

        zwn_link_free(&a_b);
        zwn_link_free(&b_a);
        if (two_servers) {
            zwn_link_free(&a2_b);
            zwn_link_free(&b_a2);
            zwn_node_free(&a2);
        }
        zwn_node_free(&a);
        zwn_node_free(&b);
        zwn_free_package(&pkg);
        PASS();
    } _test_next:;

    return failures;
}

/* ── 2: the malicious server ────────────────────────────────────────── */

static int zwn_t_malicious(const struct chain_params *params)
{
    int failures = 0;
    TEST("malicious server: wrong-hash chunks -> INVALID_CHUNK offence on "
         "the real peer object, no credit, nothing stored, named failure") {
        struct zwn_pkg pkg;
        ASSERT(zwn_make_package(&pkg, 4, 0x22));

        struct zwn_node a, b;
        ASSERT(zwn_node_init(&a, "ma", params));
        ASSERT(zwn_node_init(&b, "mb", params));
        ASSERT(zwn_store_package(a.store, &pkg));
        a.tamper_chunks = true;

        struct zwn_link a_b, b_a;
        ASSERT(zwn_link(&a, &a_b, 5, 6, 7, 8, "peer-b"));
        ASSERT(zwn_link(&b, &b_a, 1, 2, 3, 4, "peer-a"));
        ASSERT(zwn_meet_side(&a, &a_b));
        ASSERT(zwn_meet_side(&b, &b_a));

        ASSERT(vcs_swarm_engine_fetch(b.engine, pkg.root, ZWN_DAY,
                                      ++b.now) == VCS_SWARM_FETCH_OK);

        enum vcs_swarm_download_state state = VCS_SWARM_DL_INACTIVE;
        bool terminal = false;
        for (int i = 0; i < 800 && !terminal; i++) {
            ASSERT(zwn_round(&a_b, &b_a, params->pchMessageStart));
            terminal = zwn_download_done(&b, pkg.root, &state);
        }

        ASSERT(terminal);
        /* The bounded-attempts rule names the failure; the peer's real
         * misbehavior score carries the typed offence (and at the
         * default 100-point threshold the auto-ban disconnected it:
         * INVALID_CHUNK weighs 50, so the second bad chunk bans). */
        ASSERT(state == VCS_SWARM_DL_FAILED);
        {
            struct vcs_swarm_download_status st;
            memset(&st, 0, sizeof(st));
            ASSERT(vcs_swarm_engine_download_status(b.engine, pkg.root,
                                                    &st));
            ASSERT(st.rule != NULL);
        }
        ASSERT(b_a.node->misbehavior >=
               peer_offence_weight(PEER_OFFENCE_INVALID_CHUNK));
        /* Verify-before-store: not one bad chunk byte reached the CAS. */
        for (size_t i = 0; i < pkg.count; i++)
            ASSERT(!vcs_package_store_chunk_present(b.store, pkg.root,
                                                    (uint32_t)i, 0));
        /* No credit for unverified bytes; the book names them. */
        {
            uint8_t key[33];
            ASSERT(zwn_peer_key(b_a.node, key));
            struct vcs_service_key_totals kt;
            ASSERT(vcs_service_key_totals(b.book, key, ZWN_DAY, &kt));
            ASSERT(kt.verified_bytes_downloaded <= pkg.wire_len);
            ASSERT(kt.no_credit_bytes > 0);
            ASSERT(kt.offence_total > 0);
        }

        zwn_link_free(&a_b);
        zwn_link_free(&b_a);
        zwn_node_free(&a);
        zwn_node_free(&b);
        zwn_free_package(&pkg);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 3: unrequested bytes ───────────────────────────────────────────── */

static int zwn_t_unrequested(const struct chain_params *params)
{
    int failures = 0;
    TEST("unrequested DATA: UNREQUESTED offence on the wire, no credit") {
        struct zwn_pkg pkg;
        ASSERT(zwn_make_package(&pkg, 2, 0x33));

        struct zwn_node a, b;
        ASSERT(zwn_node_init(&a, "ua", params));
        ASSERT(zwn_node_init(&b, "ub", params));

        struct zwn_link a_b, b_a;
        ASSERT(zwn_link(&a, &a_b, 5, 6, 7, 8, "peer-b"));
        ASSERT(zwn_link(&b, &b_a, 1, 2, 3, 4, "peer-a"));
        ASSERT(zwn_meet_side(&a, &a_b));
        ASSERT(zwn_meet_side(&b, &b_a));

        /* A hand-built DATA frame with a request id B never issued —
         * queued straight onto the wire, no engine involvement. */
        struct vcs_package_swarm_message data;
        memset(&data, 0, sizeof(data));
        data.type = VCS_PACKAGE_SWARM_DATA;
        data.body.data.object.request_id = 424242;
        data.body.data.object.object_kind = VCS_PACKAGE_SWARM_OBJECT_CHUNK;
        memcpy(data.body.data.object.package_root, pkg.root, 32);
        data.body.data.object.file_index = 0;
        data.body.data.object.chunk_index = 0;
        ASSERT(vcs_package_chunk_hash(pkg.contents[0], pkg.lens[0],
                                      data.body.data.object.expected_hash));
        data.body.data.bytes = pkg.contents[0];
        data.body.data.bytes_len = (uint32_t)pkg.lens[0];
        uint8_t frame[1024];
        size_t frame_len = 0;
        ASSERT(vcs_package_swarm_serialize(&data, frame, sizeof(frame),
                                           &frame_len));
        zwn_send(&a, a_b.node, frame, frame_len);
        ASSERT(zwn_pump(&a_b, &b_a, params->pchMessageStart));

        ASSERT(b_a.node->misbehavior ==
               peer_offence_weight(PEER_OFFENCE_UNREQUESTED));
        {
            uint8_t key[33];
            ASSERT(zwn_peer_key(b_a.node, key));
            struct vcs_service_key_totals kt;
            ASSERT(vcs_service_key_totals(b.book, key, ZWN_DAY, &kt));
            ASSERT(kt.verified_bytes_downloaded == 0);
            ASSERT(kt.offence_total > 0);
        }

        zwn_link_free(&a_b);
        zwn_link_free(&b_a);
        zwn_node_free(&a);
        zwn_node_free(&b);
        zwn_free_package(&pkg);
        PASS();
    } _test_next:;

    return failures;
}

int test_zcode_swarm_net(void)
{
    int failures = 0;
    chain_params_select(CHAIN_MAIN);
    const struct chain_params *params = chain_params_get();

    failures += zwn_test_golden(ZWN_GOLDEN_PLAIN, params);
    failures += zwn_test_golden(ZWN_GOLDEN_RESTART, params);
    failures += zwn_test_golden(ZWN_GOLDEN_DISCONNECT, params);
    failures += zwn_t_malicious(params);
    failures += zwn_t_unrequested(params);
    return failures;
}
