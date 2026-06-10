/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * wallet_recover — Repair C23 wallet LevelDB and dump all private keys.
 * Usage: ./wallet_recover <wallet_dir>
 *
 * Calls leveldb_repair_db() to rebuild MANIFEST from .ldb SST files,
 * then reads all key entries and prints WIF-encoded private keys.
 */

#include <leveldb/c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* Base58 encoding for WIF export */
#include "crypto/sha256.h"
#include "domain/encoding/base58.h"
#include "keys/key.h"
#include "keys/pubkey.h"
#include "keys/key_io.h"
#include "chain/chainparams.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <wallet_dir>\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];

    /* Step 1: Repair the database */
    printf("Repairing LevelDB at %s...\n", path);
    leveldb_options_t *opts = leveldb_options_create();
    char *err = NULL;
    leveldb_repair_db(opts, path, &err);
    if (err) {
        fprintf(stderr, "Repair failed: %s\n", err);
        leveldb_free(err);
        leveldb_options_destroy(opts);
        return 1;
    }
    printf("Repair complete.\n");

    /* Step 2: Open the repaired database */
    leveldb_t *db = leveldb_open(opts, path, &err);
    if (err) {
        fprintf(stderr, "Open failed: %s\n", err);
        leveldb_free(err);
        leveldb_options_destroy(opts);
        return 1;
    }

    /* Step 3: Iterate all entries, find private keys */
    /* Wallet LevelDB key format: 1-byte type + data
     * Type 0x04 = "key" entry: key_id (20 bytes) -> private key (32 bytes)
     * The actual format used by wallet_db is:
     *   key: "key" + compressed_pubkey
     *   value: private_key_data (32 bytes) */

    chain_params_select(CHAIN_MAIN);
    ecc_start();
    ecc_verify_init();

    const struct chain_params *cp = chain_params_get();
    size_t pk_pfx_len, sc_pfx_len;
    const unsigned char *pk_pfx = chain_params_base58_prefix(
        cp, B58_PUBKEY_ADDRESS, &pk_pfx_len);
    const unsigned char *sc_pfx = chain_params_base58_prefix(
        cp, B58_SCRIPT_ADDRESS, &sc_pfx_len);

    leveldb_readoptions_t *ropts = leveldb_readoptions_create();
    leveldb_iterator_t *iter = leveldb_create_iterator(db, ropts);

    int key_count = 0;
    int total_entries = 0;

    for (leveldb_iter_seek_to_first(iter);
         leveldb_iter_valid(iter);
         leveldb_iter_next(iter)) {
        size_t klen = 0, vlen = 0;
        const char *k = leveldb_iter_key(iter, &klen);
        const char *v = leveldb_iter_value(iter, &vlen);
        total_entries++;

        /* Check if this is a "key\x21" entry (key prefix + 33-byte pubkey) */
        if (klen == 3 + 33 && memcmp(k, "key", 3) == 0) {
            const unsigned char *pubkey_data = (const unsigned char *)k + 3;

            /* Value should be the private key (32 bytes, possibly with
             * a length prefix from serialization) */
            const unsigned char *privkey_data = NULL;
            size_t privkey_len = 0;

            if (vlen == 32) {
                privkey_data = (const unsigned char *)v;
                privkey_len = 32;
            } else if (vlen == 33 && (unsigned char)v[0] == 32) {
                /* Length-prefixed: first byte is length */
                privkey_data = (const unsigned char *)v + 1;
                privkey_len = 32;
            } else if (vlen > 32) {
                /* Try finding 32 bytes of key data */
                privkey_data = (const unsigned char *)v + (vlen - 32);
                privkey_len = 32;
            }

            if (privkey_data && privkey_len == 32) {
                /* Reconstruct address from pubkey */
                struct pubkey pk;
                pk.size = 33;
                memcpy(pk.vch, pubkey_data, 33);

                struct key_id kid = pubkey_get_id(&pk);
                struct tx_destination dest;
                dest.type = DEST_KEY_ID;
                dest.id.key = kid;
                char addr[128];
                encode_destination(&dest, pk_pfx, pk_pfx_len,
                                   sc_pfx, sc_pfx_len, addr, sizeof(addr));

                /* Encode as WIF */
                /* WIF = base58check(0x80 + privkey + 0x01[compressed]) */
                unsigned char wif_data[34];
                wif_data[0] = 0x80;
                memcpy(wif_data + 1, privkey_data, 32);
                wif_data[33] = 0x01; /* compressed */
                char wif[128];
                if (encode_base58_check(wif_data, 34, wif, sizeof(wif)))
                    printf("%s %s\n", addr, wif);
                else
                    printf("%s (WIF encode failed)\n", addr);

                key_count++;
            }
        }
    }

    printf("\n%d keys recovered from %d total entries\n", key_count, total_entries);

    leveldb_iter_destroy(iter);
    leveldb_readoptions_destroy(ropts);
    leveldb_close(db);
    leveldb_options_destroy(opts);

    ecc_verify_destroy();
    ecc_stop();

    return 0;
}
