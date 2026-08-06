/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Canonical SHA3 identities for toolchains and fixed C23 actions. */

#include "vcs/build_action.h"

#include "crypto/sha3.h"
#include "util/spawn.h"
#include "vcs/zcode_dev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static bool build_sha3_file(const char *path, uint8_t out[32])
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    struct sha3_256_ctx sha;
    sha3_256_init(&sha);
    uint8_t buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
        sha3_256_write(&sha, buf, got);
    bool ok = ferror(f) == 0;
    fclose(f);
    if (!ok) return false;
    sha3_256_finalize(&sha, out);
    return true;
}

static bool build_gcc_query(const char *arg, char *out, size_t cap)
{
    const char *const argv[] = { "/usr/bin/gcc", arg, NULL };
    if (zcl_spawn_capture(argv, out, cap, 10000) != 0 || !out[0])
        return false;
    out[strcspn(out, "\r\n")] = '\0';
    return out[0] != '\0';
}

static bool build_gcc_file(const char *arg, const char *fallback,
                           uint8_t out[32])
{
    char named[4096];
    if (!build_gcc_query(arg, named, sizeof(named))) return false;
    const char *candidate = strchr(named, '/') ? named : fallback;
    char resolved[4096];
    if (!candidate || !realpath(candidate, resolved)) return false;
    return build_sha3_file(resolved, out);
}

static void build_hash_pair(struct sha3_256_ctx *sha, const char *label,
                            const uint8_t digest[32])
{
    build_hash_text(sha, label);
    sha3_256_write(sha, digest, 32);
}

static bool build_gcc_aggregate(const char *domain,
                                const char *const args[],
                                const char *const fallbacks[], size_t count,
                                uint8_t out[32])
{
    struct sha3_256_ctx sha;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)domain, strlen(domain) + 1u);
    for (size_t i = 0; i < count; i++) {
        uint8_t digest[32];
        if (!build_gcc_file(args[i], fallbacks ? fallbacks[i] : NULL,
                            digest))
            return false;
        build_hash_pair(&sha, args[i], digest);
    }
    sha3_256_finalize(&sha, out);
    return true;
}

bool vcs_toolchain_capsule_v1_capture_gcc(
    struct vcs_toolchain_capsule_v1 *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    char driver[4096];
    if (!realpath("/usr/bin/gcc", driver) ||
        !build_sha3_file(driver, out->compiler_driver_sha3) ||
        !build_gcc_file("-print-prog-name=cc1", NULL,
                        out->compiler_backend_sha3) ||
        !build_gcc_file("-print-prog-name=as", "/usr/bin/as",
                        out->assembler_sha3))
        return false;
    static const char *const sysroot_args[] = {
        "-print-file-name=crt1.o", "-print-file-name=crti.o",
        "-print-file-name=crtn.o",
    };
    if (!build_gcc_aggregate("zcl.toolchain.sysroot.v1", sysroot_args,
                             NULL, 3, out->sysroot_sha3))
        return false;
    char machine[256], full_version[256], version[256];
    if (!build_gcc_query("-dumpmachine", machine, sizeof(machine)) ||
        !build_gcc_query("-dumpfullversion", full_version,
                         sizeof(full_version)) ||
        !build_gcc_query("-dumpversion", version, sizeof(version)))
        return false;
    struct sha3_256_ctx probes;
    sha3_256_init(&probes);
    static const char probe_domain[] = "zcl.toolchain.target_probes.v1";
    sha3_256_write(&probes, (const uint8_t *)probe_domain,
                   sizeof(probe_domain));
    build_hash_text(&probes, machine);
    build_hash_text(&probes, full_version);
    build_hash_text(&probes, version);
    build_hash_text(&probes, VCS_BUILD_TARGET_V1);
    sha3_256_finalize(&probes, out->target_probes_sha3);
    static const char *const abi_args[] = {
        "-print-libgcc-file-name", "-print-file-name=crtbegin.o",
        "-print-file-name=libc.so.6",
    };
    if (!build_gcc_aggregate("zcl.toolchain.abi_files.v1", abi_args, NULL,
                             3, out->abi_files_sha3))
        return false;
    (void)snprintf(out->target, sizeof(out->target), "%s",
                   VCS_BUILD_TARGET_V1);
    return true;
}

void vcs_build_action_v1_fixed_flags_root(uint8_t out[32])
{
    static const char domain[] = "zcl.build_action.fixed_flags.v1";
    static const char *const values[] = {
        "/usr/bin/gcc", "-x", "cpp-output", "-std=c23", "-O2",
        "-march=x86-64-v3", "-fno-ident", "-c", "/zbuild/src/unit.i",
        "-o", "/zbuild/out/unit.o",
    };
    struct sha3_256_ctx sha;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)domain, sizeof(domain));
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
        build_hash_text(&sha, values[i]);
    sha3_256_finalize(&sha, out);
}

void vcs_build_action_v1_fixed_environment_root(uint8_t out[32])
{
    static const char domain[] = "zcl.build_action.fixed_environment.v1";
    static const char *const values[] = {
        "PATH=/usr/local/bin:/usr/bin:/bin", "LC_ALL=C",
        "TMPDIR=/zbuild/out",
    };
    struct sha3_256_ctx sha;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)domain, sizeof(domain));
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
        build_hash_text(&sha, values[i]);
    sha3_256_finalize(&sha, out);
}

uint8_t vcs_build_action_v1_work_kind(const char *kind)
{
    if (!kind) return 0;
    if (strcmp(kind, VCS_BUILD_ACTION_KIND_V1) == 0)
        return VCS_ZCODE_WORK_BUILD;
    if (strcmp(kind, VCS_BUILD_ACTION_KIND_TEST_V1) == 0)
        return VCS_ZCODE_WORK_TEST;
    if (strcmp(kind, VCS_BUILD_ACTION_KIND_FUZZ_V1) == 0)
        return VCS_ZCODE_WORK_FUZZ;
    if (strcmp(kind, VCS_BUILD_ACTION_KIND_BENCHMARK_V1) == 0)
        return VCS_ZCODE_WORK_TEST;
    if (strcmp(kind, VCS_BUILD_ACTION_KIND_BENCHMARK_REPRODUCE_V1) == 0)
        return VCS_ZCODE_WORK_REPRODUCE;
    if (strcmp(kind, VCS_BUILD_ACTION_KIND_REVIEW_V1) == 0)
        return VCS_ZCODE_WORK_REVIEW;
    return 0;
}

bool vcs_build_action_v1_descriptors(
    const char *kind, const char **workdir, const char **output,
    const char **resource)
{
    if (vcs_build_action_v1_work_kind(kind) == 0) return false;
    if (strcmp(kind, VCS_BUILD_ACTION_KIND_V1) == 0) {
        *workdir = VCS_BUILD_VIRTUAL_ROOT_V1;
        *output = VCS_BUILD_OUTPUT_V1;
        *resource = VCS_BUILD_RESOURCE_POLICY_V1;
    } else if (strcmp(kind, VCS_BUILD_ACTION_KIND_TEST_V1) == 0) {
        *workdir = VCS_BUILD_PACKAGE_VIRTUAL_ROOT_V1;
        *output = VCS_BUILD_TEST_OUTPUT_V1;
        *resource = VCS_BUILD_TEST_RESOURCE_POLICY_V1;
    } else if (strcmp(kind, VCS_BUILD_ACTION_KIND_FUZZ_V1) == 0) {
        *workdir = VCS_BUILD_PACKAGE_VIRTUAL_ROOT_V1;
        *output = VCS_BUILD_FUZZ_OUTPUT_V1;
        *resource = VCS_BUILD_FUZZ_RESOURCE_POLICY_V1;
    } else if (strcmp(kind, VCS_BUILD_ACTION_KIND_BENCHMARK_V1) == 0) {
        *workdir = VCS_BUILD_PACKAGE_VIRTUAL_ROOT_V1;
        *output = VCS_BUILD_BENCHMARK_OUTPUT_V1;
        *resource = VCS_BUILD_BENCHMARK_RESOURCE_POLICY_V1;
    } else if (strcmp(kind,
                      VCS_BUILD_ACTION_KIND_BENCHMARK_REPRODUCE_V1) == 0) {
        *workdir = VCS_BUILD_PACKAGE_VIRTUAL_ROOT_V1;
        *output = VCS_BUILD_BENCHMARK_REPRODUCE_OUTPUT_V1;
        *resource = VCS_BUILD_BENCHMARK_REPRODUCE_RESOURCE_POLICY_V1;
    } else if (strcmp(kind, VCS_BUILD_ACTION_KIND_REVIEW_V1) == 0) {
        *workdir = VCS_BUILD_REVIEW_VIRTUAL_ROOT_V1;
        *output = VCS_BUILD_REVIEW_OUTPUT_V1;
        *resource = VCS_BUILD_REVIEW_RESOURCE_POLICY_V1;
    } else {
        return false;
    }
    return true;
}

static void build_action_kind_descriptor_root(
    const char *domain, const char *kind, uint8_t out[32])
{
    struct sha3_256_ctx sha;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)domain, strlen(domain) + 1u);
    build_hash_text(&sha, kind);
    sha3_256_finalize(&sha, out);
}

bool vcs_build_action_v1_fixed_flags_root_for_kind(
    const char *kind, uint8_t out[32])
{
    if (!out || vcs_build_action_v1_work_kind(kind) == 0) return false;
    if (strcmp(kind, VCS_BUILD_ACTION_KIND_V1) == 0) {
        vcs_build_action_v1_fixed_flags_root(out);
        return true;
    }
    build_action_kind_descriptor_root(
        "zcl.build_action.fixed_flags.v1", kind, out);
    return true;
}

bool vcs_build_action_v1_fixed_environment_root_for_kind(
    const char *kind, uint8_t out[32])
{
    if (!out || vcs_build_action_v1_work_kind(kind) == 0) return false;
    if (strcmp(kind, VCS_BUILD_ACTION_KIND_V1) == 0) {
        vcs_build_action_v1_fixed_environment_root(out);
        return true;
    }
    build_action_kind_descriptor_root(
        "zcl.build_action.fixed_environment.v1", kind, out);
    return true;
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

bool vcs_build_action_v1_root_for_kind(
    const char *kind, const struct vcs_build_action_v1 *action,
    uint8_t out[32])
{
    const char *workdir = NULL, *output = NULL, *resource = NULL;
    if (!action || !out ||
        !vcs_build_action_v1_descriptors(
            kind, &workdir, &output, &resource) ||
        !build_text_valid(action->target, sizeof(action->target)) ||
        !build_text_valid(action->profile, sizeof(action->profile)) ||
        !build_text_valid(action->virtual_workdir,
                          sizeof(action->virtual_workdir)) ||
        !build_text_valid(action->declared_outputs,
                          sizeof(action->declared_outputs)) ||
        !build_text_valid(action->resource_policy,
                          sizeof(action->resource_policy)) ||
        strcmp(action->target, VCS_BUILD_TARGET_V1) != 0 ||
        strcmp(action->virtual_workdir, workdir) != 0 ||
        strcmp(action->declared_outputs, output) != 0 ||
        strcmp(action->resource_policy, resource) != 0)
        return false;
    static const char domain[] = "zcl.build_action.v1";
    struct sha3_256_ctx sha;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)domain, sizeof(domain));
    build_hash_text(&sha, kind);
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

bool vcs_build_action_v1_root(const struct vcs_build_action_v1 *action,
                              uint8_t out[32])
{
    return vcs_build_action_v1_root_for_kind(
        VCS_BUILD_ACTION_KIND_V1, action, out);
}
