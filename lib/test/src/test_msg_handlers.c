/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the split message handler files:
 *   msg_version.c, msg_headers.c, msg_blocks.c, msg_tx.c, msg_compact.c
 *
 * These tests exercise the public/testable functions without requiring
 * a full msg_processor or live P2P connection. Coverage:
 *   1. msg_headers_get_stats — NULL safety, initial zeroed state
 *   2. block_already_seen / block_mark_seen / block_clear_seen — dedup ring
 *   3. tx_already_seen / tx_mark_seen — tx dedup ring
 *   4. Dandelion globals — initial state
 */

#include "test/test_helpers.h"
#include "chain/chainparams.h"
#include "core/hash.h"
#include "net/msgprocessor.h"
#include "net/msg_internal.h"
#include "consensus/validation.h"
#include "core/uint256.h"
#include "primitives/block.h"
#include "sync/sync_state.h"
#include "util/safe_alloc.h"
#include "validation/main_state.h"

#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void test_msg_sleep_ms(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void test_msg_sync_to_idle(void)
{
    enum sync_state cur = sync_get_state();
    if (cur == SYNC_IDLE)
        return;
    if (cur == SYNC_AT_TIP) {
        (void)sync_set_state(SYNC_IDLE, "msg_handlers cleanup");
        return;
    }
    if (cur == SYNC_REORG) {
        (void)sync_set_state(SYNC_AT_TIP, "msg_handlers cleanup");
        (void)sync_set_state(SYNC_IDLE, "msg_handlers cleanup");
        return;
    }
    (void)sync_set_state(SYNC_IDLE, "msg_handlers cleanup");
}

static void test_msg_sync_to_blocks_download(void)
{
    test_msg_sync_to_idle();
    if (sync_get_state() != SYNC_IDLE)
        return;
    (void)sync_set_state(SYNC_FINDING_PEERS, "msg_handlers setup");
    (void)sync_set_state(SYNC_HEADERS_DOWNLOAD, "msg_handlers setup");
    (void)sync_set_state(SYNC_BLOCKS_DOWNLOAD, "msg_handlers setup");
}

/* ── msg_headers.c tests ───────────────────────────────────────── */

static int test_headers_stats_null_safe(void)
{
    int failures = 0;
    TEST("msg_handlers: msg_headers_get_stats(NULL) does not crash") {
        msg_headers_get_stats(NULL);
        ASSERT(true);  /* survived NULL arg without crashing */
        PASS();
    } _test_next:;
    return failures;
}

static int test_headers_stats_initial(void)
{
    int failures = 0;
    TEST("msg_handlers: msg_headers_get_stats returns zeroed counters initially") {
        struct msg_headers_stats st;
        memset(&st, 0xFF, sizeof(st));
        msg_headers_get_stats(&st);
        /* Counters start at zero (atomics initialized to 0). */
        ASSERT(st.batches_received == 0);
        ASSERT(st.total_accepted == 0);
        ASSERT(st.total_rejected == 0);
        ASSERT(st.already_known == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* ── msgprocessor.c dedup ring buffer tests ─────────────────────── */

static struct uint256 make_test_hash(uint8_t seed)
{
    struct uint256 h;
    memset(h.data, seed, 32);
    return h;
}

static int test_block_dedup_basic(void)
{
    int failures = 0;
    TEST("msg_handlers: block dedup — unseen hash returns false") {
        struct uint256 h = make_test_hash(0xAA);
        ASSERT(!block_already_seen(&h));
        PASS();
    } _test_next:;
    return failures;
}

static int test_block_dedup_mark_and_check(void)
{
    int failures = 0;
    TEST("msg_handlers: block dedup — mark then check returns true") {
        struct uint256 h = make_test_hash(0xBB);
        block_mark_seen(&h);
        ASSERT(block_already_seen(&h));
        PASS();
    } _test_next:;
    return failures;
}

static int test_block_dedup_clear(void)
{
    int failures = 0;
    TEST("msg_handlers: block dedup — clear removes hash") {
        struct uint256 h = make_test_hash(0xCC);
        block_mark_seen(&h);
        ASSERT(block_already_seen(&h));
        block_clear_seen(&h);
        ASSERT(!block_already_seen(&h));
        PASS();
    } _test_next:;
    return failures;
}

static int test_block_dedup_multiple(void)
{
    int failures = 0;
    TEST("msg_handlers: block dedup — multiple hashes tracked independently") {
        struct uint256 h1 = make_test_hash(0xD1);
        struct uint256 h2 = make_test_hash(0xD2);
        struct uint256 h3 = make_test_hash(0xD3);
        block_mark_seen(&h1);
        block_mark_seen(&h2);
        ASSERT(block_already_seen(&h1));
        ASSERT(block_already_seen(&h2));
        ASSERT(!block_already_seen(&h3));
        PASS();
    } _test_next:;
    return failures;
}

static int test_tx_dedup_basic(void)
{
    int failures = 0;
    TEST("msg_handlers: tx dedup — unseen tx returns false") {
        struct uint256 h = make_test_hash(0xE1);
        ASSERT(!tx_already_seen(&h));
        PASS();
    } _test_next:;
    return failures;
}

static int test_tx_dedup_mark_and_check(void)
{
    int failures = 0;
    TEST("msg_handlers: tx dedup — mark then check returns true") {
        struct uint256 h = make_test_hash(0xE2);
        tx_mark_seen(&h);
        ASSERT(tx_already_seen(&h));
        PASS();
    } _test_next:;
    return failures;
}

/* ── Dandelion state tests ─────────────────────────────────────── */

static int test_dandelion_initial_state(void)
{
    int failures = 0;
    TEST("msg_handlers: dandelion not initialized at start") {
        /* g_dandelion_init should be false before any peer handshake */
        ASSERT(!g_dandelion_init);
        PASS();
    } _test_next:;
    return failures;
}

/* ── msg_blocks_should_mark_seen tests ───────────────────────
 *
 * bug: block_mark_seen was called BEFORE process_new_block.
 * If the block was received + indexed but not activated (e.g.
 * ACTIVATION_SKIP_ALREADY_RUNNING under 6-peer concurrent arrival),
 * it was permanently dedup'd and never retried.
 *
 * mark_seen is gated on "block reached active chain"
 * via msg_blocks_should_mark_seen(). The helper is a pure function
 * so it can be exercised without full P2P plumbing.
 */

static int test_p148_should_mark_seen_rejects_null(void)
{
    int failures = 0;
    TEST("should_mark_seen rejects NULL chain or pindex") {
        struct active_chain ac;
        active_chain_init(&ac);
        struct block_index bi;
        block_index_init(&bi);

        ASSERT(!msg_blocks_should_mark_seen(NULL, &bi));
        ASSERT(!msg_blocks_should_mark_seen(&ac, NULL));
        ASSERT(!msg_blocks_should_mark_seen(NULL, NULL));

        active_chain_free(&ac);
        PASS();
    } _test_next:;
    return failures;
}

static int test_p148_should_mark_seen_rejects_orphan(void)
{
    int failures = 0;
    TEST("should_mark_seen rejects block NOT in active chain") {
        /* Mirrors the bug shape: block was indexed (has a pindex)
         * but activation SKIP'd, so it's not in the active chain.
         * Pre-fix, block_mark_seen was unconditional. Post-fix, we
         * must NOT mark seen — the dedup ring would otherwise hide
         * the block from subsequent arrival + retry. */
        struct active_chain ac;
        active_chain_init(&ac);

        struct block_index tip;
        block_index_init(&tip);
        tip.nHeight = 100;
        active_chain_move_window_tip(&ac, &tip);

        struct block_index orphan;
        block_index_init(&orphan);
        orphan.nHeight = 101; /* indexed above tip, not connected */

        ASSERT(!msg_blocks_should_mark_seen(&ac, &orphan));

        active_chain_free(&ac);
        PASS();
    } _test_next:;
    return failures;
}

static int test_p148_should_mark_seen_accepts_active(void)
{
    int failures = 0;
    TEST("should_mark_seen accepts block that IS in active chain") {
        struct active_chain ac;
        active_chain_init(&ac);

        struct block_index tip;
        block_index_init(&tip);
        tip.nHeight = 42;
        active_chain_move_window_tip(&ac, &tip);

        ASSERT(msg_blocks_should_mark_seen(&ac, &tip));

        active_chain_free(&ac);
        PASS();
    } _test_next:;
    return failures;
}

static int test_block_validation_retryable_classifier(void)
{
    int failures = 0;
    TEST("msg_handlers: reducer-pending block verdict is retryable") {
        struct validation_state st;
        validation_state_init(&st);
        validation_state_invalid(&st, false, REJECT_INVALID,
                                 "block-not-finalized-by-reducer",
                                 "h=7 tf_cursor=6 ua_ok=1");
        ASSERT(msg_block_validation_is_retryable(&st));
        ASSERT(strstr(st.debug_message, "tf_cursor=6") != NULL);

        const char *const intake_reasons[] = {
            "p2p-block-queued-for-reducer",
            "p2p-block-staged-for-reducer",
            "p2p-block-header-missing",
            "header-admit-inbox-full",
            "reducer-body-header-missing",
            "reducer-body-runtime-unwired",
            "reducer-body-write-failed",
            "reducer-body-verify-failed",
            "p2p-block-intake-unavailable",
            "p2p-block-intake-stopped",
            "p2p-block-intake-full",
            "p2p-block-clone-failed",
        };
        for (size_t i = 0; i < sizeof(intake_reasons) /
                               sizeof(intake_reasons[0]); i++) {
            validation_state_init(&st);
            validation_state_error(&st, intake_reasons[i]);
            ASSERT(msg_block_validation_is_retryable(&st));
        }

        validation_state_init(&st);
        validation_state_invalid(&st, false, REJECT_INVALID,
                                 "bad-txns-inputs-missingorspent", NULL);
        ASSERT(!msg_block_validation_is_retryable(&st));

        validation_state_init(&st);
        ASSERT(!msg_block_validation_is_retryable(&st));
        PASS();
    } _test_next:;
    return failures;
}

static bool submit_reducer_pending_block(struct block *block,
                                         struct validation_state *out,
                                         void *ctx)
{
    (void)block;
    int *calls = (int *)ctx;
    if (calls)
        (*calls)++;
    validation_state_invalid(out, false, REJECT_INVALID,
                             "block-not-finalized-by-reducer", NULL);
    return false;
}

static int test_process_block_msg_reducer_pending_stays_retryable(void)
{
    int failures = 0;
    TEST("msg_handlers: reducer-pending process_block_msg does not mark seen") {
        struct block blk;
        block_init(&blk);
        blk.header.nVersion = 4;
        blk.header.nTime = 1700000000u;
        blk.header.nBits = 0x1f00ffffu;
        blk.header.nNonce.data[0] = 7;

        struct uint256 hash;
        block_get_hash(&blk, &hash);
        block_clear_seen(&hash);

        struct byte_stream s;
        stream_init(&s, 256);
        ASSERT(block_serialize(&blk, &s));

        int submit_calls = 0;
        struct msg_processor mp;
        memset(&mp, 0, sizeof(mp));
        mp.block_submit = submit_reducer_pending_block;
        mp.block_submit_ctx = &submit_calls;

        struct p2p_node node;
        memset(&node, 0, sizeof(node));
        node.id = 77;
        snprintf(node.addr_name, sizeof(node.addr_name), "test-peer");

        ASSERT(process_block_msg(&mp, &node, &s));
        ASSERT(submit_calls == 1);
        ASSERT(!block_already_seen(&hash));

        stream_free(&s);
        block_free(&blk);
        PASS();
    } _test_next:;
    return failures;
}

struct async_block_submit_ctx {
    atomic_int entered;
    atomic_int release;
};

static bool submit_async_blocking_pending(struct block *block,
                                          struct validation_state *out,
                                          void *ctx)
{
    (void)block;
    struct async_block_submit_ctx *submit_ctx = ctx;
    atomic_fetch_add_explicit(&submit_ctx->entered, 1,
                              memory_order_relaxed);
    while (!atomic_load_explicit(&submit_ctx->release,
                                 memory_order_acquire)) {
        test_msg_sleep_ms(1);
    }
    validation_state_invalid(out, false, REJECT_INVALID,
                             "block-not-finalized-by-reducer", NULL);
    return false;
}

static int test_process_block_msg_queues_reducer_during_catchup(void)
{
    int failures = 0;
    TEST("msg_handlers: catch-up block intake does not block message thread") {
        test_msg_sync_to_blocks_download();
        ASSERT(sync_get_state() == SYNC_BLOCKS_DOWNLOAD);

        struct block blk;
        block_init(&blk);
        blk.header.nVersion = 4;
        blk.header.nTime = 1700000001u;
        blk.header.nBits = 0x1f00ffffu;
        blk.header.nNonce.data[0] = 9;

        struct uint256 hash;
        block_get_hash(&blk, &hash);
        block_clear_seen(&hash);

        struct byte_stream s;
        stream_init(&s, 256);
        ASSERT(block_serialize(&blk, &s));

        struct main_state ms;
        main_state_init(&ms);
        struct async_block_submit_ctx submit_ctx = {0};
        struct msg_processor mp;
        memset(&mp, 0, sizeof(mp));
        mp.main_state = &ms;
        mp.params = chain_params_get();
        mp.block_submit = submit_async_blocking_pending;
        mp.block_submit_ctx = &submit_ctx;

        struct p2p_node node;
        memset(&node, 0, sizeof(node));
        node.id = 88;
        snprintf(node.addr_name, sizeof(node.addr_name), "test-peer");

        ASSERT(process_block_msg(&mp, &node, &s));
        ASSERT(!atomic_load_explicit(&submit_ctx.release,
                                     memory_order_acquire));

        for (int i = 0; i < 200 &&
             atomic_load_explicit(&submit_ctx.entered,
                                  memory_order_acquire) == 0; i++) {
            test_msg_sleep_ms(1);
        }
        ASSERT(atomic_load_explicit(&submit_ctx.entered,
                                    memory_order_acquire) == 1);

        struct msg_block_intake_stats stats;
        msg_processor_get_block_intake_stats(&mp, &stats);
        ASSERT(stats.running);
        ASSERT(stats.capacity > 0);
        ASSERT(stats.enqueued == 1);
        ASSERT(stats.processed == 0);
        ASSERT(!block_already_seen(&hash));

        atomic_store_explicit(&submit_ctx.release, 1,
                              memory_order_release);
        msg_processor_stop_block_intake(&mp);
        stream_free(&s);
        block_free(&blk);
        main_state_free(&ms);
        test_msg_sync_to_idle();
        PASS();
    } _test_next:;
    return failures;
}

static int test_msg_block_intake_full_stays_retryable(void)
{
    int failures = 0;
    TEST("msg_handlers: full catch-up block intake stays retryable") {
        test_msg_sync_to_blocks_download();
        ASSERT(sync_get_state() == SYNC_BLOCKS_DOWNLOAD);

        struct block blk;
        block_init(&blk);
        blk.header.nVersion = 4;
        blk.header.nTime = 1700000002u;
        blk.header.nBits = 0x1f00ffffu;
        blk.header.nNonce.data[0] = 10;

        struct uint256 hash;
        block_get_hash(&blk, &hash);

        struct main_state ms;
        main_state_init(&ms);
        struct async_block_submit_ctx submit_ctx = {0};
        struct msg_processor mp;
        memset(&mp, 0, sizeof(mp));
        mp.main_state = &ms;
        mp.params = chain_params_get();
        mp.block_submit = submit_async_blocking_pending;
        mp.block_submit_ctx = &submit_ctx;

        bool saw_full = false;
        struct validation_state state;
        for (int i = 0; i < 512 && !saw_full; i++) {
            validation_state_init(&state);
            ASSERT(msg_processor_enqueue_p2p_block(&mp, &blk, &hash,
                                                   89, &state));
            saw_full = strcmp(state.reject_reason,
                              "p2p-block-intake-full") == 0;
        }

        ASSERT(saw_full);
        ASSERT(msg_block_validation_is_retryable(&state));

        struct msg_block_intake_stats stats;
        msg_processor_get_block_intake_stats(&mp, &stats);
        ASSERT(stats.capacity > 0);
        ASSERT(stats.current_depth <= stats.capacity);
        ASSERT(stats.dropped > 0);
        ASSERT(stats.enqueued > 0);

        atomic_store_explicit(&submit_ctx.release, 1,
                              memory_order_release);
        msg_processor_stop_block_intake(&mp);
        block_free(&blk);
        main_state_free(&ms);
        test_msg_sync_to_idle();
        PASS();
    } _test_next:;
    return failures;
}

static void test_init_complete_empty_message(struct net_message *msg,
                                             const char *command)
{
    static const unsigned char msgstart[MESSAGE_START_SIZE] = {
        0x24, 0xe9, 0x27, 0x64
    };
    unsigned char hash[SHA256_OUTPUT_SIZE];
    net_message_init(msg, msgstart);
    msg_header_init_full(&msg->hdr, msgstart, command, 0);
    hash256(NULL, 0, hash);
    memcpy(&msg->hdr.nChecksum, hash, sizeof(msg->hdr.nChecksum));
    msg->in_data = true;
    msg->data_pos = 0;
}

static int test_msg_process_messages_yields_after_bounded_batch(void)
{
    int failures = 0;
    TEST("msg_handlers: inbound processing yields after bounded batch") {
        const size_t total = ZCL_MSG_PROCESS_MAX_PER_CYCLE + 3;
        struct p2p_node node;
        struct msg_processor mp;
        memset(&node, 0, sizeof(node));
        memset(&mp, 0, sizeof(mp));
        node.id = 88;
        node.version = 170011;
        atomic_store(&node.state, PEER_ACTIVE);
        zcl_mutex_init(&node.cs_recv);
        node.recv_msg_cap = total;
        node.recv_msg_count = total;
        node.recv_msgs = zcl_calloc(total, sizeof(*node.recv_msgs),
                                    "test_recv_msgs");
        ASSERT(node.recv_msgs != NULL);
        snprintf(node.addr_name, sizeof(node.addr_name), "test-peer");
        for (size_t i = 0; i < total; i++)
            test_init_complete_empty_message(&node.recv_msgs[i], "noop");

        ASSERT(msg_process_messages(&mp, &node));
        ASSERT(!node.disconnect);
        ASSERT(node.recv_msg_count == total - ZCL_MSG_PROCESS_MAX_PER_CYCLE);

        for (size_t i = 0; i < node.recv_msg_count; i++)
            net_message_free(&node.recv_msgs[i]);
        free(node.recv_msgs);
        zcl_mutex_destroy(&node.cs_recv);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Entry point ───────────────────────────────────────────────── */

int test_msg_handlers(void);

int test_msg_handlers(void)
{
    int failures = 0;

    failures += test_headers_stats_null_safe();
    failures += test_headers_stats_initial();
    failures += test_block_dedup_basic();
    failures += test_block_dedup_mark_and_check();
    failures += test_block_dedup_clear();
    failures += test_block_dedup_multiple();
    failures += test_tx_dedup_basic();
    failures += test_tx_dedup_mark_and_check();
    failures += test_dandelion_initial_state();
    failures += test_p148_should_mark_seen_rejects_null();
    failures += test_p148_should_mark_seen_rejects_orphan();
    failures += test_p148_should_mark_seen_accepts_active();
    failures += test_block_validation_retryable_classifier();
    failures += test_process_block_msg_reducer_pending_stays_retryable();
    failures += test_process_block_msg_queues_reducer_during_catchup();
    failures += test_msg_block_intake_full_stays_retryable();
    failures += test_msg_process_messages_yields_after_bounded_batch();

    return failures;
}
