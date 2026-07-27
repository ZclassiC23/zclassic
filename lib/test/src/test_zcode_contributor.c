/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_zcode_contributor — the ZCODE contributor identity + ZNAM pointer
 * gate (lib/vcs/package_contributor.*, app/services/zcode_pointer.*, and
 * the slice-4 handlers in tools/command/native_zcode_contributor_command.c).
 *
 * Coverage:
 *   1. contributor show: authoritative release facts from the signed
 *      envelopes, plus the ZNAM publisher-profile pointer with its binding
 *      proof (bound == znam-owner == P2PKH(claimed pubkey)); unknown keys
 *      report has_published=false and no profile; BAD_PUBKEY rejection.
 *   2. contributor packages: bounded rows from the index publisher filter,
 *      empty publisher, BAD_PUBKEY rejection.
 *   3. package resolve: pointer and identity in separate output objects,
 *      the binding proof fields, and every rejection — UNKNOWN_NAME,
 *      NOT_A_ZCODE_POINTER, WRONG_POINTER_KIND, POINTER_NOT_BOUND,
 *      RELEASE_NOT_HOSTED.
 *   4. pointer move: re-pointing the name at a second release's root moves
 *      resolution but leaves both signed releases hosted and unchanged.
 *   5. impersonation: a name bound to an ATTACKER key that points at a
 *      victim's package root resolves with matches_pointer_publisher=false
 *      and the identity still read from the signed release.
 *   6. rebuild equivalence: reopening node.db and rebuilding the package
 *      index yields the same profile output (the index holds no truth).
 *
 * Handlers run in-process on ./test-tmp datadirs; ZNAM rows are seeded into
 * a file-backed <datadir>/node.db through the canonical model (the same
 * upsert the on-chain fold performs). CHAIN_MAIN is pinned so the binding
 * P2PKH derivation is deterministic. */

#include "test/test_core.h"

#include "command/native_command.h"

#include "chain/chainparams.h"
#include "json/json.h"
#include "kernel/command_registry.h"
#include "keys/key.h"
#include "keys/key_io.h"
#include "keys/pubkey.h"
#include "models/database.h"
#include "models/znam.h"
#include "services/zcode_pointer.h"
#include "vcs/package_index.h"
#include "vcs/package_publish.h"
#include "vcs/package_recipe.h"
#include "vcs/package_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ZC4_CHECK(name, expr) do {                                    \
    if (expr) { printf("  zcode_contributor: %s... OK\n", (name)); }  \
    else { printf("  zcode_contributor: %s... FAIL\n", (name)); \
        failures++; }                                                 \
} while (0)

/* ── fixtures (test_zcode_publish.c pattern) ────────────────────────── */

static bool zc4_keypair(uint8_t seed, struct privkey *sk, struct pubkey *pk)
{
    memset(sk->vch, seed, 32);
    sk->fValid = true;
    sk->fCompressed = true;
    return privkey_get_pubkey(sk, pk) &&
           pk->size == COMPRESSED_PUBLIC_KEY_SIZE;
}

/* ── declarative build recipe fixture (slice 5) ───────────────────────
 * Publication now requires a recipe whose root the envelope commits; the
 * commit fixture packages are all LICENSE + src/x.c, so one canonical
 * fixture recipe covers them. The globals feed zc4_release (the
 * committed recipe_root) and zc4_commit_one (the recipe wire hex). */
static char g_zc4_recipe_hex[2 * 1024 + 1];
static uint8_t g_zc4_recipe_root[32];
static bool g_zc4_recipe_ready;

static bool zc4_use_recipe(void)
{
    struct vcs_package_recipe r;
    vcs_package_recipe_init(&r);
    bool ok = vcs_package_recipe_add_source(&r, "src/x.c", NULL) &&
              vcs_package_recipe_add_define(&r, "ZCL_FIXTURE=1", NULL) &&
              vcs_package_recipe_add_library(&r, VCS_PACKAGE_RECIPE_LIB_LIBC,
                                             NULL);
    vcs_package_recipe_set_test_limits(&r, 0, 60,
                                       UINT64_C(64) * 1024u * 1024u);
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    if (ok)
        ok = vcs_package_recipe_root(&r, g_zc4_recipe_root) ==
                 VCS_PACKAGE_RECIPE_OK &&
             vcs_package_recipe_serialize(&r, &wire, &wire_len) ==
                 VCS_PACKAGE_RECIPE_OK;
    vcs_package_recipe_free(&r);
    if (!ok || !wire || 2 * wire_len + 1 > sizeof(g_zc4_recipe_hex)) {
        free(wire);
        return false;
    }
    static const char hexd[] = "0123456789abcdef";
    for (size_t i = 0; i < wire_len; i++) {
        g_zc4_recipe_hex[2 * i]     = hexd[(wire[i] >> 4) & 0xf];
        g_zc4_recipe_hex[2 * i + 1] = hexd[wire[i] & 0xf];
    }
    g_zc4_recipe_hex[2 * wire_len] = '\0';
    free(wire);
    g_zc4_recipe_ready = true;
    return true;
}

static bool zc4_pubkey_hex(uint8_t seed, char out[67])
{
    static const char hexd[] = "0123456789abcdef";
    struct privkey sk;
    struct pubkey pk;
    if (!zc4_keypair(seed, &sk, &pk))
        return false;
    for (size_t i = 0; i < pk.size; i++) {
        out[2 * i]     = hexd[(pk.vch[i] >> 4) & 0xf];
        out[2 * i + 1] = hexd[pk.vch[i] & 0xf];
    }
    out[2 * pk.size] = '\0';
    return true;
}

/* The P2PKH address of the key on the active chain — the ZNAM owner that
 * makes a pointer BOUND. */
static bool zc4_p2pkh_of_seed(uint8_t seed, char *out, size_t out_cap)
{
    struct privkey sk;
    struct pubkey pk;
    if (!zc4_keypair(seed, &sk, &pk))
        return false;
    return zcode_pointer_expected_owner(pk.vch, out, out_cap);
}

static bool zc4_sign(struct vcs_package_release *r, struct privkey *sk)
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

static bool zc4_t1_reward(char *out, size_t out_size)
{
    const struct chain_params *params = chain_params_get();
    if (!params)
        return false;
    size_t pubkey_len = 0;
    size_t script_len = 0;
    const unsigned char *pubkey_prefix =
        chain_params_base58_prefix(params, B58_PUBKEY_ADDRESS, &pubkey_len);
    const unsigned char *script_prefix =
        chain_params_base58_prefix(params, B58_SCRIPT_ADDRESS, &script_len);
    if (!pubkey_prefix || !script_prefix)
        return false;
    struct tx_destination dest;
    dest.type = DEST_KEY_ID;
    memset(dest.id.key.id.data, 0x33, 20);
    return encode_destination(&dest, pubkey_prefix, pubkey_len,
                              script_prefix, script_len, out, out_size);
}

static bool zc4_release(struct vcs_package_release *r, uint8_t key_seed,
                        uint64_t sequence, const char *name,
                        const char *license, const uint8_t package_root[32],
                        const char *znam)
{
    memset(r, 0, sizeof(*r));
    struct privkey sk;
    struct pubkey pk;
    if (!zc4_keypair(key_seed, &sk, &pk))
        return false;
    r->schema_version = VCS_PACKAGE_RELEASE_VERSION;
    snprintf(r->name, sizeof(r->name), "%s", name);
    snprintf(r->semver, sizeof(r->semver), "1.0.0");
    memcpy(r->package_root, package_root, 32);
    r->has_parent = false;
    memcpy(r->publisher_pubkey, pk.vch, COMPRESSED_PUBLIC_KEY_SIZE);
    r->publisher_sequence = sequence;
    if (!zc4_t1_reward(r->reward_address, sizeof(r->reward_address)))
        return false;
    snprintf(r->license, sizeof(r->license), "%s", license);
    memcpy(r->recipe_root, g_zc4_recipe_root, 32);
    r->has_znam = znam != NULL;
    if (znam)
        snprintf(r->znam, sizeof(r->znam), "%s", znam);
    if (!vcs_package_accept_chain_id(r->chain_id, sizeof(r->chain_id)))
        return false;
    return zc4_sign(r, &sk);
}

static char *zc4_hex(const uint8_t *data, size_t len)
{
    static const char hexd[] = "0123456789abcdef";
    char *out = malloc(2 * len + 1);
    if (!out)
        return NULL;
    for (size_t i = 0; i < len; i++) {
        out[2 * i]     = hexd[(data[i] >> 4) & 0xf];
        out[2 * i + 1] = hexd[data[i] & 0xf];
    }
    out[2 * len] = '\0';
    return out;
}

/* ── in-process command runner ──────────────────────────────────────── */

struct zc4_cmd {
    struct json_value input;
    struct zcl_command_request request;
    struct zcl_command_reply reply;
};

static void zc4_cmd_init(struct zc4_cmd *c)
{
    json_init(&c->input);
    json_set_object(&c->input);
    memset(&c->request, 0, sizeof(c->request));
    c->request.input = &c->input;
    zcl_command_reply_init(&c->reply, "zcl.zcode_contributor_test.v1");
}

static void zc4_cmd_free(struct zc4_cmd *c)
{
    zcl_command_reply_free(&c->reply);
    json_free(&c->input);
}

/* Commit one small package; root_hex_out (65) gets the package root. */
static bool zc4_commit_one(const char *dd, uint8_t key_seed, uint64_t seq,
                           const char *name, const char *license,
                           int content_seed, const char *znam,
                           char root_hex_out[65])
{
    if (!g_zc4_recipe_ready && !zc4_use_recipe())
        return false;
    char pkgdir[512];
    snprintf(pkgdir, sizeof(pkgdir), "%s/src-%d", dd, content_seed);
    mkdir(pkgdir, 0700);

    struct vcs_package_manifest manifest;
    vcs_package_manifest_init(&manifest);
    char license_text[96];
    snprintf(license_text, sizeof(license_text),
             "%s\nsee the LICENSE file, variant %d\n", license,
             content_seed);
    char source[64];
    snprintf(source, sizeof(source), "int x_%d;\n", content_seed);
    char path[512];
    snprintf(path, sizeof(path), "%s/LICENSE", pkgdir);
    FILE *f = fopen(path, "wb");
    char srcdir[512];
    snprintf(srcdir, sizeof(srcdir), "%s/src", pkgdir);
    mkdir(srcdir, 0700);
    char srcpath[512];
    snprintf(srcpath, sizeof(srcpath), "%s/x.c", srcdir);
    FILE *g = fopen(srcpath, "wb");
    bool ok = f && g;
    uint8_t lic_hash[32];
    uint8_t src_hash[32];
    if (ok)
        ok = fwrite(license_text, 1, strlen(license_text), f) ==
             strlen(license_text);
    if (f)
        fclose(f);
    if (ok)
        ok = fwrite(source, 1, strlen(source), g) == strlen(source);
    if (g)
        fclose(g);
    if (ok)
        ok = vcs_package_chunk_hash((const uint8_t *)license_text,
                                    strlen(license_text), lic_hash) &&
             vcs_package_chunk_hash((const uint8_t *)source,
                                    strlen(source), src_hash) &&
             vcs_package_manifest_add(&manifest, "LICENSE",
                                      VCS_PACKAGE_MODE_FILE,
                                      strlen(license_text), lic_hash, 1) &&
             vcs_package_manifest_add(&manifest, "src/x.c",
                                      VCS_PACKAGE_MODE_FILE,
                                      strlen(source), src_hash, 1);
    uint8_t *m_wire = NULL;
    size_t m_wire_len = 0;
    uint8_t root[32];
    if (ok)
        ok = vcs_package_manifest_serialize(&manifest, &m_wire,
                                            &m_wire_len) &&
             vcs_package_manifest_root(&manifest, root);
    vcs_package_manifest_free(&manifest);

    struct vcs_package_release r;
    if (ok)
        ok = zc4_release(&r, key_seed, seq, name, license, root, znam);
    char *r_hex = NULL;
    char *m_hex = NULL;
    uint8_t *r_wire = NULL;
    size_t r_wire_len = 0;
    if (ok)
        ok = vcs_package_release_serialize(&r, &r_wire, &r_wire_len) ==
             VCS_PACKAGE_RELEASE_OK;
    if (ok) {
        r_hex = zc4_hex(r_wire, r_wire_len);
        m_hex = zc4_hex(m_wire, m_wire_len);
        ok = r_hex && m_hex;
    }
    if (ok) {
        struct zc4_cmd c;
        zc4_cmd_init(&c);
        (void)json_push_kv_str(&c.input, "datadir", dd);
        (void)json_push_kv_str(&c.input, "release_hex", r_hex);
        (void)json_push_kv_str(&c.input, "manifest_hex", m_hex);
        (void)json_push_kv_str(&c.input, "recipe_hex", g_zc4_recipe_hex);
        (void)json_push_kv_str(&c.input, "dir", pkgdir);
        zcl_native_handle_zcode_package_publish_commit(&c.request, &c.reply);
        ok = c.reply.status == ZCL_COMMAND_STATUS_PASSED;
        zc4_cmd_free(&c);
    }
    if (ok && root_hex_out) {
        static const char hexd[] = "0123456789abcdef";
        for (int i = 0; i < 32; i++) {
            root_hex_out[2 * i]     = hexd[(root[i] >> 4) & 0xf];
            root_hex_out[2 * i + 1] = hexd[root[i] & 0xf];
        }
        root_hex_out[64] = '\0';
    }
    free(r_hex);
    free(m_hex);
    free(r_wire);
    free(m_wire);
    test_rm_rf_recursive(pkgdir);
    return ok;
}

/* ── ZNAM seeding through the canonical model ───────────────────────── */

/* Upsert one name with its ZCODE pointer records (the same rows the
 * on-chain fold writes). NULL/empty optional texts are skipped. */
static bool zc4_seed(const char *dd, const char *name, const char *owner,
                     uint8_t target_type, const char *target_value,
                     const char *kind, const char *pubkey,
                     const char *display, const char *description)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/node.db", dd);
    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    if (!node_db_open(&ndb, path) || !ndb.open)
        return false;
    struct znam_entry e;
    memset(&e, 0, sizeof(e));
    snprintf(e.name, sizeof(e.name), "%s", name);
    snprintf(e.owner_address, sizeof(e.owner_address), "%s", owner);
    e.target_type = target_type;
    snprintf(e.target_value, sizeof(e.target_value), "%s", target_value);
    memset(e.reg_txid, 0x5A, sizeof(e.reg_txid));
    memset(e.last_update_txid, 0x5A, sizeof(e.last_update_txid));
    e.reg_height = 100;
    e.expiry_height = 100 + ZNAM_REGISTRATION_TERM_BLOCKS;
    bool ok = db_znam_save(&ndb, &e);
    if (ok && kind)
        ok = db_znam_text_save(&ndb, name, ZCODE_POINTER_KEY_KIND, kind);
    if (ok && pubkey)
        ok = db_znam_text_save(&ndb, name, ZCODE_POINTER_KEY_PUBKEY, pubkey);
    if (ok && display)
        ok = db_znam_text_save(&ndb, name, ZCODE_POINTER_KEY_DISPLAY,
                               display);
    if (ok && description)
        ok = db_znam_text_save(&ndb, name, ZCODE_POINTER_KEY_DESCRIPTION,
                               description);
    node_db_close(&ndb);
    return ok;
}

static void zc4_show(struct zc4_cmd *c, const char *dd, const char *pubkey)
{
    zc4_cmd_init(c);
    (void)json_push_kv_str(&c->input, "datadir", dd);
    (void)json_push_kv_str(&c->input, "pubkey", pubkey);
    zcl_native_handle_zcode_contributor_show(&c->request, &c->reply);
}

static void zc4_packages(struct zc4_cmd *c, const char *dd,
                         const char *pubkey)
{
    zc4_cmd_init(c);
    (void)json_push_kv_str(&c->input, "datadir", dd);
    (void)json_push_kv_str(&c->input, "pubkey", pubkey);
    zcl_native_handle_zcode_contributor_packages(&c->request, &c->reply);
}

static void zc4_resolve(struct zc4_cmd *c, const char *dd, const char *name)
{
    zc4_cmd_init(c);
    (void)json_push_kv_str(&c->input, "datadir", dd);
    (void)json_push_kv_str(&c->input, "name", name);
    zcl_native_handle_zcode_package_resolve(&c->request, &c->reply);
}

/* ── 1: contributor show ────────────────────────────────────────────── */
static int t_show(void)
{
    int failures = 0;
    chain_params_select(CHAIN_MAIN);
    char dd[256];
    test_make_tmpdir(dd, sizeof(dd), "zcode_contributor", "show");
    char pk_a[67];
    char pk_c[67];
    char owner_a[64];
    ZC4_CHECK("show: fixtures",
              zc4_pubkey_hex(0xaa, pk_a) && zc4_pubkey_hex(0xcc, pk_c) &&
              zc4_p2pkh_of_seed(0xaa, owner_a, sizeof(owner_a)));
    ZC4_CHECK("show: two releases commit",
              zc4_commit_one(dd, 0xaa, 1u, "rhett/ring-buffer", "MIT", 1,
                             NULL, NULL) &&
              zc4_commit_one(dd, 0xaa, 2u, "rhett/json-lite", "Apache-2.0",
                             2, "jsonlite", NULL));
    ZC4_CHECK("show: publisher profile seeded (bound)",
              zc4_seed(dd, "rhett", owner_a, ZNAM_TYPE_ONION,
                       "rhettzcode.onion", ZCODE_POINTER_KIND_PUBLISHER,
                       pk_a, "Rhett", NULL));

    struct zc4_cmd c;
    zc4_show(&c, dd, pk_a);
    ZC4_CHECK("show: passes with release facts",
              c.reply.status == ZCL_COMMAND_STATUS_PASSED &&
              json_get_bool(json_get(&c.reply.data, "has_published")) &&
              json_get_int(json_get(&c.reply.data, "release_count")) == 2 &&
              json_get_int(json_get(&c.reply.data, "latest_sequence")) == 2);
    const char *latest = json_get_str(json_get(&c.reply.data, "latest_name"));
    const char *rzp =
        json_get_str(json_get(&c.reply.data, "release_znam_pointer"));
    ZC4_CHECK("show: latest release + its znam pointer",
              latest && strcmp(latest, "rhett/json-lite") == 0 &&
              rzp && strcmp(rzp, "jsonlite") == 0);
    const struct json_value *zp = json_get(&c.reply.data, "znam_profile");
    const struct json_value *binding = zp ? json_get(zp, "binding") : NULL;
    const char *zp_name = zp ? json_get_str(json_get(zp, "name")) : NULL;
    const char *zp_display =
        zp ? json_get_str(json_get(zp, "display_name")) : NULL;
    const char *b_owner =
        binding ? json_get_str(json_get(binding, "owner_address")) : NULL;
    const char *b_expected =
        binding ? json_get_str(json_get(binding, "expected_owner")) : NULL;
    ZC4_CHECK("show: bound profile with binding proof",
              zp && json_get_bool(json_get(zp, "found")) &&
              json_get_int(json_get(zp, "claimant_names")) == 1 &&
              zp_name && strcmp(zp_name, "rhett") == 0 &&
              zp_display && strcmp(zp_display, "Rhett") == 0 &&
              binding && json_get_bool(json_get(binding, "bound")) &&
              b_owner && b_expected && strcmp(b_owner, b_expected) == 0 &&
              strcmp(b_owner, owner_a) == 0);
    const struct json_value *roots = json_get(&c.reply.data, "package_roots");
    ZC4_CHECK("show: package roots page",
              roots && json_size(roots) == 2 &&
              !json_get_bool(
                  json_get(&c.reply.data, "package_roots_truncated")));
    zc4_cmd_free(&c);

    zc4_show(&c, dd, pk_c);
    const struct json_value *zp2 = json_get(&c.reply.data, "znam_profile");
    ZC4_CHECK("show: unknown key has neither releases nor profile",
              c.reply.status == ZCL_COMMAND_STATUS_PASSED &&
              !json_get_bool(json_get(&c.reply.data, "has_published")) &&
              zp2 && !json_get_bool(json_get(zp2, "found")));
    zc4_cmd_free(&c);

    zc4_show(&c, dd, "zz");
    ZC4_CHECK("show: malformed pubkey rejected",
              c.reply.status == ZCL_COMMAND_STATUS_FAILED &&
              strcmp(c.reply.error.code, "BAD_PUBKEY") == 0);
    zc4_cmd_free(&c);

    test_rm_rf_recursive(dd);
    return failures;
}

/* ── 2: contributor packages ────────────────────────────────────────── */
static int t_packages(void)
{
    int failures = 0;
    chain_params_select(CHAIN_MAIN);
    char dd[256];
    test_make_tmpdir(dd, sizeof(dd), "zcode_contributor", "packages");
    char pk_a[67];
    char pk_c[67];
    ZC4_CHECK("packages: fixtures",
              zc4_pubkey_hex(0xaa, pk_a) && zc4_pubkey_hex(0xcc, pk_c));
    ZC4_CHECK("packages: three releases commit (two publishers)",
              zc4_commit_one(dd, 0xaa, 1u, "rhett/alpha", "MIT", 11, NULL,
                             NULL) &&
              zc4_commit_one(dd, 0xaa, 2u, "rhett/beta", "ISC", 12, NULL,
                             NULL) &&
              zc4_commit_one(dd, 0xbb, 1u, "bob/gamma", "Zlib", 13, NULL,
                             NULL));

    struct zc4_cmd c;
    zc4_packages(&c, dd, pk_a);
    const struct json_value *rows = json_get(&c.reply.data, "packages");
    ZC4_CHECK("packages: publisher rows bounded and complete",
              c.reply.status == ZCL_COMMAND_STATUS_PASSED &&
              json_get_int(json_get(&c.reply.data, "total_matches")) == 2 &&
              json_get_int(json_get(&c.reply.data, "rendered")) == 2 &&
              !json_get_bool(json_get(&c.reply.data, "items_truncated")) &&
              rows && json_size(rows) == 2);
    const struct json_value *row0 = rows ? json_at(rows, 0) : NULL;
    ZC4_CHECK("packages: row carries identity facts",
              row0 && json_get_str(json_get(row0, "name")) &&
              json_get_str(json_get(row0, "package_root")) &&
              json_get_str(json_get(row0, "release_id")) &&
              json_get_str(json_get(row0, "license")));
    zc4_cmd_free(&c);

    zc4_packages(&c, dd, pk_c);
    ZC4_CHECK("packages: unknown publisher is empty",
              c.reply.status == ZCL_COMMAND_STATUS_PASSED &&
              json_get_int(json_get(&c.reply.data, "total_matches")) == 0);
    zc4_cmd_free(&c);

    zc4_packages(&c, dd, "nope");
    ZC4_CHECK("packages: malformed pubkey rejected",
              c.reply.status == ZCL_COMMAND_STATUS_FAILED &&
              strcmp(c.reply.error.code, "BAD_PUBKEY") == 0);
    zc4_cmd_free(&c);

    test_rm_rf_recursive(dd);
    return failures;
}

/* ── 3: package resolve + rejections ────────────────────────────────── */
static int t_resolve(void)
{
    int failures = 0;
    chain_params_select(CHAIN_MAIN);
    char dd[256];
    test_make_tmpdir(dd, sizeof(dd), "zcode_contributor", "resolve");
    char pk_a[67];
    char owner_a[64];
    char owner_b[64];
    char root1[65];
    ZC4_CHECK("resolve: fixtures",
              zc4_pubkey_hex(0xaa, pk_a) &&
              zc4_p2pkh_of_seed(0xaa, owner_a, sizeof(owner_a)) &&
              zc4_p2pkh_of_seed(0xbb, owner_b, sizeof(owner_b)));
    ZC4_CHECK("resolve: package commits",
              zc4_commit_one(dd, 0xaa, 1u, "rhett/ring-buffer", "MIT", 21,
                             NULL, root1));
    ZC4_CHECK("resolve: bound package pointer seeded",
              zc4_seed(dd, "ringbuffer", owner_a, ZNAM_TYPE_CONTENT, root1,
                       ZCODE_POINTER_KIND_PACKAGE, pk_a, NULL,
                       "a ring buffer"));

    struct zc4_cmd c;
    zc4_resolve(&c, dd, "ringbuffer");
    const struct json_value *zp = json_get(&c.reply.data, "pointer");
    const struct json_value *zi = json_get(&c.reply.data, "identity");
    const struct json_value *binding = zp ? json_get(zp, "binding") : NULL;
    const char *claimed =
        zp ? json_get_str(json_get(zp, "claimed_package_root")) : NULL;
    const char *id_pub =
        zi ? json_get_str(json_get(zi, "publisher")) : NULL;
    const char *id_root =
        zi ? json_get_str(json_get(zi, "package_root")) : NULL;
    const char *id_src = zi ? json_get_str(json_get(zi, "source")) : NULL;
    const char *p_src = zp ? json_get_str(json_get(zp, "source")) : NULL;
    ZC4_CHECK("resolve: pointer and identity separated",
              c.reply.status == ZCL_COMMAND_STATUS_PASSED && zp && zi &&
              p_src && strcmp(p_src, "znam-record") == 0 &&
              id_src && strcmp(id_src, "signed-release") == 0 &&
              json_get_str(json_get(&c.reply.data, "identity_note")));
    ZC4_CHECK("resolve: binding proof and root match",
              binding && json_get_bool(json_get(binding, "bound")) &&
              claimed && strcmp(claimed, root1) == 0 &&
              id_pub && strcmp(id_pub, pk_a) == 0 &&
              id_root && strcmp(id_root, root1) == 0 &&
              json_get_bool(json_get(zi, "matches_pointer_root")) &&
              json_get_bool(json_get(zi, "matches_pointer_publisher")));
    zc4_cmd_free(&c);

    zc4_resolve(&c, dd, "nosuch");
    ZC4_CHECK("resolve: unknown name rejected",
              c.reply.status == ZCL_COMMAND_STATUS_FAILED &&
              strcmp(c.reply.error.code, "UNKNOWN_NAME") == 0);
    zc4_cmd_free(&c);

    ZC4_CHECK("resolve: plain name seeded",
              zc4_seed(dd, "plain", owner_a, ZNAM_TYPE_ONION, "plain.onion",
                       NULL, NULL, NULL, NULL));
    zc4_resolve(&c, dd, "plain");
    ZC4_CHECK("resolve: ordinary name is not a zcode pointer",
              c.reply.status == ZCL_COMMAND_STATUS_FAILED &&
              strcmp(c.reply.error.code, "NOT_A_ZCODE_POINTER") == 0);
    zc4_cmd_free(&c);

    ZC4_CHECK("resolve: publisher profile seeded",
              zc4_seed(dd, "rhett", owner_a, ZNAM_TYPE_ONION,
                       "rhettzcode.onion", ZCODE_POINTER_KIND_PUBLISHER,
                       pk_a, NULL, NULL));
    zc4_resolve(&c, dd, "rhett");
    ZC4_CHECK("resolve: publisher profile is the wrong kind",
              c.reply.status == ZCL_COMMAND_STATUS_FAILED &&
              strcmp(c.reply.error.code, "WRONG_POINTER_KIND") == 0);
    zc4_cmd_free(&c);

    ZC4_CHECK("resolve: wrong-key pointer seeded",
              zc4_seed(dd, "fake", owner_b, ZNAM_TYPE_CONTENT, root1,
                       ZCODE_POINTER_KIND_PACKAGE, pk_a, NULL, NULL));
    zc4_resolve(&c, dd, "fake");
    ZC4_CHECK("resolve: unbound pointer refused",
              c.reply.status == ZCL_COMMAND_STATUS_FAILED &&
              strcmp(c.reply.error.code, "POINTER_NOT_BOUND") == 0);
    zc4_cmd_free(&c);

    char ghost[65];
    memset(ghost, '9', 64);
    ghost[64] = '\0';
    ZC4_CHECK("resolve: dangling pointer seeded",
              zc4_seed(dd, "ghost", owner_a, ZNAM_TYPE_CONTENT, ghost,
                       ZCODE_POINTER_KIND_PACKAGE, pk_a, NULL, NULL));
    zc4_resolve(&c, dd, "ghost");
    ZC4_CHECK("resolve: pointer to a release we do not host",
              c.reply.status == ZCL_COMMAND_STATUS_FAILED &&
              strcmp(c.reply.error.code, "RELEASE_NOT_HOSTED") == 0);
    zc4_cmd_free(&c);

    test_rm_rf_recursive(dd);
    return failures;
}

/* ── 4: pointer move leaves the signed releases untouched ───────────── */
static int t_pointer_move(void)
{
    int failures = 0;
    chain_params_select(CHAIN_MAIN);
    char dd[256];
    test_make_tmpdir(dd, sizeof(dd), "zcode_contributor", "move");
    char pk_a[67];
    char owner_a[64];
    char root1[65];
    char root2[65];
    ZC4_CHECK("move: fixtures",
              zc4_pubkey_hex(0xaa, pk_a) &&
              zc4_p2pkh_of_seed(0xaa, owner_a, sizeof(owner_a)));
    ZC4_CHECK("move: two sequences of one package commit",
              zc4_commit_one(dd, 0xaa, 1u, "rhett/ring-buffer", "MIT", 31,
                             NULL, root1) &&
              zc4_commit_one(dd, 0xaa, 2u, "rhett/ring-buffer", "MIT", 32,
                             NULL, root2));
    ZC4_CHECK("move: pointer seeded at the first root",
              zc4_seed(dd, "ringbuffer", owner_a, ZNAM_TYPE_CONTENT, root1,
                       ZCODE_POINTER_KIND_PACKAGE, pk_a, NULL, NULL));

    struct zc4_cmd c;
    zc4_resolve(&c, dd, "ringbuffer");
    const struct json_value *zi = json_get(&c.reply.data, "identity");
    const char *got1 = zi ? json_get_str(json_get(zi, "package_root")) : NULL;
    ZC4_CHECK("move: resolves to the first root",
              c.reply.status == ZCL_COMMAND_STATUS_PASSED && got1 &&
              strcmp(got1, root1) == 0);
    zc4_cmd_free(&c);

    /* Move the pointer (the on-chain UPDATE upsert) to the second root. */
    ZC4_CHECK("move: pointer re-pointed at the second root",
              zc4_seed(dd, "ringbuffer", owner_a, ZNAM_TYPE_CONTENT, root2,
                       ZCODE_POINTER_KIND_PACKAGE, pk_a, NULL, NULL));
    zc4_resolve(&c, dd, "ringbuffer");
    zi = json_get(&c.reply.data, "identity");
    const char *got2 = zi ? json_get_str(json_get(zi, "package_root")) : NULL;
    ZC4_CHECK("move: resolves to the second root",
              c.reply.status == ZCL_COMMAND_STATUS_PASSED && got2 &&
              strcmp(got2, root2) == 0);
    zc4_cmd_free(&c);

    /* The old release is still hosted and unchanged — identity lives in
     * the signed envelope, not in the pointer. */
    zc4_cmd_init(&c);
    (void)json_push_kv_str(&c.input, "datadir", dd);
    (void)json_push_kv_str(&c.input, "root", root1);
    zcl_native_handle_zcode_package_show(&c.request, &c.reply);
    const struct json_value *rel = json_get(&c.reply.data, "release");
    const char *rel_pub =
        rel ? json_get_str(json_get(rel, "publisher")) : NULL;
    ZC4_CHECK("move: old release still shows by root",
              c.reply.status == ZCL_COMMAND_STATUS_PASSED && rel &&
              rel_pub && strcmp(rel_pub, pk_a) == 0);
    zc4_cmd_free(&c);

    test_rm_rf_recursive(dd);
    return failures;
}

/* ── 5: impersonation — a bound pointer at someone else's root ──────── */
static int t_impersonation(void)
{
    int failures = 0;
    chain_params_select(CHAIN_MAIN);
    char dd[256];
    test_make_tmpdir(dd, sizeof(dd), "zcode_contributor", "impersonate");
    char pk_a[67];
    char pk_b[67];
    char owner_b[64];
    char root1[65];
    ZC4_CHECK("impersonation: fixtures",
              zc4_pubkey_hex(0xaa, pk_a) && zc4_pubkey_hex(0xbb, pk_b) &&
              zc4_p2pkh_of_seed(0xbb, owner_b, sizeof(owner_b)));
    ZC4_CHECK("impersonation: victim package commits",
              zc4_commit_one(dd, 0xaa, 1u, "rhett/ring-buffer", "MIT", 41,
                             NULL, root1));
    /* The attacker binds their own name (owner == their key) but points it
     * at the victim's package root. The binding is honestly the attacker's;
     * the identity must still read the VICTIM's signed release. */
    ZC4_CHECK("impersonation: attacker pointer seeded",
              zc4_seed(dd, "copycat", owner_b, ZNAM_TYPE_CONTENT, root1,
                       ZCODE_POINTER_KIND_PACKAGE, pk_b, NULL,
                       "totally the real ring buffer"));

    struct zc4_cmd c;
    zc4_resolve(&c, dd, "copycat");
    const struct json_value *zp = json_get(&c.reply.data, "pointer");
    const struct json_value *zi = json_get(&c.reply.data, "identity");
    const struct json_value *binding = zp ? json_get(zp, "binding") : NULL;
    const char *id_pub =
        zi ? json_get_str(json_get(zi, "publisher")) : NULL;
    ZC4_CHECK("impersonation: identity stays the signed release's",
              c.reply.status == ZCL_COMMAND_STATUS_PASSED && binding &&
              json_get_bool(json_get(binding, "bound")) &&
              id_pub && strcmp(id_pub, pk_a) == 0 &&
              !json_get_bool(json_get(zi, "matches_pointer_publisher")) &&
              json_get_bool(json_get(zi, "matches_pointer_root")));
    zc4_cmd_free(&c);

    zc4_show(&c, dd, pk_a);
    const struct json_value *prof = json_get(&c.reply.data, "znam_profile");
    ZC4_CHECK("impersonation: victim profile untouched by the claim",
              c.reply.status == ZCL_COMMAND_STATUS_PASSED &&
              json_get_bool(json_get(&c.reply.data, "has_published")) &&
              prof && !json_get_bool(json_get(prof, "found")));
    zc4_cmd_free(&c);

    test_rm_rf_recursive(dd);
    return failures;
}

/* ── 6: rebuild equivalence (reopen node.db + rebuild index) ────────── */
static int t_rebuild_equivalence(void)
{
    int failures = 0;
    chain_params_select(CHAIN_MAIN);
    char dd[256];
    test_make_tmpdir(dd, sizeof(dd), "zcode_contributor", "rebuild");
    char pk_a[67];
    char owner_a[64];
    ZC4_CHECK("rebuild: fixtures",
              zc4_pubkey_hex(0xaa, pk_a) &&
              zc4_p2pkh_of_seed(0xaa, owner_a, sizeof(owner_a)));
    ZC4_CHECK("rebuild: package commits and profile seeded",
              zc4_commit_one(dd, 0xaa, 1u, "rhett/ring-buffer", "MIT", 51,
                             NULL, NULL) &&
              zc4_seed(dd, "rhett", owner_a, ZNAM_TYPE_ONION,
                       "rhettzcode.onion", ZCODE_POINTER_KIND_PUBLISHER,
                       pk_a, "Rhett", NULL));

    /* Every handler run rebuilds the index from the CAS and reopens
     * node.db — two runs must agree field for field. */
    struct zc4_cmd a;
    struct zc4_cmd b;
    zc4_show(&a, dd, pk_a);
    zc4_show(&b, dd, pk_a);
    const struct json_value *pa = json_get(&a.reply.data, "znam_profile");
    const struct json_value *pb = json_get(&b.reply.data, "znam_profile");
    const char *na = pa ? json_get_str(json_get(pa, "name")) : NULL;
    const char *nb = pb ? json_get_str(json_get(pb, "name")) : NULL;
    const char *la = json_get_str(json_get(&a.reply.data, "latest_name"));
    const char *lb = json_get_str(json_get(&b.reply.data, "latest_name"));
    const char *ia = json_get_str(json_get(&a.reply.data, "latest_release_id"));
    const char *ib = json_get_str(json_get(&b.reply.data, "latest_release_id"));
    ZC4_CHECK("rebuild: profile output identical across rebuilds",
              a.reply.status == ZCL_COMMAND_STATUS_PASSED &&
              b.reply.status == ZCL_COMMAND_STATUS_PASSED &&
              json_get_int(json_get(&a.reply.data, "release_count")) ==
                  json_get_int(json_get(&b.reply.data, "release_count")) &&
              la && lb && strcmp(la, lb) == 0 &&
              ia && ib && strcmp(ia, ib) == 0 &&
              na && nb && strcmp(na, nb) == 0);
    zc4_cmd_free(&a);
    zc4_cmd_free(&b);

    test_rm_rf_recursive(dd);
    return failures;
}

int test_zcode_contributor(void)
{
    printf("\n=== zcode_contributor: identity + ZNAM pointers ===\n");
    int failures = 0;
    failures += t_show();
    failures += t_packages();
    failures += t_resolve();
    failures += t_pointer_move();
    failures += t_impersonation();
    failures += t_rebuild_equivalence();
    printf("=== zcode_contributor complete: %d failure(s) ===\n", failures);
    return failures;
}
