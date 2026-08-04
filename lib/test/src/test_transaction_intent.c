/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Durable intent idempotency, expiry, raw-tx recovery, and ciphertext proof. */

#include "test/test_core.h"

#include "controllers/vault_intent_controller.h"
#include "models/database.h"
#include "models/vault_intent.h"
#include "models/wallet_identity.h"
#include "models/wallet_metadata_crypto.h"
#include "wallet/wallet_lock.h"

#include <sqlite3.h>
#include <string.h>

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

int test_transaction_intent(void)
{
    int failures = 0;
    struct node_db ndb; memset(&ndb, 0, sizeof(ndb));

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

_test_next:;
    if (ndb.open) node_db_close(&ndb);
    wallet_lock_reset_for_test();
    return failures;
}
