/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Canonical dual-signed ZCODE contributor-binding wire
 * (contributor_binding.v1, ZCODE Scientific Metaverse slice S2).
 *
 * A contributor_binding binds an existing ZID Ed25519 identity to a fresh
 * ZCL secp256k1 address/key on one network. It is not a second identity
 * system: the ZID side reuses the ZID master Ed25519 key, the ZCL side
 * reuses the wallet secp256k1 key/address primitives, and the same SHA3-256
 * domain-separated root conventions as the rest of lib/vcs.
 *
 * Wire layout (exact, fixed width, little-endian integers):
 *   body (184 bytes):
 *     magic                       8   {'Z','C','B','I','N','D','\r','\n'}
 *     schema_version              2   == VCS_ZCODE_CONTRIBUTOR_BINDING_VERSION
 *     network_genesis_root       32   chain the binding lives on
 *     zid_pubkey                 32   ZID master Ed25519 public key
 *     zcl_pubkey                 33   compressed secp256k1 public key
 *     zcl_key_id                 20   hash160(zcl_pubkey), the ZCL address hash
 *     predecessor_root           32   full root of the prior binding, zero for
 *                                     ACTIVE
 *     sequence                    8   1 for ACTIVE, prior+1 for successors
 *     issued_unix                 8   > 0
 *     expires_unix                8   > issued_unix
 *     operation                   1   ACTIVE=1, ROTATE=2, REVOKE=3
 *   zid_signature                64   Ed25519 over body_root by zid_pubkey
 *   zcl_signature                64   secp256k1 r||s low-S over body_root
 * Total wire: 312 bytes.
 *
 * body_root = SHA3-256("zcl.zcode.contributor_binding.v1" || NUL || body) is
 * the exact statement both keys sign. The binding's own root — what a
 * successor's predecessor_root references — commits the full wire including
 * both signatures:
 *   root = SHA3-256("zcl.zcode.contributor_binding.root.v1" || NUL || wire).
 *
 * Semantics:
 *   ACTIVE opens a chain (sequence 1, zero predecessor).
 *   ROTATE carries a NEW zcl key and points at the prior binding.
 *   REVOKE carries the SAME zcl key it retires (so it stays standalone
 *   verifiable), points at the prior binding, and is terminal: revocation
 *   cannot create a replacement key implicitly, and no successor may
 *   reference a revoked binding. Both rules are enforced by
 *   vcs_zcode_contributor_binding_validate_successor().
 */

#ifndef ZCL_VCS_ZCODE_CONTRIBUTOR_BINDING_H
#define ZCL_VCS_ZCODE_CONTRIBUTOR_BINDING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VCS_ZCODE_CONTRIBUTOR_BINDING_VERSION 1u
#define VCS_ZCODE_CONTRIBUTOR_BINDING_DOMAIN "zcl.zcode.contributor_binding.v1"
#define VCS_ZCODE_CONTRIBUTOR_BINDING_ROOT_DOMAIN \
    "zcl.zcode.contributor_binding.root.v1"

#define VCS_ZCODE_CONTRIBUTOR_BINDING_BODY_BYTES 184u
#define VCS_ZCODE_CONTRIBUTOR_BINDING_WIRE_BYTES 312u

enum vcs_zcode_binding_operation {
    VCS_ZCODE_BINDING_ACTIVE = 1,
    VCS_ZCODE_BINDING_ROTATE = 2,
    VCS_ZCODE_BINDING_REVOKE = 3,
};

enum vcs_zcode_binding_error {
    VCS_ZCODE_BINDING_OK = 0,
    VCS_ZCODE_BINDING_ERR_NULL,
    VCS_ZCODE_BINDING_ERR_VERSION,
    VCS_ZCODE_BINDING_ERR_WIRE_SIZE,
    VCS_ZCODE_BINDING_ERR_WIRE_MAGIC,
    VCS_ZCODE_BINDING_ERR_ROOT_ZERO,
    VCS_ZCODE_BINDING_ERR_PUBKEY_ZERO,
    VCS_ZCODE_BINDING_ERR_PUBKEY_INVALID,
    VCS_ZCODE_BINDING_ERR_KEY_ID_MISMATCH,
    VCS_ZCODE_BINDING_ERR_PREDECESSOR,
    VCS_ZCODE_BINDING_ERR_SEQUENCE,
    VCS_ZCODE_BINDING_ERR_OPERATION,
    VCS_ZCODE_BINDING_ERR_TIME_ORDER,
    VCS_ZCODE_BINDING_ERR_SIGNATURE,
    VCS_ZCODE_BINDING_ERR_KEY_MISMATCH,
    VCS_ZCODE_BINDING_ERR_NETWORK_MISMATCH,
    VCS_ZCODE_BINDING_ERR_IDENTITY_MISMATCH,
    VCS_ZCODE_BINDING_ERR_EXPIRED,
    VCS_ZCODE_BINDING_ERR_REVOKED,
    VCS_ZCODE_BINDING_ERR_LINKAGE,
};

const char *vcs_zcode_binding_error_string(enum vcs_zcode_binding_error error);

struct vcs_zcode_contributor_binding_v1 {
    uint16_t schema_version;
    uint8_t network_genesis_root[32];
    uint8_t zid_pubkey[32];
    uint8_t zcl_pubkey[33];
    uint8_t zcl_key_id[20];
    uint8_t predecessor_root[32];
    uint64_t sequence;
    int64_t issued_unix;
    int64_t expires_unix;
    uint8_t operation;
    uint8_t zid_signature[64];
    uint8_t zcl_signature[64];
};

/* Structural validation. validate() requires both signatures to be
 * non-zero; validate_at() additionally rejects a binding whose expiry has
 * passed at now_unix. Neither checks the signatures cryptographically —
 * that is verify()'s job. */
enum vcs_zcode_binding_error vcs_zcode_contributor_binding_validate(
    const struct vcs_zcode_contributor_binding_v1 *binding);
enum vcs_zcode_binding_error vcs_zcode_contributor_binding_validate_at(
    const struct vcs_zcode_contributor_binding_v1 *binding, int64_t now_unix);

enum vcs_zcode_binding_error vcs_zcode_contributor_binding_serialize(
    const struct vcs_zcode_contributor_binding_v1 *binding,
    uint8_t out[VCS_ZCODE_CONTRIBUTOR_BINDING_WIRE_BYTES]);
/* Exact-size only: a short or trailing wire is WIRE_SIZE, a wrong leading
 * magic is WIRE_MAGIC, an unsupported schema_version is VERSION. On any
 * error *out is zeroed. */
enum vcs_zcode_binding_error vcs_zcode_contributor_binding_parse(
    const uint8_t *wire, size_t wire_len,
    struct vcs_zcode_contributor_binding_v1 *out);

/* The 32-byte statement both signatures sign (body only). */
enum vcs_zcode_binding_error vcs_zcode_contributor_binding_body_root(
    const struct vcs_zcode_contributor_binding_v1 *binding, uint8_t out[32]);
/* The binding's own id: commits the full signed wire. A successor's
 * predecessor_root must equal this value for its predecessor. */
enum vcs_zcode_binding_error vcs_zcode_contributor_binding_root(
    const struct vcs_zcode_contributor_binding_v1 *binding, uint8_t out[32]);

/* Sign the body root with both keys. binding->zid_pubkey must already equal
 * zid_pubkey and, for ACTIVE/ROTATE, binding->zcl_pubkey must be the key
 * derived from zcl_secret (KEY_MISMATCH otherwise). The secp256k1 signature
 * is normalized to low-S before serialization, so sealing is byte
 * deterministic. */
enum vcs_zcode_binding_error vcs_zcode_contributor_binding_seal(
    struct vcs_zcode_contributor_binding_v1 *binding,
    const uint8_t zid_secret[32], const uint8_t zid_pubkey[32],
    const uint8_t zcl_secret[32]);

/* Full verification: structural validity at now_unix, the expected network
 * genesis root and ZID master key are pinned, and BOTH signatures verify
 * over the body root (the secp256k1 signature must be low-S canonical and
 * verify under the embedded zcl_pubkey). */
enum vcs_zcode_binding_error vcs_zcode_contributor_binding_verify(
    const struct vcs_zcode_contributor_binding_v1 *binding,
    const uint8_t expected_network_genesis[32],
    const uint8_t expected_zid_pubkey[32], int64_t now_unix);

/* Chain gate: next is a valid successor of prior. Requires prior and next
 * on the same network and ZID; prior not revoked (revocation is terminal);
 * next not ACTIVE; next->predecessor_root == root(prior);
 * next->sequence == prior->sequence + 1 (replay and skips rejected);
 * ROTATE must change the zcl key; REVOKE must keep it (no implicit
 * replacement); both bindings' signatures verify (prior structurally,
 * next at now_unix). */
enum vcs_zcode_binding_error vcs_zcode_contributor_binding_validate_successor(
    const struct vcs_zcode_contributor_binding_v1 *prior,
    const struct vcs_zcode_contributor_binding_v1 *next, int64_t now_unix);

#endif /* ZCL_VCS_ZCODE_CONTRIBUTOR_BINDING_H */
