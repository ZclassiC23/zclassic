/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Adversarial/DoS coverage for the P2P message-handling path. A hostile
 * peer fully controls the wire bytes; these cases pin the bounded-and-safe
 * contract of the handlers that see them FIRST — before any consensus or
 * business logic runs:
 *
 *   A. Oversized declared counts (inv/getdata/addr/notfound) — the classic
 *      "claim 10M items, send 3 bytes" allocation-bomb shape. Each of
 *      these handlers reads a compact-size element count and must reject
 *      it BEFORE looping/allocating once it exceeds the protocol cap
 *      (MAX_INV_SZ / MAX_ADDR_TO_SEND) — no huge alloc, no unbounded loop,
 *      peer disconnected AND scored via PEER_OFFENCE_FLOOD so the same
 *      peer address still accrues toward the ban threshold across
 *      reconnects (this file's fix: msg_tx.c/msg_blocks.c/
 *      msgprocessor_inv.c previously disconnected without scoring — see
 *      case A which pins the now-added peer_scoring_record calls).
 *   B. Truncated/short payloads (inv/getdata declare N items, deliver
 *      fewer bytes than N requires) — clean failure, no partial mutation.
 *   C. A message whose declared size exceeds MAX_PROTOCOL_MESSAGE_LENGTH
 *      at the framing layer (net_message_read_data) — rejected BEFORE any
 *      allocation against the process-wide recv budget.
 *   D. An unknown/garbage command string through the real dispatch loop
 *      (msg_process_messages) — silently ignored (Bitcoin Core parity:
 *      unknown commands are not misbehaviour), connection untouched, and
 *      a subsequent honest `ping` on the SAME connection still dispatches
 *      normally afterward.
 *   E. A duplicate/replayed `headers` batch — the second delivery of
 *      identical bytes is idempotent: accepted again (not an error) but
 *      counted as already-known (not newly-added), no duplicate block-tree
 *      entries, and no misbehaviour score for replaying old data.
 *
 * Sections A/B/E use a stack p2p_node + memset (mirrors
 * test_process_headers_adversarial.c) since the code paths under test
 * return before touching any node mutex. Sections C/D touch
 * mutex-guarded node/dispatch machinery and use a heap node from
 * p2p_node_create (properly mutex-initialized). */

#include "test/test_helpers.h"

#include "mining/miner.h"
#include "net/msg_internal.h"
#include "net/msgprocessor.h"
#include "net/peer_scoring.h"
#include "net/version.h"
#include "core/hash.h"
#include "validation/chainstate.h"
#include "validation/process_block.h"  /* accept_block_header */

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DOS_CHECK(name, expr) do { \
    printf("net_msg_dos: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* Non-localhost/non-whitelisted peer so peer_misbehaving() actually
 * scores it (is_trusted_peer() in net.c exempts 127.0.0.0/8 + whitelisted
 * peers from all scoring — using that address would make every
 * misbehavior assertion below vacuously true). */
static void dos_setup_stack_node(struct p2p_node *node)
{
    memset(node, 0, sizeof(*node));
    snprintf(node->addr_name, sizeof(node->addr_name), "203.0.113.77:8033");
    node->id = 77;
    node->addr.svc.addr.ip[10] = 0xff;
    node->addr.svc.addr.ip[11] = 0xff;
    node->addr.svc.addr.ip[12] = 5;
    node->addr.svc.addr.ip[13] = 6;
    node->addr.svc.addr.ip[14] = 7;
    node->addr.svc.addr.ip[15] = 8;
}

static const struct msg_dispatch_entry *dos_find_entry(const char *cmd)
{
    const struct msg_dispatch_entry *e = msg_get_dispatch_table();
    for (; e->handler; e++) {
        if (strcmp(e->command, cmd) == 0)
            return e;
    }
    return NULL;
}

int test_net_msg_dos(void);
int test_net_msg_dos(void)
{
    int failures = 0;
    printf("\n=== net_msg_dos adversarial tests ===\n");

    chain_params_select(CHAIN_REGTEST);
    const struct chain_params *cp = chain_params_get();
    peer_scoring_init();
    enum sync_state sync0 = sync_get_state();

    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "net_msg_dos", "main");
    SetDataDir(dir);

    struct main_state ms;
    main_state_init(&ms);
    struct uint256 gh = cp->consensus.hashGenesisBlock;
    struct block_index *gen =
        chainstate_insert_block_index((struct chainstate *)&ms, &gh);
    DOS_CHECK("genesis block_index inserted", gen != NULL);
    if (gen) {
        gen->nHeight = 0;
        gen->nStatus = BLOCK_HAVE_DATA | BLOCK_VALID_SCRIPTS;
        gen->nTx = 1;
        gen->nChainTx = 1;
        active_chain_move_window_tip(&ms.chain_active, gen);
        ms.pindex_best_header = gen;
    }

    struct net_manager nm;
    net_manager_init(&nm);

    struct msg_processor mp;
    msg_processor_init(&mp, &ms, NULL, NULL, cp, dir, &nm, NULL);

    /* ── A1. inv: oversized count -> reject + disconnect + scored ── */
    {
        struct p2p_node node;
        dos_setup_stack_node(&node);
        struct byte_stream s;
        stream_init(&s, 32);
        stream_write_compact_size(&s, 50001); /* MAX_INV_SZ (50000) + 1 */
        /* Tiny payload: the classic "claim 10M items, send 3 bytes" shape.
         * A vulnerable handler would loop/allocate against the DECLARED
         * count; the guard must fire before touching a single inv item. */
        bool ret = process_inv(&mp, &node, &s);
        DOS_CHECK("inv oversized: handler returns false", ret == false);
        DOS_CHECK("inv oversized: peer disconnected", node.disconnect == true);
        DOS_CHECK("inv oversized: peer scored PEER_OFFENCE_FLOOD (20)",
                 atomic_load(&node.misbehavior) == 20);
        stream_free(&s);
    }

    /* ── A2. getdata: oversized count -> reject + disconnect + scored ── */
    {
        struct p2p_node node;
        dos_setup_stack_node(&node);
        struct byte_stream s;
        stream_init(&s, 32);
        stream_write_compact_size(&s, 50001);
        bool ret = process_getdata(&mp, &node, &s);
        DOS_CHECK("getdata oversized: handler returns false", ret == false);
        DOS_CHECK("getdata oversized: peer disconnected",
                 node.disconnect == true);
        DOS_CHECK("getdata oversized: peer scored PEER_OFFENCE_FLOOD (20)",
                 atomic_load(&node.misbehavior) == 20);
        stream_free(&s);
    }

    /* ── A3. addr: oversized count -> reject + disconnect + scored ── */
    {
        const struct msg_dispatch_entry *e = dos_find_entry("addr");
        DOS_CHECK("addr dispatch entry found", e != NULL);
        if (e) {
            struct p2p_node node;
            dos_setup_stack_node(&node);
            struct byte_stream s;
            stream_init(&s, 32);
            stream_write_compact_size(&s, 1001); /* MAX_ADDR_TO_SEND (1000) + 1 */
            bool ret = e->handler(&mp, &node, &s);
            DOS_CHECK("addr oversized: handler returns false", ret == false);
            DOS_CHECK("addr oversized: peer disconnected",
                     node.disconnect == true);
            DOS_CHECK("addr oversized: peer scored PEER_OFFENCE_FLOOD (20)",
                     atomic_load(&node.misbehavior) == 20);
            stream_free(&s);
        }
    }

    /* ── A4. notfound: oversized count -> reject + disconnect + scored ── */
    {
        const struct msg_dispatch_entry *e = dos_find_entry("notfound");
        DOS_CHECK("notfound dispatch entry found", e != NULL);
        if (e) {
            struct p2p_node node;
            dos_setup_stack_node(&node);
            struct byte_stream s;
            stream_init(&s, 32);
            stream_write_compact_size(&s, 50001);
            bool ret = e->handler(&mp, &node, &s);
            DOS_CHECK("notfound oversized: handler returns false",
                     ret == false);
            DOS_CHECK("notfound oversized: peer disconnected",
                     node.disconnect == true);
            DOS_CHECK("notfound oversized: peer scored PEER_OFFENCE_FLOOD (20)",
                     atomic_load(&node.misbehavior) == 20);
            stream_free(&s);
        }
    }

    /* ── B1. inv: truncated mid-item -> clean failure, no crash ── */
    {
        struct p2p_node node;
        dos_setup_stack_node(&node);
        struct byte_stream s;
        stream_init(&s, 32);
        stream_write_compact_size(&s, 2); /* promises 2 inv items (72 bytes)... */
        unsigned char garbage[10];
        memset(garbage, 0xab, sizeof(garbage));
        stream_write_bytes(&s, garbage, sizeof(garbage)); /* ...delivers 10B */
        bool ret = process_inv(&mp, &node, &s);
        DOS_CHECK("inv truncated: handler returns false", ret == false);
        stream_free(&s);
    }

    /* ── B2. getdata: truncated mid-item -> clean failure, no crash ── */
    {
        struct p2p_node node;
        dos_setup_stack_node(&node);
        struct byte_stream s;
        stream_init(&s, 32);
        stream_write_compact_size(&s, 2);
        unsigned char garbage[10];
        memset(garbage, 0xcd, sizeof(garbage));
        stream_write_bytes(&s, garbage, sizeof(garbage));
        bool ret = process_getdata(&mp, &node, &s);
        DOS_CHECK("getdata truncated: handler returns false", ret == false);
        stream_free(&s);
    }

    /* ── C. framing layer: declared size > MAX_PROTOCOL_MESSAGE_LENGTH
     *      is rejected BEFORE any allocation against the recv budget ── */
    {
        unsigned char magic[MESSAGE_START_SIZE] = {0x24, 0xe9, 0x27, 0x64};
        size_t base = net_recv_total_bytes();

        struct net_message m;
        net_message_init(&m, magic);
        struct msg_header hdr;
        /* 3 MiB: above MAX_PROTOCOL_MESSAGE_LENGTH (2 MiB) but below the
         * header-level MAX_SIZE (32 MiB) ceiling, so it reaches the
         * data-phase check inside net_message_read_data. */
        msg_header_init_full(&hdr, magic, "block", 3 * 1024 * 1024);
        int hn = net_message_read_header(&m, (const char *)&hdr,
                                         MSG_HEADER_SIZE);
        DOS_CHECK("oversize framing: header parsed",
                 hn == (int)MSG_HEADER_SIZE && m.in_data);

        unsigned char chunk[16];
        memset(chunk, 0x11, sizeof(chunk));
        int dn = net_message_read_data(&m, (const char *)chunk, sizeof(chunk));
        DOS_CHECK("oversize framing: read_data rejects before allocating",
                 dn < 0);
        DOS_CHECK("oversize framing: no allocation happened",
                 m.recv_data == NULL && m.recv_alloc == 0);
        DOS_CHECK("oversize framing: process-wide recv budget untouched",
                 net_recv_total_bytes() == base);
        net_message_free(&m);
    }

    /* ── D. unknown/garbage command through the real dispatch loop:
     *      silently ignored, connection untouched, honest traffic after
     *      still works on the SAME connection ── */
    {
        unsigned char magic[MESSAGE_START_SIZE] = {0x24, 0xe9, 0x27, 0x64};
        struct net_address addr;
        net_address_init(&addr);
        unsigned char ip4[4] = {198, 51, 100, 23};
        net_addr_set_ipv4(&addr.svc.addr, ip4);
        addr.svc.port = 8033;

        /* A real connected socketpair (not ZCL_INVALID_SOCKET): the
         * dispatched `ping` handler replies with a `pong`, which drives a
         * genuine send() — on an invalid fd that send() would fail and
         * socket_send_data() would legitimately close the connection,
         * which would make the "connection intact" assertion below a
         * false negative rather than a real signal. */
        int sv[2];
        bool have_sv = socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0;
        DOS_CHECK("dispatch: socketpair created", have_sv);
        struct p2p_node *node = p2p_node_create(
            &nm, have_sv ? sv[0] : ZCL_INVALID_SOCKET, &addr,
            "dos-unknown-cmd", true);
        DOS_CHECK("dispatch: node created", node != NULL);
        if (node) {
            /* Bypass "reject any pre-handshake message not in table"
             * (that path is its own, already-correct behaviour, not what
             * this case targets): simulate a post-handshake peer. */
            node->version = PROTOCOL_VERSION;

            /* Build a complete "bogus" command, empty payload, correct
             * checksum (SHA256d of empty payload). */
            struct msg_header hdr;
            msg_header_init_full(&hdr, magic, "bogus", 0);
            unsigned char empty_hash[32];
            hash256((const unsigned char *)"", 0, empty_hash);
            memcpy(&hdr.nChecksum, empty_hash, 4);

            bool recv_ok = p2p_node_receive_bytes(
                node, (const char *)&hdr, MSG_HEADER_SIZE, magic);
            DOS_CHECK("dispatch: unknown-cmd message queued", recv_ok);
            DOS_CHECK("dispatch: message reassembled complete",
                     node->recv_msg_count == 1 &&
                     net_message_complete(&node->recv_msgs[0]));

            bool loop_ok = msg_process_messages(&mp, node);
            DOS_CHECK("dispatch: msg_process_messages completes", loop_ok);
            DOS_CHECK("dispatch: unknown cmd consumed (queue drained)",
                     node->recv_msg_count == 0);
            DOS_CHECK("dispatch: unknown cmd NOT treated as misbehaviour "
                     "(Bitcoin Core parity)",
                     atomic_load(&node->misbehavior) == 0);
            DOS_CHECK("dispatch: connection NOT dropped for unknown cmd",
                     node->disconnect == false);

            /* Honest traffic on the SAME connection afterward: a `ping`
             * must still round-trip through the real dispatch loop. */
            uint64_t nonce = 0x1122334455667788ULL;
            unsigned char nonce_le[8];
            for (int i = 0; i < 8; i++)
                nonce_le[i] = (unsigned char)(nonce >> (8 * i));
            struct msg_header ping_hdr;
            msg_header_init_full(&ping_hdr, magic, "ping", 8);
            unsigned char ping_hash[32];
            hash256(nonce_le, 8, ping_hash);
            memcpy(&ping_hdr.nChecksum, ping_hash, 4);

            unsigned char ping_buf[MSG_HEADER_SIZE + 8];
            memcpy(ping_buf, &ping_hdr, MSG_HEADER_SIZE);
            memcpy(ping_buf + MSG_HEADER_SIZE, nonce_le, 8);

            bool ping_recv_ok = p2p_node_receive_bytes(
                node, (const char *)ping_buf, sizeof(ping_buf), magic);
            DOS_CHECK("dispatch: honest ping after unknown cmd queued",
                     ping_recv_ok && node->recv_msg_count == 1);
            bool ping_loop_ok = msg_process_messages(&mp, node);
            DOS_CHECK("dispatch: honest ping dispatches normally after "
                     "unknown-cmd noise",
                     ping_loop_ok && node->recv_msg_count == 0 &&
                     !node->disconnect &&
                     atomic_load(&node->misbehavior) == 0);

            p2p_node_free(node);
        }
        if (have_sv)
            close(sv[1]);
    }

    /* ── E. duplicate/replayed `headers` batch: idempotent, no double
     *      block-tree entries, no misbehaviour for replaying old data ── */
    struct block_header h1;
    {
        struct block blk;
        block_init(&blk);
        blk.header.nVersion = 4;
        blk.header.hashPrevBlock = gh;
        uint256_set_null(&blk.header.hashMerkleRoot);
        blk.header.hashMerkleRoot.data[0] = 1;
        uint256_set_null(&blk.header.hashFinalSaplingRoot);
        blk.header.nTime = 1700000000u;
        struct arith_uint256 pow_limit;
        uint256_to_arith(&pow_limit, &cp->consensus.powLimit);
        blk.header.nBits = arith_uint256_get_compact(&pow_limit, false);
        bool ok_mine = mine_block_pow(&blk, 1, cp, 0);
        DOS_CHECK("replay setup: regtest header mined", ok_mine);
        if (ok_mine)
            h1 = blk.header;
        block_free(&blk);

        if (ok_mine) {
            struct p2p_node node;
            dos_setup_stack_node(&node);

            struct byte_stream s1;
            stream_init(&s1, 512);
            stream_write_compact_size(&s1, 1);
            block_header_serialize(&h1, &s1);
            stream_write_compact_size(&s1, 0); /* tx count */

            size_t map_before_first = ms.map_block_index.size;
            struct msg_headers_stats st_before;
            msg_headers_get_stats(&st_before);
            bool ret1 = process_headers(&mp, &node, &s1);
            struct msg_headers_stats st_after1;
            msg_headers_get_stats(&st_after1);
            DOS_CHECK("replay: first delivery accepted", ret1 == true);
            DOS_CHECK("replay: first delivery newly_added +1",
                     st_after1.newly_added == st_before.newly_added + 1);
            DOS_CHECK("replay: first delivery not misbehaviour",
                     atomic_load(&node.misbehavior) == 0 && !node.disconnect);
            stream_free(&s1);

            /* Replay: resend the EXACT same header bytes. */
            struct byte_stream s2;
            stream_init(&s2, 512);
            stream_write_compact_size(&s2, 1);
            block_header_serialize(&h1, &s2);
            stream_write_compact_size(&s2, 0);

            bool ret2 = process_headers(&mp, &node, &s2);
            struct msg_headers_stats st_after2;
            msg_headers_get_stats(&st_after2);
            DOS_CHECK("replay: second (duplicate) delivery still returns ok",
                     ret2 == true);
            DOS_CHECK("replay: duplicate counted already-known, "
                     "NOT newly-added",
                     st_after2.newly_added == st_after1.newly_added &&
                     st_after2.already_known == st_after1.already_known + 1);
            DOS_CHECK("replay: no duplicate block-tree entry",
                     ms.map_block_index.size == map_before_first + 1);
            DOS_CHECK("replay: no misbehaviour for replaying old data",
                     atomic_load(&node.misbehavior) == 0 && !node.disconnect);
            stream_free(&s2);
        }
    }

    net_manager_free(&nm);
    sync_set_state(sync0, "net_msg_dos restore");
    main_state_free(&ms);
    SetDataDir("");
    ClearDataDirCache();
    test_rm_rf(dir);
    chain_params_select(CHAIN_MAIN);

    printf("net_msg_dos adversarial tests: %s\n",
           failures ? "FAILED" : "PASSED");
    return failures;
}

/* ── net_framing_dos ──────────────────────────────────────────────────
 * The message FRAMING layer (net_message_read_header / read_data and the
 * p2p_node_receive_bytes reassembler) sees a hostile peer's bytes before any
 * command dispatch. It runs without a net_manager back-pointer, so it cannot
 * score the peer directly: it TAGS node->framing_offence, and the connman
 * receive caller drains + scores it once via p2p_node_score_framing_offence().
 * These cases pin that tag→drain→score contract for the concrete free abuse
 * vectors (bad start-magic, oversize headers, oversize payloads) plus the
 * handshake-level protocol violations scored in msg_version.c. Before this
 * group these paths disconnected (or not) but never moved the per-connection
 * misbehavior score, so a flooder never crossed the ban threshold. */
int test_net_framing_dos(void);
int test_net_framing_dos(void)
{
    int failures = 0;
    printf("\n=== net_framing_dos framing + handshake DoS scoring ===\n");

    chain_params_select(CHAIN_REGTEST);
    const struct chain_params *cp = chain_params_get();
    peer_scoring_init();
    enum sync_state sync0 = sync_get_state();

    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "net_framing_dos", "main");
    SetDataDir(dir);

    struct main_state ms;
    main_state_init(&ms);

    struct net_manager nm;
    net_manager_init(&nm);

    struct msg_processor mp;
    msg_processor_init(&mp, &ms, NULL, NULL, cp, dir, &nm, NULL);

    /* Canonical regtest start-magic (matches dos_setup section C). */
    unsigned char magic[MESSAGE_START_SIZE] = {0x24, 0xe9, 0x27, 0x64};

    /* Build a routable, non-localhost peer address: localhost/whitelisted
     * peers are exempt from scoring via is_trusted_peer(), so the score would
     * never move for a 127.x node. */
    struct net_address paddr;
    net_address_init(&paddr);
    unsigned char ip4[4] = {203, 0, 113, 91};
    net_addr_set_ipv4(&paddr.svc.addr, ip4);
    paddr.svc.port = 8033;

    /* ── a) bad start-magic: header-phase offence => INVALID_HEADER (50) ── */
    {
        struct p2p_node *node = p2p_node_create(&nm, ZCL_INVALID_SOCKET,
                                                &paddr, "framing-badmagic",
                                                true);
        DOS_CHECK("badmagic: node created", node != NULL);
        if (node) {
            unsigned char wrong[MESSAGE_START_SIZE] = {0xde, 0xad, 0xbe, 0xef};
            struct msg_header hdr;
            msg_header_init_full(&hdr, wrong, "ping", 0);
            bool ok = p2p_node_receive_bytes(node, (const char *)&hdr,
                                             MSG_HEADER_SIZE, magic);
            DOS_CHECK("badmagic: frame rejected (returns false)", ok == false);
            DOS_CHECK("badmagic: framing_offence tagged INVALID_HEADER",
                     atomic_load(&node->framing_offence) ==
                         (int)PEER_OFFENCE_INVALID_HEADER);
            p2p_node_score_framing_offence(&nm, node);
            DOS_CHECK("badmagic: peer scored +50 (INVALID_HEADER)",
                     atomic_load(&node->misbehavior) == 50);
            DOS_CHECK("badmagic: tag drained to none after scoring",
                     atomic_load(&node->framing_offence) ==
                         (int)PEER_OFFENCE_NONE);
            p2p_node_free(node);
        }
    }

    /* ── b) oversize header (nMessageSize > MAX_SIZE) => INVALID_HEADER (50)  */
    {
        struct p2p_node *node = p2p_node_create(&nm, ZCL_INVALID_SOCKET,
                                                &paddr, "framing-oversize-hdr",
                                                true);
        DOS_CHECK("oversize-hdr: node created", node != NULL);
        if (node) {
            struct msg_header hdr;
            /* One byte over the 32 MiB header ceiling: rejected in
             * net_message_read_header before in_data is set. */
            msg_header_init_full(&hdr, magic, "block", (uint32_t)MAX_SIZE + 1u);
            bool ok = p2p_node_receive_bytes(node, (const char *)&hdr,
                                             MSG_HEADER_SIZE, magic);
            DOS_CHECK("oversize-hdr: frame rejected", ok == false);
            DOS_CHECK("oversize-hdr: tagged INVALID_HEADER",
                     atomic_load(&node->framing_offence) ==
                         (int)PEER_OFFENCE_INVALID_HEADER);
            p2p_node_score_framing_offence(&nm, node);
            DOS_CHECK("oversize-hdr: peer scored +50 (INVALID_HEADER)",
                     atomic_load(&node->misbehavior) == 50);
            p2p_node_free(node);
        }
    }

    /* ── c) oversize payload (2 MiB < size < MAX_SIZE) => INVALID_PAYLOAD (20) */
    {
        struct p2p_node *node = p2p_node_create(&nm, ZCL_INVALID_SOCKET,
                                                &paddr, "framing-oversize-pay",
                                                true);
        DOS_CHECK("oversize-pay: node created", node != NULL);
        if (node) {
            struct msg_header hdr;
            /* 3 MiB: passes the header ceiling but trips the data-phase
             * MAX_PROTOCOL_MESSAGE_LENGTH (2 MiB) check in read_data. */
            msg_header_init_full(&hdr, magic, "block", 3u * 1024 * 1024);
            unsigned char buf[MSG_HEADER_SIZE + 4];
            memcpy(buf, &hdr, MSG_HEADER_SIZE);
            memset(buf + MSG_HEADER_SIZE, 0x11, 4);
            bool ok = p2p_node_receive_bytes(node, (const char *)buf,
                                             sizeof(buf), magic);
            DOS_CHECK("oversize-pay: frame rejected", ok == false);
            DOS_CHECK("oversize-pay: tagged INVALID_PAYLOAD",
                     atomic_load(&node->framing_offence) ==
                         (int)PEER_OFFENCE_INVALID_PAYLOAD);
            p2p_node_score_framing_offence(&nm, node);
            DOS_CHECK("oversize-pay: peer scored +20 (INVALID_PAYLOAD)",
                     atomic_load(&node->misbehavior) == 20);
            p2p_node_free(node);
        }
    }

    /* ── d) duplicate/replayed version => PROTOCOL_VIOLATION (100) => ban ── */
    {
        struct p2p_node node;
        dos_setup_stack_node(&node);
        node.version = PROTOCOL_VERSION; /* nonzero => this is a duplicate */
        struct byte_stream s;
        stream_init(&s, 8); /* body is never read: duplicate check is first */
        bool ok = process_version(&mp, &node, &s);
        DOS_CHECK("dupversion: handler returns false", ok == false);
        DOS_CHECK("dupversion: scored +100 (PROTOCOL_VIOLATION)",
                 atomic_load(&node.misbehavior) == 100);
        DOS_CHECK("dupversion: crosses ban threshold (should_ban)",
                 peer_scoring_should_ban(&node));
        stream_free(&s);
    }

    /* ── e) repeated framing abuse on ONE connection crosses the ban
     *      threshold: two bad-magic frames (50 + 50) => should_ban ── */
    {
        struct net_address baddr;
        net_address_init(&baddr);
        unsigned char bip[4] = {203, 0, 113, 92};
        net_addr_set_ipv4(&baddr.svc.addr, bip);
        baddr.svc.port = 8033;

        struct p2p_node *node = p2p_node_create(&nm, ZCL_INVALID_SOCKET,
                                                &baddr, "framing-ban", true);
        DOS_CHECK("ban-accrual: node created", node != NULL);
        if (node) {
            unsigned char wrong[MESSAGE_START_SIZE] = {0xde, 0xad, 0xbe, 0xef};
            struct msg_header hdr;
            msg_header_init_full(&hdr, wrong, "ping", 0);

            (void)p2p_node_receive_bytes(node, (const char *)&hdr,
                                         MSG_HEADER_SIZE, magic);
            p2p_node_score_framing_offence(&nm, node);
            DOS_CHECK("ban-accrual: after 1 offence score=50, not yet bannable",
                     atomic_load(&node->misbehavior) == 50 &&
                     !peer_scoring_should_ban(node));

            (void)p2p_node_receive_bytes(node, (const char *)&hdr,
                                         MSG_HEADER_SIZE, magic);
            p2p_node_score_framing_offence(&nm, node);
            DOS_CHECK("ban-accrual: after 2 offences score=100, should_ban",
                     atomic_load(&node->misbehavior) == 100 &&
                     peer_scoring_should_ban(node));
            p2p_node_free(node);
        }
    }

    net_manager_free(&nm);
    sync_set_state(sync0, "net_framing_dos restore");
    main_state_free(&ms);
    SetDataDir("");
    ClearDataDirCache();
    test_rm_rf(dir);
    chain_params_select(CHAIN_MAIN);

    printf("net_framing_dos framing + handshake DoS scoring: %s\n",
           failures ? "FAILED" : "PASSED");
    return failures;
}
