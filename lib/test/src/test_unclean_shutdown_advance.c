/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"
#include "config/db_service.h"
#include "config/runtime.h"
#include "controllers/sync_controller.h"
#include "event/event.h"
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

    return failures;
}
