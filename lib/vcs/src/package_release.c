/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * package_release — implementation. See vcs/package_release.h. */

#include "vcs/package_release.h"

#include "crypto/sha3.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include "vcs_priv.h"

#include <secp256k1.h>
#include <string.h>

static const uint8_t release_magic[VCS_PACKAGE_RELEASE_WIRE_MAGIC_BYTES] = {
    'Z', 'C', 'L', 'R', 'E', 'L', '\r', '\n'
};
static const uint8_t release_id_domain[] = VCS_PACKAGE_RELEASE_ID_DOMAIN;

/* The vendored libsecp256k1 archive does not export the
 * secp256k1_context_static symbol, so this layer keeps its own verify-only
 * context, created once at load time — the same pattern as
 * lib/crypto_registry/src/scheme_secp256k1_ecdsa.c. */
static secp256k1_context *release_verify_ctx;

__attribute__((constructor))
static void release_verify_ctx_init(void)
{
    release_verify_ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
}

__attribute__((destructor))
static void release_verify_ctx_destroy(void)
{
    if (release_verify_ctx)
        secp256k1_context_destroy(release_verify_ctx);
}

/* secp256k1 group order half, n/2, big-endian: the low-S bound. A canonical
 * v1 signature carries s <= n/2; anything above is a malleated encoding. */
static const uint8_t release_half_order[32] = {
    0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x5d, 0x57, 0x6e, 0x73, 0x57, 0xa4, 0x50, 0x1d,
    0xdf, 0xe9, 0x2f, 0x46, 0x68, 0x1b, 0x20, 0xa0,
};

static const char *const release_license_allowlist[] = {
    "0BSD",
    "MIT",
    "Apache-2.0",
    "BSD-2-Clause",
    "BSD-3-Clause",
    "ISC",
    "Zlib",
};

/* ── field grammars ───────────────────────────────────────────────── */

static bool release_name_half_valid_n(const char *s, size_t len)
{
    if (!s || len == 0 || len > VCS_PACKAGE_RELEASE_NAME_HALF_MAX)
        return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        bool lower = c >= 'a' && c <= 'z';
        bool digit = c >= '0' && c <= '9';
        if (!lower && !digit && c != '-')
            return false;
    }
    return s[0] != '-' && s[len - 1] != '-';
}

static bool release_name_valid_n(const char *name, size_t len)
{
    if (!name || len == 0 || len > VCS_PACKAGE_RELEASE_NAME_MAX)
        return false;
    const char *slash = memchr(name, '/', len);
    if (!slash || memchr(slash + 1, '/', len - (size_t)(slash - name) - 1u))
        return false;
    size_t publisher_len = (size_t)(slash - name);
    size_t package_len = len - publisher_len - 1u;
    return release_name_half_valid_n(name, publisher_len) &&
           release_name_half_valid_n(slash + 1, package_len);
}

static bool release_semver_ident_byte(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '-';
}

/* A version-core number: digits only, no leading zeros ("0" is fine). The
 * core is strictly numeric so the prerelease '-' is never absorbed into
 * PATCH. */
static bool release_semver_number(const char **pp, const char *end)
{
    const char *p = *pp;
    size_t len = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        p++;
        len++;
    }
    if (len == 0)
        return false;
    if (len > 1 && (*pp)[0] == '0')
        return false; /* leading zero */
    *pp = p;
    return true;
}

/* One dot-separated identifier of [0-9A-Za-z-]. Numeric identifiers
 * (all digits) must not have leading zeros unless allow_leading_zero —
 * build metadata permits them, version core and prerelease do not. */
static bool release_semver_ident(const char **pp, const char *end,
                                 bool allow_leading_zero)
{
    const char *p = *pp;
    bool numeric = true;
    size_t len = 0;
    while (p < end && release_semver_ident_byte((unsigned char)*p)) {
        if (*p < '0' || *p > '9')
            numeric = false;
        p++;
        len++;
    }
    if (len == 0)
        return false;
    if (!allow_leading_zero && numeric && len > 1 && (*pp)[0] == '0')
        return false; /* leading zero in a numeric identifier */
    *pp = p;
    return true;
}

static bool release_semver_valid_n(const char *semver, size_t len)
{
    if (!semver || len == 0 || len > VCS_PACKAGE_RELEASE_SEMVER_MAX)
        return false;
    const char *p = semver;
    const char *end = semver + len;

    /* core: MAJOR "." MINOR "." PATCH — strictly numeric, no leading
     * zeros. */
    for (int part = 0; part < 3; part++) {
        if (!release_semver_number(&p, end))
            return false;
        if (part < 2) {
            if (p >= end || *p != '.')
                return false;
            p++;
        }
    }
    /* optional "-prerelease[.prerelease...]" */
    if (p < end && *p == '-') {
        p++;
        for (;;) {
            if (!release_semver_ident(&p, end, false))
                return false;
            if (p >= end || *p != '.')
                break;
            p++;
        }
    }
    /* optional "+build[.build...]" — identifiers may have leading zeros. */
    if (p < end && *p == '+') {
        p++;
        for (;;) {
            if (!release_semver_ident(&p, end, true))
                return false;
            if (p >= end || *p != '.')
                break;
            p++;
        }
    }
    return p == end;
}

static bool release_license_valid_n(const char *license, size_t len)
{
    if (!license || len == 0 || len > VCS_PACKAGE_RELEASE_LICENSE_MAX)
        return false;
    for (size_t i = 0;
         i < sizeof(release_license_allowlist) /
             sizeof(release_license_allowlist[0]); i++) {
        const char *allowed = release_license_allowlist[i];
        if (strlen(allowed) == len && memcmp(license, allowed, len) == 0)
            return true;
    }
    return false;
}

static bool release_reward_valid_n(const char *reward, size_t len)
{
    if (!reward || len > VCS_PACKAGE_RELEASE_REWARD_MAX)
        return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)reward[i];
        if (c < 0x21u || c > 0x7eu)
            return false;
    }
    return true;
}

static bool release_chain_id_valid_n(const char *chain_id, size_t len)
{
    if (!chain_id || len == 0 || len > VCS_PACKAGE_RELEASE_CHAIN_ID_MAX)
        return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)chain_id[i];
        bool lower = c >= 'a' && c <= 'z';
        bool digit = c >= '0' && c <= '9';
        if (!lower && !digit && c != '-')
            return false;
    }
    return true;
}

static bool release_pubkey_valid(const uint8_t pubkey[33])
{
    secp256k1_pubkey parsed;
    return pubkey &&
           secp256k1_ec_pubkey_parse(release_verify_ctx, &parsed,
                                     pubkey,
                                     VCS_PACKAGE_RELEASE_PUBKEY_BYTES) == 1;
}

/* s (big-endian, second half of the compact signature) must be <= n/2. */
static bool release_signature_low_s(const uint8_t signature[64])
{
    return memcmp(signature + 32, release_half_order,
                  sizeof(release_half_order)) <= 0;
}

/* A struct-field string must be NUL-terminated within its fixed buffer
 * before any grammar check runs strlen-free validation on the content. */
static size_t release_field_len(const char *field, size_t capacity)
{
    return strnlen(field, capacity);
}

/* An all-zero root is the "no object" sentinel, never a real commitment. */
static bool release_root_zero(const uint8_t root[32])
{
    uint8_t acc = 0;
    for (size_t i = 0; i < 32; i++)
        acc |= root[i];
    return acc == 0;
}

/* ── error strings ────────────────────────────────────────────────── */

const char *vcs_package_release_error_string(
    enum vcs_package_release_error error)
{
    switch (error) {
    case VCS_PACKAGE_RELEASE_OK: return "ok";
    case VCS_PACKAGE_RELEASE_ERR_NULL: return "null-argument";
    case VCS_PACKAGE_RELEASE_ERR_ALLOC: return "allocation-failed";
    case VCS_PACKAGE_RELEASE_ERR_SCHEMA_VERSION: return "schema-version";
    case VCS_PACKAGE_RELEASE_ERR_NAME: return "package-name";
    case VCS_PACKAGE_RELEASE_ERR_SEMVER: return "semantic-version";
    case VCS_PACKAGE_RELEASE_ERR_PARENT_FLAG: return "parent-presence-flag";
    case VCS_PACKAGE_RELEASE_ERR_PUBKEY: return "publisher-pubkey";
    case VCS_PACKAGE_RELEASE_ERR_REWARD: return "reward-address";
    case VCS_PACKAGE_RELEASE_ERR_LICENSE: return "spdx-license";
    case VCS_PACKAGE_RELEASE_ERR_ZNAM_FLAG: return "znam-presence-flag";
    case VCS_PACKAGE_RELEASE_ERR_ZNAM: return "znam-name";
    case VCS_PACKAGE_RELEASE_ERR_CHAIN_ID: return "chain-id";
    case VCS_PACKAGE_RELEASE_ERR_SIG_LOW_S: return "signature-not-low-s";
    case VCS_PACKAGE_RELEASE_ERR_SIG_VERIFY: return "signature-verify";
    case VCS_PACKAGE_RELEASE_ERR_WIRE_MAGIC: return "wire-magic";
    case VCS_PACKAGE_RELEASE_ERR_WIRE_OVERSIZE: return "wire-oversize";
    case VCS_PACKAGE_RELEASE_ERR_WIRE_TRUNCATED: return "wire-truncated";
    case VCS_PACKAGE_RELEASE_ERR_WIRE_TRAILING: return "wire-trailing-bytes";
    case VCS_PACKAGE_RELEASE_ERR_PACKAGE_ROOT: return "package-root-zero";
    case VCS_PACKAGE_RELEASE_ERR_RECIPE_ROOT: return "recipe-root-zero";
    case VCS_PACKAGE_RELEASE_ERR_SEQUENCE: return "publisher-sequence-zero";
    case VCS_PACKAGE_RELEASE_ERR_PARENT_ROOT: return "parent-root-zero";
    }
    return "unknown-error";
}

/* ── validation ───────────────────────────────────────────────────── */

enum vcs_package_release_error vcs_package_release_validate(
    const struct vcs_package_release *release)
{
    if (!release)
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_NULL, "vcs.release",
                   "null release");
    if (release->schema_version != VCS_PACKAGE_RELEASE_VERSION)
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_SCHEMA_VERSION, "vcs.release",
                   "schema version %u != %u",
                   (unsigned)release->schema_version,
                   VCS_PACKAGE_RELEASE_VERSION);

    size_t name_len = release_field_len(release->name,
                                        sizeof(release->name));
    if (!release_name_valid_n(release->name, name_len))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_NAME, "vcs.release",
                   "bad publisher/package name");

    size_t semver_len = release_field_len(release->semver,
                                          sizeof(release->semver));
    if (!release_semver_valid_n(release->semver, semver_len))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_SEMVER, "vcs.release",
                   "bad semantic version");

    if (release_root_zero(release->package_root))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_PACKAGE_ROOT, "vcs.release",
                   "all-zero package root");
    if (release->has_parent && release_root_zero(release->parent_root))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_PARENT_ROOT, "vcs.release",
                   "parent flagged but all-zero parent root");

    if (!release_pubkey_valid(release->publisher_pubkey))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_PUBKEY, "vcs.release",
                   "publisher pubkey is not a compressed curve point");

    if (release->publisher_sequence == 0)
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_SEQUENCE, "vcs.release",
                   "publisher sequence must be >= 1");

    size_t reward_len = release_field_len(release->reward_address,
                                          sizeof(release->reward_address));
    if (!release_reward_valid_n(release->reward_address, reward_len))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_REWARD, "vcs.release",
                   "bad contributor reward address");

    size_t license_len = release_field_len(release->license,
                                           sizeof(release->license));
    if (!release_license_valid_n(release->license, license_len))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_LICENSE, "vcs.release",
                   "license not on the v1 SPDX allowlist");

    if (release_root_zero(release->recipe_root))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_RECIPE_ROOT, "vcs.release",
                   "all-zero recipe root");

    if (release->has_znam) {
        size_t znam_len = release_field_len(release->znam,
                                            sizeof(release->znam));
        if (!release_name_half_valid_n(release->znam, znam_len))
            LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_ZNAM, "vcs.release",
                       "bad ZNAM pointer name");
    }

    size_t chain_len = release_field_len(release->chain_id,
                                         sizeof(release->chain_id));
    if (!release_chain_id_valid_n(release->chain_id, chain_len))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_CHAIN_ID, "vcs.release",
                   "bad chain identifier");

    return VCS_PACKAGE_RELEASE_OK;
}

/* ── canonical encoding ───────────────────────────────────────────── */

/* Exact byte count of the release-id preimage: the canonical encoding of
 * every field except the signature. Caller must have validated. */
static size_t release_body_bytes(const struct vcs_package_release *release)
{
    return VCS_PACKAGE_RELEASE_WIRE_MAGIC_BYTES + 2u +
           2u + strlen(release->name) +
           2u + strlen(release->semver) +
           32u +
           1u + (release->has_parent ? 32u : 0u) +
           VCS_PACKAGE_RELEASE_PUBKEY_BYTES + 8u +
           2u + strlen(release->reward_address) +
           2u + strlen(release->license) +
           32u +
           1u + (release->has_znam ? 2u + strlen(release->znam) : 0u) +
           2u + strlen(release->chain_id);
}

/* Emit the canonical body (everything but the signature) into out, which
 * must hold release_body_bytes(release) bytes. Returns the byte count. */
static size_t release_body_encode(const struct vcs_package_release *release,
                                  uint8_t *out)
{
    size_t off = 0;
    memcpy(out + off, release_magic, sizeof(release_magic));
    off += sizeof(release_magic);
    vcs_wr_u16le(out + off, release->schema_version);
    off += 2;

    size_t name_len = strlen(release->name);
    vcs_wr_u16le(out + off, (uint16_t)name_len);
    off += 2;
    memcpy(out + off, release->name, name_len);
    off += name_len;

    size_t semver_len = strlen(release->semver);
    vcs_wr_u16le(out + off, (uint16_t)semver_len);
    off += 2;
    memcpy(out + off, release->semver, semver_len);
    off += semver_len;

    memcpy(out + off, release->package_root, 32);
    off += 32;

    out[off++] = release->has_parent ? 1u : 0u;
    if (release->has_parent) {
        memcpy(out + off, release->parent_root, 32);
        off += 32;
    }

    memcpy(out + off, release->publisher_pubkey,
           VCS_PACKAGE_RELEASE_PUBKEY_BYTES);
    off += VCS_PACKAGE_RELEASE_PUBKEY_BYTES;
    vcs_wr_u64le(out + off, release->publisher_sequence);
    off += 8;

    size_t reward_len = strlen(release->reward_address);
    vcs_wr_u16le(out + off, (uint16_t)reward_len);
    off += 2;
    memcpy(out + off, release->reward_address, reward_len);
    off += reward_len;

    size_t license_len = strlen(release->license);
    vcs_wr_u16le(out + off, (uint16_t)license_len);
    off += 2;
    memcpy(out + off, release->license, license_len);
    off += license_len;

    memcpy(out + off, release->recipe_root, 32);
    off += 32;

    out[off++] = release->has_znam ? 1u : 0u;
    if (release->has_znam) {
        size_t znam_len = strlen(release->znam);
        vcs_wr_u16le(out + off, (uint16_t)znam_len);
        off += 2;
        memcpy(out + off, release->znam, znam_len);
        off += znam_len;
    }

    size_t chain_len = strlen(release->chain_id);
    vcs_wr_u16le(out + off, (uint16_t)chain_len);
    off += 2;
    memcpy(out + off, release->chain_id, chain_len);
    off += chain_len;

    return off;
}

enum vcs_package_release_error vcs_package_release_id(
    const struct vcs_package_release *release,
    uint8_t out[VCS_PACKAGE_RELEASE_ID_BYTES])
{
    if (!out)
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_NULL, "vcs.release",
                   "null release id output");
    enum vcs_package_release_error error =
        vcs_package_release_validate(release);
    if (error != VCS_PACKAGE_RELEASE_OK)
        return error;

    uint8_t body[VCS_PACKAGE_RELEASE_MAX_WIRE_BYTES];
    size_t body_len = release_body_encode(release, body);

    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    sha3_256_write(&ctx, release_id_domain, sizeof(release_id_domain));
    sha3_256_write(&ctx, body, body_len);
    sha3_256_finalize(&ctx, out);
    return VCS_PACKAGE_RELEASE_OK;
}

enum vcs_package_release_error vcs_package_release_serialize(
    const struct vcs_package_release *release, uint8_t **out,
    size_t *out_len)
{
    if (!out || !out_len)
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_NULL, "vcs.release",
                   "null release serialization output");
    *out = NULL;
    *out_len = 0;
    enum vcs_package_release_error error =
        vcs_package_release_validate(release);
    if (error != VCS_PACKAGE_RELEASE_OK)
        return error;

    size_t body_len = release_body_bytes(release);
    size_t total = body_len + VCS_PACKAGE_RELEASE_SIGNATURE_BYTES;
    uint8_t *wire = zcl_malloc(total, "vcs_release_wire");
    if (!wire)
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_ALLOC, "vcs.release",
                   "alloc release wire");
    size_t written = release_body_encode(release, wire);
    memcpy(wire + written, release->signature,
           VCS_PACKAGE_RELEASE_SIGNATURE_BYTES);
    *out = wire;
    *out_len = total;
    return VCS_PACKAGE_RELEASE_OK;
}

/* ── parsing ──────────────────────────────────────────────────────── */

static bool release_wire_has(size_t wire_len, size_t off, size_t need)
{
    return off <= wire_len && need <= wire_len - off;
}

/* Read one [2 len][bytes] string field into a fixed buffer, validating the
 * bound and NUL-terminating. An embedded NUL would let a non-canonical
 * encoding pass the grammar checks (they see only the strlen prefix), so it
 * is rejected with the field's own grammar error. */
static enum vcs_package_release_error release_read_string(
    const uint8_t *wire, size_t wire_len, size_t *off, size_t max_len,
    char *out, size_t out_capacity, const char *what,
    enum vcs_package_release_error nul_error)
{
    if (!release_wire_has(wire_len, *off, 2u))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_WIRE_TRUNCATED, "vcs.release",
                   "release wire truncated at %s length", what);
    uint16_t len = vcs_rd_u16le(wire + *off);
    *off += 2;
    if (len > max_len || (size_t)len + 1u > out_capacity ||
        !release_wire_has(wire_len, *off, len))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_WIRE_TRUNCATED, "vcs.release",
                   "release wire truncated/oversize %s", what);
    if (len > 0)
        memcpy(out, wire + *off, len);
    out[len] = '\0';
    if (strnlen(out, len) != len)
        LOG_RETURN(nul_error, "vcs.release",
                   "release wire embedded NUL in %s", what);
    *off += len;
    return VCS_PACKAGE_RELEASE_OK;
}

enum vcs_package_release_error vcs_package_release_parse(
    const uint8_t *wire, size_t wire_len,
    struct vcs_package_release *out)
{
    if (!out)
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_NULL, "vcs.release",
                   "null release parse output");
    memset(out, 0, sizeof(*out));
    if (!wire)
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_NULL, "vcs.release",
                   "null release wire");
    if (wire_len > VCS_PACKAGE_RELEASE_MAX_WIRE_BYTES)
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_WIRE_OVERSIZE, "vcs.release",
                   "release wire oversize: %zu", wire_len);

    size_t off = 0;
    if (!release_wire_has(wire_len, off, sizeof(release_magic)) ||
        memcmp(wire, release_magic, sizeof(release_magic)) != 0)
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_WIRE_MAGIC, "vcs.release",
                   "release wire bad magic");
    off += sizeof(release_magic);

    if (!release_wire_has(wire_len, off, 2u))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_WIRE_TRUNCATED, "vcs.release",
                   "release wire truncated at schema version");
    out->schema_version = vcs_rd_u16le(wire + off);
    off += 2;
    if (out->schema_version != VCS_PACKAGE_RELEASE_VERSION)
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_SCHEMA_VERSION, "vcs.release",
                   "release wire schema version %u",
                   (unsigned)out->schema_version);

    enum vcs_package_release_error error = release_read_string(
        wire, wire_len, &off, VCS_PACKAGE_RELEASE_NAME_MAX, out->name,
        sizeof(out->name), "name", VCS_PACKAGE_RELEASE_ERR_NAME);
    if (error != VCS_PACKAGE_RELEASE_OK)
        return error;
    if (!release_name_valid_n(out->name, strlen(out->name)))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_NAME, "vcs.release",
                   "release wire bad package name");

    error = release_read_string(
        wire, wire_len, &off, VCS_PACKAGE_RELEASE_SEMVER_MAX, out->semver,
        sizeof(out->semver), "semver", VCS_PACKAGE_RELEASE_ERR_SEMVER);
    if (error != VCS_PACKAGE_RELEASE_OK)
        return error;
    if (!release_semver_valid_n(out->semver, strlen(out->semver)))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_SEMVER, "vcs.release",
                   "release wire bad semantic version");

    if (!release_wire_has(wire_len, off, 32u + 1u))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_WIRE_TRUNCATED, "vcs.release",
                   "release wire truncated at package root/parent flag");
    memcpy(out->package_root, wire + off, 32);
    off += 32;
    if (release_root_zero(out->package_root))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_PACKAGE_ROOT, "vcs.release",
                   "release wire all-zero package root");
    uint8_t parent_flag = wire[off++];
    if (parent_flag > 1u)
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_PARENT_FLAG, "vcs.release",
                   "release wire parent flag %u", (unsigned)parent_flag);
    out->has_parent = parent_flag == 1u;
    if (out->has_parent) {
        if (!release_wire_has(wire_len, off, 32u))
            LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_WIRE_TRUNCATED,
                       "vcs.release",
                       "release wire truncated at parent root");
        memcpy(out->parent_root, wire + off, 32);
        off += 32;
        if (release_root_zero(out->parent_root))
            LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_PARENT_ROOT, "vcs.release",
                       "release wire parent flagged but all-zero root");
    }

    if (!release_wire_has(wire_len, off,
                          VCS_PACKAGE_RELEASE_PUBKEY_BYTES + 8u))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_WIRE_TRUNCATED, "vcs.release",
                   "release wire truncated at pubkey/sequence");
    memcpy(out->publisher_pubkey, wire + off,
           VCS_PACKAGE_RELEASE_PUBKEY_BYTES);
    off += VCS_PACKAGE_RELEASE_PUBKEY_BYTES;
    if (!release_pubkey_valid(out->publisher_pubkey))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_PUBKEY, "vcs.release",
                   "release wire pubkey is not a compressed curve point");
    out->publisher_sequence = vcs_rd_u64le(wire + off);
    off += 8;
    if (out->publisher_sequence == 0)
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_SEQUENCE, "vcs.release",
                   "release wire publisher sequence 0");

    error = release_read_string(
        wire, wire_len, &off, VCS_PACKAGE_RELEASE_REWARD_MAX,
        out->reward_address, sizeof(out->reward_address), "reward address",
        VCS_PACKAGE_RELEASE_ERR_REWARD);
    if (error != VCS_PACKAGE_RELEASE_OK)
        return error;
    if (!release_reward_valid_n(out->reward_address,
                                strlen(out->reward_address)))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_REWARD, "vcs.release",
                   "release wire bad reward address");

    error = release_read_string(
        wire, wire_len, &off, VCS_PACKAGE_RELEASE_LICENSE_MAX, out->license,
        sizeof(out->license), "license", VCS_PACKAGE_RELEASE_ERR_LICENSE);
    if (error != VCS_PACKAGE_RELEASE_OK)
        return error;
    if (!release_license_valid_n(out->license, strlen(out->license)))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_LICENSE, "vcs.release",
                   "release wire license not on the v1 SPDX allowlist");

    if (!release_wire_has(wire_len, off, 32u + 1u))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_WIRE_TRUNCATED, "vcs.release",
                   "release wire truncated at recipe root/znam flag");
    memcpy(out->recipe_root, wire + off, 32);
    off += 32;
    if (release_root_zero(out->recipe_root))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_RECIPE_ROOT, "vcs.release",
                   "release wire all-zero recipe root");
    uint8_t znam_flag = wire[off++];
    if (znam_flag > 1u)
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_ZNAM_FLAG, "vcs.release",
                   "release wire znam flag %u", (unsigned)znam_flag);
    out->has_znam = znam_flag == 1u;
    if (out->has_znam) {
        error = release_read_string(
            wire, wire_len, &off, VCS_PACKAGE_RELEASE_ZNAM_MAX, out->znam,
            sizeof(out->znam), "znam", VCS_PACKAGE_RELEASE_ERR_ZNAM);
        if (error != VCS_PACKAGE_RELEASE_OK)
            return error;
        if (!release_name_half_valid_n(out->znam, strlen(out->znam)))
            LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_ZNAM, "vcs.release",
                       "release wire bad ZNAM pointer name");
    }

    error = release_read_string(
        wire, wire_len, &off, VCS_PACKAGE_RELEASE_CHAIN_ID_MAX,
        out->chain_id, sizeof(out->chain_id), "chain id",
        VCS_PACKAGE_RELEASE_ERR_CHAIN_ID);
    if (error != VCS_PACKAGE_RELEASE_OK)
        return error;
    if (!release_chain_id_valid_n(out->chain_id, strlen(out->chain_id)))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_CHAIN_ID, "vcs.release",
                   "release wire bad chain identifier");

    if (!release_wire_has(wire_len, off,
                          VCS_PACKAGE_RELEASE_SIGNATURE_BYTES))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_WIRE_TRUNCATED, "vcs.release",
                   "release wire truncated at signature");
    memcpy(out->signature, wire + off, VCS_PACKAGE_RELEASE_SIGNATURE_BYTES);
    off += VCS_PACKAGE_RELEASE_SIGNATURE_BYTES;

    if (off != wire_len)
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_WIRE_TRAILING, "vcs.release",
                   "release wire trailing bytes: %zu at %zu",
                   wire_len - off, off);
    return VCS_PACKAGE_RELEASE_OK;
}

/* ── verification ─────────────────────────────────────────────────── */

enum vcs_package_release_error vcs_package_release_verify(
    const struct vcs_package_release *release)
{
    uint8_t id[VCS_PACKAGE_RELEASE_ID_BYTES];
    enum vcs_package_release_error error =
        vcs_package_release_id(release, id);
    if (error != VCS_PACKAGE_RELEASE_OK)
        return error;

    if (!release_signature_low_s(release->signature))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_SIG_LOW_S, "vcs.release",
                   "release signature is not low-S canonical");

    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_parse(release_verify_ctx, &pubkey,
                                   release->publisher_pubkey,
                                   VCS_PACKAGE_RELEASE_PUBKEY_BYTES))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_PUBKEY, "vcs.release",
                   "release pubkey parse failed at verify");

    secp256k1_ecdsa_signature signature;
    (void)secp256k1_ecdsa_signature_parse_compact(
        release_verify_ctx, &signature, release->signature);
    if (!secp256k1_ecdsa_verify(release_verify_ctx, &signature, id,
                                &pubkey))
        LOG_RETURN(VCS_PACKAGE_RELEASE_ERR_SIG_VERIFY, "vcs.release",
                   "release signature does not verify for the embedded "
                   "publisher key");
    return VCS_PACKAGE_RELEASE_OK;
}

bool vcs_package_release_is_duplicate(
    const struct vcs_package_release *a,
    const struct vcs_package_release *b)
{
    if (!a || !b)
        return false;
    return a->publisher_sequence == b->publisher_sequence &&
           memcmp(a->publisher_pubkey, b->publisher_pubkey,
                  VCS_PACKAGE_RELEASE_PUBKEY_BYTES) == 0 &&
           memcmp(a->package_root, b->package_root, 32) == 0;
}

bool vcs_package_release_parent(const struct vcs_package_release *release,
                                uint8_t out_root[32])
{
    if (!release || !out_root)
        return false;
    if (!release->has_parent) {
        memset(out_root, 0, 32);
        return false;
    }
    memcpy(out_root, release->parent_root, 32);
    return true;
}
