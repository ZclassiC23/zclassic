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
#include "net/rom_journal.h"
#include "net/rom_peer_scoring.h"
#include "net/rom_seed.h"
#include "net/file_service.h"
#include "command/native_command.h"
#include "kernel/command_registry.h"
#include "crypto/sha3.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "platform/time_compat.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

/* Server-side ground truth for how many "ROM" chunks crossed the wire — used
 * to prove resume refetches ONLY the missing chunk (client thread timing can't
 * fudge it; mirrors test_rom_journal_resume.c). */
static int64_t seed_chunks_served(void)
{
    struct json_value dj;
    json_init(&dj);
    (void)rom_seed_dump_state_json(&dj, NULL);
    int64_t n = json_get_int(json_get(&dj, "chunks_served"));
    json_free(&dj);
    return n;
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

/* ── Adversarial fixture: a seeder that ACCEPTS then hangs-then-drops ────
 *
 * Distinct failure mode from "nothing listening" (immediate ECONNREFUSED,
 * already covered by test_parallel_download / test_verified_multi_seeder's
 * dead-peer case): this endpoint completes the TCP handshake — so
 * rf_connect() succeeds — then stalls a bounded moment and closes without
 * ever completing the ROM handshake/reply, so the client observes an EOF
 * mid-protocol rather than a refused connect. Used to prove multi-seeder
 * failover past a seeder that goes unresponsive DURING a transfer, not just
 * one that was never reachable. */
struct rf_hang_seeder {
    int listen_fd;
    pthread_t tid;
    _Atomic bool stop;
    _Atomic int accepts;
};

static void *rf_hang_seeder_thread(void *arg)
{
    struct rf_hang_seeder *h = (struct rf_hang_seeder *)arg;
    while (!atomic_load(&h->stop)) {
        struct pollfd pfd = { .fd = h->listen_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 100);
        if (pr <= 0)
            continue;
        int cfd = accept(h->listen_fd, NULL, NULL);
        if (cfd < 0)
            continue;
        atomic_fetch_add(&h->accepts, 1);
        /* Hang briefly (simulates a wedged/overloaded seeder mid-transfer),
         * then drop the connection without ever replying — the client sees
         * this as an EOF partway through the wire protocol, not a refusal. */
        platform_sleep_ms(150);
        close(cfd);
    }
    return NULL;
}

static bool rf_hang_seeder_start(struct rf_hang_seeder *h, uint16_t port)
{
    memset(h, 0, sizeof(*h));
    h->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (h->listen_fd < 0)
        return false;
    int one = 1;
    setsockopt(h->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(h->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(h->listen_fd, 8) != 0) {
        close(h->listen_fd);
        return false;
    }
    atomic_store(&h->stop, false);
    atomic_store(&h->accepts, 0);
    if (pthread_create(&h->tid, NULL, rf_hang_seeder_thread, h) != 0) {
        close(h->listen_fd);
        return false;
    }
    return true;
}

static void rf_hang_seeder_stop(struct rf_hang_seeder *h)
{
    atomic_store(&h->stop, true);
    pthread_join(h->tid, NULL);
    close(h->listen_fd);
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

/* ── (d2) Stock rate window: the sequential driver self-paces via retry ── */

static int test_rate_cap_retry(void)
{
    int failures = 0;
    TEST("rom_fetch: sequential driver retries through the stock per-peer "
         "rate window instead of dying at the second chunk") {
        rom_seed_reset();
        /* Pin the per-peer window to exactly ONE 4 MB chunk per wall-second
         * (the charge-then-compare window in rom_seed_rate_charge): chunk 0
         * fills it, chunk 1 is refused until the wall-second ticks over. The
         * pre-retry driver failed the whole download here; the bounded
         * backoff (ROM_FETCH_CHUNK_RETRY_MS > 1 s) must carry it through.
         * The global window stays out of the way. */
        rom_seed_set_peer_bps_cap(ROM_SEED_CHUNK_SIZE);
        rom_seed_set_global_bps_cap(1ull << 30);

        char sroot[] = "/tmp/zcl_romfetch_rsrv_XXXXXX";
        char *sdir = mkdtemp(sroot);
        ASSERT(sdir != NULL);
        char croot[] = "/tmp/zcl_romfetch_rcli_XXXXXX";
        char *cdir = mkdtemp(croot);
        ASSERT(cdir != NULL);

        /* 2 chunks: one full 4 MB chunk + a short 4 KB tail. */
        size_t size = (size_t)ROM_SEED_CHUNK_SIZE + 4096;
        uint8_t *content = malloc(size);
        ASSERT(content != NULL);
        gen_content(content, size);
        ASSERT(write_file(sdir, "consensus-state-bundle-cap.sqlite",
                          content, size));

        struct rom_artifact art;
        ASSERT(rom_seed_register(sdir, "consensus-state-bundle-cap.sqlite",
                                 NULL, &art) == ROM_REG_OK);
        struct rom_fetch_manifest m;
        manifest_from_artifact(&art, &m);

        static const uint16_t cand_ports[] = { 18139, 18143, 18149 };
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

        /* THE assertion: with the window at one chunk/second this download
         * takes one refused chunk + one backoff (~1.1 s) and still completes
         * — verified, renamed, byte-exact. */
        ASSERT(rom_fetch_download("127.0.0.1", port, &m, cdir, NULL, NULL));
        char final_path[1024];
        snprintf(final_path, sizeof(final_path), "%s/%s", cdir, m.filename);
        ASSERT(rom_fetch_verify_file(final_path, &m));

        /* The delivered artifact is finalized READ-ONLY: the unified
         * installer's immutable admission (immutable_regular_file_open)
         * accepts it exactly as delivered — no manual chmod in the
         * fetch→install handoff. */
        struct stat st;
        ASSERT(stat(final_path, &st) == 0);
        ASSERT((st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0);

        fs_server_stop();

        free(content);
        char p[1024];
        snprintf(p, sizeof(p), "%s/consensus-state-bundle-cap.sqlite", sdir);
        unlink(p);
        unlink(final_path);
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

/* ── (f) Multi-seeder per-chunk-verified download: failover, content proof,
 *        trustable resume across seeders ────────────────────────────────── */

static int test_verified_multi_seeder(void)
{
    int failures = 0;
    TEST("rom_fetch: multi-seeder per-chunk-verified download fails over past "
         "a dead seeder, fails closed on bad content, and resumes only the "
         "missing chunk") {
        fs_server_stop(); /* never inherit a leaked server */
        rom_seed_reset();
        rom_peer_scoring_test_reset();
        rom_seed_set_peer_bps_cap(1ull << 30);
        rom_seed_set_global_bps_cap(1ull << 30);

        char sroot[] = "/tmp/zcl_romfetch_vmsrv_XXXXXX";
        char *sdir = mkdtemp(sroot);
        ASSERT(sdir != NULL);
        char croot[] = "/tmp/zcl_romfetch_vmcli_XXXXXX";
        char *cdir = mkdtemp(croot);
        ASSERT(cdir != NULL);

        /* 3 chunks: two full 4 MB chunks + a short 4 KB tail. */
        size_t size = 2 * (size_t)ROM_SEED_CHUNK_SIZE + 4096;
        uint8_t *content = malloc(size);
        ASSERT(content != NULL);
        gen_content(content, size);
        ASSERT(write_file(sdir, "consensus-state-bundle-vm.sqlite",
                          content, size));

        struct rom_artifact art;
        ASSERT(rom_seed_register(sdir, "consensus-state-bundle-vm.sqlite",
                                 NULL, &art) == ROM_REG_OK);
        ASSERT(art.num_chunks == 3);
        struct rom_fetch_manifest m;
        manifest_from_artifact(&art, &m);

        static const uint16_t cand_ports[] = { 18301, 18305, 18309 };
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

        /* Real per-chunk manifest over the RMF wire path. */
        uint8_t (*chunk_sha3)[32] = malloc((size_t)ROM_SEED_MAX_CHUNKS * 32);
        ASSERT(chunk_sha3 != NULL);
        uint32_t manifest_chunks = 0;
        ASSERT(rom_fetch_get_manifest("127.0.0.1", port, m.chunk_root,
                                      chunk_sha3, ROM_SEED_MAX_CHUNKS,
                                      &manifest_chunks));
        ASSERT(manifest_chunks == 3);

        char part_path[1200];
        snprintf(part_path, sizeof(part_path), "%s/%s%s", cdir, m.filename,
                 ROM_FETCH_PART_SUFFIX);
        char jrnl_path[1264];
        snprintf(jrnl_path, sizeof(jrnl_path), "%s.journal", part_path);
        char final_path[1200];
        snprintf(final_path, sizeof(final_path), "%s/%s", cdir, m.filename);
        struct stat st;

        /* (1) FAILOVER: the first seeder is DEAD (nothing on port 1); every
         * chunk must fail over to the live one and the whole-file proof pass. */
        struct rom_fetch_peer peers[2];
        memset(peers, 0, sizeof(peers));
        snprintf(peers[0].addr, sizeof(peers[0].addr), "%s", "127.0.0.1");
        peers[0].port = 1; /* tcpmux — nothing listening (dead seeder) */
        snprintf(peers[1].addr, sizeof(peers[1].addr), "%s", "127.0.0.1");
        peers[1].port = port; /* live seeder */
        ASSERT(rom_fetch_download_verified_parallel(peers, 2, &m, chunk_sha3,
                                                    manifest_chunks, cdir,
                                                    NULL, NULL));
        ASSERT(rom_fetch_verify_file(final_path, &m));
        ASSERT(stat(jrnl_path, &st) != 0); /* journal cleaned on install */
        unlink(final_path);

        /* (2) CONTENT FAIL-CLOSED: two LIVE seeder entries, but a committed
         * digest for chunk 2 that no peer can satisfy — the download fails
         * closed (never installs), leaving a resumable .part + journal. Both
         * peers point at the live server so the poisoned-content path (not a
         * dead-peer miss) drives the terminal failure. */
        struct rom_fetch_peer live2[2];
        memset(live2, 0, sizeof(live2));
        for (int i = 0; i < 2; i++) {
            snprintf(live2[i].addr, sizeof(live2[i].addr), "%s", "127.0.0.1");
            live2[i].port = port;
        }
        uint8_t (*bad_sha3)[32] = malloc((size_t)ROM_SEED_MAX_CHUNKS * 32);
        ASSERT(bad_sha3 != NULL);
        memcpy(bad_sha3, chunk_sha3, (size_t)manifest_chunks * 32);
        bad_sha3[2][0] ^= 0xFF;
        ASSERT(!rom_fetch_download_verified_parallel(live2, 2, &m, bad_sha3,
                                                     manifest_chunks, cdir,
                                                     NULL, NULL));
        ASSERT(access(final_path, F_OK) != 0); /* never installed */
        ASSERT(stat(part_path, &st) == 0);     /* .part left for resume */
        ASSERT(stat(jrnl_path, &st) == 0);     /* journal left for resume */
        /* The endpoint that served the (committed-)bad chunk is scored down. */
        ASSERT(rom_peer_is_deprioritized("127.0.0.1", port));
        free(bad_sha3);
        (void)unlink(part_path);
        (void)unlink(jrnl_path);

        /* (3) TRUSTABLE RESUME across seeders (deterministic): hand-build the
         * exact durable .part + journal a run would have left after chunks 0
         * and 1 (N-1 of N), then resume through the multi-seeder path with a
         * DEAD first peer — exactly ONE chunk must cross the wire. */
        int fd = open(part_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        ASSERT(fd >= 0);
        struct rom_journal *j = rom_journal_open(jrnl_path, m.chunk_root,
                                                 m.whole_sha3, m.chunk_size,
                                                 m.num_chunks);
        ASSERT(j != NULL);
        uint8_t *cbuf = malloc(ROM_SEED_CHUNK_SIZE);
        ASSERT(cbuf != NULL);
        for (uint32_t ci = 0; ci < 2; ci++) {
            uint32_t got = 0;
            ASSERT(rom_fetch_chunk("127.0.0.1", port, m.chunk_root, ci,
                                   cbuf, ROM_SEED_CHUNK_SIZE, &got));
            ASSERT(rom_fetch_verify_chunk(cbuf, got, chunk_sha3[ci]));
            ASSERT(pwrite(fd, cbuf, got,
                          (off_t)((uint64_t)ci * m.chunk_size)) == (ssize_t)got);
            ASSERT(fdatasync(fd) == 0);
            ASSERT(rom_journal_mark(j, ci));
        }
        free(cbuf);
        close(fd);
        ASSERT(rom_journal_count_done(j) == 2);
        rom_journal_close(j);

        int64_t served_before = seed_chunks_served();
        ASSERT(rom_fetch_download_verified_parallel(peers, 2, &m, chunk_sha3,
                                                    manifest_chunks, cdir,
                                                    NULL, NULL));
        int64_t served_after = seed_chunks_served();
        ASSERT(served_after - served_before == 1); /* only chunk 2 refetched */
        ASSERT(rom_fetch_verify_file(final_path, &m));
        ASSERT(stat(jrnl_path, &st) != 0); /* journal cleaned on install */

        free(chunk_sha3);
        fs_server_stop();
        free(content);
        char p[1024];
        snprintf(p, sizeof(p), "%s/consensus-state-bundle-vm.sqlite", sdir);
        unlink(p);
        unlink(final_path);
        rmdir(sdir);
        rmdir(cdir);
        rom_seed_reset();
        rom_peer_scoring_test_reset();
        PASS();
    } _test_next:;
    return failures;
}

/* ── (h) Multi-seeder failover past a HUNG (not just dead) seeder ──────
 *
 * test_verified_multi_seeder above already proves failover past a peer
 * nothing is listening on (immediate ECONNREFUSED). This proves the
 * distinct failure mode where the seeder DOES accept the TCP connection —
 * so it looked reachable — then goes unresponsive mid-protocol and drops
 * the connection without ever completing the ROM handshake/reply. */
static int test_verified_multi_seeder_hang_failover(void)
{
    int failures = 0;
    TEST("rom_fetch: multi-seeder download fails over past a seeder that "
         "accepts the connection then hangs-then-drops mid-transfer, "
         "completing from the live seeder") {
        fs_server_stop();
        rom_seed_reset();
        rom_peer_scoring_test_reset();
        rom_seed_set_peer_bps_cap(1ull << 30);
        rom_seed_set_global_bps_cap(1ull << 30);

        char sroot[] = "/tmp/zcl_romfetch_hgsrv_XXXXXX";
        char *sdir = mkdtemp(sroot);
        ASSERT(sdir != NULL);
        char croot[] = "/tmp/zcl_romfetch_hgcli_XXXXXX";
        char *cdir = mkdtemp(croot);
        ASSERT(cdir != NULL);

        /* 3 chunks: two full 4 MB chunks + a short 4 KB tail. */
        size_t size = 2 * (size_t)ROM_SEED_CHUNK_SIZE + 4096;
        uint8_t *content = malloc(size);
        ASSERT(content != NULL);
        gen_content(content, size);
        ASSERT(write_file(sdir, "consensus-state-bundle-hg.sqlite",
                          content, size));

        struct rom_artifact art;
        ASSERT(rom_seed_register(sdir, "consensus-state-bundle-hg.sqlite",
                                 NULL, &art) == ROM_REG_OK);
        ASSERT(art.num_chunks == 3);
        struct rom_fetch_manifest m;
        manifest_from_artifact(&art, &m);

        static const uint16_t cand_ports[] = { 18501, 18505, 18509 };
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

        uint8_t (*chunk_sha3)[32] = malloc((size_t)ROM_SEED_MAX_CHUNKS * 32);
        ASSERT(chunk_sha3 != NULL);
        uint32_t manifest_chunks = 0;
        ASSERT(rom_fetch_get_manifest("127.0.0.1", port, m.chunk_root,
                                      chunk_sha3, ROM_SEED_MAX_CHUNKS,
                                      &manifest_chunks));
        ASSERT(manifest_chunks == 3);

        /* peers[0] accepts then hangs-then-drops every connection; peers[1]
         * is the real live seeder. Chunk i starts at peer (i % 2), so
         * chunks 0 and 2 hit the hung endpoint first and must fail over. */
        struct rf_hang_seeder hang;
        const uint16_t hang_port = 18513;
        ASSERT(rf_hang_seeder_start(&hang, hang_port));

        struct rom_fetch_peer peers[2];
        memset(peers, 0, sizeof(peers));
        snprintf(peers[0].addr, sizeof(peers[0].addr), "%s", "127.0.0.1");
        peers[0].port = hang_port;
        snprintf(peers[1].addr, sizeof(peers[1].addr), "%s", "127.0.0.1");
        peers[1].port = port;

        ASSERT(rom_fetch_download_verified_parallel(peers, 2, &m, chunk_sha3,
                                                    manifest_chunks, cdir,
                                                    NULL, NULL));
        char final_path[1200];
        snprintf(final_path, sizeof(final_path), "%s/%s", cdir, m.filename);
        ASSERT(rom_fetch_verify_file(final_path, &m));
        /* The hung endpoint genuinely saw traffic (it was tried, not just
         * skipped) — proving this is a failOVER, not a lucky avoidance. */
        ASSERT(atomic_load(&hang.accepts) >= 1);

        rf_hang_seeder_stop(&hang);
        unlink(final_path);
        free(chunk_sha3);
        fs_server_stop();
        free(content);
        char p[1024];
        snprintf(p, sizeof(p), "%s/consensus-state-bundle-hg.sqlite", sdir);
        unlink(p);
        rmdir(sdir);
        rmdir(cdir);
        rom_seed_reset();
        rom_peer_scoring_test_reset();
        PASS();
    } _test_next:;
    return failures;
}

/* ── (i) Native command layer: typed blocker + typed refusal ───────────
 *
 * rom_fetch.c/test_rom_fetch.c above prove the ENGINE fails closed; these
 * prove the OPERATOR-FACING surface (app/controllers/src/rom_fetch_controller.c)
 * turns every engine failure into a structured `zcl_command_reply` — a typed
 * code + message the operator (or Claude) can act on — never a silent
 * return and never an unbounded hang, per CLAUDE.md "every native command
 * handler must set an error body". */

static int test_bundle_handler_no_seeder_blocker(void)
{
    int failures = 0;
    TEST("ops.debug.rom_fetch.bundle: no reachable seeder names a typed "
         "blocker (FAILED status, non-empty error code+message), never "
         "silence and never an unbounded hang") {
        char croot[] = "/tmp/zcl_romfetch_nsbcli_XXXXXX";
        char *cdir = mkdtemp(croot);
        ASSERT(cdir != NULL);

        uint8_t root[32], whole[32];
        memset(root, 0x11, sizeof(root));
        memset(whole, 0x22, sizeof(whole));
        char root_hex[65], whole_hex[65], hex8[17];
        HexStr(root, 32, false, root_hex, sizeof(root_hex));
        HexStr(whole, 32, false, whole_hex, sizeof(whole_hex));
        HexStr(root, 8, false, hex8, sizeof(hex8));

        struct json_value input;
        json_init(&input);
        json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "peer", "127.0.0.1"));
        ASSERT(json_push_kv_str(&input, "port", "1")); /* tcpmux: nothing listening */
        ASSERT(json_push_kv_str(&input, "root", root_hex));
        ASSERT(json_push_kv_str(&input, "whole_sha3", whole_hex));
        ASSERT(json_push_kv_str(&input, "size", "4096")); /* 1 chunk, min bound */
        ASSERT(json_push_kv_str(&input, "out_dir", cdir));
        struct zcl_command_request request = { .input = &input };
        struct zcl_command_reply reply;
        zcl_command_reply_init(&reply, "zcl.rom_fetch_bundle.v1");

        zcl_native_handle_rom_fetch_bundle(&request, &reply);

        ASSERT(reply.status == ZCL_COMMAND_STATUS_FAILED);
        ASSERT(reply.exit_code != ZCL_COMMAND_EXIT_OK);
        ASSERT(reply.error.code[0] != '\0');
        ASSERT(reply.error.message[0] != '\0');
        ASSERT(strcmp(reply.error.code, "ROM_FETCH_FAILED") == 0);

        char final_path[1200];
        snprintf(final_path, sizeof(final_path), "%s/rom-artifact-%s",
                 cdir, hex8);
        ASSERT(access(final_path, F_OK) != 0); /* nothing staged */

        zcl_command_reply_free(&reply);
        json_free(&input);
        rmdir(cdir);
        PASS();
    } _test_next:;
    return failures;
}

static int test_bundle_handler_corrupted_refused(void)
{
    int failures = 0;
    TEST("ops.debug.rom_fetch.bundle: a wrong committed whole-file digest "
         "(the file that looks right by chunk_root/size/filename but whose "
         "content commitment is wrong) is refused with a typed error, and "
         "not even a resumable partial is left — no partial trust on a "
         "full-commitment mismatch") {
        fs_server_stop();
        rom_seed_reset();
        rom_peer_scoring_test_reset();
        rom_seed_set_peer_bps_cap(1ull << 30);
        rom_seed_set_global_bps_cap(1ull << 30);

        char sroot[] = "/tmp/zcl_romfetch_corsrv_XXXXXX";
        char *sdir = mkdtemp(sroot);
        ASSERT(sdir != NULL);
        char croot[] = "/tmp/zcl_romfetch_corcli_XXXXXX";
        char *cdir = mkdtemp(croot);
        ASSERT(cdir != NULL);

        size_t size = 8192; /* 1 chunk */
        uint8_t *content = malloc(size);
        ASSERT(content != NULL);
        gen_content(content, size);
        ASSERT(write_file(sdir, "consensus-state-bundle-cor.sqlite",
                          content, size));

        struct rom_artifact art;
        ASSERT(rom_seed_register(sdir, "consensus-state-bundle-cor.sqlite",
                                 NULL, &art) == ROM_REG_OK);
        ASSERT(art.num_chunks == 1);

        static const uint16_t cand_ports[] = { 18521, 18525, 18529 };
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

        char root_hex[65], whole_hex[65];
        HexStr(art.chunk_root, 32, false, root_hex, sizeof(root_hex));
        HexStr(art.whole_sha3, 32, false, whole_hex, sizeof(whole_hex));
        /* Corrupt the committed whole-file digest only — chunk_root, size,
         * and filename all still "look right". Every individual chunk this
         * peer actually serves is real, committed content (chunk_root is
         * correct), so per-chunk verification passes; only the final
         * whole-file gate catches the mismatch. */
        whole_hex[0] = (whole_hex[0] == '0') ? '1' : '0';

        char port_s[8];
        snprintf(port_s, sizeof(port_s), "%u", (unsigned)port);
        char size_s[32];
        snprintf(size_s, sizeof(size_s), "%llu",
                 (unsigned long long)art.size_bytes);

        struct json_value input;
        json_init(&input);
        json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "peer", "127.0.0.1"));
        ASSERT(json_push_kv_str(&input, "port", port_s));
        ASSERT(json_push_kv_str(&input, "root", root_hex));
        ASSERT(json_push_kv_str(&input, "whole_sha3", whole_hex));
        ASSERT(json_push_kv_str(&input, "size", size_s));
        ASSERT(json_push_kv_str(&input, "filename", art.filename));
        ASSERT(json_push_kv_str(&input, "out_dir", cdir));
        struct zcl_command_request request = { .input = &input };
        struct zcl_command_reply reply;
        zcl_command_reply_init(&reply, "zcl.rom_fetch_bundle.v1");

        zcl_native_handle_rom_fetch_bundle(&request, &reply);

        ASSERT(reply.status == ZCL_COMMAND_STATUS_FAILED);
        ASSERT(strcmp(reply.error.code, "ROM_FETCH_FAILED") == 0);
        ASSERT(reply.error.message[0] != '\0');

        char final_path[1200];
        snprintf(final_path, sizeof(final_path), "%s/%s", cdir, art.filename);
        ASSERT(access(final_path, F_OK) != 0); /* nothing staged */
        char part_path[1200];
        snprintf(part_path, sizeof(part_path), "%s/%s%s", cdir, art.filename,
                 ROM_FETCH_PART_SUFFIX);
        ASSERT(access(part_path, F_OK) != 0); /* unlinked: no partial trust
                                                * on a whole-commitment
                                                * mismatch */

        zcl_command_reply_free(&reply);
        json_free(&input);
        fs_server_stop();
        free(content);
        char p[1024];
        snprintf(p, sizeof(p), "%s/consensus-state-bundle-cor.sqlite", sdir);
        unlink(p);
        char jrnl_path[1264];
        snprintf(jrnl_path, sizeof(jrnl_path), "%s.journal", part_path);
        unlink(jrnl_path); /* orphaned journal sidecar, harmless cleanup */
        rmdir(sdir);
        rmdir(cdir);
        rom_seed_reset();
        rom_peer_scoring_test_reset();
        PASS();
    } _test_next:;
    return failures;
}

/* RLS directory-listing round-trip: the real serve path answers a "RLS" request
 * with its {"artifacts":[...]} catalog, and the client rom_fetch_get_directory
 * receives + MAC-verifies it, parsing back to the exact registered artifact. */
static int test_directory_discovery(void)
{
    int failures = 0;
    TEST("rom_fetch: RLS directory listing round-trips over the real fs serve") {
        rom_seed_reset();
        rom_seed_set_peer_bps_cap(1ull << 30);
        rom_seed_set_global_bps_cap(1ull << 30);

        char sroot[] = "/tmp/zcl_romfetch_rls_XXXXXX";
        char *sdir = mkdtemp(sroot);
        ASSERT(sdir != NULL);

        size_t size = (size_t)ROM_SEED_CHUNK_SIZE + 4096;
        uint8_t *content = malloc(size);
        ASSERT(content != NULL);
        gen_content(content, size);
        ASSERT(write_file(sdir, "consensus-state-bundle-rls.sqlite",
                          content, size));
        struct rom_artifact art;
        ASSERT(rom_seed_register(sdir, "consensus-state-bundle-rls.sqlite",
                                 NULL, &art) == ROM_REG_OK);

        static const uint16_t cand_ports[] = { 18201, 18205, 18209 };
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

        /* (1) Fetch + MAC-verify the listing, then parse it back to the exact
         * registered artifact triple. */
        char dir_body[4096];
        ASSERT(rom_fetch_get_directory("127.0.0.1", port, dir_body,
                                       sizeof(dir_body)));
        struct rom_fetch_manifest arts[ROM_FETCH_MAX_ARTIFACTS];
        memset(arts, 0, sizeof(arts));
        int n = rom_fetch_parse_directory(dir_body, arts,
                                          ROM_FETCH_MAX_ARTIFACTS);
        ASSERT(n == 1);
        ASSERT(memcmp(arts[0].chunk_root, art.chunk_root, 32) == 0);
        ASSERT(memcmp(arts[0].whole_sha3, art.whole_sha3, 32) == 0);
        ASSERT(arts[0].size_bytes == art.size_bytes);
        ASSERT(arts[0].num_chunks == art.num_chunks);

        /* (2) A cap smaller than the body rejects the over-cap reply (the
         * bounded-read guard), rather than truncating. */
        char tiny[16];
        ASSERT(!rom_fetch_get_directory("127.0.0.1", port, tiny, sizeof(tiny)));

        fs_server_stop();

        /* (3) A dead peer (nothing listening) is a clean skip, not a crash. */
        char dead_body[4096];
        ASSERT(!rom_fetch_get_directory("127.0.0.1", port, dead_body,
                                        sizeof(dead_body)));

        free(content);
        char p[1024];
        snprintf(p, sizeof(p), "%s/consensus-state-bundle-rls.sqlite", sdir);
        unlink(p);
        rmdir(sdir);
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
    failures += test_directory_discovery();
    failures += test_rate_cap_retry();
    failures += test_parallel_download();
    failures += test_verified_multi_seeder();
    failures += test_verified_multi_seeder_hang_failover();
    failures += test_bundle_handler_no_seeder_blocker();
    failures += test_bundle_handler_corrupted_refused();
    return failures;
}
