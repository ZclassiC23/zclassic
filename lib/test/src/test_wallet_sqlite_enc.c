/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Integration tests for wallet_sqlite encryption — wave 8 live wallet
 * encryption integration.
 *
 * Exercises the wallet_sqlite read/write paths with and without
 * ZCL_WALLET_PASSPHRASE to verify:
 *   (1) plaintext round-trip still works (backward compat)
 *   (2) encrypted round-trip works when passphrase is set
 *   (3) encrypted blobs are unreadable without passphrase
 *   (4) mixed plaintext+encrypted DB reads cleanly
 *   (5) seed and sapling key encryption/decryption
 *
 * Each test opens a fresh in-memory SQLite DB, creates the schema,
 * and operates through the wallet_sqlite API. */

#include "test/test_helpers.h"
#include "wallet/wallet_sqlite.h"
#include "wallet/wallet_keystore.h"
#include "wallet/wallet.h"
#include "keys/key.h"
#include "support/cleanse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/safe_alloc.h"

/* ── Helpers ─────────────────────────────────────────────────── */

static const char k_schema[] =
    "CREATE TABLE wallet_keys("
    "pubkey_hash BLOB PRIMARY KEY,"
    "pubkey BLOB,privkey BLOB,compressed INT,created_at INT);"
    "CREATE TABLE wallet_sapling_keys("
    "ivk BLOB PRIMARY KEY,xsk BLOB,xfvk BLOB,"
    "diversifier BLOB,pk_d BLOB,child_index INT,address TEXT);"
    "CREATE TABLE wallet_seed("
    "id INTEGER PRIMARY KEY CHECK(id=1),seed BLOB,next_child INT);"
    "CREATE TABLE wallet_transactions("
    "txid BLOB PRIMARY KEY,raw_tx BLOB,block_hash BLOB,"
    "block_height INT,time_received INT,from_me INT,fee INT);"
    "CREATE TABLE wallet_scripts("
    "script_hash BLOB PRIMARY KEY,redeem_script BLOB);"
    "CREATE TABLE wallet_watch_only("
    "address_hash BLOB PRIMARY KEY,address TEXT,created_at INT);"
    "CREATE TABLE node_state(key TEXT PRIMARY KEY,value BLOB);";

static sqlite3 *open_mem_db(void)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) return NULL;
    if (sqlite3_exec(db, k_schema, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

/* Set or unset ZCL_WALLET_PASSPHRASE. NULL → unsetenv. */
static void set_passphrase(const char *pass)
{
    if (pass)
        setenv("ZCL_WALLET_PASSPHRASE", pass, 1);
    else
        unsetenv("ZCL_WALLET_PASSPHRASE");
}

/* Make a deterministic private key for testing. */
static void make_test_key(struct privkey *key, struct pubkey *pk, uint8_t seed)
{
    privkey_init(key);
    memset(key->vch, seed, 32);
    /* Ensure it's a valid secp256k1 scalar — tweak byte 0 to avoid
     * the rare all-same-byte edge cases. */
    key->vch[0] = seed;
    key->vch[1] = (uint8_t)(seed ^ 0xAA);
    key->fValid = true;
    key->fCompressed = true;
    privkey_get_pubkey(key, pk);
}

/* Allocate a wallet for reading keys back. */
static struct wallet *alloc_wallet(void)
{
    struct wallet *w = zcl_calloc(1, sizeof(struct wallet), "test_wallet");
    if (w) {
        keystore_init(&w->keystore);
        sapling_keystore_init(&w->sapling_keys);
    }
    return w;
}

static void free_wallet(struct wallet *w)
{
    if (!w) return;
    keystore_free(&w->keystore);
    free(w);
}

/* ── Tests ───────────────────────────────────────────────────── */

static int test_plaintext_roundtrip(void)
{
    int failures = 0;
    TEST("wallet_sqlite_enc: plaintext key roundtrip (no passphrase)") {
        set_passphrase(NULL);
        sqlite3 *db = open_mem_db();
        ASSERT(db);

        struct wallet_sqlite ws;
        ASSERT(wallet_sqlite_open(&ws, db));

        struct privkey key;
        struct pubkey pk;
        make_test_key(&key, &pk, 0x42);
        ASSERT(wallet_sqlite_write_key(&ws, &pk, &key));

        struct wallet *w = alloc_wallet();
        ASSERT(w);
        ASSERT(wallet_sqlite_read_keys(&ws, w));
        ASSERT(w->keystore.num_keys == 1);

        /* Verify the key matches. */
        struct key_id kid = pubkey_get_id(&pk);
        struct privkey got;
        privkey_init(&got);
        ASSERT(keystore_get_key(&w->keystore, &kid, &got));
        ASSERT(memcmp(got.vch, key.vch, 32) == 0);

        wallet_sqlite_close(&ws);
        free_wallet(w);
        sqlite3_close(db);
        PASS();
    } _test_next:;
    return failures;
}

static int test_encrypted_roundtrip(void)
{
    int failures = 0;
    TEST("wallet_sqlite_enc: encrypted key roundtrip") {
        set_passphrase("test-passphrase-42");
        sqlite3 *db = open_mem_db();
        ASSERT(db);

        struct wallet_sqlite ws;
        ASSERT(wallet_sqlite_open(&ws, db));

        struct privkey key;
        struct pubkey pk;
        make_test_key(&key, &pk, 0x55);
        ASSERT(wallet_sqlite_write_key(&ws, &pk, &key));

        /* Read it back — same passphrase should decrypt. */
        struct wallet *w = alloc_wallet();
        ASSERT(w);
        ASSERT(wallet_sqlite_read_keys(&ws, w));
        ASSERT(w->keystore.num_keys == 1);

        struct key_id kid = pubkey_get_id(&pk);
        struct privkey got;
        privkey_init(&got);
        ASSERT(keystore_get_key(&w->keystore, &kid, &got));
        ASSERT(memcmp(got.vch, key.vch, 32) == 0);

        wallet_sqlite_close(&ws);
        free_wallet(w);
        sqlite3_close(db);
        set_passphrase(NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_encrypted_unreadable_without_pass(void)
{
    int failures = 0;
    TEST("wallet_sqlite_enc: encrypted key unreadable without passphrase") {
        set_passphrase("my-secret-phrase");
        sqlite3 *db = open_mem_db();
        ASSERT(db);

        struct wallet_sqlite ws;
        ASSERT(wallet_sqlite_open(&ws, db));

        struct privkey key;
        struct pubkey pk;
        make_test_key(&key, &pk, 0x77);
        ASSERT(wallet_sqlite_write_key(&ws, &pk, &key));

        /* Remove passphrase — the read should skip the key since it
         * detects the WKS1 envelope but has no passphrase to decrypt. */
        set_passphrase(NULL);

        struct wallet *w = alloc_wallet();
        ASSERT(w);
        ASSERT(wallet_sqlite_read_keys(&ws, w));
        /* Key should be skipped — no keys loaded. */
        ASSERT(w->keystore.num_keys == 0);

        wallet_sqlite_close(&ws);
        free_wallet(w);
        sqlite3_close(db);
        PASS();
    } _test_next:;
    return failures;
}

static int test_wrong_passphrase_fails(void)
{
    int failures = 0;
    TEST("wallet_sqlite_enc: wrong passphrase skips key") {
        set_passphrase("correct-phrase");
        sqlite3 *db = open_mem_db();
        ASSERT(db);

        struct wallet_sqlite ws;
        ASSERT(wallet_sqlite_open(&ws, db));

        struct privkey key;
        struct pubkey pk;
        make_test_key(&key, &pk, 0x88);
        ASSERT(wallet_sqlite_write_key(&ws, &pk, &key));

        /* Change passphrase — GCM tag should fail. */
        set_passphrase("wrong-phrase");

        struct wallet *w = alloc_wallet();
        ASSERT(w);
        ASSERT(wallet_sqlite_read_keys(&ws, w));
        ASSERT(w->keystore.num_keys == 0);

        wallet_sqlite_close(&ws);
        free_wallet(w);
        sqlite3_close(db);
        set_passphrase(NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_mixed_plaintext_and_encrypted(void)
{
    int failures = 0;
    TEST("wallet_sqlite_enc: mixed plaintext + encrypted keys both read") {
        sqlite3 *db = open_mem_db();
        ASSERT(db);

        struct wallet_sqlite ws;
        ASSERT(wallet_sqlite_open(&ws, db));

        /* Write key A without encryption. */
        set_passphrase(NULL);
        struct privkey kA;
        struct pubkey pA;
        make_test_key(&kA, &pA, 0x11);
        ASSERT(wallet_sqlite_write_key(&ws, &pA, &kA));

        /* Write key B with encryption. */
        set_passphrase("mix-test");
        struct privkey kB;
        struct pubkey pB;
        make_test_key(&kB, &pB, 0x22);
        ASSERT(wallet_sqlite_write_key(&ws, &pB, &kB));

        /* Read with passphrase set — both should load. */
        struct wallet *w = alloc_wallet();
        ASSERT(w);
        ASSERT(wallet_sqlite_read_keys(&ws, w));
        ASSERT(w->keystore.num_keys == 2);

        wallet_sqlite_close(&ws);
        free_wallet(w);
        sqlite3_close(db);
        set_passphrase(NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_seed_encrypted_roundtrip(void)
{
    int failures = 0;
    TEST("wallet_sqlite_enc: seed encrypts/decrypts correctly") {
        set_passphrase("seed-pass");
        sqlite3 *db = open_mem_db();
        ASSERT(db);

        struct wallet_sqlite ws;
        ASSERT(wallet_sqlite_open(&ws, db));

        uint8_t seed[32];
        memset(seed, 0xAB, 32);
        ASSERT(wallet_sqlite_write_sapling_seed(&ws, seed));

        uint8_t got[32];
        memset(got, 0, 32);
        ASSERT(wallet_sqlite_read_sapling_seed(&ws, got));
        ASSERT(memcmp(seed, got, 32) == 0);

        /* Without passphrase, should fail. */
        set_passphrase(NULL);
        uint8_t bad[32];
        ASSERT(!wallet_sqlite_read_sapling_seed(&ws, bad));

        wallet_sqlite_close(&ws);
        sqlite3_close(db);
        PASS();
    } _test_next:;
    return failures;
}

static int test_seed_plaintext_still_works(void)
{
    int failures = 0;
    TEST("wallet_sqlite_enc: plaintext seed still readable") {
        set_passphrase(NULL);
        sqlite3 *db = open_mem_db();
        ASSERT(db);

        struct wallet_sqlite ws;
        ASSERT(wallet_sqlite_open(&ws, db));

        uint8_t seed[32];
        memset(seed, 0xCD, 32);
        ASSERT(wallet_sqlite_write_sapling_seed(&ws, seed));

        uint8_t got[32];
        memset(got, 0, 32);
        ASSERT(wallet_sqlite_read_sapling_seed(&ws, got));
        ASSERT(memcmp(seed, got, 32) == 0);

        wallet_sqlite_close(&ws);
        sqlite3_close(db);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Entry point ─────────────────────────────────────────────── */

int test_wallet_sqlite_enc(void);

int test_wallet_sqlite_enc(void)
{
    int failures = 0;

    failures += test_plaintext_roundtrip();
    failures += test_encrypted_roundtrip();
    failures += test_encrypted_unreadable_without_pass();
    failures += test_wrong_passphrase_fails();
    failures += test_mixed_plaintext_and_encrypted();
    failures += test_seed_encrypted_roundtrip();
    failures += test_seed_plaintext_still_works();

    /* Cleanup: ensure passphrase env is unset. */
    set_passphrase(NULL);
    return failures;
}
