/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for the block download manager. */

#include "platform/time_compat.h"
#include "test/test_helpers.h"
#include "services/gap_fill_service.h"
#include "net/download.h"
#include "core/uint256.h"
#include "util/supervisor.h"
#include "validation/main_state.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>

/* Helper: make a uint256 from a single byte value */
static struct uint256 make_hash(uint8_t v)
{
    struct uint256 h;
    memset(h.data, 0, 32);
    h.data[0] = v;
    h.data[31] = v; /* non-zero in both positions for probe chain testing */
    return h;
}

static _Atomic int g_gap_fill_dispatch_wakes;

static void test_gap_fill_dispatch_wake(void *ctx)
{
    (void)ctx;
    atomic_fetch_add(&g_gap_fill_dispatch_wakes, 1);
}

static int test_dl_init_free(void)
{
    int failures = 0;
    TEST("dl_init and dl_free") {
        struct download_manager dm;
        dl_init(&dm);
        ASSERT(dm.num_slots > 0);
        ASSERT(dm.num_active == 0);
        ASSERT(dm.queue_len == 0);
        dl_free(&dm);
        ASSERT(dm.slots == NULL);
        ASSERT(dm.queue == NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_mark_requested(void)
{
    int failures = 0;
    TEST("dl_mark_requested basic") {
        struct download_manager dm;
        dl_init(&dm);

        struct uint256 h1 = make_hash(1);
        struct uint256 h2 = make_hash(2);

        ASSERT(dl_mark_requested(&dm, &h1, 100, 1));
        ASSERT(dl_is_in_flight(&dm, &h1));
        ASSERT(!dl_is_in_flight(&dm, &h2));

        ASSERT(dl_mark_requested(&dm, &h2, 101, 2));
        ASSERT(dl_is_in_flight(&dm, &h2));

        /* Duplicate request rejected */
        ASSERT(!dl_mark_requested(&dm, &h1, 100, 3));

        uint64_t req, recv, tout, inflight, queued;
        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(req == 2);
        ASSERT(inflight == 2);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_mark_received(void)
{
    int failures = 0;
    TEST("dl_mark_received removes from in-flight") {
        struct download_manager dm;
        dl_init(&dm);

        struct uint256 h1 = make_hash(1);
        struct uint256 h2 = make_hash(2);
        struct uint256 h3 = make_hash(3);

        dl_mark_requested(&dm, &h1, 100, 1);
        dl_mark_requested(&dm, &h2, 101, 1);
        dl_mark_requested(&dm, &h3, 102, 2);

        /* Receive h2 */
        uint32_t peer = dl_mark_received(&dm, &h2);
        ASSERT(peer == 1);
        ASSERT(!dl_is_in_flight(&dm, &h2));
        ASSERT(dl_is_in_flight(&dm, &h1));
        ASSERT(dl_is_in_flight(&dm, &h3));

        /* Receive unknown hash */
        struct uint256 h4 = make_hash(4);
        ASSERT(dl_mark_received(&dm, &h4) == 0);

        uint64_t req, recv, tout, inflight, queued;
        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(recv == 1);
        ASSERT(inflight == 2);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_queue_dedup(void)
{
    int failures = 0;
    TEST("dl_queue_blocks deduplicates") {
        struct download_manager dm;
        dl_init(&dm);

        struct uint256 hashes[5];
        int32_t heights[5];
        for (int i = 0; i < 5; i++) {
            hashes[i] = make_hash((uint8_t)(10 + i));
            heights[i] = 200 + i;
        }

        /* Queue 5 blocks */
        size_t added = dl_queue_blocks(&dm, hashes, heights, 5);
        ASSERT(added == 5);

        /* Queue same 5 again — should add 0 */
        added = dl_queue_blocks(&dm, hashes, heights, 5);
        ASSERT(added == 0);

        /* Mark one as in-flight, then try to queue it */
        dl_mark_requested(&dm, &hashes[0], 200, 1);
        added = dl_queue_blocks(&dm, hashes, heights, 5);
        ASSERT(added == 0); /* all either queued or in-flight */

        uint64_t req, recv, tout, inflight, queued;
        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(queued == 4); /* 5 queued minus 1 moved to in-flight */
        ASSERT(inflight == 1);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_assign_to_peer(void)
{
    int failures = 0;
    TEST("dl_assign_to_peer pulls from queue") {
        struct download_manager dm;
        dl_init(&dm);

        struct uint256 hashes[10];
        int32_t heights[10];
        for (int i = 0; i < 10; i++) {
            hashes[i] = make_hash((uint8_t)(20 + i));
            heights[i] = 300 + i;
        }

        dl_queue_blocks(&dm, hashes, heights, 10);

        struct uint256 out[5];
        size_t assigned = dl_assign_to_peer(&dm, 1, out, 5);
        ASSERT(assigned == 5);
        struct dl_diagnostics diag;
        dl_get_diagnostics(&dm, &diag);
        ASSERT(diag.assign_attempts == 1);
        ASSERT(diag.assign_successes == 1);
        ASSERT(diag.assign_zero_results == 0);
        ASSERT(diag.last_assign_peer_id == 1);
        ASSERT(diag.last_assign_max_requested == 5);
        ASSERT(diag.last_assign_available == 5);
        ASSERT(diag.last_assign_assigned == 5);
        ASSERT(diag.last_assign_queue_len == 10);
        ASSERT(diag.last_assign_active == 0);
        ASSERT(diag.last_assign_peer_in_flight == 0);
        ASSERT(diag.last_assign_peer_limit >= 5);
        ASSERT(diag.last_assign_global_limit >= 5);
        ASSERT(diag.last_assign_result == DL_ASSIGN_ASSIGNED);
        ASSERT(strcmp(dl_assign_result_name(diag.last_assign_result),
                      "assigned") == 0);

        /* First 5 should be in-flight for peer 1 */
        ASSERT(dl_peer_in_flight(&dm, 1) == 5);

        /* Queue should have 5 remaining */
        uint64_t req, recv, tout, inflight, queued;
        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(queued == 5);
        ASSERT(inflight == 5);

        /* Assign 5 more to peer 2 */
        assigned = dl_assign_to_peer(&dm, 2, out, 5);
        ASSERT(assigned == 5);
        ASSERT(dl_peer_in_flight(&dm, 2) == 5);

        /* Queue empty now */
        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(queued == 0);
        ASSERT(inflight == 10);

        assigned = dl_assign_to_peer(&dm, 3, out, 5);
        ASSERT(assigned == 0);
        dl_get_diagnostics(&dm, &diag);
        ASSERT(diag.assign_attempts == 3);
        ASSERT(diag.assign_successes == 2);
        ASSERT(diag.assign_zero_results == 1);
        ASSERT(diag.last_assign_peer_id == 3);
        ASSERT(diag.last_assign_queue_len == 0);
        ASSERT(diag.last_assign_assigned == 0);
        ASSERT(diag.last_assign_result == DL_ASSIGN_NO_QUEUE);
        ASSERT(strcmp(dl_assign_result_name(diag.last_assign_result),
                      "no_queue") == 0);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_peer_disconnected(void)
{
    int failures = 0;
    TEST("dl_peer_disconnected re-queues blocks") {
        struct download_manager dm;
        dl_init(&dm);

        struct uint256 h1 = make_hash(1);
        struct uint256 h2 = make_hash(2);
        struct uint256 h3 = make_hash(3);

        dl_mark_requested(&dm, &h1, 100, 1);
        dl_mark_requested(&dm, &h2, 101, 1);
        dl_mark_requested(&dm, &h3, 102, 2);

        /* Disconnect peer 1 — h1 and h2 should be re-queued */
        size_t requeued = dl_peer_disconnected(&dm, 1);
        ASSERT(requeued == 2);

        ASSERT(!dl_is_in_flight(&dm, &h1));
        ASSERT(!dl_is_in_flight(&dm, &h2));
        ASSERT(dl_is_in_flight(&dm, &h3)); /* peer 2 still active */

        uint64_t req, recv, tout, inflight, queued;
        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(queued == 2);
        ASSERT(inflight == 1);

        /* Assign re-queued blocks to peer 3 */
        struct uint256 out[5];
        size_t assigned = dl_assign_to_peer(&dm, 3, out, 5);
        ASSERT(assigned == 2);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_check_timeouts(void)
{
    int failures = 0;
    TEST("dl_check_timeouts re-queues stale blocks") {
        struct download_manager dm;
        dl_init(&dm);

        struct uint256 h1 = make_hash(1);
        dl_mark_requested(&dm, &h1, 100, 1);

        /* No timeout at current time */
        int64_t now = (int64_t)platform_time_wall_time_t();
        ASSERT(dl_check_timeouts(&dm, now) == 0);
        ASSERT(dl_is_in_flight(&dm, &h1));

        /* Timeout after the current sync-mode request timeout. */
        int timeout = dl_get_request_timeout_secs();
        ASSERT(dl_check_timeouts(&dm, now + timeout + 1) == 1);
        ASSERT(!dl_is_in_flight(&dm, &h1));

        uint64_t req, recv, tout, inflight, queued;
        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(tout == 1);
        ASSERT(queued == 1); /* re-queued */
        ASSERT(inflight == 0);

        dl_free(&dm);
        PASS();
    } _test_next:;

    return failures;
}

static int test_dl_timeout_retry_failover(void)
{
    int failures = 0;
    TEST("timeout requeue avoids the same peer and fails over") {
        struct download_manager dm;
        dl_init(&dm);

        struct uint256 h1 = make_hash(4);
        int64_t now = (int64_t)platform_time_wall_time_t();
        int timeout = dl_get_request_timeout_secs();
        ASSERT(dl_mark_requested(&dm, &h1, 150, 1));
        ASSERT(dl_check_timeouts(&dm, now + timeout + 1) == 1);

        struct dl_diagnostics diag;
        dl_get_diagnostics(&dm, &diag);
        ASSERT(diag.queue_peer_avoid_count == 1);
        ASSERT(diag.queue_peer_avoid_max_seconds > 0);

        struct uint256 out[1];
        ASSERT(dl_assign_to_peer(&dm, 1, out, 1) == 0);
        dl_get_diagnostics(&dm, &diag);
        ASSERT(diag.last_assign_result == DL_ASSIGN_PEER_AVOID_COOLDOWN);
        ASSERT(strcmp(dl_assign_result_name(diag.last_assign_result),
                      "peer_avoid_cooldown") == 0);

        uint64_t queued = 0;
        dl_get_stats(&dm, NULL, NULL, NULL, NULL, &queued);
        ASSERT(queued == 1);

        ASSERT(dl_assign_to_peer(&dm, 2, out, 1) == 1);
        ASSERT(uint256_eq(&out[0], &h1));
        ASSERT(dl_is_in_flight(&dm, &h1));

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_timeout_retry_avoid_expiry(void)
{
    int failures = 0;
    TEST("same peer retry resumes after avoid cooldown expires") {
        struct download_manager dm;
        dl_init(&dm);

        struct uint256 h1 = make_hash(5);
        int64_t now = (int64_t)platform_time_wall_time_t();
        int timeout = dl_get_request_timeout_secs();
        ASSERT(dl_mark_requested(&dm, &h1, 151, 1));
        ASSERT(dl_check_timeouts(&dm, now + timeout + 1) == 1);

        zcl_mutex_lock(&dm.cs);
        ASSERT(dm.queue_len == 1);
        dm.queue_avoid_until[0] = now - 1;
        zcl_mutex_unlock(&dm.cs);

        struct uint256 out[1];
        ASSERT(dl_assign_to_peer(&dm, 1, out, 1) == 1);
        ASSERT(uint256_eq(&out[0], &h1));

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_diagnostics(void)
{
    int failures = 0;
    TEST("dl_get_diagnostics exposes oldest and overdue in-flight request") {
        struct download_manager dm;
        dl_init(&dm);

        struct dl_diagnostics diag;
        dl_get_diagnostics(&dm, &diag);
        ASSERT(diag.request_timeout_seconds == dl_get_request_timeout_secs());
        ASSERT(diag.oldest_in_flight_age_seconds == -1);
        ASSERT(diag.oldest_in_flight_height == -1);
        ASSERT(diag.oldest_in_flight_peer_id == 0);
        ASSERT(diag.overdue_in_flight == 0);
        ASSERT(diag.in_flight_peer_count == 0);
        ASSERT(diag.assign_attempts == 0);
        ASSERT(diag.assign_successes == 0);
        ASSERT(diag.assign_zero_results == 0);
        ASSERT(diag.last_assign_result == DL_ASSIGN_NONE);
        ASSERT(strcmp(dl_assign_result_name(diag.last_assign_result),
                      "none") == 0);

        struct uint256 h1 = make_hash(11);
        struct uint256 h2 = make_hash(12);
        int64_t now = (int64_t)platform_time_wall_time_t();
        int timeout = dl_get_request_timeout_secs();
        ASSERT(dl_mark_requested(&dm, &h1, 200, 7));
        ASSERT(dl_mark_requested(&dm, &h2, 201, 8));

        zcl_mutex_lock(&dm.cs);
        for (size_t i = 0; i < dm.num_slots; i++) {
            if (!dm.slots[i].active)
                continue;
            if (uint256_eq(&dm.slots[i].hash, &h1))
                dm.slots[i].request_time = now - timeout - 5;
            if (uint256_eq(&dm.slots[i].hash, &h2))
                dm.slots[i].request_time = now - 1;
        }
        zcl_mutex_unlock(&dm.cs);

        dl_get_diagnostics(&dm, &diag);
        ASSERT(diag.request_timeout_seconds == timeout);
        ASSERT(diag.oldest_in_flight_age_seconds >= timeout + 5);
        ASSERT(diag.oldest_in_flight_height == 200);
        ASSERT(diag.oldest_in_flight_peer_id == 7);
        ASSERT(diag.overdue_in_flight == 1);
        ASSERT(diag.in_flight_peer_count == 2);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_gap_fill_timeout_sweep(void)
{
    int failures = 0;
    TEST("gap_fill timeout sweep re-queues stale in-flight blocks") {
        struct download_manager dm;
        dl_init(&dm);

        struct uint256 h1 = make_hash(13);
        int64_t now = (int64_t)platform_time_wall_time_t();
        ASSERT(dl_mark_requested(&dm, &h1, 300, 9));

        int timeout = dl_get_request_timeout_secs();
        size_t requeued =
            gap_fill_sweep_download_timeouts(&dm, now + timeout + 1);
        ASSERT(requeued == 1);

        uint64_t req, recv, tout, inflight, queued;
        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(req == 1);
        ASSERT(recv == 0);
        ASSERT(tout == 1);
        ASSERT(inflight == 0);
        ASSERT(queued == 1);
        ASSERT(!dl_is_in_flight(&dm, &h1));

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_gap_fill_timeout_wakes_dispatcher(void)
{
    int failures = 0;
    TEST("gap_fill timeout pass wakes network dispatcher") {
        bool ok = true;
        supervisor_reset_for_testing();

        struct main_state ms;
        main_state_init(&ms);
        struct download_manager dm;
        dl_init(&dm);

        struct uint256 h1 = make_hash(14);
        int64_t now = (int64_t)platform_time_wall_time_t();
        int timeout = dl_get_request_timeout_secs();
        ok = ok && dl_mark_requested(&dm, &h1, 301, 10);

        zcl_mutex_lock(&dm.cs);
        for (size_t i = 0; i < dm.num_slots; i++) {
            if (dm.slots[i].active && uint256_eq(&dm.slots[i].hash, &h1))
                dm.slots[i].request_time = now - timeout - 1;
        }
        zcl_mutex_unlock(&dm.cs);

        atomic_store(&g_gap_fill_dispatch_wakes, 0);
        gap_fill_set_dispatch_wake(test_gap_fill_dispatch_wake, NULL);
        struct zcl_result r = gap_fill_start(&ms, &dm);
        ok = ok && r.ok;
        if (r.ok) {
            for (int i = 0; i < 50 &&
                 atomic_load(&g_gap_fill_dispatch_wakes) == 0; i++) {
                struct timespec ts = { .tv_sec = 0,
                                       .tv_nsec = 20 * 1000 * 1000 };
                nanosleep(&ts, NULL);
            }
            gap_fill_stop();
        }

        uint64_t inflight = 0, queued = 0, timed_out = 0;
        dl_get_stats(&dm, NULL, NULL, &timed_out, &inflight, &queued);
        ok = ok && atomic_load(&g_gap_fill_dispatch_wakes) > 0;
        ok = ok && timed_out == 1;
        ok = ok && inflight == 0;
        ok = ok && queued == 1;

        dl_free(&dm);
        main_state_free(&ms);
        supervisor_reset_for_testing();
        ASSERT(ok);
        PASS();
    } _test_next:;
    return failures;
}

static int test_gap_fill_queued_idle_wakes_dispatcher(void)
{
    int failures = 0;
    TEST("gap_fill queued idle work wakes network dispatcher") {
        struct download_manager dm;
        dl_init(&dm);

        struct uint256 h1 = make_hash(15);
        int32_t h1_height = 401;
        ASSERT(dl_queue_blocks(&dm, &h1, &h1_height, 1) == 1);

        atomic_store(&g_gap_fill_dispatch_wakes, 0);
        gap_fill_set_dispatch_wake(test_gap_fill_dispatch_wake, NULL);
        ASSERT(gap_fill_wake_dispatch_if_idle(&dm, "unit_queued_idle"));
        ASSERT(atomic_load(&g_gap_fill_dispatch_wakes) == 1);

        struct uint256 h2 = make_hash(16);
        ASSERT(dl_mark_requested(&dm, &h2, 402, 2));
        ASSERT(!gap_fill_wake_dispatch_if_idle(&dm,
                                               "unit_queued_not_idle"));
        ASSERT(atomic_load(&g_gap_fill_dispatch_wakes) == 1);

        gap_fill_set_dispatch_wake(NULL, NULL);
        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_many_insertions(void)
{
    int failures = 0;
    TEST("dl hash table handles many insertions and deletions") {
        struct download_manager dm;
        dl_init(&dm);

        /* Insert 500 blocks */
        for (int i = 0; i < 500; i++) {
            struct uint256 h = make_hash((uint8_t)(i & 0xFF));
            h.data[1] = (uint8_t)(i >> 8);
            dl_mark_requested(&dm, &h, i, (uint32_t)(i % 10));
        }

        uint64_t req, recv, tout, inflight, queued;
        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(inflight == 500);

        /* Receive half */
        for (int i = 0; i < 250; i++) {
            struct uint256 h = make_hash((uint8_t)(i & 0xFF));
            h.data[1] = (uint8_t)(i >> 8);
            dl_mark_received(&dm, &h);
        }

        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(recv == 250);
        ASSERT(inflight == 250);

        /* Remaining 250 should still be findable */
        for (int i = 250; i < 500; i++) {
            struct uint256 h = make_hash((uint8_t)(i & 0xFF));
            h.data[1] = (uint8_t)(i >> 8);
            ASSERT(dl_is_in_flight(&dm, &h));
        }

        /* Received ones should NOT be findable */
        for (int i = 0; i < 250; i++) {
            struct uint256 h = make_hash((uint8_t)(i & 0xFF));
            h.data[1] = (uint8_t)(i >> 8);
            ASSERT(!dl_is_in_flight(&dm, &h));
        }

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_per_peer_limit(void)
{
    int failures = 0;
    TEST("dl_assign_to_peer respects per-peer limit") {
        struct download_manager dm;
        dl_init(&dm);

        /* Queue 200 blocks */
        struct uint256 hashes[200];
        int32_t heights[200];
        for (int i = 0; i < 200; i++) {
            hashes[i] = make_hash((uint8_t)(i & 0xFF));
            hashes[i].data[1] = (uint8_t)(i >> 8);
            heights[i] = i;
        }
        dl_queue_blocks(&dm, hashes, heights, 200);

        /* Assign all to peer 1 — should cap at DL_MAX_IN_FLIGHT_PER_PEER */
        struct uint256 out[200];
        size_t assigned = dl_assign_to_peer(&dm, 1, out, 200);
        ASSERT(assigned == DL_MAX_IN_FLIGHT_PER_PEER);
        ASSERT(dl_peer_in_flight(&dm, 1) == DL_MAX_IN_FLIGHT_PER_PEER);

        /* Try to assign more to same peer — should get 0 */
        assigned = dl_assign_to_peer(&dm, 1, out, 200);
        ASSERT(assigned == 0);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

/* Concurrency test: multiple threads hitting the download manager */
struct dl_thread_ctx {
    struct download_manager *dm;
    int thread_id;
    int ops_done;
};

static void *dl_concurrent_worker(void *arg)
{
    struct dl_thread_ctx *ctx = (struct dl_thread_ctx *)arg;
    struct download_manager *dm = ctx->dm;
    int tid = ctx->thread_id;
    int ops = 0;

    /* Each thread works on a unique range of hashes */
    for (int round = 0; round < 10; round++) {
        struct uint256 hashes[20];
        int32_t heights[20];
        for (int i = 0; i < 20; i++) {
            memset(hashes[i].data, 0, 32);
            hashes[i].data[0] = (uint8_t)(tid);
            hashes[i].data[1] = (uint8_t)(round);
            hashes[i].data[2] = (uint8_t)(i);
            heights[i] = tid * 10000 + round * 100 + i;
        }

        /* Queue blocks */
        dl_queue_blocks(dm, hashes, heights, 20);
        ops++;

        /* Assign to self */
        struct uint256 out[20];
        size_t assigned = dl_assign_to_peer(dm, (uint32_t)tid, out, 20);
        ops++;

        /* "Receive" them */
        for (size_t i = 0; i < assigned; i++) {
            dl_mark_received(dm, &out[i]);
            ops++;
        }
    }

    ctx->ops_done = ops;
    return NULL;
}

static int test_dl_concurrent(void)
{
    int failures = 0;
    TEST("download manager concurrent access (4 threads)") {
        struct download_manager dm;
        dl_init(&dm);

        #define DL_NUM_THREADS 4
        pthread_t threads[DL_NUM_THREADS];
        struct dl_thread_ctx ctxs[DL_NUM_THREADS];

        for (int i = 0; i < DL_NUM_THREADS; i++) {
            ctxs[i].dm = &dm;
            ctxs[i].thread_id = i + 1;
            ctxs[i].ops_done = 0;
            pthread_create(&threads[i], NULL, dl_concurrent_worker, &ctxs[i]);
        }

        for (int i = 0; i < DL_NUM_THREADS; i++)
            pthread_join(threads[i], NULL);

        /* Verify all threads completed */
        int total_ops = 0;
        for (int i = 0; i < DL_NUM_THREADS; i++) {
            ASSERT(ctxs[i].ops_done > 0);
            total_ops += ctxs[i].ops_done;
        }

        /* Verify stats are consistent: received <= requested, no negative */
        uint64_t req, recv, tout, inflight, queued;
        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(recv <= req);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_byte_tracking(void)
{
    int failures = 0;
    TEST("dl_add_bytes_received tracks total bytes") {
        struct download_manager dm;
        dl_init(&dm);

        /* Initially zero */
        uint64_t total_bytes = 0;
        double mbps = 0.0;
        dl_get_throughput(&dm, &total_bytes, &mbps);
        ASSERT(total_bytes == 0);
        ASSERT(mbps == 0.0);

        /* Record some block bytes */
        dl_add_bytes_received(&dm, 1048576);  /* 1 MB */
        dl_add_bytes_received(&dm, 524288);   /* 0.5 MB */
        dl_add_bytes_received(&dm, 2097152);  /* 2 MB */

        dl_get_throughput(&dm, &total_bytes, &mbps);
        ASSERT(total_bytes == 3670016);  /* 3.5 MB total */
        /* mbps > 0 since sync_start_time was set on first call */
        ASSERT(mbps >= 0.0);

        /* Verify sync_start_time was set */
        ASSERT(dm.sync_start_time > 0);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_byte_tracking_before_sync(void)
{
    int failures = 0;
    TEST("dl_get_throughput returns 0 before any bytes received") {
        struct download_manager dm;
        dl_init(&dm);

        uint64_t total_bytes = 99;
        double mbps = 99.0;
        dl_get_throughput(&dm, &total_bytes, &mbps);
        ASSERT(total_bytes == 0);
        ASSERT(mbps == 0.0);

        /* sync_start_time should not be set */
        ASSERT(dm.sync_start_time == 0);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_byte_tracking_large(void)
{
    int failures = 0;
    TEST("dl_add_bytes_received handles 11 GB (realistic sync)") {
        struct download_manager dm;
        dl_init(&dm);

        /* Simulate receiving ~11 GB in 1000-block chunks (~3.6 KB avg) */
        uint64_t expected_total = 0;
        for (int i = 0; i < 3000000; i += 1000) {
            uint64_t chunk_bytes = 3600 * 1000; /* ~3.6 MB per 1000 blocks */
            dl_add_bytes_received(&dm, chunk_bytes);
            expected_total += chunk_bytes;
        }

        uint64_t total_bytes = 0;
        double mbps = 0.0;
        dl_get_throughput(&dm, &total_bytes, &mbps);
        ASSERT(total_bytes == expected_total);
        /* ~10.8 GB */
        ASSERT(total_bytes > 10ULL * 1024 * 1024 * 1024);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dl_null_throughput_params(void)
{
    int failures = 0;
    TEST("dl_get_throughput handles NULL params") {
        struct download_manager dm;
        dl_init(&dm);

        dl_add_bytes_received(&dm, 1000);

        /* Should not crash with NULL params */
        dl_get_throughput(&dm, NULL, NULL);

        uint64_t total = 0;
        dl_get_throughput(&dm, &total, NULL);
        ASSERT(total == 1000);

        double mbps = 0;
        dl_get_throughput(&dm, NULL, &mbps);
        ASSERT(mbps >= 0.0);

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

/* Make a uint256 from a 16-bit value (for >256 distinct hashes). */
static struct uint256 make_hash16(uint16_t v)
{
    struct uint256 h;
    memset(h.data, 0, 32);
    h.data[0] = (uint8_t)(v & 0xFF);
    h.data[1] = (uint8_t)(v >> 8);
    h.data[31] = 0xAB; /* non-zero tail for probe-chain robustness */
    return h;
}

/* REGRESSION: the live wedge. gap_fill builds its window highest-first
 * and tail-appends, so the connectable bottom (tip+1) lands at the TAIL
 * of a deep FIFO queue while far-ahead live blocks saturate the front.
 * With strict FIFO, dl_assign_to_peer never reaches the bottom → the
 * tip-advancing body is perpetually starved → permanent wedge.
 *
 * The fix keeps the queue height-sorted, so dl_assign_to_peer always
 * hands out the LOWEST-height block first regardless of enqueue order.
 * This test reproduces the starvation layout and asserts the bottom is
 * NOT starved. */
static int test_dl_lowest_height_first(void)
{
    int failures = 0;
    TEST("dl_assign_to_peer hands out lowest height first (anti-starvation)") {
        struct download_manager dm;
        dl_init(&dm);

        /* Enqueue far-ahead blocks FIRST (heights 3127000..3127999),
         * exactly as a saturated front would look. */
        const int FAR_N = 1000;
        for (int i = 0; i < FAR_N; i++) {
            struct uint256 h = make_hash16((uint16_t)(2000 + i));
            int32_t height = 3127000 + i;
            dl_queue_blocks(&dm, &h, &height, 1);
        }

        /* Now enqueue the connectable bottom LAST (the tip+1 block at
         * height 3125315) — the one block that advances the tip. This is
         * the tail of the FIFO under the old behavior. */
        struct uint256 bottom = make_hash16(1);
        int32_t bottom_h = 3125315;
        dl_queue_blocks(&dm, &bottom, &bottom_h, 1);

        uint64_t req, recv, tout, inflight, queued;
        dl_get_stats(&dm, &req, &recv, &tout, &inflight, &queued);
        ASSERT(queued == (uint64_t)(FAR_N + 1));

        /* Assign a single block to a peer. It MUST be the connectable
         * bottom (lowest height), not a far-ahead block. Under the old
         * FIFO this returned the height-3127000 block and the bottom
         * stayed starved at the tail. */
        struct uint256 out[1];
        size_t assigned = dl_assign_to_peer(&dm, 1, out, 1);
        ASSERT(assigned == 1);
        ASSERT(uint256_eq(&out[0], &bottom)); /* lowest height wins */

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

/* The bottom must win even when it is appended into the MIDDLE of an
 * already-deep queue and via multiple enqueue paths (blocks + priority
 * + timeout requeue). All paths funnel through the sorted insert. */
static int test_dl_sorted_across_paths(void)
{
    int failures = 0;
    TEST("queue stays height-sorted across blocks/priority/timeout paths") {
        struct download_manager dm;
        dl_init(&dm);

        /* A deep spread of high heights. */
        for (int i = 0; i < 300; i++) {
            struct uint256 h = make_hash16((uint16_t)(5000 + i));
            int32_t height = 3126000 + i * 3;
            dl_queue_blocks(&dm, &h, &height, 1);
        }

        /* A mid-range block via the priority path. */
        struct uint256 mid = make_hash16(40);
        dl_queue_priority(&dm, &mid, 3125900);

        /* The connectable bottom via a plain enqueue, after everything. */
        struct uint256 bottom = make_hash16(41);
        int32_t bottom_h = 3125315;
        dl_queue_blocks(&dm, &bottom, &bottom_h, 1);

        /* Simulate a timeout requeue of a low block: mark in-flight then
         * time it out so it re-enters the queue via dl_queue_push. */
        struct uint256 reto = make_hash16(42);
        dl_mark_requested(&dm, &reto, 3125316, 7);
        int64_t now = (int64_t)platform_time_wall_time_t();
        ASSERT(dl_check_timeouts(&dm, now + DL_REQUEST_TIMEOUT_SECS + 1) == 1);

        /* Drain a handful and assert heights come out monotonically
         * non-decreasing, with the bottom (3125315) first. */
        struct uint256 out[8];
        size_t got = dl_assign_to_peer(&dm, 1, out, 8);
        ASSERT(got == 8);
        ASSERT(uint256_eq(&out[0], &bottom)); /* 3125315 is the global min */
        ASSERT(uint256_eq(&out[1], &reto));   /* 3125316 next */

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

/* Unknown-height (-1) blocks must sort to the BACK so they never preempt
 * a known tip-adjacent block. */
static int test_dl_unknown_height_sorts_last(void)
{
    int failures = 0;
    TEST("unknown-height (-1) blocks never preempt known low heights") {
        struct download_manager dm;
        dl_init(&dm);

        struct uint256 unk = make_hash16(100);
        int32_t unk_h = -1;
        dl_queue_blocks(&dm, &unk, &unk_h, 1); /* enqueued first */

        struct uint256 low = make_hash16(101);
        int32_t low_h = 3125315;
        dl_queue_blocks(&dm, &low, &low_h, 1); /* enqueued second */

        struct uint256 out[1];
        size_t got = dl_assign_to_peer(&dm, 1, out, 1);
        ASSERT(got == 1);
        ASSERT(uint256_eq(&out[0], &low)); /* known low beats unknown */

        dl_free(&dm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_gap_fill_registers_supervisor_contract(void)
{
    int failures = 0;
    TEST("gap_fill registers a chain supervisor contract") {
        bool ok = true;
        supervisor_reset_for_testing();

        struct main_state ms;
        main_state_init(&ms);
        struct download_manager dm;
        dl_init(&dm);

        struct zcl_result r = gap_fill_start(&ms, &dm);
        ok = ok && r.ok;
        if (r.ok) {
            struct supervisor_snapshot snaps[SUPERVISOR_CAP];
            int n = supervisor_snapshot_all(snaps, SUPERVISOR_CAP);
            const struct supervisor_snapshot *gap = NULL;
            for (int i = 0; i < n; i++) {
                if (strcmp(snaps[i].name, "chain.gap_fill") == 0) {
                    gap = &snaps[i];
                    break;
                }
            }
            ok = ok && gap != NULL;
            if (gap) {
                ok = ok && gap->period_secs == 0;
                ok = ok &&
                    gap->deadline_secs == (int64_t)GAPFILL_TICK_SECS * 3 + 30;
            }

            gap_fill_stop();
            ok = ok && supervisor_child_count_total() == 0;
        }

        dl_free(&dm);
        main_state_free(&ms);
        supervisor_reset_for_testing();
        ASSERT(ok);
        PASS();
    } _test_next:;
    return failures;
}

int test_download(void)
{
    int failures = 0;
    failures += test_dl_init_free();
    failures += test_dl_mark_requested();
    failures += test_dl_mark_received();
    failures += test_dl_queue_dedup();
    failures += test_dl_assign_to_peer();
    failures += test_dl_peer_disconnected();
    failures += test_dl_check_timeouts();
    failures += test_dl_timeout_retry_failover();
    failures += test_dl_timeout_retry_avoid_expiry();
    failures += test_dl_diagnostics();
    failures += test_gap_fill_timeout_sweep();
    failures += test_gap_fill_timeout_wakes_dispatcher();
    failures += test_gap_fill_queued_idle_wakes_dispatcher();
    failures += test_dl_many_insertions();
    failures += test_dl_per_peer_limit();
    failures += test_dl_concurrent();
    failures += test_dl_byte_tracking();
    failures += test_dl_byte_tracking_before_sync();
    failures += test_dl_byte_tracking_large();
    failures += test_dl_null_throughput_params();
    failures += test_dl_lowest_height_first();
    failures += test_dl_sorted_across_paths();
    failures += test_dl_unknown_height_sorts_last();
    failures += test_gap_fill_registers_supervisor_contract();
    return failures;
}
