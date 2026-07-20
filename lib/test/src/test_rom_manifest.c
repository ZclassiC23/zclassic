/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Per-chunk ROM manifest protocol (net/file_service.h, net/rom_seed.h,
 * net/rom_fetch.h, net/rom_journal.h). Covers the WF2 artifact-protocol serve
 * + client cores that ride the file-service transport:
 *   - the RMF request wire size + parse,
 *   - rom_seed_manifest_blob serialization (golden bytes + bounds),
 *   - rom_fetch_parse_manifest_blob parse/verify (round-trip, bad version,
 *     truncation, non-multiple length, over-cap, chunk-root fold mismatch),
 *   - rom_fetch_verify_chunk positive/negative,
 *   - the resume journal (open fresh, mark, count, reopen-resume, is_done,
 *     header-mismatch discard, discard file).
 * All pure/local — no network, no live seeder. */

#include "test/test_helpers.h"
#include "net/file_service.h"
#include "net/rom_seed.h"
#include "net/rom_fetch.h"
#include "net/rom_journal.h"
#include "crypto/sha3.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

/* Fold per-chunk digests into the artifact chunk_root exactly as the seeder
 * (rom_seed_register) and the client parser do: SHA3 over their concatenation. */
static void fold_chunk_root(const uint8_t (*digests)[32], uint32_t n,
                            uint8_t out_root[32])
{
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    for (uint32_t i = 0; i < n; i++)
        sha3_256_write(&ctx, digests[i], 32);
    sha3_256_finalize(&ctx, out_root);
}

static int test_rom_manifest_request_sizes(void)
{
    int failures = 0;
    TEST("rom_manifest: RMF request size and manifest-blob bound are pinned") {
        ASSERT(FS_ROM_MANIFEST_REQUEST_SIZE == 35);
        ASSERT(ROM_SEED_MANIFEST_BLOB_MAX == 8u + ROM_SEED_MAX_CHUNKS * 32u);
        PASS();
    } _test_next:;
    return failures;
}

static int test_rom_manifest_request_parse(void)
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

        /* Reject: too short, wrong magic, NULL. */
        ASSERT(!fs_parse_rom_manifest_request(req, 34, root));
        req[0] = 'X';
        ASSERT(!fs_parse_rom_manifest_request(req, sizeof(req), root));
        ASSERT(!fs_parse_rom_manifest_request(NULL, sizeof(req), root));
        PASS();
    } _test_next:;
    return failures;
}

static int test_rom_manifest_blob_golden(void)
{
    int failures = 0;
    TEST("rom_manifest: manifest-blob serializes to exact golden bytes") {
        struct rom_artifact a;
        memset(&a, 0, sizeof(a));
        a.num_chunks = 2;
        memset(a.chunk_sha3[0], 0xAA, 32);
        memset(a.chunk_sha3[1], 0xBB, 32);

        uint8_t buf[ROM_SEED_MANIFEST_BLOB_MAX];
        size_t n = rom_seed_manifest_blob(&a, buf, sizeof(buf));
        ASSERT(n == 8u + 2u * 32u); /* 72 */

        /* [u32 version=1 LE][u32 num_chunks=2 LE][32×AA][32×BB]. */
        ASSERT(buf[0] == 0x01 && buf[1] == 0 && buf[2] == 0 && buf[3] == 0);
        ASSERT(buf[4] == 0x02 && buf[5] == 0 && buf[6] == 0 && buf[7] == 0);
        for (int i = 0; i < 32; i++) ASSERT(buf[8 + i] == 0xAA);
        for (int i = 0; i < 32; i++) ASSERT(buf[40 + i] == 0xBB);
        PASS();
    } _test_next:;
    return failures;
}

static int test_rom_manifest_blob_bounds(void)
{
    int failures = 0;
    TEST("rom_manifest: manifest-blob fails closed on bad args / capacity") {
        struct rom_artifact a;
        memset(&a, 0, sizeof(a));
        a.num_chunks = 3;
        uint8_t buf[ROM_SEED_MANIFEST_BLOB_MAX];

        /* NULL args → 0, no write. */
        ASSERT(rom_seed_manifest_blob(NULL, buf, sizeof(buf)) == 0);
        ASSERT(rom_seed_manifest_blob(&a, NULL, sizeof(buf)) == 0);
        ASSERT(rom_seed_manifest_blob(&a, buf, 0) == 0);

        /* Capacity one byte short of need (8 + 96 = 104) → 0. */
        ASSERT(rom_seed_manifest_blob(&a, buf, 103) == 0);
        ASSERT(rom_seed_manifest_blob(&a, buf, 104) == 104);

        /* Zero / over-max chunk count → 0. */
        a.num_chunks = 0;
        ASSERT(rom_seed_manifest_blob(&a, buf, sizeof(buf)) == 0);
        a.num_chunks = ROM_SEED_MAX_CHUNKS + 1;
        ASSERT(rom_seed_manifest_blob(&a, buf, sizeof(buf)) == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_rom_manifest_parse_roundtrip(void)
{
    int failures = 0;
    TEST("rom_manifest: blob round-trips through parse against folded root") {
        struct rom_artifact a;
        memset(&a, 0, sizeof(a));
        a.num_chunks = 4;
        for (uint32_t c = 0; c < a.num_chunks; c++)
            memset(a.chunk_sha3[c], (int)(0x10 + c), 32);
        uint8_t root[32];
        fold_chunk_root(a.chunk_sha3, a.num_chunks, root);

        uint8_t buf[ROM_SEED_MANIFEST_BLOB_MAX];
        size_t n = rom_seed_manifest_blob(&a, buf, sizeof(buf));
        ASSERT(n == 8u + 4u * 32u);

        uint8_t out[ROM_SEED_MAX_CHUNKS][32];
        uint32_t nc = 0;
        ASSERT(rom_fetch_parse_manifest_blob(buf, n, root, out,
                                             ROM_SEED_MAX_CHUNKS, &nc));
        ASSERT(nc == 4);
        for (uint32_t c = 0; c < nc; c++)
            ASSERT(memcmp(out[c], a.chunk_sha3[c], 32) == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_rom_manifest_parse_rejects(void)
{
    int failures = 0;
    TEST("rom_manifest: parse rejects bad version/length/cap/fold") {
        struct rom_artifact a;
        memset(&a, 0, sizeof(a));
        a.num_chunks = 3;
        for (uint32_t c = 0; c < a.num_chunks; c++)
            memset(a.chunk_sha3[c], (int)(0x20 + c), 32);
        uint8_t root[32];
        fold_chunk_root(a.chunk_sha3, a.num_chunks, root);

        uint8_t buf[ROM_SEED_MANIFEST_BLOB_MAX];
        size_t n = rom_seed_manifest_blob(&a, buf, sizeof(buf)); /* 104 */

        uint8_t out[ROM_SEED_MAX_CHUNKS][32];
        uint32_t nc = 99;

        /* Correct blob but WRONG committed root → fold mismatch → reject. */
        uint8_t wrong_root[32];
        memcpy(wrong_root, root, 32);
        wrong_root[0] ^= 0xFF;
        ASSERT(!rom_fetch_parse_manifest_blob(buf, n, wrong_root, out,
                                              ROM_SEED_MAX_CHUNKS, &nc));
        ASSERT(nc == 0); /* cleared on failure */

        /* Bad version (0). */
        uint8_t bad[ROM_SEED_MANIFEST_BLOB_MAX];
        memcpy(bad, buf, n);
        bad[0] = 0x00;
        ASSERT(!rom_fetch_parse_manifest_blob(bad, n, root, out,
                                              ROM_SEED_MAX_CHUNKS, &nc));

        /* Truncated (len < 8). */
        ASSERT(!rom_fetch_parse_manifest_blob(buf, 7, root, out,
                                              ROM_SEED_MAX_CHUNKS, &nc));
        /* Non-multiple-of-32 body length. */
        ASSERT(!rom_fetch_parse_manifest_blob(buf, n - 1, root, out,
                                              ROM_SEED_MAX_CHUNKS, &nc));
        /* num_chunks field says 3 but caller cap is 2 → over-cap reject. */
        ASSERT(!rom_fetch_parse_manifest_blob(buf, n, root, out, 2, &nc));
        /* Oversize len beyond the blob cap. */
        ASSERT(!rom_fetch_parse_manifest_blob(buf, ROM_SEED_MANIFEST_BLOB_MAX + 1,
                                              root, out, ROM_SEED_MAX_CHUNKS,
                                              &nc));
        /* NULL args. */
        ASSERT(!rom_fetch_parse_manifest_blob(NULL, n, root, out,
                                              ROM_SEED_MAX_CHUNKS, &nc));
        PASS();
    } _test_next:;
    return failures;
}

static int test_rom_verify_chunk(void)
{
    int failures = 0;
    TEST("rom_manifest: verify_chunk accepts matching digest, rejects tamper") {
        const uint8_t data[64] = { 1, 2, 3, 4, 5 };
        uint8_t digest[32];
        sha3_256(data, sizeof(data), digest);

        ASSERT(rom_fetch_verify_chunk(data, sizeof(data), digest));

        /* One flipped payload byte → mismatch. */
        uint8_t tampered[64];
        memcpy(tampered, data, sizeof(data));
        tampered[10] ^= 0x01;
        ASSERT(!rom_fetch_verify_chunk(tampered, sizeof(tampered), digest));

        /* One flipped digest byte → mismatch. */
        uint8_t bad_digest[32];
        memcpy(bad_digest, digest, 32);
        bad_digest[0] ^= 0x80;
        ASSERT(!rom_fetch_verify_chunk(data, sizeof(data), bad_digest));

        /* NULL args → false. */
        ASSERT(!rom_fetch_verify_chunk(NULL, 0, digest));
        ASSERT(!rom_fetch_verify_chunk(data, sizeof(data), NULL));
        PASS();
    } _test_next:;
    return failures;
}

static int test_rom_journal_mark_count_resume(void)
{
    int failures = 0;
    TEST("rom_manifest: journal mark/count/reopen-resume and header discard") {
        mkdir("./test-tmp", 0755);
        char path[256];
        snprintf(path, sizeof(path), "./test-tmp/%d_rom_journal.tmp", getpid());
        (void)rom_journal_discard(path); /* clean slate */

        uint8_t root[32], whole[32];
        memset(root, 0x11, 32);
        memset(whole, 0x22, 32);
        const uint32_t chunk_size = ROM_SEED_CHUNK_SIZE;
        const uint32_t num_chunks = 10;

        /* Fresh journal: nothing done. */
        struct rom_journal *j = rom_journal_open(path, root, whole,
                                                 chunk_size, num_chunks);
        ASSERT(j != NULL);
        ASSERT(rom_journal_count_done(j) == 0);
        ASSERT(!rom_journal_is_done(j, 0));

        /* Mark chunks 0, 3, 9; idempotent re-mark; out-of-range rejected. */
        ASSERT(rom_journal_mark(j, 0));
        ASSERT(rom_journal_mark(j, 3));
        ASSERT(rom_journal_mark(j, 9));
        ASSERT(rom_journal_mark(j, 3)); /* idempotent — count unchanged */
        ASSERT(!rom_journal_mark(j, num_chunks)); /* out of range */
        ASSERT(rom_journal_count_done(j) == 3);
        ASSERT(rom_journal_is_done(j, 0));
        ASSERT(rom_journal_is_done(j, 3));
        ASSERT(rom_journal_is_done(j, 9));
        ASSERT(!rom_journal_is_done(j, 1));
        rom_journal_close(j);

        /* Reopen with the SAME identity → resume, bits preserved. */
        j = rom_journal_open(path, root, whole, chunk_size, num_chunks);
        ASSERT(j != NULL);
        ASSERT(rom_journal_count_done(j) == 3);
        ASSERT(rom_journal_is_done(j, 9));
        ASSERT(!rom_journal_is_done(j, 5));
        rom_journal_close(j);

        /* Reopen with a DIFFERENT chunk_root → header mismatch → discard,
         * fresh journal reports zero done. */
        uint8_t other_root[32];
        memset(other_root, 0x33, 32);
        j = rom_journal_open(path, other_root, whole, chunk_size, num_chunks);
        ASSERT(j != NULL);
        ASSERT(rom_journal_count_done(j) == 0);
        rom_journal_close(j);

        /* Different num_chunks also mismatches → fresh. */
        j = rom_journal_open(path, root, whole, chunk_size, num_chunks + 1);
        ASSERT(j != NULL);
        ASSERT(rom_journal_count_done(j) == 0);
        rom_journal_close(j);

        (void)rom_journal_discard(path);
        struct stat st;
        ASSERT(stat(path, &st) != 0); /* gone */
        PASS();
    } _test_next:;
    return failures;
}

int test_rom_manifest(void)
{
    int failures = 0;
    failures += test_rom_manifest_request_sizes();
    failures += test_rom_manifest_request_parse();
    failures += test_rom_manifest_blob_golden();
    failures += test_rom_manifest_blob_bounds();
    failures += test_rom_manifest_parse_roundtrip();
    failures += test_rom_manifest_parse_rejects();
    failures += test_rom_verify_chunk();
    failures += test_rom_journal_mark_count_resume();
    return failures;
}
