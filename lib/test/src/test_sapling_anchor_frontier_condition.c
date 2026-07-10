/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * sapling_anchor_frontier_condition -- the auto-terminating cure for the empty
 * Sapling anchor-frontier stall.
 *
 * Proves, hermetically (in-memory anchor store; Pedersen hashing over synthetic
 * commitments — no ~/.zcash-params, no live chain):
 *   (a) an empty sapling_anchors table with a nonzero adoption cursor classifies
 *       as the seed-curable birth defect (detect signal);
 *   (b) seeding a header-verified frontier makes anchor_kv_latest_tree FOUND and
 *       lets the reducer's anchor fold proceed on a block that previously failed
 *       closed with shielded_anchor_history_gap;
 *   (c) a root-MISMATCH seed writes NOTHING and the fold stays failed closed
 *       (fail-closed proven — consensus reject path intact);
 *   (d) the empty-table (curable) vs missing-historical-anchor / from-genesis
 *       (not seed-curable) discrimination. */

#include "test/test_helpers.h"

#include "conditions/sapling_anchor_frontier_unavailable.h"
#include "framework/condition.h"
#include "jobs/utxo_apply_anchors.h"
#include "jobs/utxo_apply_delta.h"
#include "primitives/block.h"
#include "sapling/incremental_merkle_tree.h"
#include "storage/anchor_kv.h"
#include "util/safe_alloc.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#define SAFC_CHECK(name, expr) do {                                   \
    printf("sapling_anchor_frontier_condition: %s... ", (name));      \
    if ((expr)) printf("OK\n");                                       \
    else { printf("FAIL\n"); failures++; }                            \
} while (0)

static void safc_fill(struct uint256 *h, uint8_t seed, size_t idx)
{
    for (size_t i = 0; i < 32; i++)
        h->data[i] = (uint8_t)(seed ^ (idx + i));
}

/* Build a non-empty Sapling frontier of `n` synthetic commitments. */
static void safc_build_tree(size_t n, struct incremental_merkle_tree *out)
{
    sapling_tree_init(out);
    for (size_t i = 0; i < n; i++) {
        struct uint256 cm;
        safc_fill(&cm, 0x5A, i);
        incremental_tree_append(out, &cm);
    }
}

/* A block carrying one Sapling output (triggers fold_sapling). */
static bool safc_block_with_output(struct block *blk, uint8_t cm_seed)
{
    block_init(blk);
    blk->num_vtx = 1;
    blk->vtx = zcl_calloc(1, sizeof(*blk->vtx), "safc_block_tx");
    if (!blk->vtx)
        return false;
    transaction_init(&blk->vtx[0]);
    blk->vtx[0].num_shielded_output = 1;
    blk->vtx[0].v_shielded_output =
        zcl_calloc(1, sizeof(*blk->vtx[0].v_shielded_output), "safc_out");
    if (!blk->vtx[0].v_shielded_output) {
        block_free(blk);
        return false;
    }
    safc_fill(&blk->vtx[0].v_shielded_output[0].cm, cm_seed, 0);
    return true;
}

static void safc_summary_init(struct delta_summary *s)
{
    memset(s, 0, sizeof(*s));
    s->ok = true;
    s->status = "ok";
}

int test_sapling_anchor_frontier_condition(void);
int test_sapling_anchor_frontier_condition(void)
{
    int failures = 0;

    /* (d.1) from-genesis store (activation==0): an empty table is COMPLETE, not
     * a curable gap. */
    {
        sqlite3 *db = NULL;
        SAFC_CHECK("open from-genesis store",
                   sqlite3_open(":memory:", &db) == SQLITE_OK && db);
        SAFC_CHECK("initialize from-genesis history (activation=0)",
                   anchor_kv_initialize_history(db, 0));
        SAFC_CHECK("classify from-genesis empty table == NONE",
                   sapling_anchor_frontier_classify(db) ==
                       SAPLING_ANCHOR_GAP_NONE);
        if (db) sqlite3_close(db);
    }

    /* (a)+(d.2) empty table + activation>0 == curable EMPTY_TABLE birth defect. */
    sqlite3 *db = NULL;
    SAFC_CHECK("open snapshot-seeded store",
               sqlite3_open(":memory:", &db) == SQLITE_OK && db);
    if (!db)
        return failures + 1;
    SAFC_CHECK("initialize adopted-above-genesis history (activation=100)",
               anchor_kv_initialize_history(db, 100));
    SAFC_CHECK("classify empty table + activation>0 == EMPTY_TABLE",
               sapling_anchor_frontier_classify(db) ==
                   SAPLING_ANCHOR_GAP_EMPTY_TABLE);

    /* The stall: a shielded-output block finds the empty table -> fail closed. */
    {
        struct block blk;
        SAFC_CHECK("build shielded-output block", safc_block_with_output(&blk, 0x11));
        struct delta_summary s;
        safc_summary_init(&s);
        bool store_ok =
            utxo_apply_check_and_insert_anchors(db, &blk, 150, &s);
        SAFC_CHECK("pre-seed fold fails closed (history gap, no store error)",
                   store_ok && !s.ok &&
                   s.status &&
                   strcmp(s.status, "shielded_anchor_history_gap") == 0);
        block_free(&blk);
    }

    /* (c) root-MISMATCH seed writes NOTHING; the stall persists. */
    {
        struct incremental_merkle_tree seed_tree;
        safc_build_tree(7, &seed_tree);
        struct uint256 wrong_root;
        safc_fill(&wrong_root, 0xEE, 999);   /* deliberately != frontier root */
        SAFC_CHECK("mismatched-root seed is REFUSED",
                   !anchor_kv_seed_frontier_row(db, ANCHOR_POOL_SAPLING,
                                                &seed_tree, 100, &wrong_root));
        bool empty = false;
        SAFC_CHECK("refused seed wrote NOTHING (table still empty)",
                   anchor_kv_table_is_empty(db, ANCHOR_POOL_SAPLING, &empty) &&
                   empty);
        SAFC_CHECK("classify still EMPTY_TABLE after refused seed",
                   sapling_anchor_frontier_classify(db) ==
                       SAPLING_ANCHOR_GAP_EMPTY_TABLE);
    }

    /* (b) verified seed (root matches) -> latest_tree FOUND and the fold that
     * previously failed closed now proceeds. */
    struct incremental_merkle_tree seed_tree;
    struct uint256 seed_root;
    safc_build_tree(7, &seed_tree);
    incremental_tree_root(&seed_tree, &seed_root);
    SAFC_CHECK("verified seed (root matches) inserts the frontier",
               anchor_kv_seed_frontier_row(db, ANCHOR_POOL_SAPLING,
                                           &seed_tree, 100, &seed_root));
    {
        struct incremental_merkle_tree got;
        struct uint256 got_root;
        int64_t got_h = -1;
        SAFC_CHECK("anchor_kv_latest_tree == FOUND after verified seed",
                   anchor_kv_latest_tree(db, ANCHOR_POOL_SAPLING, &got, &got_root,
                                         &got_h) == ANCHOR_KV_FOUND &&
                   got_h == 100 &&
                   memcmp(got_root.data, seed_root.data, 32) == 0);
    }
    SAFC_CHECK("birth defect cured: classify no longer EMPTY_TABLE once seeded",
               sapling_anchor_frontier_classify(db) !=
                   SAPLING_ANCHOR_GAP_EMPTY_TABLE);
    {
        struct block blk;
        safc_block_with_output(&blk, 0x22);
        struct delta_summary s;
        safc_summary_init(&s);
        bool store_ok =
            utxo_apply_check_and_insert_anchors(db, &blk, 150, &s);
        SAFC_CHECK("post-seed fold PROCEEDS (ok, no gap) and threads the frontier",
                   store_ok && s.ok);
        block_free(&blk);
        /* The fold inserted the extended frontier at h=150. */
        struct incremental_merkle_tree t2;
        int64_t h2 = -1;
        SAFC_CHECK("fold advanced the stored frontier to h=150",
                   anchor_kv_latest_tree(db, ANCHOR_POOL_SAPLING, &t2, NULL,
                                         &h2) == ANCHOR_KV_FOUND && h2 == 150);
    }

    /* (d.3) rows present == HISTORICAL (not seed-curable via the birth-defect
     * cure). */
    SAFC_CHECK("classify non-empty table == HISTORICAL (owner-gated)",
               sapling_anchor_frontier_classify(db) ==
                   SAPLING_ANCHOR_GAP_HISTORICAL);

    sqlite3_close(db);

    /* Registration: the condition is wired into the engine registry. */
    register_sapling_anchor_frontier_unavailable();
    SAFC_CHECK("condition registered in the engine",
               condition_engine_has_registered(
                   "sapling_anchor_frontier_unavailable"));

    return failures;
}
