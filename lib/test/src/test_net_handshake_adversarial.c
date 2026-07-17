/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Adversarial coverage of the P2P version/verack handshake plus the
 * addr/getaddr message family (lib/net/src/msg_version.c and
 * lib/net/src/msgprocessor_inv.c). Complements test_net_msg_dos (which
 * covers inv/getdata/headers floods post-handshake) by covering the
 * HANDSHAKE + ADDR surface that group does not.
 *
 * Every case drives the REAL, current handler — process_version() /
 * process_verack() directly (both are the public test seam declared in
 * net/msg_internal.h; mp_handle_version/mp_handle_verack are thin
 * dispatch-table adapters around them, see msgprocessor_handshake.c),
 * or the full msg_process_messages() dispatch loop with a real,
 * checksummed wire message for behavior that lives in the dispatch
 * table itself (the "before handshake" gate) rather than in a single
 * handler. No handler source is modified — these tests only pin the
 * existing defensive contract.
 *
 * p2p_node.socket is a real socketpair() end (not ZCL_INVALID_SOCKET):
 * p2p_node_end_message() opportunistically calls socket_send_data()
 * on the FIRST queued segment, and send() on an invalid fd fails with
 * EBADF, which p2p_node_close_socket() turns into node->disconnect =
 * true as a side effect — that would corrupt the very disconnect
 * assertions these tests make. A real socketpair fd makes send()
 * succeed (small handshake messages fit well under the default AF_UNIX
 * buffer), so disconnect reflects only the protocol decision under
 * test.
 *
 * Determinism: no wall-clock reads. All timestamps are fixed epoch
 * constants; addr-timestamp sanitization is tested directly against
 * addr_info_is_terrible() with a fixed "now" anchor. */

#include "test/test_helpers.h"

#include "chain/chainparams.h"
#include "core/hash.h"
#include "net/msg_internal.h"
#include "net/version.h"
#include "platform/time_compat.h"
#include "sync/sync_state.h"
#include "storage/topology_store.h"
#include "util/safe_alloc.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Fixed epoch anchor — never read the wall clock in this group. */
#define HS_FIXED_NOW ((int64_t)1700000000)

/* ── Fixture: net_manager + msg_processor + a single p2p_node backed by
 * a real socketpair end (see file banner for why not ZCL_INVALID_SOCKET). */

struct hs_fixture {
    struct net_manager nm;
    struct msg_processor mp;
    struct p2p_node node;
    int peer_fd;
};

static bool hs_fixture_setup(struct hs_fixture *f, bool inbound)
{
    memset(f, 0, sizeof(*f));
    net_manager_init(&f->nm);
    f->nm.local_host_nonce = 0x1122334455667788ULL;

    f->mp.params = chain_params_get();
    f->mp.net_mgr = &f->nm;

#ifdef ZCL_TESTING
    /* g_has_external_ip in msg_version.c is a file-static global; make
     * sure no earlier test in this process left it set (defensive —
     * this group never sets it itself). */
    msg_version_clear_external_ip_for_test();
#endif

    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
        return false;

    f->node.socket = fds[0];
    f->peer_fd = fds[1];
    f->node.inbound = inbound;
    f->node.state = inbound ? PEER_CONNECTED : PEER_CONNECTING;
    f->node.id = 4242;
    snprintf(f->node.addr_name, sizeof(f->node.addr_name), "198.51.100.7:8033");
    memset(f->node.addr.svc.addr.ip, 0, 10);
    f->node.addr.svc.addr.ip[10] = 0xff;
    f->node.addr.svc.addr.ip[11] = 0xff;
    f->node.addr.svc.addr.ip[12] = 198;
    f->node.addr.svc.addr.ip[13] = 51;
    f->node.addr.svc.addr.ip[14] = 100;
    f->node.addr.svc.addr.ip[15] = 7;
    f->node.addr.svc.port = 8033;

    zcl_mutex_init(&f->node.cs_send);
    zcl_mutex_init(&f->node.cs_recv);
    return true;
}

static void hs_fixture_teardown(struct hs_fixture *f)
{
    close(f->peer_fd);

    struct send_segment *seg = f->node.send_head;
    while (seg) {
        struct send_segment *next = seg->next;
        send_segment_free(seg);
        seg = next;
    }

    if (f->node.recv_msgs) {
        for (size_t i = 0; i < f->node.recv_msg_count; i++)
            net_message_free(&f->node.recv_msgs[i]);
        free(f->node.recv_msgs);
    }

    close_socket(&f->node.socket);
    zcl_mutex_destroy(&f->node.cs_send);
    zcl_mutex_destroy(&f->node.cs_recv);
    net_manager_free(&f->nm);
}

/* ── Wire-message builder for full msg_process_messages() driving ──
 * (only needed for behavior that lives in the dispatch loop itself —
 * the "before handshake" gate and the addr/getaddr handlers, which
 * are reached only through the dispatch table). */

static void hs_build_wire_message(struct net_message *msg, const char *command,
                                  const struct byte_stream *payload)
{
    static const unsigned char msgstart[MESSAGE_START_SIZE] = {
        0x24, 0xe9, 0x27, 0x64
    };
    unsigned int len = (unsigned int)payload->size;
    unsigned char hash[SHA256_OUTPUT_SIZE];

    net_message_init(msg, msgstart);
    msg_header_init_full(&msg->hdr, msgstart, command, len);
    hash256(len ? payload->data : (const unsigned char *)"", len, hash);
    memcpy(&msg->hdr.nChecksum, hash, sizeof(msg->hdr.nChecksum));

    if (len > 0) {
        msg->recv_data = zcl_malloc(len, "hs_test_wire_payload");
        memcpy(msg->recv_data, payload->data, len);
        msg->recv_alloc = len;
    }
    msg->data_pos = len;
    msg->in_data = true;
}

/* Queue exactly one real wire message and run it through the actual
 * inbound dispatch loop (msgprocessor.c::msg_process_messages). Frees
 * any previous single-message buffer this helper installed. */
static bool hs_drive_message(struct msg_processor *mp, struct p2p_node *node,
                             const char *command, struct byte_stream *payload)
{
    if (node->recv_msgs) {
        for (size_t i = 0; i < node->recv_msg_count; i++)
            net_message_free(&node->recv_msgs[i]);
        free(node->recv_msgs);
        node->recv_msgs = NULL;
        node->recv_msg_count = 0;
        node->recv_msg_cap = 0;
    }

    struct net_message msg;
    hs_build_wire_message(&msg, command, payload);

    struct net_message *buf = zcl_calloc(1, sizeof(*buf), "hs_test_recv_buf");
    buf[0] = msg;
    node->recv_msgs = buf;
    node->recv_msg_count = 1;
    node->recv_msg_cap = 1;

    return msg_process_messages(mp, node);
}

/* p2p_node_end_message() synchronously calls send() on a segment that is
 * first in the queue (see net.c::socket_send_data). With a real
 * socketpair fd that send() actually succeeds, so by the time a driving
 * helper returns, any reply the handler queued has already been written
 * to the socket AND dequeued/freed on our side — node->send_head is back
 * to empty. The only way to observe what was actually sent is to read
 * the OTHER end of the socketpair (f.peer_fd). */

#define HS_CAPTURE_CAP 8192

struct hs_capture {
    uint8_t buf[HS_CAPTURE_CAP];
    size_t len;
};

static void hs_capture_sent(int peer_fd, struct hs_capture *cap)
{
    cap->len = 0;
    for (;;) {
        ssize_t n = recv(peer_fd, cap->buf + cap->len,
                         sizeof(cap->buf) - cap->len, MSG_DONTWAIT);
        if (n <= 0)
            break;
        cap->len += (size_t)n;
        if (cap->len >= sizeof(cap->buf))
            break;
    }
}

/* Locate cmd's message-header start within a captured byte run (wire
 * layout: msgstart[4] command[12] size[4] checksum[4] payload...).
 * Returns the header start offset, or -1 if not found. */
static ssize_t hs_find_command_header(const struct hs_capture *cap,
                                      const char *cmd)
{
    char padded[COMMAND_SIZE];
    memset(padded, 0, sizeof(padded));
    strncpy(padded, cmd, COMMAND_SIZE);

    if (cap->len < (size_t)MSG_HEADER_SIZE)
        return -1;
    for (size_t i = MESSAGE_START_SIZE; i + COMMAND_SIZE <= cap->len; i++) {
        if (memcmp(cap->buf + i, padded, COMMAND_SIZE) == 0)
            return (ssize_t)(i - MESSAGE_START_SIZE);
    }
    return -1;
}

static bool hs_captured_has_command(const struct hs_capture *cap, const char *cmd)
{
    return hs_find_command_header(cap, cmd) >= 0;
}

/* ── version-message payload builders (direct handler driving) ──── */

static void hs_build_version_payload_relay(struct byte_stream *out,
                                           int32_t proto, uint64_t nonce,
                                           const char *subver, bool relay)
{
    struct version_message ver;
    version_message_init(&ver);
    ver.protocol_version = proto;
    ver.services = NODE_NETWORK;
    ver.timestamp = HS_FIXED_NOW;
    ver.nonce = nonce;
    snprintf(ver.sub_version, sizeof(ver.sub_version), "%s",
             subver ? subver : "/test:0.1/");
    ver.start_height = 100;
    ver.relay = relay;

    stream_init(out, 128);
    version_message_serialize(&ver, out);
}

static void hs_build_version_payload(struct byte_stream *out, int32_t proto,
                                     uint64_t nonce, const char *subver)
{
    hs_build_version_payload_relay(out, proto, nonce, subver, true);
}

/* A version payload whose subver compact-size length claims 1000 bytes
 * (>= MAX_SUBVER_LENGTH=256). Deliberately does NOT include 1000 bytes
 * of subver data — version_message_deserialize() rejects on the length
 * field alone, before it ever reads the (absent) bytes, so this stays
 * a small, well-formed-except-for-the-length-claim payload. */
static void hs_build_oversized_subver_payload(struct byte_stream *out)
{
    struct net_address addr;
    net_address_init(&addr);

    stream_init(out, 64);
    stream_write_i32_le(out, PROTOCOL_VERSION);
    stream_write_u64_le(out, NODE_NETWORK);
    stream_write_i64_le(out, HS_FIXED_NOW);
    net_address_serialize(&addr, out, false);
    net_address_serialize(&addr, out, false);
    stream_write_u64_le(out, 0xCAFEBABE12345678ULL);
    stream_write_compact_size(out, 1000); /* subver_len — garbage/oversized */
}

/* ── addr entry builder (public IP range so net_addr_is_routable()
 * accepts it — mirrors test_addrman_rebalance.c's convention). ───── */

static struct net_address hs_make_pub_addr(uint8_t a, uint8_t b, uint8_t c,
                                           uint8_t d, uint16_t port,
                                           uint32_t ntime)
{
    struct net_address addr;
    memset(&addr, 0, sizeof(addr));
    memset(addr.svc.addr.ip, 0, 10);
    addr.svc.addr.ip[10] = 0xff;
    addr.svc.addr.ip[11] = 0xff;
    addr.svc.addr.ip[12] = a;
    addr.svc.addr.ip[13] = b;
    addr.svc.addr.ip[14] = c;
    addr.svc.addr.ip[15] = d ? d : 1;
    addr.svc.port = port;
    addr.nTime = ntime;
    addr.nServices = 1;
    return addr;
}

static void hs_build_addr_payload(struct byte_stream *out,
                                  const struct net_address *addrs, size_t n)
{
    stream_init(out, n * 30 + 8);
    stream_write_compact_size(out, (uint64_t)n);
    for (size_t i = 0; i < n; i++)
        net_address_serialize(&addrs[i], out, true);
}

/* addr message declaring only a compact-size count with NO actual
 * entries. process_addr() reads the count then checks it against
 * MAX_ADDR_TO_SEND before ever reading an entry (msgprocessor_inv.c),
 * so a well-formed-but-empty payload is enough to reach the cap. */
static void hs_build_addr_count_only_payload(struct byte_stream *out,
                                             uint64_t count)
{
    stream_init(out, 16);
    stream_write_compact_size(out, count);
}

/* ── 1. version below MIN_PEER_PROTO_VERSION -> rejected, disconnected. */

static int test_version_too_old_rejected(void)
{
    int failures = 0;
    TEST("handshake: version below MIN_PEER_PROTO_VERSION is rejected + disconnected") {
        struct hs_fixture f;
        ASSERT(hs_fixture_setup(&f, true));

        struct byte_stream payload;
        hs_build_version_payload(&payload, MIN_PEER_PROTO_VERSION - 1,
                                 0xAAAAAAAAAAAAAAAAULL, NULL);

        /* Rides on msg_version.c:process_version()'s
         * `ver.protocol_version < MIN_PEER_PROTO_VERSION` check, which
         * LOG_FAILs (logs + returns false) before node->version is ever
         * assigned or any reply is queued. */
        bool ok = process_version(&f.mp, &f.node, &payload);
        ASSERT(!ok);
        ASSERT(f.node.disconnect);
        ASSERT(f.node.version == 0);

        struct hs_capture cap;
        hs_capture_sent(f.peer_fd, &cap);
        ASSERT_EQ(cap.len, 0); /* rejected before any reply was queued */

        stream_free(&payload);
        hs_fixture_teardown(&f);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 2. any message before version -> rejected without crash, peer
 * state intact. Rides on msgprocessor.c's dispatch loop
 * (`e->requires_handshake && node->version == 0` gate) — the handler
 * (process_ping) is never even invoked. */

static int test_message_before_version_rejected(void)
{
    int failures = 0;
    TEST("handshake: message before version is rejected pre-dispatch, no crash") {
        struct hs_fixture f;
        ASSERT(hs_fixture_setup(&f, true));
        ASSERT_EQ(f.node.version, 0);

        struct byte_stream ping_payload;
        stream_init(&ping_payload, 8);
        stream_write_u64_le(&ping_payload, 0x0102030405060708ULL);

        bool ok = hs_drive_message(&f.mp, &f.node, "ping", &ping_payload);
        ASSERT(ok); /* msg_process_messages itself always returns true */
        ASSERT(f.node.disconnect);
        ASSERT_EQ(f.node.version, 0); /* handler never ran to set anything */
        /* process_ping would have queued a "pong" reply if it had been
         * dispatched — its absence proves the handler was never called. */
        struct hs_capture cap;
        hs_capture_sent(f.peer_fd, &cap);
        ASSERT_EQ(cap.len, 0);

        stream_free(&ping_payload);
        hs_fixture_teardown(&f);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 3. duplicate version -> handled per the real code: rejected via
 * the top-of-function `node->version != 0` guard (emits
 * EV_PEER_MISBEHAVE + LOG_FAILs before even deserializing the second
 * payload), connection state left exactly as after the first (valid)
 * version — not corrupted. */

static int test_duplicate_version_rejected(void)
{
    int failures = 0;
    TEST("handshake: duplicate version rejected, first handshake's state intact") {
        struct hs_fixture f;
        ASSERT(hs_fixture_setup(&f, true));

        struct byte_stream first;
        hs_build_version_payload(&first, PROTOCOL_VERSION, 0x1111111111111111ULL,
                                 "/test:0.1/");
        ASSERT(process_version(&f.mp, &f.node, &first));
        ASSERT_EQ(f.node.version, PROTOCOL_VERSION);
        enum peer_state state_after_first = f.node.state;
        int version_after_first = f.node.version;
        stream_free(&first);

        /* Drain whatever the first (successful) handshake wrote before
         * driving the duplicate, so the post-duplicate capture below
         * reflects only the duplicate call. */
        struct hs_capture drain;
        hs_capture_sent(f.peer_fd, &drain);

        struct byte_stream second;
        hs_build_version_payload(&second, PROTOCOL_VERSION, 0x2222222222222222ULL,
                                 "/test:0.1/");
        bool ok = process_version(&f.mp, &f.node, &second);
        ASSERT(!ok);
        /* Rejected before touching node fields — unchanged from the
         * first (successful) handshake. */
        ASSERT_EQ(f.node.version, version_after_first);
        ASSERT(f.node.state == state_after_first);

        struct hs_capture cap;
        hs_capture_sent(f.peer_fd, &cap);
        ASSERT_EQ(cap.len, 0); /* the duplicate produced no new reply */

        stream_free(&second);
        hs_fixture_teardown(&f);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 4. self-connection: version carrying our own nonce -> detected,
 * connection dropped. Rides on msg_version.c's
 * `ver.nonce == mp->net_mgr->local_host_nonce` guard. */

static int test_self_connection_detected(void)
{
    int failures = 0;
    TEST("handshake: version carrying our own nonce is detected as self-connect") {
        struct hs_fixture f;
        ASSERT(hs_fixture_setup(&f, true));

        struct byte_stream payload;
        hs_build_version_payload(&payload, PROTOCOL_VERSION,
                                 f.nm.local_host_nonce, NULL);

        bool ok = process_version(&f.mp, &f.node, &payload);
        ASSERT(!ok);
        ASSERT(f.node.disconnect);
        ASSERT(f.node.version == 0); /* short-circuited before assignment */

        struct hs_capture cap;
        hs_capture_sent(f.peer_fd, &cap);
        ASSERT_EQ(cap.len, 0); /* rejected before any reply was queued */

        stream_free(&payload);
        hs_fixture_teardown(&f);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 5. addr message declaring more than MAX_ADDR_TO_SEND -> rejected +
 * disconnected, no unbounded processing. Rides on
 * msgprocessor_inv.c::process_addr() -> msg_count_exceeds(). */

static int test_addr_over_cap_rejected(void)
{
    int failures = 0;
    TEST("addr: count over MAX_ADDR_TO_SEND is rejected + disconnected") {
        struct hs_fixture f;
        ASSERT(hs_fixture_setup(&f, true));
        f.node.version = PROTOCOL_VERSION; /* already-handshaked peer fixture,
                                             * matching the existing
                                             * test_msg_handlers.c convention */

        struct byte_stream payload;
        hs_build_addr_count_only_payload(&payload, MAX_ADDR_TO_SEND + 1);

        bool ok = hs_drive_message(&f.mp, &f.node, "addr", &payload);
        ASSERT(ok);
        ASSERT(f.node.disconnect);
        /* No addrman entries were added — the handler bailed before the
         * per-entry loop, so random_size stays at zero. */
        ASSERT_EQ(f.nm.addrman.random_size, 0);

        stream_free(&payload);
        hs_fixture_teardown(&f);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 6. getaddr response is bounded by the wire cap (MAX_ADDR_TO_SEND)
 * and answers at most once per peer. Rides on
 * msgprocessor_inv.c::process_getaddr() (the `addrs[MAX_ADDR_TO_SEND]`
 * stack array + `node->sent_addr` guard). */

static int test_getaddr_bounded_and_answered_once(void)
{
    int failures = 0;
    TEST("getaddr: response count never exceeds MAX_ADDR_TO_SEND, answered once") {
        struct hs_fixture f;
        ASSERT(hs_fixture_setup(&f, true));
        f.node.version = PROTOCOL_VERSION;

        /* Feed 50 distinct routable addresses through the real addr
         * handler so addrman has something to return. addrman_get_addr()
         * filters entries via addr_info_is_terrible(..., GetAdjustedTime())
         * — the REAL current time, not the fixed HS_FIXED_NOW anchor used
         * elsewhere in this file — so these entries need a genuinely
         * recent nTime or the real "terrible" rule (tested directly and
         * deterministically in test_addr_timestamp_sanitization_rule)
         * would filter every one of them out as stale. */
        struct net_address addrs[50];
        uint32_t recent = (uint32_t)platform_time_wall_time_t() - 60;
        for (int i = 0; i < 50; i++)
            addrs[i] = hs_make_pub_addr(60, (uint8_t)(i / 4), (uint8_t)(i % 4),
                                        1, 8033, recent);
        struct byte_stream addr_payload;
        hs_build_addr_payload(&addr_payload, addrs, 50);
        ASSERT(hs_drive_message(&f.mp, &f.node, "addr", &addr_payload));
        ASSERT(!f.node.disconnect);
        stream_free(&addr_payload);

        struct byte_stream empty;
        stream_init(&empty, 0);
        ASSERT(hs_drive_message(&f.mp, &f.node, "getaddr", &empty));
        ASSERT(!f.node.disconnect);

        struct hs_capture cap;
        hs_capture_sent(f.peer_fd, &cap);
        ssize_t addr_hdr = hs_find_command_header(&cap, "addr");
        ASSERT(addr_hdr >= 0);
        size_t payload_off = (size_t)addr_hdr + (size_t)MSG_HEADER_SIZE;
        ASSERT(payload_off <= cap.len);
        struct byte_stream reply_payload;
        stream_init_from_data(&reply_payload, cap.buf + payload_off,
                              cap.len - payload_off);
        uint64_t sent_count = 0;
        ASSERT(stream_read_compact_size(&reply_payload, &sent_count));
        ASSERT(sent_count > 0);
        ASSERT(sent_count <= MAX_ADDR_TO_SEND);
        ASSERT(f.node.sent_addr);

        /* Repeat getaddr on the same peer: sent_addr already true, so
         * process_getaddr's early-return means no second "addr" reply
         * is queued/sent. */
        ASSERT(hs_drive_message(&f.mp, &f.node, "getaddr", &empty));
        struct hs_capture cap2;
        hs_capture_sent(f.peer_fd, &cap2);
        ASSERT_EQ(cap2.len, 0);

        stream_free(&empty);
        hs_fixture_teardown(&f);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 6b. a real addr message drives the topology graph, not just
 * addrman: process_addr() (msgprocessor_inv.c) records one
 * storage/topology_store.h edge per deserialized entry, keyed on the
 * already-handshaked peer as observer. The fixture's baked-in node addr
 * (198.51.100.7, an RFC5737 documentation address) is deliberately
 * non-routable for the other handshake cases in this file, but
 * topology_store's own net_addr_is_routable() gate would silently
 * reject every edge with it as observer — override it with a genuine
 * public address (mirrors hs_make_pub_addr's convention) so this test
 * exercises the accept path, not the reject path. */

static int test_addr_message_records_topology_edge(void)
{
    int failures = 0;
    TEST("addr: a real addr message records a topology graph edge") {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "hs_topology", "edge");
        ASSERT(topology_store_open(dir));
        topology_store_test_reset();

        struct hs_fixture f;
        ASSERT(hs_fixture_setup(&f, true));
        f.node.version = PROTOCOL_VERSION; /* already-handshaked peer,
                                             * matching the other addr
                                             * cases in this file */
        uint32_t recent = (uint32_t)platform_time_wall_time_t() - 60;
        f.node.addr = hs_make_pub_addr(60, 9, 9, 1, 8033, recent);

        struct net_address advertised[2] = {
            hs_make_pub_addr(60, 10, 1, 1, 8033, recent),
            hs_make_pub_addr(60, 10, 2, 1, 8033, recent),
        };
        struct byte_stream payload;
        hs_build_addr_payload(&payload, advertised, 2);
        ASSERT(hs_drive_message(&f.mp, &f.node, "addr", &payload));
        ASSERT(!f.node.disconnect);

        ASSERT_EQ(topology_store_test_edge_count(), 2);

        stream_free(&payload);
        hs_fixture_teardown(&f);
        topology_store_close();
        test_cleanup_tmpdir(dir);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 7. addr timestamp sanitization: far-future / far-past timestamps
 * are the real "terrible" predicate (addrman.c::addr_info_is_terrible)
 * that addrman_get_addr() filters the getaddr response through. Tested
 * directly against the real, current predicate with a fixed anchor —
 * no wall clock, fully deterministic. */

static int test_addr_timestamp_sanitization_rule(void)
{
    int failures = 0;
    TEST("addr: far-future/far-past timestamps are 'terrible' per the real rule") {
        const int64_t now = HS_FIXED_NOW;

        struct addr_info sane;
        memset(&sane, 0, sizeof(sane));
        sane.addr = hs_make_pub_addr(61, 1, 1, 1, 8033,
                                     (uint32_t)(now - 3600)); /* 1h old */
        ASSERT(!addr_info_is_terrible(&sane, now));

        struct addr_info future;
        memset(&future, 0, sizeof(future));
        future.addr = hs_make_pub_addr(61, 1, 1, 2, 8033,
                                       (uint32_t)(now + 1000 * 24 * 3600));
        ASSERT(addr_info_is_terrible(&future, now));

        struct addr_info stale;
        memset(&stale, 0, sizeof(stale));
        stale.addr = hs_make_pub_addr(61, 1, 1, 3, 8033,
                                      (uint32_t)(now - 60 * 24 * 3600)); /* 60d old */
        ASSERT(addr_info_is_terrible(&stale, now));

        struct addr_info zero_time;
        memset(&zero_time, 0, sizeof(zero_time));
        zero_time.addr = hs_make_pub_addr(61, 1, 1, 4, 8033, 0);
        ASSERT(addr_info_is_terrible(&zero_time, now));

        PASS();
    } _test_next:;
    return failures;
}

/* ── 8. oversized/garbage user-agent in version -> bounded/rejected,
 * no overflow. Rides on p2p_message.c::version_message_deserialize()'s
 * `subver_len >= MAX_SUBVER_LENGTH` bound, which fires before any
 * subver bytes are read. */

static int test_oversized_user_agent_rejected(void)
{
    int failures = 0;
    TEST("handshake: version with oversized subver length is bounded/rejected") {
        struct hs_fixture f;
        ASSERT(hs_fixture_setup(&f, true));

        struct byte_stream payload;
        hs_build_oversized_subver_payload(&payload);

        bool ok = process_version(&f.mp, &f.node, &payload);
        ASSERT(!ok); /* version_message_deserialize's LOG_FAIL propagates */
        ASSERT_EQ(f.node.version, 0);
        ASSERT_EQ(f.node.sub_ver[0], '\0');

        stream_free(&payload);
        hs_fixture_teardown(&f);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 9. control: an honest peer's handshake still completes end to end
 * after all of the above. Proves these adversarial cases pin real
 * defensive behavior without breaking the ordinary path. */

static int test_honest_handshake_completes(void)
{
    int failures = 0;
    TEST("handshake: honest inbound version+verack still completes (control)") {
        struct hs_fixture f;
        ASSERT(hs_fixture_setup(&f, true));

        struct byte_stream version_payload;
        hs_build_version_payload(&version_payload, PROTOCOL_VERSION,
                                 0x9999999999999999ULL, "/test:0.1/");
        ASSERT(hs_drive_message(&f.mp, &f.node, "version", &version_payload));
        ASSERT(!f.node.disconnect);
        ASSERT_EQ(f.node.version, PROTOCOL_VERSION);
        ASSERT(f.node.state == PEER_HANDSHAKE_COMPLETE);

        struct hs_capture cap;
        hs_capture_sent(f.peer_fd, &cap);
        ASSERT(hs_captured_has_command(&cap, "verack"));

        stream_free(&version_payload);
        hs_fixture_teardown(&f);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 10-12. Mempool sync-on-connect (msg_tx.c::msg_tx_maybe_request_mempool,
 * wired into process_verack() in msg_version.c). A fresh node never
 * proactively pulled a peer's mempool before this; now it sends ONE
 * outbound "mempool" message right after the verack round-trip confirms
 * the handshake, gated on relay_txes and on not being deep in IBD. */

/* sync_get_state() is a process-wide FSM shared with every other test
 * group forked from test_parallel — restore to SYNC_IDLE around any case
 * that forces IBD so later cases in this same forked process (all of
 * test_net_handshake_adversarial runs in one process) see the ordinary
 * default. Mirrors test_msg_handlers.c's test_msg_sync_to_idle /
 * test_msg_sync_to_blocks_download. */
static void hs_force_sync_idle(void)
{
    enum sync_state cur = sync_get_state();
    if (cur == SYNC_IDLE)
        return;
    if (cur == SYNC_AT_TIP) {
        (void)sync_set_state(SYNC_IDLE, "hs mempool test cleanup");
        return;
    }
    if (cur == SYNC_REORG) {
        (void)sync_set_state(SYNC_AT_TIP, "hs mempool test cleanup");
        (void)sync_set_state(SYNC_IDLE, "hs mempool test cleanup");
        return;
    }
    (void)sync_set_state(SYNC_IDLE, "hs mempool test cleanup");
}

static void hs_force_sync_headers_download(void)
{
    hs_force_sync_idle();
    if (sync_get_state() != SYNC_IDLE)
        return;
    (void)sync_set_state(SYNC_FINDING_PEERS, "hs mempool test setup");
    (void)sync_set_state(SYNC_HEADERS_DOWNLOAD, "hs mempool test setup");
}

/* ── 10. Honest, relay-capable peer: exactly ONE outbound "mempool" is
 * queued right after the verack round-trip, and a second (duplicate)
 * verack from the same peer does NOT queue a second one — pins the
 * per-peer node->mempool_requested once-only guard. */

static int test_mempool_requested_once_for_relay_peer(void)
{
    int failures = 0;
    TEST("mempool sync-on-connect: queued exactly once for a relay-capable peer") {
        struct hs_fixture f;
        ASSERT(hs_fixture_setup(&f, true));
        hs_force_sync_idle();
        ASSERT(!f.node.mempool_requested);

        struct byte_stream version_payload;
        hs_build_version_payload_relay(&version_payload, PROTOCOL_VERSION,
                                       0xAAAABBBBCCCCDDDDULL, "/test:0.1/",
                                       true /* relay */);
        ASSERT(hs_drive_message(&f.mp, &f.node, "version", &version_payload));
        ASSERT(!f.node.disconnect);
        ASSERT(f.node.relay_txes);
        stream_free(&version_payload);

        /* Drain the version-triggered replies (verack/version/sendheaders)
         * so the capture below reflects only the verack-triggered send. */
        struct hs_capture drain;
        hs_capture_sent(f.peer_fd, &drain);

        struct byte_stream empty;
        stream_init(&empty, 0);
        ASSERT(hs_drive_message(&f.mp, &f.node, "verack", &empty));
        ASSERT(!f.node.disconnect);
        ASSERT(f.node.mempool_requested);

        struct hs_capture cap;
        hs_capture_sent(f.peer_fd, &cap);
        ASSERT(hs_captured_has_command(&cap, "mempool"));

        /* A second verack from the same peer (e.g. misbehaving/duplicate)
         * must NOT queue a second "mempool" — the guard is per-peer, not
         * per-call. */
        ASSERT(hs_drive_message(&f.mp, &f.node, "verack", &empty));
        struct hs_capture cap2;
        hs_capture_sent(f.peer_fd, &cap2);
        ASSERT(!hs_captured_has_command(&cap2, "mempool"));

        stream_free(&empty);
        hs_fixture_teardown(&f);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 11. A peer whose version explicitly declares relay=false never gets
 * an outbound "mempool" pull. */

static int test_mempool_not_requested_for_non_relay_peer(void)
{
    int failures = 0;
    TEST("mempool sync-on-connect: not queued for a non-relay peer") {
        struct hs_fixture f;
        ASSERT(hs_fixture_setup(&f, true));
        hs_force_sync_idle();

        struct byte_stream version_payload;
        hs_build_version_payload_relay(&version_payload, PROTOCOL_VERSION,
                                       0x1234123412341234ULL, "/test:0.1/",
                                       false /* relay */);
        ASSERT(hs_drive_message(&f.mp, &f.node, "version", &version_payload));
        ASSERT(!f.node.disconnect);
        ASSERT(!f.node.relay_txes);
        stream_free(&version_payload);

        struct hs_capture drain;
        hs_capture_sent(f.peer_fd, &drain);

        struct byte_stream empty;
        stream_init(&empty, 0);
        ASSERT(hs_drive_message(&f.mp, &f.node, "verack", &empty));
        ASSERT(!f.node.disconnect);
        ASSERT(!f.node.mempool_requested);

        struct hs_capture cap;
        hs_capture_sent(f.peer_fd, &cap);
        ASSERT(!hs_captured_has_command(&cap, "mempool"));

        stream_free(&empty);
        hs_fixture_teardown(&f);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 12. Deep in IBD (bulk historical sync): even a relay-capable peer's
 * verack must NOT trigger a mempool pull — mempool inventory is
 * irrelevant while headers/blocks are still catching up. */

static int test_mempool_not_requested_during_ibd(void)
{
    int failures = 0;
    TEST("mempool sync-on-connect: not queued while deep in IBD") {
        struct hs_fixture f;
        ASSERT(hs_fixture_setup(&f, true));
        hs_force_sync_headers_download();
        ASSERT_EQ(sync_get_state(), SYNC_HEADERS_DOWNLOAD);

        struct byte_stream version_payload;
        hs_build_version_payload_relay(&version_payload, PROTOCOL_VERSION,
                                       0x5678567856785678ULL, "/test:0.1/",
                                       true /* relay */);
        ASSERT(hs_drive_message(&f.mp, &f.node, "version", &version_payload));
        ASSERT(!f.node.disconnect);
        ASSERT(f.node.relay_txes);
        stream_free(&version_payload);

        struct hs_capture drain;
        hs_capture_sent(f.peer_fd, &drain);

        struct byte_stream empty;
        stream_init(&empty, 0);
        ASSERT(hs_drive_message(&f.mp, &f.node, "verack", &empty));
        ASSERT(!f.node.disconnect);
        ASSERT(!f.node.mempool_requested);

        struct hs_capture cap;
        hs_capture_sent(f.peer_fd, &cap);
        ASSERT(!hs_captured_has_command(&cap, "mempool"));

        stream_free(&empty);
        hs_force_sync_idle();
        hs_fixture_teardown(&f);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Entry point ───────────────────────────────────────────────── */

int test_net_handshake_adversarial(void);

int test_net_handshake_adversarial(void)
{
    int failures = 0;

    failures += test_version_too_old_rejected();
    failures += test_message_before_version_rejected();
    failures += test_duplicate_version_rejected();
    failures += test_self_connection_detected();
    failures += test_addr_over_cap_rejected();
    failures += test_getaddr_bounded_and_answered_once();
    failures += test_addr_message_records_topology_edge();
    failures += test_addr_timestamp_sanitization_rule();
    failures += test_oversized_user_agent_rejected();
    failures += test_honest_handshake_completes();
    failures += test_mempool_requested_once_for_relay_peer();
    failures += test_mempool_not_requested_for_non_relay_peer();
    failures += test_mempool_not_requested_during_ibd();

    return failures;
}
