/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Durable intent idempotency, expiry, raw-tx recovery, and ciphertext proof. */

#include "test/test_core.h"

#include "controllers/vault_intent_controller.h"
#include "models/database.h"
#include "models/vault_intent.h"
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

_test_next:;
    if (ndb.open) node_db_close(&ndb);
    wallet_lock_reset_for_test();
    return failures;
}
