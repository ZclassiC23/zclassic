/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"
#include "config/db_service.h"
#include "config/runtime.h"
#include "controllers/sync_controller.h"
#include "core/serialize.h"
#include "event/event.h"
#include "sapling/incremental_merkle_tree.h"
#include "validation/chainstate.h"
#include "validation/process_block.h"

#include <stdatomic.h>

static _Atomic int g_sapling_persist_events = 0;

static void unclean_shutdown_event_observer(enum event_type type,
                                            uint32_t peer_id,
                                            const void *payload,
                                            uint32_t payload_len,
                                            void *ctx)
{
    (void)type;
    (void)peer_id;
    (void)payload;
    (void)payload_len;
    (void)ctx;
    atomic_fetch_add(&g_sapling_persist_events, 1);
}

static void init_sapling_rebuild_index(struct block_index *bi, int height,
                                       uint8_t tag)
{
    block_index_init(bi);
    bi->nHeight = height;
    memset(bi->hashBlock.data, tag, 32);
    bi->phashBlock = &bi->hashBlock;
}

static void build_one_leaf_sapling_tree(struct incremental_merkle_tree *tree,
                                        uint8_t tag)
{
    struct uint256 cm;

    sapling_tree_init(tree);
    memset(cm.data, tag, 32);
    incremental_tree_append(tree, &cm);
}

static int test_sapling_rebuild_rejects_unverified_checkpoint(void)
{
    int failures = 0;
    const int sapling_height = 476969;
    const int checkpoint_height = sapling_height + 100000;
    char dir[256];
    char dbpath[512];

    printf("sapling rebuild rejects unverified legacy checkpoint... ");

    test_make_tmpdir(dir, sizeof(dir), "sapling_rebuild", "unverified");
    snprintf(dbpath, sizeof(dbpath), "%s/node.db", dir);

    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    bool ok = node_db_open(&ndb, dbpath);

    struct incremental_merkle_tree tree;
    sapling_tree_init(&tree);
    struct byte_stream ts;
    stream_init(&ts, 4096);
    ok = ok && incremental_tree_serialize(&tree, &ts);
    ok = ok && node_db_state_set(&ndb, "sapling_tree", ts.data, ts.size);
    ok = ok && node_db_state_set_int(&ndb, "sapling_tree_rebuild_height",
                                     checkpoint_height);

    struct active_chain chain;
    active_chain_init(&chain);
    struct block_index sapling;
    struct block_index checkpoint;
    init_sapling_rebuild_index(&sapling, sapling_height, 0xa5);
    init_sapling_rebuild_index(&checkpoint, checkpoint_height, 0xc5);
    /* Leave checkpoint.hashFinalSaplingRoot all-zero: the legacy resume
     * checkpoint is therefore unverified and must not be accepted. */
    ok = ok && active_chain_install_tip_slot(&chain, &sapling);
    ok = ok && active_chain_install_tip_slot(&chain, &checkpoint);

    int appended = ok ? sapling_tree_rebuild(&ndb, &chain, dir) : 0;
    ok = ok && appended < 0;

    active_chain_free(&chain);
    stream_free(&ts);
    node_db_close(&ndb);
    test_rm_rf_recursive(dir);

    if (ok) {
        printf("OK\n");
    } else {
        printf("FAIL (appended=%d)\n", appended);
        failures++;
    }
    return failures;
}

static int test_sapling_rebuild_accepts_verified_checkpoint(void)
{
    int failures = 0;
    const int sapling_height = 476969;
    const int checkpoint_height = sapling_height + 100000;
    char dir[256];
    char dbpath[512];

    printf("sapling rebuild accepts verified legacy checkpoint... ");

    test_make_tmpdir(dir, sizeof(dir), "sapling_rebuild", "verified");
    snprintf(dbpath, sizeof(dbpath), "%s/node.db", dir);

    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    bool ok = node_db_open(&ndb, dbpath);

    struct incremental_merkle_tree tree;
    build_one_leaf_sapling_tree(&tree, 0x31);
    struct uint256 root;
    incremental_tree_root(&tree, &root);

    struct byte_stream ts;
    stream_init(&ts, 4096);
    ok = ok && incremental_tree_serialize(&tree, &ts);
    ok = ok && node_db_state_set(&ndb, "sapling_tree", ts.data, ts.size);
    ok = ok && node_db_state_set_int(&ndb, "sapling_tree_rebuild_height",
                                     checkpoint_height);

    struct active_chain chain;
    active_chain_init(&chain);
    struct block_index sapling;
    struct block_index checkpoint;
    init_sapling_rebuild_index(&sapling, sapling_height, 0xa6);
    init_sapling_rebuild_index(&checkpoint, checkpoint_height, 0xc6);
    checkpoint.hashFinalSaplingRoot = root;
    ok = ok && active_chain_install_tip_slot(&chain, &sapling);
    ok = ok && active_chain_install_tip_slot(&chain, &checkpoint);

    int appended = ok ? sapling_tree_rebuild(&ndb, &chain, dir) : -1;
    ok = ok && appended == (int)incremental_tree_size(&tree);

    active_chain_free(&chain);
    stream_free(&ts);
    node_db_close(&ndb);
    test_rm_rf_recursive(dir);

    if (ok) {
        printf("OK\n");
    } else {
        printf("FAIL (appended=%d)\n", appended);
        failures++;
    }
    return failures;
}

static int test_sapling_rebuild_rejects_mismatched_checkpoint(void)
{
    int failures = 0;
    const int sapling_height = 476969;
    const int checkpoint_height = sapling_height + 100000;
    char dir[256];
    char dbpath[512];

    printf("sapling rebuild rejects mismatched legacy checkpoint... ");

    test_make_tmpdir(dir, sizeof(dir), "sapling_rebuild", "mismatch");
    snprintf(dbpath, sizeof(dbpath), "%s/node.db", dir);

    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    bool ok = node_db_open(&ndb, dbpath);

    struct incremental_merkle_tree tree;
    build_one_leaf_sapling_tree(&tree, 0x32);
    struct byte_stream ts;
    stream_init(&ts, 4096);
    ok = ok && incremental_tree_serialize(&tree, &ts);
    ok = ok && node_db_state_set(&ndb, "sapling_tree", ts.data, ts.size);
    ok = ok && node_db_state_set_int(&ndb, "sapling_tree_rebuild_height",
                                     checkpoint_height);

    struct active_chain chain;
    active_chain_init(&chain);
    struct block_index sapling;
    struct block_index checkpoint;
    init_sapling_rebuild_index(&sapling, sapling_height, 0xa7);
    init_sapling_rebuild_index(&checkpoint, checkpoint_height, 0xc7);
    memset(checkpoint.hashFinalSaplingRoot.data, 0x7e, 32);
    ok = ok && active_chain_install_tip_slot(&chain, &sapling);
    ok = ok && active_chain_install_tip_slot(&chain, &checkpoint);

    int appended = ok ? sapling_tree_rebuild(&ndb, &chain, dir) : 0;
    ok = ok && appended < 0;

    active_chain_free(&chain);
    stream_free(&ts);
    node_db_close(&ndb);
    test_rm_rf_recursive(dir);

    if (ok) {
        printf("OK\n");
    } else {
        printf("FAIL (appended=%d)\n", appended);
        failures++;
    }
    return failures;
}

int test_unclean_shutdown_advance(void)
{
    int failures = 0;

    printf("sapling persist escalates after 3 consecutive failures... ");

    event_log_init();
    atomic_store(&g_sapling_persist_events, 0);
    event_observe(EV_SAPLING_PERSIST_FAIL, unclean_shutdown_event_observer,
                  NULL);

    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    bool opened = node_db_open(&ndb, ":memory:");

    struct db_service svc;
    db_service_init(&svc);
    bool attached = db_service_attach(&svc, &ndb);
    bool started = db_service_start(&svc);

    struct app_runtime_context runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.db_service = &svc;
    app_runtime_set_current(&runtime);

    struct incremental_merkle_tree tree;
    sapling_tree_init(&tree);
    {
        struct uint256 cm;
        memset(&cm, 0x42, sizeof(cm));
        incremental_tree_append(&tree, &cm);
    }
    set_sapling_tree_for_flush(&tree);

    process_block_test_fail_next_sapling_persists(3);
    atomic_store(&g_sapling_tree_rebuilding, false);

    bool ok = opened && attached && started;
    bool first = process_block_test_persist_sapling_tree(false);
    bool second = process_block_test_persist_sapling_tree(false);
    bool third = process_block_test_persist_sapling_tree(false);

    ok = ok && first;
    ok = ok && second;
    ok = ok && !third;
    ok = ok && atomic_load(&g_sapling_persist_events) == 3;
    ok = ok && atomic_load(&g_sapling_tree_rebuilding);

    set_sapling_tree_for_flush(NULL);
    process_block_test_fail_next_sapling_persists(0);
    app_runtime_set_current(NULL);
    db_service_stop(&svc);
    node_db_close(&ndb);

    if (ok) {
        printf("OK\n");
    } else {
        printf("FAIL (opened=%d attached=%d started=%d first=%d second=%d third=%d events=%d rebuilding=%d)\n",
               opened, attached, started, first, second, third,
               atomic_load(&g_sapling_persist_events),
               atomic_load(&g_sapling_tree_rebuilding));
        failures++;
    }

    failures += test_sapling_rebuild_rejects_unverified_checkpoint();
    failures += test_sapling_rebuild_accepts_verified_checkpoint();
    failures += test_sapling_rebuild_rejects_mismatched_checkpoint();

    return failures;
}
