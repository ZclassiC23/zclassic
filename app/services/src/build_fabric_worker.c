/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Confined fixed-C23 execution backed by the existing ZVCS CAS. */

#include "services/build_fabric_worker.h"

#include "base/hex.h"
#include "base/serialize_le.h"
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
#include "vcs/zcode_dev.h"

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
#define BFW_TEST_EVIDENCE_BYTES 84u

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
        bool synced = wrote == (ssize_t)sizeof(seed) && fsync(fd) == 0;
        bool ok = close(fd) == 0 && synced;
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
                   "linux,x86-64-v3,gcc,%s,%s", VCS_BUILD_ACTION_KIND_V1,
                   VCS_BUILD_ACTION_KIND_TEST_V1);
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

static bool bfw_capability_has(const char *capabilities, const char *wanted)
{
    if (!capabilities || !wanted || !wanted[0]) return false;
    size_t wanted_len = strlen(wanted);
    const char *at = capabilities;
    while (*at) {
        const char *end = strchr(at, ',');
        size_t len = end ? (size_t)(end - at) : strlen(at);
        if (len == wanted_len && memcmp(at, wanted, len) == 0)
            return true;
        if (!end) break;
        at = end + 1;
    }
    return false;
}

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
                                        const char *kind,
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
    if (strcmp(kind, VCS_BUILD_ACTION_KIND_V1) == 0) {
        (void)snprintf(p->input, sizeof(p->input), "%s/unit.i", p->src);
        (void)snprintf(p->output, sizeof(p->output), "%s/%s", p->build,
                       VCS_BUILD_OUTPUT_V1);
    } else if (strcmp(kind, VCS_BUILD_ACTION_KIND_TEST_V1) == 0) {
        (void)snprintf(p->input, sizeof(p->input), "%s/test.bin", p->build);
        (void)snprintf(p->output, sizeof(p->output), "%s/%s", p->build,
                       VCS_BUILD_TEST_OUTPUT_V1);
    } else {
        return ZCL_ERR(-1, "worker action kind has no fixed executor");
    }
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

static bool bfw_write_input(const char *path, const uint8_t *bytes, size_t len,
                            bool executable)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                  executable ? 0500 : 0400);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < len) {
        ssize_t wrote = write(fd, bytes + off, len - off);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            close(fd); (void)unlink(path); return false;
        }
        if (wrote == 0) {
            close(fd); (void)unlink(path); return false;
        }
        off += (size_t)wrote;
    }
    bool synced = fsync(fd) == 0;
    bool ok = close(fd) == 0 && synced;
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

static bool bfw_test_evidence(const uint8_t *bytes, size_t len,
                              uint8_t *status, int *exit_status)
{
    if (!bytes || len != BFW_TEST_EVIDENCE_BYTES ||
        memcmp(bytes, "ZCTEST\r\n", 8) != 0 || bytes[8] != 1 ||
        bytes[9] != 0 || (bytes[10] != 1 && bytes[10] != 2) ||
        (bytes[11] & ~UINT8_C(7)) != 0 || !status || !exit_status)
        return false;
    uint32_t code = zcl_read_u32_le(bytes + 12);
    uint32_t signal = zcl_read_u32_le(bytes + 16);
    if (bytes[10] == 1 &&
        (code != 0 || signal != 0 || (bytes[11] & UINT8_C(1)) != 0))
        return false;
    *status = bytes[10] == 1 ? VCS_ZCODE_WORK_PASS : VCS_ZCODE_WORK_FAIL;
    if (*status == VCS_ZCODE_WORK_PASS) {
        *exit_status = 0;
    } else if (code > 0 && code <= 255) {
        *exit_status = (int)code;
    } else {
        *exit_status = 255;
    }
    return true;
}

static bool bfw_input_root_current(const char *workspace,
                                   const char *root_hex)
{
    uint8_t root[32], checked[32], *bytes = NULL;
    size_t len = 0;
    bool loaded = zcl_hex_decode_lower(root_hex, root, 32) &&
        vcs_object_load_raw(workspace, root, &bytes, &len) == 0;
    if (loaded) sha3_256(bytes, len, checked);
    free(bytes);
    return loaded && memcmp(root, checked, 32) == 0;
}

static bool bfw_toolchain_current(const struct db_build_job *job)
{
    struct vcs_toolchain_capsule_v1 capsule;
    uint8_t root[32]; char root_hex[65];
    if (!vcs_toolchain_capsule_v1_capture_gcc(&capsule) ||
        !vcs_toolchain_capsule_v1_root(&capsule, root))
        return false;
    zcl_hex_encode(root, 32, root_hex);
    return strcmp(root_hex, job->toolchain_sha3) == 0;
}

static bool bfw_binding_current(
    struct node_db *ndb, const struct db_build_job *expected_job,
    const struct db_build_action *expected_action, const char *lease_id)
{
    struct db_build_job job;
    struct db_build_action action;
    char action_id[65], job_id[65];
    return db_build_action_find(ndb, expected_action->action_id, &action) &&
        db_build_job_find(ndb, expected_job->job_id, &job) &&
        strcmp(action.state, "VERIFYING") == 0 &&
        strcmp(action.lease_id, lease_id) == 0 &&
        build_fabric_action_id(&job, &action, action_id).ok &&
        strcmp(action_id, expected_action->action_id) == 0 &&
        build_fabric_job_id(&job, action_id, job_id).ok &&
        strcmp(job_id, expected_job->job_id) == 0 &&
        strcmp(action.task_root_sha3,
               expected_action->task_root_sha3) == 0 &&
        strcmp(action.candidate_root_sha3,
               expected_action->candidate_root_sha3) == 0 &&
        strcmp(action.proof_policy_root_sha3,
               expected_action->proof_policy_root_sha3) == 0 &&
        strcmp(action.context_root_sha3,
               expected_action->context_root_sha3) == 0;
}

static struct zcl_result bfw_load_zcode_context(
    const char *workspace, const struct db_build_job *job,
    const struct db_build_action *action, int64_t now,
    struct vcs_zcode_task_v1 *task,
    struct vcs_zcode_candidate_v1 *candidate, bool *present)
{
    *present = false;
    if (!action->task_root_sha3[0]) return ZCL_OK;
    uint8_t task_root[32], candidate_root[32], policy_root[32];
    if (!zcl_hex_decode_lower(action->task_root_sha3, task_root, 32) ||
        !zcl_hex_decode_lower(action->candidate_root_sha3,
                              candidate_root, 32) ||
        !zcl_hex_decode_lower(action->proof_policy_root_sha3,
                              policy_root, 32))
        return ZCL_ERR(-1, "zcode-context-roots-invalid");
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    if (vcs_object_load_raw(workspace, task_root, &wire, &wire_len) != 0 ||
        vcs_zcode_task_parse(wire, wire_len, task) != VCS_ZCODE_DEV_OK) {
        free(wire);
        return ZCL_ERR(-1, "zcode-task-cas-miss-or-corrupt");
    }
    free(wire); wire = NULL; wire_len = 0;
    uint8_t checked[32];
    if (vcs_zcode_task_root(task, checked) != VCS_ZCODE_DEV_OK ||
        memcmp(checked, task_root, 32) != 0 ||
        vcs_zcode_task_validate_at(task, now) != VCS_ZCODE_DEV_OK)
        return ZCL_ERR(-1, "zcode-task-stale-or-expired");
    if (vcs_object_load_raw(workspace, candidate_root, &wire, &wire_len) != 0 ||
        vcs_zcode_candidate_parse(wire, wire_len, candidate) !=
            VCS_ZCODE_DEV_OK) {
        free(wire);
        return ZCL_ERR(-1, "zcode-candidate-cas-miss-or-corrupt");
    }
    free(wire); wire = NULL; wire_len = 0;
    if (vcs_zcode_candidate_root(candidate, checked) != VCS_ZCODE_DEV_OK ||
        memcmp(checked, candidate_root, 32) != 0 ||
        vcs_zcode_candidate_validate_for_task(task, candidate, now) !=
            VCS_ZCODE_DEV_OK)
        return ZCL_ERR(-1, "zcode-candidate-stale");
    struct vcs_zcode_proof_policy_v1 policy;
    if (vcs_object_load_raw(workspace, policy_root, &wire, &wire_len) != 0 ||
        vcs_zcode_proof_policy_parse(wire, wire_len, &policy) !=
            VCS_ZCODE_DEV_OK) {
        free(wire);
        return ZCL_ERR(-1, "zcode-proof-policy-cas-miss-or-corrupt");
    }
    free(wire);
    if (vcs_zcode_proof_policy_root(&policy, checked) != VCS_ZCODE_DEV_OK ||
        memcmp(checked, policy_root, 32) != 0 ||
        memcmp(task->proof_policy_root, policy_root, 32) != 0)
        return ZCL_ERR(-1, "zcode-proof-policy-stale");
    char root_hex[65];
    zcl_hex_encode(task->toolchain_capsule_root, 32, root_hex);
    if (strcmp(root_hex, job->toolchain_sha3) != 0) {
        return ZCL_ERR(-1, "zcode-toolchain-stale");
    }
    zcl_hex_encode(candidate->candidate_source_root, 32, root_hex);
    if (strcmp(root_hex, job->source_cas_sha3) != 0)
        return ZCL_ERR(-1, "zcode-candidate-source-stale");
    *present = true;
    return ZCL_OK;
}

static struct zcl_result bfw_canonical_receipt(
    const char *workspace, const struct db_build_action *action,
    const struct vcs_zcode_task_v1 *task,
    const struct vcs_zcode_candidate_v1 *candidate,
    const uint8_t output_root[32], int64_t started, int64_t finished,
    uint8_t work_kind, uint8_t status, int exit_status,
    const char *confinement,
    const uint8_t signer_secret[32], const uint8_t signer_pubkey[32],
    char out_hex[65])
{
    uint8_t confinement_root[32];
    sha3_256((const uint8_t *)confinement, strlen(confinement),
             confinement_root);
    if (!vcs_object_put_addressed(workspace, confinement_root,
                                  (const uint8_t *)confinement,
                                  strlen(confinement)))
        return ZCL_ERR(-1, "confinement-evidence-cas-store-failed");
    struct vcs_zcode_work_receipt_v1 receipt = {
        .schema_version = VCS_ZCODE_DEV_VERSION,
        .work_kind = work_kind,
        .status = status,
        .exit_status = (uint32_t)exit_status,
        .started_unix = started,
        .finished_unix = finished,
    };
    if (!zcl_hex_decode_lower(action->task_root_sha3, receipt.task_root, 32) ||
        !zcl_hex_decode_lower(action->candidate_root_sha3,
                              receipt.candidate_root, 32) ||
        !zcl_hex_decode_lower(action->action_id, receipt.action_root, 32) ||
        !zcl_hex_decode_lower(action->input_root_sha3, receipt.input_root, 32) ||
        !zcl_hex_decode_lower(action->proof_policy_root_sha3,
                              receipt.proof_policy_root, 32) ||
        !zcl_hex_decode_lower(action->lease_id, receipt.lease_id, 32))
        return ZCL_ERR(-1, "canonical-receipt-roots-invalid");
    memcpy(receipt.output_root, output_root, 32);
    memcpy(receipt.toolchain_capsule_root, task->toolchain_capsule_root, 32);
    memcpy(receipt.evidence_root, output_root, 32);
    memcpy(receipt.confinement_root, confinement_root, 32);
    if (vcs_zcode_work_receipt_seal(&receipt, signer_secret, signer_pubkey) !=
            VCS_ZCODE_DEV_OK ||
        vcs_zcode_work_receipt_validate_for_candidate(
            task, candidate, &receipt, finished) != VCS_ZCODE_DEV_OK ||
        vcs_zcode_work_receipt_verify(&receipt, signer_pubkey) !=
            VCS_ZCODE_DEV_OK)
        return ZCL_ERR(-1, "canonical-work-receipt-refused");
    uint8_t wire[VCS_ZCODE_WORK_RECEIPT_WIRE_BYTES], root[32];
    if (vcs_zcode_work_receipt_serialize(&receipt, wire) != VCS_ZCODE_DEV_OK ||
        vcs_zcode_work_receipt_id(&receipt, root) != VCS_ZCODE_DEV_OK ||
        !vcs_object_put_addressed(workspace, root, wire, sizeof(wire)))
        return ZCL_ERR(-1, "canonical-work-receipt-cas-store-failed");
    zcl_hex_encode(root, 32, out_hex);
    return ZCL_OK;
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
    uint8_t work_kind = vcs_build_action_v1_work_kind(action.kind);
    if ((work_kind != VCS_ZCODE_WORK_BUILD &&
         work_kind != VCS_ZCODE_WORK_TEST) ||
        !bfw_capability_has(worker.capabilities, action.kind))
        return bfw_fail(ndb, action_id, lease_id,
                        "fixed-action-executor-unavailable");
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
    if (!vcs_build_action_v1_fixed_flags_root_for_kind(
            action.kind, fixed_flags) ||
        !vcs_build_action_v1_fixed_environment_root_for_kind(
            action.kind, fixed_environment))
        return bfw_fail(ndb, action_id, lease_id,
                        "fixed-action-descriptor-missing");
    zcl_hex_encode(fixed_flags, 32, fixed_flags_hex);
    zcl_hex_encode(fixed_environment, 32, fixed_environment_hex);
    if (strcmp(fixed_flags_hex, action.flags_sha3) != 0 ||
        strcmp(fixed_environment_hex, action.environment_sha3) != 0)
        return bfw_fail(ndb, action_id, lease_id,
                        "fixed-flags-or-environment-stale");
    struct vcs_zcode_task_v1 zcode_task;
    struct vcs_zcode_candidate_v1 zcode_candidate;
    bool zcode_context = false;
    struct zcl_result context = bfw_load_zcode_context(
        workspace, &job, &action, (int64_t)platform_time_wall_unix(),
        &zcode_task, &zcode_candidate, &zcode_context);
    if (!context.ok)
        return bfw_fail(ndb, action_id, lease_id, context.message);
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
    int64_t work_started = (int64_t)platform_time_wall_unix();
    struct zcl_result start = build_fabric_start(
        ndb, action_id, lease_id, work_started);
    if (!start.ok) { free(input); return start; }
    struct bfw_paths paths = {0};
    struct zcl_result paths_result = bfw_paths_init(
        workspace, lease_id, action.kind, &paths);
    bool test_action = work_kind == VCS_ZCODE_WORK_TEST;
    if (!paths_result.ok ||
        !bfw_write_input(paths.input, input, input_len, test_action)) {
        free(input); bfw_paths_cleanup(&paths);
        return bfw_fail(ndb, action_id, lease_id, "input-materialize-failed");
    }
    free(input);
    char input_arg[BFW_PATH_MAX + 32];
    char output_arg[BFW_PATH_MAX + 32];
    (void)snprintf(input_arg, sizeof(input_arg),
                   test_action ? "--zbuild-test-input=%s"
                               : "--zbuild-input=%s",
                   paths.input);
    (void)snprintf(output_arg, sizeof(output_arg),
                   test_action ? "--zbuild-test-output=%s"
                               : "--zbuild-output=%s",
                   paths.output);
    const char *const argv[] = { paths.worker, input_arg, output_arg,
                                "--require-full-isolation", NULL };
    char capture[BFW_CAPTURE_MAX];
    int rc = zcl_spawn_capture(argv, capture, sizeof(capture),
                               BFW_EXEC_TIMEOUT_MS);
    const char *success_marker = test_action ? "zbuild-test-ok=1"
                                             : "zbuild-ok=1";
    if (rc != 0 || strstr(capture, success_marker) == NULL) {
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
    if (!bfw_binding_current(ndb, &job, &action, lease_id)) {
        bfw_paths_cleanup(&paths);
        return bfw_fail(ndb, action_id, lease_id,
                        "action-binding-changed-during-execution");
    }
    size_t output_len = 0;
    uint8_t *output = bfw_read_output(paths.output, &output_len);
    uint8_t work_status = VCS_ZCODE_WORK_PASS;
    int work_exit_status = 0;
    bool output_valid = test_action
        ? bfw_test_evidence(output, output_len, &work_status,
                            &work_exit_status)
        : bfw_elf_relocatable_x86_64(output, output_len);
    if (!output || !output_valid) {
        free(output); bfw_paths_cleanup(&paths);
        return bfw_fail(ndb, action_id, lease_id,
                        test_action ? "test-evidence-invalid"
                                    : "output-elf-invalid");
    }
    uint8_t output_root[32];
    struct zcl_result stored = bfw_store_artifact(
        workspace, action_id, output, output_len, output_root);
    free(output);
    bfw_paths_cleanup(&paths);
    if (!stored.ok)
        return bfw_fail(ndb, action_id, lease_id, "output-cas-store-failed");
    int64_t work_finished = (int64_t)platform_time_wall_unix();
    if (!bfw_input_root_current(workspace, action.input_root_sha3))
        return bfw_fail(ndb, action_id, lease_id,
                        "input-cas-changed-during-execution");
    if (!bfw_toolchain_current(&job))
        return bfw_fail(ndb, action_id, lease_id,
                        "toolchain-changed-during-execution");
    if (zcode_context) {
        struct vcs_zcode_task_v1 checked_task;
        struct vcs_zcode_candidate_v1 checked_candidate;
        bool checked_present = false;
        context = bfw_load_zcode_context(
            workspace, &job, &action, work_finished, &checked_task,
            &checked_candidate, &checked_present);
        if (!context.ok || !checked_present)
            return bfw_fail(ndb, action_id, lease_id,
                            context.ok ? "zcode-context-disappeared"
                                       : context.message);
        zcode_task = checked_task;
        zcode_candidate = checked_candidate;
    }
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
    const char *confinement = test_action
        ? "landlock=1,seccomp=1,rlimits=1,network=0,test=fixed"
        : "landlock=1,seccomp=1,rlimits=1,network=0,gcc=fixed";
    (void)snprintf(receipt.confinement, sizeof(receipt.confinement),
                   "%s", confinement);
    (void)snprintf(receipt.trust_state, sizeof(receipt.trust_state),
                   "LOCAL_ACCEPTED");
    receipt.exit_status = work_exit_status;
    receipt.created_at = work_finished;
    if (zcode_context) {
        struct zcl_result canonical = bfw_canonical_receipt(
            workspace, &action, &zcode_task, &zcode_candidate, output_root,
            work_started, work_finished, work_kind, work_status,
            work_exit_status, confinement, signer_secret, signer_pubkey,
            receipt.work_receipt_sha3);
        if (!canonical.ok)
            return bfw_fail(ndb, action_id, lease_id, canonical.message);
    }
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
