/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for sync integrity and audit features:
 *   - SHA3 UTXO checkpoint verification logic
 *   - UTXO count sanity check logic
 *   - XOR commitment save/load/compare roundtrip
 *   - bg_hash_verification_service state machine
 *   - Stall recovery window expansion */

#include "test/test_helpers.h"
#include "coins/utxo_commitment.h"
#include "chain/checkpoints.h"
#include "services/bg_validation_service.h"
#include "services/bg_hash_verification_service.h"
#include "sync/sync_planner.h"
#include "services/utxo_recovery_service.h"
#include "util/supervisor.h"
#include "validation/main_state.h"
#include <string.h>
#include <stdlib.h>
#include <sqlite3.h>

/* ── Helper: open in-memory SQLite with node_state + utxos tables ── */

static sqlite3 *open_test_db(void)
{
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    sqlite3_exec(db,
        "CREATE TABLE node_state (key TEXT PRIMARY KEY, value BLOB);"
        "CREATE TABLE utxos ("
        "  txid BLOB, vout INT, value INT, script BLOB,"
        "  script_type INT, address_hash BLOB, height INT, is_coinbase INT);",
        NULL, NULL, NULL);
    return db;
}

/* ── SHA3 UTXO checkpoint tests ──────────────────────────────────── */

static int test_integrity_sha3_checkpoint_exists(void)
{
    int failures = 0;

    TEST("integrity: SHA3 UTXO checkpoint is available") {
        const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
        ASSERT(cp != NULL);
        ASSERT(cp->height > 0);
        ASSERT(cp->utxo_count > 0);
        ASSERT(cp->total_supply > 0);

        /* SHA3 hash should not be all-zero */
        uint8_t zero[32] = {0};
        ASSERT(memcmp(cp->sha3_hash, zero, 32) != 0);
        ASSERT(memcmp(cp->block_hash, zero, 32) != 0);
        PASS();
    } _test_next:;

    return failures;
}

static int test_integrity_sha3_empty_db(void)
{
    int failures = 0;

    TEST("integrity: SHA3 of empty UTXO set is deterministic") {
        sqlite3 *db = open_test_db();
        uint8_t hash1[32], hash2[32];
        uint64_t count1 = 0, count2 = 0;

        utxo_commitment_sha3_compute(db, hash1, &count1);
        utxo_commitment_sha3_compute(db, hash2, &count2);

        ASSERT(count1 == 0);
        ASSERT(count2 == 0);
        ASSERT(memcmp(hash1, hash2, 32) == 0);

        sqlite3_close(db);
        PASS();
    } _test_next:;

    return failures;
}

static int test_integrity_sha3_deterministic_with_data(void)
{
    int failures = 0;

    TEST("integrity: SHA3 is deterministic over same UTXO set") {
        sqlite3 *db = open_test_db();

        /* Insert two UTXOs */
        uint8_t txid1[32] = {0}, txid2[32] = {0};
        txid1[0] = 0x11;
        txid2[0] = 0x22;
        sqlite3_stmt *ins = NULL;
        sqlite3_prepare_v2(db,
            "INSERT INTO utxos (txid, vout, value, script, script_type,"
            " address_hash, height, is_coinbase)"
            " VALUES (?, ?, ?, X'76A914', 1, X'00', ?, 0)",
            -1, &ins, NULL);

        sqlite3_bind_blob(ins, 1, txid1, 32, SQLITE_STATIC);
        sqlite3_bind_int(ins, 2, 0);
        sqlite3_bind_int64(ins, 3, 50000000);
        sqlite3_bind_int(ins, 4, 100);
        sqlite3_step(ins);
        sqlite3_reset(ins);

        sqlite3_bind_blob(ins, 1, txid2, 32, SQLITE_STATIC);
        sqlite3_bind_int(ins, 2, 1);
        sqlite3_bind_int64(ins, 3, 25000000);
        sqlite3_bind_int(ins, 4, 200);
        sqlite3_step(ins);
        sqlite3_finalize(ins);

        uint8_t hash1[32], hash2[32];
        uint64_t count1 = 0, count2 = 0;
        utxo_commitment_sha3_compute(db, hash1, &count1);
        utxo_commitment_sha3_compute(db, hash2, &count2);

        ASSERT(count1 == 2);
        ASSERT(count2 == 2);
        ASSERT(memcmp(hash1, hash2, 32) == 0);

        /* Different from empty set */
        sqlite3 *db2 = open_test_db();
        uint8_t empty_hash[32];
        uint64_t empty_count = 0;
        utxo_commitment_sha3_compute(db2, empty_hash, &empty_count);
        ASSERT(memcmp(hash1, empty_hash, 32) != 0);

        sqlite3_close(db2);
        sqlite3_close(db);
        PASS();
    } _test_next:;

    return failures;
}

static int test_integrity_sha3_save_load_roundtrip(void)
{
    int failures = 0;

    TEST("integrity: SHA3 save/load roundtrip preserves data") {
        sqlite3 *db = open_test_db();
        uint8_t hash[32];
        memset(hash, 0xAB, 32);

        ASSERT(utxo_commitment_sha3_save(db, hash, 3056758, 1350000));

        uint8_t loaded[32] = {0};
        int32_t height = 0;
        uint64_t count = 0;
        ASSERT(utxo_commitment_sha3_load(db, loaded, &height, &count));
        ASSERT(memcmp(hash, loaded, 32) == 0);
        ASSERT(height == 3056758);
        ASSERT(count == 1350000);

        sqlite3_close(db);
        PASS();
    } _test_next:;

    return failures;
}

/* ── XOR commitment save/load/compare tests ──────────────────────── */

static int test_integrity_xor_save_load_roundtrip(void)
{
    int failures = 0;

    TEST("integrity: XOR commitment save/load roundtrip") {
        sqlite3 *db = open_test_db();

        struct utxo_commitment uc;
        utxo_commitment_init(&uc);
        uint8_t txid[32];
        memset(txid, 0x77, 32);
        utxo_commitment_add(&uc, txid, 3, 42000000, 999);

        ASSERT(utxo_commitment_save_checkpoint(db, &uc));

        struct utxo_commitment loaded;
        ASSERT(utxo_commitment_load_checkpoint(db, &loaded));
        ASSERT(utxo_commitment_equal(&uc, &loaded));

        sqlite3_close(db);
        PASS();
    } _test_next:;

    return failures;
}

static int test_integrity_xor_load_missing(void)
{
    int failures = 0;

    TEST("integrity: XOR load returns false when no checkpoint saved") {
        sqlite3 *db = open_test_db();

        struct utxo_commitment uc;
        ASSERT(!utxo_commitment_load_checkpoint(db, &uc));

        sqlite3_close(db);
        PASS();
    } _test_next:;

    return failures;
}

static int test_integrity_xor_verify_db_match(void)
{
    int failures = 0;

    TEST("integrity: XOR verify_db matches compute_db for same data") {
        sqlite3 *db = open_test_db();

        /* Insert a UTXO */
        sqlite3_exec(db,
            "INSERT INTO utxos (txid, vout, value, script, script_type,"
            " address_hash, height, is_coinbase)"
            " VALUES (X'DEAD000000000000000000000000000000000000000000000000000000000000',"
            " 0, 100000, X'76A914', 1, X'00', 50, 0)",
            NULL, NULL, NULL);

        struct utxo_commitment computed;
        utxo_commitment_compute_db(db, &computed);
        ASSERT(computed.count > 0);

        /* Verify should pass against itself */
        ASSERT(utxo_commitment_verify_db(db, &computed));

        /* Verify should fail against a wrong commitment */
        struct utxo_commitment wrong;
        utxo_commitment_init(&wrong);
        ASSERT(!utxo_commitment_verify_db(db, &wrong));

        sqlite3_close(db);
        PASS();
    } _test_next:;

    return failures;
}

/* ── UTXO count sanity check tests ───────────────────────────────── */

static int test_integrity_utxo_count_check(void)
{
    int failures = 0;

    TEST("integrity: UTXO count sanity check logic") {
        const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
        if (!cp) { PASS(); goto _test_next; }

        /* Simulate count within 10% — should be OK */
        uint64_t good_count = cp->utxo_count;
        double ratio = (double)good_count / (double)cp->utxo_count;
        ASSERT(ratio >= 0.9 && ratio <= 1.1);

        /* Count off by 20% — should trigger warning */
        uint64_t warn_count = (uint64_t)(cp->utxo_count * 0.8);
        ratio = (double)warn_count / (double)cp->utxo_count;
        ASSERT(ratio < 0.9);
        struct utxo_count_check_result warn =
            utxo_recovery_classify_count_check(
                cp->height, cp->height, cp->utxo_count, warn_count);
        ASSERT(warn.level == UTXO_COUNT_CHECK_WARNING);

        /* Count off by 60% — should trigger critical */
        uint64_t crit_count = (uint64_t)(cp->utxo_count * 0.4);
        ratio = (double)crit_count / (double)cp->utxo_count;
        ASSERT(ratio < 0.5);
        struct utxo_count_check_result crit =
            utxo_recovery_classify_count_check(
                cp->height, cp->height, cp->utxo_count, crit_count);
        ASSERT(crit.level == UTXO_COUNT_CHECK_CRITICAL);

        /* Far past the checkpoint, current UTXO count can legitimately
         * drift; the operator needs a stale-reference diagnostic, not a
         * corruption warning. */
        struct utxo_count_check_result stale =
            utxo_recovery_classify_count_check(
                cp->height + 10000, cp->height, cp->utxo_count, warn_count);
        ASSERT(stale.level == UTXO_COUNT_CHECK_INFO_STALE_REFERENCE);

        PASS();
    } _test_next:;

    return failures;
}

/* (wave-2 deletion) test_integrity_xor_mismatch_policy removed with the
 * hardcoded-false predicate utxo_recovery_xor_mismatch_is_corruption_candidate
 * — the XOR-mismatch path now unconditionally refreshes the stale checkpoint. */

/* ── bg_hash_verification_service tests ──────────────────────────── */

static int test_integrity_bg_hash_verify_init(void)
{
    int failures = 0;

    TEST("integrity: bg_hash_verify initializes to idle state") {
        struct bg_hash_verification_service svc;
        bg_hash_verify_init(&svc, NULL, NULL, "/tmp", NULL);

        ASSERT(!svc.thread_started);
        ASSERT(!atomic_load(&svc.stop_requested));

        struct bg_hash_verify_progress p = bg_hash_verify_get_progress(&svc);
        ASSERT(p.state == BG_HASH_VERIFY_IDLE);
        ASSERT(p.verified_height == 0);
        ASSERT(p.chain_height == 0);
        ASSERT(p.mismatches == 0);
        PASS();
    } _test_next:;

    return failures;
}

static int test_integrity_bg_hash_verify_state_names(void)
{
    int failures = 0;

    TEST("integrity: bg_hash_verify state names are correct") {
        ASSERT_STR_EQ(bg_hash_verify_state_name(BG_HASH_VERIFY_IDLE), "idle");
        ASSERT_STR_EQ(bg_hash_verify_state_name(BG_HASH_VERIFY_RUNNING), "running");
        ASSERT_STR_EQ(bg_hash_verify_state_name(BG_HASH_VERIFY_COMPLETE), "complete");
        ASSERT_STR_EQ(bg_hash_verify_state_name(BG_HASH_VERIFY_FAILED), "failed");
        ASSERT_STR_EQ(bg_hash_verify_state_name(99), "unknown");
        PASS();
    } _test_next:;

    return failures;
}

static int test_integrity_bg_hash_verify_no_start_without_ms(void)
{
    int failures = 0;

    TEST("integrity: bg_hash_verify refuses to start without main_state") {
        struct bg_hash_verification_service svc;
        bg_hash_verify_init(&svc, NULL, NULL, "/tmp", NULL);

        ASSERT(!bg_hash_verify_start(&svc).ok);
        ASSERT(!svc.thread_started);
        PASS();
    } _test_next:;

    return failures;
}

static int test_integrity_bg_hash_verify_supervisor_contract(void)
{
    int failures = 0;

    TEST("integrity: bg_hash_verify registers a chain supervisor contract") {
        bool ok = true;
        supervisor_reset_for_testing();

        struct main_state ms;
        main_state_init(&ms);
        struct bg_hash_verification_service svc;
        bg_hash_verify_init(&svc, &ms, NULL, "/tmp", NULL);

        struct zcl_result r = bg_hash_verify_start(&svc);
        ok = ok && r.ok;
        if (r.ok) {
            struct supervisor_snapshot snaps[SUPERVISOR_CAP];
            int n = supervisor_snapshot_all(snaps, SUPERVISOR_CAP);
            const struct supervisor_snapshot *hash = NULL;
            for (int i = 0; i < n; i++) {
                if (strcmp(snaps[i].name, "chain.bg_hash_verify") == 0) {
                    hash = &snaps[i];
                    break;
                }
            }
            ok = ok && hash != NULL;
            if (hash)
                ok = ok && hash->period_secs == 0;
            bg_hash_verify_stop(&svc);
            ok = ok && supervisor_child_count_total() == 0;
        }

        main_state_free(&ms);
        supervisor_reset_for_testing();
        ASSERT(ok);
        PASS();
    } _test_next:;

    return failures;
}

static int test_integrity_bg_validation_supervisor_contract(void)
{
    int failures = 0;

    TEST("integrity: bg_validation registers a chain supervisor contract") {
        bool ok = true;
        supervisor_reset_for_testing();

        struct main_state ms;
        main_state_init(&ms);
        struct bg_validation_service svc;
        bg_validation_init(&svc, &ms, NULL, "/tmp", NULL);

        ok = ok && bg_validation_start(&svc);
        if (ok) {
            struct supervisor_snapshot snaps[SUPERVISOR_CAP];
            int n = supervisor_snapshot_all(snaps, SUPERVISOR_CAP);
            const struct supervisor_snapshot *valid = NULL;
            for (int i = 0; i < n; i++) {
                if (strcmp(snaps[i].name, "chain.bg_validation") == 0) {
                    valid = &snaps[i];
                    break;
                }
            }
            ok = ok && valid != NULL;
            if (valid)
                ok = ok && valid->period_secs == 0;
            bg_validation_stop(&svc);
            ok = ok && supervisor_child_count_total() == 0;
        }

        main_state_free(&ms);
        supervisor_reset_for_testing();
        ASSERT(ok);
        PASS();
    } _test_next:;

    return failures;
}

/* ── Stall recovery window expansion tests ───────────────────────── */

static int test_integrity_stall_recovery_plan(void)
{
    int failures = 0;

    TEST("integrity: stall recovery plans getheaders action") {
        struct sync_getheaders_action action;
        struct sync_stall_recovery recovery;

        memset(&action, 0, sizeof(action));
        memset(&recovery, 0, sizeof(recovery));

        /* No recovery → no action */
        syncsvc_plan_recovery_getheaders(&action, &recovery, NULL);
        ASSERT(!action.should_send);

        /* Active recovery → should send */
        recovery.should_recover = true;
        syncsvc_plan_recovery_getheaders(&action, &recovery, NULL);
        ASSERT(action.should_send);
        ASSERT(action.anchor == SYNC_HEADER_REQUEST_TIP);

        /* With tip parent request */
        struct block_index tip, parent;
        memset(&tip, 0, sizeof(tip));
        memset(&parent, 0, sizeof(parent));
        tip.pprev = &parent;
        recovery.should_request_tip_parent = true;
        syncsvc_plan_recovery_getheaders(&action, &recovery, &tip);
        ASSERT(action.should_send);
        ASSERT(action.anchor == SYNC_HEADER_REQUEST_TIP_PARENT);
        PASS();
    } _test_next:;

    return failures;
}

static int test_integrity_stall_recovery_anchor(void)
{
    int failures = 0;

    TEST("integrity: stall recovery anchor selection") {
        struct sync_stall_recovery recovery;
        memset(&recovery, 0, sizeof(recovery));

        /* Not recovering → TIP */
        ASSERT(syncsvc_recovery_header_anchor(&recovery, NULL) ==
               SYNC_HEADER_REQUEST_TIP);

        /* Recovering without tip parent → TIP */
        recovery.should_recover = true;
        ASSERT(syncsvc_recovery_header_anchor(&recovery, NULL) ==
               SYNC_HEADER_REQUEST_TIP);

        /* Recovering with tip parent → TIP_PARENT */
        struct block_index tip, parent;
        memset(&tip, 0, sizeof(tip));
        memset(&parent, 0, sizeof(parent));
        tip.pprev = &parent;
        recovery.should_request_tip_parent = true;
        ASSERT(syncsvc_recovery_header_anchor(&recovery, &tip) ==
               SYNC_HEADER_REQUEST_TIP_PARENT);
        PASS();
    } _test_next:;

    return failures;
}

static int test_integrity_valid_block_at_tip(void)
{
    int failures = 0;

    TEST("integrity: valid block triggers AT_TIP when headers caught up") {
        struct sync_block_acceptance result;
        struct p2p_node node;

        memset(&node, 0, sizeof(node));
        node.starting_height = 1000;
        node.state = PEER_SYNCING_BLOCKS;

        /* Not yet at peer tip */
        syncsvc_note_valid_block(&result, &node, SYNC_BLOCKS_DOWNLOAD,
                                 999, 1500, 0, 0);
        ASSERT(!result.reached_peer_tip);

        /* At peer tip, headers caught up */
        syncsvc_note_valid_block(&result, &node, SYNC_BLOCKS_DOWNLOAD,
                                 1000, 1001, 0, 0);
        ASSERT(result.reached_peer_tip);
        ASSERT(result.should_set_sync_state);
        ASSERT(result.next_sync_state == SYNC_AT_TIP);
        ASSERT(result.should_update_peer_state);
        ASSERT(result.next_peer_state == PEER_ACTIVE);
        PASS();
    } _test_next:;

    return failures;
}

static int test_integrity_progress_snapshot(void)
{
    int failures = 0;

    TEST("integrity: progress snapshot collects sync metrics") {
        struct sync_progress_snapshot snap;

        syncsvc_collect_progress(&snap, NULL, SYNC_BLOCKS_DOWNLOAD,
                                 500, 1000, 0, 0);
        ASSERT(snap.sync_state == SYNC_BLOCKS_DOWNLOAD);
        ASSERT(snap.chain_height == 500);
        ASSERT(snap.header_height == 1000);
        ASSERT(snap.should_log_progress);

        /* AT_TIP should not log progress */
        syncsvc_collect_progress(&snap, NULL, SYNC_AT_TIP,
                                 1000, 1000, 0, 0);
        ASSERT(!snap.should_log_progress);

        /* AT_TIP with stale tip */
        syncsvc_collect_progress(&snap, NULL, SYNC_AT_TIP,
                                 1000, 1000, 100, 800);
        ASSERT(snap.tip_stale);
        ASSERT(snap.tip_stale_seconds == 700);
        PASS();
    } _test_next:;

    return failures;
}

/* ── Registration ─────────────────────────────────────────────────── */

int test_integrity(void)
{
    int failures = 0;

    /* SHA3 UTXO checkpoint verification */
    failures += test_integrity_sha3_checkpoint_exists();
    failures += test_integrity_sha3_empty_db();
    failures += test_integrity_sha3_deterministic_with_data();
    failures += test_integrity_sha3_save_load_roundtrip();

    /* XOR commitment persistence */
    failures += test_integrity_xor_save_load_roundtrip();
    failures += test_integrity_xor_load_missing();
    failures += test_integrity_xor_verify_db_match();

    /* UTXO count sanity check */
    failures += test_integrity_utxo_count_check();

    /* bg_hash_verification_service */
    failures += test_integrity_bg_hash_verify_init();
    failures += test_integrity_bg_hash_verify_state_names();
    failures += test_integrity_bg_hash_verify_no_start_without_ms();
    failures += test_integrity_bg_hash_verify_supervisor_contract();
    failures += test_integrity_bg_validation_supervisor_contract();

    /* Stall recovery and sync integrity */
    failures += test_integrity_stall_recovery_plan();
    failures += test_integrity_stall_recovery_anchor();
    failures += test_integrity_valid_block_at_tip();
    failures += test_integrity_progress_snapshot();

    return failures;
}
