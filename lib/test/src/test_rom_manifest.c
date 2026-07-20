/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Per-chunk ROM manifest wire surface (net/file_service.h, net/rom_seed.h,
 * net/rom_fetch.h). Step-0 contract test: the RMF request size/parse and the
 * manifest-blob bound. WF2 lanes 2A/2D land the real serializer + golden wire
 * vectors + malformed-manifest fuzz. */

#include "test/test_helpers.h"
#include "net/file_service.h"
#include "net/rom_seed.h"
#include "net/rom_fetch.h"
#include <string.h>

static int test_rom_manifest_request_sizes(void)
{
    int failures = 0;
    TEST("rom_manifest: RMF request size and manifest-blob bound are pinned") {
        ASSERT(FS_ROM_MANIFEST_REQUEST_SIZE == 35);
        /* [u32 version][u32 num_chunks][num_chunks × 32B], capped at MAX. */
        ASSERT(ROM_SEED_MANIFEST_BLOB_MAX == 8u + ROM_SEED_MAX_CHUNKS * 32u);
        PASS();
    } _test_next:;
    return failures;
}

static int test_rom_manifest_parse_roundtrip(void)
{
    int failures = 0;
    TEST("rom_manifest: well-formed RMF request parses and round-trips root") {
        uint8_t req[FS_ROM_MANIFEST_REQUEST_SIZE];
        memcpy(req, "RMF", 3);
        for (int i = 0; i < 32; i++) req[3 + i] = (uint8_t)(i + 1);

        uint8_t root[32];
        memset(root, 0, sizeof(root));
        ASSERT(fs_parse_rom_manifest_request(req, sizeof(req), root));
        ASSERT(memcmp(root, req + 3, 32) == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_rom_manifest_parse_rejects_malformed(void)
{
    int failures = 0;
    TEST("rom_manifest: wrong magic / short buffer is rejected") {
        uint8_t req[FS_ROM_MANIFEST_REQUEST_SIZE];
        memcpy(req, "RMF", 3);
        memset(req + 3, 0, 32);
        uint8_t root[32];

        /* Too short. */
        ASSERT(!fs_parse_rom_manifest_request(req, 34, root));
        /* Wrong magic. */
        req[0] = 'X';
        ASSERT(!fs_parse_rom_manifest_request(req, sizeof(req), root));
        /* NULL payload. */
        ASSERT(!fs_parse_rom_manifest_request(NULL, sizeof(req), root));
        PASS();
    } _test_next:;
    return failures;
}

static int test_rom_manifest_blob_stub_fails_closed(void)
{
    int failures = 0;
    TEST("rom_manifest: manifest-blob stub fails closed on bad args") {
        uint8_t buf[64];
        /* Step-0 stub returns 0; NULL args must never write. */
        ASSERT(rom_seed_manifest_blob(NULL, buf, sizeof(buf)) == 0);
        PASS();
    } _test_next:;
    return failures;
}

int test_rom_manifest(void)
{
    int failures = 0;
    failures += test_rom_manifest_request_sizes();
    failures += test_rom_manifest_parse_roundtrip();
    failures += test_rom_manifest_parse_rejects_malformed();
    failures += test_rom_manifest_blob_stub_fails_closed();
    return failures;
}
