/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_boot_bundle_fetch — THE WELD (config/src/boot_bundle_fetch.c).
 *
 * Proves the instant-on download-before-autodetect weld end to end against the
 * REAL file-service serve path on a loopback seeder:
 *
 *   (a) the gate (boot_bundle_fetch_should_run): fresh datadir → run; opted out
 *       (-nofilesync / ZCL_NO_BUNDLE_FETCH) → skip; sovereign marker present →
 *       skip; a *.sqlite already staged → skip.
 *   (b) manifest pick (boot_bundle_pick_manifest): a /directory.json body parses
 *       to the largest sane artifact and is assigned a classifiable
 *       consensus-state-bundle-*.sqlite name; empty/garbage → refused.
 *   (c) E2E: fresh datadir + no bundle + a reachable seeder ⇒ the content-
 *       verified bundle lands under <datadir>/bundles/, the autodetect then
 *       FINDS it (install fires), and the install of this synthetic (non-
 *       checkpoint) bundle FAILS CLOSED with NO sovereign marker written —
 *       sovereignty preserved. A byte-mismatched committed digest ⇒ the fetch
 *       is REFUSED, nothing lands, and the autodetect finds nothing (no install).
 *   (d) the production entry (boot_bundle_fetch_maybe): a directory.json hint +
 *       an -fileservice peer drives the same land-in-bundles/ result.
 *
 * Fixtures live under mkdtemp() dirs in /tmp — never a real datadir. The
 * synthetic bundle is a valid SQLite-magic blob that the serve path streams and
 * the fetch content-verifies, but it is NOT a real consensus-state bundle, so
 * the install path fail-closes at admission (exactly the sovereignty guard). */

#include "test/test_helpers.h"

#include "config/boot.h"                       /* struct app_context */
#include "config/boot_bundle_fetch.h"
#include "config/boot_consensus_bundle_marker.h"
#include "config/consensus_state_install_runtime.h"
#include "chain/checkpoints.h"
#include "net/rom_fetch.h"
#include "net/rom_seed.h"
#include "net/file_service.h"
#include "encoding/utilstrencodings.h"
#include "platform/time_compat.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* SQLite-magic-prefixed deterministic content the serve path accepts + the
 * fetch content-verifies (mirrors test_rom_fetch). */
static void bbf_gen_content(uint8_t *buf, size_t size)
{
    static const uint8_t magic[16] = "SQLite format 3";
    for (size_t i = 0; i < size; i++)
        buf[i] = (uint8_t)((i * 131u + 7u) & 0xffu);
    if (size >= 16)
        memcpy(buf, magic, 16);
}

static bool bbf_write_file(const char *dir, const char *name,
                           const uint8_t *buf, size_t size)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    bool ok = (write(fd, buf, size) == (ssize_t)size);
    close(fd);
    return ok;
}

/* Wrap the serve-side artifacts array (rom_seed_directory_json) into the full
 * /directory.json object shape the parser expects ({"artifacts":[...]}). */
static void bbf_directory_json(char *out, size_t cap)
{
    char arts[2560];
    size_t an = rom_seed_directory_json(arts, sizeof(arts));
    const char *body = (an > 0) ? arts : "[]";
    snprintf(out, cap, "{\"count\":0,\"artifacts\":%s}", body);
}

/* ── (a) Gate ───────────────────────────────────────────────────────────── */

static int case_gate(void)
{
    int failures = 0;
    TEST("boot_bundle_fetch: gate runs on a fresh datadir, skips otherwise") {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "bbf_gate", "ok");

        /* Fresh, marker-less, no bundle → run. */
        ASSERT(boot_bundle_fetch_should_run(dir, NULL));

        /* Opt-out via env. */
        setenv("ZCL_NO_BUNDLE_FETCH", "1", 1);
        ASSERT(!boot_bundle_fetch_should_run(dir, NULL));
        unsetenv("ZCL_NO_BUNDLE_FETCH");
        ASSERT(boot_bundle_fetch_should_run(dir, NULL));

        /* Opt-out via -nofilesync (ctx->no_file_sync). */
        struct app_context ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.no_file_sync = true;
        ASSERT(!boot_bundle_fetch_should_run(dir, &ctx));
        ctx.no_file_sync = false;
        ASSERT(boot_bundle_fetch_should_run(dir, &ctx));

        /* Empty datadir → never runs. */
        ASSERT(!boot_bundle_fetch_should_run("", NULL));

        /* Sovereign marker present → never re-fetch. */
        uint8_t digest[32];
        memset(digest, 0xab, sizeof(digest));
        ASSERT(boot_consensus_bundle_marker_write(dir, 3056758, digest));
        ASSERT(!boot_bundle_fetch_should_run(dir, NULL));

        test_rm_rf_recursive(dir);

        /* A *.sqlite already staged under bundles/ → autodetect installs it, so
         * the weld skips the download. */
        char dir2[256];
        test_make_tmpdir(dir2, sizeof(dir2), "bbf_gate_staged", "ok");
        char bundles[320];
        snprintf(bundles, sizeof(bundles), "%s/bundles", dir2);
        ASSERT(mkdir(bundles, 0700) == 0);
        char staged[400];
        snprintf(staged, sizeof(staged), "%s/consensus-state-bundle-1.sqlite",
                 bundles);
        FILE *sf = fopen(staged, "w");
        ASSERT(sf != NULL);
        fclose(sf);
        ASSERT(!boot_bundle_fetch_should_run(dir2, NULL));
        test_rm_rf_recursive(dir2);
    } _test_next:;
    return failures;
}

/* ── (b) Manifest pick from a /directory.json body ──────────────────────── */

static int case_pick(void)
{
    int failures = 0;
    TEST("boot_bundle_fetch: pick_manifest chooses + names the bundle artifact") {
        /* A single sane artifact: 2 chunks (one full + a 4 KB tail). */
        uint64_t size = (uint64_t)ROM_SEED_CHUNK_SIZE + 4096;
        char body[512];
        snprintf(body, sizeof(body),
                 "{\"artifacts\":[{\"kind\":\"consensus_state_bundle\","
                 "\"digest\":\"%064d\",\"whole_sha3\":\"%064d\","
                 "\"size\":%llu,\"chunk_size\":%u,\"chunks\":2}]}",
                 1, 2, (unsigned long long)size, (unsigned)ROM_SEED_CHUNK_SIZE);

        struct rom_fetch_manifest m;
        memset(&m, 0, sizeof(m));
        ASSERT(boot_bundle_pick_manifest(body, &m));
        ASSERT(m.size_bytes == size);
        ASSERT(m.num_chunks == 2);
        ASSERT(m.chunk_size == ROM_SEED_CHUNK_SIZE);
        /* Assigned a classifiable *.sqlite name (directory.json carries none). */
        ASSERT(strncmp(m.filename, "consensus-state-bundle-", 23) == 0);
        size_t fl = strlen(m.filename);
        ASSERT(fl > 7 && strcmp(m.filename + fl - 7, ".sqlite") == 0);
        ASSERT(rom_fetch_manifest_sane(&m));

        /* Negative: no artifacts, garbage, NULL → refused. */
        struct rom_fetch_manifest z;
        ASSERT(!boot_bundle_pick_manifest("{\"artifacts\":[]}", &z));
        ASSERT(!boot_bundle_pick_manifest("{not json", &z));
        ASSERT(!boot_bundle_pick_manifest(NULL, &z));
    } _test_next:;
    return failures;
}

/* ── (c)+(d) E2E against the real serve path ────────────────────────────── */

static int case_e2e(void)
{
    int failures = 0;
    TEST("boot_bundle_fetch: E2E download lands + install fires + guards hold") {
        rom_seed_reset();
        /* Move ~3x the artifact inside one wall-second — raise the byte caps so
         * the outcome is not wall-clock dependent (reset restores defaults). */
        rom_seed_set_peer_bps_cap(1ull << 30);
        rom_seed_set_global_bps_cap(1ull << 30);

        char sroot[] = "/tmp/zcl_bbf_srv_XXXXXX";
        char *sdir = mkdtemp(sroot);
        ASSERT(sdir != NULL);

        /* 2 chunks: one full 4 MB chunk + a short 4 KB tail. */
        size_t size = (size_t)ROM_SEED_CHUNK_SIZE + 4096;
        uint8_t *content = malloc(size);
        ASSERT(content != NULL);
        bbf_gen_content(content, size);
        ASSERT(bbf_write_file(sdir, "consensus-state-bundle-3056758.sqlite",
                              content, size));

        struct rom_artifact art;
        ASSERT(rom_seed_register(sdir, "consensus-state-bundle-3056758.sqlite",
                                 NULL, &art) == ROM_REG_OK);

        static const uint16_t cand_ports[] = { 18159, 18163, 18169 };
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

        /* pick_manifest consumes the seeder's REAL directory.json. */
        char dirjson[3072];
        bbf_directory_json(dirjson, sizeof(dirjson));
        struct rom_fetch_manifest m;
        memset(&m, 0, sizeof(m));
        ASSERT(boot_bundle_pick_manifest(dirjson, &m));
        ASSERT(memcmp(m.chunk_root, art.chunk_root, 32) == 0);
        ASSERT(m.size_bytes == art.size_bytes);
        ASSERT(m.num_chunks == art.num_chunks);

        /* (c) Download core lands the content-verified bundle in bundles/. */
        char croot[] = "/tmp/zcl_bbf_cli_XXXXXX";
        char *cdir = mkdtemp(croot);
        ASSERT(cdir != NULL);
        struct rom_fetch_peer peers[1];
        memset(peers, 0, sizeof(peers));
        snprintf(peers[0].addr, sizeof(peers[0].addr), "%s", "127.0.0.1");
        peers[0].port = port;

        ASSERT(boot_bundle_fetch_download(cdir, peers, 1, &m));
        char landed[1200];
        snprintf(landed, sizeof(landed), "%s/bundles/%s", cdir, m.filename);
        ASSERT(rom_fetch_verify_file(landed, &m));

        /* Reseed (Lane A2): the just-landed bundle is registered with rom_seed
         * IMMEDIATELY on download success — no restart, no scan needed — so
         * this node is already a swarm source for it. Assert via the rom_seed
         * catalog API (rom_seed_list) that a "bundles/<filename>" entry exists
         * with the SAME chunk_root the source artifact carries. */
        {
            char want[ROM_SEED_NAME_MAX];
            snprintf(want, sizeof(want), "%s/%s", ROM_SEED_BUNDLES_SUBDIR,
                     m.filename);
            struct rom_artifact cat[ROM_SEED_MAX_ARTIFACTS];
            int cn = rom_seed_list(cat, ROM_SEED_MAX_ARTIFACTS);
            bool reseed_found = false;
            for (int i = 0; i < cn; i++) {
                if (strcmp(cat[i].filename, want) != 0)
                    continue;
                reseed_found = true;
                ASSERT(memcmp(cat[i].chunk_root, art.chunk_root, 32) == 0);
                ASSERT(cat[i].size_bytes == m.size_bytes);
                ASSERT(cat[i].num_chunks == m.num_chunks);
                break;
            }
            ASSERT(reseed_found);
        }

        /* Install FIRES: the autodetect now finds the downloaded bundle. */
        char *auto_path = boot_autodetect_consensus_bundle(cdir);
        ASSERT(auto_path != NULL);
        ASSERT(strcmp(auto_path, landed) == 0);

        /* Sovereignty: this synthetic (non-checkpoint) bundle is REFUSED at
         * install — fail-closed at admission, no state, NO marker. */
        struct consensus_state_install_runtime_result rr;
        struct zcl_result r =
            consensus_state_install_from_bundle(NULL, NULL, auto_path, cdir, &rr);
        ASSERT(!r.ok);
        ASSERT(!rr.state_installed);
        ASSERT(!rr.marker_written);
        ASSERT(!boot_consensus_bundle_marker_exists(cdir));
        free(auto_path);

        /* Byte-mismatch: a wrong committed whole-digest downloads every chunk,
         * fails the whole-file content proof, and lands NOTHING → no install. */
        char croot2[] = "/tmp/zcl_bbf_bad_XXXXXX";
        char *cdir2 = mkdtemp(croot2);
        ASSERT(cdir2 != NULL);
        struct rom_fetch_manifest bad = m;
        bad.whole_sha3[0] ^= 0x01;
        ASSERT(!boot_bundle_fetch_download(cdir2, peers, 1, &bad));
        char *bad_auto = boot_autodetect_consensus_bundle(cdir2);
        ASSERT(bad_auto == NULL); /* nothing installable landed */
        free(bad_auto);
        /* Nothing landed on disk for cdir2 at all, so there is nothing rom_seed
         * could have (re-)registered for it either — the reseed call only runs
         * on the success path, guarded by boot_bundle_fetch_download's own
         * `if (!ok) return false;` above the reseed block. */

        /* (d) Production entry: a directory.json hint + -fileservice peer drives
         * the whole gate → pick → seed-assembly → download → land path. */
        char croot3[] = "/tmp/zcl_bbf_maybe_XXXXXX";
        char *cdir3 = mkdtemp(croot3);
        ASSERT(cdir3 != NULL);
        char b3[400];
        snprintf(b3, sizeof(b3), "%s/bundles", cdir3);
        ASSERT(mkdir(b3, 0700) == 0);
        ASSERT(bbf_write_file(b3, "directory.json",
                              (const uint8_t *)dirjson, strlen(dirjson)));

        struct app_context ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.datadir = cdir3;
        /* host:port so the weld's peer parse targets the loopback seeder; and
         * connect_only so the hardcoded (unreachable-in-test) clearnet seeds are
         * skipped — deterministic + fast. */
        char peer_hp[64];
        snprintf(peer_hp, sizeof(peer_hp), "127.0.0.1:%u", (unsigned)port);
        ctx.file_service_peer = peer_hp;
        ctx.connect_only = true;

        ASSERT(boot_bundle_fetch_maybe(cdir3, &ctx));
        char *auto3 = boot_autodetect_consensus_bundle(cdir3);
        ASSERT(auto3 != NULL);
        free(auto3);
        /* The production entry point reseeds too (it drives the same
         * boot_bundle_fetch_download core). */
        {
            char want3[ROM_SEED_NAME_MAX];
            snprintf(want3, sizeof(want3), "%s/%s", ROM_SEED_BUNDLES_SUBDIR,
                     m.filename);
            struct rom_artifact cat3[ROM_SEED_MAX_ARTIFACTS];
            int cn3 = rom_seed_list(cat3, ROM_SEED_MAX_ARTIFACTS);
            bool reseed_found3 = false;
            for (int i = 0; i < cn3; i++) {
                if (strcmp(cat3[i].filename, want3) == 0) {
                    reseed_found3 = true;
                    break;
                }
            }
            ASSERT(reseed_found3);
        }

        fs_server_stop();
        free(content);
        test_rm_rf_recursive(cdir);
        test_rm_rf_recursive(cdir2);
        test_rm_rf_recursive(cdir3);
        rmdir(sdir);
        rom_seed_reset();
    } _test_next:;
    return failures;
}

/* ── (e) Baked-facts cross-check + quorum decision (pure) ────────────────── */

static int case_baked_facts(void)
{
    int failures = 0;
    TEST("boot_bundle_fetch: baked-facts cross-check rejects off-shape manifests") {
        struct rom_fetch_manifest m;
        memset(&m, 0, sizeof(m));
        m.chunk_size = ROM_SEED_CHUNK_SIZE;
        m.size_bytes = (uint64_t)ROM_SEED_CHUNK_SIZE + 4096;
        m.num_chunks = 2; /* ceil((CHUNK+4096)/CHUNK) */
        ASSERT(boot_bundle_manifest_facts_ok_for_test(&m));

        /* num_chunks != ceil(size/chunk). */
        struct rom_fetch_manifest bad = m;
        bad.num_chunks = 3;
        ASSERT(!boot_bundle_manifest_facts_ok_for_test(&bad));

        /* chunk_size != ROM_SEED_CHUNK_SIZE. */
        bad = m;
        bad.chunk_size = 1234;
        ASSERT(!boot_bundle_manifest_facts_ok_for_test(&bad));

        /* size below ROM_SEED_MIN_ARTIFACT_BYTES (num_chunks consistent). */
        bad = m;
        bad.size_bytes = 100;
        bad.num_chunks = 1;
        ASSERT(!boot_bundle_manifest_facts_ok_for_test(&bad));

        ASSERT(!boot_bundle_manifest_facts_ok_for_test(NULL));
    } _test_next:;
    return failures;
}

static int case_quorum(void)
{
    int failures = 0;
    TEST("boot_bundle_fetch: quorum decision needs >=2 agree or an explicit seed") {
        /* No candidates → no quorum. */
        ASSERT(boot_bundle_quorum_pick_for_test(NULL, NULL, 0) == -1);

        /* Lone non-explicit seed → refused (bandwidth-DoS guard). */
        int c1[] = { 1 };
        bool f_false[] = { false };
        ASSERT(boot_bundle_quorum_pick_for_test(c1, f_false, 1) == -1);

        /* Lone explicit -fileservice seed → accepted at quorum=1. */
        bool f_true[] = { true };
        ASSERT(boot_bundle_quorum_pick_for_test(c1, f_true, 1) == 0);

        /* >=2 agree → accepted even when non-explicit. */
        int c2[] = { 2 };
        ASSERT(boot_bundle_quorum_pick_for_test(c2, f_false, 1) == 0);

        /* Two candidates: the >=2-agreed one wins over a lone explicit. */
        int cc[] = { 1, 2 };
        bool ff[] = { true, false };
        ASSERT(boot_bundle_quorum_pick_for_test(cc, ff, 2) == 1);

        /* Tie on count → prefer the explicit-seed candidate. */
        int tie[] = { 1, 1 };
        bool tie_f[] = { false, true };
        ASSERT(boot_bundle_quorum_pick_for_test(tie, tie_f, 2) == 1);
    } _test_next:;
    return failures;
}

/* ── (f) Absent-local-manifest → RLS discovery drives the whole path ─────── */

static int case_discovery(void)
{
    int failures = 0;
    TEST("boot_bundle_fetch: no local hint → RLS discovery lands + persists it") {
        rom_seed_reset();
        rom_seed_set_peer_bps_cap(1ull << 30);
        rom_seed_set_global_bps_cap(1ull << 30);

        char sroot[] = "/tmp/zcl_bbf_disc_srv_XXXXXX";
        char *sdir = mkdtemp(sroot);
        ASSERT(sdir != NULL);

        size_t size = (size_t)ROM_SEED_CHUNK_SIZE + 4096;
        uint8_t *content = malloc(size);
        ASSERT(content != NULL);
        bbf_gen_content(content, size);
        ASSERT(bbf_write_file(sdir, "consensus-state-bundle-3056758.sqlite",
                              content, size));
        struct rom_artifact art;
        ASSERT(rom_seed_register(sdir, "consensus-state-bundle-3056758.sqlite",
                                 NULL, &art) == ROM_REG_OK);

        static const uint16_t cand_ports[] = { 18175, 18181, 18187 };
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

        /* Fresh client datadir with NO bundles/directory.json hint. The explicit
         * -fileservice peer + connect_only (no clearnet seeds) makes the lone
         * reachable seed the operator-named one → quorum=1 accepted. */
        char croot[] = "/tmp/zcl_bbf_disc_cli_XXXXXX";
        char *cdir = mkdtemp(croot);
        ASSERT(cdir != NULL);

        struct app_context ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.datadir = cdir;
        char peer_hp[64];
        snprintf(peer_hp, sizeof(peer_hp), "127.0.0.1:%u", (unsigned)port);
        ctx.file_service_peer = peer_hp;
        ctx.connect_only = true;

        /* No local hint exists yet. */
        char hint[512];
        snprintf(hint, sizeof(hint), "%s/bundles/directory.json", cdir);
        ASSERT(access(hint, F_OK) != 0);

        ASSERT(boot_bundle_fetch_maybe(cdir, &ctx));

        /* Discovery persisted the winning directory.json for resume/reseed. */
        ASSERT(access(hint, F_OK) == 0);
        /* And the content-verified bundle landed → autodetect installs it. */
        char *auto_p = boot_autodetect_consensus_bundle(cdir);
        ASSERT(auto_p != NULL);
        free(auto_p);

        fs_server_stop();
        free(content);
        test_rm_rf_recursive(cdir);
        char p[1024];
        snprintf(p, sizeof(p), "%s/consensus-state-bundle-3056758.sqlite", sdir);
        unlink(p);
        rmdir(sdir);
        rom_seed_reset();
    } _test_next:;
    return failures;
}

int test_boot_bundle_fetch(void)
{
    printf("\n=== boot_bundle_fetch ===\n");
    int failures = 0;
    failures += case_gate();
    failures += case_pick();
    failures += case_e2e();
    failures += case_baked_facts();
    failures += case_quorum();
    failures += case_discovery();
    printf("=== boot_bundle_fetch: %d failure(s) ===\n", failures);
    return failures;
}
