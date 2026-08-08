/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Truth table for rescanwitnesses' consensus guard. A rebuilt Sapling tree is
 * persistable only when it matches a non-zero header root and every witness root
 * agrees with that tree.
 */

#include "test/test_core.h"
#include "controllers/wallet_rescan_controller_internal.h"
#include "storage/anchor_kv.h"

#include <sqlite3.h>
#include <string.h>

static struct uint256 test_root(uint8_t seed)
{
    struct uint256 r;
    for (size_t i = 0; i < sizeof(r.data); i++)
        r.data[i] = (uint8_t)(seed + i);
    return r;
}

static void check_case(const char *name, bool got, bool want, int *failures)
{
    printf("%s... ", name);
    if (got == want) {
        printf("OK\n");
    } else {
        printf("FAIL\n");
        (*failures)++;
    }
}

int test_rescanwitnesses_diverge_guard(void)
{
    printf("\n=== rescanwitnesses divergence guard ===\n");
    int failures = 0;

    struct uint256 root = test_root(0x11);
    struct uint256 same = root;
    struct uint256 other = test_root(0x22);
    struct uint256 zero = {{0}};

    check_case("matching non-zero root with zero mismatches is valid",
               rescan_result_consensus_valid(&root, &same, 0), true,
               &failures);
    check_case("all-zero header root is invalid",
               rescan_result_consensus_valid(&root, &zero, 0), false,
               &failures);
    check_case("different header root is invalid",
               rescan_result_consensus_valid(&root, &other, 0), false,
               &failures);
    check_case("positive witness mismatch count is invalid",
               rescan_result_consensus_valid(&root, &same, 1), false,
               &failures);
    check_case("negative witness mismatch count is invalid",
               rescan_result_consensus_valid(&root, &same, -1), false,
               &failures);
    check_case("NULL rebuilt root is invalid",
               rescan_result_consensus_valid(NULL, &same, 0), false,
               &failures);
    check_case("NULL header root is invalid",
               rescan_result_consensus_valid(&root, NULL, 0), false,
               &failures);

    sqlite3 *db = NULL;
    struct active_chain chain;
    active_chain_init(&chain);
    struct incremental_merkle_tree anchored;
    sapling_tree_init(&anchored);
    struct uint256 cm = test_root(0x44);
    incremental_tree_append(&anchored, &cm);
    struct uint256 anchor_root;
    incremental_tree_root(&anchored, &anchor_root);
    struct block_index prior;
    block_index_init(&prior);
    prior.nHeight = 500000;
    prior.hashBlock = test_root(0x55);
    prior.phashBlock = &prior.hashBlock;
    prior.hashFinalSaplingRoot = anchor_root;
    prior.nStatus = BLOCK_HAVE_DATA;
    struct db_sapling_note note;
    memset(&note, 0, sizeof(note));
    note.block_height = 500001;
    struct incremental_merkle_tree seeded;
    sapling_tree_init(&seeded);
    int replay_start = -1;
    int seed_height = -1;
    bool seed_ok = sqlite3_open(":memory:", &db) == SQLITE_OK &&
        anchor_kv_ensure_schema(db) && anchor_kv_initialize_history(db, 0) &&
        anchor_kv_add_tree(db, ANCHOR_POOL_SAPLING, &anchored, 499999) &&
        active_chain_install_tip_slot(&chain, &prior) &&
        rescan_seed_before_oldest_note_from_db(
            db, &chain, &note, 1, &seeded, &replay_start, &seed_height);
    struct uint256 seeded_root;
    incremental_tree_root(&seeded, &seeded_root);
    check_case("header-bound prior anchor seeds at the oldest note",
               seed_ok && replay_start == 500001 && seed_height == 500000 &&
               uint256_eq(&seeded_root, &anchor_root), true, &failures);

    prior.hashFinalSaplingRoot = test_root(0x66);
    check_case("missing prior-header anchor refuses the shortcut",
               rescan_seed_before_oldest_note_from_db(
                   db, &chain, &note, 1, &seeded, &replay_start,
                   &seed_height), false, &failures);
    if (db)
        sqlite3_close(db);
    active_chain_free(&chain);

    printf("rescanwitnesses divergence guard: %s (%d failures)\n",
           failures == 0 ? "OK" : "FAIL", failures);
    return failures;
}
