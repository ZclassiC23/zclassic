/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Confined fixed-C23 execution backed by the existing ZVCS CAS. */

#include "services/build_fabric_worker.h"

#include "base/hex.h"
#include "crypto/ed25519.h"
#include "crypto/random_secret.h"
#include "crypto/sha3.h"
#include "platform/os_proc.h"
#include "platform/time_compat.h"
#include "services/build_fabric_service.h"
#include "util/safe_alloc.h"
#include "util/spawn.h"
#include "vcs/build_action.h"
#include "vcs/build_artifact_manifest.h"
#include "vcs/vcs_object.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BFW_PATH_MAX 4096
#define BFW_CAPTURE_MAX 4096
#define BFW_EXEC_TIMEOUT_MS 125000

struct zcl_result build_fabric_worker_identity_load(
    const char *datadir, struct db_build_worker *worker,
    uint8_t signer_secret[32], uint8_t signer_pubkey[32])
{
    if (!datadir || !datadir[0] || !worker || !signer_secret ||
        !signer_pubkey)
        return ZCL_ERR(-1, "worker identity requires datadir and outputs");
    char zcode[BFW_PATH_MAX];
    char path[BFW_PATH_MAX];
    int n = snprintf(zcode, sizeof(zcode), "%s/zcode", datadir);
    if (n <= 0 || (size_t)n >= sizeof(zcode))
        return ZCL_ERR(-1, "worker key path too long");
    if (mkdir(zcode, 0700) != 0 && errno != EEXIST)
        return ZCL_ERR(-1, "mkdir %s: %s", zcode, strerror(errno));
    n = snprintf(path, sizeof(path), "%s/build-worker.ed25519", zcode);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return ZCL_ERR(-1, "worker key path too long");
    uint8_t seed[32];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) {
        if (!zcl_random_secret_bytes(seed, sizeof(seed), "zbuild_worker_key"))
            return ZCL_ERR(-1, "worker key CSPRNG failed");
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0)
            return ZCL_ERR(-1, "create worker key: %s", strerror(errno));
        ssize_t wrote = write(fd, seed, sizeof(seed));
        bool ok = wrote == (ssize_t)sizeof(seed) && fsync(fd) == 0 &&
                  close(fd) == 0;
        if (!ok) {
            (void)unlink(path);
            memset(seed, 0, sizeof(seed));
            return ZCL_ERR(-1, "durable worker key write failed");
        }
        fd = open(path, O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0)
        return ZCL_ERR(-1, "open worker key: %s", strerror(errno));
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        (st.st_mode & 077) != 0 || st.st_size != (off_t)sizeof(seed)) {
        close(fd);
        return ZCL_ERR(-1, "worker key must be a private 32-byte regular file");
    }
    size_t off = 0;
    while (off < sizeof(seed)) {
        ssize_t got = read(fd, seed + off, sizeof(seed) - off);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        off += (size_t)got;
    }
    close(fd);
    if (off != sizeof(seed)) {
        memset(seed, 0, sizeof(seed));
        return ZCL_ERR(-1, "worker key read was truncated");
    }
    ed25519_keypair(signer_pubkey, signer_secret, seed);
    memset(seed, 0, sizeof(seed));
    static const char domain[] = "zcl.build_worker.v1";
    struct sha3_256_ctx sha;
    uint8_t worker_id[32];
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)domain, sizeof(domain));
    sha3_256_write(&sha, signer_pubkey, 32);
    sha3_256_finalize(&sha, worker_id);
    memset(worker, 0, sizeof(*worker));
    zcl_hex_encode(worker_id, sizeof(worker_id), worker->worker_id);
    zcl_hex_encode(signer_pubkey, 32, worker->signer_pubkey);
    (void)snprintf(worker->capabilities, sizeof(worker->capabilities),
                   "linux,x86-64-v3,gcc,%s", VCS_BUILD_ACTION_KIND_V1);
    return ZCL_OK;
}

struct bfw_paths {
    char worker[BFW_PATH_MAX];
    char work[BFW_PATH_MAX];
    char src[BFW_PATH_MAX];
    char build[BFW_PATH_MAX];
    char input[BFW_PATH_MAX];
    char output[BFW_PATH_MAX];
};

static struct zcl_result bfw_worker_path(const char *workspace,
                                         char *out, size_t cap)
{
    char exe[BFW_PATH_MAX];
    if (!os_proc_exe_path(exe, sizeof(exe)))
        return ZCL_ERR(-1, "cannot resolve the running executable");
    char *deleted = strstr(exe, " (deleted)");
    if (deleted) *deleted = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash) return ZCL_ERR(-1, "running executable has no directory");
    *slash = '\0';
    int n = snprintf(out, cap, "%s/zclassic23-package-verify", exe);
    if (n > 0 && (size_t)n < cap && access(out, X_OK) == 0)
        return ZCL_OK;
    n = snprintf(out, cap, "%s/build/bin/zclassic23-package-verify",
                 workspace);
    if (n > 0 && (size_t)n < cap && access(out, X_OK) == 0)
        return ZCL_OK;
    n = snprintf(out, cap, "build/bin/zclassic23-package-verify");
    if (n > 0 && (size_t)n < cap && access(out, X_OK) == 0)
        return ZCL_OK;
    return ZCL_ERR(-1, "fixed package verifier is not built");
}

static struct zcl_result bfw_paths_init(const char *workspace,
                                        const char *lease_id,
                                        struct bfw_paths *p)
{
    if (!workspace || !workspace[0] || !lease_id || !p)
        return ZCL_ERR(-1, "worker paths require workspace and lease");
    ZCL_CHECK(bfw_worker_path(workspace, p->worker, sizeof(p->worker)));
    char base[BFW_PATH_MAX];
    int n = snprintf(base, sizeof(base), "%s/.zvcs/build-work", workspace);
    if (n <= 0 || (size_t)n >= sizeof(base))
        return ZCL_ERR(-1, "worker base path too long");
    if (mkdir(base, 0700) != 0 && errno != EEXIST)
        return ZCL_ERR(-1, "mkdir %s: %s", base, strerror(errno));
    n = snprintf(p->work, sizeof(p->work), "%s/%s", base, lease_id);
    if (n <= 0 || (size_t)n >= sizeof(p->work) || mkdir(p->work, 0700) != 0)
        return ZCL_ERR(-1, "lease work directory exists or cannot be created");
    (void)snprintf(p->src, sizeof(p->src), "%s/src", p->work);
    (void)snprintf(p->build, sizeof(p->build), "%s/build", p->work);
    if (mkdir(p->src, 0700) != 0 || mkdir(p->build, 0700) != 0)
        return ZCL_ERR(-1, "cannot create isolated source/output directories");
    (void)snprintf(p->input, sizeof(p->input), "%s/unit.i", p->src);
    (void)snprintf(p->output, sizeof(p->output), "%s/%s", p->build,
                   VCS_BUILD_OUTPUT_V1);
    return ZCL_OK;
}

static void bfw_paths_cleanup(const struct bfw_paths *p)
{
    if (!p) return;
    (void)unlink(p->output);
    (void)unlink(p->input);
    (void)rmdir(p->build);
    (void)rmdir(p->src);
    (void)rmdir(p->work);
}

static bool bfw_write_input(const char *path, const uint8_t *bytes, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0400);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < len) {
        ssize_t wrote = write(fd, bytes + off, len - off);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            close(fd); (void)unlink(path); return false;
        }
        off += (size_t)wrote;
    }
    bool ok = fsync(fd) == 0 && close(fd) == 0;
    if (!ok) (void)unlink(path);
    return ok;
}

static uint8_t *bfw_read_output(const char *path, size_t *len_out)
{
    *len_out = 0;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size > VCS_BUILD_ARTIFACT_MAX_BYTES)
        return NULL;
    size_t len = (size_t)st.st_size;
    uint8_t *bytes = zcl_malloc(len, "zbuild.output");
    if (!bytes) return NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { free(bytes); return NULL; }
    size_t off = 0;
    while (off < len) {
        ssize_t got = read(fd, bytes + off, len - off);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        off += (size_t)got;
    }
    close(fd);
    if (off != len) { free(bytes); return NULL; }
    *len_out = len;
    return bytes;
}

static bool bfw_elf_relocatable_x86_64(const uint8_t *bytes, size_t len)
{
    return bytes && len >= 64 && bytes[0] == 0x7f && bytes[1] == 'E' &&
           bytes[2] == 'L' && bytes[3] == 'F' && bytes[4] == 2 &&
           bytes[5] == 1 && bytes[6] == 1 && bytes[16] == 1 &&
           bytes[17] == 0 && bytes[18] == 62 && bytes[19] == 0;
}

static struct zcl_result bfw_store_artifact(
    const char *workspace, const char *action_id, const uint8_t *bytes,
    size_t len, uint8_t manifest_root[32])
{
    struct vcs_build_artifact_manifest_v1 manifest = {0};
    if (!zcl_hex_decode_lower(action_id, manifest.action_sha3, 32))
        return ZCL_ERR(-1, "action id is not canonical lowercase hex");
    manifest.total_bytes = len;
    manifest.chunk_bytes = VCS_BUILD_ARTIFACT_CHUNK_BYTES;
    manifest.chunk_count = (uint32_t)((len + VCS_BUILD_ARTIFACT_CHUNK_BYTES - 1u) /
                                      VCS_BUILD_ARTIFACT_CHUNK_BYTES);
    for (uint32_t i = 0; i < manifest.chunk_count; i++) {
        size_t off = (size_t)i * VCS_BUILD_ARTIFACT_CHUNK_BYTES;
        size_t take = len - off;
        if (take > VCS_BUILD_ARTIFACT_CHUNK_BYTES)
            take = VCS_BUILD_ARTIFACT_CHUNK_BYTES;
        sha3_256(bytes + off, take, manifest.chunk_sha3[i]);
        if (!vcs_object_put_addressed(workspace, manifest.chunk_sha3[i],
                                      bytes + off, take))
            return ZCL_ERR(-1, "cannot persist build artifact chunk %u", i);
    }
    uint8_t wire[VCS_BUILD_ARTIFACT_WIRE_MAX];
    size_t wire_len = 0;
    if (!vcs_build_artifact_manifest_v1_root(&manifest, manifest_root) ||
        !vcs_build_artifact_manifest_v1_serialize(
            &manifest, wire, sizeof(wire), &wire_len) ||
        !vcs_object_put_addressed(workspace, manifest_root, wire, wire_len))
        return ZCL_ERR(-1, "cannot persist build artifact manifest");
    return ZCL_OK;
}

static struct zcl_result bfw_fail(struct node_db *ndb,
                                  const char *action_id,
                                  const char *lease_id, const char *detail)
{
    int64_t now = (int64_t)platform_time_wall_unix();
    struct zcl_result finish = build_fabric_finish_leased(
        ndb, action_id, lease_id, "LOCAL_FALLBACK", detail, now);
    if (!finish.ok)
        return ZCL_ERR(-1, "%s; fallback transition failed: %s", detail,
                       finish.message);
    return ZCL_ERR(-1, "%s; named outcome LOCAL_FALLBACK", detail);
}

// long-function-ok:one-confined-action — every recheck brackets the exact
// sandbox/CAS/signature sequence; splitting it would make stale publication
// reachable between independently callable phases.
struct zcl_result build_fabric_worker_execute(
    struct node_db *ndb, const char *workspace, const char *action_id,
    const char *lease_id, const uint8_t signer_secret[32],
    const uint8_t signer_pubkey[32], struct db_build_receipt *out_receipt)
{
    if (!ndb || !ndb->open || !workspace || !action_id || !lease_id ||
        !signer_secret || !signer_pubkey || !out_receipt)
        return ZCL_ERR(-1, "worker execution requires lease, workspace, and key");
    char workspace_resolved[BFW_PATH_MAX];
    if (!realpath(workspace, workspace_resolved))
        return ZCL_ERR(-1, "worker workspace cannot be resolved: %s",
                       strerror(errno));
    workspace = workspace_resolved;
    struct db_build_action action;
    struct db_build_job job;
    struct db_build_worker worker;
    if (!db_build_action_find(ndb, action_id, &action) ||
        !db_build_job_find(ndb, action.job_id, &job) ||
        !db_build_worker_find(ndb, action.worker_id, &worker) ||
        strcmp(action.lease_id, lease_id) != 0)
        return ZCL_ERR(-1, "claimed worker action is missing or stale");
    char signer_hex[65];
    zcl_hex_encode(signer_pubkey, 32, signer_hex);
    if (strcmp(signer_hex, worker.signer_pubkey) != 0)
        return bfw_fail(ndb, action_id, lease_id, "worker-signer-mismatch");
    struct vcs_toolchain_capsule_v1 capsule;
    uint8_t capsule_root[32];
    char capsule_hex[65];
    if (!vcs_toolchain_capsule_v1_capture_gcc(&capsule) ||
        !vcs_toolchain_capsule_v1_root(&capsule, capsule_root))
        return bfw_fail(ndb, action_id, lease_id, "toolchain-capture-failed");
    zcl_hex_encode(capsule_root, 32, capsule_hex);
    if (strcmp(capsule_hex, job.toolchain_sha3) != 0)
        return bfw_fail(ndb, action_id, lease_id, "toolchain-capsule-stale");
    uint8_t fixed_flags[32], fixed_environment[32];
    char fixed_flags_hex[65], fixed_environment_hex[65];
    vcs_build_action_v1_fixed_flags_root(fixed_flags);
    vcs_build_action_v1_fixed_environment_root(fixed_environment);
    zcl_hex_encode(fixed_flags, 32, fixed_flags_hex);
    zcl_hex_encode(fixed_environment, 32, fixed_environment_hex);
    if (strcmp(fixed_flags_hex, action.flags_sha3) != 0 ||
        strcmp(fixed_environment_hex, action.environment_sha3) != 0)
        return bfw_fail(ndb, action_id, lease_id,
                        "fixed-flags-or-environment-stale");
    uint8_t input_root[32];
    uint8_t *input = NULL;
    size_t input_len = 0;
    if (!zcl_hex_decode_lower(action.input_root_sha3, input_root, 32) ||
        vcs_object_load_raw(workspace, input_root, &input, &input_len) != 0 ||
        input_len == 0 || input_len > VCS_BUILD_ARTIFACT_MAX_BYTES) {
        free(input);
        return bfw_fail(ndb, action_id, lease_id, "input-cas-miss");
    }
    uint8_t checked_input[32];
    sha3_256(input, input_len, checked_input);
    if (memcmp(checked_input, input_root, 32) != 0) {
        free(input);
        return bfw_fail(ndb, action_id, lease_id, "input-cas-corrupt");
    }
    struct zcl_result start = build_fabric_start(
        ndb, action_id, lease_id, (int64_t)platform_time_wall_unix());
    if (!start.ok) { free(input); return start; }
    struct bfw_paths paths = {0};
    struct zcl_result paths_result = bfw_paths_init(workspace, lease_id, &paths);
    if (!paths_result.ok || !bfw_write_input(paths.input, input, input_len)) {
        free(input); bfw_paths_cleanup(&paths);
        return bfw_fail(ndb, action_id, lease_id, "input-materialize-failed");
    }
    free(input);
    char input_arg[BFW_PATH_MAX + 32];
    char output_arg[BFW_PATH_MAX + 32];
    (void)snprintf(input_arg, sizeof(input_arg), "--zbuild-input=%s",
                   paths.input);
    (void)snprintf(output_arg, sizeof(output_arg), "--zbuild-output=%s",
                   paths.output);
    const char *const argv[] = { paths.worker, input_arg, output_arg,
                                "--require-full-isolation", NULL };
    char capture[BFW_CAPTURE_MAX];
    int rc = zcl_spawn_capture(argv, capture, sizeof(capture),
                               BFW_EXEC_TIMEOUT_MS);
    if (rc != 0 || strstr(capture, "zbuild-ok=1") == NULL) {
        for (size_t i = 0; capture[i]; i++)
            if ((unsigned char)capture[i] < 0x20 ||
                (unsigned char)capture[i] > 0x7e)
                capture[i] = ' ';
        char detail[BUILD_FABRIC_ERROR_MAX + 1];
        (void)snprintf(detail, sizeof(detail), "sandbox-exit-%d: %.180s", rc,
                       capture[0] ? capture : "no-report");
        bfw_paths_cleanup(&paths);
        return bfw_fail(ndb, action_id, lease_id, detail);
    }
    struct zcl_result verify = build_fabric_begin_verify(
        ndb, action_id, lease_id, (int64_t)platform_time_wall_unix());
    if (!verify.ok) { bfw_paths_cleanup(&paths); return verify; }
    size_t output_len = 0;
    uint8_t *output = bfw_read_output(paths.output, &output_len);
    if (!output || !bfw_elf_relocatable_x86_64(output, output_len)) {
        free(output); bfw_paths_cleanup(&paths);
        return bfw_fail(ndb, action_id, lease_id, "output-elf-invalid");
    }
    uint8_t output_root[32];
    struct zcl_result stored = bfw_store_artifact(
        workspace, action_id, output, output_len, output_root);
    free(output);
    bfw_paths_cleanup(&paths);
    if (!stored.ok)
        return bfw_fail(ndb, action_id, lease_id, "output-cas-store-failed");
    struct db_build_receipt receipt = {0};
    (void)snprintf(receipt.action_id, sizeof(receipt.action_id), "%s",
                   action.action_id);
    (void)snprintf(receipt.job_id, sizeof(receipt.job_id), "%s", action.job_id);
    (void)snprintf(receipt.worker_id, sizeof(receipt.worker_id), "%s",
                   action.worker_id);
    (void)snprintf(receipt.lease_id, sizeof(receipt.lease_id), "%s", lease_id);
    (void)snprintf(receipt.action_sha3, sizeof(receipt.action_sha3), "%s",
                   action.action_id);
    zcl_hex_encode(output_root, 32, receipt.output_sha3);
    (void)snprintf(receipt.confinement, sizeof(receipt.confinement),
                   "landlock=1,seccomp=1,rlimits=1,network=0,gcc=fixed");
    receipt.exit_status = 0;
    receipt.created_at = (int64_t)platform_time_wall_unix();
    if (!build_fabric_receipt_id(&receipt, receipt.receipt_id).ok)
        return bfw_fail(ndb, action_id, lease_id, "receipt-id-failed");
    uint8_t receipt_id[32], signature[64];
    if (!zcl_hex_decode_lower(receipt.receipt_id, receipt_id, 32))
        return bfw_fail(ndb, action_id, lease_id, "receipt-id-invalid");
    ed25519_sign(signature, receipt_id, sizeof(receipt_id), signer_secret,
                 signer_pubkey);
    zcl_hex_encode(signature, sizeof(signature), receipt.signature);
    struct zcl_result accepted = build_fabric_receipt_accept(
        ndb, &receipt, receipt.created_at);
    if (!accepted.ok) return accepted;
    *out_receipt = receipt;
    return ZCL_OK;
}
