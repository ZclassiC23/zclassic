/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Project-neutral toolchain and preprocessed-C23 action identities. */

#ifndef ZCL_VCS_BUILD_ACTION_H
#define ZCL_VCS_BUILD_ACTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VCS_BUILD_TARGET_V1 "linux-x86_64-v3"
#define VCS_BUILD_ACTION_KIND_V1 "c23.compile.preprocessed.v1"
#define VCS_BUILD_VIRTUAL_ROOT_V1 "/zbuild/src"
#define VCS_BUILD_OUTPUT_V1 "unit.o"
#define VCS_BUILD_RESOURCE_POLICY_V1 \
    "cpu=1,memory_mb=2048,timeout_s=120,network=0"

struct vcs_toolchain_capsule_v1 {
    uint8_t compiler_driver_sha3[32];
    uint8_t compiler_backend_sha3[32];
    uint8_t assembler_sha3[32];
    uint8_t sysroot_sha3[32];
    uint8_t target_probes_sha3[32];
    uint8_t abi_files_sha3[32];
    char target[64];
};

struct vcs_build_action_v1 {
    uint8_t source_sha256[32];
    uint8_t source_cas_sha3[32];
    uint8_t input_root_sha3[32];
    uint8_t toolchain_capsule_sha3[32];
    uint8_t flags_sha3[32];
    uint8_t environment_sha3[32];
    char target[64];
    char profile[32];
    char virtual_workdir[256];
    char declared_outputs[256];
    char resource_policy[256];
    uint64_t sequence;
};

bool vcs_toolchain_capsule_v1_root(
    const struct vcs_toolchain_capsule_v1 *capsule, uint8_t out[32]);
bool vcs_build_action_v1_root(const struct vcs_build_action_v1 *action,
                              uint8_t out[32]);

#endif /* ZCL_VCS_BUILD_ACTION_H */
