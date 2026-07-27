/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_zcode_verify — the external-verifier attestation gate (slice 6:
 * lib/vcs/package_attest.*, lib/vcs/package_verify_policy.*, the
 * zcode package verify handler in tools/command/native_zcode_command.c,
 * and the build/bin/zclassic23-package-verify external program).
 *
 * Coverage:
 *   1. Attestation codec: roundtrip for every result class, a frozen KAT
 *      attestation id (canonical-encoding drift guard), every bound and
 *      consistency rule (closed grammar: magic/version/truncated/trailing/
 *      oversize wires, order rules, detail/class/test/sanitizer
 *      consistency), invalid signature, high-S malleation, wrong key.
 *   2. Approved-verifier policy: allowlist text parsing (comments, blanks,
 *      bad lines with line numbers, duplicates, the 64-key bound,
 *      off-curve keys) and the quorum rule — 2-of-N approved matching
 *      verifies; unapproved keys, self-verification, duplicate signers,
 *      and non-matching attestations are all named and never counted;
 *      a quorum on a FAIL class is not "verified".
 *   3. zcode package verify: verified quorum report over a fixture store,
 *      NO_APPROVED_VERIFIERS without the local allowlist, UNKNOWN_PACKAGE,
 *      BAD_ROOT, and the not-quite-quorum states.
 *   4. End-to-end external verifier: a tiny real C package is published
 *      into a fixture store, build/bin/zclassic23-package-verify runs it
 *      (gcc + clang, plain + ASan/UBSan), and the signed attestation is
 *      checked (test-pass, verifier key, temp tree cleaned). Hostile
 *      fixtures fail closed with the named rule: a syntax-error source
 *      (build-fail/compile-error) and a test that calls socket()
 *      (test-fail/test-signal — the seccomp network denial firing).
 *
 * Command handlers run in-process on ./test-tmp datadirs. The e2e lane
 * forks the real verifier binary — it MUST exist (make
 * zclassic23-package-verify); a missing binary is a loud failure, never
 * a silent skip. */

#include "test/test_core.h"

#include "command/native_command.h"

#include "core/uint256.h"
#include "crypto/sha3.h"
#include "json/json.h"
#include "keys/key.h"
#include "keys/pubkey.h"
#include "util/spawn.h"
#include "vcs/package_attest.h"
#include "vcs/package_manifest.h"
#include "vcs/package_recipe.h"
#include "vcs/package_release.h"
#include "vcs/package_verify_policy.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ZV_CHECK(name, expr) do {                                     \
    if (expr) { printf("  zcode_verify: %s... OK\n", (name)); }       \
    else { printf("  zcode_verify: %s... FAIL\n", (name)); failures++; } \
} while (0)

#define ZV_VERIFIER_BIN "build/bin/zclassic23-package-verify"

/* ── small fixtures ─────────────────────────────────────────────────── */

static void zv_hex_enc(const uint8_t *in, size_t len, char *out)
{
    static const char hexd[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i]     = hexd[(in[i] >> 4) & 0xf];
        out[2 * i + 1] = hexd[in[i] & 0xf];
    }
    out[2 * len] = '\0';
}

static bool zv_keypair(uint8_t seed, struct privkey *sk, struct pubkey *pk)
{
    memset(sk->vch, seed, 32);
    sk->fValid = true;
    sk->fCompressed = true;
    return privkey_get_pubkey(sk, pk) &&
           pk->size == COMPRESSED_PUBLIC_KEY_SIZE;
}

static bool zv_pubkey_hex(uint8_t seed, char out[67])
{
    struct privkey sk;
    struct pubkey pk;
    if (!zv_keypair(seed, &sk, &pk))
        return false;
    zv_hex_enc(pk.vch, pk.size, out);
    return true;
}

static bool zv_mkdir_p(const char *path)
{
    char buf[4096];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buf))
        return false;
    memcpy(buf, path, len + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buf, 0700) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    return mkdir(buf, 0700) == 0 || errno == EEXIST;
}

static bool zv_rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0;
    DIR *dir = opendir(path);
    if (!dir)
        return false;
    bool ok = true;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char child[4096];
        int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (n <= 0 || (size_t)n >= sizeof(child)) {
            ok = false;
            continue;
        }
        if (!zv_rm_rf(child))
            ok = false;
    }
    closedir(dir);
    if (rmdir(path) != 0)
        ok = false;
    return ok;
}

static bool zv_write_file(const char *path, const void *data, size_t len,
                          mode_t mode)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    size_t written = fwrite(data, 1, len, f);
    if (fclose(f) != 0 || written != len)
        return false;
    return chmod(path, mode) == 0;
}

static bool zv_read_file(const char *path, uint8_t *out, size_t cap,
                         size_t *out_len)
{
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    size_t len = fread(out, 1, cap, f);
    bool ok = !ferror(f) && feof(f) && len > 0;
    fclose(f);
    if (!ok)
        return false;
    *out_len = len;
    return true;
}

/* ── attestation fixtures ───────────────────────────────────────────── */

static bool zv_sign_attest(struct vcs_package_attest *a, struct privkey *sk)
{
    uint8_t id[VCS_PACKAGE_ATTEST_ID_BYTES];
    if (vcs_package_attest_id(a, id) != VCS_PACKAGE_ATTEST_OK)
        return false;
    struct uint256 hash;
    memcpy(hash.data, id, 32);
    unsigned char compact[COMPACT_SIGNATURE_SIZE];
    if (!privkey_sign_compact(sk, &hash, compact))
        return false;
    memcpy(a->signature, compact + 1, VCS_PACKAGE_ATTEST_SIGNATURE_BYTES);
    return true;
}

/* A valid, signed attestation of the given class over the given roots,
 * signed by the given key. Field sets mirror what the external verifier
 * produces (clang + gcc, ASan + UBSan where tests run). */
static bool zv_attest(struct vcs_package_attest *a, uint8_t cls,
                      const uint8_t package_root[32],
                      const uint8_t release_id[32],
                      const uint8_t recipe_root[32],
                      uint8_t signer_seed)
{
    struct privkey sk;
    struct pubkey pk;
    if (!zv_keypair(signer_seed, &sk, &pk))
        return false;
    memset(a, 0, sizeof(*a));
    a->schema_version = VCS_PACKAGE_ATTEST_VERSION;
    memcpy(a->package_root, package_root, 32);
    memcpy(a->release_id, release_id, 32);
    memcpy(a->recipe_root, recipe_root, 32);
    a->result_class = cls;
    snprintf(a->compilers[0].id, sizeof(a->compilers[0].id), "clang");
    snprintf(a->compilers[0].version, sizeof(a->compilers[0].version),
             "18.1.3");
    snprintf(a->compilers[1].id, sizeof(a->compilers[1].id), "gcc");
    snprintf(a->compilers[1].version, sizeof(a->compilers[1].version),
             "13.2.0");
    a->compiler_count = 2;
    a->compilers[0].outcome = VCS_PACKAGE_ATTEST_OUTCOME_PASS;
    a->compilers[1].outcome = VCS_PACKAGE_ATTEST_OUTCOME_PASS;
    a->isolation = VCS_PACKAGE_ATTEST_ISOLATION_FULL;
    switch (cls) {
    case VCS_PACKAGE_ATTEST_RESULT_BUILD_PASS:
        a->test_ran = false;
        break;
    case VCS_PACKAGE_ATTEST_RESULT_BUILD_FAIL:
        a->test_ran = false;
        a->detail_code = VCS_PACKAGE_ATTEST_DETAIL_COMPILE_ERROR;
        snprintf(a->detail, sizeof(a->detail),
                 "gcc: src/x.c:4:5: error: expected expression");
        a->compilers[0].outcome = VCS_PACKAGE_ATTEST_OUTCOME_FAIL;
        break;
    case VCS_PACKAGE_ATTEST_RESULT_TEST_PASS:
        a->test_ran = true;
        a->test_exit_code = 0;
        snprintf(a->sanitizers[0].name, sizeof(a->sanitizers[0].name),
                 "asan");
        snprintf(a->sanitizers[1].name, sizeof(a->sanitizers[1].name),
                 "ubsan");
        a->sanitizer_count = 2;
        a->sanitizers[0].outcome = VCS_PACKAGE_ATTEST_OUTCOME_PASS;
        a->sanitizers[1].outcome = VCS_PACKAGE_ATTEST_OUTCOME_PASS;
        break;
    case VCS_PACKAGE_ATTEST_RESULT_TEST_FAIL:
        a->test_ran = true;
        a->test_exit_code = 1;
        a->detail_code = VCS_PACKAGE_ATTEST_DETAIL_TEST_EXIT_MISMATCH;
        snprintf(a->detail, sizeof(a->detail), "gcc: exit 1, expected 0");
        snprintf(a->sanitizers[0].name, sizeof(a->sanitizers[0].name),
                 "asan");
        snprintf(a->sanitizers[1].name, sizeof(a->sanitizers[1].name),
                 "ubsan");
        a->sanitizer_count = 2;
        a->sanitizers[0].outcome = VCS_PACKAGE_ATTEST_OUTCOME_PASS;
        a->sanitizers[1].outcome = VCS_PACKAGE_ATTEST_OUTCOME_PASS;
        break;
    case VCS_PACKAGE_ATTEST_RESULT_SANITIZER_FAIL:
        a->test_ran = true;
        a->test_exit_code = 99;
        a->detail_code = VCS_PACKAGE_ATTEST_DETAIL_ASAN_FINDINGS;
        snprintf(a->detail, sizeof(a->detail),
                 "gcc+san: exit 99, expected 0");
        snprintf(a->sanitizers[0].name, sizeof(a->sanitizers[0].name),
                 "asan");
        snprintf(a->sanitizers[1].name, sizeof(a->sanitizers[1].name),
                 "ubsan");
        a->sanitizer_count = 2;
        a->sanitizers[0].outcome = VCS_PACKAGE_ATTEST_OUTCOME_FAIL;
        a->sanitizers[1].outcome = VCS_PACKAGE_ATTEST_OUTCOME_PASS;
        break;
    default:
        return false;
    }
    memcpy(a->verifier_pubkey, pk.vch, 33);
    return zv_sign_attest(a, &sk);
}

static void zv_pattern_root(uint8_t seed, uint8_t out[32])
{
    for (size_t i = 0; i < 32; i++)
        out[i] = (uint8_t)(seed + i);
}

/* ── 1. codec ───────────────────────────────────────────────────────── */

static int t_codec(void)
{
    int failures = 0;
    uint8_t pr[32], ri[32], rr[32];
    zv_pattern_root(0x10, pr);
    zv_pattern_root(0x40, ri);
    zv_pattern_root(0x80, rr);

    /* Roundtrip for every result class. */
    static const uint8_t k_classes[] = {
        VCS_PACKAGE_ATTEST_RESULT_BUILD_PASS,
        VCS_PACKAGE_ATTEST_RESULT_BUILD_FAIL,
        VCS_PACKAGE_ATTEST_RESULT_TEST_PASS,
        VCS_PACKAGE_ATTEST_RESULT_TEST_FAIL,
        VCS_PACKAGE_ATTEST_RESULT_SANITIZER_FAIL,
    };
    bool roundtrip = true;
    for (size_t i = 0; i < sizeof(k_classes); i++) {
        struct vcs_package_attest a;
        if (!zv_attest(&a, k_classes[i], pr, ri, rr, 0x42)) {
            roundtrip = false;
            break;
        }
        uint8_t *wire = NULL;
        size_t wire_len = 0;
        if (vcs_package_attest_serialize(&a, &wire, &wire_len) !=
            VCS_PACKAGE_ATTEST_OK) {
            roundtrip = false;
            break;
        }
        struct vcs_package_attest b;
        bool ok = vcs_package_attest_parse(wire, wire_len, &b) ==
                      VCS_PACKAGE_ATTEST_OK &&
                  vcs_package_attest_verify(&b) == VCS_PACKAGE_ATTEST_OK &&
                  b.result_class == a.result_class &&
                  b.detail_code == a.detail_code &&
                  strcmp(b.detail, a.detail) == 0 &&
                  b.compiler_count == a.compiler_count &&
                  b.sanitizer_count == a.sanitizer_count &&
                  b.test_ran == a.test_ran &&
                  b.test_exit_code == a.test_exit_code &&
                  b.isolation == a.isolation &&
                  memcmp(b.verifier_pubkey, a.verifier_pubkey, 33) == 0 &&
                  memcmp(b.signature, a.signature, 64) == 0;
        free(wire);
        if (!ok) {
            roundtrip = false;
            break;
        }
    }
    ZV_CHECK("codec: roundtrip every result class", roundtrip);

    /* KAT: the frozen attestation id guards the canonical encoding. */
    struct vcs_package_attest kat;
    bool kat_ok = zv_attest(&kat, VCS_PACKAGE_ATTEST_RESULT_TEST_PASS, pr,
                            ri, rr, 0x42);
    uint8_t kat_id[32];
    char kat_hex[65];
    if (kat_ok)
        kat_ok = vcs_package_attest_id(&kat, kat_id) ==
                 VCS_PACKAGE_ATTEST_OK;
    if (kat_ok)
        zv_hex_enc(kat_id, 32, kat_hex);
    static const char *k_kat_expect =
        "b67be9be495ffd9f381fd729273c40609e3e92d77233c530fdd8ce018d57da20";
    ZV_CHECK("codec: frozen KAT attestation id",
             kat_ok && strcmp(kat_hex, k_kat_expect) == 0);
    if (kat_ok && strcmp(kat_hex, k_kat_expect) != 0)
        printf("  zcode_verify: KAT actual %s\n", kat_hex);

    /* Hostile wires. */
    struct vcs_package_attest a;
    if (!zv_attest(&a, VCS_PACKAGE_ATTEST_RESULT_TEST_PASS, pr, ri, rr,
                   0x42)) {
        ZV_CHECK("codec: fixture builds", false);
        return failures + 1;
    }
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    if (vcs_package_attest_serialize(&a, &wire, &wire_len) !=
            VCS_PACKAGE_ATTEST_OK) {
        ZV_CHECK("codec: fixture serializes", false);
        return failures + 1;
    }
    struct vcs_package_attest out;
    ZV_CHECK("codec: truncated wire",
             vcs_package_attest_parse(wire, wire_len - 10, &out) ==
                 VCS_PACKAGE_ATTEST_ERR_WIRE_TRUNCATED);
    {
        uint8_t bigger[VCS_PACKAGE_ATTEST_MAX_WIRE_BYTES + 1u];
        memcpy(bigger, wire, wire_len);
        bigger[wire_len] = 0x00;
        ZV_CHECK("codec: trailing byte",
                 vcs_package_attest_parse(bigger, wire_len + 1, &out) ==
                     VCS_PACKAGE_ATTEST_ERR_WIRE_TRAILING);
        ZV_CHECK("codec: oversize wire",
                 vcs_package_attest_parse(bigger, sizeof(bigger), &out) ==
                     VCS_PACKAGE_ATTEST_ERR_WIRE_OVERSIZE);
    }
    {
        uint8_t bad[VCS_PACKAGE_ATTEST_MAX_WIRE_BYTES];
        memcpy(bad, wire, wire_len);
        bad[0] ^= 0x01;
        ZV_CHECK("codec: bad magic",
                 vcs_package_attest_parse(bad, wire_len, &out) ==
                     VCS_PACKAGE_ATTEST_ERR_WIRE_MAGIC);
        memcpy(bad, wire, wire_len);
        bad[8] = 2; /* schema_version low byte */
        bad[9] = 0;
        ZV_CHECK("codec: unknown version",
                 vcs_package_attest_parse(bad, wire_len, &out) ==
                     VCS_PACKAGE_ATTEST_ERR_SCHEMA_VERSION);
        memcpy(bad, wire, wire_len);
        bad[106] = 9; /* result class */
        ZV_CHECK("codec: unknown result class",
                 vcs_package_attest_parse(bad, wire_len, &out) ==
                     VCS_PACKAGE_ATTEST_ERR_RESULT_CLASS);
        memcpy(bad, wire, wire_len);
        bad[107] = 250; /* detail code */
        ZV_CHECK("codec: unknown detail code",
                 vcs_package_attest_parse(bad, wire_len, &out) ==
                     VCS_PACKAGE_ATTEST_ERR_DETAIL_CODE);
    }
    free(wire);

    /* Consistency rules via validate() on mutated structs. */
    struct vcs_package_attest m;
    ZV_CHECK("codec: validate accepts the fixture",
             zv_attest(&m, VCS_PACKAGE_ATTEST_RESULT_TEST_PASS, pr, ri, rr,
                       0x42) &&
             vcs_package_attest_validate(&m) == VCS_PACKAGE_ATTEST_OK);
    zv_attest(&m, VCS_PACKAGE_ATTEST_RESULT_TEST_PASS, pr, ri, rr, 0x42);
    memset(m.package_root, 0, 32);
    ZV_CHECK("codec: zero package root",
             vcs_package_attest_validate(&m) ==
                 VCS_PACKAGE_ATTEST_ERR_PACKAGE_ROOT);
    zv_attest(&m, VCS_PACKAGE_ATTEST_RESULT_TEST_PASS, pr, ri, rr, 0x42);
    m.detail[0] = 0x01;
    ZV_CHECK("codec: non-printable detail",
             vcs_package_attest_validate(&m) ==
                 VCS_PACKAGE_ATTEST_ERR_DETAIL_TEXT);
    zv_attest(&m, VCS_PACKAGE_ATTEST_RESULT_TEST_PASS, pr, ri, rr, 0x42);
    snprintf(m.detail, sizeof(m.detail), "surplus");
    ZV_CHECK("codec: pass class carrying detail",
             vcs_package_attest_validate(&m) ==
                 VCS_PACKAGE_ATTEST_ERR_DETAIL_FORBIDDEN);
    zv_attest(&m, VCS_PACKAGE_ATTEST_RESULT_TEST_FAIL, pr, ri, rr, 0x42);
    m.detail_code = VCS_PACKAGE_ATTEST_DETAIL_NONE;
    ZV_CHECK("codec: fail class without detail code",
             vcs_package_attest_validate(&m) ==
                 VCS_PACKAGE_ATTEST_ERR_DETAIL_REQUIRED);
    zv_attest(&m, VCS_PACKAGE_ATTEST_RESULT_BUILD_PASS, pr, ri, rr, 0x42);
    m.test_ran = true;
    ZV_CHECK("codec: build-pass with test_ran",
             vcs_package_attest_validate(&m) ==
                 VCS_PACKAGE_ATTEST_ERR_TEST_CLASS);
    zv_attest(&m, VCS_PACKAGE_ATTEST_RESULT_BUILD_PASS, pr, ri, rr, 0x42);
    m.test_exit_code = 3;
    ZV_CHECK("codec: non-canonical exit code without a test run",
             vcs_package_attest_validate(&m) ==
                 VCS_PACKAGE_ATTEST_ERR_TEST_EXIT);
    zv_attest(&m, VCS_PACKAGE_ATTEST_RESULT_SANITIZER_FAIL, pr, ri, rr,
              0x42);
    m.sanitizers[0].outcome = VCS_PACKAGE_ATTEST_OUTCOME_PASS;
    ZV_CHECK("codec: sanitizer-fail without findings",
             vcs_package_attest_validate(&m) ==
                 VCS_PACKAGE_ATTEST_ERR_SANITIZER_FINDINGS);
    zv_attest(&m, VCS_PACKAGE_ATTEST_RESULT_TEST_PASS, pr, ri, rr, 0x42);
    m.sanitizers[1].outcome = VCS_PACKAGE_ATTEST_OUTCOME_FAIL;
    ZV_CHECK("codec: findings in a pass class",
             vcs_package_attest_validate(&m) ==
                 VCS_PACKAGE_ATTEST_ERR_SANITIZER_FINDINGS);
    zv_attest(&m, VCS_PACKAGE_ATTEST_RESULT_TEST_PASS, pr, ri, rr, 0x42);
    m.compilers[1].outcome = VCS_PACKAGE_ATTEST_OUTCOME_FAIL;
    ZV_CHECK("codec: failed compiler in a pass class",
             vcs_package_attest_validate(&m) ==
                 VCS_PACKAGE_ATTEST_ERR_OUTCOME_CLASS);
    zv_attest(&m, VCS_PACKAGE_ATTEST_RESULT_TEST_PASS, pr, ri, rr, 0x42);
    {
        struct vcs_package_attest_compiler tmp = m.compilers[0];
        m.compilers[0] = m.compilers[1];
        m.compilers[1] = tmp;
    }
    ZV_CHECK("codec: unsorted compilers",
             vcs_package_attest_validate(&m) ==
                 VCS_PACKAGE_ATTEST_ERR_COMPILER_ORDER);
    zv_attest(&m, VCS_PACKAGE_ATTEST_RESULT_TEST_PASS, pr, ri, rr, 0x42);
    m.isolation = 9;
    ZV_CHECK("codec: unknown isolation level",
             vcs_package_attest_validate(&m) ==
                 VCS_PACKAGE_ATTEST_ERR_ISOLATION);
    return failures;
}

static int t_signature(void)
{
    int failures = 0;
    uint8_t pr[32], ri[32], rr[32];
    zv_pattern_root(0x10, pr);
    zv_pattern_root(0x40, ri);
    zv_pattern_root(0x80, rr);
    struct vcs_package_attest a;
    bool ok = zv_attest(&a, VCS_PACKAGE_ATTEST_RESULT_TEST_PASS, pr, ri,
                        rr, 0x42);
    ZV_CHECK("sign: signed attestation verifies",
             ok && vcs_package_attest_verify(&a) == VCS_PACKAGE_ATTEST_OK);
    struct vcs_package_attest m = a;
    m.signature[7] ^= 0x01;
    ZV_CHECK("sign: flipped signature byte",
             vcs_package_attest_verify(&m) ==
                 VCS_PACKAGE_ATTEST_ERR_SIG_VERIFY);
    m = a;
    memset(m.signature + 32, 0xff, 32); /* s = 2^256-1 > n/2 */
    ZV_CHECK("sign: high-S malleation",
             vcs_package_attest_verify(&m) ==
                 VCS_PACKAGE_ATTEST_ERR_SIG_LOW_S);
    m = a;
    {
        struct pubkey other;
        struct privkey osk;
        zv_keypair(0x99, &osk, &other);
        memcpy(m.verifier_pubkey, other.vch, 33);
    }
    ZV_CHECK("sign: wrong verifier key",
             vcs_package_attest_verify(&m) ==
                 VCS_PACKAGE_ATTEST_ERR_SIG_VERIFY);
    m = a;
    m.test_exit_code ^= 1; /* tamper after signing */
    ZV_CHECK("sign: tampered field",
             vcs_package_attest_verify(&m) ==
                 VCS_PACKAGE_ATTEST_ERR_SIG_VERIFY);
    return failures;
}

/* ── 2. policy + quorum ─────────────────────────────────────────────── */

static int t_policy(void)
{
    int failures = 0;
    char ka[67], kb[67];
    zv_pubkey_hex(0x22, ka);
    zv_pubkey_hex(0x33, kb);

    struct vcs_verifier_policy p;
    vcs_verifier_policy_init(&p);
    char text[256];
    snprintf(text, sizeof(text), "# approved verifiers\n\n%s\n%s\n", ka,
             kb);
    enum vcs_verifier_policy_error err = VCS_VERIFIER_POLICY_OK;
    ZV_CHECK("policy: comments and blanks parse",
             vcs_verifier_policy_parse_text(&p, text, strlen(text), &err,
                                            NULL) &&
             p.count == 2);

    vcs_verifier_policy_init(&p);
    size_t line = 0;
    ZV_CHECK("policy: malformed line names the rule and line",
             !vcs_verifier_policy_parse_text(&p, "zz\n", 3, &err, &line) &&
             err == VCS_VERIFIER_POLICY_ERR_KEY_GRAMMAR && line == 1);

    vcs_verifier_policy_init(&p);
    snprintf(text, sizeof(text), "%s\n%s\n", ka, ka);
    ZV_CHECK("policy: duplicate key rejected at its line",
             !vcs_verifier_policy_parse_text(&p, text, strlen(text), &err,
                                             &line) &&
             err == VCS_VERIFIER_POLICY_ERR_DUPLICATE && line == 2);

    vcs_verifier_policy_init(&p);
    {
        char offcurve[67];
        snprintf(offcurve, sizeof(offcurve),
                 "0200000000000000000000000000000000000000000000000000000000"
                 "00000000");
        ZV_CHECK("policy: off-curve key rejected",
                 !vcs_verifier_policy_parse_text(&p, offcurve,
                                                 strlen(offcurve), &err,
                                                 NULL) &&
                 err == VCS_VERIFIER_POLICY_ERR_KEY_OFFCURVE);
    }

    /* The 64-key bound. */
    vcs_verifier_policy_init(&p);
    bool bound_ok = true;
    for (size_t i = 0; i < VCS_VERIFIER_POLICY_MAX_KEYS; i++) {
        uint8_t key[33];
        struct privkey sk;
        struct pubkey pk;
        if (!zv_keypair((uint8_t)(0x30 + i), &sk, &pk)) {
            bound_ok = false;
            break;
        }
        memcpy(key, pk.vch, 33);
        if (!vcs_verifier_policy_add(&p, key, &err)) {
            bound_ok = false;
            break;
        }
    }
    if (bound_ok) {
        uint8_t extra[33];
        struct privkey sk;
        struct pubkey pk;
        zv_keypair(0x01, &sk, &pk);
        memcpy(extra, pk.vch, 33);
        bound_ok = !vcs_verifier_policy_add(&p, extra, &err) &&
                   err == VCS_VERIFIER_POLICY_ERR_TOO_MANY;
    }
    ZV_CHECK("policy: 64-key bound", bound_ok);
    return failures;
}

struct zv_quorum_ctx {
    uint8_t package_root[32];
    uint8_t release_id[32];
    uint8_t recipe_root[32];
    uint8_t publisher[33];
    struct vcs_verifier_policy policy;
};

static void zv_quorum_ctx_init(struct zv_quorum_ctx *ctx)
{
    zv_pattern_root(0x10, ctx->package_root);
    zv_pattern_root(0x40, ctx->release_id);
    zv_pattern_root(0x80, ctx->recipe_root);
    struct privkey sk;
    struct pubkey pk;
    zv_keypair(0x11, &sk, &pk);
    memcpy(ctx->publisher, pk.vch, 33);
    vcs_verifier_policy_init(&ctx->policy);
    for (uint8_t seed = 0x22; seed <= 0x25; seed++) {
        zv_keypair(seed, &sk, &pk);
        vcs_verifier_policy_add(&ctx->policy, pk.vch, NULL);
    }
}

static bool zv_row_rule_is(const struct vcs_verify_quorum *q, size_t i,
                           enum vcs_verify_row_rule rule)
{
    return i < q->row_count && q->rows[i].rule == rule;
}

static int t_quorum(void)
{
    int failures = 0;
    struct zv_quorum_ctx ctx;
    zv_quorum_ctx_init(&ctx);

    /* 2-of-N approved matching -> verified. */
    {
        struct vcs_verify_candidate cands[2];
        cands[0].parsed = zv_attest(&cands[0].attestation,
                                    VCS_PACKAGE_ATTEST_RESULT_TEST_PASS,
                                    ctx.package_root, ctx.release_id,
                                    ctx.recipe_root, 0x22);
        cands[1].parsed = zv_attest(&cands[1].attestation,
                                    VCS_PACKAGE_ATTEST_RESULT_TEST_PASS,
                                    ctx.package_root, ctx.release_id,
                                    ctx.recipe_root, 0x23);
        struct vcs_verify_quorum q;
        vcs_verify_evaluate(cands, 2, ctx.package_root, ctx.recipe_root,
                            ctx.publisher, &ctx.policy, &q);
        ZV_CHECK("quorum: two approved matching verify",
                 cands[0].parsed && cands[1].parsed && q.verified &&
                 q.quorum_reached && q.quorum_signers == 2 &&
                 q.quorum_class == VCS_PACKAGE_ATTEST_RESULT_TEST_PASS &&
                 zv_row_rule_is(&q, 0, VCS_VERIFY_ROW_COUNTED) &&
                 zv_row_rule_is(&q, 1, VCS_VERIFY_ROW_COUNTED));
    }

    /* Three matching signers count three. */
    {
        struct vcs_verify_candidate cands[3];
        for (size_t i = 0; i < 3; i++)
            cands[i].parsed = zv_attest(
                &cands[i].attestation, VCS_PACKAGE_ATTEST_RESULT_TEST_PASS,
                ctx.package_root, ctx.release_id, ctx.recipe_root,
                (uint8_t)(0x22 + i));
        struct vcs_verify_quorum q;
        vcs_verify_evaluate(cands, 3, ctx.package_root, ctx.recipe_root,
                            ctx.publisher, &ctx.policy, &q);
        ZV_CHECK("quorum: 3-of-N counts three signers",
                 q.verified && q.quorum_signers == 3);
    }

    /* Non-matching classes never reach a quorum. */
    {
        struct vcs_verify_candidate cands[2];
        cands[0].parsed = zv_attest(&cands[0].attestation,
                                    VCS_PACKAGE_ATTEST_RESULT_TEST_PASS,
                                    ctx.package_root, ctx.release_id,
                                    ctx.recipe_root, 0x22);
        cands[1].parsed = zv_attest(&cands[1].attestation,
                                    VCS_PACKAGE_ATTEST_RESULT_TEST_FAIL,
                                    ctx.package_root, ctx.release_id,
                                    ctx.recipe_root, 0x23);
        struct vcs_verify_quorum q;
        vcs_verify_evaluate(cands, 2, ctx.package_root, ctx.recipe_root,
                            ctx.publisher, &ctx.policy, &q);
        ZV_CHECK("quorum: different result classes do not verify",
                 !q.verified && !q.quorum_reached &&
                 q.counted == 2);
    }

    /* An unapproved key is named and never counted. */
    {
        struct vcs_verify_candidate cands[2];
        cands[0].parsed = zv_attest(&cands[0].attestation,
                                    VCS_PACKAGE_ATTEST_RESULT_TEST_PASS,
                                    ctx.package_root, ctx.release_id,
                                    ctx.recipe_root, 0x22);
        cands[1].parsed = zv_attest(&cands[1].attestation,
                                    VCS_PACKAGE_ATTEST_RESULT_TEST_PASS,
                                    ctx.package_root, ctx.release_id,
                                    ctx.recipe_root, 0x44); /* not listed */
        struct vcs_verify_quorum q;
        vcs_verify_evaluate(cands, 2, ctx.package_root, ctx.recipe_root,
                            ctx.publisher, &ctx.policy, &q);
        ZV_CHECK("quorum: unapproved signer named, no quorum",
                 !q.verified && q.counted == 1 &&
                 zv_row_rule_is(&q, 1, VCS_VERIFY_ROW_SIGNER_NOT_APPROVED));
    }

    /* Self-verification (publisher == verifier) is rejected even when the
     * operator foolishly approved the publisher key. */
    {
        struct vcs_verifier_policy with_self;
        vcs_verifier_policy_init(&with_self);
        vcs_verifier_policy_add(&with_self, ctx.publisher, NULL);
        struct privkey sk;
        struct pubkey pk;
        zv_keypair(0x22, &sk, &pk);
        vcs_verifier_policy_add(&with_self, pk.vch, NULL);
        struct vcs_verify_candidate cands[2];
        cands[0].parsed = zv_attest(&cands[0].attestation,
                                    VCS_PACKAGE_ATTEST_RESULT_TEST_PASS,
                                    ctx.package_root, ctx.release_id,
                                    ctx.recipe_root, 0x11); /* publisher */
        cands[1].parsed = zv_attest(&cands[1].attestation,
                                    VCS_PACKAGE_ATTEST_RESULT_TEST_PASS,
                                    ctx.package_root, ctx.release_id,
                                    ctx.recipe_root, 0x22);
        struct vcs_verify_quorum q;
        vcs_verify_evaluate(cands, 2, ctx.package_root, ctx.recipe_root,
                            ctx.publisher, &with_self, &q);
        ZV_CHECK("quorum: self-verification rejected",
                 !q.verified && q.counted == 1 &&
                 zv_row_rule_is(&q, 0, VCS_VERIFY_ROW_SELF_VERIFICATION));
    }

    /* A duplicate signer counts once (second attestation differs in the
     * compiler versions — it still MATCHES on roots+class). */
    {
        struct vcs_verify_candidate cands[2];
        cands[0].parsed = zv_attest(&cands[0].attestation,
                                    VCS_PACKAGE_ATTEST_RESULT_TEST_PASS,
                                    ctx.package_root, ctx.release_id,
                                    ctx.recipe_root, 0x22);
        cands[1].parsed = zv_attest(&cands[1].attestation,
                                    VCS_PACKAGE_ATTEST_RESULT_TEST_PASS,
                                    ctx.package_root, ctx.release_id,
                                    ctx.recipe_root, 0x22);
        if (cands[1].parsed)
            snprintf(cands[1].attestation.compilers[1].version,
                     sizeof(cands[1].attestation.compilers[1].version),
                     "14.1.0");
        /* Re-sign after the field change. */
        struct privkey sk;
        struct pubkey pk;
        zv_keypair(0x22, &sk, &pk);
        cands[1].parsed = cands[1].parsed &&
                          zv_sign_attest(&cands[1].attestation, &sk);
        struct vcs_verify_quorum q;
        vcs_verify_evaluate(cands, 2, ctx.package_root, ctx.recipe_root,
                            ctx.publisher, &ctx.policy, &q);
        ZV_CHECK("quorum: duplicate signer counts once",
                 !q.verified && q.counted == 1 &&
                 zv_row_rule_is(&q, 1, VCS_VERIFY_ROW_DUPLICATE_SIGNER));
    }

    /* A quorum on a FAIL class is reached but never "verified". */
    {
        struct vcs_verify_candidate cands[2];
        cands[0].parsed = zv_attest(&cands[0].attestation,
                                    VCS_PACKAGE_ATTEST_RESULT_BUILD_FAIL,
                                    ctx.package_root, ctx.release_id,
                                    ctx.recipe_root, 0x22);
        cands[1].parsed = zv_attest(&cands[1].attestation,
                                    VCS_PACKAGE_ATTEST_RESULT_BUILD_FAIL,
                                    ctx.package_root, ctx.release_id,
                                    ctx.recipe_root, 0x23);
        struct vcs_verify_quorum q;
        vcs_verify_evaluate(cands, 2, ctx.package_root, ctx.recipe_root,
                            ctx.publisher, &ctx.policy, &q);
        ZV_CHECK("quorum: fail-class quorum is not verified",
                 !q.verified && q.quorum_reached && q.quorum_signers == 2 &&
                 q.quorum_class == VCS_PACKAGE_ATTEST_RESULT_BUILD_FAIL);
    }

    /* A different recipe root is a named mismatch, never a match. */
    {
        uint8_t other_recipe[32];
        zv_pattern_root(0x90, other_recipe);
        struct vcs_verify_candidate cands[2];
        cands[0].parsed = zv_attest(&cands[0].attestation,
                                    VCS_PACKAGE_ATTEST_RESULT_TEST_PASS,
                                    ctx.package_root, ctx.release_id,
                                    ctx.recipe_root, 0x22);
        cands[1].parsed = zv_attest(&cands[1].attestation,
                                    VCS_PACKAGE_ATTEST_RESULT_TEST_PASS,
                                    ctx.package_root, ctx.release_id,
                                    other_recipe, 0x33);
        struct vcs_verify_quorum q;
        vcs_verify_evaluate(cands, 2, ctx.package_root, ctx.recipe_root,
                            ctx.publisher, &ctx.policy, &q);
        ZV_CHECK("quorum: recipe mismatch named, no quorum",
                 !q.verified && q.counted == 1 &&
                 zv_row_rule_is(&q, 1, VCS_VERIFY_ROW_RECIPE_ROOT_MISMATCH));
    }

    /* An invalid signature and an unparseable wire are named invalid. */
    {
        struct vcs_verify_candidate cands[3];
        cands[0].parsed = zv_attest(&cands[0].attestation,
                                    VCS_PACKAGE_ATTEST_RESULT_TEST_PASS,
                                    ctx.package_root, ctx.release_id,
                                    ctx.recipe_root, 0x22);
        cands[1].parsed = zv_attest(&cands[1].attestation,
                                    VCS_PACKAGE_ATTEST_RESULT_TEST_PASS,
                                    ctx.package_root, ctx.release_id,
                                    ctx.recipe_root, 0x33);
        if (cands[1].parsed)
            cands[1].attestation.signature[0] ^= 0x01;
        cands[2].parsed = false;
        struct vcs_verify_quorum q;
        vcs_verify_evaluate(cands, 3, ctx.package_root, ctx.recipe_root,
                            ctx.publisher, &ctx.policy, &q);
        ZV_CHECK("quorum: invalid attestations named",
                 !q.verified && q.counted == 1 &&
                 zv_row_rule_is(&q, 1, VCS_VERIFY_ROW_ATTESTATION_INVALID) &&
                 zv_row_rule_is(&q, 2, VCS_VERIFY_ROW_ATTESTATION_INVALID) &&
                 !q.rows[2].has_pubkey);
    }
    return failures;
}

/* ── 3. zcode package verify command ────────────────────────────────── */

struct zv_cmd {
    struct json_value input;
    struct zcl_command_request request;
    struct zcl_command_reply reply;
};

static void zv_cmd_init(struct zv_cmd *c, const char *datadir,
                        const char *root_hex)
{
    json_init(&c->input);
    json_set_object(&c->input);
    memset(&c->request, 0, sizeof(c->request));
    c->request.input = &c->input;
    zcl_command_reply_init(&c->reply, "zcl.zcode_verify_test.v1");
    (void)json_push_kv_str(&c->input, "datadir", datadir);
    (void)json_push_kv_str(&c->input, "root", root_hex);
}

static void zv_cmd_free(struct zv_cmd *c)
{
    zcl_command_reply_free(&c->reply);
    json_free(&c->input);
}

/* Publish one tiny fixture package into <store>: manifest + chunks +
 * recipe + a signed release by publisher key 0x11. Fills the roots. */
static bool zv_publish_fixture(const char *store, const char *src_content,
                               const char *test_content,
                               uint8_t package_root_out[32],
                               uint8_t release_id_out[32],
                               uint8_t recipe_root_out[32])
{
    char dir[4400];
    snprintf(dir, sizeof(dir), "%s/manifests", store);
    if (!zv_mkdir_p(dir))
        return false;
    snprintf(dir, sizeof(dir), "%s/releases", store);
    if (!zv_mkdir_p(dir))
        return false;
    snprintf(dir, sizeof(dir), "%s/recipes", store);
    if (!zv_mkdir_p(dir))
        return false;
    snprintf(dir, sizeof(dir), "%s/attestations", store);
    if (!zv_mkdir_p(dir))
        return false;
    snprintf(dir, sizeof(dir), "%s/cas/sha3", store);
    if (!zv_mkdir_p(dir))
        return false;

    struct {
        const char *path;
        const char *content;
    } files[] = {
        { "src/add.h", "#pragma once\nint add(int a, int b);\n" },
        { "src/add.c", src_content },
        { "test/test_add.c", test_content },
    };
    struct vcs_package_manifest m;
    vcs_package_manifest_init(&m);
    bool ok = true;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]) && ok; i++) {
        size_t len = strlen(files[i].content);
        uint8_t hash[32];
        struct sha3_256_ctx c;
        sha3_256_init(&c);
        sha3_256_write(&c, (const uint8_t *)files[i].content, len);
        sha3_256_finalize(&c, hash);
        ok = vcs_package_manifest_add(&m, files[i].path,
                                      VCS_PACKAGE_MODE_FILE, len, hash, 1);
        if (ok) {
            char hex[65];
            zv_hex_enc(hash, 32, hex);
            char chunk_dir[4400];
            snprintf(chunk_dir, sizeof(chunk_dir), "%s/cas/sha3/%.2s",
                     store, hex);
            char chunk_path[4400];
            snprintf(chunk_path, sizeof(chunk_path), "%s/%s", chunk_dir,
                     hex);
            ok = zv_mkdir_p(chunk_dir) &&
                 zv_write_file(chunk_path, files[i].content, len, 0600);
        }
    }
    if (ok)
        ok = vcs_package_manifest_root(&m, package_root_out);
    uint8_t *mwire = NULL;
    size_t mwire_len = 0;
    if (ok)
        ok = vcs_package_manifest_serialize(&m, &mwire, &mwire_len);
    vcs_package_manifest_free(&m);
    if (!ok)
        return false;
    char root_hex[65];
    zv_hex_enc(package_root_out, 32, root_hex);
    char path[4400];
    snprintf(path, sizeof(path), "%s/manifests/%s", store, root_hex);
    ok = zv_write_file(path, mwire, mwire_len, 0600);
    free(mwire);
    if (!ok)
        return false;

    struct vcs_package_recipe r;
    vcs_package_recipe_init(&r);
    ok = vcs_package_recipe_add_header(&r, "src/add.h", NULL) &&
         vcs_package_recipe_add_source(&r, "src/add.c", NULL) &&
         vcs_package_recipe_add_test_source(&r, "test/test_add.c", NULL) &&
         vcs_package_recipe_add_include_dir(&r, "src", NULL) &&
         vcs_package_recipe_add_library(&r, VCS_PACKAGE_RECIPE_LIB_LIBC,
                                        NULL);
    vcs_package_recipe_set_test_limits(&r, 0, 60,
                                       UINT64_C(64) * 1024u * 1024u);
    uint8_t *rwire = NULL;
    size_t rwire_len = 0;
    if (ok)
        ok = vcs_package_recipe_root(&r, recipe_root_out) ==
                 VCS_PACKAGE_RECIPE_OK &&
             vcs_package_recipe_serialize(&r, &rwire, &rwire_len) ==
                 VCS_PACKAGE_RECIPE_OK;
    vcs_package_recipe_free(&r);
    if (!ok)
        return false;
    char rroot_hex[65];
    zv_hex_enc(recipe_root_out, 32, rroot_hex);
    snprintf(path, sizeof(path), "%s/recipes/%s", store, rroot_hex);
    ok = zv_write_file(path, rwire, rwire_len, 0600);
    free(rwire);
    if (!ok)
        return false;

    struct privkey sk;
    struct pubkey pk;
    if (!zv_keypair(0x11, &sk, &pk))
        return false;
    struct vcs_package_release rel;
    memset(&rel, 0, sizeof(rel));
    rel.schema_version = VCS_PACKAGE_RELEASE_VERSION;
    snprintf(rel.name, sizeof(rel.name), "alice/addpkg");
    snprintf(rel.semver, sizeof(rel.semver), "1.0.0");
    memcpy(rel.package_root, package_root_out, 32);
    rel.has_parent = false;
    memcpy(rel.publisher_pubkey, pk.vch, COMPRESSED_PUBLIC_KEY_SIZE);
    rel.publisher_sequence = 1;
    snprintf(rel.reward_address, sizeof(rel.reward_address), "t1fixture");
    snprintf(rel.license, sizeof(rel.license), "MIT");
    memcpy(rel.recipe_root, recipe_root_out, 32);
    rel.has_znam = false;
    snprintf(rel.chain_id, sizeof(rel.chain_id), "zclassic-main");
    uint8_t id[VCS_PACKAGE_RELEASE_ID_BYTES];
    if (vcs_package_release_id(&rel, id) != VCS_PACKAGE_RELEASE_OK)
        return false;
    struct uint256 hash;
    memcpy(hash.data, id, 32);
    unsigned char compact[COMPACT_SIGNATURE_SIZE];
    if (!privkey_sign_compact(&sk, &hash, compact))
        return false;
    memcpy(rel.signature, compact + 1, VCS_PACKAGE_RELEASE_SIGNATURE_BYTES);
    uint8_t *relwire = NULL;
    size_t relwire_len = 0;
    if (vcs_package_release_serialize(&rel, &relwire, &relwire_len) !=
        VCS_PACKAGE_RELEASE_OK)
        return false;
    memcpy(release_id_out, id, 32);
    char id_hex[65];
    zv_hex_enc(id, 32, id_hex);
    snprintf(path, sizeof(path), "%s/releases/%s", store, id_hex);
    ok = zv_write_file(path, relwire, relwire_len, 0600);
    free(relwire);
    return ok;
}

/* Persist one signed attestation into <store>/attestations. */
static bool zv_store_attestation(const char *store, uint8_t cls,
                                 const uint8_t package_root[32],
                                 const uint8_t release_id[32],
                                 const uint8_t recipe_root[32],
                                 uint8_t signer_seed)
{
    struct vcs_package_attest a;
    if (!zv_attest(&a, cls, package_root, release_id, recipe_root,
                   signer_seed))
        return false;
    uint8_t id[32];
    if (vcs_package_attest_id(&a, id) != VCS_PACKAGE_ATTEST_OK)
        return false;
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    if (vcs_package_attest_serialize(&a, &wire, &wire_len) !=
        VCS_PACKAGE_ATTEST_OK)
        return false;
    char id_hex[65];
    zv_hex_enc(id, 32, id_hex);
    char path[4400];
    snprintf(path, sizeof(path), "%s/attestations/%s", store, id_hex);
    bool ok = zv_write_file(path, wire, wire_len, 0600);
    free(wire);
    return ok;
}

static bool zv_write_policy(const char *store)
{
    char ka[67], kb[67];
    if (!zv_pubkey_hex(0x22, ka) || !zv_pubkey_hex(0x33, kb))
        return false;
    char text[256];
    int n = snprintf(text, sizeof(text),
                     "# local approved verifiers\n%s\n%s\n", ka, kb);
    char path[4400];
    snprintf(path, sizeof(path), "%s/approved_verifiers", store);
    return n > 0 && zv_write_file(path, text, (size_t)n, 0600);
}

static int t_command(void)
{
    int failures = 0;
    char datadir[4400];
    snprintf(datadir, sizeof(datadir), "test-tmp/zv_cmd_%ld",
             (long)getpid());
    char store[4400];
    snprintf(store, sizeof(store), "%s/zcode", datadir);
    zv_rm_rf(datadir);

    uint8_t package_root[32], release_id[32], recipe_root[32];
    bool fixture = zv_publish_fixture(
        store,
        "#include \"add.h\"\nint add(int a, int b) { return a + b; }\n",
        "#include \"add.h\"\nint main(void) { return add(2, 3) == 5 ? 0 : 1; }\n",
        package_root, release_id, recipe_root);
    ZV_CHECK("command: fixture store publishes", fixture);
    if (!fixture)
        return failures + 1;
    char root_hex[65];
    zv_hex_enc(package_root, 32, root_hex);

    /* No allowlist -> named rejection. */
    {
        struct zv_cmd c;
        zv_cmd_init(&c, datadir, root_hex);
        zcl_native_handle_zcode_package_verify(&c.request, &c.reply);
        ZV_CHECK("command: NO_APPROVED_VERIFIERS without an allowlist",
                 strcmp(c.reply.error.code, "NO_APPROVED_VERIFIERS") == 0);
        zv_cmd_free(&c);
    }

    /* One attestation short of quorum. */
    ZV_CHECK("command: allowlist writes", zv_write_policy(store));
    ZV_CHECK("command: attestation A persists",
             zv_store_attestation(store, VCS_PACKAGE_ATTEST_RESULT_TEST_PASS,
                                  package_root, release_id, recipe_root,
                                  0x22));
    {
        struct zv_cmd c;
        zv_cmd_init(&c, datadir, root_hex);
        zcl_native_handle_zcode_package_verify(&c.request, &c.reply);
        ZV_CHECK("command: one signer is not a quorum",
                 !json_get_bool(json_get(&c.reply.data, "verified")) &&
                 !json_get_bool(json_get(&c.reply.data, "quorum_reached")) &&
                 json_get_int(json_get(&c.reply.data, "quorum_signers")) == 1);
        zv_cmd_free(&c);
    }

    /* Two approved matching attestations -> verified. */
    ZV_CHECK("command: attestation B persists",
             zv_store_attestation(store, VCS_PACKAGE_ATTEST_RESULT_TEST_PASS,
                                  package_root, release_id, recipe_root,
                                  0x33));
    {
        struct zv_cmd c;
        zv_cmd_init(&c, datadir, root_hex);
        zcl_native_handle_zcode_package_verify(&c.request, &c.reply);
        const char *qclass =
            json_get_str(json_get(&c.reply.data, "quorum_class"));
        const struct json_value *rows = json_get(&c.reply.data, "rows");
        ZV_CHECK("command: 2-of-N approved matching verifies",
                 json_get_bool(json_get(&c.reply.data, "verified")) &&
                 json_get_bool(json_get(&c.reply.data, "quorum_reached")) &&
                 json_get_int(json_get(&c.reply.data, "quorum_signers")) == 2 &&
                 qclass && strcmp(qclass, "test-pass") == 0 &&
                 json_get_int(json_get(&c.reply.data,
                                       "attestations_scanned")) == 2 &&
                 rows && json_at(rows, 1) != NULL);
        zv_cmd_free(&c);
    }

    /* A third, unapproved attestation is named and changes nothing. */
    ZV_CHECK("command: unapproved attestation persists",
             zv_store_attestation(store, VCS_PACKAGE_ATTEST_RESULT_TEST_PASS,
                                  package_root, release_id, recipe_root,
                                  0x44));
    {
        struct zv_cmd c;
        zv_cmd_init(&c, datadir, root_hex);
        zcl_native_handle_zcode_package_verify(&c.request, &c.reply);
        const struct json_value *rows = json_get(&c.reply.data, "rows");
        /* readdir order is unspecified — find the unapproved row wherever
         * it landed. */
        bool named_unapproved = false;
        for (size_t i = 0; rows && json_at(rows, i); i++) {
            const char *rule =
                json_get_str(json_get(json_at(rows, i), "rule"));
            if (rule && strcmp(rule, "signer-not-approved") == 0)
                named_unapproved = true;
        }
        ZV_CHECK("command: unapproved signer named in rows",
                 json_get_bool(json_get(&c.reply.data, "verified")) &&
                 json_get_int(json_get(&c.reply.data,
                                       "attestations_scanned")) == 3 &&
                 named_unapproved);
        zv_cmd_free(&c);
    }

    /* Rejections. */
    {
        struct zv_cmd c;
        zv_cmd_init(&c, datadir, "zz");
        zcl_native_handle_zcode_package_verify(&c.request, &c.reply);
        ZV_CHECK("command: BAD_ROOT",
                 strcmp(c.reply.error.code, "BAD_ROOT") == 0);
        zv_cmd_free(&c);
    }
    {
        uint8_t other[32];
        zv_pattern_root(0x55, other);
        char other_hex[65];
        zv_hex_enc(other, 32, other_hex);
        struct zv_cmd c;
        zv_cmd_init(&c, datadir, other_hex);
        zcl_native_handle_zcode_package_verify(&c.request, &c.reply);
        ZV_CHECK("command: UNKNOWN_PACKAGE",
                 strcmp(c.reply.error.code, "UNKNOWN_PACKAGE") == 0);
        zv_cmd_free(&c);
    }
    zv_rm_rf(datadir);
    return failures;
}

/* ── 4. end-to-end external verifier ────────────────────────────────── */

static bool zv_dir_is_empty(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return false;
    bool empty = true;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
            empty = false;
            break;
        }
    }
    closedir(d);
    return empty;
}

/* Run the verifier binary against a fixture store; returns the spawn exit
 * code and fills the captured stdout. */
static int zv_run_verifier(const char *root_hex, const char *store,
                           const char *key_path, const char *work,
                           char *buf, size_t cap)
{
    char store_arg[4400];
    char key_arg[4400];
    char work_arg[4400];
    snprintf(store_arg, sizeof(store_arg), "--store=%s", store);
    snprintf(key_arg, sizeof(key_arg), "--key=%s", key_path);
    snprintf(work_arg, sizeof(work_arg), "--work=%s", work);
    const char *argv[] = { ZV_VERIFIER_BIN, root_hex, store_arg, key_arg,
                           work_arg, NULL };
    return zcl_spawn_capture(argv, buf, cap, 300000);
}

/* The one attestation file a verifier run must have written. */
static bool zv_read_only_attestation(const char *store,
                                     struct vcs_package_attest *out)
{
    char dir[4400];
    snprintf(dir, sizeof(dir), "%s/attestations", store);
    DIR *d = opendir(dir);
    if (!d)
        return false;
    size_t count = 0;
    char name[256] = "";
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strlen(ent->d_name) != 64)
            continue;
        count++;
        snprintf(name, sizeof(name), "%s", ent->d_name);
    }
    closedir(d);
    if (count != 1)
        return false;
    char path[4400];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    uint8_t wire[VCS_PACKAGE_ATTEST_MAX_WIRE_BYTES];
    size_t wire_len = 0;
    if (!zv_read_file(path, wire, sizeof(wire), &wire_len))
        return false;
    return vcs_package_attest_parse(wire, wire_len, out) ==
           VCS_PACKAGE_ATTEST_OK;
}

static bool zv_write_key_file(const char *path, uint8_t seed, mode_t mode)
{
    struct privkey sk;
    struct pubkey pk;
    if (!zv_keypair(seed, &sk, &pk))
        return false;
    char hex[65];
    zv_hex_enc(sk.vch, 32, hex);
    return zv_write_file(path, hex, 64, mode);
}

static int t_verifier_e2e(void)
{
    int failures = 0;
    struct stat st;
    if (stat(ZV_VERIFIER_BIN, &st) != 0) {
        printf("  zcode_verify: e2e... FAIL (%s missing — run `make "
               "zclassic23-package-verify` first)\n", ZV_VERIFIER_BIN);
        return 1;
    }
    char base[4400];
    snprintf(base, sizeof(base), "test-tmp/zv_e2e_%ld", (long)getpid());
    zv_rm_rf(base);
    if (!zv_mkdir_p(base)) {
        ZV_CHECK("e2e: fixture dir", false);
        return 1;
    }

    /* ── pass fixture: a tiny real C package builds and tests green ── */
    char store[4400];
    snprintf(store, sizeof(store), "%s/store", base);
    uint8_t package_root[32], release_id[32], recipe_root[32];
    bool fixture = zv_publish_fixture(
        store,
        "#include \"add.h\"\nint add(int a, int b) { return a + b; }\n",
        "#include \"add.h\"\n#include <string.h>\n"
        "int main(void) {\n"
        "    char buf[16];\n"
        "    memset(buf, 0, sizeof(buf));\n"
        "    return add(2, 3) == 5 && buf[0] == 0 ? 0 : 1;\n"
        "}\n",
        package_root, release_id, recipe_root);
    ZV_CHECK("e2e: pass fixture publishes", fixture);
    char key_path[4400];
    snprintf(key_path, sizeof(key_path), "%s/verifier.key", base);
    ZV_CHECK("e2e: key file writes (0600)",
             zv_write_key_file(key_path, 0x22, 0600));
    char work[4400];
    snprintf(work, sizeof(work), "%s/work", base);
    ZV_CHECK("e2e: work dir", zv_mkdir_p(work));

    char root_hex[65];
    zv_hex_enc(package_root, 32, root_hex);
    char out[2048];
    int rc = zv_run_verifier(root_hex, store, key_path, work, out,
                             sizeof(out));
    struct vcs_package_attest att;
    bool have_att = zv_read_only_attestation(store, &att);
    ZV_CHECK("e2e: verifier exits 0 and writes one attestation",
             rc == 0 && have_att);
    if (rc != 0)
        printf("  zcode_verify: e2e verifier rc=%d out=%s\n", rc, out);
    if (have_att) {
        struct pubkey vk;
        struct privkey vsk;
        zv_keypair(0x22, &vsk, &vk);
        if (vcs_package_attest_verify(&att) != VCS_PACKAGE_ATTEST_OK ||
            att.result_class != VCS_PACKAGE_ATTEST_RESULT_TEST_PASS ||
            memcmp(att.verifier_pubkey, vk.vch, 33) != 0 ||
            memcmp(att.package_root, package_root, 32) != 0 ||
            memcmp(att.recipe_root, recipe_root, 32) != 0 ||
            memcmp(att.release_id, release_id, 32) != 0 ||
            !att.test_ran || att.test_exit_code != 0)
            printf("  zcode_verify: e2e attest mismatch: verify=%d class=%s "
                   "signer=%d proot=%d rroot=%d rid=%d test_ran=%d exit=%u "
                   "isolation=%u detail=%s\n",
                   vcs_package_attest_verify(&att),
                   vcs_package_attest_result_string(att.result_class),
                   memcmp(att.verifier_pubkey, vk.vch, 33) == 0,
                   memcmp(att.package_root, package_root, 32) == 0,
                   memcmp(att.recipe_root, recipe_root, 32) == 0,
                   memcmp(att.release_id, release_id, 32) == 0,
                   att.test_ran, att.test_exit_code, att.isolation,
                   att.detail);
        ZV_CHECK("e2e: attestation verifies, test-pass, signer, roots",
                 vcs_package_attest_verify(&att) == VCS_PACKAGE_ATTEST_OK &&
                 att.result_class == VCS_PACKAGE_ATTEST_RESULT_TEST_PASS &&
                 memcmp(att.verifier_pubkey, vk.vch, 33) == 0 &&
                 memcmp(att.package_root, package_root, 32) == 0 &&
                 memcmp(att.recipe_root, recipe_root, 32) == 0 &&
                 memcmp(att.release_id, release_id, 32) == 0 &&
                 att.test_ran && att.test_exit_code == 0 &&
                 (att.isolation == VCS_PACKAGE_ATTEST_ISOLATION_FULL ||
                  att.isolation == VCS_PACKAGE_ATTEST_ISOLATION_DEGRADED));
    } else {
        ZV_CHECK("e2e: attestation verifies, test-pass, signer, roots",
                 false);
    }
    ZV_CHECK("e2e: produced binaries deleted (work tree empty)",
             zv_dir_is_empty(work));

    /* A world-readable key file is refused (exit 3, nothing signed). */
    {
        char store2[4400];
        snprintf(store2, sizeof(store2), "%s/store_keymode", base);
        uint8_t pr2[32], ri2[32], rr2[32];
        zv_publish_fixture(store2,
                           "#include \"add.h\"\nint add(int a, int b) { return a + b; }\n",
                           "#include \"add.h\"\nint main(void) { return add(2, 3) == 5 ? 0 : 1; }\n",
                           pr2, ri2, rr2);
        char bad_key[4400];
        snprintf(bad_key, sizeof(bad_key), "%s/bad.key", base);
        zv_write_key_file(bad_key, 0x22, 0644);
        char pr2_hex[65];
        zv_hex_enc(pr2, 32, pr2_hex);
        int krc = zv_run_verifier(pr2_hex, store2, bad_key, work, out,
                                  sizeof(out));
        ZV_CHECK("e2e: world-readable key refused, no attestation",
                 krc == 3 && !zv_read_only_attestation(store2, &att));
    }

    /* ── hostile: a syntax-error source builds a build-fail attestation ─ */
    {
        char store3[4400];
        snprintf(store3, sizeof(store3), "%s/store_buildfail", base);
        uint8_t pr3[32], ri3[32], rr3[32];
        bool f3 = zv_publish_fixture(
            store3,
            "#include \"add.h\"\nint add(int a, int b) { return a + ; }\n",
            "#include \"add.h\"\nint main(void) { return add(2, 3) == 5 ? 0 : 1; }\n",
            pr3, ri3, rr3);
        char pr3_hex[65];
        zv_hex_enc(pr3, 32, pr3_hex);
        int brc = zv_run_verifier(pr3_hex, store3, key_path, work, out,
                                  sizeof(out));
        struct vcs_package_attest batt;
        bool have_batt = zv_read_only_attestation(store3, &batt);
        ZV_CHECK("e2e: syntax-error source fails closed (build-fail)",
                 f3 && brc == 0 && have_batt &&
                 batt.result_class == VCS_PACKAGE_ATTEST_RESULT_BUILD_FAIL &&
                 (batt.detail_code == VCS_PACKAGE_ATTEST_DETAIL_COMPILE_ERROR ||
                  batt.detail_code == VCS_PACKAGE_ATTEST_DETAIL_LINK_ERROR) &&
                 vcs_package_attest_verify(&batt) == VCS_PACKAGE_ATTEST_OK);
        ZV_CHECK("e2e: hostile work tree cleaned", zv_dir_is_empty(work));
    }

    /* ── hostile: socket() in a test dies by seccomp (network denial) ── */
    {
        char store4[4400];
        snprintf(store4, sizeof(store4), "%s/store_socket", base);
        uint8_t pr4[32], ri4[32], rr4[32];
        bool f4 = zv_publish_fixture(
            store4,
            "#include \"add.h\"\nint add(int a, int b) { return a + b; }\n",
            "#include <sys/socket.h>\n#include <netinet/in.h>\n"
            "int main(void) {\n"
            "    int s = socket(AF_INET, SOCK_STREAM, 0);\n"
            "    return s >= 0 ? 0 : 1;\n"
            "}\n",
            pr4, ri4, rr4);
        char pr4_hex[65];
        zv_hex_enc(pr4, 32, pr4_hex);
        int src = zv_run_verifier(pr4_hex, store4, key_path, work, out,
                                  sizeof(out));
        struct vcs_package_attest satt;
        bool have_satt = zv_read_only_attestation(store4, &satt);
        ZV_CHECK("e2e: socket() test killed by sandbox (test-fail/signal)",
                 f4 && src == 0 && have_satt &&
                 satt.result_class == VCS_PACKAGE_ATTEST_RESULT_TEST_FAIL &&
                 satt.detail_code == VCS_PACKAGE_ATTEST_DETAIL_TEST_SIGNAL &&
                 vcs_package_attest_verify(&satt) == VCS_PACKAGE_ATTEST_OK);
        if (!f4 || src != 0 || !have_satt ||
            satt.result_class != VCS_PACKAGE_ATTEST_RESULT_TEST_FAIL ||
            satt.detail_code != VCS_PACKAGE_ATTEST_DETAIL_TEST_SIGNAL ||
            vcs_package_attest_verify(&satt) != VCS_PACKAGE_ATTEST_OK)
            printf("  zcode_verify: socket e2e f4=%d src=%d have=%d "
                   "class=%s detail=%s text=%s out=%s\n", f4, src, have_satt,
                   vcs_package_attest_result_string(satt.result_class),
                   vcs_package_attest_detail_string(satt.detail_code),
                   satt.detail, out);
    }

    zv_rm_rf(base);
    return failures;
}

int test_zcode_verify(void)
{
    printf("\n=== zcode_verify: external verifier attestations ===\n");
    int failures = 0;
    failures += t_codec();
    failures += t_signature();
    failures += t_policy();
    failures += t_quorum();
    failures += t_command();
    failures += t_verifier_e2e();
    printf("=== zcode_verify complete: %d failure(s) ===\n", failures);
    return failures;
}
