/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "conditions/download_queue_starved.h"
#include "conditions/header_stall_at_height.h"
#include "conditions/local_header_refill_needed.h"
#include "conditions/sync_state_stuck.h"
#include "framework/condition.h"
#include "json/json.h"
#include "net/download.h"
#include "net/protocol.h"
#include "platform/clock.h"
#include "services/sync_monitor.h"
#include "validation/chainstate.h"

#include <stdatomic.h>
#include <string.h>

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
    sync_monitor_init();
    sync_monitor_set_context(cm, dm, ms);
    condition_engine_set_main_state(ms);
}

static void cleanup_sync_watchdog(void)
{
    condition_engine_reset_for_testing();
    sync_monitor_set_context(NULL, NULL, NULL);
    clock_reset_default();
}

static struct block_index *insert_watchdog_block(struct main_state *ms,
                                                 struct uint256 *hash,
                                                 uint8_t salt,
                                                 int height,
                                                 struct block_index *prev,
                                                 unsigned status)
{
    memset(hash, 0, sizeof(*hash));
    hash->data[0] = salt;
    hash->data[1] = (uint8_t)(height & 0xff);
    hash->data[2] = (uint8_t)((height >> 8) & 0xff);
    struct block_index *bi =
        chainstate_insert_block_index((struct chainstate *)ms, hash);
    if (!bi)
        return NULL;
    bi->nHeight = height;
    bi->pprev = prev;
    bi->nStatus = status;
    bi->nTx = (status & BLOCK_HAVE_DATA) ? 1 : 0;
    bi->nChainTx = prev ? prev->nChainTx + bi->nTx : bi->nTx;
    return bi;
}

static const struct json_value *sync_watchdog_condition_json(
    const struct json_value *conditions,
    const char *name)
{
    if (!conditions || !name)
        return NULL;
    for (size_t i = 0; i < json_size(conditions); i++) {
        const struct json_value *cond = json_at(conditions, i);
        const struct json_value *n = cond ? json_get(cond, "name") : NULL;
        if (n && strcmp(json_get_str(n), name) == 0)
            return cond;
    }
    return NULL;
}

int test_sync_watchdog_conditions(void)
{
    printf("\n=== sync watchdog condition tests ===\n");
    int failures = 0;

    {
        struct fake_clock clock;
        fake_clock_install(&clock, 6000);
        sync_monitor_init();

        struct json_value state;
        json_init(&state);
        bool ok = sync_monitor_dump_state_json(&state, NULL);
        ok = ok && json_get(&state, "last_block_connected_height") != NULL;
        ok = ok && json_get(&state, "last_block_connected_time") != NULL;
        ok = ok && json_get(&state, "tip_advance_age_seconds") != NULL;
        ok = ok && json_get(&state, "download_queued") != NULL;
        ok = ok && json_get(&state, "download_in_flight") != NULL;
        ok = ok && json_get(&state, "download_timed_out") != NULL;
        ok = ok && json_get_int(json_get(
            &state, "last_block_connected_height")) == -1;
        ok = ok && json_get_int(json_get(
            &state, "last_block_connected_time")) == 0;
        ok = ok && json_get_int(json_get(
            &state, "tip_advance_age_seconds")) == -1;
        json_free(&state);

        sync_monitor_on_block_connected(42);
        fake_clock_set(&clock, 6015);
        json_init(&state);
        ok = ok && sync_monitor_dump_state_json(&state, NULL);
        ok = ok && json_get_int(json_get(
            &state, "last_block_connected_height")) == 42;
        ok = ok && json_get_int(json_get(
            &state, "last_block_connected_time")) == 6000;
        ok = ok && json_get_int(json_get(
            &state, "tip_advance_age_seconds")) == 15;
        json_free(&state);

        SYNC_WATCHDOG_CHECK("sync monitor dump exposes tip advance input",
                            ok);
        cleanup_sync_watchdog();
    }

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
        peer.services = NODE_NETWORK;
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
        struct json_value dump;
        json_init(&dump);
        json_set_object(&dump);
        ok = ok && condition_engine_dump_state_json(&dump, NULL);
        const struct json_value *conditions = json_get(&dump, "conditions");
        const struct json_value *cond = sync_watchdog_condition_json(
            conditions, "download_queue_starved");
        const struct json_value *detail = cond ? json_get(cond, "detail")
                                               : NULL;
        ok = ok && detail != NULL;
        ok = ok && strcmp(json_get_str(json_get(detail, "sync_state")),
                          "blocks_download") == 0;
        ok = ok && json_get_int(json_get(detail, "peer_count")) == 1;
        ok = ok && json_get_int(json_get(detail, "detect_age_s")) == 121;
        ok = ok && json_get_int(json_get(
            detail, "requested_at_detect")) == 0;
        ok = ok && json_get_int(json_get(detail, "current_requested")) == 0;
        ok = ok && json_get_int(json_get(
            detail, "last_witness_requested")) == 0;
        ok = ok && !json_get_bool(json_get(
            detail, "witness_request_counter_advanced"));
        ok = ok && json_get(detail, "last_assign_result") != NULL;
        ok = ok && json_get(detail, "remedy_contract") != NULL;
        json_free(&dump);
        SYNC_WATCHDOG_CHECK(
            "download queue starved kicks refill with detail json", ok);
        cleanup_sync_watchdog();
    }

    {
        struct fake_clock clock;
        fake_clock_install(&clock, 3500);
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
        fake_clock_set(&clock, 3621);
        condition_engine_tick();
        ok = ok && download_queue_starved_test_remedy_calls() == 1;
        ok = ok && condition_engine_get_active_count() == 1;

        sync_set_state(SYNC_AT_TIP, "test-at-tip");
        fake_clock_set(&clock, 3622);
        condition_engine_tick();
        ok = ok && condition_engine_get_active_count() == 0;

        SYNC_WATCHDOG_CHECK(
            "download queue starved clears after leaving block download", ok);
        cleanup_sync_watchdog();
    }

    {
        struct fake_clock clock;
        fake_clock_install(&clock, 5000);
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
        for (int i = 1; i <= 5; i++) {
            fake_clock_set(&clock, 5121 + ((int64_t)(i - 1) * 120));
            condition_engine_tick();
        }

        struct condition_runtime_snapshot snap;
        memset(&snap, 0, sizeof(snap));
        ok = ok &&
             condition_engine_get_registered_snapshot(
                 "download_queue_starved", &snap);
        ok = ok && download_queue_starved_test_remedy_calls() == 5;
        ok = ok && snap.max_attempts == 5;
        ok = ok && snap.attempts == 5;
        ok = ok && snap.last_outcome == COND_REMEDY_UNWITNESSED;
        ok = ok && snap.operator_needed_emitted;
        ok = ok && condition_engine_get_unresolved_count() == 1;

        /* Never-give-up contract (changed): at the attempt cap this
         * EXTERNAL-resource fault pages ONCE (operator_needed_emitted above)
         * but RE-ARMS on the cooldown instead of latching forever — a node
         * with no peers / an empty fetch window must resume automatically when
         * the resource returns. After the cooldown the budget resets to 0 and
         * the remedy runs again (a 6th call), so escalation happens without a
         * permanent give-up. Mirrors peer_floor_violated. */
        fake_clock_set(&clock, 5721);
        condition_engine_tick();
        ok = ok && download_queue_starved_test_remedy_calls() == 6;
        memset(&snap, 0, sizeof(snap));
        ok = ok &&
             condition_engine_get_registered_snapshot(
                 "download_queue_starved", &snap);
        ok = ok && snap.attempts < 5;   /* re-armed: budget reset, not latched */
        SYNC_WATCHDOG_CHECK(
            "download queue starved pages then re-arms (never latches)", ok);
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
        p1.services = p2.services = p3.services = NODE_NETWORK;
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
        struct json_value dump;
        json_init(&dump);
        json_set_object(&dump);
        ok = ok && condition_engine_dump_state_json(&dump, NULL);
        const struct json_value *conditions = json_get(&dump, "conditions");
        const struct json_value *cond = sync_watchdog_condition_json(
            conditions, "local_header_refill_needed");
        const struct json_value *detail = cond ? json_get(cond, "detail")
                                               : NULL;
        ok = ok && detail != NULL;
        ok = ok && strcmp(json_get_str(json_get(detail, "sync_state")),
                          "headers_download") == 0;
        ok = ok && json_get_int(json_get(
            detail, "tip_height_at_detect")) == 10;
        ok = ok && json_get_int(json_get(detail, "missing_height")) == 11;
        ok = ok && json_get_int(json_get(detail, "peer_max_at_detect")) == 20;
        ok = ok && json_get_bool(json_get(
            detail, "local_recovery_active"));
        ok = ok && json_get_int(json_get(
            detail, "local_recovery_missing_height")) == 11;
        ok = ok && json_get_int(json_get(detail, "retry_count")) == 1;
        ok = ok && json_get_int(json_get(
            detail, "distinct_peer_count")) == 3;
        ok = ok && strcmp(json_get_str(json_get(
            detail, "local_recovery_mode")), "next-child-missing") == 0;
        ok = ok && json_get(detail, "remedy_contract") != NULL;
        json_free(&dump);
        SYNC_WATCHDOG_CHECK(
            "local header refill rotates peers with detail json", ok);
        cleanup_sync_watchdog();
    }

    {
        struct fake_clock clock;
        fake_clock_install(&clock, 4300);
        struct connman cm;
        struct download_manager dm;
        struct main_state ms;
        reset_sync_watchdog(&cm, &dm, &ms);
        zcl_mutex_destroy(&dm.cs);
        dl_init(&dm);
        sync_monitor_set_context(&cm, &dm, &ms);
        block_map_init(&ms.map_block_index);
        active_chain_init(&ms.chain_active);
        bool ok = true;
        register_local_header_refill_needed();

        struct uint256 active_h, fork_h, best_h;
        struct block_index *tip = insert_watchdog_block(
            &ms, &active_h, 0xA0, 10, NULL,
            BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA);
        struct block_index *fork_tip = insert_watchdog_block(
            &ms, &fork_h, 0xF0, 10, NULL, BLOCK_VALID_TREE);
        struct block_index *best = insert_watchdog_block(
            &ms, &best_h, 0xF1, 11, fork_tip, BLOCK_VALID_TREE);
        ms.pindex_best_header = best;
        ok = ok && tip && fork_tip && best &&
             active_chain_move_window_tip(&ms.chain_active, tip);

        struct p2p_node p1 = {0}, p2 = {0}, p3 = {0};
        p1.id = 1; p1.starting_height = 20; p1.state = PEER_ACTIVE;
        p2.id = 2; p2.starting_height = 20; p2.state = PEER_ACTIVE;
        p3.id = 3; p3.starting_height = 20; p3.state = PEER_ACTIVE;
        p1.services = p2.services = p3.services = NODE_NETWORK;
        struct p2p_node *peers[3] = { &p1, &p2, &p3 };
        cm.manager.nodes = peers;
        cm.manager.num_nodes = 3;
        sync_set_state(SYNC_HEADERS_DOWNLOAD, "setup");
        sync_set_state(SYNC_BLOCKS_DOWNLOAD, "test");

        condition_engine_tick();

        bool queued = local_header_refill_needed_test_remedy_calls() == 1 &&
            sync_get_state() == SYNC_BLOCKS_DOWNLOAD &&
            dm.queue_len == 1 &&
            dm.queue_heights && dm.queue_heights[0] == 10 &&
            dm.queue && uint256_eq(&dm.queue[0], fork_tip->phashBlock);
        struct watchdog_stats wd;
        sync_monitor_get_stats(&wd);
        bool recorded = wd.last_recovery == WATCHDOG_BODY_FRONTIER_MISSING &&
            wd.last_recovery_peer_height == 10;
        ok = ok && queued && recorded;
        SYNC_WATCHDOG_CHECK("local header refill queues fork body hash",
                            queued);
        SYNC_WATCHDOG_CHECK("local header refill records fork body recovery",
                            recorded);
        SYNC_WATCHDOG_CHECK("local header refill queues same-height fork body",
                            ok);
        dl_free(&dm);
        zcl_mutex_destroy(&dm.cs);
        main_state_free(&ms);
        cleanup_sync_watchdog();
    }

    {
        struct fake_clock clock;
        fake_clock_install(&clock, 4500);
        struct connman cm;
        struct download_manager dm;
        struct main_state ms;
        reset_sync_watchdog(&cm, &dm, &ms);
        bool ok = true;
        register_local_header_refill_needed();

        struct block_index tip = {0};
        tip.nHeight = 10;
        ok = ok && active_chain_move_window_tip(&ms.chain_active, &tip);
        struct p2p_node inbound = {0};
        inbound.id = 7;
        inbound.inbound = true;
        inbound.starting_height = 20;
        inbound.state = PEER_ACTIVE;
        inbound.services = NODE_NETWORK;
        inbound.last_getheaders_time = 4499;
        struct p2p_node *peers[1] = { &inbound };
        cm.manager.nodes = peers;
        cm.manager.num_nodes = 1;
        sync_set_state(SYNC_HEADERS_DOWNLOAD, "setup");
        sync_set_state(SYNC_BLOCKS_DOWNLOAD, "test");

        condition_engine_tick();
        ok = ok && local_header_refill_needed_test_remedy_calls() == 1;
        ok = ok && inbound.state == PEER_ACTIVE;
        ok = ok && inbound.last_getheaders_time == 4499;
        ok = ok && sync_get_state() == SYNC_BLOCKS_DOWNLOAD;
        struct watchdog_local_recovery_stats lr;
        sync_monitor_get_local_recovery_stats(&lr);
        ok = ok && lr.active && lr.missing_height == 11;
        ok = ok && lr.retry_count == 1;
        ok = ok && lr.distinct_peer_count == 0;
        ok = ok && !lr.retries_exhausted;
        ok = ok && lr.mirror_repair_gated;
        SYNC_WATCHDOG_CHECK("local header refill waits for outbound peer", ok);
        cleanup_sync_watchdog();
    }

    {
        struct fake_clock clock;
        fake_clock_install(&clock, 4700);
        struct connman cm;
        struct download_manager dm;
        struct main_state ms;
        reset_sync_watchdog(&cm, &dm, &ms);
        bool ok = true;
        register_local_header_refill_needed();

        struct block_index tip = {0};
        tip.nHeight = 10;
        ok = ok && active_chain_move_window_tip(&ms.chain_active, &tip);
        struct p2p_node peer = {0};
        peer.id = 1;
        peer.starting_height = 20;
        peer.state = PEER_ACTIVE;
        peer.services = NODE_NETWORK;
        peer.last_getheaders_time = 4699;
        struct p2p_node *peers[1] = { &peer };
        cm.manager.nodes = peers;
        cm.manager.num_nodes = 1;
        sync_set_state(SYNC_HEADERS_DOWNLOAD, "setup");
        sync_set_state(SYNC_BLOCKS_DOWNLOAD, "test");

        condition_engine_tick();
        ok = ok && local_header_refill_needed_test_remedy_calls() == 1;
        ok = ok && condition_engine_get_active_count() == 1;

        struct block_index advanced = {0};
        advanced.nHeight = 11;
        peer.starting_height = 11;
        ok = ok && active_chain_move_window_tip(&ms.chain_active, &advanced);
        condition_engine_tick();
        ok = ok && condition_engine_get_active_count() == 0;
        SYNC_WATCHDOG_CHECK(
            "local header refill clears after active tip advances", ok);
        cleanup_sync_watchdog();
    }

    cleanup_sync_watchdog();
    return failures;
}
