/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * bundle-publish-serve weld — a synced node's on-disk starter artifacts become
 * SERVED. Proves the registration weld the ops.debug.rom_seed.publish command
 * (app/controllers/src/rom_seed_controller.c) and the standing exporter's
 * post-produce reseed (config/src/bundle_exporter.c) both rely on:
 *
 *   1. a datadir carrying a valid consensus-state bundle under bundles/ AND a
 *      header-chain seed (block_index.bin) at the root — exactly the shape a
 *      synced node holds — is one idempotent rom_seed_scan_datadir away from
 *      registering BOTH with the right kinds, so the served directory listing
 *      advertises a usable bundle + header-seed manifest to a fresh peer;
 *
 *   2. a corrupt (non-SQLite) bundle and an unknown-kind file are REFUSED at
 *      registration and never offered.
 *
 * Fixtures live under mkdtemp() — never a real datadir, never the network.
 * Touches no consensus predicate. */

#include "test/test_helpers.h"
#include "net/rom_seed.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Deterministic content: SQLite magic in the first 16 bytes when `sqlite_ok`,
 * else 0xEE garbage; then a fixed byte pattern. */
static void bps_gen(uint8_t *buf, size_t size, bool sqlite_ok)
{
    static const uint8_t magic[16] = "SQLite format 3"; /* 15 chars + NUL */
    for (size_t i = 0; i < size; i++)
        buf[i] = (uint8_t)((i * 131u + 7u) & 0xffu);
    if (size >= 16) {
        if (sqlite_ok) memcpy(buf, magic, 16);
        else memset(buf, 0xEE, 16);
    }
}

static bool bps_write(const char *dir, const char *name,
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

static bool bps_have_kind(enum rom_artifact_kind k)
{
    struct rom_artifact arts[ROM_SEED_MAX_ARTIFACTS];
    int n = rom_seed_list(arts, ROM_SEED_MAX_ARTIFACTS);
    for (int i = 0; i < n; i++)
        if (arts[i].kind == k) return true;
    return false;
}

/* (a) A synced datadir (bundle under bundles/, header seed at root) serves
 * BOTH artifacts, with the directory-listing kinds discovery requires. */
static int test_bps_serves_both(void)
{
    int failures = 0;
    TEST("bundle-publish-serve: synced datadir serves bundle + header seed") {
        rom_seed_reset();
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "bundle_publish", "ok");

        char bundles[320];
        snprintf(bundles, sizeof(bundles), "%s/bundles", dir);
        ASSERT(mkdir(bundles, 0700) == 0);

        /* Header-chain seed at the datadir root (a synced node writes this via
         * save_block_index_flat). No magic; the size band is its only guard. */
        size_t hs_size = 8192;
        uint8_t *hs = malloc(hs_size);
        ASSERT(hs != NULL);
        bps_gen(hs, hs_size, false);
        ASSERT(bps_write(dir, "block_index.bin", hs, hs_size));

        /* A valid consensus-state bundle staged under bundles/ (a fetch or the
         * standing exporter lands it there). SQLite-shaped so the content
         * check passes. */
        size_t b_size = 65536;
        uint8_t *b = malloc(b_size);
        ASSERT(b != NULL);
        bps_gen(b, b_size, true);
        ASSERT(bps_write(dir, "bundles/consensus-state-bundle-100.sqlite",
                         b, b_size));

        /* The single idempotent scan both the publish command and boot run. */
        int reg = rom_seed_scan_datadir(dir);
        ASSERT(reg == 2);
        ASSERT(rom_seed_count() == 2);
        ASSERT(bps_have_kind(ROM_ARTIFACT_CONSENSUS_BUNDLE));
        ASSERT(bps_have_kind(ROM_ARTIFACT_HEADER_SEED));

        /* Re-scan is idempotent: the same two artifacts stay registered. */
        int reg2 = rom_seed_scan_datadir(dir);
        ASSERT(reg2 == 2);
        ASSERT(rom_seed_count() == 2);

        /* The served directory listing carries the exact kind tokens a fresh
         * peer's discovery matches (boot_bundle_pick_manifest /
         * boot_bundle_pick_header_seed_manifest). */
        char json[4096];
        size_t jn = rom_seed_directory_json(json, sizeof(json));
        ASSERT(jn > 0);
        ASSERT(strstr(json, "\"consensus_bundle\"") != NULL);
        ASSERT(strstr(json, "\"header_seed\"") != NULL);

        free(hs);
        free(b);
        rom_seed_reset();
        test_rm_rf_recursive(dir);
        PASS();
    } _test_next:;
    return failures;
}

/* (b) A corrupt bundle and an unknown-kind file are refused loudly and never
 * offered — publishing mints no trust, it only serves content-verified bytes. */
static int test_bps_refuses_corrupt(void)
{
    int failures = 0;
    TEST("bundle-publish-serve: corrupt / wrong-kind artifacts are refused") {
        rom_seed_reset();
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "bundle_publish", "bad");

        char bundles[320];
        snprintf(bundles, sizeof(bundles), "%s/bundles", dir);
        ASSERT(mkdir(bundles, 0700) == 0);

        size_t size = 65536;
        uint8_t *bad = malloc(size);
        ASSERT(bad != NULL);
        bps_gen(bad, size, false); /* 0xEE header, NOT SQLite magic */

        /* A bundle-named file that is not SQLite → content check fails. */
        ASSERT(bps_write(dir, "bundles/consensus-state-bundle-200.sqlite",
                         bad, size));
        /* An unknown-kind file the classifier ignores entirely. */
        ASSERT(bps_write(dir, "random-file.txt", bad, size));

        /* Direct registration names each refusal precisely. */
        ASSERT(rom_seed_register(dir,
                                 "bundles/consensus-state-bundle-200.sqlite",
                                 NULL, NULL) == ROM_REG_ERR_CORRUPT);
        ASSERT(rom_seed_register(dir, "random-file.txt", NULL, NULL)
               == ROM_REG_ERR_UNKNOWN_KIND);

        /* And the scan registers NEITHER — nothing corrupt/unknown is served. */
        int reg = rom_seed_scan_datadir(dir);
        ASSERT(reg == 0);
        ASSERT(rom_seed_count() == 0);
        ASSERT(!bps_have_kind(ROM_ARTIFACT_CONSENSUS_BUNDLE));

        free(bad);
        rom_seed_reset();
        test_rm_rf_recursive(dir);
        PASS();
    } _test_next:;
    return failures;
}

int test_bundle_publish_serve(void)
{
    int failures = 0;
    failures += test_bps_serves_both();
    failures += test_bps_refuses_corrupt();
    return failures;
}
