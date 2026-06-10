/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"
#include "wallet/keystore.h"
#include "wallet/sapling_keys.h"
#include "wallet/wallet.h"
#include "util/safe_alloc.h"

/* basic_keystore is ~40MB (4096 script entries * 10KB each).
 * Must be heap-allocated to avoid stack overflow. */

int test_wallet(void)
{
    int failures = 0;

    printf("keystore_init zeroes state... ");
    {
        struct basic_keystore *ks = zcl_calloc(1, sizeof(struct basic_keystore), "test_keystore");
        if (!ks) { printf("FAIL (alloc)\n"); failures++; goto skip_ks; }
        keystore_init(ks);
        if (ks->num_keys == 0 && ks->num_scripts == 0 && ks->num_watching == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
        keystore_free(ks);
        free(ks);
    }
skip_ks:

    printf("keystore_add_key + keystore_have_key... ");
    {
        struct basic_keystore *ks = zcl_calloc(1, sizeof(struct basic_keystore), "test_keystore");
        if (!ks) { printf("FAIL (alloc)\n"); failures++; goto skip_add; }
        keystore_init(ks);

        struct privkey k;
        privkey_make_new(&k, true);
        struct pubkey pk;
        privkey_get_pubkey(&k, &pk);
        struct key_id kid = pubkey_get_id(&pk);

        bool added = keystore_add_key(ks, &k);
        bool have = keystore_have_key(ks, &kid);

        if (added && have && ks->num_keys == 1)
            printf("OK\n");
        else {
            printf("FAIL (added=%d, have=%d, num_keys=%zu)\n",
                   added, have, ks->num_keys);
            failures++;
        }
        keystore_free(ks);
        free(ks);
    }
skip_add:

    printf("keystore_have_key returns false for unknown key... ");
    {
        struct basic_keystore *ks = zcl_calloc(1, sizeof(struct basic_keystore), "test_keystore");
        if (!ks) { printf("FAIL (alloc)\n"); failures++; goto skip_unknown; }
        keystore_init(ks);

        struct key_id kid;
        memset(kid.id.data, 0xFF, 20);

        if (!keystore_have_key(ks, &kid))
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
        keystore_free(ks);
        free(ks);
    }
skip_unknown:

    printf("keystore_get_key retrieves added key... ");
    {
        struct basic_keystore *ks = zcl_calloc(1, sizeof(struct basic_keystore), "test_keystore");
        if (!ks) { printf("FAIL (alloc)\n"); failures++; goto skip_get; }
        keystore_init(ks);

        struct privkey k;
        privkey_make_new(&k, true);
        struct pubkey pk;
        privkey_get_pubkey(&k, &pk);
        struct key_id kid = pubkey_get_id(&pk);

        keystore_add_key(ks, &k);

        struct privkey retrieved;
        bool got = keystore_get_key(ks, &kid, &retrieved);
        if (got && memcmp(retrieved.vch, k.vch, 32) == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
        keystore_free(ks);
        free(ks);
    }
skip_get:

    printf("keystore_get_pubkey retrieves pubkey... ");
    {
        struct basic_keystore *ks = zcl_calloc(1, sizeof(struct basic_keystore), "test_keystore");
        if (!ks) { printf("FAIL (alloc)\n"); failures++; goto skip_pubkey; }
        keystore_init(ks);

        struct privkey k;
        privkey_make_new(&k, true);
        struct pubkey pk;
        privkey_get_pubkey(&k, &pk);
        struct key_id kid = pubkey_get_id(&pk);

        keystore_add_key(ks, &k);

        struct pubkey retrieved;
        bool got = keystore_get_pubkey(ks, &kid, &retrieved);
        if (got && retrieved.size == pk.size &&
            memcmp(retrieved.vch, pk.vch, pk.size) == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
        keystore_free(ks);
        free(ks);
    }
skip_pubkey:

    printf("wallet_is_mine false for unknown script... ");
    {
        struct wallet *w = zcl_calloc(1, sizeof(struct wallet), "test_wallet");
        if (!w) { printf("FAIL (alloc)\n"); failures++; goto skip_mine1; }
        wallet_init(w);

        struct tx_out txout;
        tx_out_set_null(&txout);
        txout.value = 100000;
        txout.script_pub_key.size = 3;
        txout.script_pub_key.data[0] = OP_TRUE;

        bool mine = wallet_is_mine(w, &txout);
        if (!mine)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
        wallet_free(w);
        free(w);
    }
skip_mine1:

    printf("wallet_is_mine true for own P2PKH output... ");
    {
        struct wallet *w = zcl_calloc(1, sizeof(struct wallet), "test_wallet");
        if (!w) { printf("FAIL (alloc)\n"); failures++; goto skip_mine2; }
        wallet_init(w);

        struct privkey k;
        privkey_make_new(&k, true);
        struct pubkey pk;
        privkey_get_pubkey(&k, &pk);

        keystore_add_key(&w->keystore, &k);

        struct key_id kid = pubkey_get_id(&pk);
        struct tx_destination dest;
        dest.type = DEST_KEY_ID;
        dest.id.key = kid;

        struct tx_out txout;
        tx_out_set_null(&txout);
        txout.value = 100000;
        script_for_destination(&txout.script_pub_key, &dest);

        bool mine = wallet_is_mine(w, &txout);
        if (mine)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
        wallet_free(w);
        free(w);
    }
skip_mine2:

    printf("wallet_get_balance empty wallet returns 0... ");
    {
        struct wallet *w = zcl_calloc(1, sizeof(struct wallet), "test_wallet");
        if (!w) { printf("FAIL (alloc)\n"); failures++; goto skip_bal; }
        wallet_init(w);

        int64_t balance = wallet_get_balance(w);
        if (balance == 0)
            printf("OK\n");
        else {
            printf("FAIL (balance=%" PRId64 ")\n", balance);
            failures++;
        }
        wallet_free(w);
        free(w);
    }
skip_bal:

    printf("wallet_get_unconfirmed_balance empty wallet returns 0... ");
    {
        struct wallet *w = zcl_calloc(1, sizeof(struct wallet), "test_wallet");
        if (!w) { printf("FAIL (alloc)\n"); failures++; goto skip_ubal; }
        wallet_init(w);

        int64_t balance = wallet_get_unconfirmed_balance(w);
        if (balance == 0)
            printf("OK\n");
        else {
            printf("FAIL (balance=%" PRId64 ")\n", balance);
            failures++;
        }
        wallet_free(w);
        free(w);
    }
skip_ubal:

    printf("sapling_keystore_init zeroes state... ");
    {
        struct sapling_keystore sks;
        sapling_keystore_init(&sks);

        if (!sks.has_seed && sks.num_keys == 0 && sks.next_child_index == 0)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
        sapling_keystore_free(&sks);
    }

    printf("sapling_keystore_generate_seed... ");
    {
        struct sapling_keystore sks;
        sapling_keystore_init(&sks);

        bool ok = sapling_keystore_generate_seed(&sks);
        uint8_t zero[32];
        memset(zero, 0, 32);

        if (ok && sks.has_seed && memcmp(sks.seed, zero, 32) != 0)
            printf("OK\n");
        else {
            printf("FAIL (ok=%d, has_seed=%d)\n", ok, sks.has_seed);
            failures++;
        }
        sapling_keystore_free(&sks);
    }

    printf("sapling_keystore_new_address produces valid diversifier... ");
    {
        struct sapling_keystore sks;
        sapling_keystore_init(&sks);
        sapling_keystore_generate_seed(&sks);

        uint8_t diversifier[ZC_DIVERSIFIER_SIZE];
        uint8_t pk_d[32];
        bool ok = sapling_keystore_new_address(&sks, diversifier, pk_d);

        uint8_t zero_d[ZC_DIVERSIFIER_SIZE];
        memset(zero_d, 0, ZC_DIVERSIFIER_SIZE);
        uint8_t zero_pk[32];
        memset(zero_pk, 0, 32);

        if (ok && sks.num_keys == 1 &&
            (memcmp(diversifier, zero_d, ZC_DIVERSIFIER_SIZE) != 0 ||
             memcmp(pk_d, zero_pk, 32) != 0))
            printf("OK\n");
        else {
            printf("FAIL (ok=%d, num_keys=%zu)\n", ok, sks.num_keys);
            failures++;
        }
        sapling_keystore_free(&sks);
    }

    printf("sapling_keystore_new_address increments child index... ");
    {
        struct sapling_keystore sks;
        sapling_keystore_init(&sks);
        sapling_keystore_generate_seed(&sks);

        uint8_t d1[ZC_DIVERSIFIER_SIZE], pk1[32];
        uint8_t d2[ZC_DIVERSIFIER_SIZE], pk2[32];
        bool ok1 = sapling_keystore_new_address(&sks, d1, pk1);
        bool ok2 = sapling_keystore_new_address(&sks, d2, pk2);

        if (ok1 && ok2 && sks.num_keys == 2 && sks.next_child_index == 2)
            printf("OK\n");
        else {
            printf("FAIL (ok1=%d, ok2=%d, num_keys=%zu, next=%u)\n",
                   ok1, ok2, sks.num_keys, sks.next_child_index);
            failures++;
        }
        sapling_keystore_free(&sks);
    }

    printf("sapling_keystore_have_spending_key... ");
    {
        struct sapling_keystore sks;
        sapling_keystore_init(&sks);
        sapling_keystore_generate_seed(&sks);

        uint8_t diversifier[ZC_DIVERSIFIER_SIZE], pk_d[32];
        sapling_keystore_new_address(&sks, diversifier, pk_d);

        const struct sapling_key_entry *entry = &sks.keys[0];
        bool have = sapling_keystore_have_spending_key(&sks, entry->ivk);

        if (have)
            printf("OK\n");
        else {
            printf("FAIL\n");
            failures++;
        }
        sapling_keystore_free(&sks);
    }

    /* Regression: the sapling-note readers race a concurrent append realloc.
     * wallet_copy_sapling_notes() must hand back an independent buffer that
     * survives the live array being freed/moved (the UAF scenario), and the
     * locked accessors must report correct balance / spent state. */
    printf("sapling note snapshot is an independent, lock-safe copy... ");
    {
        struct wallet *w = zcl_calloc(1, sizeof(struct wallet), "test_wallet_sap");
        if (!w) { printf("FAIL (alloc)\n"); failures++; goto skip_sap_snap; }
        wallet_init(w);

        /* Seed two notes directly — the add path is static, and the array is
         * a plain heap buffer, exactly what wallet_add_sapling_note manages. */
        w->sapling_notes = zcl_calloc(2, sizeof(*w->sapling_notes),
                                      "test_sap_notes");
        if (!w->sapling_notes) {
            printf("FAIL (notes alloc)\n"); failures++;
            wallet_free(w); free(w); goto skip_sap_snap;
        }
        w->num_sapling_notes = 2;
        w->sapling_notes_cap = 2;
        w->sapling_notes[0].value = 100;
        w->sapling_notes[0].used = true;
        w->sapling_notes[0].spent = false;
        memset(w->sapling_notes[0].nf, 0xA1, 32);
        w->sapling_notes[1].value = 250;
        w->sapling_notes[1].used = true;
        w->sapling_notes[1].spent = true; /* spent → excluded from balance */
        memset(w->sapling_notes[1].nf, 0xB2, 32);

        int ok = 1;
        /* Balance counts only unspent, used notes. */
        if (wallet_get_sapling_balance(w) != 100) ok = 0;

        /* nullifier_is_spent matches the spent note, not the unspent one. */
        uint8_t nf_spent[32];   memset(nf_spent, 0xB2, 32);
        uint8_t nf_unspent[32]; memset(nf_unspent, 0xA1, 32);
        if (!wallet_sapling_nullifier_is_spent(w, nf_spent)) ok = 0;
        if (wallet_sapling_nullifier_is_spent(w, nf_unspent)) ok = 0;

        size_t snap_n = 0;
        struct sapling_received_note *snap =
            wallet_copy_sapling_notes(w, &snap_n);
        if (!snap || snap_n != 2 || snap == w->sapling_notes) ok = 0;

        if (snap && snap_n == 2) {
            /* Mutating the live array must not perturb the snapshot. */
            w->sapling_notes[0].value = 999999;
            if (snap[0].value != 100 || snap[1].value != 250) ok = 0;

            /* Freeing/replacing the live array (the realloc-UAF scenario)
             * must leave the snapshot fully readable. */
            free(w->sapling_notes);
            w->sapling_notes = NULL;
            w->num_sapling_notes = 0;
            w->sapling_notes_cap = 0;
            if (snap[0].value != 100 || snap[1].value != 250) ok = 0;
        }
        free(snap);

        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
        wallet_free(w);
        free(w);
    }
skip_sap_snap:;

    return failures;
}
