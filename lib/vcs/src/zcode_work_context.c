/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Canonical content.v2 carrier for one fixed ZCODE build action. */

#include "vcs/zcode_work_context.h"

#include "vcs_priv.h"

#include "crypto/sha3.h"
#include "util/safe_alloc.h"
#include "vcs/build_action.h"
#include "vcs/package_manifest.h"
#include "vcs/package_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t context_magic[8] = {
    'Z', 'C', 'C', 'T', 'X', '\r', '\n', 0
};

const char *vcs_zcode_work_context_result_string(
    enum vcs_zcode_work_context_result result)
{
    switch (result) {
    case VCS_ZCODE_WORK_CONTEXT_OK: return "ok";
    case VCS_ZCODE_WORK_CONTEXT_NULL: return "null-argument";
    case VCS_ZCODE_WORK_CONTEXT_SHAPE: return "noncanonical-context";
    case VCS_ZCODE_WORK_CONTEXT_LIMIT: return "context-limit";
    case VCS_ZCODE_WORK_CONTEXT_STALE: return "stale-context";
    case VCS_ZCODE_WORK_CONTEXT_ACTION: return "action-mismatch";
    case VCS_ZCODE_WORK_CONTEXT_STORE: return "package-store-refused";
    case VCS_ZCODE_WORK_CONTEXT_ABSENT: return "context-absent";
    case VCS_ZCODE_WORK_CONTEXT_CORRUPT: return "context-corrupt";
    case VCS_ZCODE_WORK_CONTEXT_ALLOC: return "allocation-failed";
    }
    return "unknown";
}

void vcs_zcode_work_context_init(struct vcs_zcode_work_context_v1 *context)
{
    if (context) memset(context, 0, sizeof(*context));
}

void vcs_zcode_work_context_free(struct vcs_zcode_work_context_v1 *context)
{
    if (!context) return;
    free(context->fixed_input);
    vcs_zcode_work_context_init(context);
}

static bool context_nonzero(const uint8_t *bytes, size_t len)
{
    uint8_t any = 0;
    for (size_t i = 0; i < len; i++) any |= bytes[i];
    return any != 0;
}

static enum vcs_zcode_work_context_result context_validate(
    const struct vcs_zcode_work_context_v1 *context, int64_t now_unix)
{
    if (!context || !context->fixed_input)
        return VCS_ZCODE_WORK_CONTEXT_NULL;
    size_t profile_len = strnlen(context->profile, sizeof(context->profile));
    if (!context_nonzero(context->source_sha256, 32) || profile_len == 0 ||
        profile_len > VCS_ZCODE_WORK_CONTEXT_PROFILE_MAX)
        return VCS_ZCODE_WORK_CONTEXT_SHAPE;
    if (context->fixed_input_len == 0 ||
        context->fixed_input_len > context->task.max_context_bytes ||
        context->fixed_input_len >
            VCS_PACKAGE_STORE_MAX_PACKAGE_BYTES -
                VCS_ZCODE_WORK_CONTEXT_FIXED_BYTES - profile_len)
        return VCS_ZCODE_WORK_CONTEXT_LIMIT;
    if (vcs_zcode_task_validate_at(&context->task, now_unix) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_candidate_validate_for_task(
            &context->task, &context->candidate, now_unix) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_proof_policy_validate(&context->proof_policy) !=
            VCS_ZCODE_DEV_OK)
        return VCS_ZCODE_WORK_CONTEXT_STALE;
    uint8_t policy_root[32];
    if (vcs_zcode_proof_policy_root(&context->proof_policy, policy_root) !=
            VCS_ZCODE_DEV_OK ||
        memcmp(policy_root, context->task.proof_policy_root, 32) != 0)
        return VCS_ZCODE_WORK_CONTEXT_STALE;
    return VCS_ZCODE_WORK_CONTEXT_OK;
}

enum vcs_zcode_work_context_result vcs_zcode_work_context_action_root_for_kind(
    const struct vcs_zcode_work_context_v1 *context, const char *kind,
    int64_t now_unix, uint8_t action_root[32], uint8_t input_root[32])
{
    if (!action_root || !input_root)
        return VCS_ZCODE_WORK_CONTEXT_NULL;
    enum vcs_zcode_work_context_result valid =
        context_validate(context, now_unix);
    if (valid != VCS_ZCODE_WORK_CONTEXT_OK) return valid;
    struct vcs_build_action_v1 action = {0};
    memcpy(action.source_sha256, context->source_sha256, 32);
    memcpy(action.source_cas_sha3, context->candidate.candidate_source_root,
           32);
    sha3_256(context->fixed_input, context->fixed_input_len, input_root);
    memcpy(action.input_root_sha3, input_root, 32);
    memcpy(action.toolchain_capsule_sha3,
           context->task.toolchain_capsule_root, 32);
    const char *workdir = NULL, *output = NULL, *resource = NULL;
    if (!vcs_build_action_v1_descriptors(
            kind, &workdir, &output, &resource) ||
        !vcs_build_action_v1_fixed_flags_root_for_kind(
            kind, action.flags_sha3) ||
        !vcs_build_action_v1_fixed_environment_root_for_kind(
            kind, action.environment_sha3))
        return VCS_ZCODE_WORK_CONTEXT_ACTION;
    (void)snprintf(action.target, sizeof(action.target), "%s",
                   VCS_BUILD_TARGET_V1);
    (void)snprintf(action.profile, sizeof(action.profile), "%s",
                   context->profile);
    (void)snprintf(action.virtual_workdir, sizeof(action.virtual_workdir),
                   "%s", workdir);
    (void)snprintf(action.declared_outputs, sizeof(action.declared_outputs),
                   "%s", output);
    (void)snprintf(action.resource_policy, sizeof(action.resource_policy),
                   "%s", resource);
    if (!vcs_build_action_v1_root_for_kind(kind, &action, action_root))
        return VCS_ZCODE_WORK_CONTEXT_ACTION;
    return VCS_ZCODE_WORK_CONTEXT_OK;
}

enum vcs_zcode_work_context_result vcs_zcode_work_context_action_root(
    const struct vcs_zcode_work_context_v1 *context, int64_t now_unix,
    uint8_t action_root[32], uint8_t input_root[32])
{
    return vcs_zcode_work_context_action_root_for_kind(
        context, VCS_BUILD_ACTION_KIND_V1, now_unix, action_root,
        input_root);
}

enum vcs_zcode_work_context_result vcs_zcode_work_context_serialize(
    const struct vcs_zcode_work_context_v1 *context, int64_t now_unix,
    uint8_t **out, size_t *out_len)
{
    if (!out || !out_len) return VCS_ZCODE_WORK_CONTEXT_NULL;
    *out = NULL; *out_len = 0;
    enum vcs_zcode_work_context_result valid =
        context_validate(context, now_unix);
    if (valid != VCS_ZCODE_WORK_CONTEXT_OK) return valid;
    size_t profile_len = strlen(context->profile);
    size_t total = VCS_ZCODE_WORK_CONTEXT_FIXED_BYTES + profile_len +
                   context->fixed_input_len;
    uint8_t *wire = zcl_malloc(total, "zcode.work_context");
    if (!wire) return VCS_ZCODE_WORK_CONTEXT_ALLOC;
    size_t off = 0;
    memcpy(wire + off, context_magic, sizeof(context_magic)); off += 8;
    vcs_wr_u16le(wire + off, VCS_ZCODE_WORK_CONTEXT_VERSION); off += 2;
    vcs_wr_u16le(wire + off, (uint16_t)profile_len); off += 2;
    vcs_wr_u32le(wire + off, 0); off += 4;
    vcs_wr_u64le(wire + off, (uint64_t)context->fixed_input_len); off += 8;
    memcpy(wire + off, context->source_sha256, 32); off += 32;
    if (vcs_zcode_task_serialize(&context->task, wire + off) !=
            VCS_ZCODE_DEV_OK) goto reject;
    off += VCS_ZCODE_TASK_WIRE_BYTES;
    if (vcs_zcode_candidate_serialize(&context->candidate, wire + off) !=
            VCS_ZCODE_DEV_OK) goto reject;
    off += VCS_ZCODE_CANDIDATE_WIRE_BYTES;
    if (vcs_zcode_proof_policy_serialize(
            &context->proof_policy, wire + off) != VCS_ZCODE_DEV_OK)
        goto reject;
    off += VCS_ZCODE_PROOF_POLICY_WIRE_BYTES;
    memcpy(wire + off, context->profile, profile_len); off += profile_len;
    memcpy(wire + off, context->fixed_input,
           context->fixed_input_len); off += context->fixed_input_len;
    if (off != total) goto reject;
    *out = wire; *out_len = total;
    return VCS_ZCODE_WORK_CONTEXT_OK;
reject:
    free(wire);
    return VCS_ZCODE_WORK_CONTEXT_SHAPE;
}

enum vcs_zcode_work_context_result vcs_zcode_work_context_parse(
    const uint8_t *wire, size_t wire_len, int64_t now_unix,
    struct vcs_zcode_work_context_v1 *out)
{
    if (!wire || !out) return VCS_ZCODE_WORK_CONTEXT_NULL;
    vcs_zcode_work_context_init(out);
    if (wire_len < VCS_ZCODE_WORK_CONTEXT_FIXED_BYTES ||
        wire_len > VCS_PACKAGE_STORE_MAX_PACKAGE_BYTES ||
        memcmp(wire, context_magic, sizeof(context_magic)) != 0 ||
        vcs_rd_u16le(wire + 8) != VCS_ZCODE_WORK_CONTEXT_VERSION ||
        vcs_rd_u32le(wire + 12) != 0)
        return VCS_ZCODE_WORK_CONTEXT_SHAPE;
    uint16_t profile_len = vcs_rd_u16le(wire + 10);
    uint64_t input_len64 = vcs_rd_u64le(wire + 16);
    if (profile_len == 0 || profile_len > VCS_ZCODE_WORK_CONTEXT_PROFILE_MAX ||
        input_len64 == 0 || input_len64 > SIZE_MAX ||
        (uint64_t)VCS_ZCODE_WORK_CONTEXT_FIXED_BYTES + profile_len +
                input_len64 != wire_len)
        return VCS_ZCODE_WORK_CONTEXT_SHAPE;
    size_t off = 24;
    memcpy(out->source_sha256, wire + off, 32); off += 32;
    if (vcs_zcode_task_parse(wire + off, VCS_ZCODE_TASK_WIRE_BYTES,
                             &out->task) != VCS_ZCODE_DEV_OK)
        goto reject;
    off += VCS_ZCODE_TASK_WIRE_BYTES;
    if (vcs_zcode_candidate_parse(wire + off,
            VCS_ZCODE_CANDIDATE_WIRE_BYTES, &out->candidate) !=
            VCS_ZCODE_DEV_OK) goto reject;
    off += VCS_ZCODE_CANDIDATE_WIRE_BYTES;
    if (vcs_zcode_proof_policy_parse(wire + off,
            VCS_ZCODE_PROOF_POLICY_WIRE_BYTES, &out->proof_policy) !=
            VCS_ZCODE_DEV_OK) goto reject;
    off += VCS_ZCODE_PROOF_POLICY_WIRE_BYTES;
    memcpy(out->profile, wire + off, profile_len); off += profile_len;
    out->profile[profile_len] = '\0';
    out->fixed_input_len = (size_t)input_len64;
    out->fixed_input = zcl_malloc(out->fixed_input_len,
                                   "zcode.work_context.input");
    if (!out->fixed_input) {
        vcs_zcode_work_context_free(out);
        return VCS_ZCODE_WORK_CONTEXT_ALLOC;
    }
    memcpy(out->fixed_input, wire + off, out->fixed_input_len);
    enum vcs_zcode_work_context_result valid =
        context_validate(out, now_unix);
    if (valid != VCS_ZCODE_WORK_CONTEXT_OK) {
        vcs_zcode_work_context_free(out);
        return valid;
    }
    return VCS_ZCODE_WORK_CONTEXT_OK;
reject:
    vcs_zcode_work_context_free(out);
    return VCS_ZCODE_WORK_CONTEXT_SHAPE;
}

enum vcs_zcode_work_context_result vcs_zcode_work_context_put_for_kind(
    struct vcs_package_store *store,
    const struct vcs_zcode_work_context_v1 *context, const char *kind,
    int64_t now_unix, uint8_t package_root[32], uint8_t action_root[32])
{
    if (!store || !package_root || !action_root)
        return VCS_ZCODE_WORK_CONTEXT_NULL;
    uint8_t input_root[32];
    enum vcs_zcode_work_context_result result =
        vcs_zcode_work_context_action_root_for_kind(
            context, kind, now_unix, action_root, input_root);
    if (result != VCS_ZCODE_WORK_CONTEXT_OK) return result;
    uint8_t *wire = NULL; size_t wire_len = 0;
    result = vcs_zcode_work_context_serialize(context, now_unix, &wire,
                                              &wire_len);
    if (result != VCS_ZCODE_WORK_CONTEXT_OK) return result;
    uint32_t chunks = (uint32_t)((wire_len + VCS_PACKAGE_CHUNK_BYTES - 1u) /
                                 VCS_PACKAGE_CHUNK_BYTES);
    uint8_t *hashes = zcl_malloc((size_t)chunks * 32u,
                                 "zcode.work_context.hashes");
    if (!hashes) { free(wire); return VCS_ZCODE_WORK_CONTEXT_ALLOC; }
    for (uint32_t i = 0; i < chunks; i++) {
        size_t off = (size_t)i * VCS_PACKAGE_CHUNK_BYTES;
        size_t take = wire_len - off;
        if (take > VCS_PACKAGE_CHUNK_BYTES) take = VCS_PACKAGE_CHUNK_BYTES;
        if (!vcs_package_chunk_hash(wire + off, take, hashes + i * 32u)) {
            free(hashes); free(wire); return VCS_ZCODE_WORK_CONTEXT_CORRUPT;
        }
    }
    struct vcs_package_manifest manifest;
    vcs_package_manifest_init(&manifest);
    bool built = vcs_package_manifest_add(
        &manifest, VCS_ZCODE_WORK_CONTEXT_PATH, VCS_PACKAGE_MODE_FILE,
        wire_len, hashes, chunks);
    free(hashes);
    uint8_t *manifest_wire = NULL; size_t manifest_len = 0;
    built = built && vcs_package_manifest_root(&manifest, package_root) &&
            vcs_package_manifest_serialize(&manifest, &manifest_wire,
                                           &manifest_len);
    vcs_package_manifest_free(&manifest);
    if (!built) {
        free(manifest_wire); free(wire);
        return VCS_ZCODE_WORK_CONTEXT_SHAPE;
    }
    uint8_t admitted[32];
    enum vcs_package_store_result stored = vcs_package_store_put_manifest(
        store, manifest_wire, manifest_len, admitted);
    free(manifest_wire);
    if (stored != VCS_PACKAGE_STORE_OK ||
        memcmp(admitted, package_root, 32) != 0) {
        free(wire); return VCS_ZCODE_WORK_CONTEXT_STORE;
    }
    for (uint32_t i = 0; i < chunks; i++) {
        size_t off = (size_t)i * VCS_PACKAGE_CHUNK_BYTES;
        size_t take = wire_len - off;
        if (take > VCS_PACKAGE_CHUNK_BYTES) take = VCS_PACKAGE_CHUNK_BYTES;
        stored = vcs_package_store_put_chunk(
            store, package_root, VCS_ZCODE_WORK_CONTEXT_PATH, i,
            wire + off, take);
        if (stored != VCS_PACKAGE_STORE_OK) {
            free(wire); return VCS_ZCODE_WORK_CONTEXT_STORE;
        }
    }
    free(wire);
    return VCS_ZCODE_WORK_CONTEXT_OK;
}

enum vcs_zcode_work_context_result vcs_zcode_work_context_put(
    struct vcs_package_store *store,
    const struct vcs_zcode_work_context_v1 *context, int64_t now_unix,
    uint8_t package_root[32], uint8_t action_root[32])
{
    return vcs_zcode_work_context_put_for_kind(
        store, context, VCS_BUILD_ACTION_KIND_V1, now_unix, package_root,
        action_root);
}

enum vcs_zcode_work_context_result vcs_zcode_work_context_get(
    struct vcs_package_store *store, const uint8_t package_root[32],
    int64_t now_unix, struct vcs_zcode_work_context_v1 *out)
{
    if (!store || !package_root || !out)
        return VCS_ZCODE_WORK_CONTEXT_NULL;
    struct vcs_package_store_status status;
    if (!vcs_package_store_package_status(store, package_root, &status) ||
        !status.complete)
        return VCS_ZCODE_WORK_CONTEXT_ABSENT;
    uint8_t *manifest_wire = NULL; size_t manifest_len = 0;
    if (vcs_package_store_get_manifest_wire(
            store, package_root, &manifest_wire, &manifest_len) !=
            VCS_PACKAGE_STORE_OK)
        return VCS_ZCODE_WORK_CONTEXT_ABSENT;
    struct vcs_package_manifest manifest;
    bool parsed = vcs_package_manifest_parse(manifest_wire, manifest_len,
                                              &manifest);
    free(manifest_wire);
    uint8_t derived[32];
    if (!parsed || manifest.count != 1 ||
        strcmp(manifest.files[0].path, VCS_ZCODE_WORK_CONTEXT_PATH) != 0 ||
        manifest.files[0].mode != VCS_PACKAGE_MODE_FILE ||
        manifest.files[0].size < VCS_ZCODE_WORK_CONTEXT_FIXED_BYTES ||
        manifest.files[0].size > VCS_PACKAGE_STORE_MAX_PACKAGE_BYTES ||
        !vcs_package_manifest_root(&manifest, derived) ||
        memcmp(derived, package_root, 32) != 0) {
        if (parsed) vcs_package_manifest_free(&manifest);
        return VCS_ZCODE_WORK_CONTEXT_CORRUPT;
    }
    size_t wire_len = (size_t)manifest.files[0].size;
    uint8_t *wire = zcl_malloc(wire_len, "zcode.work_context.package");
    if (!wire) {
        vcs_package_manifest_free(&manifest);
        return VCS_ZCODE_WORK_CONTEXT_ALLOC;
    }
    size_t written = 0;
    for (uint32_t i = 0; i < manifest.files[0].chunk_count; i++) {
        uint8_t *chunk = NULL; size_t chunk_len = 0;
        enum vcs_package_store_result got = vcs_package_store_get_chunk_at(
            store, package_root, 0, i, &chunk, &chunk_len);
        if (got != VCS_PACKAGE_STORE_OK ||
            !vcs_package_verify_chunk(&manifest.files[0], i, chunk,
                                      chunk_len) ||
            chunk_len > wire_len - written) {
            free(chunk); free(wire);
            vcs_package_manifest_free(&manifest);
            return VCS_ZCODE_WORK_CONTEXT_CORRUPT;
        }
        memcpy(wire + written, chunk, chunk_len);
        written += chunk_len;
        free(chunk);
    }
    vcs_package_manifest_free(&manifest);
    if (written != wire_len) {
        free(wire); return VCS_ZCODE_WORK_CONTEXT_CORRUPT;
    }
    enum vcs_zcode_work_context_result result =
        vcs_zcode_work_context_parse(wire, wire_len, now_unix, out);
    free(wire);
    return result;
}
