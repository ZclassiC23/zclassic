/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for ZCL Market — file sharing serialization, cache, and DB. */

#include "platform/time_compat.h"
#include "test/test_helpers.h"
#include "net/file_market.h"
#include "core/serialize.h"
#include "models/database.h"
#include <stdint.h>

int test_file_market(void)
{
    int failures = 0;

    printf("\n=== File Market Tests ===\n");

    /* ── File offer serialize/deserialize roundtrip ────────────── */

    printf("file_offer serialize+deserialize roundtrip... ");
    {
        struct file_offer offer = {0};
        memset(offer.root_hash, 0xAA, 32);
        snprintf(offer.filename, sizeof(offer.filename), "test-file.dat");
        offer.size_bytes = 104857600;  /* 100 MB */
        offer.num_chunks = 2;
        offer.price_per_mb = 10000;
        memset(offer.z_addr, 0xBB, 43);
        memset(offer.peer_ip, 0, 16);
        offer.peer_ip[12] = 192; offer.peer_ip[13] = 168;
        offer.peer_ip[14] = 1;  offer.peer_ip[15] = 1;
        offer.peer_port = 8033;
        offer.ttl = 3;

        struct byte_stream ws;
        stream_init(&ws, 512);
        bool ser = file_offer_serialize(&offer, &ws);

        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        struct file_offer got = {0};
        bool des = file_offer_deserialize(&got, &rs);

        if (ser && des &&
            memcmp(got.root_hash, offer.root_hash, 32) == 0 &&
            strcmp(got.filename, "test-file.dat") == 0 &&
            got.size_bytes == 104857600 &&
            got.num_chunks == 2 &&
            got.price_per_mb == 10000 &&
            memcmp(got.z_addr, offer.z_addr, 43) == 0 &&
            got.peer_port == 8033 &&
            got.ttl == 3) {
            printf("OK\n");
        } else {
            printf("FAIL (ser=%d des=%d)\n", ser, des);
            failures++;
        }
        stream_free(&ws);
    }

    /* ── File challenge serialize/deserialize roundtrip ────────── */

    printf("file_challenge serialize+deserialize roundtrip... ");
    {
        struct file_challenge chal = {0};
        memset(chal.root_hash, 0xCC, 32);
        chal.chunk_index = 42;

        struct byte_stream ws;
        stream_init(&ws, 128);
        bool ser = file_challenge_serialize(&chal, &ws);

        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        struct file_challenge got = {0};
        bool des = file_challenge_deserialize(&got, &rs);

        if (ser && des &&
            memcmp(got.root_hash, chal.root_hash, 32) == 0 &&
            got.chunk_index == 42) {
            printf("OK\n");
        } else { printf("FAIL\n"); failures++; }
        stream_free(&ws);
    }

    /* ── File proof serialize/deserialize roundtrip ────────────── */

    printf("file_proof serialize+deserialize roundtrip... ");
    {
        struct file_proof proof = {0};
        memset(proof.root_hash, 0xDD, 32);
        proof.chunk_index = 7;
        memset(proof.chunk_hash, 0xEE, 32);

        struct byte_stream ws;
        stream_init(&ws, 128);
        bool ser = file_proof_serialize(&proof, &ws);

        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        struct file_proof got = {0};
        bool des = file_proof_deserialize(&got, &rs);

        if (ser && des &&
            memcmp(got.root_hash, proof.root_hash, 32) == 0 &&
            got.chunk_index == 7 &&
            memcmp(got.chunk_hash, proof.chunk_hash, 32) == 0) {
            printf("OK\n");
        } else { printf("FAIL\n"); failures++; }
        stream_free(&ws);
    }

    /* ── File payment serialize/deserialize roundtrip ──────────── */

    printf("file_payment serialize+deserialize roundtrip... ");
    {
        struct file_payment pay = {0};
        memset(pay.root_hash, 0x11, 32);
        memset(pay.txid, 0x22, 32);
        pay.chunks_paid = 10;
        pay.chunk_start = 5;

        struct byte_stream ws;
        stream_init(&ws, 128);
        bool ser = file_payment_serialize(&pay, &ws);

        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        struct file_payment got = {0};
        bool des = file_payment_deserialize(&got, &rs);

        if (ser && des &&
            memcmp(got.root_hash, pay.root_hash, 32) == 0 &&
            memcmp(got.txid, pay.txid, 32) == 0 &&
            got.chunks_paid == 10 &&
            got.chunk_start == 5) {
            printf("OK\n");
        } else { printf("FAIL\n"); failures++; }
        stream_free(&ws);
    }

    /* ── Offer with long filename ─────────────────────────────── */

    printf("file_offer: long filename truncated to 255... ");
    {
        struct file_offer offer = {0};
        memset(offer.root_hash, 0xFF, 32);
        memset(offer.filename, 'x', 255);
        offer.filename[255] = '\0';
        offer.size_bytes = 1000;
        offer.num_chunks = 1;
        offer.price_per_mb = 100;
        offer.ttl = 1;

        struct byte_stream ws;
        stream_init(&ws, 512);
        bool ser = file_offer_serialize(&offer, &ws);

        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        struct file_offer got = {0};
        bool des = file_offer_deserialize(&got, &rs);

        if (ser && des && strlen(got.filename) == 255)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        stream_free(&ws);
    }

    /* ── Offer with empty filename ────────────────────────────── */

    printf("file_offer: empty filename... ");
    {
        struct file_offer offer = {0};
        memset(offer.root_hash, 0x77, 32);
        offer.filename[0] = '\0';
        offer.size_bytes = 1000;
        offer.num_chunks = 1;
        offer.price_per_mb = 0;
        offer.ttl = 2;

        struct byte_stream ws;
        stream_init(&ws, 256);
        bool ser = file_offer_serialize(&offer, &ws);

        struct byte_stream rs;
        stream_init_from_data(&rs, ws.data, ws.size);
        struct file_offer got = {0};
        bool des = file_offer_deserialize(&got, &rs);

        if (ser && des && got.filename[0] == '\0')
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
        stream_free(&ws);
    }

    /* ── In-memory offer cache ────────────────────────────────── */

    printf("file_market_add_offer: add new offer... ");
    {
        struct file_offer offer = {0};
        memset(offer.root_hash, 0x01, 32);
        snprintf(offer.filename, sizeof(offer.filename), "cached.dat");
        offer.num_chunks = 1;
        offer.ttl = 2;

        bool is_new = file_market_add_offer(&offer);
        if (is_new) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("file_market_add_offer: reject null... ");
    {
        if (!file_market_add_offer(NULL)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("file_market_add_offer: reject zero ttl... ");
    {
        struct file_offer offer = {0};
        offer.ttl = 0;
        offer.num_chunks = 1;
        if (!file_market_add_offer(&offer)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("file_market_add_offer: reject zero chunks... ");
    {
        struct file_offer offer = {0};
        offer.ttl = 1;
        offer.num_chunks = 0;
        if (!file_market_add_offer(&offer)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("file_market_find_offer: find by root_hash... ");
    {
        uint8_t hash[32];
        memset(hash, 0x01, 32);
        struct file_offer found;
        bool ok = file_market_find_offer(hash, &found);
        if (ok && strcmp(found.filename, "cached.dat") == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("file_market_find_offer: miss on unknown hash... ");
    {
        uint8_t hash[32];
        memset(hash, 0xFE, 32);
        struct file_offer found;
        if (!file_market_find_offer(hash, &found)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("file_market_get_offers: returns count... ");
    {
        struct file_offer out[10];
        int count = file_market_get_offers(out, 10);
        if (count >= 1) printf("OK (count=%d)\n", count);
        else { printf("FAIL\n"); failures++; }
    }

    printf("file_market_count: returns count... ");
    {
        int count = file_market_count();
        if (count >= 1) printf("OK (count=%d)\n", count);
        else { printf("FAIL\n"); failures++; }
    }

    printf("file_market_add_offer: update existing (same hash)... ");
    {
        struct file_offer offer = {0};
        memset(offer.root_hash, 0x01, 32);
        snprintf(offer.filename, sizeof(offer.filename), "updated.dat");
        offer.num_chunks = 1;
        offer.ttl = 3;

        bool is_new = file_market_add_offer(&offer);
        if (!is_new) {
            struct file_offer found;
            uint8_t hash[32];
            memset(hash, 0x01, 32);
            file_market_find_offer(hash, &found);
            if (strcmp(found.filename, "updated.dat") == 0)
                printf("OK\n");
            else { printf("FAIL (not updated)\n"); failures++; }
        } else { printf("FAIL (should not be new)\n"); failures++; }
    }

    printf("file_market_prune: removes old offers... ");
    {
        int pruned = file_market_prune(0);
        if (pruned >= 0) printf("OK (pruned=%d)\n", pruned);
        else { printf("FAIL\n"); failures++; }
    }

    /* ── num_chunks overflow guard ──────────────────────── */

    printf("num_chunks: 0 bytes -> 0 chunks... ");
    {
        uint32_t n = 99;
        bool ok = file_market_num_chunks_for_size(0, &n);
        if (ok && n == 0) printf("OK\n");
        else { printf("FAIL (ok=%d n=%u)\n", ok, n); failures++; }
    }

    printf("num_chunks: 1 byte -> 1 chunk... ");
    {
        uint32_t n = 0;
        bool ok = file_market_num_chunks_for_size(1, &n);
        if (ok && n == 1) printf("OK\n");
        else { printf("FAIL (ok=%d n=%u)\n", ok, n); failures++; }
    }

    printf("num_chunks: exactly CHUNK_SIZE -> 1 chunk... ");
    {
        uint32_t n = 0;
        bool ok = file_market_num_chunks_for_size(
            (uint64_t)FILE_MARKET_CHUNK_SIZE, &n);
        if (ok && n == 1) printf("OK\n");
        else { printf("FAIL (ok=%d n=%u)\n", ok, n); failures++; }
    }

    printf("num_chunks: CHUNK_SIZE + 1 -> 2 chunks... ");
    {
        uint32_t n = 0;
        bool ok = file_market_num_chunks_for_size(
            (uint64_t)FILE_MARKET_CHUNK_SIZE + 1, &n);
        if (ok && n == 2) printf("OK\n");
        else { printf("FAIL (ok=%d n=%u)\n", ok, n); failures++; }
    }

    printf("num_chunks: UINT32_MAX * CHUNK_SIZE accepted (at cap)... ");
    {
        uint32_t n = 0;
        uint64_t max_ok =
            (uint64_t)UINT32_MAX * (uint64_t)FILE_MARKET_CHUNK_SIZE;
        bool ok = file_market_num_chunks_for_size(max_ok, &n);
        if (ok && n == UINT32_MAX) printf("OK\n");
        else { printf("FAIL (ok=%d n=%u)\n", ok, n); failures++; }
    }

    printf("num_chunks: UINT32_MAX * CHUNK_SIZE + 1 rejected "
           "(pre-fix wrapped to 0)... ");
    {
        uint32_t n = 99;
        uint64_t over =
            (uint64_t)UINT32_MAX * (uint64_t)FILE_MARKET_CHUNK_SIZE + 1;
        bool ok = file_market_num_chunks_for_size(over, &n);
        if (!ok) printf("OK\n");
        else { printf("FAIL (accepted n=%u for 225 PB file)\n", n); failures++; }
    }

    printf("num_chunks: silent-truncation shape rejected "
           "(225 PB + 5*CHUNK -> would report 4)... ");
    {
        /* The pre-fix expression wraps UINT32_MAX + 5 to 4 — the
         * exact attack shape where a malformed huge file reports a
         * plausible small chunk count instead of the add_offer
         * guard's num_chunks==0 reject. */
        uint32_t n = 99;
        uint64_t shape =
            (uint64_t)UINT32_MAX * (uint64_t)FILE_MARKET_CHUNK_SIZE +
            5 * (uint64_t)FILE_MARKET_CHUNK_SIZE;
        bool ok = file_market_num_chunks_for_size(shape, &n);
        if (!ok) printf("OK\n");
        else { printf("FAIL (accepted n=%u for truncation shape)\n", n); failures++; }
    }

    printf("num_chunks: UINT64_MAX rejected... ");
    {
        uint32_t n = 99;
        bool ok = file_market_num_chunks_for_size(UINT64_MAX, &n);
        if (!ok) printf("OK\n");
        else { printf("FAIL (accepted UINT64_MAX)\n"); failures++; }
    }

    printf("num_chunks: NULL out_chunks rejected... ");
    {
        bool ok = file_market_num_chunks_for_size(1000, NULL);
        if (!ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── SQLite persistence ───────────────────────────────────── */

    printf("file_market DB save+find+list+prune roundtrip... ");
    {
        sqlite3 *db = NULL;
        int rc = sqlite3_open(":memory:", &db);
        if (rc != SQLITE_OK) { printf("FAIL (open)\n"); failures++; }
        else {
            sqlite3_exec(db,
                "CREATE TABLE file_offers("
                "root_hash BLOB PRIMARY KEY, filename TEXT,"
                "size_bytes INTEGER, num_chunks INTEGER,"
                "price_per_mb INTEGER, z_addr BLOB,"
                "peer_ip BLOB, peer_port INTEGER,"
                "ttl INTEGER, last_seen INTEGER)",
                NULL, NULL, NULL);

            struct node_db ndb = { .db = db, .open = true };

            struct file_offer offer = {0};
            memset(offer.root_hash, 0xAA, 32);
            snprintf(offer.filename, sizeof(offer.filename), "test.dat");
            offer.size_bytes = 5000;
            offer.num_chunks = 1;
            offer.price_per_mb = 1000;
            memset(offer.z_addr, 0x23, sizeof(offer.z_addr));
            offer.peer_port = 8233;
            offer.ttl = 2;
            offer.last_seen = (int64_t)platform_time_wall_time_t();

            bool save = db_file_offer_save(&ndb, &offer);
            struct file_offer found = {0};
            bool find = db_file_offer_find(&ndb, offer.root_hash, &found);
            struct file_offer list[10];
            int count = db_file_offer_list(&ndb, list, 10);

            if (save && find &&
                strcmp(found.filename, "test.dat") == 0 &&
                count == 1) {
                printf("OK\n");
            } else {
                printf("FAIL (save=%d find=%d count=%d)\n", save, find, count);
                failures++;
            }

            /* Prune old entries */
            printf("file_market DB prune... ");
            offer.last_seen = 1;  /* very old */
            db_file_offer_save(&ndb, &offer);
            int pruned = db_file_offer_prune(&ndb, 60);
            if (pruned >= 0) printf("OK (pruned=%d)\n", pruned);
            else { printf("FAIL\n"); failures++; }

            sqlite3_close(db);
        }
    }

    printf("\n%d file_market test(s) failed\n", failures);
    return failures;
}
