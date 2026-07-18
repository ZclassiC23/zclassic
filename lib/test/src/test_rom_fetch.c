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
 * The loopback E2E test runs the REAL serve path (fs_server_start on a
 * fixture datadir) and fetches from 127.0.0.1 — proving the client's wire
 * assumptions (zero-root handshake, frame/chunk counter alignment, MAC) and
 * the no-partial-trust discard against the merged server code. */

#include "test/test_helpers.h"
#include "net/rom_fetch.h"
#include "net/rom_seed.h"
#include "net/file_service.h"
#include "crypto/sha3.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "platform/time_compat.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

/* ── (d) Loopback E2E against the real serve path ───────────────────── */

static int test_loopback_e2e(void)
{
    int failures = 0;
    TEST("rom_fetch: loopback E2E fetches + verifies from the real fs serve path") {
        rom_seed_reset();
        /* Raise the byte-rate caps: this test moves ~3x the artifact size
         * inside one wall second and the default 8 MB/s peer window would
         * otherwise make the outcome wall-clock dependent. reset() at the
         * end restores the defaults. */
        rom_seed_set_peer_bps_cap(1ull << 30);
        rom_seed_set_global_bps_cap(1ull << 30);

        char sroot[] = "/tmp/zcl_romfetch_srv_XXXXXX";
        char *sdir = mkdtemp(sroot);
        ASSERT(sdir != NULL);
        char croot[] = "/tmp/zcl_romfetch_cli_XXXXXX";
        char *cdir = mkdtemp(croot);
        ASSERT(cdir != NULL);

        /* 2 chunks: one full 4 MB chunk + a short 4 KB tail. */
        size_t size = (size_t)ROM_SEED_CHUNK_SIZE + 4096;
        uint8_t *content = malloc(size);
        ASSERT(content != NULL);
        gen_content(content, size);
        ASSERT(write_file(sdir, "consensus-state-bundle-e2e.sqlite",
                          content, size));

        struct rom_artifact art;
        ASSERT(rom_seed_register(sdir, "consensus-state-bundle-e2e.sqlite",
                                 NULL, &art) == ROM_REG_OK);
        struct rom_fetch_manifest m;
        manifest_from_artifact(&art, &m);

        /* Start the real serve path on the fixture datadir; retry across a
         * few candidate ports in case one is already bound. */
        static const uint16_t cand_ports[] = { 18099, 18107, 18113 };
        uint16_t port = 0;
        for (size_t i = 0; i < sizeof(cand_ports) / sizeof(cand_ports[0]); i++) {
            fs_server_start(sdir, cand_ports[i]);
            for (int w = 0; w < 40 && !fs_server_is_running(); w++)
                platform_sleep_ms(50); /* up to 2 s for bind+listen */
            if (fs_server_is_running()) {
                port = cand_ports[i];
                break;
            }
            fs_server_stop();
        }
        ASSERT(port != 0);

        /* (1) One chunk over the wire matches the source bytes exactly. */
        uint8_t *cbuf = malloc(ROM_SEED_CHUNK_SIZE);
        ASSERT(cbuf != NULL);
        uint32_t csz = 0;
        ASSERT(rom_fetch_chunk("127.0.0.1", port, m.chunk_root, 1,
                               cbuf, ROM_SEED_CHUNK_SIZE, &csz));
        ASSERT(csz == 4096);
        ASSERT(memcmp(cbuf, content + ROM_SEED_CHUNK_SIZE, 4096) == 0);
        free(cbuf);

        /* (2) Full download through the driver: per-chunk fetch, whole-file
         * verification, atomic rename to the committed filename. */
        ASSERT(rom_fetch_download("127.0.0.1", port, &m, cdir, NULL, NULL));
        char final_path[1024];
        snprintf(final_path, sizeof(final_path), "%s/%s", cdir, m.filename);
        ASSERT(rom_fetch_verify_file(final_path, &m));

        /* (3) A wrong committed whole-digest downloads every chunk, then
         * fails the content proof and unlinks the .part — no partial trust. */
        struct rom_fetch_manifest bad = m;
        bad.whole_sha3[0] ^= 0x01;
        snprintf(bad.filename, sizeof(bad.filename), "%s",
                 "consensus-state-bundle-bad.sqlite");
        ASSERT(!rom_fetch_download("127.0.0.1", port, &bad, cdir, NULL, NULL));
        char bad_path[1024];
        snprintf(bad_path, sizeof(bad_path), "%s/%s%s", cdir, bad.filename,
                 ROM_FETCH_PART_SUFFIX);
        ASSERT(access(bad_path, F_OK) != 0);
        snprintf(bad_path, sizeof(bad_path), "%s/%s", cdir, bad.filename);
        ASSERT(access(bad_path, F_OK) != 0);

        /* (4) An unknown chunk_root is refused by the serve path — the
         * client sees a clean chunk failure, never bogus bytes. */
        uint8_t bogus_root[32];
        memset(bogus_root, 0xA5, sizeof(bogus_root));
        cbuf = malloc(ROM_SEED_CHUNK_SIZE);
        ASSERT(cbuf != NULL);
        csz = 0;
        ASSERT(!rom_fetch_chunk("127.0.0.1", port, bogus_root, 0,
                                cbuf, ROM_SEED_CHUNK_SIZE, &csz));
        free(cbuf);

        /* (5) The fetch status records both attempts (the bogus chunk_root
         * never reached rom_fetch_download, so it is not an attempt). */
        struct rom_fetch_status st;
        rom_fetch_status_snapshot(&st);
        ASSERT(st.ever_attempted && !st.in_progress);
        ASSERT(st.attempts == 2 && st.successes == 1 && st.failures == 1);
        ASSERT(!st.last_ok);
        ASSERT(strstr(st.detail, "digest mismatch") != NULL);
        ASSERT(st.bytes_total == m.size_bytes);

        /* (6) dumpstate body is well-formed and reports the counters. */
        struct json_value dj;
        json_init(&dj);
        ASSERT(rom_fetch_dump_state_json(&dj, NULL));
        ASSERT(dj.type == JSON_OBJ);
        ASSERT(json_get_int(json_get(&dj, "successes")) == 1);
        ASSERT(json_get_int(json_get(&dj, "failures")) == 1);
        ASSERT(json_get(json_get(&dj, "last"), "peer") != NULL);
        json_free(&dj);

        fs_server_stop();

        free(content);
        char p[1024];
        snprintf(p, sizeof(p), "%s/consensus-state-bundle-e2e.sqlite", sdir);
        unlink(p);
        snprintf(p, sizeof(p), "%s/%s", cdir, m.filename);
        unlink(p);
        rmdir(sdir);
        rmdir(cdir);
        rom_seed_reset();
        PASS();
    } _test_next:;
    return failures;
}

/* ── (e) Parallel multi-seeder scheduling ───────────────────────────── */

static int test_parallel_download(void)
{
    int failures = 0;
    TEST("rom_fetch: parallel workers fetch all chunks, fail closed on dead peers") {
        rom_seed_reset();
        rom_seed_set_peer_bps_cap(1ull << 30);
        rom_seed_set_global_bps_cap(1ull << 30);

        char sroot[] = "/tmp/zcl_romfetch_psrv_XXXXXX";
        char *sdir = mkdtemp(sroot);
        ASSERT(sdir != NULL);
        char croot[] = "/tmp/zcl_romfetch_pcli_XXXXXX";
        char *cdir = mkdtemp(croot);
        ASSERT(cdir != NULL);

        /* 3 chunks: two full 4 MB chunks + a 4 KB tail. */
        size_t size = 2 * (size_t)ROM_SEED_CHUNK_SIZE + 4096;
        uint8_t *content = malloc(size);
        ASSERT(content != NULL);
        gen_content(content, size);
        ASSERT(write_file(sdir, "consensus-state-bundle-par.sqlite",
                          content, size));

        struct rom_artifact art;
        ASSERT(rom_seed_register(sdir, "consensus-state-bundle-par.sqlite",
                                 NULL, &art) == ROM_REG_OK);
        struct rom_fetch_manifest m;
        manifest_from_artifact(&art, &m);

        static const uint16_t cand_ports[] = { 18119, 18127, 18133 };
        uint16_t port = 0;
        for (size_t i = 0; i < sizeof(cand_ports) / sizeof(cand_ports[0]); i++) {
            fs_server_start(sdir, cand_ports[i]);
            for (int w = 0; w < 40 && !fs_server_is_running(); w++)
                platform_sleep_ms(50);
            if (fs_server_is_running()) {
                port = cand_ports[i];
                break;
            }
            fs_server_stop();
        }
        ASSERT(port != 0);

        /* (1) 2 workers against 1 peer (the serve-side per-peer inflight
         * cap): every chunk lands at its offset; whole-file proof passes. */
        struct rom_fetch_peer one = { {0}, 0 };
        snprintf(one.addr, sizeof(one.addr), "%s", "127.0.0.1");
        one.port = port;
        ASSERT(rom_fetch_download_parallel(&one, 1, &m, cdir, 2, NULL, NULL));
        char final_path[1024];
        snprintf(final_path, sizeof(final_path), "%s/%s", cdir, m.filename);
        ASSERT(rom_fetch_verify_file(final_path, &m));
        unlink(final_path);

        /* (2) A dead peer fails every chunk -> download fails, .part left. */
        struct rom_fetch_peer dead = { {0}, 0 };
        snprintf(dead.addr, sizeof(dead.addr), "%s", "127.0.0.1");
        dead.port = 1; /* tcpmux: nothing listening */
        ASSERT(!rom_fetch_download_parallel(&dead, 1, &m, cdir, 2, NULL, NULL));
        char part_path[1024];
        snprintf(part_path, sizeof(part_path), "%s/%s%s", cdir, m.filename,
                 ROM_FETCH_PART_SUFFIX);
        struct stat st;
        ASSERT(stat(part_path, &st) == 0); /* left for a future resume */
        unlink(part_path);

        /* (3) Worker count clamps to chunk count (1-chunk artifact, 8 ask). */
        struct rom_fetch_manifest small = m;
        small.size_bytes = 8192;
        small.num_chunks = 1;
        /* Re-derive the digests for the truncated manifest from the source
         * bytes: chunk 0 of the real artifact IS the first 4 MB, so a
         * 8192-byte file needs its own digests. Build them directly. */
        {
            uint8_t head[8192];
            memcpy(head, content, sizeof(head));
            struct sha3_256_ctx whole;
            sha3_256_init(&whole);
            sha3_256_write(&whole, head, sizeof(head));
            sha3_256_finalize(&whole, small.whole_sha3);
            uint8_t ch[32];
            sha3_256(head, sizeof(head), ch);
            struct sha3_256_ctx root;
            sha3_256_init(&root);
            sha3_256_write(&root, ch, 32);
            sha3_256_finalize(&root, small.chunk_root);
        }
        /* The serve path keys chunks on the REGISTERED artifact's root, so
         * the truncated manifest is only exercised against the dead peer:
         * it must fail fast (not hang, not overrun the chunk table). */
        ASSERT(!rom_fetch_download_parallel(&dead, 1, &small, cdir,
                                            ROM_FETCH_MAX_WORKERS, NULL, NULL));
        snprintf(part_path, sizeof(part_path), "%s/%s%s", cdir,
                 small.filename, ROM_FETCH_PART_SUFFIX);
        unlink(part_path);

        fs_server_stop();

        free(content);
        char p[1024];
        snprintf(p, sizeof(p), "%s/consensus-state-bundle-par.sqlite", sdir);
        unlink(p);
        rmdir(sdir);
        rmdir(cdir);
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
    failures += test_loopback_e2e();
    failures += test_parallel_download();
    return failures;
}
