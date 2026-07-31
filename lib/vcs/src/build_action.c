/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Canonical SHA3 identities for toolchains and fixed C23 actions. */

#include "vcs/build_action.h"

#include "crypto/sha3.h"

#include <string.h>

static void build_hash_text(struct sha3_256_ctx *sha, const char *value)
{
    uint64_t length = value ? strlen(value) : 0;
    uint8_t le[8];
    for (unsigned i = 0; i < sizeof(le); i++)
        le[i] = (uint8_t)((length >> (8U * i)) & 0xffU);
    sha3_256_write(sha, le, sizeof(le));
    if (length)
        sha3_256_write(sha, (const uint8_t *)value, (size_t)length);
}

static void build_hash_u64(struct sha3_256_ctx *sha, uint64_t value)
{
    uint8_t le[8];
    for (unsigned i = 0; i < sizeof(le); i++)
        le[i] = (uint8_t)((value >> (8U * i)) & 0xffU);
    sha3_256_write(sha, le, sizeof(le));
}

static bool build_text_valid(const char *value, size_t cap)
{
    return value && value[0] && strnlen(value, cap) < cap;
}

bool vcs_toolchain_capsule_v1_root(
    const struct vcs_toolchain_capsule_v1 *capsule, uint8_t out[32])
{
    if (!capsule || !out ||
        !build_text_valid(capsule->target, sizeof(capsule->target)) ||
        strcmp(capsule->target, VCS_BUILD_TARGET_V1) != 0)
        return false;
    static const char domain[] = "zcl.toolchain_capsule.v1";
    struct sha3_256_ctx sha;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)domain, sizeof(domain));
    sha3_256_write(&sha, capsule->compiler_driver_sha3, 32);
    sha3_256_write(&sha, capsule->compiler_backend_sha3, 32);
    sha3_256_write(&sha, capsule->assembler_sha3, 32);
    sha3_256_write(&sha, capsule->sysroot_sha3, 32);
    sha3_256_write(&sha, capsule->target_probes_sha3, 32);
    sha3_256_write(&sha, capsule->abi_files_sha3, 32);
    build_hash_text(&sha, capsule->target);
    sha3_256_finalize(&sha, out);
    return true;
}

bool vcs_build_action_v1_root(const struct vcs_build_action_v1 *action,
                              uint8_t out[32])
{
    if (!action || !out ||
        !build_text_valid(action->target, sizeof(action->target)) ||
        !build_text_valid(action->profile, sizeof(action->profile)) ||
        !build_text_valid(action->virtual_workdir,
                          sizeof(action->virtual_workdir)) ||
        !build_text_valid(action->declared_outputs,
                          sizeof(action->declared_outputs)) ||
        !build_text_valid(action->resource_policy,
                          sizeof(action->resource_policy)) ||
        strcmp(action->target, VCS_BUILD_TARGET_V1) != 0 ||
        strcmp(action->virtual_workdir, VCS_BUILD_VIRTUAL_ROOT_V1) != 0 ||
        strcmp(action->declared_outputs, VCS_BUILD_OUTPUT_V1) != 0 ||
        strcmp(action->resource_policy, VCS_BUILD_RESOURCE_POLICY_V1) != 0)
        return false;
    static const char domain[] = "zcl.build_action.v1";
    struct sha3_256_ctx sha;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)domain, sizeof(domain));
    build_hash_text(&sha, VCS_BUILD_ACTION_KIND_V1);
    sha3_256_write(&sha, action->source_sha256, 32);
    sha3_256_write(&sha, action->source_cas_sha3, 32);
    sha3_256_write(&sha, action->input_root_sha3, 32);
    sha3_256_write(&sha, action->toolchain_capsule_sha3, 32);
    sha3_256_write(&sha, action->flags_sha3, 32);
    sha3_256_write(&sha, action->environment_sha3, 32);
    build_hash_text(&sha, action->target);
    build_hash_text(&sha, action->profile);
    build_hash_text(&sha, action->virtual_workdir);
    build_hash_text(&sha, action->declared_outputs);
    build_hash_text(&sha, action->resource_policy);
    build_hash_u64(&sha, action->sequence);
    sha3_256_finalize(&sha, out);
    return true;
}
