/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: canonical simulation-only package continuity policies. */
#include "vcs/zcode_continuity_policy.h"

#include "base/checked.h"
#include "codec/cursor.h"
#include "crypto/ed25519.h"
#include "crypto/sha3.h"

#include <stdbool.h>
#include <string.h>

static const uint8_t continuity_magic[8] =
    {'Z','C','C','O','N','T','\r','\n'};

static bool continuity_nonzero(const uint8_t value[32])
{
    uint8_t any = 0;
    for (size_t i = 0; i < 32; i++) any |= value[i];
    return any != 0;
}

static void continuity_hash(const char *domain, size_t domain_len,
                            const uint8_t *wire, size_t wire_len,
                            uint8_t out[32])
{
    struct sha3_256_ctx sha;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)domain, domain_len);
    sha3_256_write(&sha, wire, wire_len);
    sha3_256_finalize(&sha, out);
}

const char *vcs_zcode_continuity_error_string(
    enum vcs_zcode_continuity_error error)
{
    switch (error) {
    case VCS_ZCODE_CONTINUITY_OK: return "ok";
    case VCS_ZCODE_CONTINUITY_NULL: return "null-argument";
    case VCS_ZCODE_CONTINUITY_WIRE_SIZE: return "wire-size";
    case VCS_ZCODE_CONTINUITY_MAGIC: return "wire-magic";
    case VCS_ZCODE_CONTINUITY_VERSION: return "schema-version";
    case VCS_ZCODE_CONTINUITY_EVENT_MASK: return "closed-event-mask";
    case VCS_ZCODE_CONTINUITY_FLAGS: return "authority-or-simulation-flags";
    case VCS_ZCODE_CONTINUITY_ROOT: return "required-root";
    case VCS_ZCODE_CONTINUITY_TRANSITION: return "capsule-transition";
    case VCS_ZCODE_CONTINUITY_CAP: return "cycle-or-amount-cap";
    case VCS_ZCODE_CONTINUITY_TIME: return "time-order";
    case VCS_ZCODE_CONTINUITY_SEQUENCE: return "sequence";
    case VCS_ZCODE_CONTINUITY_SIGNATURE: return "signature";
    }
    return "unknown";
}

static enum vcs_zcode_continuity_error continuity_fields(
    const struct vcs_zcode_continuity_policy_v1 *policy, bool signed_wire)
{
    if (!policy) return VCS_ZCODE_CONTINUITY_NULL;
    if (policy->schema_version != VCS_ZCODE_CONTINUITY_POLICY_VERSION)
        return VCS_ZCODE_CONTINUITY_VERSION;
    if (policy->event_mask == 0 ||
        (policy->event_mask & ~VCS_ZCODE_CONTINUITY_ALLOWED_EVENT_MASK) != 0)
        return VCS_ZCODE_CONTINUITY_EVENT_MASK;
    uint8_t required = VCS_ZCODE_CONTINUITY_NO_AUTHORITY |
                       VCS_ZCODE_CONTINUITY_SIMULATION_ONLY;
    uint8_t allowed = required | VCS_ZCODE_CONTINUITY_ANONYMOUS_DISPLAY;
    if ((policy->flags & required) != required ||
        (policy->flags & ~allowed) != 0)
        return VCS_ZCODE_CONTINUITY_FLAGS;
    const uint8_t *roots[] = {
        policy->network_genesis_root,
        policy->zc23_token_or_simulation_root,
        policy->patron_contributor_binding_root, policy->patron_zid_pubkey,
        policy->package_root, policy->current_release_root,
        policy->from_capsule_root, policy->to_capsule_root,
        policy->proof_policy_root,
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        if (!continuity_nonzero(roots[i]))
            return VCS_ZCODE_CONTINUITY_ROOT;
    if (memcmp(policy->from_capsule_root, policy->to_capsule_root, 32) == 0)
        return VCS_ZCODE_CONTINUITY_TRANSITION;
    uint64_t maximum_total = 0;
    if (policy->maximum_cycles == 0 || policy->per_cycle_cap_atoms == 0 ||
        policy->total_cap_atoms < policy->per_cycle_cap_atoms ||
        !zcl_u64_mul(policy->maximum_cycles, policy->per_cycle_cap_atoms,
                     &maximum_total) ||
        policy->total_cap_atoms > maximum_total)
        return VCS_ZCODE_CONTINUITY_CAP;
    if (policy->created_unix <= 0 ||
        policy->expires_unix <= policy->created_unix)
        return VCS_ZCODE_CONTINUITY_TIME;
    if (policy->sequence == 0) return VCS_ZCODE_CONTINUITY_SEQUENCE;
    if (signed_wire && !continuity_nonzero(policy->signature))
        return VCS_ZCODE_CONTINUITY_SIGNATURE;
    return VCS_ZCODE_CONTINUITY_OK;
}

enum vcs_zcode_continuity_error vcs_zcode_continuity_policy_validate(
    const struct vcs_zcode_continuity_policy_v1 *policy)
{
    return continuity_fields(policy, true);
}

static enum vcs_zcode_continuity_error continuity_body(
    const struct vcs_zcode_continuity_policy_v1 *policy,
    uint8_t out[VCS_ZCODE_CONTINUITY_POLICY_BODY_BYTES])
{
    enum vcs_zcode_continuity_error error =
        continuity_fields(policy, false);
    if (error != VCS_ZCODE_CONTINUITY_OK || !out)
        return out ? error : VCS_ZCODE_CONTINUITY_NULL;
    struct zcl_codec_writer writer;
    zcl_codec_writer_init(&writer, out,
                          VCS_ZCODE_CONTINUITY_POLICY_BODY_BYTES);
    bool ok = zcl_codec_write_bytes(&writer, continuity_magic, 8) &&
        zcl_codec_write_u16le(&writer, policy->schema_version) &&
        zcl_codec_write_u16le(&writer, policy->event_mask) &&
        zcl_codec_write_u8(&writer, policy->flags) &&
        zcl_codec_write_u8(&writer, 0) &&
        zcl_codec_write_u16le(&writer, 0) &&
        zcl_codec_write_bytes(&writer, policy->network_genesis_root, 32) &&
        zcl_codec_write_bytes(&writer,
            policy->zc23_token_or_simulation_root, 32) &&
        zcl_codec_write_bytes(&writer,
            policy->patron_contributor_binding_root, 32) &&
        zcl_codec_write_bytes(&writer, policy->patron_zid_pubkey, 32) &&
        zcl_codec_write_bytes(&writer, policy->package_root, 32) &&
        zcl_codec_write_bytes(&writer, policy->current_release_root, 32) &&
        zcl_codec_write_bytes(&writer, policy->from_capsule_root, 32) &&
        zcl_codec_write_bytes(&writer, policy->to_capsule_root, 32) &&
        zcl_codec_write_bytes(&writer, policy->proof_policy_root, 32) &&
        zcl_codec_write_u32le(&writer, policy->maximum_cycles) &&
        zcl_codec_write_u32le(&writer, 0) &&
        zcl_codec_write_u64le(&writer, policy->per_cycle_cap_atoms) &&
        zcl_codec_write_u64le(&writer, policy->total_cap_atoms) &&
        zcl_codec_write_i64le(&writer, policy->created_unix) &&
        zcl_codec_write_i64le(&writer, policy->expires_unix) &&
        zcl_codec_write_u64le(&writer, policy->sequence);
    size_t written = 0;
    return ok && zcl_codec_writer_finish(&writer, &written) &&
           written == VCS_ZCODE_CONTINUITY_POLICY_BODY_BYTES
        ? VCS_ZCODE_CONTINUITY_OK : VCS_ZCODE_CONTINUITY_WIRE_SIZE;
}

static enum vcs_zcode_continuity_error continuity_signing_root(
    const struct vcs_zcode_continuity_policy_v1 *policy, uint8_t out[32])
{
    if (out) memset(out, 0, 32);
    if (!out) return VCS_ZCODE_CONTINUITY_NULL;
    uint8_t body[VCS_ZCODE_CONTINUITY_POLICY_BODY_BYTES];
    enum vcs_zcode_continuity_error error = continuity_body(policy, body);
    if (error != VCS_ZCODE_CONTINUITY_OK) return error;
    static const char domain[] = VCS_ZCODE_CONTINUITY_POLICY_DOMAIN;
    continuity_hash(domain, sizeof(domain), body, sizeof(body), out);
    return VCS_ZCODE_CONTINUITY_OK;
}

enum vcs_zcode_continuity_error vcs_zcode_continuity_policy_serialize(
    const struct vcs_zcode_continuity_policy_v1 *policy,
    uint8_t out[VCS_ZCODE_CONTINUITY_POLICY_WIRE_BYTES])
{
    if (!out) return VCS_ZCODE_CONTINUITY_NULL;
    enum vcs_zcode_continuity_error error = continuity_fields(policy, true);
    if (error != VCS_ZCODE_CONTINUITY_OK) return error;
    error = continuity_body(policy, out);
    if (error == VCS_ZCODE_CONTINUITY_OK)
        memcpy(out + VCS_ZCODE_CONTINUITY_POLICY_BODY_BYTES,
               policy->signature, 64);
    return error;
}

enum vcs_zcode_continuity_error vcs_zcode_continuity_policy_parse(
    const uint8_t *wire, size_t wire_len,
    struct vcs_zcode_continuity_policy_v1 *out)
{
    if (!wire || !out) return VCS_ZCODE_CONTINUITY_NULL;
    memset(out, 0, sizeof(*out));
    if (wire_len != VCS_ZCODE_CONTINUITY_POLICY_WIRE_BYTES)
        return VCS_ZCODE_CONTINUITY_WIRE_SIZE;
    struct zcl_codec_reader reader;
    uint8_t magic[8], reserved8 = 0;
    uint16_t reserved16 = 0;
    uint32_t reserved32 = 0;
    zcl_codec_reader_init(&reader, wire, wire_len);
    bool ok = zcl_codec_read_bytes(&reader, magic, 8) &&
        zcl_codec_read_u16le(&reader, &out->schema_version) &&
        zcl_codec_read_u16le(&reader, &out->event_mask) &&
        zcl_codec_read_u8(&reader, &out->flags) &&
        zcl_codec_read_u8(&reader, &reserved8) &&
        zcl_codec_read_u16le(&reader, &reserved16) &&
        zcl_codec_read_bytes(&reader, out->network_genesis_root, 32) &&
        zcl_codec_read_bytes(&reader,
            out->zc23_token_or_simulation_root, 32) &&
        zcl_codec_read_bytes(&reader,
            out->patron_contributor_binding_root, 32) &&
        zcl_codec_read_bytes(&reader, out->patron_zid_pubkey, 32) &&
        zcl_codec_read_bytes(&reader, out->package_root, 32) &&
        zcl_codec_read_bytes(&reader, out->current_release_root, 32) &&
        zcl_codec_read_bytes(&reader, out->from_capsule_root, 32) &&
        zcl_codec_read_bytes(&reader, out->to_capsule_root, 32) &&
        zcl_codec_read_bytes(&reader, out->proof_policy_root, 32) &&
        zcl_codec_read_u32le(&reader, &out->maximum_cycles) &&
        zcl_codec_read_u32le(&reader, &reserved32) &&
        zcl_codec_read_u64le(&reader, &out->per_cycle_cap_atoms) &&
        zcl_codec_read_u64le(&reader, &out->total_cap_atoms) &&
        zcl_codec_read_i64le(&reader, &out->created_unix) &&
        zcl_codec_read_i64le(&reader, &out->expires_unix) &&
        zcl_codec_read_u64le(&reader, &out->sequence) &&
        zcl_codec_read_bytes(&reader, out->signature, 64) &&
        zcl_codec_reader_finish(&reader);
    enum vcs_zcode_continuity_error error = !ok
        ? VCS_ZCODE_CONTINUITY_WIRE_SIZE
        : memcmp(magic, continuity_magic, 8) != 0
            ? VCS_ZCODE_CONTINUITY_MAGIC
            : reserved8 || reserved16 || reserved32
                ? VCS_ZCODE_CONTINUITY_FLAGS
                : continuity_fields(out, true);
    if (error != VCS_ZCODE_CONTINUITY_OK) memset(out, 0, sizeof(*out));
    return error;
}

enum vcs_zcode_continuity_error vcs_zcode_continuity_policy_root(
    const struct vcs_zcode_continuity_policy_v1 *policy, uint8_t out[32])
{
    if (out) memset(out, 0, 32);
    if (!out) return VCS_ZCODE_CONTINUITY_NULL;
    uint8_t wire[VCS_ZCODE_CONTINUITY_POLICY_WIRE_BYTES];
    enum vcs_zcode_continuity_error error =
        vcs_zcode_continuity_policy_serialize(policy, wire);
    if (error != VCS_ZCODE_CONTINUITY_OK) return error;
    static const char domain[] = VCS_ZCODE_CONTINUITY_POLICY_ROOT_DOMAIN;
    continuity_hash(domain, sizeof(domain), wire, sizeof(wire), out);
    return VCS_ZCODE_CONTINUITY_OK;
}

enum vcs_zcode_continuity_error vcs_zcode_continuity_policy_seal(
    struct vcs_zcode_continuity_policy_v1 *policy,
    const uint8_t secret[32], const uint8_t pubkey[32])
{
    if (!policy || !secret || !pubkey) return VCS_ZCODE_CONTINUITY_NULL;
    if (memcmp(policy->patron_zid_pubkey, pubkey, 32) != 0)
        return VCS_ZCODE_CONTINUITY_SIGNATURE;
    uint8_t root[32];
    enum vcs_zcode_continuity_error error =
        continuity_signing_root(policy, root);
    if (error != VCS_ZCODE_CONTINUITY_OK) return error;
    ed25519_sign(policy->signature, root, sizeof(root), secret, pubkey);
    return VCS_ZCODE_CONTINUITY_OK;
}

enum vcs_zcode_continuity_error vcs_zcode_continuity_policy_verify(
    const struct vcs_zcode_continuity_policy_v1 *policy, int64_t now_unix)
{
    enum vcs_zcode_continuity_error error = continuity_fields(policy, true);
    if (error != VCS_ZCODE_CONTINUITY_OK) return error;
    if (now_unix < policy->created_unix || now_unix >= policy->expires_unix)
        return VCS_ZCODE_CONTINUITY_TIME;
    uint8_t root[32];
    error = continuity_signing_root(policy, root);
    if (error != VCS_ZCODE_CONTINUITY_OK) return error;
    return ed25519_verify(policy->signature, root, sizeof(root),
                          policy->patron_zid_pubkey)
        ? VCS_ZCODE_CONTINUITY_OK : VCS_ZCODE_CONTINUITY_SIGNATURE;
}
