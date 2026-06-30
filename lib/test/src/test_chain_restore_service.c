/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for chain_restore_service — planning pattern tests.
 * Each test exercises the pure plan() function with struct inputs. */

#include "platform/time_compat.h"
#include "test/test_helpers.h"
#include "services/block_index_integrity.h"
#include "services/chain_restore_boot_activation.h"
#include "services/chain_restore_boot_snapshot.h"
#include "services/chain_restore_executor.h"
#include "services/chain_restore_integrity.h"
#include "services/chain_restore_planner.h"
#include "services/chain_restore_repair.h"
#include "validation/main_state.h"
#include "chain/chain.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "storage/disk_block_io.h"
#include "storage/progress_store.h"
#include "core/amount.h"
#include <sqlite3.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include "util/safe_alloc.h"

extern bool process_block_test_hydrate_index_from_disk(
    struct block_index *pindex, const char *datadir);

/* ── Plan tests ────────────────────────────────────────────────── */

static int test_plan_hash_found_in_map(void) {
    int failures = 0;
    TEST("chain_restore_plan: hash found in block_map sets chain tip") {
        struct chain_restore_input in = {0};
        uint256_set_hex(&in.coins_best_hash, "0000abcd");
        in.hash_found_in_map = true;
        in.found_height = 500000;
        in.source = CHAIN_RESTORE_SRC_LDB_IMPORT;

        struct chain_restore_plan plan;
        chain_restore_plan(&plan, &in);

        ASSERT(plan.next_state == CHAIN_RESTORE_FOUND_IN_INDEX);
        ASSERT(plan.should_create_anchor == false);
        ASSERT(plan.should_set_chain_tip == true);
        ASSERT(plan.should_set_best_header == true);
        ASSERT(plan.should_skip_activate == true);
        ASSERT(plan.anchor_height == 500000);
        PASS();
    } _test_next:;
    return failures;
}

static int test_plan_hash_not_found_with_height(void) {
    int failures = 0;
    TEST("chain_restore_plan: hash not found, utxo height → create anchor") {
        struct chain_restore_input in = {0};
        uint256_set_hex(&in.coins_best_hash, "0000abcd");
        in.hash_found_in_map = false;
        in.utxo_max_height = 3072280;
        in.source = CHAIN_RESTORE_SRC_LDB_IMPORT;

        struct chain_restore_plan plan;
        chain_restore_plan(&plan, &in);

        ASSERT(plan.next_state == CHAIN_RESTORE_ANCHOR_CREATED);
        ASSERT(plan.should_create_anchor == true);
        ASSERT(plan.should_set_chain_tip == false);
        ASSERT(plan.should_set_best_header == false);
        ASSERT(plan.should_set_snapshot_anchor == true);
        ASSERT(plan.should_skip_activate == true);
        ASSERT(plan.anchor_height == 3072280);
        PASS();
    } _test_next:;
    return failures;
}

static int test_plan_null_hash(void) {
    int failures = 0;
    TEST("chain_restore_plan: null hash → FAILED") {
        struct chain_restore_input in = {0};
        /* coins_best_hash is all zeros (null) */

        struct chain_restore_plan plan;
        chain_restore_plan(&plan, &in);

        ASSERT(plan.next_state == CHAIN_RESTORE_FAILED);
        ASSERT(plan.should_create_anchor == false);
        ASSERT(plan.should_skip_activate == true);
        PASS();
    } _test_next:;
    return failures;
}

static int test_plan_no_utxos(void) {
    int failures = 0;
    TEST("chain_restore_plan: hash not found, no UTXOs → FAILED") {
        struct chain_restore_input in = {0};
        uint256_set_hex(&in.coins_best_hash, "0000abcd");
        in.hash_found_in_map = false;
        in.utxo_max_height = 0;

        struct chain_restore_plan plan;
        chain_restore_plan(&plan, &in);

        ASSERT(plan.next_state == CHAIN_RESTORE_FAILED);
        ASSERT(plan.should_skip_activate == true);
        PASS();
    } _test_next:;
    return failures;
}

/* Round 7 A4: plan result must land in the boot snapshot so
 * zcl_state(subsystem="boot") shows WHY chain_restore failed. */
static int test_plan_records_failed_state_in_boot_snapshot(void) {
    int failures = 0;
    TEST("chain_restore_plan: FAILED case records reason in boot snapshot") {
        struct chain_restore_input in = {0};
        uint256_set_hex(&in.coins_best_hash, "0000abcd");
        in.hash_found_in_map = false;
        in.utxo_max_height = 0;

        struct chain_restore_plan plan;
        chain_restore_plan(&plan, &in);

        struct chain_restore_boot_snapshot snap;
        chain_restore_get_boot_snapshot(&snap);
        ASSERT(snap.plan_recorded == true);
        ASSERT(snap.plan_next_state == (int)CHAIN_RESTORE_FAILED);
        ASSERT(snap.plan_should_skip_activate == true);
        ASSERT(strstr(snap.plan_reason, "height unknown") != NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_plan_snapshot_source(void) {
    int failures = 0;
    TEST("chain_restore_plan: snapshot source creates anchor with reason") {
        struct chain_restore_input in = {0};
        uint256_set_hex(&in.coins_best_hash, "0000abcd");
        in.hash_found_in_map = false;
        in.utxo_max_height = 100000;
        in.source = CHAIN_RESTORE_SRC_SNAPSHOT;

        struct chain_restore_plan plan;
        chain_restore_plan(&plan, &in);

        ASSERT(plan.next_state == CHAIN_RESTORE_ANCHOR_CREATED);
        ASSERT(plan.should_create_anchor == true);
        /* reason should mention "snapshot" */
        ASSERT(strstr(plan.reason, "snapshot") != NULL);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Execution tests ───────────────────────────────────────────── */

static int test_execute_anchor_creation(void) {
    int failures = 0;
    TEST("chain_restore_execute: creates anchor in block_map") {
        struct main_state ms;
        main_state_init(&ms);

        struct uint256 hash;
        uint256_set_hex(&hash, "0000abcdef1234567890");

        struct block_index *anchor = chain_restore_create_anchor(
            &ms, &hash, 500000);

        ASSERT(anchor != NULL);
        ASSERT(anchor->nHeight == 500000);
        ASSERT((anchor->nStatus & BLOCK_VALID_MASK) == BLOCK_VALID_UNKNOWN);
        ASSERT((anchor->nStatus & BLOCK_HAVE_DATA) == 0);
        ASSERT(anchor->nChainTx == 0);
        ASSERT(anchor->nTx == 0);
        ASSERT(anchor->phashBlock != NULL);

        /* Verify findable in block_map */
        struct block_index *found = block_map_find(&ms.map_block_index, &hash);
        ASSERT(found == anchor);

        struct arith_uint256 zero;
        arith_uint256_set_zero(&zero);
        ASSERT(arith_uint256_compare(&anchor->nChainWork, &zero) == 0);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);
        PASS();
    } _test_next:;
    return failures;
}

static int test_execute_records_anchor_without_consensus_tip(void) {
    int failures = 0;
    TEST("chain_restore_execute: records anchor without consensus tip") {
        struct main_state ms;
        main_state_init(&ms);

        struct chain_restore_input in = {0};
        uint256_set_hex(&in.coins_best_hash, "0000abcdef1234567890");
        in.hash_found_in_map = false;
        in.utxo_max_height = 500000;
        in.source = CHAIN_RESTORE_SRC_NORMAL_BOOT;

        struct chain_restore_plan plan;
        chain_restore_plan(&plan, &in);

        struct block_index *result = chain_restore_execute(&plan, &ms);
        ASSERT(result != NULL);

        struct block_index *tip = active_chain_tip(&ms.chain_active);
        ASSERT(tip == NULL);
        ASSERT(ms.pindex_best_header == NULL);
        ASSERT((result->nStatus & BLOCK_HAVE_DATA) == 0);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);
        PASS();
    } _test_next:;
    return failures;
}

static int test_execute_found_in_index(void) {
    int failures = 0;
    TEST("chain_restore_execute: found in index sets tip without creating anchor") {
        struct main_state ms;
        main_state_init(&ms);

        /* Pre-insert a block_index */
        struct uint256 hash;
        uint256_set_hex(&hash, "0000abcdef1234567890");
        struct block_index *existing = zcl_calloc(1, sizeof(struct block_index), "test_block_index");
        block_index_init(existing);
        existing->nHeight = 300000;
        existing->nStatus = BLOCK_VALID_TREE | BLOCK_HAVE_DATA;
        block_map_insert(&ms.map_block_index, &hash, existing);
        existing->hashBlock = hash;
        existing->phashBlock = &existing->hashBlock;

        struct chain_restore_input in = {0};
        in.coins_best_hash = hash;
        in.hash_found_in_map = true;
        in.found_height = 300000;

        struct chain_restore_plan plan;
        chain_restore_plan(&plan, &in);

        struct block_index *result = chain_restore_execute(&plan, &ms);
        ASSERT(result == existing);

        struct block_index *tip = active_chain_tip(&ms.chain_active);
        ASSERT(tip == existing);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Validation tests ──────────────────────────────────────────── */

static int test_validate_after_metadata_anchor(void) {
    int failures = 0;
    TEST("chain_restore_validate: metadata anchor is not a chain tip") {
        struct main_state ms;
        main_state_init(&ms);

        struct uint256 hash;
        uint256_set_hex(&hash, "0000abcdef1234567890");

        struct chain_restore_input in = {0};
        in.coins_best_hash = hash;
        in.hash_found_in_map = false;
        in.utxo_max_height = 500000;

        struct chain_restore_plan plan;
        chain_restore_plan(&plan, &in);
        chain_restore_execute(&plan, &ms);

        struct chain_restore_validation val;
        chain_restore_validate(&val, &ms, &hash, 500000);

        ASSERT(val.coins_hash_valid == true);
        ASSERT(val.anchor_in_map == true);
        ASSERT(val.chain_tip_set == false);
        ASSERT(val.tip_matches_expected == false);
        ASSERT(val.all_ok == false);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Activation decision tests ─────────────────────────────────── */

static int test_activation_normal_boot(void) {
    int failures = 0;
    TEST("boot_should_activate: normal boot with UTXOs → activate") {
        struct boot_activation_decision dec;
        boot_should_activate_chain(&dec, 500000, 1000000, 600000,
                                   false, false);
        ASSERT(dec.should_activate == true);
        ASSERT(dec.reason == ACTIVATE_OK);
        PASS();
    } _test_next:;
    return failures;
}

static int test_activation_legacy_import(void) {
    int failures = 0;
    TEST("boot_should_activate: legacy import → skip") {
        struct boot_activation_decision dec;
        boot_should_activate_chain(&dec, 0, 0, 0, true, false);
        ASSERT(dec.should_activate == false);
        ASSERT(dec.reason == ACTIVATE_SKIP_LEGACY_IMPORT);
        PASS();
    } _test_next:;
    return failures;
}

static int test_activation_anchor_created(void) {
    int failures = 0;
    TEST("boot_should_activate: anchor created → skip") {
        struct boot_activation_decision dec;
        boot_should_activate_chain(&dec, 3072280, 1000000, 600000,
                                   false, true);
        ASSERT(dec.should_activate == false);
        ASSERT(dec.reason == ACTIVATE_SKIP_ANCHOR_CREATED);
        PASS();
    } _test_next:;
    return failures;
}

static int test_activation_no_utxos_many_headers(void) {
    int failures = 0;
    TEST("boot_should_activate: no UTXOs, many headers → skip (awaiting snapshot)") {
        struct boot_activation_decision dec;
        boot_should_activate_chain(&dec, 0, 50, 100000,
                                   false, false);
        ASSERT(dec.should_activate == false);
        ASSERT(dec.reason == ACTIVATE_SKIP_NO_UTXOS_AWAITING);
        PASS();
    } _test_next:;
    return failures;
}

static int test_activation_few_utxos_few_headers(void) {
    int failures = 0;
    TEST("boot_should_activate: few UTXOs, few headers → activate (not awaiting)") {
        struct boot_activation_decision dec;
        boot_should_activate_chain(&dec, 0, 50, 10,
                                   false, false);
        ASSERT(dec.should_activate == true);
        ASSERT(dec.reason == ACTIVATE_OK);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Integrity ───────────────────────────────── */

/* Build a clean chain of N linked pindex entries (h=0..N-1) with real
 * nBits and pprev, then set the tip. Integrity must hold. */
static int test_integrity_passes_on_clean_chain(void) {
    int failures = 0;
    TEST("chain_integrity: clean pprev-linked chain passes") {
        struct main_state ms;
        main_state_init(&ms);

        const int N = 20;
        struct uint256 hashes[20];
        for (int h = 0; h < N; h++) {
            memset(&hashes[h], 0, sizeof(hashes[h]));
            hashes[h].data[0] = (uint8_t)(h & 0xFF);
            hashes[h].data[1] = (uint8_t)((h >> 8) & 0xFF);
            hashes[h].data[3] = 0xAB;
            struct block_index *pi = chainstate_insert_block_index(
                (struct chainstate *)&ms, &hashes[h]);
            ASSERT(pi != NULL);
            pi->nHeight  = h;
            pi->nBits    = 0x1f07ffff;
            pi->nTime    = 1000000 + (uint32_t)h * 150;
            pi->nVersion = 4;
            pi->nStatus  = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
            pi->nTx      = 1;
            pi->nChainTx = (uint32_t)(h + 1);
            arith_uint256_set_u64(&pi->nChainWork, (uint64_t)(h + 1));
            if (h > 0) {
                struct block_index *prev = block_map_find(
                    &ms.map_block_index, &hashes[h - 1]);
                if (prev) pi->pprev = prev;
            }
        }
        struct block_index *tip = block_map_find(
            &ms.map_block_index, &hashes[N - 1]);
        ASSERT(active_chain_move_window_tip(&ms.chain_active, tip));

        struct chain_integrity_result r;
        chain_integrity_check_post_restore(&r, &ms);
        ASSERT(r.zero_nbits_count == 0);
        ASSERT(r.active_chain_holes == 0);
        ASSERT(r.tip_height == N - 1);
        ASSERT(r.first_nbits_zero_height == -1);
        ASSERT(r.first_hole_height == -1);
        ASSERT(r.ok == true);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);
        PASS();
    } _test_next:;
    return failures;
}

/* Round 5 design: a synthetic anchor (BLOCK_VALID_UNKNOWN, no
 * BLOCK_HAVE_DATA) is a benign placeholder created by chain_restore
 * when coins_best_block is unrecoverable. The integrity gate
 * deliberately skips such entries (no header → no validation walk →
 * nBits=0 is harmless) and treats below-tip holes as diagnostic
 * counters that do not gate `ok`. Operational requirement: nBits
 * clean across BLOCK_HAVE_DATA entries + tip slot populated.
 *
 * If this test asserts r.ok == false, the integrity gate has
 * regressed to its pre-Round-5 behavior and will crash-loop nodes
 * whose chain_restore had to fall back to anchor recovery. */
static int test_integrity_anchor_restore_is_benign(void) {
    int failures = 0;
    TEST("chain_integrity: synthetic anchor (no DATA, nBits=0) is benign") {
        struct main_state ms;
        main_state_init(&ms);

        const int H = 1000;
        struct uint256 anchor_hash;
        uint256_set_hex(&anchor_hash, "00000000feedface");
        struct block_index *anchor =
            chain_restore_create_anchor(&ms, &anchor_hash, H);
        ASSERT(anchor != NULL);
        ASSERT(anchor->nHeight == H);
        ASSERT(anchor->nBits == 0);
        ASSERT(anchor->pprev == NULL);
        ASSERT(!(anchor->nStatus & BLOCK_HAVE_DATA));

        ASSERT(active_chain_move_window_tip(&ms.chain_active, anchor));

        struct chain_integrity_result r;
        chain_integrity_check_post_restore(&r, &ms);
        /* Anchor lacks BLOCK_HAVE_DATA → skipped by nBits scan. */
        ASSERT(r.zero_nbits_count == 0);
        ASSERT(r.first_nbits_zero_height == -1);
        /* A synthetic anchor is not a real validation tip. Even though
         * below-tip holes are diagnostic-only for real tips, placeholders
         * must remain fail-closed until backed by block data. */
        ASSERT(r.active_chain_holes == H);
        ASSERT(r.first_hole_height == 0);
        ASSERT(r.tip_height == H);
        /* Tip slot is populated by anchor itself. */
        ASSERT(active_chain_at(&ms.chain_active, H) == anchor);
        ASSERT(r.ok == false);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);
        PASS();
    } _test_next:;
    return failures;
}

static int test_integrity_live_tip_only_chain_is_operational(void) {
    int failures = 0;
    TEST("chain_integrity: near-tip holes and mismatches need repair") {
        struct main_state ms;
        main_state_init(&ms);

        const int H = 64;
        struct uint256 hashes[65];
        struct block_index *idx[65];
        memset(idx, 0, sizeof(idx));
        for (int h = 0; h <= H; h++) {
            memset(&hashes[h], 0, sizeof(hashes[h]));
            hashes[h].data[0] = (uint8_t)(h & 0xFF);
            hashes[h].data[3] = 0xBC;
            idx[h] = chainstate_insert_block_index(
                (struct chainstate *)&ms, &hashes[h]);
            ASSERT(idx[h] != NULL);
            idx[h]->nHeight = h;
            idx[h]->nBits = 0x1f07ffff;
            idx[h]->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
            idx[h]->nTx = 1;
            idx[h]->nChainTx = (uint32_t)(h + 1);
            if (h > 0)
                idx[h]->pprev = idx[h - 1];
            arith_uint256_set_u64(&idx[h]->nChainWork,
                                  (uint64_t)(h + 1));
        }
        ASSERT(active_chain_move_window_tip(&ms.chain_active, idx[H]));

        for (int h = H - 20; h < H; h++)
            ms.chain_active.chain[h] = NULL;
        ms.chain_active.chain[10] = idx[9];

        struct chain_integrity_result r;
        chain_integrity_check_post_restore(&r, &ms);
        ASSERT(r.zero_nbits_count == 0);
        ASSERT(r.tip_window_holes == 20);
        ASSERT(r.active_chain_mismatches > 0);
        ASSERT(active_chain_at(&ms.chain_active, H) == idx[H]);
        ASSERT(r.ok == false);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);
        PASS();
    } _test_next:;
    return failures;
}

/* Isolate a clean pprev-linked chain with ONE nBits=0 entry
 * above genesis must trip the limb independently. */
static int test_integrity_detects_isolated_nbits_zero(void) {
    int failures = 0;
    TEST("chain_integrity: single nBits=0 above genesis is detected") {
        struct main_state ms;
        main_state_init(&ms);

        const int N = 5;
        struct uint256 hashes[5];
        for (int h = 0; h < N; h++) {
            memset(&hashes[h], 0, sizeof(hashes[h]));
            hashes[h].data[0] = (uint8_t)(h & 0xFF);
            hashes[h].data[3] = 0xCD;
            struct block_index *pi = chainstate_insert_block_index(
                (struct chainstate *)&ms, &hashes[h]);
            pi->nHeight  = h;
            pi->nBits    = (h == 3) ? 0u : 0x1f07ffffu;   /* poison h=3 */
            pi->nTime    = 1000 + (uint32_t)h * 150;
            pi->nStatus  = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
            if (h > 0) pi->pprev = block_map_find(
                &ms.map_block_index, &hashes[h - 1]);
            arith_uint256_set_u64(&pi->nChainWork, (uint64_t)(h + 1));
        }
        struct block_index *tip = block_map_find(
            &ms.map_block_index, &hashes[N - 1]);
        ASSERT(active_chain_move_window_tip(&ms.chain_active, tip));

        struct chain_integrity_result r;
        chain_integrity_check_post_restore(&r, &ms);
        ASSERT(r.zero_nbits_count == 1);
        ASSERT(r.first_nbits_zero_height == 3);
        ASSERT(r.active_chain_holes == 0);
        ASSERT(r.ok == false);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Post-restore repair ──────────────────
 *
 * These exercise the GREEN limbs of the fix:
 *   - chain_restore_rebuild_active_chain fills holes below the tip
 *     via block_map when the pprev chain dead-ends at an anchor,
 *   - chain_restore_backfill_nbits_from_disk reads the header from
 *     the block file and assigns nBits when the pindex carries zero. */

static int test_rebuild_active_chain_fills_holes_from_block_map(void) {
    int failures = 0;
    TEST("chain_restore_rebuild: fills chain_active holes from block_map") {
        struct main_state ms;
        main_state_init(&ms);

        /* Scenario: LDB snapshot populated block_map with entries at
         * h=0..H (pprev NOT linked — matches the live-node post-scan
         * shape where heights have been patched but pprev is stale).
         * Then an anchor-restore installed tip=anchor at h=H with
         * anchor->pprev=NULL — active_chain_move_window_tip wrote NULL into
         * slots 0..H-1. rebuild must fill them by height lookup. */
        const int H = 10;
        struct uint256 hashes[11];
        for (int h = 0; h <= H; h++) {
            memset(&hashes[h], 0, sizeof(hashes[h]));
            hashes[h].data[0] = (uint8_t)(h & 0xFF);
            hashes[h].data[3] = 0xEE;
            struct block_index *pi = chainstate_insert_block_index(
                (struct chainstate *)&ms, &hashes[h]);
            ASSERT(pi != NULL);
            pi->nHeight = h;
            pi->nBits   = 0x1f07ffff;
            pi->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
            pi->nTx     = 1;
            if (h > 0)
                pi->pprev = block_map_find(&ms.map_block_index,
                                            &hashes[h - 1]);
            arith_uint256_set_u64(&pi->nChainWork, (uint64_t)(h + 1));
        }
        struct block_index *tip = block_map_find(
            &ms.map_block_index, &hashes[H]);
        ASSERT(tip != NULL);
        ASSERT(active_chain_move_window_tip(&ms.chain_active, tip));
        for (int h = 0; h < H; h++)
            ms.chain_active.chain[h] = NULL;

        /* Pre-rebuild: integrity check reports H holes below the tip.
         * Round 4 Part 1.5.1: `ok` no longer requires zero holes —
         * only nBits clean + tip-slot populated. Tip IS populated
         * via active_chain_move_window_tip above, so r0.ok may be true even
         * with holes below. We still verify the hole counts. */
        struct chain_integrity_result r0;
        chain_integrity_check_post_restore(&r0, &ms);
        ASSERT(r0.active_chain_holes == H);
        ASSERT(r0.first_hole_height == 0);

        /* Apply rebuild. Every slot 0..H must now resolve to the
         * block_map entry of that height. */
        int populated = chain_restore_rebuild_active_chain(&ms, tip, NULL);
        ASSERT(populated >= H);

        for (int h = 0; h <= H; h++) {
            struct block_index *got = active_chain_at(&ms.chain_active, h);
            ASSERT(got != NULL);
            ASSERT(got->nHeight == h);
        }

        struct chain_integrity_result r1;
        chain_integrity_check_post_restore(&r1, &ms);
        ASSERT(r1.active_chain_holes == 0);
        ASSERT(r1.first_hole_height == -1);
        ASSERT(r1.zero_nbits_count == 0);
        ASSERT(r1.ok == true);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);
        PASS();
    } _test_next:;
    return failures;
}

/* Regression test: rebuild_active_chain must be O(N), not O(N²).
 *
 * Live shape: post-anchor restore installs a tip at ~h=3M with pprev=NULL.
 * active_chain_move_window_tip writes NULL into every slot below the tip. The
 * residual-hole fill then has tip_h NULL slots, and the pre-fix code did
 * a fresh block_map scan per hole — tip_h × block_map_size ops. At live
 * scale that's ~10 trillion ops and pins the node at ~92% CPU for >5min
 * before RPC comes up, which is why the coordinator has to SIGTERM every
 * boot. This test reproduces the shape at N=100k; pre-fix it takes many
 * seconds-to-minutes, post-fix it completes in O(N). */
static int test_rebuild_active_chain_scales_at_100k(void) {
    int failures = 0;
    TEST("chain_restore_rebuild: completes in <2s at realistic chain depth") {
        struct main_state ms;
        main_state_init(&ms);

        const int H = 100000;
        ASSERT(block_map_reserve(&ms.map_block_index, (size_t)(H + 16)));

        struct uint256 *hashes = zcl_calloc(
            (size_t)(H + 1), sizeof(struct uint256), "p14_13_hashes");
        ASSERT(hashes != NULL);

        for (int h = 0; h <= H; h++) {
            memcpy(hashes[h].data, &h, sizeof(h));
            hashes[h].data[16] = 0xAB;
            hashes[h].data[17] = 0xCD;
            struct block_index *pi = chainstate_insert_block_index(
                (struct chainstate *)&ms, &hashes[h]);
            ASSERT(pi != NULL);
            pi->nHeight = h;
            pi->nBits   = 0x1f07ffff;
            pi->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
            pi->nTx     = 1;
            if (h > 0)
                pi->pprev = block_map_find(&ms.map_block_index,
                                            &hashes[h - 1]);
            arith_uint256_set_u64(&pi->nChainWork, (uint64_t)(h + 1));
        }

        struct block_index *tip = block_map_find(
            &ms.map_block_index, &hashes[H]);
        ASSERT(tip != NULL);
        ASSERT(active_chain_move_window_tip(&ms.chain_active, tip));
        for (int h = 0; h < H; h++)
            ms.chain_active.chain[h] = NULL;

        /* Pre-rebuild sanity: every sub-tip slot is NULL. */
        ASSERT(active_chain_at(&ms.chain_active, 0) == NULL);
        ASSERT(active_chain_at(&ms.chain_active, H / 2) == NULL);
        ASSERT(active_chain_at(&ms.chain_active, H - 1) == NULL);

        struct timespec t0, t1;
        platform_time_monotonic_timespec(&t0);
        int populated = chain_restore_rebuild_active_chain(&ms, tip, NULL);
        platform_time_monotonic_timespec(&t1);

        double elapsed = (double)(t1.tv_sec - t0.tv_sec)
                       + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

        /* Correctness at scale — every height slot must resolve. */
        ASSERT(populated == H + 1);
        for (int h = 0; h <= H; h += 1000) {
            struct block_index *got = active_chain_at(&ms.chain_active, h);
            ASSERT(got != NULL);
            ASSERT(got->nHeight == h);
        }

        /* target. Pre-fix the residual-hole loop is O(N²) and
         * blows well past this budget even at N=100k. */
        if (elapsed >= 2.0) {
            printf("[] rebuild took %.3fs (>=2s) at H=%d — O(N^2) shape\n",
                   elapsed, H);
        }
        ASSERT(elapsed < 2.0);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);
        free(hashes);
        PASS();
    } _test_next:;
    return failures;
}

static int test_rebuild_high_tip_prefers_pprev_lineage_over_height_guess(void) {
    int failures = 0;
    TEST("chain_restore_rebuild: high-tip repair follows pprev lineage before by-height guess") {
        struct main_state ms;
        main_state_init(&ms);

        const int low = 999000;
        const int tip_h = 1010500;
        const int fork_h = 1000000;
        const int count = tip_h - low + 1;
        ASSERT(block_map_reserve(&ms.map_block_index, (size_t)(count + 8)));

        struct uint256 *hashes = zcl_calloc(
            (size_t)count, sizeof(struct uint256), "chain_restore_high_hashes");
        ASSERT(hashes != NULL);

        struct block_index *prev = NULL;
        struct block_index *tip = NULL;
        struct block_index *correct_fork_height = NULL;
        struct block_index *correct_fork_prev = NULL;
        for (int i = 0; i < count; i++) {
            int h = low + i;
            memcpy(hashes[i].data, &h, sizeof(h));
            hashes[i].data[16] = 0xCC;
            hashes[i].data[17] = 0x23;

            struct block_index *pi = chainstate_insert_block_index(
                (struct chainstate *)&ms, &hashes[i]);
            ASSERT(pi != NULL);
            pi->nHeight = h;
            pi->nBits = 0x1f07ffff;
            pi->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
            pi->nTx = 1;
            pi->pprev = prev;
            arith_uint256_set_u64(&pi->nChainWork, (uint64_t)(i + 1));

            if (h == fork_h) {
                correct_fork_height = pi;
                correct_fork_prev = prev;
            }
            prev = pi;
            tip = pi;
        }
        ASSERT(tip != NULL);
        ASSERT(correct_fork_height != NULL);
        ASSERT(correct_fork_prev != NULL);

        struct uint256 wrong_hash;
        memset(&wrong_hash, 0, sizeof(wrong_hash));
        memcpy(wrong_hash.data, &fork_h, sizeof(fork_h));
        wrong_hash.data[16] = 0xFA;
        wrong_hash.data[17] = 0x11;
        struct block_index *wrong = chainstate_insert_block_index(
            (struct chainstate *)&ms, &wrong_hash);
        ASSERT(wrong != NULL);
        wrong->nHeight = fork_h;
        wrong->nBits = 0x1f07ffff;
        wrong->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
        wrong->nTx = 1;
        wrong->pprev = correct_fork_prev->pprev;
        arith_uint256_set_u64(&wrong->nChainWork, (uint64_t)count + 1000u);

        int populated = chain_restore_rebuild_active_chain(&ms, tip, NULL);
        ASSERT(populated >= count);
        ASSERT(active_chain_at(&ms.chain_active, fork_h) == correct_fork_height);
        ASSERT(active_chain_at(&ms.chain_active, fork_h) != wrong);
        ASSERT(active_chain_at(&ms.chain_active, fork_h - 1) == correct_fork_prev);
        ASSERT(correct_fork_height->pprev == correct_fork_prev);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);
        free(hashes);
        PASS();
    } _test_next:;
    return failures;
}

/* Regression test: rebuild_active_chain must populate block_index.skipList
 * (pskip pointers) so post-restore ancestor walks are O(log N) not O(N).
 *
 * Live shape: after chain_restore_create_anchor the tip's pprev is NULL
 * and its pskip is NULL. Entries loaded from block_map may also have
 * pskip=NULL when the flat-file load path didn't build skips for every
 * height. Without pskip, block_index_get_ancestor falls back to pprev
 * hops only — 3M hops at live tip. The rebuild pass is the natural
 * place to wire pprev (from chain[h-1]) and BuildSkip() on every slot.
 *
 * Pre-fix: 1000 ancestor walks on a 100k-entry chain are either
 *   (a) impossible (pprev NULL → get_ancestor returns NULL), or
 *   (b) O(N) per walk (pprev wired, pskip NULL) which is multi-second.
 * Post-fix: pskip is populated on every slot above h=1; ancestor walks
 * complete in O(log N) ≈ ~20 hops per query. */
static int test_rebuild_populates_skiplist_for_log_n_ancestor(void) {
    int failures = 0;
    TEST("chain_restore_rebuild: populates skipList for O(log N) ancestor walk") {
        struct main_state ms;
        main_state_init(&ms);

        const int H = 100000;
        ASSERT(block_map_reserve(&ms.map_block_index, (size_t)(H + 16)));

        struct uint256 *hashes = zcl_calloc(
            (size_t)(H + 1), sizeof(struct uint256), "p14_14_hashes");
        ASSERT(hashes != NULL);

        for (int h = 0; h <= H; h++) {
            memcpy(hashes[h].data, &h, sizeof(h));
            hashes[h].data[16] = 0x14;
            hashes[h].data[17] = 0x14;
            struct block_index *pi = chainstate_insert_block_index(
                (struct chainstate *)&ms, &hashes[h]);
            ASSERT(pi != NULL);
            pi->nHeight = h;
            pi->nBits   = 0x1f07ffff;
            pi->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
            pi->nTx     = 1;
            if (h > 0)
                pi->pprev = block_map_find(&ms.map_block_index,
                                            &hashes[h - 1]);
            pi->pskip = NULL;
            arith_uint256_set_u64(&pi->nChainWork, (uint64_t)(h + 1));
        }

        struct block_index *tip = block_map_find(
            &ms.map_block_index, &hashes[H]);
        ASSERT(tip != NULL);
        ASSERT(tip->pskip == NULL);
        ASSERT(active_chain_move_window_tip(&ms.chain_active, tip));
        for (int h = 0; h < H; h++)
            ms.chain_active.chain[h] = NULL;

        int populated = chain_restore_rebuild_active_chain(&ms, tip, NULL);
        ASSERT(populated == H + 1);

        /* acceptance: every slot above h=1 must have pskip set.
         * Pre-fix this is 100% NULL → test goes RED. */
        int missing_skip = 0;
        int missing_prev = 0;
        for (int h = 2; h <= H; h++) {
            struct block_index *at = active_chain_at(&ms.chain_active, h);
            if (!at) continue;
            if (at->pskip == NULL) missing_skip++;
            if (at->pprev == NULL) missing_prev++;
        }
        ASSERT(missing_skip == 0);
        ASSERT(missing_prev == 0);

        /* Ancestor walk correctness — tip → genesis must return the
         * entry at h=0. Pre-fix, pprev=NULL → get_ancestor returns NULL. */
        struct block_index *a0 = block_index_get_ancestor(tip, 0);
        ASSERT(a0 != NULL);
        ASSERT(a0->nHeight == 0);
        ASSERT(a0 == active_chain_at(&ms.chain_active, 0));

        struct block_index *amid = block_index_get_ancestor(tip, H / 2);
        ASSERT(amid != NULL);
        ASSERT(amid->nHeight == H / 2);

        /* Performance budget: 1000 random-depth ancestor walks must
         * complete in <1s. At H=100k a pprev-only walk averages ~50k
         * hops, so 1000 walks ≈ 5×10^7 hops which measures in seconds
         * on this host. With pskip, each walk is ~20 hops → <50ms
         * total. The 1s budget leaves plenty of CI headroom. */
        struct timespec t0, t1;
        platform_time_monotonic_timespec(&t0);
        int queries = 1000;
        for (int i = 0; i < queries; i++) {
            int target = (i * 7919) % (H + 1);
            struct block_index *r = block_index_get_ancestor(tip, target);
            ASSERT(r != NULL);
            ASSERT(r->nHeight == target);
        }
        platform_time_monotonic_timespec(&t1);
        double elapsed = (double)(t1.tv_sec - t0.tv_sec)
                       + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
        if (elapsed >= 1.0) {
            printf("[] %d ancestor walks took %.3fs at H=%d "
                   "(pskip missing or ineffective)\n",
                   queries, elapsed, H);
        }
        ASSERT(elapsed < 1.0);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);
        free(hashes);
        PASS();
    } _test_next:;
    return failures;
}

/* Write a minimal real block to disk; return its disk_block_pos. */
static bool write_block_fixture(const char *datadir,
                                struct disk_block_pos *pos,
                                uint32_t nbits_value)
{
    struct block b;
    block_init(&b);
    b.header.nVersion = 4;
    b.header.nTime    = 1700000000;
    b.header.nBits    = nbits_value;
    b.num_vtx = 1;
    b.vtx = calloc(1, sizeof(struct transaction)); // raw-alloc-ok:test-fixture
    transaction_init(&b.vtx[0]);
    transaction_alloc(&b.vtx[0], 1, 1);
    b.vtx[0].vin[0].sequence = 0xffffffff;
    b.vtx[0].vout[0].value   = 10 * COIN;

    unsigned char msg_start[4] = {0x24, 0xe9, 0x27, 0x64};
    bool ok = write_block_to_disk(&b, pos, datadir, msg_start);
    block_free(&b);
    return ok;
}

static bool write_chain_block_fixture(const char *datadir,
                                      struct disk_block_pos *pos,
                                      const struct uint256 *prev,
                                      uint32_t nbits_value,
                                      uint32_t ntime,
                                      struct uint256 *hash_out)
{
    struct block b;
    block_init(&b);
    b.header.nVersion = 4;
    if (prev)
        b.header.hashPrevBlock = *prev;
    b.header.nTime = ntime;
    b.header.nBits = nbits_value;
    b.num_vtx = 1;
    b.vtx = calloc(1, sizeof(struct transaction)); // raw-alloc-ok:test-fixture
    transaction_init(&b.vtx[0]);
    transaction_alloc(&b.vtx[0], 1, 1);
    b.vtx[0].vin[0].sequence = 0xffffffff;
    b.vtx[0].vout[0].value = 10 * COIN;

    block_get_hash(&b, hash_out);
    unsigned char msg_start[4] = {0x24, 0xe9, 0x27, 0x64};
    bool ok = write_block_to_disk(&b, pos, datadir, msg_start);
    block_free(&b);
    return ok;
}

static bool next_block_append_pos(const char *datadir,
                                  struct disk_block_pos *pos)
{
    char path[320];
    snprintf(path, sizeof(path), "%s/blocks/blk00000.dat", datadir);
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    pos->nFile = 0;
    pos->nPos = (unsigned int)st.st_size;
    return true;
}

static int test_seed_anchor_backing_requires_exact_provenance(void) {
    int failures = 0;
    TEST("chain_restore_backing: seed anchor disk carve-out requires exact provenance") {
        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "./test-tmp/%d_seed_backing",
                 (int)getpid());
        mkdir("./test-tmp", 0755);
        mkdir(tmpdir, 0755);
        char blocksdir[320];
        snprintf(blocksdir, sizeof(blocksdir), "%s/blocks", tmpdir);
        mkdir(blocksdir, 0755);

        struct disk_block_pos pos = { .nFile = 0, .nPos = 0 };
        struct uint256 hash;
        const uint32_t expected_nbits = 0x1e14f400;
        ASSERT(write_chain_block_fixture(tmpdir, &pos, NULL,
                                         expected_nbits, 1700000000,
                                         &hash));

        struct main_state ms;
        main_state_init(&ms);
        struct block_index *seed = chainstate_insert_block_index(
            (struct chainstate *)&ms, &hash);
        ASSERT(seed != NULL);
        seed->nHeight = 500000;
        seed->pprev = NULL;
        seed->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
        seed->nFile = pos.nFile;
        seed->nDataPos = pos.nPos;
        seed->nBits = expected_nbits;
        seed->nTx = 1;
        seed->nChainTx = 1;

        ASSERT(!chain_restore_block_is_consensus_backed_on_disk(seed, tmpdir));
        ASSERT(chain_restore_block_is_consensus_backed_on_disk_seeded(
            seed, tmpdir, &hash, seed->nHeight));

        struct uint256 wrong_hash = hash;
        wrong_hash.data[0] ^= 0x01;
        ASSERT(!chain_restore_block_is_consensus_backed_on_disk_seeded(
            seed, tmpdir, &wrong_hash, seed->nHeight));
        ASSERT(!chain_restore_block_is_consensus_backed_on_disk_seeded(
            seed, tmpdir, &hash, seed->nHeight - 1));

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);

        char rm_cmd[512];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", tmpdir);
        (void)system(rm_cmd);
        PASS();
    } _test_next:;
    return failures;
}

static int test_rebuild_active_chain_scans_block_files_for_canonical_positions(void) {
    int failures = 0;
    TEST("chain_restore_rebuild: scans block files when index disk positions are stale") {
        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "./test-tmp/%d_rebuild_scan",
                 (int)getpid());
        mkdir("./test-tmp", 0755);
        mkdir(tmpdir, 0755);
        char blocksdir[320];
        snprintf(blocksdir, sizeof(blocksdir), "%s/blocks", tmpdir);
        mkdir(blocksdir, 0755);

        struct disk_block_pos pos[3] = {{ .nFile = 0, .nPos = 0 }};
        struct uint256 hashes[3];
        ASSERT(write_chain_block_fixture(tmpdir, &pos[0], NULL,
                                         0x1e14f400, 1700000000,
                                         &hashes[0]));
        ASSERT(next_block_append_pos(tmpdir, &pos[1]));
        ASSERT(write_chain_block_fixture(tmpdir, &pos[1], &hashes[0],
                                         0x1e14f401, 1700000001,
                                         &hashes[1]));
        ASSERT(next_block_append_pos(tmpdir, &pos[2]));
        ASSERT(write_chain_block_fixture(tmpdir, &pos[2], &hashes[1],
                                         0x1e14f402, 1700000002,
                                         &hashes[2]));

        struct main_state ms;
        main_state_init(&ms);
        struct block_index *idx[3];
        for (int h = 0; h < 3; h++) {
            idx[h] = chainstate_insert_block_index(
                (struct chainstate *)&ms, &hashes[h]);
            ASSERT(idx[h] != NULL);
            idx[h]->nHeight = h;
            idx[h]->nBits = 0x1e14f400 + (uint32_t)h;
            idx[h]->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
            idx[h]->nFile = pos[h].nFile;
            idx[h]->nDataPos = pos[h].nPos;
            idx[h]->nTx = 1;
            idx[h]->nChainTx = (unsigned int)(h + 1);
            if (h > 0)
                idx[h]->pprev = idx[h - 1];
        }

        /* Poison the middle entry so index-based ancestry reads the
         * genesis bytes for h=1. The block-file fallback must ignore
         * this stale position and recover by header hash. */
        idx[1]->nDataPos = pos[0].nPos;

        ASSERT(active_chain_move_window_tip(&ms.chain_active, idx[2]));
        int populated = chain_restore_rebuild_active_chain(&ms, idx[2], tmpdir);
        ASSERT(populated == 3);
        ASSERT(active_chain_at(&ms.chain_active, 0) == idx[0]);
        ASSERT(active_chain_at(&ms.chain_active, 1) == idx[1]);
        ASSERT(active_chain_at(&ms.chain_active, 2) == idx[2]);
        ASSERT(idx[1]->nHeight == 1);
        ASSERT(idx[1]->nDataPos == pos[1].nPos);
        ASSERT(idx[2]->pprev == idx[1]);
        ASSERT(idx[1]->pprev == idx[0]);
        ASSERT(idx[0]->pprev == NULL);

        struct chain_integrity_result r;
        chain_integrity_check_post_restore(&r, &ms);
        ASSERT(r.ok);
        ASSERT(r.active_chain_mismatches == 0);
        ASSERT(r.active_chain_holes == 0);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);

        char rm_cmd[512];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", tmpdir);
        (void)system(rm_cmd);
        PASS();
    } _test_next:;
    return failures;
}

static int test_backfill_nbits_reads_from_block_file(void) {
    int failures = 0;
    TEST("chain_restore_backfill_nbits: reads header nBits from disk") {
        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "./test-tmp/%d_nbits_backfill",
                 (int)getpid());
        mkdir("./test-tmp", 0755);
        mkdir(tmpdir, 0755);
        char blocksdir[320];
        snprintf(blocksdir, sizeof(blocksdir), "%s/blocks", tmpdir);
        mkdir(blocksdir, 0755);

        struct disk_block_pos pos = { .nFile = 0, .nPos = 0 };
        const uint32_t expected_nbits = 0x1e14f400;
        ASSERT(write_block_fixture(tmpdir, &pos, expected_nbits));
        ASSERT(pos.nFile >= 0);
        ASSERT(pos.nPos > 0);

        /* Build an index entry that matches the on-disk block but carries
         * nBits=0 — the live-node shape that `add_to_block_index` fails
         * to restore during anchor-restore rehydration. */
        struct main_state ms;
        main_state_init(&ms);

        struct block b_check;
        ASSERT(read_block_from_disk_pread(&b_check, &pos, tmpdir));
        struct uint256 blk_hash;
        block_get_hash(&b_check, &blk_hash);
        block_free(&b_check);

        struct block_index *pi = chainstate_insert_block_index(
            (struct chainstate *)&ms, &blk_hash);
        ASSERT(pi != NULL);
        pi->nHeight  = 500;
        pi->nBits = 0; /* shape */
        pi->nStatus  = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
        pi->nFile    = pos.nFile;
        pi->nDataPos = pos.nPos;

        int fixed = chain_restore_backfill_nbits_from_disk(&ms, tmpdir);
        ASSERT(fixed == 1);
        ASSERT(pi->nBits == expected_nbits);

        /* Idempotent: second call should not touch anything. */
        int fixed2 = chain_restore_backfill_nbits_from_disk(&ms, tmpdir);
        ASSERT(fixed2 == 0);
        ASSERT(pi->nBits == expected_nbits);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);

        char rm_cmd[512];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", tmpdir);
        (void)system(rm_cmd);
        PASS();
    } _test_next:;
    return failures;
}

static int test_connect_tip_hydrates_placeholder_from_disk(void) {
    int failures = 0;
    TEST("connect_tip hydration: verified disk block repairs nBits=0 HAVE_DATA entry") {
        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "./test-tmp/%d_connect_tip_hydrate",
                 (int)getpid());
        mkdir("./test-tmp", 0755);
        mkdir(tmpdir, 0755);
        char blocksdir[320];
        snprintf(blocksdir, sizeof(blocksdir), "%s/blocks", tmpdir);
        mkdir(blocksdir, 0755);

        struct disk_block_pos pos = { .nFile = 0, .nPos = 0 };
        const uint32_t expected_nbits = 0x1e14f400;
        ASSERT(write_block_fixture(tmpdir, &pos, expected_nbits));
        ASSERT(pos.nFile >= 0);
        ASSERT(pos.nPos > 0);

        struct block b_check;
        ASSERT(read_block_from_disk_pread(&b_check, &pos, tmpdir));
        struct uint256 blk_hash;
        block_get_hash(&b_check, &blk_hash);
        uint32_t expected_time = b_check.header.nTime;
        int32_t expected_version = b_check.header.nVersion;
        block_free(&b_check);

        struct block_index pi;
        block_index_init(&pi);
        pi.phashBlock = &blk_hash;
        pi.nHeight = 500;
        pi.nVersion = 0;
        pi.nTime = 0;
        pi.nBits = 0;
        pi.nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
        pi.nFile = pos.nFile;
        pi.nDataPos = pos.nPos;

        ASSERT(process_block_test_hydrate_index_from_disk(&pi, tmpdir));
        ASSERT(pi.nBits == expected_nbits);
        ASSERT(pi.nTime == expected_time);
        ASSERT(pi.nVersion == expected_version);

        char rm_cmd[512];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", tmpdir);
        (void)system(rm_cmd);
        PASS();
    } _test_next:;
    return failures;
}

static int test_backfill_nbits_skips_synthetic_anchor(void) {
    int failures = 0;
    TEST("chain_restore_backfill_nbits: skips synthetic anchors (nDataPos==0)") {
        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "./test-tmp/%d_nbits_skip",
                 (int)getpid());
        mkdir("./test-tmp", 0755);
        mkdir(tmpdir, 0755);

        struct main_state ms;
        main_state_init(&ms);

        struct uint256 anchor_hash;
        uint256_set_hex(&anchor_hash, "00000000feedface");
        struct block_index *anchor =
            chain_restore_create_anchor(&ms, &anchor_hash, 3081408);
        ASSERT(anchor != NULL);
        ASSERT(anchor->nBits == 0);
        ASSERT(anchor->nDataPos == 0);

        /* Without disk data, backfill must be a no-op and leave nBits==0. */
        int fixed = chain_restore_backfill_nbits_from_disk(&ms, tmpdir);
        ASSERT(fixed == 0);
        ASSERT(anchor->nBits == 0);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);

        char rm_cmd[512];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", tmpdir);
        (void)system(rm_cmd);
        PASS();
    } _test_next:;
    return failures;
}

static int test_finalize_null_datadir_skips_disk(void) {
    int failures = 0;
    TEST("chain_restore_finalize: NULL datadir skips disk path") {
        struct main_state ms;
        main_state_init(&ms);

        const int N = 4;
        struct uint256 hashes[4];
        for (int h = 0; h < N; h++) {
            memset(&hashes[h], 0, sizeof(hashes[h]));
            hashes[h].data[0] = (uint8_t)(h & 0xFF);
            hashes[h].data[3] = 0xDD;
            struct block_index *pi = chainstate_insert_block_index(
                (struct chainstate *)&ms, &hashes[h]);
            pi->nHeight = h;
            pi->nBits   = 0x1f07ffff;
            pi->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
            if (h > 0)
                pi->pprev = block_map_find(&ms.map_block_index, &hashes[h-1]);
            arith_uint256_set_u64(&pi->nChainWork, (uint64_t)(h + 1));
        }
        struct block_index *tip = block_map_find(
            &ms.map_block_index, &hashes[N-1]);
        ASSERT(active_chain_move_window_tip(&ms.chain_active, tip));
        bii_record_recovery_status(BII_TIP_MISSING_IN_SQL,
                                   BII_RECOVERY_RECONCILE_REQUIRED,
                                   "unit-test stale reconcile",
                                   true, false);

        /* Fully clean chain — finalize returns ZCL_OK. */
        ASSERT(chain_restore_finalize(&ms, NULL).ok == true);
        struct bii_recovery_status st;
        memset(&st, 0, sizeof(st));
        bii_get_recovery_status(&st);
        ASSERT(st.verdict == BII_OK);
        ASSERT(st.action == BII_RECOVERY_ACCEPTED);
        ASSERT(!st.degraded);
        ASSERT(strstr(st.reason, "post-restore integrity clean") != NULL);

        block_map_free(&ms.map_block_index);
        active_chain_free(&ms.chain_active);
        PASS();
    } _test_next:;
    return failures;
}

static int test_finalize_quarantine_preserves_served_floor(void) {
    int failures = 0;
    TEST("chain_restore_finalize: quarantine refuses below served finality") {
        char tmpdir[256];
        test_make_tmpdir(tmpdir, sizeof(tmpdir),
                         "chain_restore", "served_floor_quarantine");

        progress_store_close();
        ASSERT(progress_store_open(tmpdir));
        sqlite3 *db = progress_store_db();
        ASSERT(db != NULL);
        ASSERT(sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS tip_finalize_log("
            "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER)",
            NULL, NULL, NULL) == SQLITE_OK);
        ASSERT(sqlite3_exec(db,
            "INSERT OR REPLACE INTO tip_finalize_log"
            "(height,status,ok) VALUES(3,'finalized',1)",
            NULL, NULL, NULL) == SQLITE_OK);

        struct main_state ms;
        main_state_init(&ms);

        struct uint256 hashes[4];
        struct block_index *idx[4] = {0};
        for (int h = 0; h < 4; h++) {
            memset(&hashes[h], 0, sizeof(hashes[h]));
            hashes[h].data[0] = (uint8_t)(0x60 + h);
            hashes[h].data[31] = 0xC3;
            idx[h] = chainstate_insert_block_index(
                (struct chainstate *)&ms, &hashes[h]);
            ASSERT(idx[h] != NULL);
            idx[h]->nHeight = h;
            idx[h]->pprev = h > 0 ? idx[h - 1] : NULL;
            idx[h]->nBits = 0x1f07ffff;
            idx[h]->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
            idx[h]->nFile = 0;
            idx[h]->nDataPos = 1;
            idx[h]->nTx = 1;
            idx[h]->nChainTx = (unsigned)(h + 1);
            idx[h]->hashMerkleRoot.data[0] = (uint8_t)(0xA0 + h);
            arith_uint256_set_u64(&idx[h]->nChainWork, (uint64_t)(h + 1));
        }

        /* The active tip is synthetic/not consensus-backed; the nearest backed
         * ancestor is h=1. The served floor at h=3 must prevent publishing
         * that lower replacement. */
        idx[2]->nStatus = BLOCK_VALID_HEADER;
        idx[2]->nDataPos = 0;
        idx[2]->nTx = 0;
        idx[2]->nChainTx = 0;
        idx[3]->nStatus = BLOCK_VALID_HEADER;
        idx[3]->nDataPos = 0;
        idx[3]->nTx = 0;
        idx[3]->nChainTx = 0;

        ASSERT(active_chain_move_window_tip(&ms.chain_active, idx[3]));
        ASSERT(ms.chain_active.height == 3);

        (void)chain_restore_finalize(&ms, NULL);
        ASSERT(ms.chain_active.height == 3);
        ASSERT(active_chain_cached_tip(&ms.chain_active) == idx[3]);

        main_state_free(&ms);
        progress_store_close();
        test_cleanup_tmpdir(tmpdir);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Registration ──────────────────────────────────────────────── */

int test_chain_restore_service(void) {
    int failures = 0;
    /* Plan tests */
    failures += test_plan_hash_found_in_map();
    failures += test_plan_hash_not_found_with_height();
    failures += test_plan_null_hash();
    failures += test_plan_no_utxos();
    failures += test_plan_records_failed_state_in_boot_snapshot();
    failures += test_plan_snapshot_source();
    /* Execution tests */
    failures += test_execute_anchor_creation();
    failures += test_execute_records_anchor_without_consensus_tip();
    failures += test_execute_found_in_index();
    /* Validation tests */
    failures += test_validate_after_metadata_anchor();
    /* Activation decision tests */
    failures += test_activation_normal_boot();
    failures += test_activation_legacy_import();
    failures += test_activation_anchor_created();
    failures += test_activation_no_utxos_many_headers();
    failures += test_activation_few_utxos_few_headers();
    /* integrity-check tests */
    failures += test_integrity_passes_on_clean_chain();
    failures += test_integrity_anchor_restore_is_benign();
    failures += test_integrity_live_tip_only_chain_is_operational();
    failures += test_integrity_detects_isolated_nbits_zero();
    /* — post-restore repair tests */
    failures += test_rebuild_active_chain_fills_holes_from_block_map();
    failures += test_rebuild_active_chain_scales_at_100k();
    failures += test_rebuild_high_tip_prefers_pprev_lineage_over_height_guess();
    failures += test_rebuild_populates_skiplist_for_log_n_ancestor();
    failures += test_seed_anchor_backing_requires_exact_provenance();
    failures += test_rebuild_active_chain_scans_block_files_for_canonical_positions();
    failures += test_backfill_nbits_reads_from_block_file();
    failures += test_connect_tip_hydrates_placeholder_from_disk();
    failures += test_backfill_nbits_skips_synthetic_anchor();
    failures += test_finalize_null_datadir_skips_disk();
    failures += test_finalize_quarantine_preserves_served_floor();
    return failures;
}
