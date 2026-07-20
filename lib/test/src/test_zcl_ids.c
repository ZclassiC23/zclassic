/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Strong-id wrapper types (core/zcl_ids.h). Step-0 contract test: proves the
 * 32-byte wrappers are layout-identical to a bare uint8_t[32] (so wire memcpy
 * stays valid) and the eq/copy helpers behave. WF1 lane 1B extends this with
 * the adoption proofs in flyclient.h / snapshot_manifest.h. */

#include "test/test_helpers.h"
#include "core/zcl_ids.h"
#include <string.h>

static int test_zcl_ids_sizes(void)
{
    int failures = 0;
    TEST("zcl_ids: 32-byte wrappers are exactly 32 bytes (wire-compatible)") {
        ASSERT(sizeof(struct zcl_block_hash) == 32);
        ASSERT(sizeof(struct zcl_sha3_digest) == 32);
        ASSERT(sizeof(struct zcl_utxo_root) == 32);
        ASSERT(sizeof(struct zcl_mmb_root) == 32);
        ASSERT(sizeof(struct zcl_chunk_root) == 32);
        ASSERT(sizeof(struct zcl_artifact_id) == 32);
        ASSERT(sizeof(struct zcl_chainwork_be256) == 32);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zcl_ids_scalar_sizes(void)
{
    int failures = 0;
    TEST("zcl_ids: scalar wrappers wrap their exact scalar") {
        ASSERT(sizeof(struct zcl_height) == sizeof(int32_t));
        ASSERT(sizeof(struct zcl_byte_count) == sizeof(uint64_t));
        ASSERT(sizeof(struct zcl_chunk_index) == sizeof(uint32_t));
        ASSERT(sizeof(struct zcl_peer_id) == sizeof(uint64_t));
        PASS();
    } _test_next:;
    return failures;
}

static int test_zcl_ids_hash_helpers(void)
{
    int failures = 0;
    TEST("zcl_ids: hash eq/copy round-trip and stay memcpy-compatible") {
        struct zcl_chunk_root a, b;
        for (int i = 0; i < 32; i++) a.bytes[i] = (uint8_t)(i * 7 + 1);
        zcl_chunk_root_copy(&b, &a);
        ASSERT(zcl_chunk_root_eq(&a, &b));
        b.bytes[13] ^= 0xFF;
        ASSERT(!zcl_chunk_root_eq(&a, &b));

        /* Wire boundary: a bare 32-byte memcpy into .bytes still works. */
        uint8_t wire[32];
        memset(wire, 0xAB, sizeof(wire));
        struct zcl_utxo_root r;
        memcpy(r.bytes, wire, 32);
        ASSERT(memcmp(r.bytes, wire, 32) == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_zcl_ids_scalar_helpers(void)
{
    int failures = 0;
    TEST("zcl_ids: scalar eq/copy behave") {
        struct zcl_height h = { .value = -1 }, h2;
        zcl_height_copy(&h2, h);
        ASSERT(zcl_height_eq(h, h2));
        struct zcl_chunk_index i1 = { .value = 42 }, i2 = { .value = 43 };
        ASSERT(!zcl_chunk_index_eq(i1, i2));
        PASS();
    } _test_next:;
    return failures;
}

int test_zcl_ids(void)
{
    int failures = 0;
    failures += test_zcl_ids_sizes();
    failures += test_zcl_ids_scalar_sizes();
    failures += test_zcl_ids_hash_helpers();
    failures += test_zcl_ids_scalar_helpers();
    return failures;
}
