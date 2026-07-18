/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ROM artifact fetching: manifest sanity + directory parse + whole-file
 * content verification (lib/net/src/rom_fetch.c).
 *
 * Proves the client side of the ROM delivery trust model:
 *   1. a committed manifest is range-checked (sizes, chunking, filename);
 *   2. the /directory.json `artifacts` array parses into bounded, validated
 *      manifests (bad entries skipped, never fatal);
 *   3. whole-file verification re-derives the chunk-root fold + whole-file
 *      digest and agrees with the SERVE side's own registration digests
 *      (rom_seed_register) — pass on the committed bytes, fail closed on
 *      any digest/size/content mismatch.
 *
 * All fixtures live under a mkdtemp() dir in /tmp — never a real datadir.
 * The network chunk-fetch path (rom_fetch_chunk / rom_fetch_download) is
 * exercised by the loopback E2E test planned in
 * docs/work/wt-rom-fetch-engine.md, not here. */

#include "test/test_helpers.h"
#include "net/rom_fetch.h"
#include "net/rom_seed.h"
#include "crypto/sha3.h"
#include "encoding/utilstrencodings.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void gen_content(uint8_t *buf, size_t size)
{
    static const uint8_t magic[16] = "SQLite format 3";
    for (size_t i = 0; i < size; i++)
        buf[i] = (uint8_t)((i * 131u + 7u) & 0xffu);
    if (size >= 16)
        memcpy(buf, magic, 16);
}

static bool write_file(const char *dir, const char *name,
                       const uint8_t *buf, size_t size)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < size) {
        ssize_t w = write(fd, buf + off, size - off);
        if (w <= 0) { close(fd); return false; }
        off += (size_t)w;
    }
    close(fd);
    return true;
}

/* Fill a committed manifest from a serve-side registered artifact. */
static void manifest_from_artifact(const struct rom_artifact *a,
                                   struct rom_fetch_manifest *m)
{
    memset(m, 0, sizeof(*m));
    m->used = true;
    snprintf(m->filename, sizeof(m->filename), "%s", a->filename);
    m->size_bytes = a->size_bytes;
    m->chunk_size = a->chunk_size;
    m->num_chunks = a->num_chunks;
    memcpy(m->chunk_root, a->chunk_root, 32);
    memcpy(m->whole_sha3, a->whole_sha3, 32);
}

/* ── (a) Manifest sanity ────────────────────────────────────────────── */

static int test_manifest_sane(void)
{
    int failures = 0;
    TEST("rom_fetch: manifest sanity accepts valid, rejects out-of-range") {
        struct rom_fetch_manifest m;
        memset(&m, 0, sizeof(m));
        m.size_bytes = (uint64_t)ROM_SEED_CHUNK_SIZE + 4096;
        m.chunk_size = ROM_SEED_CHUNK_SIZE;
        m.num_chunks = 2;
        snprintf(m.filename, sizeof(m.filename), "%s",
                 "consensus-state-bundle-100.sqlite");
        ASSERT(rom_fetch_manifest_sane(&m));

        /* Empty filename allowed at discovery time. */
        m.filename[0] = '\0';
        ASSERT(rom_fetch_manifest_sane(&m));

        /* Wrong chunk size. */
        m.chunk_size = 1024 * 1024;
        ASSERT(!rom_fetch_manifest_sane(&m));
        m.chunk_size = ROM_SEED_CHUNK_SIZE;

        /* Inconsistent chunk count. */
        m.num_chunks = 3;
        ASSERT(!rom_fetch_manifest_sane(&m));
        m.num_chunks = 2;

        /* Below the minimum artifact size. */
        m.size_bytes = 100;
        m.num_chunks = 1;
        ASSERT(!rom_fetch_manifest_sane(&m));
        m.size_bytes = (uint64_t)ROM_SEED_CHUNK_SIZE + 4096;
        m.num_chunks = 2;

        /* Traversal / separator filenames rejected. */
        snprintf(m.filename, sizeof(m.filename), "%s", "../evil.sqlite");
        ASSERT(!rom_fetch_manifest_sane(&m));
        snprintf(m.filename, sizeof(m.filename), "%s", "a/b.sqlite");
        ASSERT(!rom_fetch_manifest_sane(&m));

        ASSERT(!rom_fetch_manifest_sane(NULL));
        PASS();
    } _test_next:;
    return failures;
}

/* ── (b) Directory parse ────────────────────────────────────────────── */

static int test_parse_directory(void)
{
    int failures = 0;
    TEST("rom_fetch: directory.json artifacts parse bounded + validated") {
        struct rom_fetch_manifest out[ROM_FETCH_MAX_ARTIFACTS];
        memset(out, 0, sizeof(out));

        /* Two valid artifacts + one bad-digest entry + one inconsistent
         * layout entry: the valid ones parse, the bad ones are skipped. */
        const char *doc =
            "{\"nodes\":[],\"count\":0,\"artifacts\":["
            "{\"kind\":\"consensus_bundle\","
            "\"digest\":\"00112233445566778899aabbccddeeff"
            "00112233445566778899aabbccddeeff\","
            "\"whole_sha3\":\"ffffffffffffffffffffffffffffffff"
            "ffffffffffffffffffffffffffffffff\","
            "\"size\":4198400,\"chunk_size\":4194304,\"chunks\":2},"
            "{\"kind\":\"consensus_bundle\","
            "\"digest\":\"zz112233445566778899aabbccddeeff"
            "00112233445566778899aabbccddeeff\","
            "\"whole_sha3\":\"ffffffffffffffffffffffffffffffff"
            "ffffffffffffffffffffffffffffffff\","
            "\"size\":4198400,\"chunk_size\":4194304,\"chunks\":2},"
            "{\"kind\":\"consensus_bundle\","
            "\"digest\":\"00112233445566778899aabbccddeeff"
            "00112233445566778899aabbccddeeff\","
            "\"whole_sha3\":\"ffffffffffffffffffffffffffffffff"
            "ffffffffffffffffffffffffffffffff\","
            "\"size\":4198400,\"chunk_size\":4194304,\"chunks\":7}]}";
        int n = rom_fetch_parse_directory(doc, out, ROM_FETCH_MAX_ARTIFACTS);
        ASSERT_EQ(n, 1);
        ASSERT(out[0].used);
        ASSERT(out[0].size_bytes == 4198400);
        ASSERT(out[0].num_chunks == 2);
        ASSERT(out[0].chunk_root[0] == 0x00 && out[0].chunk_root[1] == 0x11);
        ASSERT(out[0].whole_sha3[0] == 0xff);
        ASSERT(out[0].filename[0] == '\0'); /* directory entries carry no name */

        /* Unparseable JSON → -1. */
        ASSERT_EQ(rom_fetch_parse_directory("{not json", out,
                                           ROM_FETCH_MAX_ARTIFACTS), -1);
        /* Valid JSON without the artifacts key → 0, not an error. */
        ASSERT_EQ(rom_fetch_parse_directory("{\"nodes\":[]}", out,
                                           ROM_FETCH_MAX_ARTIFACTS), 0);
        ASSERT_EQ(rom_fetch_parse_directory(NULL, out,
                                           ROM_FETCH_MAX_ARTIFACTS), -1);
        PASS();
    } _test_next:;
    return failures;
}

/* ── (c) Whole-file verification vs serve-side digests ──────────────── */

static int test_verify_file(void)
{
    int failures = 0;
    TEST("rom_fetch: whole-file verify agrees with rom_seed digests, fails closed") {
        rom_seed_reset();
        char root[] = "/tmp/zcl_romfetch_vfy_XXXXXX";
        char *dir = mkdtemp(root);
        ASSERT(dir != NULL);

        /* Two chunks: one full 4 MB chunk + a short 4 KB tail. */
        size_t size = (size_t)ROM_SEED_CHUNK_SIZE + 4096;
        uint8_t *content = malloc(size);
        ASSERT(content != NULL);
        gen_content(content, size);
        ASSERT(write_file(dir, "consensus-state-bundle-100.sqlite",
                          content, size));

        /* The SERVE side's registration re-derives every digest from disk —
         * use it as the independent oracle for the fetch-side verifier. */
        struct rom_artifact art;
        ASSERT(rom_seed_register(dir, "consensus-state-bundle-100.sqlite",
                                 NULL, &art) == ROM_REG_OK);
        struct rom_fetch_manifest m;
        manifest_from_artifact(&art, &m);

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, art.filename);

        /* The committed bytes verify. */
        ASSERT(rom_fetch_verify_file(path, &m));

        /* A wrong whole-file digest fails closed. */
        struct rom_fetch_manifest bad = m;
        bad.whole_sha3[0] ^= 0x01;
        ASSERT(!rom_fetch_verify_file(path, &bad));

        /* A wrong chunk-root fails closed. */
        bad = m;
        bad.chunk_root[31] ^= 0x80;
        ASSERT(!rom_fetch_verify_file(path, &bad));

        /* A wrong committed size fails closed. */
        bad = m;
        bad.size_bytes -= 4096;
        bad.num_chunks = 1;
        ASSERT(!rom_fetch_verify_file(path, &bad));

        /* Tampered content fails closed. */
        ASSERT(write_file(dir, "consensus-state-bundle-100.sqlite",
                          content, size - 1));
        ASSERT(!rom_fetch_verify_file(path, &m));

        free(content);
        char p[1024];
        snprintf(p, sizeof(p), "%s/consensus-state-bundle-100.sqlite", dir);
        unlink(p);
        rmdir(dir);
        rom_seed_reset();
        PASS();
    } _test_next:;
    return failures;
}

int test_rom_fetch(void)
{
    int failures = 0;
    failures += test_manifest_sane();
    failures += test_parse_directory();
    failures += test_verify_file();
    return failures;
}
