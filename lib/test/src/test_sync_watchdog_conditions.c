/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "conditions/download_queue_starved.h"
#include "conditions/header_stall_at_height.h"
#include "conditions/local_header_refill_needed.h"
#include "conditions/sync_state_stuck.h"
#include "framework/condition.h"
#include "net/download.h"
#include "platform/clock.h"
#include "services/sync_monitor.h"

#include <stdatomic.h>

#define SYNC_WATCHDOG_CHECK(name, expr) do { \
    printf("sync_watchdog_conditions: %s... ", (name)); \
    if (expr) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

struct fake_clock {
    _Atomic int64_t wall_ms;
};

static int64_t fake_now_mono(void *self)
{
    (void)self;
    return 1;
}

static int64_t fake_now_wall(void *self)
{
    struct fake_clock *c = (struct fake_clock *)self;
    return atomic_load(&c->wall_ms);
}

static void fake_clock_install(struct fake_clock *c, int64_t unix_s)
{
    atomic_store(&c->wall_ms, unix_s * 1000);
    static clock_iface_t iface;
    iface.now_monotonic_ns = fake_now_mono;
    iface.now_wall_ms = fake_now_wall;
    iface.self = c;
    clock_set_default(&iface);
}

static void fake_clock_set(struct fake_clock *c, int64_t unix_s)
{
    atomic_store(&c->wall_ms, unix_s * 1000);
}

static void reset_sync_watchdog(struct connman *cm,
                                struct download_manager *dm,
                                struct main_state *ms)
{
    condition_engine_reset_for_testing();
    header_stall_at_height_test_reset();
    sync_state_stuck_test_reset();
    download_queue_starved_test_reset();
    local_header_refill_needed_test_reset();
    memset(cm, 0, sizeof(*cm));
    memset(dm, 0, sizeof(*dm));
    memset(ms, 0, sizeof(*ms));
    zcl_mutex_init(&cm->manager.cs_nodes);
    zcl_mutex_init(&dm->cs);
    zcl_mutex_init(&ms->cs_main);
    sync_monitor_set_context(cm, dm, ms);
    condition_engine_set_main_state(ms);
    sync_monitor_init();
}

static void cleanup_sync_watchdog(void)
{
    condition_engine_reset_for_testing();
    sync_monitor_set_context(NULL, NULL, NULL);
    clock_reset_default();
}

int test_sync_watchdog_conditions(void)
{
    printf("\n=== sync watchdog condition tests ===\n");
    int failures = 0;

    {
        struct fake_clock clock;
        fake_clock_install(&clock, 1000);
        struct connman cm;
        struct download_manager dm;
        struct main_state ms;
        reset_sync_watchdog(&cm, &dm, &ms);
        bool ok = true;
        register_header_stall_at_height();

        struct block_index header = {0};
        header.nHeight = 2000;
        ms.pindex_best_header = &header;
        struct p2p_node peer = {0};
        peer.id = 1;
        peer.starting_height = 2600;
        peer.state = PEER_ACTIVE;
        peer.last_getheaders_time = 999;
        peer.getheaders_stale_count = 3;
        struct p2p_node *peers[1] = { &peer };
        cm.manager.nodes = peers;
        cm.manager.num_nodes = 1;
        sync_set_state(SYNC_HEADERS_DOWNLOAD, "test");

        condition_engine_tick();
        fake_clock_set(&clock, 1301);
        condition_engine_tick();
        ok = ok && header_stall_at_height_test_remedy_calls() == 1;
        ok = ok && peer.last_getheaders_time == 0;
        ok = ok && peer.getheaders_stale_count == 0;
        header.nHeight = 2001;
        fake_clock_set(&clock, 1302);
        condition_engine_tick();
        ok = ok && condition_engine_get_active_count() == 0;
        SYNC_WATCHDOG_CHECK("header stall kicks header fetch", ok);
        cleanup_sync_watchdog();
    }

    {
        struct fake_clock clock;
        fake_clock_install(&clock, 2000);
        struct connman cm;
        struct download_manager dm;
        struct main_state ms;
        reset_sync_watchdog(&cm, &dm, &ms);
        bool ok = true;
        register_sync_state_stuck();
        /* Honest witness (Law 7) requires the tip to ACTUALLY advance, not
         * just the FSM to flip. Stand the tip at height 100 before detect. */
        struct block_index tip = {0};
        tip.nHeight = 100;
        ok = ok && active_chain_move_window_tip(&ms.chain_active, &tip);
        sync_set_state(SYNC_FINDING_PEERS, "test");
        sync_state_test_set_entered_unix(2000);
        fake_clock_set(&clock, 2601);

        condition_engine_tick();
        ok = ok && sync_state_stuck_test_remedy_calls() == 1;
        ok = ok && sync_get_state() == SYNC_HEADERS_DOWNLOAD;
        /* The kicked FSM made real forward progress: the tip advanced. Only
         * then may the witness honestly deactivate the condition. */
        struct block_index next = {0};
        next.nHeight = 101;
        ok = ok && active_chain_move_window_tip(&ms.chain_active, &next);
        condition_engine_tick();
        ok = ok && condition_engine_get_active_count() == 0;
        SYNC_WATCHDOG_CHECK("sync state stuck kicks FSM", ok);
        cleanup_sync_watchdog();
    }

    {
        struct fake_clock clock;
        fake_clock_install(&clock, 3000);
        struct connman cm;
        struct download_manager dm;
        struct main_state ms;
        reset_sync_watchdog(&cm, &dm, &ms);
        bool ok = true;
        register_download_queue_starved();

        struct p2p_node peer = {0};
        struct p2p_node *peers[1] = { &peer };
        cm.manager.nodes = peers;
        cm.manager.num_nodes = 1;
        sync_set_state(SYNC_HEADERS_DOWNLOAD, "setup");
        sync_set_state(SYNC_BLOCKS_DOWNLOAD, "test");
        condition_engine_tick();
        fake_clock_set(&clock, 3121);
        condition_engine_tick();
        ok = ok && download_queue_starved_test_remedy_calls() == 1;
        SYNC_WATCHDOG_CHECK("download queue starved kicks refill", ok);
        cleanup_sync_watchdog();
    }

    {
        struct fake_clock clock;
        fake_clock_install(&clock, 4000);
        struct connman cm;
        struct download_manager dm;
        struct main_state ms;
        reset_sync_watchdog(&cm, &dm, &ms);
        bool ok = true;
        register_local_header_refill_needed();

        struct block_index tip = {0};
        tip.nHeight = 10;
        ok = ok && active_chain_move_window_tip(&ms.chain_active, &tip);
        struct p2p_node p1 = {0}, p2 = {0}, p3 = {0};
        p1.id = 1; p1.starting_height = 20; p1.state = PEER_ACTIVE;
        p2.id = 2; p2.starting_height = 20; p2.state = PEER_ACTIVE;
        p3.id = 3; p3.starting_height = 20; p3.state = PEER_ACTIVE;
        p1.last_getheaders_time = p2.last_getheaders_time =
            p3.last_getheaders_time = 3999;
        struct p2p_node *peers[3] = { &p1, &p2, &p3 };
        cm.manager.nodes = peers;
        cm.manager.num_nodes = 3;
        sync_set_state(SYNC_HEADERS_DOWNLOAD, "setup");
        sync_set_state(SYNC_BLOCKS_DOWNLOAD, "test");

        condition_engine_tick();
        ok = ok && local_header_refill_needed_test_remedy_calls() == 1;
        ok = ok && p1.state == PEER_SYNCING_HEADERS;
        ok = ok && p2.state == PEER_SYNCING_HEADERS;
        ok = ok && p3.state == PEER_SYNCING_HEADERS;
        ok = ok && p1.last_getheaders_time == 0;
        struct watchdog_local_recovery_stats lr;
        sync_monitor_get_local_recovery_stats(&lr);
        ok = ok && lr.active && lr.missing_height == 11;
        SYNC_WATCHDOG_CHECK("local header refill rotates peers", ok);
        cleanup_sync_watchdog();
    }

    cleanup_sync_watchdog();
    return failures;
}
