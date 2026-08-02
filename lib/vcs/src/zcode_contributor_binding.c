/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: contributor_binding.v1 implementation. See
 * vcs/zcode_contributor_binding.h for the wire layout and semantics. */

#include "vcs/zcode_contributor_binding.h"

#include "base/serialize_le.h"
#include "core/hash.h"
#include "crypto/ed25519.h"
#include "crypto/sha3.h"

#include <secp256k1.h>
#include <string.h>

static const uint8_t binding_magic[8] = {'Z','C','B','I','N','D','\r','\n'};

/* The vendored libsecp256k1 archive does not export the
 * secp256k1_context_static symbol, so this layer keeps its own context,
 * created once at load time — the same pattern as
 * lib/vcs/src/package_release.c. Seal needs SIGN; verify needs VERIFY. */
static secp256k1_context *binding_ctx;

__attribute__((constructor))
static void binding_ctx_init(void)
{
    binding_ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN |
                                           SECP256K1_CONTEXT_VERIFY);
}

__attribute__((destructor))
static void binding_ctx_destroy(void)
{
    if (binding_ctx)
        secp256k1_context_destroy(binding_ctx);
}

/* secp256k1 group order half, n/2, big-endian: the low-S bound. A canonical
 * v1 signature carries s <= n/2; anything above is a malleated encoding. */
static const uint8_t binding_half_order[32] = {
    0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x5d, 0x57, 0x6e, 0x73, 0x57, 0xa4, 0x50, 0x1d,
    0xdf, 0xe9, 0x2f, 0x46, 0x68, 0x1b, 0x20, 0xa0,
};

static bool bytes_nonzero(const uint8_t *bytes, size_t len)
{
    uint8_t any = 0;
    if (!bytes) return false;
    for (size_t i = 0; i < len; i++) any |= bytes[i];
    return any != 0;
}

static bool root_nonzero(const uint8_t root[32])
{
    return bytes_nonzero(root, 32);
}

static void put_bytes(uint8_t *wire, size_t *off, const void *src, size_t len)
{
    memcpy(wire + *off, src, len);
    *off += len;
}

static void put_u16(uint8_t *wire, size_t *off, uint16_t value)
{
    zcl_write_u16_le(wire + *off, value);
    *off += 2;
}

static void put_u64(uint8_t *wire, size_t *off, uint64_t value)
{
    zcl_write_u64_le(wire + *off, value);
    *off += 8;
}

static void get_bytes(const uint8_t *wire, size_t *off, void *out, size_t len)
{
    memcpy(out, wire + *off, len);
    *off += len;
}

static uint16_t get_u16(const uint8_t *wire, size_t *off)
{
    uint16_t value = zcl_read_u16_le(wire + *off);
    *off += 2;
    return value;
}

static uint64_t get_u64(const uint8_t *wire, size_t *off)
{
    uint64_t value = zcl_read_u64_le(wire + *off);
    *off += 8;
    return value;
}

static void binding_root_hash(const char *domain, size_t domain_len,
                              const uint8_t *wire, size_t wire_len,
                              uint8_t out[32])
{
    struct sha3_256_ctx sha;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)domain, domain_len);
    sha3_256_write(&sha, wire, wire_len);
    sha3_256_finalize(&sha, out);
}

/* s (big-endian, second half of the compact signature) must be <= n/2. */
static bool binding_signature_low_s(const uint8_t signature[64])
{
    return memcmp(signature + 32, binding_half_order,
                  sizeof(binding_half_order)) <= 0;
}

static bool binding_zcl_pubkey_valid(const uint8_t pubkey[33])
{
    secp256k1_pubkey parsed;
    return pubkey &&
           secp256k1_ec_pubkey_parse(binding_ctx, &parsed, pubkey, 33) == 1;
}

const char *vcs_zcode_binding_error_string(enum vcs_zcode_binding_error error)
{
    switch (error) {
    case VCS_ZCODE_BINDING_OK: return "ok";
    case VCS_ZCODE_BINDING_ERR_NULL: return "null-argument";
    case VCS_ZCODE_BINDING_ERR_VERSION: return "schema-version";
    case VCS_ZCODE_BINDING_ERR_WIRE_SIZE: return "wire-size";
    case VCS_ZCODE_BINDING_ERR_WIRE_MAGIC: return "wire-magic";
    case VCS_ZCODE_BINDING_ERR_ROOT_ZERO: return "root-zero";
    case VCS_ZCODE_BINDING_ERR_PUBKEY_ZERO: return "pubkey-zero";
    case VCS_ZCODE_BINDING_ERR_PUBKEY_INVALID: return "pubkey-invalid";
    case VCS_ZCODE_BINDING_ERR_KEY_ID_MISMATCH: return "key-id-mismatch";
    case VCS_ZCODE_BINDING_ERR_PREDECESSOR: return "predecessor-invalid";
    case VCS_ZCODE_BINDING_ERR_SEQUENCE: return "sequence-invalid";
    case VCS_ZCODE_BINDING_ERR_OPERATION: return "operation-invalid";
    case VCS_ZCODE_BINDING_ERR_TIME_ORDER: return "time-order-invalid";
    case VCS_ZCODE_BINDING_ERR_SIGNATURE: return "signature-invalid";
    case VCS_ZCODE_BINDING_ERR_KEY_MISMATCH: return "key-mismatch";
    case VCS_ZCODE_BINDING_ERR_NETWORK_MISMATCH:
        return "network-genesis-mismatch";
    case VCS_ZCODE_BINDING_ERR_IDENTITY_MISMATCH:
        return "identity-root-mismatch";
    case VCS_ZCODE_BINDING_ERR_EXPIRED: return "object-expired";
    case VCS_ZCODE_BINDING_ERR_REVOKED: return "binding-revoked-terminal";
    case VCS_ZCODE_BINDING_ERR_LINKAGE: return "successor-linkage-invalid";
    case VCS_ZCODE_BINDING_ERR_NOT_YET_VALID: return "not-yet-valid";
    }
    return "unknown";
}

static enum vcs_zcode_binding_error binding_fields(
    const struct vcs_zcode_contributor_binding_v1 *binding,
    bool require_signatures)
{
    if (!binding) return VCS_ZCODE_BINDING_ERR_NULL;
    if (binding->schema_version != VCS_ZCODE_CONTRIBUTOR_BINDING_VERSION)
        return VCS_ZCODE_BINDING_ERR_VERSION;
    if (!root_nonzero(binding->network_genesis_root))
        return VCS_ZCODE_BINDING_ERR_ROOT_ZERO;
    if (!root_nonzero(binding->zid_pubkey))
        return VCS_ZCODE_BINDING_ERR_PUBKEY_ZERO;
    if (binding->operation < VCS_ZCODE_BINDING_ACTIVE ||
        binding->operation > VCS_ZCODE_BINDING_REVOKE)
        return VCS_ZCODE_BINDING_ERR_OPERATION;
    if (binding->sequence == 0)
        return VCS_ZCODE_BINDING_ERR_SEQUENCE;
    if (binding->operation == VCS_ZCODE_BINDING_ACTIVE) {
        if (binding->sequence != 1)
            return VCS_ZCODE_BINDING_ERR_SEQUENCE;
        if (root_nonzero(binding->predecessor_root))
            return VCS_ZCODE_BINDING_ERR_PREDECESSOR;
    } else {
        if (binding->sequence == 1)
            return VCS_ZCODE_BINDING_ERR_SEQUENCE;
        if (!root_nonzero(binding->predecessor_root))
            return VCS_ZCODE_BINDING_ERR_PREDECESSOR;
    }
    /* The zcl key must be a real curve point and the key id its hash160.
     * REVOKE keeps the key it retires so the binding stays standalone
     * verifiable; the chain gate — not field validation — is what makes a
     * revoke terminal and bars an implicit replacement key. */
    if (!binding_zcl_pubkey_valid(binding->zcl_pubkey))
        return VCS_ZCODE_BINDING_ERR_PUBKEY_INVALID;
    uint8_t want_key_id[20];
    hash160(binding->zcl_pubkey, sizeof(binding->zcl_pubkey), want_key_id);
    if (memcmp(binding->zcl_key_id, want_key_id, sizeof(want_key_id)) != 0)
        return VCS_ZCODE_BINDING_ERR_KEY_ID_MISMATCH;
    if (binding->issued_unix <= 0 ||
        binding->expires_unix <= binding->issued_unix)
        return VCS_ZCODE_BINDING_ERR_TIME_ORDER;
    if (require_signatures) {
        if (!bytes_nonzero(binding->zid_signature,
                           sizeof(binding->zid_signature)))
            return VCS_ZCODE_BINDING_ERR_SIGNATURE;
        if (!bytes_nonzero(binding->zcl_signature,
                           sizeof(binding->zcl_signature)))
            return VCS_ZCODE_BINDING_ERR_SIGNATURE;
    }
    return VCS_ZCODE_BINDING_OK;
}

enum vcs_zcode_binding_error vcs_zcode_contributor_binding_validate(
    const struct vcs_zcode_contributor_binding_v1 *binding)
{
    return binding_fields(binding, true);
}

enum vcs_zcode_binding_error vcs_zcode_contributor_binding_validate_at(
    const struct vcs_zcode_contributor_binding_v1 *binding, int64_t now_unix)
{
    enum vcs_zcode_binding_error error =
        vcs_zcode_contributor_binding_validate(binding);
    if (error != VCS_ZCODE_BINDING_OK) return error;
    /* A binding is usable only inside [issued_unix, expires_unix): early use
     * is NOT_YET_VALID, use at or after expiry is EXPIRED. */
    if (now_unix < binding->issued_unix)
        return VCS_ZCODE_BINDING_ERR_NOT_YET_VALID;
    if (now_unix >= binding->expires_unix)
        return VCS_ZCODE_BINDING_ERR_EXPIRED;
    return VCS_ZCODE_BINDING_OK;
}

static enum vcs_zcode_binding_error binding_body(
    const struct vcs_zcode_contributor_binding_v1 *binding,
    uint8_t out[VCS_ZCODE_CONTRIBUTOR_BINDING_BODY_BYTES])
{
    enum vcs_zcode_binding_error error = binding_fields(binding, false);
    if (error != VCS_ZCODE_BINDING_OK || !out)
        return out ? error : VCS_ZCODE_BINDING_ERR_NULL;
    size_t off = 0;
    put_bytes(out, &off, binding_magic, sizeof(binding_magic));
    put_u16(out, &off, binding->schema_version);
    put_bytes(out, &off, binding->network_genesis_root, 32);
    put_bytes(out, &off, binding->zid_pubkey, 32);
    put_bytes(out, &off, binding->zcl_pubkey, 33);
    put_bytes(out, &off, binding->zcl_key_id, 20);
    put_bytes(out, &off, binding->predecessor_root, 32);
    put_u64(out, &off, binding->sequence);
    put_u64(out, &off, (uint64_t)binding->issued_unix);
    put_u64(out, &off, (uint64_t)binding->expires_unix);
    out[off++] = binding->operation;
    return off == VCS_ZCODE_CONTRIBUTOR_BINDING_BODY_BYTES
               ? VCS_ZCODE_BINDING_OK : VCS_ZCODE_BINDING_ERR_WIRE_SIZE;
}

enum vcs_zcode_binding_error vcs_zcode_contributor_binding_serialize(
    const struct vcs_zcode_contributor_binding_v1 *binding,
    uint8_t out[VCS_ZCODE_CONTRIBUTOR_BINDING_WIRE_BYTES])
{
    enum vcs_zcode_binding_error error =
        vcs_zcode_contributor_binding_validate(binding);
    if (error != VCS_ZCODE_BINDING_OK || !out)
        return out ? error : VCS_ZCODE_BINDING_ERR_NULL;
    error = binding_body(binding, out);
    if (error != VCS_ZCODE_BINDING_OK) return error;
    memcpy(out + VCS_ZCODE_CONTRIBUTOR_BINDING_BODY_BYTES,
           binding->zid_signature, 64);
    memcpy(out + VCS_ZCODE_CONTRIBUTOR_BINDING_BODY_BYTES + 64,
           binding->zcl_signature, 64);
    return VCS_ZCODE_BINDING_OK;
}

enum vcs_zcode_binding_error vcs_zcode_contributor_binding_parse(
    const uint8_t *wire, size_t len,
    struct vcs_zcode_contributor_binding_v1 *out)
{
    if (!wire || !out) return VCS_ZCODE_BINDING_ERR_NULL;
    memset(out, 0, sizeof(*out));
    if (len != VCS_ZCODE_CONTRIBUTOR_BINDING_WIRE_BYTES)
        return VCS_ZCODE_BINDING_ERR_WIRE_SIZE;
    if (memcmp(wire, binding_magic, sizeof(binding_magic)) != 0)
        return VCS_ZCODE_BINDING_ERR_WIRE_MAGIC;
    size_t off = sizeof(binding_magic);
    out->schema_version = get_u16(wire, &off);
    get_bytes(wire, &off, out->network_genesis_root, 32);
    get_bytes(wire, &off, out->zid_pubkey, 32);
    get_bytes(wire, &off, out->zcl_pubkey, 33);
    get_bytes(wire, &off, out->zcl_key_id, 20);
    get_bytes(wire, &off, out->predecessor_root, 32);
    out->sequence = get_u64(wire, &off);
    out->issued_unix = (int64_t)get_u64(wire, &off);
    out->expires_unix = (int64_t)get_u64(wire, &off);
    out->operation = wire[off++];
    get_bytes(wire, &off, out->zid_signature, 64);
    get_bytes(wire, &off, out->zcl_signature, 64);
    enum vcs_zcode_binding_error error =
        vcs_zcode_contributor_binding_validate(out);
    if (error != VCS_ZCODE_BINDING_OK) memset(out, 0, sizeof(*out));
    return error;
}

enum vcs_zcode_binding_error vcs_zcode_contributor_binding_body_root(
    const struct vcs_zcode_contributor_binding_v1 *binding, uint8_t out[32])
{
    uint8_t body[VCS_ZCODE_CONTRIBUTOR_BINDING_BODY_BYTES];
    enum vcs_zcode_binding_error error = binding_body(binding, body);
    if (error != VCS_ZCODE_BINDING_OK || !out)
        return out ? error : VCS_ZCODE_BINDING_ERR_NULL;
    static const char domain[] = VCS_ZCODE_CONTRIBUTOR_BINDING_DOMAIN;
    binding_root_hash(domain, sizeof(domain), body, sizeof(body), out);
    return VCS_ZCODE_BINDING_OK;
}

enum vcs_zcode_binding_error vcs_zcode_contributor_binding_root(
    const struct vcs_zcode_contributor_binding_v1 *binding, uint8_t out[32])
{
    uint8_t wire[VCS_ZCODE_CONTRIBUTOR_BINDING_WIRE_BYTES];
    enum vcs_zcode_binding_error error =
        vcs_zcode_contributor_binding_serialize(binding, wire);
    if (error != VCS_ZCODE_BINDING_OK || !out)
        return out ? error : VCS_ZCODE_BINDING_ERR_NULL;
    static const char domain[] = VCS_ZCODE_CONTRIBUTOR_BINDING_ROOT_DOMAIN;
    binding_root_hash(domain, sizeof(domain), wire, sizeof(wire), out);
    return VCS_ZCODE_BINDING_OK;
}

enum vcs_zcode_binding_error vcs_zcode_contributor_binding_seal(
    struct vcs_zcode_contributor_binding_v1 *binding,
    const uint8_t zid_secret[32], const uint8_t zid_pubkey[32],
    const uint8_t zcl_secret[32])
{
    if (!binding || !zid_secret || !zid_pubkey || !zcl_secret)
        return VCS_ZCODE_BINDING_ERR_NULL;
    if (!root_nonzero(zid_pubkey)) return VCS_ZCODE_BINDING_ERR_PUBKEY_ZERO;
    /* The ZID public key is re-derived from the supplied secret: a secret
     * that does not produce the claimed pubkey must never seal — the
     * resulting signature would be unverifiable garbage under either key. */
    uint8_t zid_derived_pk[32], zid_derived_sk[32];
    ed25519_keypair(zid_derived_pk, zid_derived_sk, zid_secret);
    if (memcmp(zid_derived_pk, zid_pubkey, 32) != 0)
        return VCS_ZCODE_BINDING_ERR_KEY_MISMATCH;
    if (memcmp(binding->zid_pubkey, zid_pubkey, 32) != 0)
        return VCS_ZCODE_BINDING_ERR_KEY_MISMATCH;

    /* The zcl secret must derive the embedded zcl key — a seal under a
     * different key would be an unverifiable binding. */
    secp256k1_pubkey derived;
    if (!secp256k1_ec_pubkey_create(binding_ctx, &derived, zcl_secret))
        return VCS_ZCODE_BINDING_ERR_PUBKEY_INVALID;
    uint8_t derived_ser[33];
    size_t derived_len = sizeof(derived_ser);
    (void)secp256k1_ec_pubkey_serialize(
        binding_ctx, derived_ser, &derived_len, &derived,
        SECP256K1_EC_COMPRESSED);
    if (derived_len != sizeof(binding->zcl_pubkey) ||
        memcmp(binding->zcl_pubkey, derived_ser,
               sizeof(binding->zcl_pubkey)) != 0)
        return VCS_ZCODE_BINDING_ERR_KEY_MISMATCH;

    enum vcs_zcode_binding_error error = binding_fields(binding, false);
    if (error != VCS_ZCODE_BINDING_OK) return error;

    uint8_t root[32];
    error = vcs_zcode_contributor_binding_body_root(binding, root);
    if (error != VCS_ZCODE_BINDING_OK) return error;

    ed25519_sign(binding->zid_signature, root, sizeof(root), zid_secret,
                 zid_pubkey);

    secp256k1_ecdsa_signature signature;
    if (!secp256k1_ecdsa_sign(binding_ctx, &signature, root, zcl_secret,
                              NULL, NULL))
        return VCS_ZCODE_BINDING_ERR_SIGNATURE;
    /* Canonicalize to low-S so the sealed wire is byte deterministic. */
    (void)secp256k1_ecdsa_signature_normalize(binding_ctx, &signature,
                                              &signature);
    (void)secp256k1_ecdsa_signature_serialize_compact(
        binding_ctx, binding->zcl_signature, &signature);
    return VCS_ZCODE_BINDING_OK;
}

/* Both signatures over the body root, no expiry gate: the ZID Ed25519
 * signature under the embedded zid_pubkey, then the low-S canonical
 * secp256k1 signature under the embedded zcl_pubkey. */
static enum vcs_zcode_binding_error binding_verify_sigs(
    const struct vcs_zcode_contributor_binding_v1 *binding)
{
    uint8_t root[32];
    enum vcs_zcode_binding_error error =
        vcs_zcode_contributor_binding_body_root(binding, root);
    if (error != VCS_ZCODE_BINDING_OK) return error;

    if (!ed25519_verify(binding->zid_signature, root, sizeof(root),
                        binding->zid_pubkey))
        return VCS_ZCODE_BINDING_ERR_SIGNATURE;

    if (!binding_signature_low_s(binding->zcl_signature))
        return VCS_ZCODE_BINDING_ERR_SIGNATURE;
    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_parse(binding_ctx, &pubkey, binding->zcl_pubkey,
                                   sizeof(binding->zcl_pubkey)))
        return VCS_ZCODE_BINDING_ERR_PUBKEY_INVALID;
    secp256k1_ecdsa_signature signature;
    (void)secp256k1_ecdsa_signature_parse_compact(binding_ctx, &signature,
                                                  binding->zcl_signature);
    if (!secp256k1_ecdsa_verify(binding_ctx, &signature, root, &pubkey))
        return VCS_ZCODE_BINDING_ERR_SIGNATURE;
    return VCS_ZCODE_BINDING_OK;
}

enum vcs_zcode_binding_error vcs_zcode_contributor_binding_verify(
    const struct vcs_zcode_contributor_binding_v1 *binding,
    const uint8_t expected_network_genesis[32],
    const uint8_t expected_zid_pubkey[32], int64_t now_unix)
{
    enum vcs_zcode_binding_error error =
        vcs_zcode_contributor_binding_validate_at(binding, now_unix);
    if (error != VCS_ZCODE_BINDING_OK) return error;
    if (!expected_network_genesis ||
        memcmp(binding->network_genesis_root, expected_network_genesis,
               32) != 0)
        return VCS_ZCODE_BINDING_ERR_NETWORK_MISMATCH;
    if (!expected_zid_pubkey ||
        memcmp(binding->zid_pubkey, expected_zid_pubkey, 32) != 0)
        return VCS_ZCODE_BINDING_ERR_IDENTITY_MISMATCH;
    return binding_verify_sigs(binding);
}

enum vcs_zcode_binding_error vcs_zcode_contributor_binding_validate_successor(
    const struct vcs_zcode_contributor_binding_v1 *prior,
    const struct vcs_zcode_contributor_binding_v1 *next, int64_t now_unix)
{
    if (!prior || !next) return VCS_ZCODE_BINDING_ERR_NULL;
    enum vcs_zcode_binding_error error =
        vcs_zcode_contributor_binding_validate(prior);
    if (error != VCS_ZCODE_BINDING_OK) return error;
    error = vcs_zcode_contributor_binding_validate_at(next, now_unix);
    if (error != VCS_ZCODE_BINDING_OK) return error;

    if (memcmp(prior->network_genesis_root, next->network_genesis_root,
               32) != 0)
        return VCS_ZCODE_BINDING_ERR_NETWORK_MISMATCH;
    if (memcmp(prior->zid_pubkey, next->zid_pubkey, 32) != 0)
        return VCS_ZCODE_BINDING_ERR_IDENTITY_MISMATCH;
    /* A revoked binding is terminal: revocation cannot create a replacement
     * key implicitly, and nothing may succeed it. */
    if (prior->operation == VCS_ZCODE_BINDING_REVOKE)
        return VCS_ZCODE_BINDING_ERR_REVOKED;
    /* A successor can only rotate or revoke — a fresh ACTIVE would fork the
     * chain back to sequence 1. */
    if (next->operation == VCS_ZCODE_BINDING_ACTIVE)
        return VCS_ZCODE_BINDING_ERR_OPERATION;

    uint8_t prior_root[32];
    error = vcs_zcode_contributor_binding_root(prior, prior_root);
    if (error != VCS_ZCODE_BINDING_OK) return error;
    if (memcmp(next->predecessor_root, prior_root, sizeof(prior_root)) != 0)
        return VCS_ZCODE_BINDING_ERR_PREDECESSOR;
    /* Exact +1 sequencing rejects both replay (same sequence) and skips. */
    if (next->sequence != prior->sequence + 1)
        return VCS_ZCODE_BINDING_ERR_SEQUENCE;
    /* Time must move forward along the chain: a successor issued at or
     * before its predecessor is a reordering, not a rotation. */
    if (next->issued_unix <= prior->issued_unix)
        return VCS_ZCODE_BINDING_ERR_TIME_ORDER;

    if (next->operation == VCS_ZCODE_BINDING_ROTATE) {
        if (memcmp(prior->zcl_pubkey, next->zcl_pubkey, 33) == 0)
            return VCS_ZCODE_BINDING_ERR_LINKAGE;
    } else { /* REVOKE retires the SAME key; it never names a replacement. */
        if (memcmp(prior->zcl_pubkey, next->zcl_pubkey, 33) != 0 ||
            memcmp(prior->zcl_key_id, next->zcl_key_id, 20) != 0)
            return VCS_ZCODE_BINDING_ERR_LINKAGE;
    }

    error = binding_verify_sigs(prior);
    if (error != VCS_ZCODE_BINDING_OK) return error;
    return binding_verify_sigs(next);
}
