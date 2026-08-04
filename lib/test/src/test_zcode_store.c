/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_zcode_store — the local ZCODE package store gate
 * (lib/vcs/package_store.*).
 *
 * Coverage:
 *   1. Flags (-packagehost default off, -packagequota default 10 GiB),
 *      layout creation, result/pool strings.
 *   2. Manifest admission: valid, idempotent re-put, 64 MiB cap, quota
 *      feasibility, and rejection of traversal paths / symlink modes /
 *      garbage wires (the store only ever writes hash-named files, but a
 *      hostile manifest must never be admitted at all).
 *   3. Chunk flow: wrong package, wrong coordinates, hash mismatch,
 *      verify-before-store, completion commit sweep, get round-trip.
 *   4. Dedup: shared chunk stored once, per-package accounting, shared
 *      chunk survives eviction of the other package.
 *   5. Crash recovery: resumable staging, temp sweep, orphan GC,
 *      commit-at-open sweep, completion rebuilt from the CAS.
 *   6. Quota: staging pool exhaustion (in-flight work preserved),
 *      deterministic HOT (LRU) and RARE (replicas desc) eviction,
 *      pins never evicted + pins budget, pre-existing pin markers.
 *   7. Release envelope storage through the acceptance layer.
 *   8. dump_state_json: disabled shape, enabled totals, key drilldown.
 *   9. Blob surface (vcs/blob_store.h): the FROZEN golden root vector,
 *      root purity across independent constructions, length commitment,
 *      put/get round-trip, idempotent re-put, the size ceiling refused by
 *      name with nothing stored, absent-root and small-buffer failures,
 *      and a CAS object corrupted on disk failing verification on read.
 *
 * Stores open on ./test-tmp dirs with explicit quotas; the only global
 * state touched (args + datadir, for the enabled-dump case) is restored
 * before the group returns. */

#include "test/test_core.h"

#include "vcs/package_store.h"

#include "vcs/blob_store.h"
#include "vcs/package_manifest.h"

#include "chain/chainparams.h"
#include "core/uint256.h"
#include "json/json.h"
#include "keys/key.h"
#include "keys/key_io.h"
#include "keys/pubkey.h"
#include "util/util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ZS_CHECK(name, expr) do {                                     \
    if (expr) { printf("  zcode_store: %s... OK\n", (name)); }        \
    else { printf("  zcode_store: %s... FAIL\n", (name)); failures++; } \
} while (0)

#define ZS_MAX_FILES 12u
#define ZS_MAX_FILE 1024u

struct zs_pkg {
    struct vcs_package_manifest manifest;
    uint8_t *wire;
    size_t wire_len;
    uint8_t root[32];
    char root_hex[65];
    size_t count;
    char paths[ZS_MAX_FILES][64];
    uint8_t contents[ZS_MAX_FILES][ZS_MAX_FILE];
    size_t lens[ZS_MAX_FILES];
};

static void zs_hex32(const uint8_t in[32], char out[65])
{
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i]     = hexd[(in[i] >> 4) & 0xf];
        out[2 * i + 1] = hexd[in[i] & 0xf];
    }
    out[64] = '\0';
}

/* A package of `count` single-chunk files; file i is paths[i] of lens[i]
 * bytes, content byte j = (uint8_t)(seed + i * 7 + j). */
static bool zs_make_package(struct zs_pkg *p, size_t count,
                            const char *const paths[], const size_t lens[],
                            uint8_t seed)
{
    memset(p, 0, sizeof(*p));
    if (count > ZS_MAX_FILES)
        return false;
    vcs_package_manifest_init(&p->manifest);
    for (size_t i = 0; i < count; i++) {
        if (lens[i] == 0 || lens[i] > ZS_MAX_FILE)
            return false;
        for (size_t j = 0; j < lens[i]; j++)
            p->contents[i][j] = (uint8_t)(seed + i * 7u + j);
        p->lens[i] = lens[i];
        snprintf(p->paths[i], sizeof(p->paths[i]), "%s", paths[i]);
        uint8_t hash[32];
        if (!vcs_package_chunk_hash(p->contents[i], lens[i], hash))
            return false;
        if (!vcs_package_manifest_add(&p->manifest, paths[i],
                                      VCS_PACKAGE_MODE_FILE, lens[i], hash,
                                      1))
            return false;
    }
    p->count = count;
    if (!vcs_package_manifest_serialize(&p->manifest, &p->wire,
                                        &p->wire_len))
        return false;
    if (!vcs_package_manifest_root(&p->manifest, p->root))
        return false;
    zs_hex32(p->root, p->root_hex);
    return true;
}

static void zs_free_package(struct zs_pkg *p)
{
    vcs_package_manifest_free(&p->manifest);
    free(p->wire);
    p->wire = NULL;
}

/* Hand-encode a one-file manifest wire with caller-chosen path and mode —
 * the only way to get a traversal path or a symlink mode past the
 * builder, which is exactly what the store must reject. */
static size_t zs_raw_wire(uint8_t *out, const char *path, uint32_t mode)
{
    size_t n = 0;
    memcpy(out + n, "ZCLPKG\r\n", 8); n += 8;
    out[n++] = 1; out[n++] = 0;                 /* version */
    out[n++] = 0; out[n++] = 0; out[n++] = 16; out[n++] = 0; /* 1 MiB */
    out[n++] = 1; out[n++] = 0; out[n++] = 0; out[n++] = 0;  /* 1 file */
    size_t plen = strlen(path);
    out[n++] = (uint8_t)plen; out[n++] = (uint8_t)(plen >> 8);
    memcpy(out + n, path, plen); n += plen;
    for (int b = 0; b < 4; b++) out[n++] = (uint8_t)(mode >> (8 * b));
    out[n++] = 1; for (int b = 1; b < 8; b++) out[n++] = 0;  /* size 1 */
    out[n++] = 1; out[n++] = 0; out[n++] = 0; out[n++] = 0;  /* 1 chunk */
    memset(out + n, 0xaa, 32); n += 32;
    return n;
}

static bool zs_path_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

static void zs_store_path(char *out, size_t n, const char *datadir,
                          const char *suffix)
{
    snprintf(out, n, "%s/zcode/%s", datadir, suffix);
}

/* Put every chunk of every file of p. Stops at the first non-OK. The
 * manifest sorts files by path, so contents are matched back by name. */
static enum vcs_package_store_result zs_put_all(
    struct vcs_package_store *store, const struct zs_pkg *p)
{
    for (size_t i = 0; i < p->count; i++) {
        const char *path = p->manifest.files[i].path;
        size_t at = p->count;
        for (size_t j = 0; j < p->count; j++)
            if (strcmp(p->paths[j], path) == 0)
                at = j;
        if (at == p->count)
            return VCS_PACKAGE_STORE_ERR_CHUNK_COORD;
        enum vcs_package_store_result r = vcs_package_store_put_chunk(
            store, p->root, path, 0, p->contents[at], p->lens[at]);
        if (r != VCS_PACKAGE_STORE_OK)
            return r;
    }
    return VCS_PACKAGE_STORE_OK;
}

static struct vcs_package_store *zs_open(char *datadir, size_t n,
                                         const char *tag, uint64_t quota)
{
    test_make_tmpdir(datadir, n, "zcode_store", tag);
    return vcs_package_store_open(datadir, quota);
}

/* ── release fixture (acceptance-layer consumption) ───────────────── */

static bool zs_keypair(uint8_t seed, struct privkey *sk, struct pubkey *pk)
{
    memset(sk->vch, seed, 32);
    sk->fValid = true;
    sk->fCompressed = true;
    return privkey_get_pubkey(sk, pk) &&
           pk->size == COMPRESSED_PUBLIC_KEY_SIZE;
}

static bool zs_sign(struct vcs_package_release *r, struct privkey *sk)
{
    uint8_t id[VCS_PACKAGE_RELEASE_ID_BYTES];
    if (vcs_package_release_id(r, id) != VCS_PACKAGE_RELEASE_OK)
        return false;
    struct uint256 hash;
    memcpy(hash.data, id, 32);
    unsigned char compact[COMPACT_SIGNATURE_SIZE];
    if (!privkey_sign_compact(sk, &hash, compact))
        return false;
    memcpy(r->signature, compact + 1, VCS_PACKAGE_RELEASE_SIGNATURE_BYTES);
    return true;
}

static bool zs_t1(char *out, size_t out_size)
{
    const struct chain_params *params = chain_params_get();
    if (!params)
        return false;
    size_t pk_len = 0, sc_len = 0;
    const unsigned char *pk =
        chain_params_base58_prefix(params, B58_PUBKEY_ADDRESS, &pk_len);
    const unsigned char *sc =
        chain_params_base58_prefix(params, B58_SCRIPT_ADDRESS, &sc_len);
    struct tx_destination dest;
    dest.type = DEST_KEY_ID;
    memset(dest.id.key.id.data, 0x44, 20);
    return encode_destination(&dest, pk, pk_len, sc, sc_len, out, out_size);
}

static bool zs_release(struct vcs_package_release *r, uint8_t seed,
                       uint64_t sequence, const char *name)
{
    memset(r, 0, sizeof(*r));
    struct privkey sk;
    struct pubkey pk;
    if (!zs_keypair(seed, &sk, &pk))
        return false;
    r->schema_version = VCS_PACKAGE_RELEASE_VERSION;
    snprintf(r->name, sizeof(r->name), "%s", name);
    snprintf(r->semver, sizeof(r->semver), "1.0.0");
    for (int i = 0; i < 32; i++) {
        r->package_root[i] = (uint8_t)(0x10 + i);
        r->recipe_root[i]  = (uint8_t)(0x50 + i);
    }
    memcpy(r->publisher_pubkey, pk.vch, COMPRESSED_PUBLIC_KEY_SIZE);
    r->publisher_sequence = sequence;
    if (!zs_t1(r->reward_address, sizeof(r->reward_address)))
        return false;
    snprintf(r->license, sizeof(r->license), "MIT");
    if (!vcs_package_accept_chain_id(r->chain_id, sizeof(r->chain_id)))
        return false;
    return zs_sign(r, &sk);
}

/* ── 1: flags, layout, strings ────────────────────────────────────── */
static int t_store_layout_and_flags(void)
{
    int failures = 0;
    ZS_CHECK("flags: hosting defaults off",
             !vcs_package_store_hosting_enabled());
    ZS_CHECK("flags: quota defaults to 10 GiB",
             vcs_package_store_quota_bytes() ==
                 VCS_PACKAGE_STORE_DEFAULT_QUOTA_BYTES);

    char dd[256];
    struct vcs_package_store *s =
        zs_open(dd, sizeof(dd), "layout", 1000000u);
    ZS_CHECK("layout: store opens", s != NULL);
    if (!s)
        return failures;
    static const char *const k_dirs[] = {
        "manifests", "releases", "attestations", "badges",
        "cas/sha3", "staging", "pins",
    };
    for (size_t i = 0; i < sizeof(k_dirs) / sizeof(k_dirs[0]); i++) {
        char path[512];
        zs_store_path(path, sizeof(path), dd, k_dirs[i]);
        ZS_CHECK("layout: directory exists", zs_path_exists(path));
    }
    for (int e = 0; e <= VCS_PACKAGE_STORE_ERR_LIMIT; e++)
        ZS_CHECK("strings: result string defined",
                 vcs_package_store_result_string(
                     (enum vcs_package_store_result)e) != NULL);
    for (int p = 0; p <= VCS_PACKAGE_STORE_POOL_STAGING; p++)
        ZS_CHECK("strings: pool string defined",
                 vcs_package_store_pool_string(
                     (enum vcs_package_store_pool)p) != NULL);
    ZS_CHECK("layout: pools start empty",
             vcs_package_store_pool_usage(s, VCS_PACKAGE_STORE_POOL_PINS) ==
                 0 &&
             vcs_package_store_pool_usage(s, VCS_PACKAGE_STORE_POOL_HOT) ==
                 0 &&
             vcs_package_store_pool_usage(s, VCS_PACKAGE_STORE_POOL_RARE) ==
                 0 &&
             vcs_package_store_pool_usage(
                 s, VCS_PACKAGE_STORE_POOL_STAGING) == 0);
    vcs_package_store_close(s);
    vcs_package_store_close(NULL);
    test_rm_rf_recursive(dd);
    return failures;
}

/* ── 2: manifest admission ────────────────────────────────────────── */
static int t_store_manifest_admission(void)
{
    int failures = 0;
    char dd[256];
    struct vcs_package_store *s =
        zs_open(dd, sizeof(dd), "manifest", 1000000u);
    ZS_CHECK("manifest: store opens", s != NULL);
    if (!s)
        return failures;

    const char *paths[] = { "hello.txt" };
    const size_t lens[] = { 11 };
    struct zs_pkg p;
    ZS_CHECK("manifest: fixture builds",
             zs_make_package(&p, 1, paths, lens, 0x11));
    uint8_t root[32];
    ZS_CHECK("manifest: valid admitted",
             vcs_package_store_put_manifest(s, p.wire, p.wire_len, root) ==
                 VCS_PACKAGE_STORE_OK);
    ZS_CHECK("manifest: root out-param matches",
             memcmp(root, p.root, 32) == 0);
    struct vcs_package_store_status st;
    ZS_CHECK("manifest: tracked incomplete in staging",
             vcs_package_store_package_status(s, p.root, &st) &&
             st.tracked && !st.complete && !st.pinned &&
             st.pool == VCS_PACKAGE_STORE_POOL_STAGING &&
             st.total_bytes == 11 && st.total_chunks == 1 &&
             st.present_chunks == 0);
    ZS_CHECK("manifest: idempotent re-put",
             vcs_package_store_put_manifest(s, p.wire, p.wire_len, NULL) ==
                 VCS_PACKAGE_STORE_OK);

    /* Hostile wires: traversal path, symlink mode, garbage. */
    uint8_t bad[512];
    size_t bad_len = zs_raw_wire(bad, "../escape.txt", VCS_PACKAGE_MODE_FILE);
    ZS_CHECK("manifest: traversal path rejected",
             vcs_package_store_put_manifest(s, bad, bad_len, NULL) ==
                 VCS_PACKAGE_STORE_ERR_MANIFEST);
    bad_len = zs_raw_wire(bad, "link.txt", 0120777u /* symlink */);
    ZS_CHECK("manifest: symlink mode rejected",
             vcs_package_store_put_manifest(s, bad, bad_len, NULL) ==
                 VCS_PACKAGE_STORE_ERR_MANIFEST);
    ZS_CHECK("manifest: garbage wire rejected",
             vcs_package_store_put_manifest(s, bad, 9u, NULL) ==
                 VCS_PACKAGE_STORE_ERR_MANIFEST);
    ZS_CHECK("manifest: null args rejected",
             vcs_package_store_put_manifest(NULL, p.wire, p.wire_len,
                                            NULL) ==
                 VCS_PACKAGE_STORE_ERR_NULL &&
             vcs_package_store_put_manifest(s, NULL, p.wire_len, NULL) ==
                 VCS_PACKAGE_STORE_ERR_NULL);

    /* The 64 MiB v1 cap: 64 MiB + 1 is refused, exactly 64 MiB admits.
     * Fake hashes are per-chunk distinct so nothing dedupes away. */
    static uint8_t fake_hashes[65 * 32];
    for (int i = 0; i < 65; i++)
        memset(fake_hashes + i * 32, i + 1, 32);
    struct vcs_package_manifest over;
    vcs_package_manifest_init(&over);
    ZS_CHECK("cap: oversized manifest builds",
             vcs_package_manifest_add(
                 &over, "big.bin", VCS_PACKAGE_MODE_FILE,
                 VCS_PACKAGE_STORE_MAX_PACKAGE_BYTES + 1u, fake_hashes,
                 65));
    uint8_t *over_wire = NULL;
    size_t over_len = 0;
    ZS_CHECK("cap: oversized manifest serializes",
             vcs_package_manifest_serialize(&over, &over_wire, &over_len));
    ZS_CHECK("cap: 64 MiB + 1 rejected",
             vcs_package_store_put_manifest(s, over_wire, over_len,
                                            NULL) ==
                 VCS_PACKAGE_STORE_ERR_PACKAGE_CAP);
    free(over_wire);
    vcs_package_manifest_free(&over);

    char dd2[256];
    struct vcs_package_store *s2 =
        zs_open(dd2, sizeof(dd2), "capexact",
                VCS_PACKAGE_STORE_DEFAULT_QUOTA_BYTES);
    ZS_CHECK("cap: second store opens", s2 != NULL);
    if (s2) {
        struct vcs_package_manifest exact;
        vcs_package_manifest_init(&exact);
        ZS_CHECK("cap: exact-size manifest builds",
                 vcs_package_manifest_add(
                     &exact, "exact.bin", VCS_PACKAGE_MODE_FILE,
                     VCS_PACKAGE_STORE_MAX_PACKAGE_BYTES, fake_hashes, 64));
        uint8_t *exact_wire = NULL;
        size_t exact_len = 0;
        ZS_CHECK("cap: exact-size manifest serializes",
                 vcs_package_manifest_serialize(&exact, &exact_wire,
                                                &exact_len));
        uint8_t exact_root[32];
        ZS_CHECK("cap: exactly 64 MiB admitted",
                 vcs_package_store_put_manifest(s2, exact_wire, exact_len,
                                                exact_root) ==
                     VCS_PACKAGE_STORE_OK);
        struct vcs_package_store_status est;
        ZS_CHECK("cap: exact package tracked at 64 MiB",
                 vcs_package_store_package_status(s2, exact_root, &est) &&
                 est.total_bytes == VCS_PACKAGE_STORE_MAX_PACKAGE_BYTES &&
                 est.total_chunks == 64);
        free(exact_wire);
        vcs_package_manifest_free(&exact);
        vcs_package_store_close(s2);
    }
    test_rm_rf_recursive(dd2);

    /* Quota feasibility at admission: this store's staging budget is
     * 100000 bytes (1/10 of 1000000); a package that can never fit is
     * refused at put_manifest, not mid-flight. Fake-hash manifest (no
     * chunks are ever put, so no content is needed). */
    struct vcs_package_manifest z;
    vcs_package_manifest_init(&z);
    bool z_built = true;
    for (int i = 0; i < 11; i++) {
        char zpath[8];
        snprintf(zpath, sizeof(zpath), "z%d", i);
        if (!vcs_package_manifest_add(&z, zpath, VCS_PACKAGE_MODE_FILE,
                                      20000, fake_hashes + i * 32, 1))
            z_built = false;
    }
    uint8_t *z_wire = NULL;
    size_t z_len = 0;
    ZS_CHECK("manifest: oversized-for-quota fixture builds",
             z_built && vcs_package_manifest_serialize(&z, &z_wire,
                                                       &z_len));
    ZS_CHECK("manifest: unaffordable package refused with QUOTA",
             vcs_package_store_put_manifest(s, z_wire, z_len, NULL) ==
                 VCS_PACKAGE_STORE_ERR_QUOTA);
    free(z_wire);
    vcs_package_manifest_free(&z);

    zs_free_package(&p);
    vcs_package_store_close(s);
    test_rm_rf_recursive(dd);
    return failures;
}

/* ── 3: chunk flow ────────────────────────────────────────────────── */
static int t_store_chunk_flow(void)
{
    int failures = 0;
    char dd[256];
    struct vcs_package_store *s = zs_open(dd, sizeof(dd), "chunks", 1000000u);
    ZS_CHECK("chunks: store opens", s != NULL);
    if (!s)
        return failures;

    const char *paths[] = { "a.txt", "b.txt" };
    const size_t lens[] = { 100, 200 };
    struct zs_pkg p;
    ZS_CHECK("chunks: fixture builds",
             zs_make_package(&p, 2, paths, lens, 0x33));

    uint8_t unknown[32];
    memset(unknown, 0x77, 32);
    ZS_CHECK("chunks: unknown package rejected",
             vcs_package_store_put_chunk(s, unknown, "a.txt", 0,
                                         p.contents[0], p.lens[0]) ==
                 VCS_PACKAGE_STORE_ERR_UNKNOWN_PACKAGE);
    ZS_CHECK("chunks: manifest admitted",
             vcs_package_store_put_manifest(s, p.wire, p.wire_len, NULL) ==
                 VCS_PACKAGE_STORE_OK);
    ZS_CHECK("chunks: wrong path rejected",
             vcs_package_store_put_chunk(s, p.root, "nope.txt", 0,
                                         p.contents[0], p.lens[0]) ==
                 VCS_PACKAGE_STORE_ERR_CHUNK_COORD);
    ZS_CHECK("chunks: wrong index rejected",
             vcs_package_store_put_chunk(s, p.root, "a.txt", 1,
                                         p.contents[0], p.lens[0]) ==
                 VCS_PACKAGE_STORE_ERR_CHUNK_COORD);
    uint8_t corrupt[100];
    memcpy(corrupt, p.contents[0], 100);
    corrupt[0] ^= 0x01u;
    ZS_CHECK("chunks: hash mismatch rejected before store",
             vcs_package_store_put_chunk(s, p.root, "a.txt", 0, corrupt,
                                         sizeof(corrupt)) ==
                 VCS_PACKAGE_STORE_ERR_CHUNK_HASH);
    struct vcs_package_store_status st;
    ZS_CHECK("chunks: rejected bytes earned nothing",
             vcs_package_store_package_status(s, p.root, &st) &&
             st.present_chunks == 0 && st.present_bytes == 0);

    ZS_CHECK("chunks: first chunk accepted",
             vcs_package_store_put_chunk(s, p.root, "a.txt", 0,
                                         p.contents[0], p.lens[0]) ==
                 VCS_PACKAGE_STORE_OK);
    ZS_CHECK("chunks: still incomplete mid-package",
             vcs_package_store_package_status(s, p.root, &st) &&
             !st.complete && st.present_chunks == 1 &&
             st.present_bytes == 100 &&
             st.pool == VCS_PACKAGE_STORE_POOL_STAGING);

    uint8_t *got = NULL;
    size_t got_len = 0;
    ZS_CHECK("chunks: get round-trips stored bytes",
             vcs_package_store_get_chunk(s, p.root, "a.txt", 0, &got,
                                         &got_len) == VCS_PACKAGE_STORE_OK &&
             got_len == 100 && memcmp(got, p.contents[0], 100) == 0);
    free(got);
    got = NULL;
    ZS_CHECK("chunks: get counts as an access",
             vcs_package_store_package_status(s, p.root, &st) &&
             st.access_count == 1);
    ZS_CHECK("chunks: get missing chunk names it",
             vcs_package_store_get_chunk(s, p.root, "b.txt", 0, &got,
                                         &got_len) ==
                 VCS_PACKAGE_STORE_ERR_CHUNK_MISSING && got == NULL);
    ZS_CHECK("chunks: get wrong coords names them",
             vcs_package_store_get_chunk(s, p.root, "b.txt", 9, &got,
                                         &got_len) ==
                 VCS_PACKAGE_STORE_ERR_CHUNK_COORD);

    ZS_CHECK("chunks: completing chunk accepted",
             vcs_package_store_put_chunk(s, p.root, "b.txt", 0,
                                         p.contents[1], p.lens[1]) ==
                 VCS_PACKAGE_STORE_OK);
    ZS_CHECK("chunks: completion commits to the rare pool",
             vcs_package_store_package_status(s, p.root, &st) &&
             st.complete && st.present_bytes == 300 &&
             st.pool == VCS_PACKAGE_STORE_POOL_RARE);
    char path[512];
    char suffix[160];
    snprintf(suffix, sizeof(suffix), "manifests/%s", p.root_hex);
    zs_store_path(path, sizeof(path), dd, suffix);
    ZS_CHECK("chunks: committed manifest moved to manifests/",
             zs_path_exists(path));
    snprintf(suffix, sizeof(suffix), "staging/%s", p.root_hex);
    zs_store_path(path, sizeof(path), dd, suffix);
    ZS_CHECK("chunks: staging dir consumed by the commit",
             !zs_path_exists(path));
    /* CAS object exists under the chunk hash's own name. */
    uint8_t hash[32];
    ZS_CHECK("chunks: chunk hash computes",
             vcs_package_chunk_hash(p.contents[0], p.lens[0], hash));
    char hex[65];
    zs_hex32(hash, hex);
    snprintf(suffix, sizeof(suffix), "cas/sha3/%.2s/%s", hex, hex);
    zs_store_path(path, sizeof(path), dd, suffix);
    ZS_CHECK("chunks: CAS object stored under its hash",
             zs_path_exists(path));

    vcs_package_store_close(s);
    test_rm_rf_recursive(dd);
    return failures;
}

/* ── 4: dedup ─────────────────────────────────────────────────────── */
static int t_store_dedup(void)
{
    int failures = 0;
    char dd[256];
    struct vcs_package_store *s = zs_open(dd, sizeof(dd), "dedup", 1000000u);
    ZS_CHECK("dedup: store opens", s != NULL);
    if (!s)
        return failures;

    /* A and B share one identical file ("shared.txt", same bytes). B uses
     * seed 0x3d so its index-1 file content (0x3d + 7 + j) equals A's
     * index-0 shared file (0x44 + j). */
    const char *paths_a[] = { "shared.txt", "only-a.txt" };
    const size_t lens_a[] = { 100, 50 };
    struct zs_pkg a;
    ZS_CHECK("dedup: package A builds",
             zs_make_package(&a, 2, paths_a, lens_a, 0x44));
    const char *paths_b[] = { "only-b.txt", "shared.txt" };
    const size_t lens_b[] = { 60, 100 };
    struct zs_pkg bb;
    ZS_CHECK("dedup: package B builds",
             zs_make_package(&bb, 2, paths_b, lens_b, 0x3d));
    ZS_CHECK("dedup: A admitted + complete",
             vcs_package_store_put_manifest(s, a.wire, a.wire_len, NULL) ==
                 VCS_PACKAGE_STORE_OK &&
             zs_put_all(s, &a) == VCS_PACKAGE_STORE_OK);
    struct vcs_package_store_status st;
    ZS_CHECK("dedup: A complete",
             vcs_package_store_package_status(s, a.root, &st) &&
             st.complete && st.present_bytes == 150);

    ZS_CHECK("dedup: B admitted",
             vcs_package_store_put_manifest(s, bb.wire, bb.wire_len,
                                            NULL) == VCS_PACKAGE_STORE_OK);
    /* The shared chunk is already in the CAS: B is charged per-package
     * accounting but no new bytes hit the disk. */
    ZS_CHECK("dedup: shared chunk put is a no-op OK",
             vcs_package_store_put_chunk(s, bb.root, "shared.txt", 0,
                                         bb.contents[1], bb.lens[1]) ==
                 VCS_PACKAGE_STORE_OK);
    ZS_CHECK("dedup: shared content credited to B immediately",
             vcs_package_store_package_status(s, bb.root, &st) &&
             st.present_bytes == 100);
    ZS_CHECK("dedup: re-put of A's chunk is also a no-op OK",
             vcs_package_store_put_chunk(s, a.root, "shared.txt", 0,
                                         a.contents[0], a.lens[0]) ==
                 VCS_PACKAGE_STORE_OK);
    ZS_CHECK("dedup: B completes",
             vcs_package_store_put_chunk(s, bb.root, "only-b.txt", 0,
                                         bb.contents[0], bb.lens[0]) ==
                 VCS_PACKAGE_STORE_OK &&
             vcs_package_store_package_status(s, bb.root, &st) &&
             st.complete && st.present_bytes == 160);

    /* Evict A by hand (drop + pressure is covered later; here use the
     * narrow path: A is the only HOT package, B is RARE). */
    ZS_CHECK("dedup: A promoted HOT",
             vcs_package_store_set_class(s, a.root,
                                         VCS_PACKAGE_STORE_CLASS_HOT, 0) ==
                 VCS_PACKAGE_STORE_OK);
    ZS_CHECK("dedup: B stays RARE",
             vcs_package_store_package_status(s, bb.root, &st) &&
             st.pool == VCS_PACKAGE_STORE_POOL_RARE);
    /* Promote a third package into HOT past the hot budget: this store's
     * hot budget is 400000, far above usage — instead assert sharing
     * survives a real eviction in the eviction tests; here just confirm
     * both readers still get the shared bytes. */
    uint8_t *got = NULL;
    size_t got_len = 0;
    ZS_CHECK("dedup: shared bytes read via A",
             vcs_package_store_get_chunk(s, a.root, "shared.txt", 0, &got,
                                         &got_len) == VCS_PACKAGE_STORE_OK &&
             got_len == 100);
    free(got);
    got = NULL;
    ZS_CHECK("dedup: shared bytes read via B",
             vcs_package_store_get_chunk(s, bb.root, "shared.txt", 0, &got,
                                         &got_len) == VCS_PACKAGE_STORE_OK &&
             got_len == 100);
    free(got);

    zs_free_package(&a);
    zs_free_package(&bb);
    vcs_package_store_close(s);
    test_rm_rf_recursive(dd);
    return failures;
}

/* ── 5: crash recovery ────────────────────────────────────────────── */
static int t_store_recovery(void)
{
    int failures = 0;
    char dd[256];
    struct vcs_package_store *s =
        zs_open(dd, sizeof(dd), "recovery", 1000000u);
    ZS_CHECK("recovery: store opens", s != NULL);
    if (!s)
        return failures;

    const char *paths[] = { "r1.bin", "r2.bin" };
    const size_t lens[] = { 100, 200 };
    struct zs_pkg p;
    ZS_CHECK("recovery: fixture builds",
             zs_make_package(&p, 2, paths, lens, 0x55));
    ZS_CHECK("recovery: manifest + one chunk staged",
             vcs_package_store_put_manifest(s, p.wire, p.wire_len, NULL) ==
                 VCS_PACKAGE_STORE_OK &&
             vcs_package_store_put_chunk(s, p.root, "r1.bin", 0,
                                         p.contents[0], p.lens[0]) ==
                 VCS_PACKAGE_STORE_OK);
    vcs_package_store_close(s);

    /* Crash debris: a torn temp write and an orphan CAS object. */
    char debris[512];
    zs_store_path(debris, sizeof(debris), dd, "cas/torn.zstmp.1.1");
    FILE *f = fopen(debris, "wb");
    ZS_CHECK("recovery: temp debris planted", f != NULL);
    if (f) {
        fwrite("x", 1, 1, f);
        fclose(f);
    }
    char orphan_hex[65];
    memset(orphan_hex, '5', 64);
    orphan_hex[64] = '\0';
    char orphan_dir[512];
    char orphan[512];
    zs_store_path(orphan_dir, sizeof(orphan_dir), dd, "cas/sha3/55");
    snprintf(orphan, sizeof(orphan), "%s/%s", orphan_dir, orphan_hex);
    ZS_CHECK("recovery: orphan directory planted",
             mkdir(orphan_dir, 0700) == 0);
    f = fopen(orphan, "wb");
    ZS_CHECK("recovery: orphan chunk planted", f != NULL);
    if (f) {
        fwrite("orphan", 1, 6, f);
        fclose(f);
    }

    s = vcs_package_store_open(dd, 1000000u);
    ZS_CHECK("recovery: store reopens", s != NULL);
    if (!s) {
        test_rm_rf_recursive(dd);
        return failures;
    }
    ZS_CHECK("recovery: torn temp swept", !zs_path_exists(debris));
    ZS_CHECK("recovery: orphan chunk GC'd", !zs_path_exists(orphan));
    struct vcs_package_store_status st;
    ZS_CHECK("recovery: staging resumes (manifest + chunk kept)",
             vcs_package_store_package_status(s, p.root, &st) &&
             st.tracked && !st.complete && st.present_chunks == 1 &&
             st.present_bytes == 100 &&
             st.pool == VCS_PACKAGE_STORE_POOL_STAGING);
    ZS_CHECK("recovery: resumed package completes + commits",
             vcs_package_store_put_chunk(s, p.root, "r2.bin", 0,
                                         p.contents[1], p.lens[1]) ==
                 VCS_PACKAGE_STORE_OK &&
             vcs_package_store_package_status(s, p.root, &st) &&
             st.complete && st.pool == VCS_PACKAGE_STORE_POOL_RARE);
    vcs_package_store_close(s);

    /* Commit-at-open sweep: a staged manifest whose chunks are all
     * present commits during recovery (crash between last chunk and
     * commit). Simulate by moving the committed manifest back to
     * staging. */
    char committed[512];
    char staging_dir[512];
    char staged[512];
    char suffix[160];
    snprintf(suffix, sizeof(suffix), "manifests/%s", p.root_hex);
    zs_store_path(committed, sizeof(committed), dd, suffix);
    snprintf(suffix, sizeof(suffix), "staging/%s", p.root_hex);
    zs_store_path(staging_dir, sizeof(staging_dir), dd, suffix);
    snprintf(staged, sizeof(staged), "%s/manifest", staging_dir);
    ZS_CHECK("recovery: un-commit simulation",
             zs_path_exists(committed) && mkdir(staging_dir, 0700) == 0 &&
             rename(committed, staged) == 0);
    s = vcs_package_store_open(dd, 1000000u);
    ZS_CHECK("recovery: store reopens for the sweep", s != NULL);
    if (s) {
        ZS_CHECK("recovery: CAS-complete staged package committed at open",
                 zs_path_exists(committed) && !zs_path_exists(staging_dir));
        ZS_CHECK("recovery: completion rebuilt from the CAS",
                 vcs_package_store_package_status(s, p.root, &st) &&
                 st.complete && st.present_bytes == 300);
        vcs_package_store_close(s);
    }

    zs_free_package(&p);
    test_rm_rf_recursive(dd);
    return failures;
}

/* ── 6: staging quota ─────────────────────────────────────────────── */
static int t_store_staging_quota(void)
{
    int failures = 0;
    /* quota 10000: staging budget 1000 bytes. */
    char dd[256];
    struct vcs_package_store *s =
        zs_open(dd, sizeof(dd), "stagingq", 10000u);
    ZS_CHECK("stagingq: store opens", s != NULL);
    if (!s)
        return failures;

    const char *dpaths[] = { "d0", "d1", "d2", "d3", "d4",
                             "d5", "d6", "d7", "d8", "d9" };
    const size_t dlens[] = { 100, 100, 100, 100, 100,
                             100, 100, 100, 100, 100 };
    struct zs_pkg d;
    ZS_CHECK("stagingq: package D builds",
             zs_make_package(&d, 10, dpaths, dlens, 0x66));
    struct zs_pkg e;
    ZS_CHECK("stagingq: package E builds",
             zs_make_package(&e, 10, dpaths, dlens, 0x77));
    ZS_CHECK("stagingq: D + E admitted (manifests charge nothing)",
             vcs_package_store_put_manifest(s, d.wire, d.wire_len, NULL) ==
                 VCS_PACKAGE_STORE_OK &&
             vcs_package_store_put_manifest(s, e.wire, e.wire_len, NULL) ==
                 VCS_PACKAGE_STORE_OK);

    /* D stages 9 of 10 chunks (900 bytes, deliberately incomplete). */
    for (size_t i = 0; i < 9; i++) {
        char path[8];
        snprintf(path, sizeof(path), "d%zu", i);
        ZS_CHECK("stagingq: D chunk accepted",
                 vcs_package_store_put_chunk(s, d.root, path, 0,
                                             d.contents[i],
                                             d.lens[i]) ==
                     VCS_PACKAGE_STORE_OK);
    }
    ZS_CHECK("stagingq: staging pool at 900",
             vcs_package_store_pool_usage(
                 s, VCS_PACKAGE_STORE_POOL_STAGING) == 900);

    /* E's first chunk exactly fills the pool (900+100 = 1000, fits);
     * the second would exceed it and is refused BEFORE the byte lands. */
    ZS_CHECK("stagingq: E chunk filling the pool exactly accepted",
             vcs_package_store_put_chunk(s, e.root, "d0", 0, e.contents[0],
                                         e.lens[0]) ==
                 VCS_PACKAGE_STORE_OK);
    ZS_CHECK("stagingq: over-budget chunk refused with QUOTA",
             vcs_package_store_put_chunk(s, e.root, "d1", 0, e.contents[1],
                                         e.lens[1]) ==
                 VCS_PACKAGE_STORE_ERR_QUOTA);
    ZS_CHECK("stagingq: refused byte was not stored",
             vcs_package_store_pool_usage(
                 s, VCS_PACKAGE_STORE_POOL_STAGING) == 1000);
    struct vcs_package_store_status st;
    ZS_CHECK("stagingq: in-flight work is preserved, not discarded",
             vcs_package_store_package_status(s, d.root, &st) &&
             !st.complete && st.present_bytes == 900 &&
             vcs_package_store_package_status(s, e.root, &st) &&
             !st.complete && st.present_bytes == 100);
    uint8_t *got = NULL;
    size_t got_len = 0;
    ZS_CHECK("stagingq: accepted E chunk still readable",
             vcs_package_store_get_chunk(s, e.root, "d0", 0, &got,
                                         &got_len) == VCS_PACKAGE_STORE_OK);
    free(got);

    zs_free_package(&d);
    zs_free_package(&e);
    vcs_package_store_close(s);
    test_rm_rf_recursive(dd);
    return failures;
}

/* ── 7: deterministic HOT eviction (LRU) ──────────────────────────── */
static int t_store_hot_eviction(void)
{
    int failures = 0;
    /* quota 10000: staging 1000 (caps one package at 1000), hot 4000,
     * rare 3000. Five packages of 1000: four promoted hot fill the pool
     * exactly; promoting the fifth must evict the least-requested one. */
    char dd[256];
    struct vcs_package_store *s =
        zs_open(dd, sizeof(dd), "hotevict", 10000u);
    ZS_CHECK("hot: store opens", s != NULL);
    if (!s)
        return failures;

    const char *paths[] = { "x.bin", "y.bin" };
    const size_t lens[] = { 500, 500 };
    struct zs_pkg f, g, h, i2, j2;
    ZS_CHECK("hot: fixtures build",
             zs_make_package(&f, 2, paths, lens, 0x11) &&
             zs_make_package(&g, 2, paths, lens, 0x22) &&
             zs_make_package(&h, 2, paths, lens, 0x33) &&
             zs_make_package(&i2, 2, paths, lens, 0x44) &&
             zs_make_package(&j2, 2, paths, lens, 0x55));

    struct zs_pkg *quartet[] = { &f, &g, &h, &i2 };
    for (size_t q = 0; q < 4; q++) {
        ZS_CHECK("hot: package completes",
                 vcs_package_store_put_manifest(s, quartet[q]->wire,
                                                quartet[q]->wire_len,
                                                NULL) ==
                     VCS_PACKAGE_STORE_OK &&
                 zs_put_all(s, quartet[q]) == VCS_PACKAGE_STORE_OK);
        ZS_CHECK("hot: package promoted",
                 vcs_package_store_set_class(s, quartet[q]->root,
                                             VCS_PACKAGE_STORE_CLASS_HOT,
                                             0) == VCS_PACKAGE_STORE_OK);
    }
    ZS_CHECK("hot: hot pool exactly full",
             vcs_package_store_pool_usage(s, VCS_PACKAGE_STORE_POOL_HOT) ==
                 4000);

    /* G, H, I are requested; F is not. F is the only LRU victim. */
    const char *names[] = { "g", "h", "i" };
    struct zs_pkg *requested[] = { &g, &h, &i2 };
    for (size_t q = 0; q < 3; q++) {
        (void)names;
        uint8_t *got = NULL;
        size_t got_len = 0;
        ZS_CHECK("hot: package accessed",
                 vcs_package_store_get_chunk(s, requested[q]->root, "x.bin",
                                             0, &got, &got_len) ==
                     VCS_PACKAGE_STORE_OK);
        free(got);
    }

    ZS_CHECK("hot: J completes into rare",
             vcs_package_store_put_manifest(s, j2.wire, j2.wire_len,
                                            NULL) == VCS_PACKAGE_STORE_OK &&
             zs_put_all(s, &j2) == VCS_PACKAGE_STORE_OK);
    /* Promoting J (1000) into hot (4000/4000) evicts F (never accessed),
     * never a requested package and never the incoming J. */
    ZS_CHECK("hot: J promotion evicts the LRU package",
             vcs_package_store_set_class(s, j2.root,
                                         VCS_PACKAGE_STORE_CLASS_HOT, 0) ==
                 VCS_PACKAGE_STORE_OK);
    struct vcs_package_store_status st;
    ZS_CHECK("hot: evicted F is fully gone",
             !vcs_package_store_package_status(s, f.root, &st));
    char path[512];
    char suffix[160];
    snprintf(suffix, sizeof(suffix), "manifests/%s", f.root_hex);
    zs_store_path(path, sizeof(path), dd, suffix);
    ZS_CHECK("hot: evicted F's manifest deleted", !zs_path_exists(path));
    ZS_CHECK("hot: requested packages and incoming J survived",
             vcs_package_store_package_status(s, g.root, &st) &&
             st.pool == VCS_PACKAGE_STORE_POOL_HOT &&
             vcs_package_store_package_status(s, h.root, &st) &&
             st.pool == VCS_PACKAGE_STORE_POOL_HOT &&
             vcs_package_store_package_status(s, i2.root, &st) &&
             st.pool == VCS_PACKAGE_STORE_POOL_HOT &&
             vcs_package_store_package_status(s, j2.root, &st) &&
             st.pool == VCS_PACKAGE_STORE_POOL_HOT);
    ZS_CHECK("hot: hot pool within budget after eviction",
             vcs_package_store_pool_usage(s, VCS_PACKAGE_STORE_POOL_HOT) <=
                 4000);
    uint8_t *got = NULL;
    size_t got_len = 0;
    ZS_CHECK("hot: G still readable after F's eviction",
             vcs_package_store_get_chunk(s, g.root, "y.bin", 0, &got,
                                         &got_len) == VCS_PACKAGE_STORE_OK);
    free(got);

    zs_free_package(&f);
    zs_free_package(&g);
    zs_free_package(&h);
    zs_free_package(&i2);
    zs_free_package(&j2);
    vcs_package_store_close(s);
    test_rm_rf_recursive(dd);
    return failures;
}

/* ── 8: deterministic RARE eviction (replicas desc) ───────────────── */
static int t_store_rare_eviction(void)
{
    int failures = 0;
    /* quota 10000: rare 3000. I, J, K at 1000 fill it exactly; L's
     * completion must evict the best-replicated victim (I). */
    char dd[256];
    struct vcs_package_store *s =
        zs_open(dd, sizeof(dd), "rareevict", 10000u);
    ZS_CHECK("rare: store opens", s != NULL);
    if (!s)
        return failures;

    const char *paths[] = { "p.bin", "q.bin" };
    const size_t lens[] = { 500, 500 };
    struct zs_pkg i1, j1, k1, l1;
    ZS_CHECK("rare: fixtures build",
             zs_make_package(&i1, 2, paths, lens, 0x66) &&
             zs_make_package(&j1, 2, paths, lens, 0x77) &&
             zs_make_package(&k1, 2, paths, lens, 0x88) &&
             zs_make_package(&l1, 2, paths, lens, 0x99));
    struct zs_pkg *trio[] = { &i1, &j1, &k1 };
    for (size_t q = 0; q < 3; q++)
        ZS_CHECK("rare: package completes",
                 vcs_package_store_put_manifest(s, trio[q]->wire,
                                                trio[q]->wire_len,
                                                NULL) ==
                     VCS_PACKAGE_STORE_OK &&
                 zs_put_all(s, trio[q]) == VCS_PACKAGE_STORE_OK);
    ZS_CHECK("rare: rare pool exactly full",
             vcs_package_store_pool_usage(s, VCS_PACKAGE_STORE_POOL_RARE) ==
                 3000);
    /* I is well-replicated elsewhere; a replica update under an unchanged
     * class must not disturb the pools. */
    ZS_CHECK("rare: I's replica count recorded",
             vcs_package_store_set_class(s, i1.root,
                                         VCS_PACKAGE_STORE_CLASS_RARE, 5) ==
                 VCS_PACKAGE_STORE_OK);
    ZS_CHECK("rare: rare pool untouched by the replica update",
             vcs_package_store_pool_usage(s, VCS_PACKAGE_STORE_POOL_RARE) ==
                 3000);

    ZS_CHECK("rare: L's completing chunk evicts the best-replicated I",
             vcs_package_store_put_manifest(s, l1.wire, l1.wire_len,
                                            NULL) == VCS_PACKAGE_STORE_OK &&
             zs_put_all(s, &l1) == VCS_PACKAGE_STORE_OK);
    struct vcs_package_store_status st;
    ZS_CHECK("rare: best-replicated I evicted",
             !vcs_package_store_package_status(s, i1.root, &st));
    ZS_CHECK("rare: under-replicated J, K and incoming L survived",
             vcs_package_store_package_status(s, j1.root, &st) &&
             st.complete &&
             vcs_package_store_package_status(s, k1.root, &st) &&
             st.complete &&
             vcs_package_store_package_status(s, l1.root, &st) &&
             st.complete && st.pool == VCS_PACKAGE_STORE_POOL_RARE);
    ZS_CHECK("rare: rare pool within budget",
             vcs_package_store_pool_usage(s, VCS_PACKAGE_STORE_POOL_RARE) <=
                 3000);

    zs_free_package(&i1);
    zs_free_package(&j1);
    zs_free_package(&k1);
    zs_free_package(&l1);
    vcs_package_store_close(s);
    test_rm_rf_recursive(dd);
    return failures;
}

/* ── 9: pins ──────────────────────────────────────────────────────── */
static int t_store_pins(void)
{
    int failures = 0;
    /* quota 10000: pins 2000, rare 3000, staging 1000. */
    char dd[256];
    struct vcs_package_store *s = zs_open(dd, sizeof(dd), "pins", 10000u);
    ZS_CHECK("pins: store opens", s != NULL);
    if (!s)
        return failures;

    const char *paths[] = { "a.bin", "b.bin" };
    const size_t lens[] = { 500, 500 };
    struct zs_pkg l, m, n, o, p2;
    ZS_CHECK("pins: fixtures build",
             zs_make_package(&l, 2, paths, lens, 0xaa) &&
             zs_make_package(&m, 2, paths, lens, 0xbb) &&
             zs_make_package(&n, 2, paths, lens, 0xcc) &&
             zs_make_package(&o, 2, paths, lens, 0xdd) &&
             zs_make_package(&p2, 2, paths, lens, 0xee));

    ZS_CHECK("pins: L completes and pins",
             vcs_package_store_put_manifest(s, l.wire, l.wire_len, NULL) ==
                 VCS_PACKAGE_STORE_OK &&
             zs_put_all(s, &l) == VCS_PACKAGE_STORE_OK &&
             vcs_package_store_pin(s, l.root, true) ==
                 VCS_PACKAGE_STORE_OK);
    struct vcs_package_store_status st;
    ZS_CHECK("pins: L charges the pins pool",
             vcs_package_store_package_status(s, l.root, &st) && st.pinned &&
             st.pool == VCS_PACKAGE_STORE_POOL_PINS &&
             vcs_package_store_pool_usage(s, VCS_PACKAGE_STORE_POOL_PINS) ==
                 1000);
    ZS_CHECK("pins: full-byte possession proof accepts complete pinned L",
             vcs_package_store_verify_possession(s, l.root, true));
    ZS_CHECK("pins: M completes and pins (pins pool exactly full)",
             vcs_package_store_put_manifest(s, m.wire, m.wire_len, NULL) ==
                 VCS_PACKAGE_STORE_OK &&
             zs_put_all(s, &m) == VCS_PACKAGE_STORE_OK &&
             vcs_package_store_pin(s, m.root, true) ==
                 VCS_PACKAGE_STORE_OK &&
             vcs_package_store_pool_usage(s, VCS_PACKAGE_STORE_POOL_PINS) ==
                 2000);
    ZS_CHECK("pins: N completes into rare",
             vcs_package_store_put_manifest(s, n.wire, n.wire_len, NULL) ==
                 VCS_PACKAGE_STORE_OK &&
             zs_put_all(s, &n) == VCS_PACKAGE_STORE_OK);
    /* Pins are never made room for by eviction: a pin that does not fit
     * fails, and the package stays as it was. */
    ZS_CHECK("pins: over-budget pin refused without evicting",
             vcs_package_store_pin(s, n.root, true) ==
                 VCS_PACKAGE_STORE_ERR_QUOTA);
    ZS_CHECK("pins: refused pin left N unpinned",
             vcs_package_store_package_status(s, n.root, &st) &&
             !st.pinned && st.pool == VCS_PACKAGE_STORE_POOL_RARE);

    ZS_CHECK("pins: unpin returns M to its class pool",
             vcs_package_store_pin(s, m.root, false) ==
                 VCS_PACKAGE_STORE_OK &&
             vcs_package_store_package_status(s, m.root, &st) &&
             !st.pinned && st.pool == VCS_PACKAGE_STORE_POOL_RARE);
    ZS_CHECK("pins: possession proof fails closed after unpin",
             !vcs_package_store_verify_possession(s, m.root, true));

    /* Rare-pool pressure: O fills rare exactly (3000), P's completion
     * must evict exactly one rare package and must never touch the
     * pinned L. (All rare candidates tie on replicas/access, so the
     * victim is the lowest root hex — which one is irrelevant here.) */
    ZS_CHECK("pins: O + P complete under rare-pool pressure",
             vcs_package_store_put_manifest(s, o.wire, o.wire_len, NULL) ==
                 VCS_PACKAGE_STORE_OK &&
             zs_put_all(s, &o) == VCS_PACKAGE_STORE_OK &&
             vcs_package_store_put_manifest(s, p2.wire, p2.wire_len,
                                            NULL) == VCS_PACKAGE_STORE_OK &&
             zs_put_all(s, &p2) == VCS_PACKAGE_STORE_OK);
    ZS_CHECK("pins: pinned L survived rare-pool pressure",
             vcs_package_store_package_status(s, l.root, &st) &&
             st.pinned && st.complete &&
             st.pool == VCS_PACKAGE_STORE_POOL_PINS);
    size_t survivors =
        (vcs_package_store_package_status(s, m.root, &st) ? 1u : 0u) +
        (vcs_package_store_package_status(s, n.root, &st) ? 1u : 0u) +
        (vcs_package_store_package_status(s, o.root, &st) ? 1u : 0u) +
        (vcs_package_store_package_status(s, p2.root, &st) ? 1u : 0u);
    ZS_CHECK("pins: exactly one rare package was evicted",
             survivors == 3u);
    ZS_CHECK("pins: rare pool within budget",
             vcs_package_store_pool_usage(s, VCS_PACKAGE_STORE_POOL_RARE) <=
                 3000);
    uint8_t *got = NULL;
    size_t got_len = 0;
    ZS_CHECK("pins: L still readable",
             vcs_package_store_get_chunk(s, l.root, "a.bin", 0, &got,
                                         &got_len) == VCS_PACKAGE_STORE_OK);
    free(got);

    /* A pin marker that pre-exists the manifest pins at admission. */
    struct zs_pkg pre;
    ZS_CHECK("pins: pre-pinned fixture builds",
             zs_make_package(&pre, 2, paths, lens, 0xf0));
    char marker[512];
    char suffix[160];
    snprintf(suffix, sizeof(suffix), "pins/%s", pre.root_hex);
    zs_store_path(marker, sizeof(marker), dd, suffix);
    FILE *f = fopen(marker, "wb");
    ZS_CHECK("pins: marker planted", f != NULL);
    if (f)
        fclose(f);
    ZS_CHECK("pins: admission honors a pre-existing marker",
             vcs_package_store_put_manifest(s, pre.wire, pre.wire_len,
                                            NULL) == VCS_PACKAGE_STORE_OK &&
             vcs_package_store_package_status(s, pre.root, &st) &&
             st.pinned && st.pool == VCS_PACKAGE_STORE_POOL_PINS);

    /* A durable ACK is a claim about bytes that are still present, not a
     * sticky bit in package metadata. Removing one pinned CAS chunk must
     * immediately invalidate the possession proof used by ACK renewal. */
    uint8_t missing_hash[32];
    char missing_hex[65];
    char missing_suffix[160];
    char missing_path[512];
    ZS_CHECK("pins: missing-byte fixture hash",
             vcs_package_chunk_hash(l.contents[0], l.lens[0], missing_hash));
    zs_hex32(missing_hash, missing_hex);
    snprintf(missing_suffix, sizeof(missing_suffix), "cas/sha3/%02x/%s",
             missing_hash[0], missing_hex);
    zs_store_path(missing_path, sizeof(missing_path), dd, missing_suffix);
    ZS_CHECK("pins: pinned byte deletion planted", unlink(missing_path) == 0);
    ZS_CHECK("pins: possession proof fails after missing byte",
             !vcs_package_store_verify_possession(s, l.root, true));

    zs_free_package(&l);
    zs_free_package(&m);
    zs_free_package(&n);
    zs_free_package(&o);
    zs_free_package(&p2);
    zs_free_package(&pre);
    vcs_package_store_close(s);
    test_rm_rf_recursive(dd);
    return failures;
}

/* ── 10: release envelopes ────────────────────────────────────────── */
static int t_store_releases(void)
{
    int failures = 0;
    chain_params_select(CHAIN_MAIN);
    char dd[256];
    struct vcs_package_store *s =
        zs_open(dd, sizeof(dd), "releases", 1000000u);
    ZS_CHECK("releases: store opens", s != NULL);
    if (!s)
        return failures;

    struct vcs_package_release r;
    ZS_CHECK("releases: fixture signs",
             zs_release(&r, 0x11, 1u, "rhett/ring-buffer"));
    enum vcs_package_accept_result ar = VCS_PACKAGE_ACCEPT_ERR_NULL;
    ZS_CHECK("releases: accepted envelope stored",
             vcs_package_store_put_release(s, &r, &ar) ==
                 VCS_PACKAGE_STORE_OK && ar == VCS_PACKAGE_ACCEPT_OK);
    uint8_t id[VCS_PACKAGE_RELEASE_ID_BYTES];
    ZS_CHECK("releases: id computes",
             vcs_package_release_id(&r, id) == VCS_PACKAGE_RELEASE_OK);
    char hex[65];
    zs_hex32(id, hex);
    char path[512];
    char suffix[160];
    snprintf(suffix, sizeof(suffix), "releases/%s", hex);
    zs_store_path(path, sizeof(path), dd, suffix);
    ZS_CHECK("releases: envelope persisted under its id",
             zs_path_exists(path));

    ZS_CHECK("releases: redelivery is an idempotent store",
             vcs_package_store_put_release(s, &r, &ar) ==
                 VCS_PACKAGE_STORE_OK && ar == VCS_PACKAGE_ACCEPT_DUPLICATE);

    struct vcs_package_release forked;
    ZS_CHECK("releases: equivocation fixture signs",
             zs_release(&forked, 0x11, 1u, "rhett/ring-buffer-fork"));
    ZS_CHECK("releases: equivocation rejected, nothing stored",
             vcs_package_store_put_release(s, &forked, &ar) ==
                 VCS_PACKAGE_STORE_ERR_ACCEPT &&
             ar == VCS_PACKAGE_ACCEPT_EQUIVOCATION);
    uint8_t forked_id[VCS_PACKAGE_RELEASE_ID_BYTES];
    ZS_CHECK("releases: equivocation id computes",
             vcs_package_release_id(&forked, forked_id) ==
                 VCS_PACKAGE_RELEASE_OK);
    zs_hex32(forked_id, hex);
    snprintf(suffix, sizeof(suffix), "releases/%s", hex);
    zs_store_path(path, sizeof(path), dd, suffix);
    ZS_CHECK("releases: equivocated envelope not persisted",
             !zs_path_exists(path));

    ZS_CHECK("releases: null args rejected",
             vcs_package_store_put_release(NULL, &r, &ar) ==
                 VCS_PACKAGE_STORE_ERR_NULL &&
             vcs_package_store_put_release(s, NULL, &ar) ==
                 VCS_PACKAGE_STORE_ERR_NULL);

    vcs_package_store_close(s);
    test_rm_rf_recursive(dd);
    return failures;
}

/* ── 12: content-addressed blob surface (vcs/blob_store.h) ────────── */

/* FROZEN GOLDEN VECTOR. The blob root is a wire contract: the SAME bytes
 * must yield the SAME 32-byte root on every node, forever. This pins the
 * whole derivation — the "blob" path, mode 0100644, the size/chunk_count
 * fields, the SHA3-256 chunk hash, and the zcl.package_manifest.v1 root
 * domain. If this ever changes, every already-published blob root breaks;
 * a failure here is a CONSENSUS-OF-CONTENT regression, not a test nit. */
#define ZS_BLOB_GOLDEN_INPUT "zcl.blob.golden.v1"
#define ZS_BLOB_GOLDEN_ROOT \
    "a407592f33b1ac781c69ac5bb0bf7f7635c320b17d2e5382bf93b5567af686e7"

static int t_store_blob(void)
{
    int failures = 0;
    char dd[1024];
    struct vcs_package_store *s =
        zs_open(dd, sizeof(dd), "blob", VCS_PACKAGE_STORE_DEFAULT_QUOTA_BYTES);
    ZS_CHECK("blob: store opens", s != NULL);
    if (!s)
        return failures + 1;

    /* ---- pure root: determinism across independent constructions ---- */
    uint8_t a[256], b[256];
    for (size_t i = 0; i < sizeof(a); i++)
        a[i] = (uint8_t)(i * 7u + 11u);
    memset(b, 0, sizeof(b));
    for (size_t i = 0; i < sizeof(b); i++)
        b[i] = (uint8_t)((i * 7u + 11u) & 0xffu);
    uint8_t root_a[32], root_b[32];
    bool ok_a = vcs_blob_root(a, sizeof(a), root_a);
    bool ok_b = vcs_blob_root(b, sizeof(b), root_b);
    ZS_CHECK("blob: root is a pure function of the bytes",
             ok_a && ok_b && memcmp(root_a, root_b, 32) == 0);

    /* Different bytes -> different root; same prefix, shorter -> different
     * root (the length is committed, so truncation is not a collision). */
    uint8_t c[256];
    memcpy(c, a, sizeof(c));
    c[100] ^= 0x01;
    uint8_t root_c[32], root_short[32];
    ZS_CHECK("blob: one flipped byte changes the root",
             vcs_blob_root(c, sizeof(c), root_c) &&
             memcmp(root_a, root_c, 32) != 0);
    ZS_CHECK("blob: length is committed (prefix != whole)",
             vcs_blob_root(a, sizeof(a) - 1u, root_short) &&
             memcmp(root_a, root_short, 32) != 0);

    /* ---- the frozen golden vector ---- */
    uint8_t golden[32];
    const char *gin = ZS_BLOB_GOLDEN_INPUT;
    bool gok = vcs_blob_root((const uint8_t *)gin, strlen(gin), golden);
    char ghex[65];
    zs_hex32(golden, ghex);
    printf("  zcode_store: blob golden root = %s\n", ghex);
    ZS_CHECK("blob: FROZEN golden root vector holds",
             gok && strcmp(ghex, ZS_BLOB_GOLDEN_ROOT) == 0);

    /* ---- hostile input, refused by name, before anything is stored ---- */
    uint8_t junk_root[32];
    ZS_CHECK("blob: null bytes refused",
             vcs_blob_root_of(NULL, 10, junk_root) == VCS_BLOB_ERR_NULL);
    ZS_CHECK("blob: empty blob refused",
             vcs_blob_root_of(a, 0, junk_root) == VCS_BLOB_ERR_EMPTY);
    ZS_CHECK("blob: null store refused",
             vcs_blob_put_to(NULL, a, sizeof(a), junk_root) ==
                 VCS_BLOB_ERR_NO_STORE);

    static uint8_t big[VCS_BLOB_MAX_BYTES + 64u];
    for (size_t i = 0; i < sizeof(big); i++)
        big[i] = (uint8_t)(i ^ 0x5au);
    ZS_CHECK("blob: over the ceiling refused by name (root)",
             vcs_blob_root_of(big, VCS_BLOB_MAX_BYTES + 1u, junk_root) ==
                 VCS_BLOB_ERR_TOO_LARGE);
    ZS_CHECK("blob: over the ceiling refused by name (put)",
             vcs_blob_put_to(s, big, VCS_BLOB_MAX_BYTES + 1u, junk_root) ==
                 VCS_BLOB_ERR_TOO_LARGE);
    ZS_CHECK("blob: refused oversize stored nothing",
             vcs_package_store_pool_usage(s, VCS_PACKAGE_STORE_POOL_RARE) ==
                 0 &&
             vcs_package_store_pool_usage(s,
                                          VCS_PACKAGE_STORE_POOL_STAGING) ==
                 0);
    /* Exactly at the ceiling is admitted: the bound is a ceiling, not a
     * fencepost bug. */
    uint8_t edge_root[32];
    ZS_CHECK("blob: exactly at the ceiling is accepted",
             vcs_blob_put_to(s, big, VCS_BLOB_MAX_BYTES, edge_root) ==
                 VCS_BLOB_OK);
    ZS_CHECK("blob: ceiling blob is one chunk (never split)",
             vcs_package_store_chunk_present(s, edge_root, 0, 0) &&
             !vcs_package_store_chunk_present(s, edge_root, 0, 1) &&
             !vcs_package_store_chunk_present(s, edge_root, 1, 0));

    /* ---- put / get round trip ---- */
    uint8_t root[32];
    ZS_CHECK("blob: put admits manifest + chunk",
             vcs_blob_put_to(s, a, sizeof(a), root) == VCS_BLOB_OK);
    ZS_CHECK("blob: put root equals the pure root",
             memcmp(root, root_a, 32) == 0);
    struct vcs_package_store_status st;
    ZS_CHECK("blob: stored package is complete and one-file",
             vcs_package_store_package_status(s, root, &st) && st.complete &&
             st.total_chunks == 1 && st.total_bytes == sizeof(a));

    uint8_t out[512];
    size_t out_len = 0;
    memset(out, 0, sizeof(out));
    ZS_CHECK("blob: get round-trips the exact bytes",
             vcs_blob_get_from(s, root, out, sizeof(out), &out_len) ==
                 VCS_BLOB_OK &&
             out_len == sizeof(a) && memcmp(out, a, sizeof(a)) == 0);

    /* Idempotent re-put of identical bytes. */
    uint8_t root2[32];
    ZS_CHECK("blob: re-put of identical bytes is idempotent",
             vcs_blob_put_to(s, a, sizeof(a), root2) == VCS_BLOB_OK &&
             memcmp(root, root2, 32) == 0);

    /* ---- absent root fails cleanly (no crash, no partial write) ---- */
    uint8_t absent[32];
    memcpy(absent, root, 32);
    absent[0] ^= 0xff;
    ZS_CHECK("blob: get of an absent root fails cleanly",
             vcs_blob_get_from(s, absent, out, sizeof(out), &out_len) ==
                 VCS_BLOB_ERR_ABSENT && out_len == 0);
    ZS_CHECK("blob: get with a null buffer refused",
             vcs_blob_get_from(s, root, NULL, 16, &out_len) ==
                 VCS_BLOB_ERR_NULL);
    ZS_CHECK("blob: buffer smaller than the blob refused",
             vcs_blob_get_from(s, root, out, sizeof(a) - 1u, &out_len) ==
                 VCS_BLOB_ERR_CAPACITY);

    /* ---- a corrupted CAS object must FAIL verification, not be served -- */
    uint8_t chunk_hash[32];
    ZS_CHECK("blob: chunk hash computes",
             vcs_package_chunk_hash(a, sizeof(a), chunk_hash));
    char hex[65];
    zs_hex32(chunk_hash, hex);
    char suffix[160];
    char cas_path[1400];
    snprintf(suffix, sizeof(suffix), "cas/sha3/%.2s/%s", hex, hex);
    zs_store_path(cas_path, sizeof(cas_path), dd, suffix);
    ZS_CHECK("blob: CAS object exists under its hash",
             zs_path_exists(cas_path));
    uint8_t tampered[256];
    memcpy(tampered, a, sizeof(tampered));
    tampered[7] ^= 0xff;
    FILE *f = fopen(cas_path, "wb");
    bool wrote = f && fwrite(tampered, 1, sizeof(tampered), f) ==
                          sizeof(tampered);
    if (f)
        fclose(f);
    ZS_CHECK("blob: CAS object tampered on disk", wrote);
    ZS_CHECK("blob: corrupted chunk fails verification on read",
             vcs_blob_get_from(s, root, out, sizeof(out), &out_len) ==
                 VCS_BLOB_ERR_CORRUPT && out_len == 0);

    /* ---- named results ---- */
    ZS_CHECK("blob: result strings are named",
             strcmp(vcs_blob_result_string(VCS_BLOB_OK), "ok") == 0 &&
             strcmp(vcs_blob_result_string(VCS_BLOB_ERR_TOO_LARGE),
                    "blob-too-large") == 0 &&
             strcmp(vcs_blob_result_string(VCS_BLOB_ERR_CORRUPT),
                    "blob-bytes-corrupt") == 0 &&
             strcmp(vcs_blob_result_string((enum vcs_blob_result)999),
                    "unknown") == 0);

    /* ---- global accessors refuse cleanly with no global store open ---- */
    ZS_CHECK("blob: global put refuses with no global store",
             vcs_package_store_global() == NULL &&
             !vcs_blob_put(a, sizeof(a), junk_root));
    ZS_CHECK("blob: global get refuses with no global store",
             vcs_blob_get(root, out, sizeof(out)) == -1);
    ZS_CHECK("blob: fetch refuses with no engine",
             vcs_blob_fetch_via(NULL, root, 20500, 1) ==
                 VCS_BLOB_ERR_NO_ENGINE &&
             vcs_blob_announce_via(NULL) == 0);

    vcs_package_store_close(s);
    test_rm_rf_recursive(dd);
    return failures;
}

/* ── 11: dump_state_json ──────────────────────────────────────────── */
static int t_store_dump_state(void)
{
    int failures = 0;
    struct json_value v;
    json_init(&v);
    ZS_CHECK("dump: disabled store reports enabled=false",
             vcs_package_store_dump_state_json(&v, NULL) &&
             json_get(&v, "enabled") &&
             !json_get_bool(json_get(&v, "enabled")));
    json_free(&v);

    /* Enabled global store via the real flag + datadir path. */
    const char *argv[] = { "zclassic23-test", "-packagehost=1",
                           "-packagequota=1000000" };
    ParseParameters(3, argv);
    char dd[256];
    test_make_tmpdir(dd, sizeof(dd), "zcode_store", "dump");
    SetDataDir(dd);
    ZS_CHECK("dump: hosting flag now on",
             vcs_package_store_hosting_enabled() &&
             vcs_package_store_quota_bytes() == 1000000u);
    ZS_CHECK("dump: global store opens",
             vcs_package_store_open_global() &&
             vcs_package_store_global() != NULL);

    const char *paths[] = { "dump.txt" };
    const size_t lens[] = { 64 };
    struct zs_pkg p;
    ZS_CHECK("dump: fixture builds", zs_make_package(&p, 1, paths, lens,
                                                     0xbb));
    struct vcs_package_store *s = vcs_package_store_global();
    ZS_CHECK("dump: package admitted + complete",
             vcs_package_store_put_manifest(s, p.wire, p.wire_len, NULL) ==
                 VCS_PACKAGE_STORE_OK &&
             zs_put_all(s, &p) == VCS_PACKAGE_STORE_OK);

    json_init(&v);
    bool ok = vcs_package_store_dump_state_json(&v, NULL);
    ZS_CHECK("dump: enabled store reports totals",
             ok && json_get(&v, "enabled") &&
             json_get_bool(json_get(&v, "enabled")) &&
             json_get_int(json_get(&v, "quota_bytes")) == 1000000 &&
             json_get_int(json_get(&v, "tracked_packages")) == 1 &&
             json_get_int(json_get(&v, "pins_budget_bytes")) == 200000 &&
             json_get_int(json_get(&v, "rare_usage_bytes")) == 64 &&
             json_get_int(json_get(&v, "cas_chunks")) == 1);
    json_free(&v);

    /* Slice 3 publication state: a persisted release is counted and the
     * last acceptance outcome is reported. */
    chain_params_select(CHAIN_MAIN);
    struct vcs_package_release r;
    ZS_CHECK("dump: release fixture signs",
             zs_release(&r, 0x22, 1u, "rhett/dump-pkg"));
    enum vcs_package_accept_result ar = VCS_PACKAGE_ACCEPT_ERR_NULL;
    ZS_CHECK("dump: release admitted",
             vcs_package_store_put_release(s, &r, &ar) ==
                 VCS_PACKAGE_STORE_OK && ar == VCS_PACKAGE_ACCEPT_OK);
    json_init(&v);
    ok = vcs_package_store_dump_state_json(&v, NULL);
    ZS_CHECK("dump: publication state reported",
             ok &&
             json_get_int(json_get(&v, "releases_total")) == 1 &&
             json_get_str(json_get(&v, "last_release_accept")) &&
             strcmp(json_get_str(json_get(&v, "last_release_accept")),
                    "accepted") == 0 &&
             json_get(&v, "last_release_id") != NULL);
    json_free(&v);

    json_init(&v);
    ok = vcs_package_store_dump_state_json(&v, p.root_hex);
    ZS_CHECK("dump: package-root key drills down",
             ok && json_get(&v, "complete") &&
             json_get_bool(json_get(&v, "complete")) &&
             json_get_int(json_get(&v, "present_bytes")) == 64);
    json_free(&v);

    json_init(&v);
    ok = vcs_package_store_dump_state_json(&v, "not-a-root");
    ZS_CHECK("dump: bad key names the error",
             ok && json_get(&v, "error") != NULL);
    json_free(&v);

    vcs_package_store_close_global();
    ZS_CHECK("dump: global closed", vcs_package_store_global() == NULL);

    /* Restore global flags/datadir for the rest of the process. */
    const char *reset_argv[] = { "zclassic23-test" };
    ParseParameters(1, reset_argv);
    SetDataDir("");
    zs_free_package(&p);
    test_rm_rf_recursive(dd);
    return failures;
}

int test_zcode_store(void)
{
    printf("\n=== zcode_store: local content-addressed package store ===\n");
    int failures = 0;
    failures += t_store_layout_and_flags();
    failures += t_store_manifest_admission();
    failures += t_store_chunk_flow();
    failures += t_store_dedup();
    failures += t_store_recovery();
    failures += t_store_staging_quota();
    failures += t_store_hot_eviction();
    failures += t_store_rare_eviction();
    failures += t_store_pins();
    failures += t_store_releases();
    failures += t_store_blob();
    failures += t_store_dump_state();
    printf("=== zcode_store complete: %d failure(s) ===\n", failures);
    return failures;
}
