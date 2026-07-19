/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Regression: addrman shutdown-ordering race.
 *
 * LIVE CRASH (2026-07-19, serve1): during a graceful shutdown, AFTER
 * "[shutdown] connman stopped" was logged, a detached message-cycle thread was
 * still processing a P2P `addr` message:
 *
 *   connman_run_message_cycle -> msg_process_messages -> mp_handle_addr
 *     -> addrman_add -> (find_addr: "bad args") -> SIGSEGV at addrman_add+0x79f
 *
 * Root cause: connman_join()'s bounded timed_join() on the message thread timed
 * out and DETACHED the still-running thread, then connman_free() -> net_manager_free()
 * -> addrman_free() nulled am->entries and destroyed am->cs out from under it.
 * find_addr() returned NULL (its guard fired) but addrman_add() kept going into
 * create_entry() and dereferenced the freed am->entries.
 *
 * The fix is defense in depth:
 *  1. ORDERING — connman_join() records message_thread_detached; connman_free()
 *     defers (leaks) net_manager/addrman teardown when the thread is still live,
 *     so the detached thread never touches freed state.
 *  2. FAIL-CLOSED — addrman_add() guards the SAME condition find_addr() does
 *     (torn-down/invalid addrman) BEFORE locking, returning false rather than
 *     dereferencing freed entries or locking a destroyed mutex.
 *
 * These tests assert both layers hold and neither path crashes. */

#include "platform/time_compat.h"
#include "test/test_helpers.h"
#include "net/addrman.h"
#include "net/connman.h"
#include "net/net.h"
#include "net/netaddr.h"
#include "net/protocol.h"
#include "net/msgprocessor.h"
#include "core/serialize.h"
#include "util/timedata.h"
#include <string.h>
#include <stdio.h>

/* mp_handle_addr is declared only in the msgprocessor src-internal header;
 * declare it here (non-static) to drive the exact crash path from the test. */
extern bool mp_handle_addr(struct msg_processor *mp, struct p2p_node *node,
                           struct byte_stream *s);

/* Build a routable IPv4-mapped address (::ffff:a.b.c.d) using a public range. */
static struct net_address make_pub_addr(uint8_t a, uint8_t b, uint8_t c,
                                        uint8_t d, uint16_t port)
{
    struct net_address addr;
    memset(&addr, 0, sizeof(addr));
    addr.svc.addr.ip[10] = 0xff;
    addr.svc.addr.ip[11] = 0xff;
    addr.svc.addr.ip[12] = a;
    addr.svc.addr.ip[13] = b;
    addr.svc.addr.ip[14] = c;
    addr.svc.addr.ip[15] = d ? d : 1;
    addr.svc.port = port;
    addr.nTime = (uint32_t)platform_time_wall_time_t();
    addr.nServices = 1;
    return addr;
}

/* Layer 2: addrman_add() fails closed on a torn-down / NULL addrman. */
static int test_addrman_add_failclosed_on_teardown(void)
{
    int failures = 0;
    TEST("addrman_shutdown_race: addrman_add fails closed on freed/NULL addrman") {
        struct net_address addr = make_pub_addr(52, 10, 20, 30, 8033);
        struct net_addr src; net_addr_init(&src);

        /* NULL manager — must not deref, must return false. */
        ASSERT(addrman_add(NULL, &addr, &src, 0) == false);

        /* Freed manager: addrman_free() nulls entries and destroys cs — exactly
         * the post-teardown state the detached thread observed live. The guard
         * must return BEFORE touching entries or locking the destroyed mutex. */
        struct addr_man am;
        addrman_init(&am);
        addrman_free(&am);
        ASSERT(am.entries == NULL);            /* sanity: torn down */
        ASSERT(addrman_add(&am, &addr, &src, 0) == false);  /* no crash, closed */

        PASS();
    } _test_next:;
    return failures;
}

/* Layer 2 (integration): an addr message driven through mp_handle_addr against a
 * torn-down addrman must not crash — this is the literal crashing call chain. */
static int test_mp_handle_addr_survives_torndown_addrman(void)
{
    int failures = 0;
    TEST("addrman_shutdown_race: mp_handle_addr survives a torn-down addrman") {
        /* net_manager whose addrman has been freed (entries == NULL). */
        struct net_manager nm;
        memset(&nm, 0, sizeof(nm));
        addrman_init(&nm.addrman);
        addrman_free(&nm.addrman);
        ASSERT(nm.addrman.entries == NULL);

        struct msg_processor mp;
        memset(&mp, 0, sizeof(mp));
        mp.net_mgr = &nm;

        struct p2p_node node;
        memset(&node, 0, sizeof(node));
        snprintf(node.addr_name, sizeof(node.addr_name), "torn-peer");
        /* Give the observing peer a routable source IP. */
        struct net_address peer = make_pub_addr(51, 40, 50, 60, 8033);
        node.addr = peer;

        /* One routable address in the addr message body. */
        struct net_address gossip = make_pub_addr(52, 70, 80, 90, 8033);
        struct byte_stream s;
        stream_init(&s, 64);
        ASSERT(stream_write_compact_size(&s, 1));
        ASSERT(net_address_serialize(&gossip, &s, true));

        /* Drive the exact crashing path. It must return (true) without a
         * SIGSEGV; addrman_add's fail-closed guard swallows the write. */
        bool ok = mp_handle_addr(&mp, &node, &s);
        ASSERT(ok == true);

        stream_free(&s);
        PASS();
    } _test_next:;
    return failures;
}

/* Layer 1: connman_free() defers net_manager/addrman teardown when the message
 * thread was detached (still running), so the detached thread never dereferences
 * freed state. Assert the addrman is intentionally left intact (leaked). */
static int test_connman_free_defers_teardown_when_detached(void)
{
    int failures = 0;
    TEST("addrman_shutdown_race: connman_free defers addrman teardown when message thread detached") {
        struct connman cm;
        memset(&cm, 0, sizeof(cm));
        addrman_init(&cm.manager.addrman);   /* allocates entries */
        ASSERT(cm.manager.addrman.entries != NULL);

        cm.started = false;                  /* skip connman_stop */
        cm.datadir = NULL;                   /* connman_save_addrman early-returns */
        cm.message_thread_detached = true;   /* the join-timeout condition */

        /* Must NOT free the addrman — the (simulated) detached thread would
         * dereference it. entries stays live. */
        connman_free(&cm);
        ASSERT(cm.manager.addrman.entries != NULL);

        /* Cleanup the deliberately-leaked state ourselves (no live thread here). */
        addrman_free(&cm.manager.addrman);

        /* Control: NOT detached -> normal teardown frees the addrman. */
        struct connman cm2;
        memset(&cm2, 0, sizeof(cm2));
        net_manager_init(&cm2.manager);      /* full init so destroy is valid */
        cm2.started = false;
        cm2.datadir = NULL;
        cm2.message_thread_detached = false;
        connman_free(&cm2);
        ASSERT(cm2.manager.addrman.entries == NULL);  /* freed on the clean path */

        PASS();
    } _test_next:;
    return failures;
}

int test_addrman_shutdown_race(void)
{
    int failures = 0;
    failures += test_addrman_add_failclosed_on_teardown();
    failures += test_mp_handle_addr_survives_torndown_addrman();
    failures += test_connman_free_defers_teardown_when_detached();
    return failures;
}
