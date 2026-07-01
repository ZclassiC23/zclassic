/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

/* AR test callbacks (must be file-scope for ISO C23) */

static bool ar_test_reject_zero(void *record, void *ctx) {
    (void)ctx;
    struct db_block *b = record;
    return b->height > 0;
}

static int ar_test_save_count = 0;
static void ar_test_count_saves(void *record, void *ctx) {
    (void)record; (void)ctx;
    ar_test_save_count++;
}

static bool ar_test_prevent_delete(void *record, void *ctx) {
    (void)record; (void)ctx;
    return false;
}

static bool ar_test_reject_always(void *record, void *ctx) {
    (void)record; (void)ctx;
    return false;
}

static bool ar_test_handler(const void *params, bool help, void *result) {
    (void)params; (void)help; (void)result;
    return true;
}

static bool ar_test_auth_passed = true;
static bool ar_test_auth_filter(const char *method, void *ctx) {
    (void)method; (void)ctx;
    return ar_test_auth_passed;
}

static bool ar_test_route_reject = false;
static bool ar_test_per_route_filter(const char *method, void *ctx) {
    (void)method; (void)ctx;
    return !ar_test_route_reject;
}

int test_activerecord(void)
{
    int failures = 0;

    /* ================================================================ */
    /* ActiveRecord callbacks, validations, relationships, router       */
    /* ================================================================ */

    /* AR validation — block model */
    {
        printf("AR validation: block... ");
        struct ar_errors errs;
        struct db_block blk;
        memset(&blk, 0, sizeof(blk));

        /* Empty block should fail validation (hash, prev_hash, merkle_root blank) */
        bool ok = !db_block_validate(&blk, &errs);
        ok = ok && ar_errors_any(&errs);
        ok = ok && (errs.count >= 3); /* hash, prev_hash, merkle_root, time, bits */

        /* Valid block should pass */
        memset(blk.hash, 0xAA, 32);
        memset(blk.prev_hash, 0xBB, 32);
        memset(blk.merkle_root, 0xCC, 32);
        blk.height = 100;
        blk.time = 1700000000;
        blk.bits = 0x1d00ffff;
        blk.status = 3;
        ok = ok && db_block_validate(&blk, &errs);
        ok = ok && !ar_errors_any(&errs);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR validation — tx model */
    {
        printf("AR validation: tx... ");
        struct ar_errors errs;
        struct db_tx_index tx;
        memset(&tx, 0, sizeof(tx));

        bool ok = !db_tx_validate(&tx, &errs);
        ok = ok && (errs.count >= 2); /* txid, block_hash blank */

        memset(tx.txid, 0x11, 32);
        memset(tx.block_hash, 0x22, 32);
        tx.block_height = 100;
        ok = ok && db_tx_validate(&tx, &errs);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR validation — utxo model */
    {
        printf("AR validation: utxo... ");
        struct ar_errors errs;
        struct db_utxo u;
        memset(&u, 0, sizeof(u));

        bool ok = !db_utxo_validate(&u, &errs);
        ok = ok && ar_errors_any(&errs);

        memset(u.txid, 0x33, 32);
        u.value = 50000;
        u.height = 100;
        u.script_type = SCRIPT_P2PKH;
        ok = ok && db_utxo_validate(&u, &errs);

        /* Bad script_type */
        u.script_type = (enum script_type)99;
        ok = ok && !db_utxo_validate(&u, &errs);
        u.script_type = SCRIPT_P2PKH;

        /* has_address with blank address_hash — now ALLOWED.
         * All-zeros Hash160 is a valid (rare) burn address. */
        u.has_address = true;
        memset(u.address_hash, 0, 20);
        ok = ok && db_utxo_validate(&u, &errs);
        memset(u.address_hash, 0xAA, 20);
        ok = ok && db_utxo_validate(&u, &errs);
        u.has_address = false;

        /* MAX_MONEY check */
        u.value = 2100000000000001LL;
        ok = ok && !db_utxo_validate(&u, &errs);
        u.value = 2100000000000000LL;
        ok = ok && db_utxo_validate(&u, &errs);
        u.value = 50000;

        /* Script size bounds */
        u.script_len = 10001;
        u.script = (uint8_t *)&u; /* non-null pointer */
        ok = ok && !db_utxo_validate(&u, &errs);
        u.script_len = 0;
        u.script = NULL;
        ok = ok && db_utxo_validate(&u, &errs);

        /* Null pointer with nonzero length */
        u.script_len = 100;
        u.script = NULL;
        ok = ok && !db_utxo_validate(&u, &errs);
        u.script_len = 0;

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR validation — wallet key model */
    {
        printf("AR validation: wallet key... ");
        struct ar_errors errs;
        struct db_wallet_key k;
        memset(&k, 0, sizeof(k));

        bool ok = !db_wallet_key_validate(&k, &errs);
        ok = ok && (errs.count >= 3); /* pubkey_hash, pubkey, privkey blank */

        memset(k.pubkey_hash, 0x44, 20);
        memset(k.pubkey, 0x55, 33);
        k.pubkey_len = 33;
        k.compressed = true;
        memset(k.privkey, 0x66, 32);
        ok = ok && db_wallet_key_validate(&k, &errs);

        /* Bad pubkey_len */
        k.pubkey_len = 10;
        ok = ok && !db_wallet_key_validate(&k, &errs);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR validation — peer model */
    {
        printf("AR validation: peer... ");
        struct ar_errors errs;
        struct db_peer p;
        memset(&p, 0, sizeof(p));

        bool ok = !db_peer_validate(&p, &errs);

        memset(p.ip, 0x77, 16);
        p.port = 8033;
        ok = ok && db_peer_validate(&p, &errs);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR validation — mempool model */
    {
        printf("AR validation: mempool... ");
        struct ar_errors errs;
        struct db_mempool_entry e;
        memset(&e, 0, sizeof(e));

        bool ok = !db_mempool_validate(&e, &errs);

        memset(e.txid, 0x88, 32);
        e.size = 225;
        e.time_added = 1700000000;
        ok = ok && db_mempool_validate(&e, &errs);

        /* time_added=0 should fail */
        e.time_added = 0;
        ok = ok && !db_mempool_validate(&e, &errs);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR validation — wallet tx */
    {
        printf("AR validation: wallet tx... ");
        struct ar_errors errs;
        struct db_wallet_tx t;
        memset(&t, 0, sizeof(t));

        bool ok = !db_wallet_tx_validate(&t, &errs);
        ok = ok && (errs.count >= 2); /* txid blank, time_received */

        memset(t.txid, 0x11, 32);
        t.time_received = 1700000000;
        ok = ok && db_wallet_tx_validate(&t, &errs);

        /* Negative block_height with has_block should fail */
        t.has_block = true;
        memset(t.block_hash, 0xAA, 32);
        t.block_height = -1;
        ok = ok && !db_wallet_tx_validate(&t, &errs);

        t.block_height = 100;
        ok = ok && db_wallet_tx_validate(&t, &errs);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR validation — wallet utxo */
    {
        printf("AR validation: wallet utxo... ");
        struct ar_errors errs;
        struct db_wallet_utxo u;
        memset(&u, 0, sizeof(u));

        bool ok = !db_wallet_utxo_validate(&u, &errs);

        memset(u.txid, 0x22, 32);
        u.value = 100000;
        u.height = 500;
        ok = ok && db_wallet_utxo_validate(&u, &errs);

        /* Negative value should fail */
        u.value = -1;
        ok = ok && !db_wallet_utxo_validate(&u, &errs);
        u.value = 100000;

        /* MAX_MONEY check */
        u.value = 2100000000000001LL;
        ok = ok && !db_wallet_utxo_validate(&u, &errs);
        u.value = 2100000000000000LL;
        ok = ok && db_wallet_utxo_validate(&u, &errs);
        u.value = 100000;

        /* Script size bounds */
        u.script_len = 10001;
        u.script = (uint8_t *)&u;
        ok = ok && !db_wallet_utxo_validate(&u, &errs);
        u.script_len = 0;
        u.script = NULL;

        /* Null pointer with nonzero length */
        u.script_len = 100;
        u.script = NULL;
        ok = ok && !db_wallet_utxo_validate(&u, &errs);
        u.script_len = 0;

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR validation — sapling note */
    {
        printf("AR validation: sapling note... ");
        struct ar_errors errs;
        struct db_sapling_note n;
        memset(&n, 0, sizeof(n));

        bool ok = !db_sapling_note_validate(&n, &errs);
        ok = ok && (errs.count >= 7); /* txid, ivk, nullifier, cm, pk_d, diversifier, rcm */

        memset(n.txid, 0x33, 32);
        memset(n.ivk, 0x44, 32);
        memset(n.nullifier, 0x55, 32);
        memset(n.cm, 0x66, 32);
        memset(n.pk_d, 0x77, 32);
        memset(n.diversifier, 0x88, 11);
        memset(n.rcm, 0x99, 32);
        snprintf(n.source, sizeof(n.source), "%s",
                 DB_SAPLING_NOTE_SOURCE_LOCAL);
        n.value = 50000;
        ok = ok && db_sapling_note_validate(&n, &errs);

        /* Negative value should fail */
        n.value = -1;
        ok = ok && !db_sapling_note_validate(&n, &errs);
        n.value = 50000;

        /* MAX_MONEY check */
        n.value = 2100000000000001LL;
        ok = ok && !db_sapling_note_validate(&n, &errs);
        n.value = 2100000000000000LL;
        ok = ok && db_sapling_note_validate(&n, &errs);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR validation — sapling key */
    {
        printf("AR validation: sapling key... ");
        struct ar_errors errs;
        struct db_sapling_key sk;
        memset(&sk, 0, sizeof(sk));

        bool ok = !db_sapling_key_validate(&sk, &errs);
        ok = ok && (errs.count >= 6); /* ivk, xsk, xfvk, diversifier, pk_d, address */

        memset(sk.ivk, 0x11, 32);
        memset(sk.xsk, 0x55, 169);
        memset(sk.xfvk, 0x22, 169);
        memset(sk.diversifier, 0x33, 11);
        memset(sk.pk_d, 0x44, 32);
        snprintf(sk.address, sizeof(sk.address), "zs1test");
        ok = ok && db_sapling_key_validate(&sk, &errs);

        /* Blank address should fail */
        sk.address[0] = '\0';
        ok = ok && !db_sapling_key_validate(&sk, &errs);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR before_save callback — halt save */
    {
        printf("AR before_save callback (halt)... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        /* Register a before_save that rejects blocks at height 0 */
        struct ar_callbacks *cbs = db_block_callbacks();
        ar_callbacks_init(cbs); /* Reset */

        ar_register_before_save(cbs, ar_test_reject_zero);

        uint8_t sol[] = {0x01};
        struct db_block blk;
        memset(&blk, 0, sizeof(blk));
        memset(blk.hash, 0xAA, 32);
        memset(blk.prev_hash, 0xBB, 32);
        memset(blk.merkle_root, 0xCC, 32);
        blk.time = 1700000000;
        blk.bits = 0x1d00ffff;
        blk.status = 3;
        blk.solution = sol;
        blk.solution_len = 1;

        /* height 0 → should be rejected by callback */
        blk.height = 0;
        ok = ok && !db_block_save(&ndb, &blk);

        /* height 1 → should succeed */
        blk.height = 1;
        ok = ok && db_block_save(&ndb, &blk);
        ok = ok && (db_block_count(&ndb) == 1);

        ar_callbacks_init(cbs); /* Clean up */
        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR after_save callback — counter */
    {
        printf("AR after_save callback (counter)... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        ar_test_save_count = 0;

        struct ar_callbacks *cbs = db_block_callbacks();
        ar_callbacks_init(cbs);
        ar_register_after_save(cbs, ar_test_count_saves);

        uint8_t sol[] = {0x01};
        struct db_block blk;
        memset(&blk, 0, sizeof(blk));
        memset(blk.prev_hash, 0xBB, 32);
        memset(blk.merkle_root, 0xCC, 32);
        blk.time = 1700000000;
        blk.bits = 0x1d00ffff;
        blk.status = 3;
        blk.solution = sol;
        blk.solution_len = 1;

        for (int i = 0; i < 5; i++) {
            memset(blk.hash, i + 1, 32);
            blk.height = i + 1;
            ok = ok && db_block_save(&ndb, &blk);
        }
        ok = ok && (ar_test_save_count == 5);
        ok = ok && (db_block_count(&ndb) == 5);

        ar_callbacks_init(cbs);
        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR before_destroy callback */
    {
        printf("AR before_destroy callback... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        struct ar_callbacks *cbs = db_block_callbacks();
        ar_callbacks_init(cbs);
        ar_register_before_destroy(cbs, ar_test_prevent_delete);

        uint8_t sol[] = {0x01};
        struct db_block blk;
        memset(&blk, 0, sizeof(blk));
        memset(blk.hash, 0xDD, 32);
        memset(blk.prev_hash, 0xBB, 32);
        memset(blk.merkle_root, 0xCC, 32);
        blk.height = 42;
        blk.time = 1700000000;
        blk.bits = 0x1d00ffff;
        blk.status = 3;
        blk.solution = sol;
        blk.solution_len = 1;
        ok = ok && db_block_save(&ndb, &blk);
        ok = ok && (db_block_count(&ndb) == 1);

        /* Delete should be prevented */
        ok = ok && !db_block_delete(&ndb, blk.hash);
        ok = ok && (db_block_count(&ndb) == 1);

        ar_callbacks_init(cbs);
        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR error messages accumulation */
    {
        printf("AR error messages... ");
        struct ar_errors errs;
        ar_errors_clear(&errs);

        bool ok = !ar_errors_any(&errs);
        ar_errors_add(&errs, "height", "must be positive");
        ar_errors_add(&errs, "hash", "can't be blank");
        ar_errors_add(&errs, "time", "can't be zero");
        ok = ok && ar_errors_any(&errs);
        ok = ok && (errs.count == 3);

        char buf[1024];
        ar_errors_full_messages(&errs, buf, sizeof(buf));
        ok = ok && (strstr(buf, "height") != NULL);
        ok = ok && (strstr(buf, "hash") != NULL);
        ok = ok && (strstr(buf, "time") != NULL);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* Block → Transaction relationship (has_many) */
    {
        printf("AR relationship: block has_many transactions... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        /* Reset block callbacks */
        struct ar_callbacks *cbs = db_block_callbacks();
        ar_callbacks_init(cbs);

        /* Save a block */
        uint8_t sol[] = {0x01};
        struct db_block blk;
        memset(&blk, 0, sizeof(blk));
        memset(blk.hash, 0xAA, 32);
        memset(blk.prev_hash, 0xBB, 32);
        memset(blk.merkle_root, 0xCC, 32);
        blk.height = 100;
        blk.time = 1700000000;
        blk.bits = 0x1d00ffff;
        blk.status = 3;
        blk.solution = sol;
        blk.solution_len = 1;
        blk.num_tx = 3;
        ok = ok && db_block_save(&ndb, &blk);

        /* Save 3 transactions belonging to this block */
        for (int i = 0; i < 3; i++) {
            struct db_tx_index tx;
            memset(&tx, 0, sizeof(tx));
            memset(tx.txid, 0x10 + i, 32);
            memcpy(tx.block_hash, blk.hash, 32);
            tx.block_height = 100;
            tx.tx_index = i;
            tx.file_num = 1;
            tx.file_pos = 1000 + i * 200;
            tx.is_coinbase = (i == 0);
            ok = ok && db_tx_save(&ndb, &tx);
        }

        /* Query relationship: block.transactions */
        struct db_tx_index txs[10];
        int count = db_block_transactions(&ndb, blk.hash, txs, 10);
        ok = ok && (count == 3);
        ok = ok && (txs[0].tx_index == 0);
        ok = ok && txs[0].is_coinbase;
        ok = ok && (txs[2].tx_index == 2);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* Transaction → Block relationship (belongs_to) */
    {
        printf("AR relationship: tx belongs_to block... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        struct ar_callbacks *cbs = db_block_callbacks();
        ar_callbacks_init(cbs);

        /* Save a block */
        uint8_t sol[] = {0x01};
        struct db_block blk;
        memset(&blk, 0, sizeof(blk));
        memset(blk.hash, 0xDD, 32);
        memset(blk.prev_hash, 0xEE, 32);
        memset(blk.merkle_root, 0xFF, 32);
        blk.height = 200;
        blk.time = 1700000000;
        blk.bits = 0x1d00ffff;
        blk.status = 3;
        blk.solution = sol;
        blk.solution_len = 1;
        ok = ok && db_block_save(&ndb, &blk);

        /* Save a transaction */
        struct db_tx_index tx;
        memset(&tx, 0, sizeof(tx));
        memset(tx.txid, 0x20, 32);
        memcpy(tx.block_hash, blk.hash, 32);
        tx.block_height = 200;
        tx.tx_index = 0;
        ok = ok && db_tx_save(&ndb, &tx);

        /* Query relationship: tx.block */
        struct db_block found;
        ok = ok && db_tx_block(&ndb, &tx, &found);
        ok = ok && (found.height == 200);
        ok = ok && (memcmp(found.hash, blk.hash, 32) == 0);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* tx_index lookup order used by self-heal fallback */
    {
        printf("AR tx_index: native/reversed lookup... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        uint8_t native_txid[32];
        for (size_t i = 0; i < sizeof(native_txid); i++)
            native_txid[i] = (uint8_t)(i + 1);

        struct db_tx_index native_tx;
        memset(&native_tx, 0, sizeof(native_tx));
        memcpy(native_tx.txid, native_txid, sizeof(native_tx.txid));
        memset(native_tx.block_hash, 0x42, sizeof(native_tx.block_hash));
        native_tx.block_height = 210;
        native_tx.tx_index = 2;
        native_tx.file_num = 3;
        native_tx.file_pos = 400;
        ok = ok && db_tx_save(&ndb, &native_tx);

        struct db_tx_index found;
        bool used_reversed = true;
        memset(&found, 0, sizeof(found));
        ok = ok && db_tx_find_native_or_reversed(&ndb, native_txid,
                                                 &found, &used_reversed);
        ok = ok && !used_reversed;
        ok = ok && (found.block_height == 210);
        ok = ok && (found.tx_index == 2);

        uint8_t native_txid_2[32];
        uint8_t reversed_txid_2[32];
        for (size_t i = 0; i < sizeof(native_txid_2); i++)
            native_txid_2[i] = (uint8_t)(0xa0 + i);
        for (size_t i = 0; i < sizeof(reversed_txid_2); i++)
            reversed_txid_2[i] =
                native_txid_2[sizeof(native_txid_2) - 1 - i];

        struct db_tx_index reversed_tx;
        memset(&reversed_tx, 0, sizeof(reversed_tx));
        memcpy(reversed_tx.txid, reversed_txid_2,
               sizeof(reversed_tx.txid));
        memset(reversed_tx.block_hash, 0x43,
               sizeof(reversed_tx.block_hash));
        reversed_tx.block_height = 211;
        reversed_tx.tx_index = 3;
        reversed_tx.file_num = 4;
        reversed_tx.file_pos = 500;
        ok = ok && db_tx_save(&ndb, &reversed_tx);

        used_reversed = false;
        memset(&found, 0, sizeof(found));
        ok = ok && db_tx_find_native_or_reversed(&ndb, native_txid_2,
                                                 &found, &used_reversed);
        ok = ok && used_reversed;
        ok = ok && (found.block_height == 211);
        ok = ok && (found.tx_index == 3);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* Block → prev/next relationship */
    {
        printf("AR relationship: block prev/next... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        struct ar_callbacks *cbs = db_block_callbacks();
        ar_callbacks_init(cbs);

        /* Save two blocks in sequence */
        uint8_t sol[] = {0x01};
        struct db_block blk1, blk2;
        memset(&blk1, 0, sizeof(blk1));
        memset(blk1.hash, 0x01, 32);
        memset(blk1.prev_hash, 0xFF, 32);
        memset(blk1.merkle_root, 0xAA, 32);
        blk1.height = 1; blk1.time = 1700000000;
        blk1.bits = 0x1d00ffff; blk1.status = 3;
        blk1.solution = sol; blk1.solution_len = 1;
        ok = ok && db_block_save(&ndb, &blk1);

        memset(&blk2, 0, sizeof(blk2));
        memset(blk2.hash, 0x02, 32);
        memcpy(blk2.prev_hash, blk1.hash, 32);
        memset(blk2.merkle_root, 0xBB, 32);
        blk2.height = 2; blk2.time = 1700000100;
        blk2.bits = 0x1d00ffff; blk2.status = 3;
        blk2.solution = sol; blk2.solution_len = 1;
        ok = ok && db_block_save(&ndb, &blk2);

        /* blk2.prev should be blk1 */
        struct db_block prev;
        ok = ok && db_block_prev(&ndb, &blk2, &prev);
        ok = ok && (prev.height == 1);

        /* blk1.next should be blk2 */
        struct db_block next;
        ok = ok && db_block_next(&ndb, &blk1, &next);
        ok = ok && (next.height == 2);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* UTXO → Transaction relationship (belongs_to) */
    {
        printf("AR relationship: utxo belongs_to tx... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        struct ar_callbacks *cbsu = db_utxo_callbacks();
        ar_callbacks_init(cbsu);

        /* Save a tx index entry */
        struct db_tx_index tx;
        memset(&tx, 0, sizeof(tx));
        memset(tx.txid, 0x30, 32);
        memset(tx.block_hash, 0x31, 32);
        tx.block_height = 300;
        ok = ok && db_tx_save(&ndb, &tx);

        /* Save a UTXO referencing that tx */
        uint8_t script[] = {0x76, 0xa9, 0x14}; /* P2PKH prefix */
        struct db_utxo u;
        memset(&u, 0, sizeof(u));
        memcpy(u.txid, tx.txid, 32);
        u.vout = 0;
        u.value = 100000;
        u.script = script;
        u.script_len = sizeof(script);
        u.height = 300;
        ok = ok && db_utxo_save(&ndb, &u);

        /* Query relationship: utxo.transaction */
        struct db_tx_index found_tx;
        ok = ok && db_utxo_transaction(&ndb, &u, &found_tx);
        ok = ok && (found_tx.block_height == 300);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* WalletKey → WalletUTXOs relationship (has_many) */
    {
        printf("AR relationship: wallet_key has_many utxos... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        /* Save a wallet key */
        struct db_wallet_key k;
        memset(&k, 0, sizeof(k));
        memset(k.pubkey_hash, 0x40, 20);
        memset(k.pubkey, 0x41, 33);
        k.pubkey_len = 33;
        memset(k.privkey, 0x42, 32);
        k.compressed = true;
        k.created_at = 1700000000;
        ok = ok && db_wallet_key_save(&ndb, &k);

        /* Save 2 wallet UTXOs for this key */
        uint8_t script[] = {0x76, 0xa9};
        for (int i = 0; i < 2; i++) {
            struct db_wallet_utxo u;
            memset(&u, 0, sizeof(u));
            memset(u.txid, 0x50 + i, 32);
            u.vout = 0;
            u.value = 10000 * (i + 1);
            memcpy(u.address_hash, k.pubkey_hash, 20);
            u.script = script;
            u.script_len = sizeof(script);
            u.height = 400 + i;
            ok = ok && db_wallet_utxo_save(&ndb, &u);
        }

        /* Query relationship: key.utxos */
        struct db_wallet_utxo utxos[10];
        int count = db_wallet_key_utxos(&ndb, k.pubkey_hash, utxos, 10);
        ok = ok && (count == 2);
        /* Ordered by value DESC */
        ok = ok && (utxos[0].value == 20000);
        ok = ok && (utxos[1].value == 10000);

        /* WalletUTXO belongs_to key */
        struct db_wallet_key found_key;
        ok = ok && db_wallet_utxo_key(&ndb, &utxos[0], &found_key);
        ok = ok && (memcmp(found_key.pubkey_hash, k.pubkey_hash, 20) == 0);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR Router — route dispatch and filters */
    {
        printf("AR router: dispatch and filters... ");
        struct ar_router router;
        ar_router_init(&router);

        bool ok = ar_router_add_route(&router, "getbalance", "wallet",
                                        ar_test_handler);
        ok = ok && ar_router_add_route(&router, "getinfo", "misc",
                                        ar_test_handler);
        ok = ok && (router.num_routes == 2);

        /* Find route */
        const struct ar_route *r = ar_router_find(&router, "getbalance");
        ok = ok && (r != NULL);
        ok = ok && (strcmp(r->category, "wallet") == 0);

        /* Nonexistent route */
        ok = ok && (ar_router_find(&router, "nosuchmethod") == NULL);

        /* Dispatch succeeds */
        ok = ok && ar_router_dispatch(&router, "getbalance",
                                       NULL, false, NULL, NULL);

        /* Add global filter */
        ar_router_add_filter(&router, ar_test_auth_filter);

        /* Dispatch with filter passing */
        ar_test_auth_passed = true;
        ok = ok && ar_router_dispatch(&router, "getinfo",
                                       NULL, false, NULL, NULL);

        /* Dispatch with filter failing */
        ar_test_auth_passed = false;
        ok = ok && !ar_router_dispatch(&router, "getinfo",
                                        NULL, false, NULL, NULL);

        /* Per-route filter */
        ar_test_auth_passed = true;
        ar_route_add_filter(&router, "getbalance", ar_test_per_route_filter);

        ar_test_route_reject = false;
        ok = ok && ar_router_dispatch(&router, "getbalance",
                                       NULL, false, NULL, NULL);

        ar_test_route_reject = true;
        ok = ok && !ar_router_dispatch(&router, "getbalance",
                                        NULL, false, NULL, NULL);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR wallet_tx validation */
    {
        printf("AR validation: wallet_tx... ");
        struct ar_errors errs;
        struct db_wallet_tx t;
        memset(&t, 0, sizeof(t));

        bool ok = !db_wallet_tx_validate(&t, &errs);
        ok = ok && ar_errors_any(&errs);

        memset(t.txid, 0x99, 32);
        t.time_received = 1700000000;
        ok = ok && db_wallet_tx_validate(&t, &errs);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR validation: wallet_utxo */
    {
        printf("AR validation: wallet_utxo... ");
        struct ar_errors errs;
        struct db_wallet_utxo u;
        memset(&u, 0, sizeof(u));

        bool ok = !db_wallet_utxo_validate(&u, &errs);
        ok = ok && ar_errors_any(&errs);

        memset(u.txid, 0xAA, 32);
        u.value = 50000;
        u.height = 100;
        ok = ok && db_wallet_utxo_validate(&u, &errs);

        /* Negative value fails */
        u.value = -1;
        ok = ok && !db_wallet_utxo_validate(&u, &errs);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR validation: sapling_note */
    {
        printf("AR validation: sapling_note... ");
        struct ar_errors errs;
        struct db_sapling_note n;
        memset(&n, 0, sizeof(n));

        bool ok = !db_sapling_note_validate(&n, &errs);
        ok = ok && ar_errors_any(&errs);

        memset(n.txid, 0xBB, 32);
        memset(n.ivk, 0xCC, 32);
        memset(n.nullifier, 0xDD, 32);
        memset(n.cm, 0xEE, 32);
        memset(n.pk_d, 0xAA, 32);
        memset(n.diversifier, 0x11, 11);
        memset(n.rcm, 0x22, 32);
        snprintf(n.source, sizeof(n.source), "%s",
                 DB_SAPLING_NOTE_SOURCE_LOCAL);
        n.value = 100000;
        ok = ok && db_sapling_note_validate(&n, &errs);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR save rejects invalid wallet_utxo */
    {
        printf("AR save rejects invalid wallet_utxo... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        struct ar_callbacks *cbs = db_wallet_utxo_callbacks();
        ar_callbacks_init(cbs);

        uint8_t dummy_script[] = {0x76, 0xa9};
        struct db_wallet_utxo u;
        memset(&u, 0, sizeof(u));
        /* txid is zero → invalid */
        u.value = 50000;
        u.height = 100;
        u.script = dummy_script;
        u.script_len = sizeof(dummy_script);
        ok = ok && !db_wallet_utxo_save(&ndb, &u);

        /* Valid save */
        memset(u.txid, 0xAA, 32);
        memset(u.address_hash, 0xBB, 20);
        ok = ok && db_wallet_utxo_save(&ndb, &u);

        int64_t bal = db_wallet_utxo_balance(&ndb);
        ok = ok && (bal == 50000);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR save rejects invalid sapling_note */
    {
        printf("AR save rejects invalid sapling_note... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        struct ar_callbacks *cbs = db_sapling_note_callbacks();
        ar_callbacks_init(cbs);

        struct db_sapling_note n;
        memset(&n, 0, sizeof(n));
        /* All zeros → invalid (txid, ivk, nullifier blank) */
        ok = ok && !db_sapling_note_save(&ndb, &n);

        /* Valid save */
        memset(n.txid, 0xAA, 32);
        memset(n.ivk, 0xBB, 32);
        memset(n.nullifier, 0xCC, 32);
        memset(n.diversifier, 0xDD, 11);
        memset(n.pk_d, 0xEE, 32);
        memset(n.cm, 0xFF, 32);
        memset(n.rcm, 0x11, 32);
        n.value = 200000;
        n.block_height = 500;
        ok = ok && db_sapling_note_save(&ndb, &n);

        int64_t bal = db_sapling_note_balance(&ndb);
        ok = ok && (bal == 200000);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR validation: sapling_key */
    {
        printf("AR validation: sapling_key... ");
        struct ar_errors errs;
        struct db_sapling_key k;
        memset(&k, 0, sizeof(k));

        bool ok = !db_sapling_key_validate(&k, &errs);
        ok = ok && errs.count >= 6; /* ivk, xsk, xfvk, diversifier, pk_d, address */

        memset(k.ivk, 0xAA, 32);
        memset(k.xsk, 0x11, 169);
        memset(k.xfvk, 0xBB, 169);
        memset(k.diversifier, 0xCC, 11);
        memset(k.pk_d, 0xDD, 32);
        snprintf(k.address, sizeof(k.address), "zs1test");
        ok = ok && db_sapling_key_validate(&k, &errs);

        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR save rejects invalid sapling_key */
    {
        printf("AR save rejects invalid sapling_key... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        struct ar_callbacks *cbs = db_sapling_key_callbacks();
        ar_callbacks_init(cbs);

        struct db_sapling_key k;
        memset(&k, 0, sizeof(k));
        ok = ok && !db_sapling_key_save(&ndb, &k);

        memset(k.ivk, 0xAA, 32);
        memset(k.xsk, 0x11, 169);
        memset(k.xfvk, 0xBB, 169);
        memset(k.diversifier, 0xCC, 11);
        memset(k.pk_d, 0xDD, 32);
        snprintf(k.address, sizeof(k.address), "zs1test");
        ok = ok && db_sapling_key_save(&ndb, &k);
        ok = ok && (db_sapling_key_count(&ndb) == 1);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR validation: wallet_seed rejects zero seed */
    {
        printf("AR validation: wallet_seed rejects zero... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        uint8_t zero_seed[32] = {0};
        ok = ok && !db_wallet_seed_save(&ndb, zero_seed, 0);

        uint8_t real_seed[32];
        memset(real_seed, 0x42, 32);
        ok = ok && db_wallet_seed_save(&ndb, real_seed, 0);

        uint8_t loaded[32];
        uint32_t next = 99;
        ok = ok && db_wallet_seed_load(&ndb, loaded, &next);
        ok = ok && (memcmp(loaded, real_seed, 32) == 0);
        ok = ok && (next == 0);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR validation: wallet_script rejects empty */
    {
        printf("AR validation: wallet_script rejects empty... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        struct db_wallet_script sc;
        memset(&sc, 0, sizeof(sc));
        ok = ok && !db_wallet_script_save(&ndb, &sc);

        memset(sc.script_hash, 0xAA, 20);
        ok = ok && !db_wallet_script_save(&ndb, &sc);

        uint8_t rs[] = {0x52, 0x21};
        sc.redeem_script = rs;
        sc.script_len = sizeof(rs);
        ok = ok && db_wallet_script_save(&ndb, &sc);

        struct db_wallet_script found;
        ok = ok && db_wallet_script_find(&ndb, sc.script_hash, &found);
        ok = ok && (found.script_len == 2);
        free(found.redeem_script);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR cascading delete: block → transactions */
    {
        printf("AR cascading delete: block -> transactions... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        struct ar_callbacks *cbs = db_block_callbacks();
        ar_callbacks_init(cbs);

        uint8_t sol[] = {0x01};
        struct db_block blk;
        memset(&blk, 0, sizeof(blk));
        memset(blk.hash, 0xAA, 32);
        memset(blk.prev_hash, 0xBB, 32);
        memset(blk.merkle_root, 0xCC, 32);
        blk.height = 100;
        blk.time = 1700000000;
        blk.bits = 0x1d00ffff;
        blk.status = 3;
        blk.solution = sol;
        blk.solution_len = 1;
        ok = ok && db_block_save(&ndb, &blk);

        /* Add 3 transactions referencing this block */
        for (int i = 0; i < 3; i++) {
            struct db_tx_index tx;
            memset(&tx, 0, sizeof(tx));
            memset(tx.txid, 0x10 + i, 32);
            memcpy(tx.block_hash, blk.hash, 32);
            tx.block_height = 100;
            tx.tx_index = i;
            ok = ok && db_tx_save(&ndb, &tx);
        }

        struct db_tx_index txs[10];
        int count = db_block_transactions(&ndb, blk.hash, txs, 10);
        ok = ok && (count == 3);

        /* Delete block → cascades to transactions */
        ok = ok && db_block_delete(&ndb, blk.hash);
        ok = ok && (db_block_count(&ndb) == 0);

        /* Transactions should be gone */
        count = db_block_transactions(&ndb, blk.hash, txs, 10);
        ok = ok && (count == 0);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR cascading delete: wallet_tx → wallet_utxos */
    {
        printf("AR cascading delete: wallet_tx -> wallet_utxos... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        struct ar_callbacks *cbs_wtx = db_wallet_tx_callbacks();
        ar_callbacks_init(cbs_wtx);
        struct ar_callbacks *cbs_utxo = db_wallet_utxo_callbacks();
        ar_callbacks_init(cbs_utxo);

        uint8_t txid[32];
        memset(txid, 0xAA, 32);
        uint8_t raw[] = {0x01, 0x00};

        struct db_wallet_tx t;
        memset(&t, 0, sizeof(t));
        memcpy(t.txid, txid, 32);
        t.raw_tx = raw;
        t.raw_tx_len = 2;
        t.time_received = 1700000000;
        ok = ok && db_wallet_tx_save(&ndb, &t);

        /* Add 2 UTXOs for this tx */
        uint8_t sc2[] = {0x76, 0xa9};
        for (int i = 0; i < 2; i++) {
            struct db_wallet_utxo u;
            memset(&u, 0, sizeof(u));
            memcpy(u.txid, txid, 32);
            u.vout = (uint32_t)i;
            u.value = 25000 * (i + 1);
            u.height = 100;
            memset(u.address_hash, 0xBB, 20);
            u.script = sc2;
            u.script_len = sizeof(sc2);
            ok = ok && db_wallet_utxo_save(&ndb, &u);
        }

        int64_t bal = db_wallet_utxo_balance(&ndb);
        ok = ok && (bal == 75000);

        /* Delete wallet_tx → cascades to UTXOs */
        ok = ok && db_wallet_tx_delete(&ndb, txid);
        ok = ok && (db_wallet_tx_count(&ndb) == 0);

        /* UTXOs should be gone */
        bal = db_wallet_utxo_balance(&ndb);
        ok = ok && (bal == 0);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR has_many: wallet_tx → wallet_utxos */
    {
        printf("AR has_many: wallet_tx -> wallet_utxos... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        struct ar_callbacks *cbs = db_wallet_utxo_callbacks();
        ar_callbacks_init(cbs);

        uint8_t txid1[32], txid2[32];
        memset(txid1, 0xAA, 32);
        memset(txid2, 0xBB, 32);
        uint8_t sc3[] = {0x76, 0xa9};

        /* 3 UTXOs for tx1, 1 for tx2 */
        for (int i = 0; i < 3; i++) {
            struct db_wallet_utxo u;
            memset(&u, 0, sizeof(u));
            memcpy(u.txid, txid1, 32);
            u.vout = (uint32_t)i;
            u.value = 10000;
            u.height = 100;
            memset(u.address_hash, 0xCC, 20);
            u.script = sc3;
            u.script_len = sizeof(sc3);
            ok = ok && db_wallet_utxo_save(&ndb, &u);
        }
        {
            struct db_wallet_utxo u;
            memset(&u, 0, sizeof(u));
            memcpy(u.txid, txid2, 32);
            u.vout = 0;
            u.value = 50000;
            u.height = 200;
            memset(u.address_hash, 0xDD, 20);
            u.script = sc3;
            u.script_len = sizeof(sc3);
            ok = ok && db_wallet_utxo_save(&ndb, &u);
        }

        struct db_wallet_utxo utxos[10];
        int count = db_wallet_tx_utxos(&ndb, txid1, utxos, 10);
        ok = ok && (count == 3);
        ok = ok && (utxos[0].vout == 0);
        ok = ok && (utxos[1].vout == 1);
        ok = ok && (utxos[2].vout == 2);

        count = db_wallet_tx_utxos(&ndb, txid2, utxos, 10);
        ok = ok && (count == 1);
        ok = ok && (utxos[0].value == 50000);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR has_many: wallet_tx → sapling_notes */
    {
        printf("AR has_many: wallet_tx -> sapling_notes... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        struct ar_callbacks *cbs = db_sapling_note_callbacks();
        ar_callbacks_init(cbs);

        uint8_t txid[32];
        memset(txid, 0xAA, 32);

        for (int i = 0; i < 2; i++) {
            struct db_sapling_note n;
            memset(&n, 0, sizeof(n));
            memcpy(n.txid, txid, 32);
            n.output_index = (uint32_t)i;
            n.value = 100000 * (i + 1);
            memset(n.ivk, 0xBB, 32);
            memset(n.nullifier, 0xCC + i, 32);
            memset(n.diversifier, 0xDD, 11);
            memset(n.pk_d, 0xEE, 32);
            memset(n.cm, 0xFF, 32);
            memset(n.rcm, 0x11, 32);
            n.block_height = 500;
            ok = ok && db_sapling_note_save(&ndb, &n);
        }

        struct db_sapling_note notes[10];
        int count = db_wallet_tx_notes(&ndb, txid, notes, 10);
        ok = ok && (count == 2);
        ok = ok && (notes[0].output_index == 0);
        ok = ok && (notes[0].value == 100000);
        ok = ok && (notes[1].output_index == 1);
        ok = ok && (notes[1].value == 200000);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR before_save callback on wallet_utxo */
    {
        printf("AR before_save callback on wallet_utxo... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        struct ar_callbacks *cbs = db_wallet_utxo_callbacks();
        ar_callbacks_init(cbs);

        /* Register callback that always rejects */
        ar_register_before_save(cbs, ar_test_reject_always);

        uint8_t sc4[] = {0x76, 0xa9};
        struct db_wallet_utxo u;
        memset(&u, 0, sizeof(u));
        memset(u.txid, 0xAA, 32);
        memset(u.address_hash, 0xBB, 20);
        u.height = 100;
        u.script = sc4;
        u.script_len = sizeof(sc4);
        u.value = 50000;
        /* Should be rejected by before_save callback */
        ok = ok && !db_wallet_utxo_save(&ndb, &u);

        /* Remove callback, save should succeed */
        ar_callbacks_init(cbs);
        ok = ok && db_wallet_utxo_save(&ndb, &u);

        int64_t bal = db_wallet_utxo_balance(&ndb);
        ok = ok && (bal == 50000);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR after_save callback fires */
    {
        printf("AR after_save callback fires... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        ar_test_save_count = 0;

        struct ar_callbacks *cbs = db_wallet_utxo_callbacks();
        ar_callbacks_init(cbs);
        ar_register_after_save(cbs, ar_test_count_saves);

        uint8_t sc5[] = {0x76, 0xa9};
        struct db_wallet_utxo u;
        memset(&u, 0, sizeof(u));
        memset(u.txid, 0xAA, 32);
        memset(u.address_hash, 0xBB, 20);
        u.script = sc5;
        u.script_len = sizeof(sc5);
        u.value = 10000;
        u.height = 100;

        ok = ok && db_wallet_utxo_save(&ndb, &u);
        u.vout = 1;
        ok = ok && db_wallet_utxo_save(&ndb, &u);

        ok = ok && (ar_test_save_count == 2);
        ok = ok && (db_wallet_utxo_balance(&ndb) == 20000);

        ar_callbacks_init(cbs);
        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR before_destroy callback halts delete */
    {
        printf("AR before_destroy callback on wallet_tx... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        struct ar_callbacks *cbs = db_wallet_tx_callbacks();
        ar_callbacks_init(cbs);

        uint8_t txid[32];
        memset(txid, 0xAA, 32);
        uint8_t raw[] = {0x01};

        struct db_wallet_tx t;
        memset(&t, 0, sizeof(t));
        memcpy(t.txid, txid, 32);
        t.raw_tx = raw;
        t.raw_tx_len = 1;
        t.time_received = 1700000000;
        ok = ok && db_wallet_tx_save(&ndb, &t);
        ok = ok && (db_wallet_tx_count(&ndb) == 1);

        /* Register callback that always rejects destroy */
        ar_register_before_destroy(cbs, ar_test_reject_always);
        ok = ok && !db_wallet_tx_delete(&ndb, txid);
        ok = ok && (db_wallet_tx_count(&ndb) == 1); /* Still there */

        ar_callbacks_init(cbs);
        ok = ok && db_wallet_tx_delete(&ndb, txid);
        ok = ok && (db_wallet_tx_count(&ndb) == 0);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR callbacks on tx_index model */
    {
        printf("AR callbacks: tx_index before_save/after_save... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        ar_test_save_count = 0;
        struct ar_callbacks *cbs = db_tx_callbacks();
        ar_callbacks_init(cbs);
        ar_register_before_save(cbs, ar_test_reject_zero);
        ar_register_after_save(cbs, ar_test_count_saves);

        struct db_tx_index tx;
        memset(&tx, 0, sizeof(tx));
        memset(tx.txid, 0x11, 32);
        memset(tx.block_hash, 0x22, 32);
        tx.tx_index = 0;
        tx.block_height = 0;
        /* block_height 0 → reject_zero checks height field at offset of db_block;
         * For tx_index, we test that before_save fires. Use reject_always instead. */
        ar_callbacks_init(cbs);
        ar_register_before_save(cbs, ar_test_reject_always);
        ar_register_after_save(cbs, ar_test_count_saves);

        /* before_save rejects */
        ok = ok && !db_tx_save(&ndb, &tx);
        ok = ok && (ar_test_save_count == 0);

        /* Remove reject, save succeeds */
        ar_callbacks_init(cbs);
        ar_register_after_save(cbs, ar_test_count_saves);
        tx.block_height = 100;
        ok = ok && db_tx_save(&ndb, &tx);
        ok = ok && (ar_test_save_count == 1);

        ar_callbacks_init(cbs);
        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR callbacks on tx_index delete */
    {
        printf("AR callbacks: tx_index before_destroy... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        struct ar_callbacks *cbs = db_tx_callbacks();
        ar_callbacks_init(cbs);

        struct db_tx_index tx;
        memset(&tx, 0, sizeof(tx));
        memset(tx.txid, 0x33, 32);
        memset(tx.block_hash, 0x44, 32);
        tx.block_height = 50;
        ok = ok && db_tx_save(&ndb, &tx);
        ok = ok && (db_tx_count(&ndb) == 1);

        /* Register before_destroy that rejects */
        ar_register_before_destroy(cbs, ar_test_prevent_delete);
        ok = ok && !db_tx_delete(&ndb, tx.txid);
        ok = ok && (db_tx_count(&ndb) == 1);

        /* Remove callback, delete succeeds */
        ar_callbacks_init(cbs);
        ok = ok && db_tx_delete(&ndb, tx.txid);
        ok = ok && (db_tx_count(&ndb) == 0);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR callbacks on utxo model */
    {
        printf("AR callbacks: utxo before_destroy... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        struct ar_callbacks *cbs = db_utxo_callbacks();
        ar_callbacks_init(cbs);

        uint8_t script[] = {0x76, 0xa9, 0x14};
        struct db_utxo u;
        memset(&u, 0, sizeof(u));
        memset(u.txid, 0x55, 32);
        u.vout = 0;
        u.value = 100000;
        u.script = script;
        u.script_len = sizeof(script);
        u.height = 300;
        ok = ok && db_utxo_save(&ndb, &u);
        ok = ok && (db_utxo_count(&ndb) == 1);

        ar_register_before_destroy(cbs, ar_test_prevent_delete);
        ok = ok && !db_utxo_delete(&ndb, u.txid, u.vout);
        ok = ok && (db_utxo_count(&ndb) == 1);

        ar_callbacks_init(cbs);
        ok = ok && db_utxo_delete(&ndb, u.txid, u.vout);
        ok = ok && (db_utxo_count(&ndb) == 0);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR callbacks on mempool_entry model */
    {
        printf("AR callbacks: mempool before_save/before_destroy... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        struct ar_callbacks *cbs = db_mempool_callbacks();
        ar_callbacks_init(cbs);

        uint8_t raw[] = {0x01, 0x00, 0x00, 0x00};
        struct db_mempool_entry e;
        memset(&e, 0, sizeof(e));
        memset(e.txid, 0x66, 32);
        e.raw_tx = raw;
        e.raw_tx_len = sizeof(raw);
        e.fee = 1000;
        e.size = 225;
        e.time_added = 1700000000;
        e.height_added = 500;

        /* before_save reject */
        ar_register_before_save(cbs, ar_test_reject_always);
        ok = ok && !db_mempool_save(&ndb, &e);
        ok = ok && (db_mempool_count(&ndb) == 0);

        /* Allow save */
        ar_callbacks_init(cbs);
        ok = ok && db_mempool_save(&ndb, &e);
        ok = ok && (db_mempool_count(&ndb) == 1);

        /* before_destroy reject */
        ar_register_before_destroy(cbs, ar_test_prevent_delete);
        ok = ok && !db_mempool_delete(&ndb, e.txid);
        ok = ok && (db_mempool_count(&ndb) == 1);

        /* Allow delete */
        ar_callbacks_init(cbs);
        ok = ok && db_mempool_delete(&ndb, e.txid);
        ok = ok && (db_mempool_count(&ndb) == 0);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR callbacks on peer model */
    {
        printf("AR callbacks: peer before_save/before_destroy... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        struct ar_callbacks *cbs = db_peer_callbacks();
        ar_callbacks_init(cbs);

        struct db_peer p;
        memset(&p, 0, sizeof(p));
        memset(p.ip, 0x77, 16);
        p.port = 8033;
        p.services = 1;
        p.last_seen = 1700000000;

        /* before_save reject */
        ar_register_before_save(cbs, ar_test_reject_always);
        ok = ok && !db_peer_save(&ndb, &p);
        ok = ok && (db_peer_count(&ndb) == 0);

        /* Allow save */
        ar_callbacks_init(cbs);
        ok = ok && db_peer_save(&ndb, &p);
        ok = ok && (db_peer_count(&ndb) == 1);

        /* before_destroy reject */
        ar_register_before_destroy(cbs, ar_test_prevent_delete);
        ok = ok && !db_peer_delete(&ndb, p.ip, p.port);
        ok = ok && (db_peer_count(&ndb) == 1);

        /* Allow delete */
        ar_callbacks_init(cbs);
        ok = ok && db_peer_delete(&ndb, p.ip, p.port);
        ok = ok && (db_peer_count(&ndb) == 0);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR callbacks on wallet_key model */
    {
        printf("AR callbacks: wallet_key save/delete... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        struct ar_callbacks *cbs = db_wallet_key_callbacks();
        ar_callbacks_init(cbs);

        ar_test_save_count = 0;
        ar_register_after_save(cbs, ar_test_count_saves);

        struct db_wallet_key k;
        memset(&k, 0, sizeof(k));
        memset(k.pubkey_hash, 0x88, 20);
        memset(k.pubkey, 0x99, 33);
        k.pubkey_len = 33;
        memset(k.privkey, 0xAA, 32);
        k.compressed = true;
        k.created_at = 1700000000;
        ok = ok && db_wallet_key_save(&ndb, &k);
        ok = ok && (ar_test_save_count == 1);
        ok = ok && (db_wallet_key_count(&ndb) == 1);

        /* before_destroy reject */
        ar_register_before_destroy(cbs, ar_test_prevent_delete);
        ok = ok && !db_wallet_key_delete(&ndb, k.pubkey_hash);
        ok = ok && (db_wallet_key_count(&ndb) == 1);

        /* Allow delete */
        ar_callbacks_init(cbs);
        ok = ok && db_wallet_key_delete(&ndb, k.pubkey_hash);
        ok = ok && (db_wallet_key_count(&ndb) == 0);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR save rejects invalid wallet_key (has validation gate) */
    {
        printf("AR save rejects invalid wallet_key... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        struct ar_callbacks *cbs = db_wallet_key_callbacks();
        ar_callbacks_init(cbs);

        /* Invalid wallet_key (zero pubkey_hash, zero pubkey, zero privkey) */
        struct db_wallet_key k;
        memset(&k, 0, sizeof(k));
        ok = ok && !db_wallet_key_save(&ndb, &k);
        ok = ok && (db_wallet_key_count(&ndb) == 0);

        /* Valid wallet_key saves */
        memset(k.pubkey_hash, 0xAA, 20);
        memset(k.pubkey, 0xBB, 33);
        k.pubkey_len = 33;
        memset(k.privkey, 0xCC, 32);
        k.compressed = true;
        ok = ok && db_wallet_key_save(&ndb, &k);
        ok = ok && (db_wallet_key_count(&ndb) == 1);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR after_destroy callback fires on mempool */
    {
        printf("AR after_destroy callback fires... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        ar_test_save_count = 0;
        struct ar_callbacks *cbs = db_mempool_callbacks();
        ar_callbacks_init(cbs);
        ar_register_after_destroy(cbs, ar_test_count_saves);

        uint8_t raw[] = {0x01};
        struct db_mempool_entry e;
        memset(&e, 0, sizeof(e));
        memset(e.txid, 0xAA, 32);
        e.raw_tx = raw;
        e.raw_tx_len = 1;
        e.fee = 500;
        e.size = 100;
        e.time_added = 1700000000;
        ok = ok && db_mempool_save(&ndb, &e);
        ok = ok && (ar_test_save_count == 0); /* after_destroy not yet fired */

        ok = ok && db_mempool_delete(&ndb, e.txid);
        ok = ok && (ar_test_save_count == 1); /* after_destroy fired */

        ar_callbacks_init(cbs);
        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* SaplingKey → SaplingNotes relationship (has_many) */
    {
        printf("AR relationship: sapling_key has_many notes... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        struct ar_callbacks *cbs = db_sapling_note_callbacks();
        ar_callbacks_init(cbs);

        /* Save a sapling key */
        struct db_sapling_key sk;
        memset(&sk, 0, sizeof(sk));
        memset(sk.ivk, 0xAA, 32);
        memset(sk.xsk, 0xBB, 169);
        memset(sk.xfvk, 0xCC, 169);
        memset(sk.diversifier, 0xDD, 11);
        memset(sk.pk_d, 0xEE, 32);
        snprintf(sk.address, sizeof(sk.address), "zs1test");
        ok = ok && db_sapling_key_save(&ndb, &sk);

        /* Save 2 notes for this key */
        for (int i = 0; i < 2; i++) {
            struct db_sapling_note n;
            memset(&n, 0, sizeof(n));
            memset(n.txid, 0x50 + i, 32);
            n.output_index = (uint32_t)i;
            n.value = 100000 * (i + 1);
            memcpy(n.ivk, sk.ivk, 32);
            memset(n.nullifier, 0x60 + i, 32);
            memset(n.diversifier, 0xDD, 11);
            memset(n.pk_d, 0xEE, 32);
            memset(n.cm, 0x70 + i, 32);
            memset(n.rcm, 0x80 + i, 32);
            n.block_height = 600;
            ok = ok && db_sapling_note_save(&ndb, &n);
        }

        struct db_sapling_note notes[10];
        int count = db_sapling_key_notes(&ndb, sk.ivk, notes, 10);
        ok = ok && (count == 2);
        ok = ok && (notes[0].value == 200000);
        ok = ok && (notes[1].value == 100000);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* AR validation with save integration — mempool rejects invalid */
    {
        printf("AR save rejects invalid mempool entry... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");
        struct ar_callbacks *cbs = db_mempool_callbacks();
        ar_callbacks_init(cbs);

        struct db_mempool_entry e;
        memset(&e, 0, sizeof(e));
        /* txid is zero, size is zero → fails validation */
        uint8_t raw[] = {0x01};
        e.raw_tx = raw;
        e.raw_tx_len = 1;
        /* mempool_save doesn't call validate (no validation gate in save) —
         * just test that zero-size entry works via explicit validation */
        ok = ok && !db_mempool_validate(&e, &(struct ar_errors){0});

        memset(e.txid, 0xFF, 32);
        e.size = 200;
        e.time_added = 1700000000;
        struct ar_errors errs;
        ok = ok && db_mempool_validate(&e, &errs);
        ok = ok && !ar_errors_any(&errs);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Production before_save hooks ──────────────────────────── */

    /* UTXO before_save rejects value > MAX_MONEY */
    {
        printf("UTXO before_save rejects over MAX_MONEY... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        uint8_t dummy_script[] = {0x76, 0xa9, 0x14,
            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
            0x88, 0xac}; /* P2PKH */
        struct db_utxo u;
        memset(&u, 0, sizeof(u));
        memset(u.txid, 0xAA, 32);
        u.vout = 0;
        u.value = 2100000000000001LL; /* MAX_MONEY + 1 */
        u.height = 100;
        u.script_type = SCRIPT_P2PKH;
        u.script = dummy_script;
        u.script_len = sizeof(dummy_script);

        ok = ok && !db_utxo_save(&ndb, &u);

        /* Valid value should succeed */
        u.value = 100000;
        ok = ok && db_utxo_save(&ndb, &u);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* UTXO before_save rejects negative height */
    {
        printf("UTXO before_save rejects negative height... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        uint8_t dummy_script[] = {0x76, 0xa9, 0x14,
            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
            0x88, 0xac};
        struct db_utxo u;
        memset(&u, 0, sizeof(u));
        memset(u.txid, 0xBB, 32);
        u.vout = 0;
        u.value = 50000;
        u.height = -1;
        u.script_type = SCRIPT_P2PKH;
        u.script = dummy_script;
        u.script_len = sizeof(dummy_script);

        ok = ok && !db_utxo_save(&ndb, &u);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* Block before_save rejects null hash */
    {
        printf("Block before_save rejects null hash... ");
        struct node_db ndb;
        bool ok = node_db_open(&ndb, ":memory:");

        uint8_t sol[] = {0x01};
        struct db_block blk;
        memset(&blk, 0, sizeof(blk));
        /* hash is all zeros (null) */
        memset(blk.prev_hash, 0xBB, 32);
        memset(blk.merkle_root, 0xCC, 32);
        blk.time = 1700000000;
        blk.bits = 0x1d00ffff;
        blk.status = 3;
        blk.solution = sol;
        blk.solution_len = 1;
        blk.height = 1;

        ok = ok && !db_block_save(&ndb, &blk);

        /* Non-null hash should succeed */
        memset(blk.hash, 0xAA, 32);
        ok = ok && db_block_save(&ndb, &blk);

        node_db_close(&ndb);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* This group calls ar_callbacks_init() on the SHARED production wallet
     * model callback structs to silence emits during validation tests, which
     * wipes the before/after_save hooks process-wide. Restore them so later
     * groups (wallet_projection) see the model save emits again. */
    wallet_key_reset_hooks_for_testing();
    wallet_tx_reset_hooks_for_testing();

    return failures;
}
