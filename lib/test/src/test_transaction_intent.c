/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Durable intent idempotency, expiry, raw-tx recovery, and ciphertext proof. */

#include "test/test_core.h"

#include "controllers/vault_intent_controller.h"
#include "json/json.h"
#include "models/database.h"
#include "models/vault_intent.h"
#include "models/wallet_identity.h"
#include "models/wallet_metadata_crypto.h"
#include "platform/time_compat.h"
#include "services/overlay_transaction_intent_service.h"
#include "services/vault_intent_async_service.h"
#include "services/znam_transaction_intent_service.h"
#include "services/zslp_transaction_intent_service.h"
#include "validation/main_state.h"
#include "wallet/wallet_lock.h"

#include <sqlite3.h>
#include <stdatomic.h>
#include <string.h>

static struct node_db *g_ti_async_ndb;
static uint8_t g_ti_async_plan_id[32];
static _Atomic int g_ti_async_gate;
static _Atomic int g_ti_async_calls;

static bool ti_async_execute(const struct json_value *input,
                             struct json_value *result)
{
    (void)input;
    (void)atomic_fetch_add(&g_ti_async_calls, 1);
    while (!atomic_load(&g_ti_async_gate))
        platform_sleep_ms(1);
    uint8_t txid[32]; memset(txid, 0xa7, sizeof(txid));
    bool stored = vault_intent_set_state(
        g_ti_async_ndb, g_ti_async_plan_id,
        VAULT_INTENT_MEMPOOL_ACCEPTED, txid, "", 200);
    json_set_object(result);
    (void)json_push_kv_bool(result, "ok", stored);
    if (!stored)
        (void)json_push_kv_str(result, "code", "FIXTURE_PERSIST_FAILED");
    return true;
}

static bool ti_db_contains(struct node_db *ndb, const char *needle)
{
    sqlite3_stmt *s = NULL;
    bool found = false;
    if (sqlite3_prepare_v2(ndb->db,
        "SELECT instr(encrypted_payload,?) FROM vault_intents", -1, &s,
        NULL) == SQLITE_OK && s) {
        sqlite3_bind_text(s, 1, needle, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) // raw-sql-ok:test-readonly-probe
            found = sqlite3_column_int(s, 0) != 0;
    }
    if (s) sqlite3_finalize(s);
    return found;
}

static void ti_bound_row(struct vault_intent_row *row, uint8_t tag,
                         const struct wallet_identity_row *identity,
                         int64_t reserved_zat)
{
    memset(row, 0, sizeof(*row));
    memset(row->plan_id, tag, 32);
    memset(row->digest, tag + 1, 32);
    memset(row->anchor_hash, tag + 2, 32);
    memset(row->encrypted_payload, tag + 3, 32);
    row->encrypted_payload_len = 32;
    row->state = VAULT_INTENT_PLANNED;
    row->route = VAULT_INTENT_ROUTE_TRANSPARENT;
    row->created_at = 100;
    row->expires_at = 700;
    row->updated_at = 100;
    row->anchor_height = 42;
    row->confirm_height = -1;
    snprintf(row->wallet_scope, sizeof(row->wallet_scope), "dev");
    snprintf(row->wallet_instance_id, sizeof(row->wallet_instance_id), "%s",
             identity->wallet_instance_id);
    wallet_identity_genesis_hex(identity, row->wallet_genesis);
    memset(row->snapshot_root, tag + 4, 32);
    row->has_snapshot_root = true;
    row->recipient_value_zat = reserved_zat;
    row->max_fee_zat = 0;
    row->reserved_zat = reserved_zat;
}

struct ti_zslp_fixture {
    struct wallet_identity_row identity;
    uint8_t tip_hash[32];
    uint8_t money_root[32];
    uint8_t post_prepare_root[32];
    uint8_t input_tag;
    int prepares;
    int publishes;
};

static struct zcl_result ti_zslp_money(
    void *opaque, const char *scope, struct wallet_money_snapshot *out)
{
    struct ti_zslp_fixture *f = opaque;
    if (!f || !out || strcmp(scope, "dev") != 0)
        return ZCL_ERR(-1, "fixture scope mismatch");
    memset(out, 0, sizeof(*out));
    out->identity = f->identity;
    snprintf(out->wallet_scope, sizeof(out->wallet_scope), "dev");
    snprintf(out->status, sizeof(out->status), "CURRENT");
    snprintf(out->reason, sizeof(out->reason), "fixture current");
    out->complete = true;
    out->tip_height = 100;
    memcpy(out->tip_hash, f->tip_hash, 32);
    memcpy(out->snapshot_root, f->money_root, 32);
    out->confirmed_zat = 30000000;
    out->agent_available_zat = 5000000;
    return ZCL_OK;
}

static struct zcl_result ti_zslp_prepare(
    void *opaque, const struct zslp_intent_request *request,
    int64_t maximum_fee_zat, uint8_t *raw_tx, size_t raw_capacity,
    size_t *raw_tx_len, uint8_t txid_out[32], int64_t *actual_fee_zat,
    struct vault_intent_input *inputs, size_t input_capacity,
    size_t *input_count)
{
    struct ti_zslp_fixture *f = opaque;
    if (!f || !request || request->operation != ZSLP_INTENT_GENESIS ||
        maximum_fee_zat != 1000 || raw_capacity < 4 || input_capacity < 1)
        return ZCL_ERR(-1, "fixture prepare contract mismatch");
    const uint8_t prepared[4] = { 'Z', 'S', 'L', f->input_tag };
    memcpy(raw_tx, prepared, sizeof(prepared));
    *raw_tx_len = sizeof(prepared);
    memset(txid_out, (uint8_t)(f->input_tag + 1), 32);
    *actual_fee_zat = 500;
    memset(inputs[0].txid, f->input_tag, 32);
    inputs[0].vout = 3;
    *input_count = 1;
    memcpy(f->money_root, f->post_prepare_root, 32);
    f->prepares++;
    return ZCL_OK;
}

static struct zcl_result ti_zslp_publish(
    void *opaque, const uint8_t *raw_tx, size_t raw_tx_len,
    const uint8_t expected_txid[32])
{
    struct ti_zslp_fixture *f = opaque;
    if (!f || !raw_tx || raw_tx_len != 4 || raw_tx[0] != 'Z' ||
        raw_tx[3] != f->input_tag ||
        expected_txid[0] != (uint8_t)(f->input_tag + 1))
        return ZCL_ERR(-1, "fixture publish identity mismatch");
    f->publishes++;
    return ZCL_OK;
}

static struct zcl_result ti_znam_money(
    void *opaque, const char *scope, struct wallet_money_snapshot *out)
{
    return ti_zslp_money(opaque, scope, out);
}

static struct zcl_result ti_znam_prepare(
    void *opaque, const struct znam_intent_request *request,
    int64_t maximum_fee_zat, uint8_t *raw_tx, size_t raw_capacity,
    size_t *raw_tx_len, uint8_t txid_out[32], int64_t *actual_fee_zat,
    struct vault_intent_input *inputs, size_t input_capacity,
    size_t *input_count)
{
    struct ti_zslp_fixture *f = opaque;
    if (!f || !request || request->operation != ZNAM_INTENT_REGISTER ||
        maximum_fee_zat != 1000 || raw_capacity < 4 || input_capacity < 1)
        return ZCL_ERR(-1, "ZNAM fixture prepare contract mismatch");
    const uint8_t prepared[4] = { 'Z', 'N', 'A', f->input_tag };
    memcpy(raw_tx, prepared, sizeof(prepared));
    *raw_tx_len = sizeof(prepared);
    memset(txid_out, (uint8_t)(f->input_tag + 1), 32);
    *actual_fee_zat = 500;
    memset(inputs[0].txid, f->input_tag, 32);
    inputs[0].vout = 7;
    *input_count = 1;
    memcpy(f->money_root, f->post_prepare_root, 32);
    f->prepares++;
    return ZCL_OK;
}

static struct zcl_result ti_znam_publish(
    void *opaque, const uint8_t *raw_tx, size_t raw_tx_len,
    const uint8_t expected_txid[32])
{
    struct ti_zslp_fixture *f = opaque;
    if (!f || !raw_tx || raw_tx_len != 4 || raw_tx[0] != 'Z' ||
        raw_tx[1] != 'N' || raw_tx[3] != f->input_tag ||
        expected_txid[0] != (uint8_t)(f->input_tag + 1))
        return ZCL_ERR(-1, "ZNAM fixture publish identity mismatch");
    f->publishes++;
    return ZCL_OK;
}

static struct zcl_result ti_overlay_prepare(
    void *opaque, const struct overlay_intent_request *request,
    int64_t maximum_fee_zat, uint8_t *raw_tx, size_t raw_capacity,
    size_t *raw_tx_len, uint8_t txid_out[32], int64_t *actual_fee_zat,
    struct vault_intent_input *inputs, size_t input_capacity,
    size_t *input_count)
{
    struct ti_zslp_fixture *f = opaque;
    if (!f || !request || strcmp(request->application_kind, "zid_intent") != 0 ||
        strcmp(request->operation, "anchor") != 0 ||
        maximum_fee_zat != 1000 || raw_capacity < 4 || input_capacity < 1)
        return ZCL_ERR(-1, "overlay fixture prepare contract mismatch");
    const uint8_t prepared[4] = { 'O', 'T', 'I', f->input_tag };
    memcpy(raw_tx, prepared, sizeof(prepared));
    *raw_tx_len = sizeof(prepared);
    memset(txid_out, (uint8_t)(f->input_tag + 1), 32);
    *actual_fee_zat = 500;
    memset(inputs[0].txid, f->input_tag, 32);
    inputs[0].vout = 9;
    *input_count = 1;
    memcpy(f->money_root, f->post_prepare_root, 32);
    f->prepares++;
    return ZCL_OK;
}

static struct zcl_result ti_overlay_publish(
    void *opaque, const uint8_t *raw_tx, size_t raw_tx_len,
    const uint8_t expected_txid[32])
{
    struct ti_zslp_fixture *f = opaque;
    if (!f || !raw_tx || raw_tx_len != 4 || raw_tx[0] != 'O' ||
        raw_tx[2] != 'I' || raw_tx[3] != f->input_tag ||
        expected_txid[0] != (uint8_t)(f->input_tag + 1))
        return ZCL_ERR(-1, "overlay fixture publish identity mismatch");
    f->publishes++;
    return ZCL_OK;
}

int test_transaction_intent(void)
{
    int failures = 0;
    struct node_db ndb; memset(&ndb, 0, sizeof(ndb));

    TEST("vault intent receipts render chain hashes in canonical display order") {
        struct vault_intent_row row;
        memset(&row, 0, sizeof(row));
        row.state = VAULT_INTENT_CONFIRMED;
        row.confirm_height = 42;
        row.has_txid = true;
        row.has_confirm_hash = true;
        for (size_t i = 0; i < 32; i++) {
            row.txid[i] = (uint8_t)i;
            row.confirm_hash[i] = (uint8_t)(0xa0 + i);
        }
        struct json_value rendered;
        json_init(&rendered);
        json_set_object(&rendered);
        vault_intent_render_row(NULL, &rendered, &row);
        ASSERT_STR_EQ(json_get_str(json_get(&rendered, "txid")),
            "1f1e1d1c1b1a19181716151413121110"
            "0f0e0d0c0b0a09080706050403020100");
        ASSERT_STR_EQ(json_get_str(json_get(&rendered,
            "confirmed_block_hash")),
            "bfbebdbcbbbab9b8b7b6b5b4b3b2b1b0"
            "afaeadacabaaa9a8a7a6a5a4a3a2a1a0");
        json_free(&rendered);

        /* Zero-initialized creation rows historically rendered height zero
         * and tip+1 confirmations while still only mempool-accepted. State is
         * the authority: no confirmed fields exist before CONFIRMED. */
        memset(&row, 0, sizeof(row));
        row.state = VAULT_INTENT_MEMPOOL_ACCEPTED;
        row.has_txid = true;
        json_init(&rendered);
        json_set_object(&rendered);
        vault_intent_render_row(NULL, &rendered, &row);
        ASSERT_EQ(json_get_int(json_get(&rendered, "confirmations")), 0);
        ASSERT(json_get(&rendered, "confirmed_height") == NULL);
        ASSERT(json_get(&rendered, "confirmed_block_hash") == NULL);
        json_free(&rendered);
        PASS();
    }

    TEST("vault intent confirmations derive from canonical block height") {
        struct main_state ms;
        main_state_init(&ms);
        struct block_index blocks[8];
        struct uint256 hashes[8];
        memset(blocks, 0, sizeof(blocks));
        memset(hashes, 0, sizeof(hashes));
        bool built = true;
        for (int i = 0; i < 8; i++) {
            hashes[i].data[0] = (uint8_t)(i + 1);
            blocks[i].nHeight = i;
            blocks[i].pprev = i > 0 ? &blocks[i - 1] : NULL;
            built = built && block_map_insert(&ms.map_block_index,
                                               &hashes[i], &blocks[i]);
        }
        built = built && active_chain_move_window_tip(&ms.chain_active,
                                                       &blocks[7]);
        ASSERT(built);

        int32_t height = -1;
        int32_t confirmations = 0;
        ASSERT(vault_intent_chain_confirmation(&ms, hashes[2].data,
                                                &height, &confirmations));
        ASSERT_EQ(height, 2);
        ASSERT_EQ(confirmations, 6);

        struct block_index fork;
        struct uint256 fork_hash;
        memset(&fork, 0, sizeof(fork));
        memset(&fork_hash, 0, sizeof(fork_hash));
        fork_hash.data[0] = 0xf2;
        fork.nHeight = 2;
        fork.pprev = &blocks[1];
        ASSERT(block_map_insert(&ms.map_block_index, &fork_hash, &fork));
        ASSERT(!vault_intent_chain_confirmation(&ms, fork_hash.data,
                                                 &height, &confirmations));
        main_state_free(&ms);
        PASS();
    }

    TEST("transaction intent amount grammar is exact and range bounded") {
        int64_t amount = 0;
        ASSERT(vault_intent_parse_zcl_amount("0.00000001", &amount));
        ASSERT_EQ(amount, 1);
        ASSERT(vault_intent_parse_zcl_amount("21000000.00000000", &amount));
        ASSERT_EQ(amount, 2100000000000000LL);
        ASSERT(!vault_intent_parse_zcl_amount("21000000.00000001", &amount));
        ASSERT(!vault_intent_parse_zcl_amount("1.000000001", &amount));
        ASSERT(!vault_intent_parse_zcl_amount("1.", &amount));
        ASSERT(!vault_intent_parse_zcl_amount("1e-8", &amount));
        ASSERT(!vault_intent_parse_zcl_amount("92233720368", &amount));
        PASS();
    }

    TEST("transaction intent idempotency keys are printable and bounded") {
        char key[VAULT_INTENT_IDEMPOTENCY_MAX + 2];
        memset(key, 'k', sizeof(key));
        key[VAULT_INTENT_IDEMPOTENCY_MAX] = '\0';
        ASSERT(vault_intent_idempotency_key_valid("payment-001"));
        ASSERT(vault_intent_idempotency_key_valid(key));
        key[VAULT_INTENT_IDEMPOTENCY_MAX] = 'k';
        key[VAULT_INTENT_IDEMPOTENCY_MAX + 1] = '\0';
        ASSERT(!vault_intent_idempotency_key_valid(key));
        ASSERT(!vault_intent_idempotency_key_valid(""));
        ASSERT(!vault_intent_idempotency_key_valid("line\nbreak"));
        PASS();
    }

    TEST("exact intent inputs are atomically exclusive until terminal") {
        struct node_db exact_db; memset(&exact_db, 0, sizeof(exact_db));
        ASSERT(node_db_open(&exact_db, ":memory:"));
        struct wallet_identity_row identity;
        const uint8_t genesis[32] = { 0x92 };
        ASSERT(wallet_identity_ensure(&exact_db, genesis, "prod", &identity));

        struct vault_intent_row first, second, parallel;
        ti_bound_row(&first, 0x31, &identity, 1);
        ti_bound_row(&second, 0x41, &identity, 1);
        ti_bound_row(&parallel, 0x51, &identity, 1);
        (void)snprintf(first.wallet_scope, sizeof(first.wallet_scope), "prod");
        (void)snprintf(second.wallet_scope, sizeof(second.wallet_scope), "prod");
        (void)snprintf(parallel.wallet_scope, sizeof(parallel.wallet_scope),
                       "prod");
        struct vault_intent_input claimed = { .vout = 7 };
        memset(claimed.txid, 0xa1, sizeof(claimed.txid));
        const uint8_t raw[] = { 1, 2, 3 };
        ASSERT(vault_intent_reserve_with_raw_inputs(
            &exact_db, &first, 100, raw, sizeof(raw), &claimed, 1));
        ASSERT(!vault_intent_reserve_with_raw_inputs(
            &exact_db, &second, 100, raw, sizeof(raw), &claimed, 1));

        struct vault_intent_input other = claimed;
        other.vout = 8;
        ASSERT(vault_intent_reserve_with_raw_inputs(
            &exact_db, &parallel, 100, raw, sizeof(raw), &other, 1));
        ASSERT(vault_intent_set_state(&exact_db, first.plan_id,
            VAULT_INTENT_FAILED, NULL, "TEST_RELEASE", 101));
        ASSERT(vault_intent_reserve_with_raw_inputs(
            &exact_db, &second, 100, raw, sizeof(raw), &claimed, 1));
        node_db_close(&exact_db);
        PASS();
    }

    TEST("async queue marker and owner cancellation are atomic") {
        struct node_db async_db; memset(&async_db, 0, sizeof(async_db));
        ASSERT(node_db_open(&async_db, ":memory:"));
        struct wallet_identity_row identity;
        const uint8_t genesis[32] = { 0x93 };
        ASSERT(wallet_identity_ensure(&async_db, genesis, "prod", &identity));

        struct vault_intent_row row;
        ti_bound_row(&row, 0x61, &identity, 1);
        (void)snprintf(row.wallet_scope, sizeof(row.wallet_scope), "prod");
        ASSERT(vault_intent_save(&async_db, &row));
        ASSERT(vault_intent_record_planned_error(
            &async_db, row.plan_id, "ASYNC_QUEUED", 101));
        struct vault_intent_row got;
        ASSERT(vault_intent_find(&async_db, row.plan_id, &got));
        ASSERT_EQ(got.state, VAULT_INTENT_PLANNED);
        ASSERT(strcmp(got.error_code, "ASYNC_QUEUED") == 0);
        ASSERT(vault_intent_cancel_planned(&async_db, row.plan_id, 102));
        ASSERT(!vault_intent_cancel_planned(&async_db, row.plan_id, 103));
        ASSERT(!vault_intent_record_planned_error(
            &async_db, row.plan_id, "LATE_WORKER", 104));
        ASSERT(vault_intent_find(&async_db, row.plan_id, &got));
        ASSERT_EQ(got.state, VAULT_INTENT_FAILED);
        ASSERT(strcmp(got.error_code, "CANCELLED_BY_OWNER") == 0);
        ASSERT_EQ(vault_intent_reserved_total(
            &async_db, "prod", identity.wallet_instance_id), 0);
        node_db_close(&async_db);
        PASS();
    }

    TEST("async intent execution returns immediately and deduplicates") {
        struct node_db async_db; memset(&async_db, 0, sizeof(async_db));
        ASSERT(node_db_open(&async_db, ":memory:"));
        struct wallet_identity_row identity;
        const uint8_t genesis[32] = { 0x94 };
        ASSERT(wallet_identity_ensure(&async_db, genesis, "prod", &identity));
        struct vault_intent_row row;
        ti_bound_row(&row, 0x71, &identity, 1);
        (void)snprintf(row.wallet_scope, sizeof(row.wallet_scope), "prod");
        ASSERT(vault_intent_save(&async_db, &row));

        char plan_hex[65];
        for (size_t i = 0; i < 32; i++)
            (void)snprintf(plan_hex + i * 2, 3, "%02x", row.plan_id[i]);
        g_ti_async_ndb = &async_db;
        memcpy(g_ti_async_plan_id, row.plan_id, sizeof(g_ti_async_plan_id));
        atomic_store(&g_ti_async_gate, 0);
        atomic_store(&g_ti_async_calls, 0);
        bool duplicate = false;
        ASSERT(vault_intent_async_start(
            &async_db, &row, plan_hex, true, ti_async_execute,
            &duplicate).ok);
        ASSERT(!duplicate);
        ASSERT(vault_intent_async_start(
            &async_db, &row, plan_hex, true, ti_async_execute,
            &duplicate).ok);
        ASSERT(duplicate);
        struct vault_intent_row queued;
        ASSERT(vault_intent_find(&async_db, row.plan_id, &queued));
        ASSERT(strcmp(queued.error_code, "ASYNC_QUEUED") == 0);
        atomic_store(&g_ti_async_gate, 1);
        for (int i = 0; i < 100 && atomic_load(&g_ti_async_calls) < 1; i++)
            platform_sleep_ms(5);
        for (int i = 0; i < 100; i++) {
            ASSERT(vault_intent_find(&async_db, row.plan_id, &queued));
            if (queued.state == VAULT_INTENT_MEMPOOL_ACCEPTED)
                break;
            platform_sleep_ms(5);
        }
        ASSERT_EQ(atomic_load(&g_ti_async_calls), 1);
        ASSERT_EQ(queued.state, VAULT_INTENT_MEMPOOL_ACCEPTED);
        ASSERT(vault_intent_async_recover(&async_db, ti_async_execute).ok);
        platform_sleep_ms(10);
        ASSERT_EQ(atomic_load(&g_ti_async_calls), 1);
        g_ti_async_ndb = NULL;
        memset(g_ti_async_plan_id, 0, sizeof(g_ti_async_plan_id));
        node_db_close(&async_db);
        PASS();
    }

    TEST("transaction intent is encrypted, claim-once, recoverable, idempotent") {
        ASSERT(node_db_open(&ndb, ":memory:"));
        wallet_lock_reset_for_test();
        wallet_lock_note_encrypted_at_rest();
        ASSERT(wallet_lock_unlock(NULL, NULL, "intent-test-pass").ok);

        struct vault_intent_row row; memset(&row, 0, sizeof(row));
        memset(row.plan_id, 1, 32); memset(row.digest, 2, 32);
        memset(row.anchor_hash, 3, 32);
        row.state = VAULT_INTENT_PLANNED;
        row.route = VAULT_INTENT_ROUTE_TRANSPARENT;
        row.created_at = 100; row.expires_at = 700; row.updated_at = 100;
        row.anchor_height = 42; row.confirm_height = -1;
        const uint8_t payload[] = "t1-private-recipient|0.01000000";
        ASSERT(wallet_metadata_encrypt(&ndb, row.plan_id, 32, payload,
            sizeof(payload) - 1, row.encrypted_payload,
            sizeof(row.encrypted_payload), &row.encrypted_payload_len));
        ASSERT(vault_intent_save(&ndb, &row));
        ASSERT(!ti_db_contains(&ndb, "t1-private-recipient"));

        struct vault_intent_row got;
        ASSERT(vault_intent_find(&ndb, row.plan_id, &got));
        ASSERT_EQ(got.state, VAULT_INTENT_PLANNED);
        ASSERT(vault_intent_claim_commit(&ndb, row.plan_id, 200));
        ASSERT(!vault_intent_claim_commit(&ndb, row.plan_id, 200));
        ASSERT(!vault_intent_has_raw(&ndb, row.plan_id));
        ASSERT(!vault_intent_reclaim_proving(&ndb, row.plan_id, 199, 499));
        ASSERT(vault_intent_reclaim_proving(&ndb, row.plan_id, 200, 500));
        ASSERT(vault_intent_claim_commit(&ndb, row.plan_id, 500));
        ASSERT(vault_intent_find(&ndb, row.plan_id, &got));
        ASSERT_EQ(got.state, VAULT_INTENT_PROVING);

        const uint8_t raw[] = {1, 2, 3, 4, 5};
        uint8_t loaded[16]; size_t loaded_len = 0;
        ASSERT(vault_intent_store_raw(&ndb, row.plan_id, raw, sizeof(raw)));
        ASSERT(vault_intent_has_raw(&ndb, row.plan_id));
        ASSERT(vault_intent_load_raw(&ndb, row.plan_id, loaded,
                                     sizeof(loaded), &loaded_len));
        ASSERT_EQ(loaded_len, sizeof(raw));
        ASSERT(memcmp(loaded, raw, sizeof(raw)) == 0);
        uint8_t txid[32]; memset(txid, 9, sizeof(txid));
        ASSERT(vault_intent_set_state(&ndb, row.plan_id,
            VAULT_INTENT_MEMPOOL_ACCEPTED, txid, "", 501));
        ASSERT(vault_intent_find(&ndb, row.plan_id, &got));
        ASSERT(got.has_txid && memcmp(got.txid, txid, 32) == 0);
        ASSERT(strcmp(vault_intent_state_name(got.state),
                      "mempool_accepted") == 0);
        uint8_t block_hash[32]; memset(block_hash, 8, sizeof(block_hash));
        ASSERT(vault_intent_set_confirmation(&ndb, row.plan_id,
            VAULT_INTENT_CONFIRMED, 500, block_hash, 502));
        ASSERT(vault_intent_find(&ndb, row.plan_id, &got));
        ASSERT_EQ(got.state, VAULT_INTENT_CONFIRMED);
        ASSERT_EQ(got.confirm_height, 500);
        ASSERT(got.has_confirm_hash &&
               memcmp(got.confirm_hash, block_hash, 32) == 0);
        ASSERT(vault_intent_set_confirmation(&ndb, row.plan_id,
            VAULT_INTENT_FINALIZED, 500, block_hash, 503));
        ASSERT(vault_intent_set_state(&ndb, row.plan_id,
            VAULT_INTENT_REORGED, txid, "CONFIRMATION_REORGED", 504));
        ASSERT(vault_intent_find(&ndb, row.plan_id, &got));
        ASSERT_EQ(got.state, VAULT_INTENT_REORGED);
        ASSERT(strcmp(got.error_code, "CONFIRMATION_REORGED") == 0);
        PASS();
    }

    TEST("transaction intent expiry only advances planned rows") {
        struct vault_intent_row row; memset(&row, 0, sizeof(row));
        memset(row.plan_id, 4, 32); memset(row.digest, 5, 32);
        memset(row.anchor_hash, 6, 32);
        row.state = VAULT_INTENT_PLANNED;
        row.route = VAULT_INTENT_ROUTE_TRANSPARENT;
        row.created_at = 100; row.expires_at = 110; row.updated_at = 100;
        row.anchor_height = 1; row.confirm_height = -1;
        const uint8_t payload[] = "expiry-payload";
        ASSERT(wallet_metadata_encrypt(&ndb, row.plan_id, 32, payload,
            sizeof(payload) - 1, row.encrypted_payload,
            sizeof(row.encrypted_payload), &row.encrypted_payload_len));
        ASSERT(vault_intent_save(&ndb, &row));
        ASSERT(vault_intent_expire_due(&ndb, 110));
        struct vault_intent_row got;
        ASSERT(vault_intent_find(&ndb, row.plan_id, &got));
        ASSERT_EQ(got.state, VAULT_INTENT_EXPIRED);
        ASSERT(strcmp(got.error_code, "PLAN_EXPIRED") == 0);
        struct vault_intent_row rows[4];
        ASSERT_EQ(vault_intent_list(&ndb, rows, 4), 2);
        PASS();
    }

    TEST("application-bound intents deduplicate without replacing exact plans") {
        struct vault_intent_row first; memset(&first, 0, sizeof(first));
        memset(first.plan_id, 0x71, 32);
        memset(first.digest, 0x72, 32);
        memset(first.anchor_hash, 0x73, 32);
        memset(first.encrypted_payload, 0x74, 32);
        first.encrypted_payload_len = 32;
        first.state = VAULT_INTENT_PLANNED;
        first.route = VAULT_INTENT_ROUTE_PRIVATE;
        first.created_at = 200;
        first.expires_at = 800;
        first.updated_at = 200;
        first.anchor_height = 43;
        first.confirm_height = -1;
        snprintf(first.application_kind, sizeof(first.application_kind),
                 "market_purchase");
        snprintf(first.idempotency_key, sizeof(first.idempotency_key),
                 "buyer-request-1");
        memset(first.request_digest, 0x75, 32);
        first.has_request_digest = true;
        ASSERT(!vault_intent_save(&ndb, &first));

        struct vault_intent_row found;
        ASSERT(vault_intent_find_application_idempotency(
            &ndb, "dev", "market_purchase", "buyer-request-1", &found) ==
            false);
        /* Legacy-empty custody fields cannot become an application plan.
         * Save it only after removing the partial application binding. */
        first.application_kind[0] = '\0';
        first.idempotency_key[0] = '\0';
        first.has_request_digest = false;
        ASSERT(vault_intent_save(&ndb, &first));

        struct wallet_identity_row app_identity;
        const uint8_t app_genesis[32] = { 0x61 };
        ASSERT(wallet_identity_ensure(&ndb, app_genesis, "dev", &app_identity));
        ti_bound_row(&first, 0x71, &app_identity, 1);
        snprintf(first.application_kind, sizeof(first.application_kind),
                 "market_purchase");
        snprintf(first.idempotency_key, sizeof(first.idempotency_key),
                 "buyer-request-1");
        memset(first.request_digest, 0x75, 32);
        first.has_request_digest = true;
        ASSERT(vault_intent_save(&ndb, &first));
        ASSERT(vault_intent_find_application_idempotency(
            &ndb, "dev", "market_purchase", "buyer-request-1", &found));
        ASSERT(memcmp(found.plan_id, first.plan_id, 32) == 0);
        ASSERT(memcmp(found.request_digest, first.request_digest, 32) == 0);

        struct vault_intent_row duplicate = first;
        memset(duplicate.plan_id, 0x81, 32);
        memset(duplicate.digest, 0x82, 32);
        ASSERT(!vault_intent_save(&ndb, &duplicate));
        ASSERT(vault_intent_find_application_idempotency(
            &ndb, "dev", "market_purchase", "buyer-request-1", &found));
        ASSERT(memcmp(found.plan_id, first.plan_id, 32) == 0);

        duplicate = first;
        duplicate.idempotency_key[0] = '\0';
        ASSERT(!vault_intent_save(&ndb, &duplicate));

        /* Fee-only overlay anchors are valid only as named idempotent
         * application plans, and their exact prepared bytes are inserted in
         * the same transaction as the reservation. */
        struct vault_intent_row fee_only;
        ti_bound_row(&fee_only, 0x91, &app_identity, 1000);
        fee_only.recipient_value_zat = 0;
        fee_only.max_fee_zat = 1000;
        snprintf(fee_only.application_kind,
                 sizeof(fee_only.application_kind), "blog_anchor");
        snprintf(fee_only.idempotency_key,
                 sizeof(fee_only.idempotency_key), "blog-anchor-1");
        memset(fee_only.request_digest, 0x92, 32);
        fee_only.has_request_digest = true;
        const uint8_t prepared_raw[] = { 0x5a, 0x42, 0x4c, 0x47, 1 };
        ASSERT(vault_intent_reserve_with_raw(
            &ndb, &fee_only, 30000000, prepared_raw,
            sizeof(prepared_raw)));
        ASSERT(vault_intent_find(&ndb, fee_only.plan_id, &found));
        ASSERT(vault_intent_has_raw(&ndb, fee_only.plan_id));
        uint8_t loaded_raw[8]; size_t loaded_raw_len = 0;
        ASSERT(vault_intent_load_raw(&ndb, fee_only.plan_id, loaded_raw,
                                     sizeof(loaded_raw), &loaded_raw_len));
        ASSERT(loaded_raw_len == sizeof(prepared_raw) &&
               memcmp(loaded_raw, prepared_raw, sizeof(prepared_raw)) == 0);
        PASS();
    }

    TEST("ZSLP intents prepare exact bytes, replay idempotently, and fail "
         "closed on snapshot drift") {
        struct node_db zdb; memset(&zdb, 0, sizeof(zdb));
        ASSERT(node_db_open(&zdb, ":memory:"));
        wallet_lock_reset_for_test();
        wallet_lock_note_encrypted_at_rest();
        ASSERT(wallet_lock_unlock(NULL, NULL, "zslp-intent-test-pass").ok);
        struct ti_zslp_fixture fixture; memset(&fixture, 0, sizeof(fixture));
        const uint8_t genesis[32] = { 0xa4 };
        ASSERT(wallet_identity_ensure(&zdb, genesis, "dev", &fixture.identity));
        memset(fixture.tip_hash, 0xb1, sizeof(fixture.tip_hash));
        memset(fixture.money_root, 0xc1, sizeof(fixture.money_root));
        memset(fixture.post_prepare_root, 0xc2,
               sizeof(fixture.post_prepare_root));
        fixture.input_tag = 0xd1;
        struct zslp_intent_runtime runtime = {
            .node_db = &zdb,
            .read_money = ti_zslp_money, .money_ctx = &fixture,
            .prepare = ti_zslp_prepare, .prepare_ctx = &fixture,
            .publish = ti_zslp_publish, .publish_ctx = &fixture,
            .tip_height = 100, .maximum_fee_zat = 1000, .now_unix = 1000,
        };
        memcpy(runtime.tip_hash, fixture.tip_hash, 32);
        struct zslp_intent_request request; memset(&request, 0, sizeof(request));
        snprintf(request.wallet_scope, sizeof(request.wallet_scope), "dev");
        request.operation = ZSLP_INTENT_GENESIS;
        snprintf(request.ticker, sizeof(request.ticker), "PRIVATE");
        snprintf(request.name, sizeof(request.name), "Private Fixture");
        request.decimals = 0;
        request.supply = 1000;
        snprintf(request.idempotency_key, sizeof(request.idempotency_key),
                 "zslp-genesis-1");
        struct zslp_intent_result planned;
        ASSERT(zslp_transaction_intent_plan(
            &runtime, &request, &planned).ok);
        ASSERT_EQ(fixture.prepares, 1);
        ASSERT(!planned.broadcast);
        ASSERT_EQ(planned.actual_fee_zat, 500);
        ASSERT_EQ(planned.maximum_fee_zat, 1000);
        ASSERT_EQ(planned.reserved_zat, 1000);
        ASSERT(!ti_db_contains(&zdb, "Private Fixture"));
        ASSERT(vault_intent_has_raw(&zdb, planned.plan_id));

        struct zslp_intent_result replay;
        ASSERT(zslp_transaction_intent_plan(&runtime, &request, &replay).ok);
        ASSERT(replay.idempotent_replay);
        ASSERT(memcmp(replay.plan_id, planned.plan_id, 32) == 0);
        ASSERT_EQ(fixture.prepares, 1);
        struct zslp_intent_request changed = request;
        changed.supply++;
        ASSERT(!zslp_transaction_intent_plan(
            &runtime, &changed, &replay).ok);

        struct zslp_intent_result committed;
        ASSERT(zslp_transaction_intent_commit(
            &runtime, "dev", planned.plan_id, &committed).ok);
        ASSERT(committed.broadcast);
        ASSERT_EQ(fixture.publishes, 1);
        ASSERT(zslp_transaction_intent_commit(
            &runtime, "dev", planned.plan_id, &replay).ok);
        ASSERT(replay.idempotent_replay);
        ASSERT_EQ(fixture.publishes, 1);
        ASSERT(!zslp_transaction_intent_commit(
            &runtime, "prod", planned.plan_id, &replay).ok);

        fixture.input_tag = 0xd2;
        memset(fixture.money_root, 0xc3, sizeof(fixture.money_root));
        memset(fixture.post_prepare_root, 0xc4,
               sizeof(fixture.post_prepare_root));
        snprintf(request.idempotency_key, sizeof(request.idempotency_key),
                 "zslp-genesis-2");
        runtime.now_unix = 1100;
        struct zslp_intent_result drifted;
        ASSERT(zslp_transaction_intent_plan(
            &runtime, &request, &drifted).ok);
        memset(fixture.money_root, 0xc5, sizeof(fixture.money_root));
        ASSERT(!zslp_transaction_intent_commit(
            &runtime, "dev", drifted.plan_id, &replay).ok);
        struct vault_intent_row conflicted;
        ASSERT(vault_intent_find(&zdb, drifted.plan_id, &conflicted));
        ASSERT_EQ(conflicted.state, VAULT_INTENT_CONFLICTED);
        ASSERT_EQ(fixture.publishes, 1);
        node_db_close(&zdb);
        wallet_lock_reset_for_test();
        PASS();
    }

    TEST("opaque overlay intents encrypt semantics and publish exact bytes once") {
        struct node_db odb; memset(&odb, 0, sizeof(odb));
        ASSERT(node_db_open(&odb, ":memory:"));
        wallet_lock_reset_for_test();
        wallet_lock_note_encrypted_at_rest();
        ASSERT(wallet_lock_unlock(NULL, NULL, "overlay-intent-test-pass").ok);
        struct ti_zslp_fixture fixture; memset(&fixture, 0, sizeof(fixture));
        const uint8_t genesis[32] = { 0xa6 };
        ASSERT(wallet_identity_ensure(&odb, genesis, "dev", &fixture.identity));
        memset(fixture.tip_hash, 0xb3, sizeof(fixture.tip_hash));
        memset(fixture.money_root, 0xcb, sizeof(fixture.money_root));
        memset(fixture.post_prepare_root, 0xcc,
               sizeof(fixture.post_prepare_root));
        fixture.input_tag = 0xd5;
        struct overlay_intent_runtime runtime = {
            .node_db = &odb,
            .read_money = ti_zslp_money, .money_ctx = &fixture,
            .prepare = ti_overlay_prepare, .prepare_ctx = &fixture,
            .publish = ti_overlay_publish, .publish_ctx = &fixture,
            .tip_height = 100, .maximum_fee_zat = 1000, .now_unix = 1400,
        };
        memcpy(runtime.tip_hash, fixture.tip_hash, 32);
        struct overlay_intent_request request; memset(&request, 0, sizeof(request));
        snprintf(request.wallet_scope, sizeof(request.wallet_scope), "dev");
        snprintf(request.application_kind, sizeof(request.application_kind),
                 "zid_intent");
        snprintf(request.operation, sizeof(request.operation), "anchor");
        const char private_semantics[] = "private-overlay-pubkey";
        memcpy(request.semantics, private_semantics,
               sizeof(private_semantics) - 1);
        request.semantics_len = sizeof(private_semantics) - 1;
        snprintf(request.idempotency_key, sizeof(request.idempotency_key),
                 "zid-anchor-1");
        struct overlay_intent_result planned;
        ASSERT(overlay_transaction_intent_plan(
            &runtime, &request, &planned).ok);
        ASSERT_EQ(fixture.prepares, 1);
        ASSERT(!planned.broadcast);
        ASSERT_EQ(planned.actual_fee_zat, 500);
        ASSERT_EQ(planned.reserved_zat, 1000);
        ASSERT_STR_EQ(planned.application_kind, "zid_intent");
        ASSERT_STR_EQ(planned.operation, "anchor");
        ASSERT(vault_intent_has_raw(&odb, planned.plan_id));
        ASSERT(!ti_db_contains(&odb, private_semantics));

        struct overlay_intent_result replay;
        ASSERT(overlay_transaction_intent_plan(&runtime, &request, &replay).ok);
        ASSERT(replay.idempotent_replay);
        ASSERT(memcmp(replay.plan_id, planned.plan_id, 32) == 0);
        ASSERT_EQ(fixture.prepares, 1);
        struct overlay_intent_request changed = request;
        changed.semantics[0] ^= 1;
        ASSERT(!overlay_transaction_intent_plan(
            &runtime, &changed, &replay).ok);
        ASSERT(!overlay_transaction_intent_commit(
            &runtime, "zdir_intent", "dev", planned.plan_id, &replay).ok);
        ASSERT(!overlay_transaction_intent_commit(
            &runtime, "zid_intent", "prod", planned.plan_id, &replay).ok);

        struct overlay_intent_result committed;
        ASSERT(overlay_transaction_intent_commit(
            &runtime, "zid_intent", "dev", planned.plan_id, &committed).ok);
        ASSERT(committed.broadcast);
        ASSERT_EQ(fixture.publishes, 1);
        ASSERT(overlay_transaction_intent_commit(
            &runtime, "zid_intent", "dev", planned.plan_id, &replay).ok);
        ASSERT(replay.idempotent_replay);
        ASSERT_EQ(fixture.publishes, 1);

        fixture.input_tag = 0xd6;
        memset(fixture.money_root, 0xcd, sizeof(fixture.money_root));
        memset(fixture.post_prepare_root, 0xce,
               sizeof(fixture.post_prepare_root));
        snprintf(request.idempotency_key, sizeof(request.idempotency_key),
                 "zid-anchor-2");
        runtime.now_unix = 1500;
        struct overlay_intent_result drifted;
        ASSERT(overlay_transaction_intent_plan(
            &runtime, &request, &drifted).ok);
        memset(fixture.money_root, 0xcf, sizeof(fixture.money_root));
        ASSERT(!overlay_transaction_intent_commit(
            &runtime, "zid_intent", "dev", drifted.plan_id, &replay).ok);
        struct vault_intent_row conflicted;
        ASSERT(vault_intent_find(&odb, drifted.plan_id, &conflicted));
        ASSERT_EQ(conflicted.state, VAULT_INTENT_CONFLICTED);
        ASSERT_EQ(fixture.publishes, 1);
        node_db_close(&odb);
        wallet_lock_reset_for_test();
        PASS();
    }

    TEST("ZNAM intents reserve exact bytes, hide semantics, and publish once") {
        struct node_db zdb; memset(&zdb, 0, sizeof(zdb));
        ASSERT(node_db_open(&zdb, ":memory:"));
        wallet_lock_reset_for_test();
        wallet_lock_note_encrypted_at_rest();
        ASSERT(wallet_lock_unlock(NULL, NULL, "znam-intent-test-pass").ok);
        struct ti_zslp_fixture fixture; memset(&fixture, 0, sizeof(fixture));
        const uint8_t genesis[32] = { 0xa5 };
        ASSERT(wallet_identity_ensure(&zdb, genesis, "dev", &fixture.identity));
        memset(fixture.tip_hash, 0xb2, sizeof(fixture.tip_hash));
        memset(fixture.money_root, 0xc6, sizeof(fixture.money_root));
        memset(fixture.post_prepare_root, 0xc7,
               sizeof(fixture.post_prepare_root));
        fixture.input_tag = 0xd3;
        struct znam_intent_runtime runtime = {
            .node_db = &zdb,
            .read_money = ti_znam_money, .money_ctx = &fixture,
            .prepare = ti_znam_prepare, .prepare_ctx = &fixture,
            .publish = ti_znam_publish, .publish_ctx = &fixture,
            .tip_height = 100, .maximum_fee_zat = 1000, .now_unix = 1200,
        };
        memcpy(runtime.tip_hash, fixture.tip_hash, 32);
        struct znam_intent_request request; memset(&request, 0, sizeof(request));
        snprintf(request.wallet_scope, sizeof(request.wallet_scope), "dev");
        request.operation = ZNAM_INTENT_REGISTER;
        snprintf(request.name, sizeof(request.name), "private-name");
        request.target_type = 1;
        snprintf(request.value, sizeof(request.value), "private-value");
        snprintf(request.idempotency_key, sizeof(request.idempotency_key),
                 "znam-register-1");
        struct znam_intent_result planned;
        ASSERT(znam_transaction_intent_plan(&runtime, &request, &planned).ok);
        ASSERT_EQ(fixture.prepares, 1);
        ASSERT(!planned.broadcast);
        ASSERT_EQ(planned.actual_fee_zat, 500);
        ASSERT_EQ(planned.reserved_zat, 1000);
        ASSERT(vault_intent_has_raw(&zdb, planned.plan_id));
        ASSERT(!ti_db_contains(&zdb, "private-name"));
        ASSERT(!ti_db_contains(&zdb, "private-value"));

        struct znam_intent_result replay;
        ASSERT(znam_transaction_intent_plan(&runtime, &request, &replay).ok);
        ASSERT(replay.idempotent_replay);
        ASSERT(memcmp(replay.plan_id, planned.plan_id, 32) == 0);
        ASSERT_EQ(fixture.prepares, 1);
        struct znam_intent_request changed = request;
        snprintf(changed.value, sizeof(changed.value), "different-private");
        ASSERT(!znam_transaction_intent_plan(&runtime, &changed, &replay).ok);

        struct znam_intent_result committed;
        ASSERT(znam_transaction_intent_commit(
            &runtime, "dev", planned.plan_id, &committed).ok);
        ASSERT(committed.broadcast);
        ASSERT_EQ(fixture.publishes, 1);
        ASSERT(znam_transaction_intent_commit(
            &runtime, "dev", planned.plan_id, &replay).ok);
        ASSERT(replay.idempotent_replay);
        ASSERT_EQ(fixture.publishes, 1);
        ASSERT(!znam_transaction_intent_commit(
            &runtime, "prod", planned.plan_id, &replay).ok);

        fixture.input_tag = 0xd4;
        memset(fixture.money_root, 0xc8, sizeof(fixture.money_root));
        memset(fixture.post_prepare_root, 0xc9,
               sizeof(fixture.post_prepare_root));
        snprintf(request.idempotency_key, sizeof(request.idempotency_key),
                 "znam-register-2");
        runtime.now_unix = 1300;
        struct znam_intent_result drifted;
        ASSERT(znam_transaction_intent_plan(
            &runtime, &request, &drifted).ok);
        memset(fixture.money_root, 0xca, sizeof(fixture.money_root));
        ASSERT(!znam_transaction_intent_commit(
            &runtime, "dev", drifted.plan_id, &replay).ok);
        struct vault_intent_row conflicted;
        ASSERT(vault_intent_find(&zdb, drifted.plan_id, &conflicted));
        ASSERT_EQ(conflicted.state, VAULT_INTENT_CONFLICTED);
        ASSERT_EQ(fixture.publishes, 1);
        node_db_close(&zdb);
        wallet_lock_reset_for_test();
        PASS();
    }

    TEST("wallet identity persists, rejects lane/network changes, and dev "
         "reservations atomically enforce the reserve and lab ceiling") {
        if (ndb.open) node_db_close(&ndb);
        char dir[256], path[320];
        test_make_tmpdir(dir, sizeof(dir), "transaction_intent", "custody");
        snprintf(path, sizeof(path), "%s/node.db", dir);
        ASSERT(node_db_open(&ndb, path));
        const uint8_t genesis[32] = { 0x42 };
        struct wallet_identity_row identity;
        ASSERT(wallet_identity_ensure(&ndb, genesis, "dev", &identity));
        char instance_id[WALLET_INSTANCE_ID_HEX_LEN + 1];
        snprintf(instance_id, sizeof(instance_id), "%s",
                 identity.wallet_instance_id);
        node_db_close(&ndb);
        ASSERT(node_db_open(&ndb, path));
        struct wallet_identity_row restarted;
        ASSERT(wallet_identity_ensure(&ndb, genesis, "dev", &restarted));
        ASSERT_STR_EQ(restarted.wallet_instance_id, instance_id);
        uint8_t wrong_genesis[32] = { 0x43 };
        ASSERT(!wallet_identity_ensure(&ndb, wrong_genesis, "dev",
                                       &restarted));
        ASSERT(!wallet_identity_ensure(&ndb, genesis, "canonical",
                                       &restarted));

        struct vault_intent_row a, b, over;
        ti_bound_row(&a, 0x11, &identity, 3000000);
        ti_bound_row(&b, 0x21, &identity, 2000000);
        ti_bound_row(&over, 0x31, &identity, 1);
        ASSERT(vault_intent_reserve(&ndb, &a, 30000000));
        ASSERT(vault_intent_reserve(&ndb, &b, 30000000));
        ASSERT_EQ(vault_intent_reserved_total(
                      &ndb, "dev", identity.wallet_instance_id), 5000000);
        ASSERT_EQ(vault_intent_reserved_total_at(
                      &ndb, "dev", identity.wallet_instance_id, 699),
                  5000000);
        ASSERT_EQ(vault_intent_reserved_total_at(
                      &ndb, "dev", identity.wallet_instance_id, 700), 0);
        ASSERT_EQ(vault_intent_reserved_total_at(
                      &ndb, "dev", identity.wallet_instance_id, -1), -1);
        ASSERT(!vault_intent_reserve(&ndb, &over, 30000000));
        ASSERT(vault_intent_set_state(&ndb, a.plan_id, VAULT_INTENT_FAILED,
                                      NULL, "LAB_ROLLBACK", 200));
        ASSERT(vault_intent_reserve(&ndb, &over, 30000000));
        ASSERT_EQ(vault_intent_reserved_total(
                      &ndb, "dev", identity.wallet_instance_id), 2000001);
        node_db_close(&ndb);
        test_rm_rf(dir);
        PASS();
    }

    TEST("isolated test custody reserves and survives restart without "
         "entering dev or prod scope") {
        if (ndb.open) node_db_close(&ndb);
        char dir[256], path[320];
        test_make_tmpdir(dir, sizeof(dir), "transaction_intent", "test_scope");
        snprintf(path, sizeof(path), "%s/node.db", dir);
        ASSERT(node_db_open(&ndb, path));
        const uint8_t genesis[32] = { 0x52 };
        struct wallet_identity_row identity;
        ASSERT(wallet_identity_ensure(&ndb, genesis, "test", &identity));
        struct vault_intent_row planned;
        ti_bound_row(&planned, 0x41, &identity, 21000);
        snprintf(planned.wallet_scope, sizeof(planned.wallet_scope), "test");
        ASSERT(vault_intent_reserve(&ndb, &planned, 600000));
        ASSERT_EQ(vault_intent_reserved_total(
                      &ndb, "test", identity.wallet_instance_id), 21000);
        ASSERT_EQ(vault_intent_reserved_total(
                      &ndb, "dev", identity.wallet_instance_id), 0);
        ASSERT_EQ(vault_intent_reserved_total(
                      &ndb, "prod", identity.wallet_instance_id), 0);
        node_db_close(&ndb);

        ASSERT(node_db_open(&ndb, path));
        struct vault_intent_row recovered;
        ASSERT(vault_intent_find(&ndb, planned.plan_id, &recovered));
        ASSERT_STR_EQ(recovered.wallet_scope, "test");
        ASSERT_EQ(recovered.reserved_zat, 21000);
        ASSERT_EQ(vault_intent_reserved_total(
                      &ndb, "test", identity.wallet_instance_id), 21000);
        node_db_close(&ndb);
        test_rm_rf(dir);
        PASS();
    }

_test_next:;
    if (ndb.open) node_db_close(&ndb);
    wallet_lock_reset_for_test();
    return failures;
}
