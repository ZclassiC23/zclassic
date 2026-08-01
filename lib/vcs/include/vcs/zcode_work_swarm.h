/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Requester-coordinated, untrusted ZCODE work exchange wire. */

#ifndef ZCL_VCS_ZCODE_WORK_SWARM_H
#define ZCL_VCS_ZCODE_WORK_SWARM_H

#include "vcs/zcode_dev.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VCS_ZCODE_WORK_SWARM_VERSION 1u
#define VCS_ZCODE_WORK_SWARM_HEADER_BYTES 8u
#define VCS_ZCODE_WORK_SWARM_MAX_WIRE_BYTES 592u
#define VCS_ZCODE_WORK_SWARM_MAX_RESULTS 64u
#define VCS_ZCODE_WORK_SWARM_MAX_APPROVED 64u

enum vcs_zcode_work_swarm_type {
    VCS_ZCODE_WORK_SWARM_CAPABILITY = 1,
    VCS_ZCODE_WORK_SWARM_REQUEST = 2,
    VCS_ZCODE_WORK_SWARM_RESULT = 3,
    VCS_ZCODE_WORK_SWARM_CANCEL = 4,
};

enum vcs_zcode_work_target {
    VCS_ZCODE_WORK_TARGET_LINUX_X86_64_V3 = 1,
};

enum vcs_zcode_work_confinement {
    VCS_ZCODE_WORK_CONFINEMENT_LANDLOCK = 1u << 0,
    VCS_ZCODE_WORK_CONFINEMENT_SECCOMP = 1u << 1,
    VCS_ZCODE_WORK_CONFINEMENT_RLIMITS = 1u << 2,
    VCS_ZCODE_WORK_CONFINEMENT_NO_NETWORK = 1u << 3,
};

#define VCS_ZCODE_WORK_CONFINEMENT_V1_MASK \
    (VCS_ZCODE_WORK_CONFINEMENT_LANDLOCK | \
     VCS_ZCODE_WORK_CONFINEMENT_SECCOMP | \
     VCS_ZCODE_WORK_CONFINEMENT_RLIMITS | \
     VCS_ZCODE_WORK_CONFINEMENT_NO_NETWORK)

struct vcs_zcode_work_capability_v1 {
    uint8_t signer_pubkey[32];
    uint8_t toolchain_capsule_root[32];
    uint32_t work_kinds;
    uint32_t target;
    uint32_t confinement;
    uint32_t max_cpu_seconds;
    uint64_t max_memory_bytes;
    uint64_t max_output_bytes;
    uint32_t max_lease_seconds;
    uint16_t slots;
    uint16_t queue_headroom;
    int64_t expires_unix;
    uint8_t signature[64];
};

struct vcs_zcode_work_request_v1 {
    uint64_t request_id;
    uint8_t requester_pubkey[32];
    uint8_t task_root[32];
    /* Zero only for PROPOSE; every proof/review request pins a candidate. */
    uint8_t candidate_root[32];
    uint8_t action_root[32];
    uint8_t input_root[32];
    /* content.v2 package carrying the immutable task/action/input objects */
    uint8_t context_root[32];
    uint8_t proof_policy_root[32];
    uint8_t toolchain_capsule_root[32];
    uint8_t work_kind;
    uint8_t target;
    uint32_t max_cpu_seconds;
    uint64_t max_memory_bytes;
    uint64_t max_output_bytes;
    int64_t deadline_unix;
    uint8_t signature[64];
};

struct vcs_zcode_work_result_v1 {
    uint64_t request_id;
    uint8_t task_root[32];
    uint8_t candidate_root[32];
    uint8_t action_root[32];
    uint8_t output_root[32];
    struct vcs_zcode_work_receipt_v1 receipt;
};

struct vcs_zcode_work_cancel_v1 {
    uint64_t request_id;
    uint8_t task_root[32];
    uint8_t requester_pubkey[32];
    uint8_t signature[64];
};

struct vcs_zcode_work_swarm_message {
    uint8_t type;
    union {
        struct vcs_zcode_work_capability_v1 capability;
        struct vcs_zcode_work_request_v1 request;
        struct vcs_zcode_work_result_v1 result;
        struct vcs_zcode_work_cancel_v1 cancel;
    } body;
};

size_t vcs_zcode_work_swarm_wire_size(
    const struct vcs_zcode_work_swarm_message *message);
bool vcs_zcode_work_swarm_serialize(
    const struct vcs_zcode_work_swarm_message *message,
    uint8_t *out, size_t out_cap, size_t *out_len);
bool vcs_zcode_work_swarm_parse(
    const uint8_t *wire, size_t wire_len,
    struct vcs_zcode_work_swarm_message *out);

/* Requests, cancellations, and capability advertisements are signed so a
 * transport peer cannot mutate authority, limits, or cancellation state. */
bool vcs_zcode_work_capability_seal(
    struct vcs_zcode_work_capability_v1 *capability,
    const uint8_t secret[32], const uint8_t pubkey[32]);
bool vcs_zcode_work_capability_verify(
    const struct vcs_zcode_work_capability_v1 *capability);
bool vcs_zcode_work_request_seal(
    struct vcs_zcode_work_request_v1 *request,
    const uint8_t secret[32], const uint8_t pubkey[32]);
bool vcs_zcode_work_request_verify(
    const struct vcs_zcode_work_request_v1 *request);
bool vcs_zcode_work_cancel_seal(
    struct vcs_zcode_work_cancel_v1 *cancel,
    const uint8_t secret[32], const uint8_t pubkey[32]);
bool vcs_zcode_work_cancel_verify(
    const struct vcs_zcode_work_cancel_v1 *cancel);

/* Remote work is evidence, never authority. This verifies exact request
 * binding and a pinned approved signer. Quorum additionally requires distinct
 * approved signers returning the same output root. */
bool vcs_zcode_work_result_verify(
    const struct vcs_zcode_work_request_v1 *request,
    const struct vcs_zcode_work_result_v1 *result,
    const uint8_t expected_signer[32]);
size_t vcs_zcode_work_result_quorum(
    const struct vcs_zcode_work_request_v1 *request,
    const struct vcs_zcode_work_result_v1 *results, size_t result_count,
    const uint8_t (*approved_signers)[32], size_t approved_count,
    size_t required, uint8_t output_root[32]);

#endif /* ZCL_VCS_ZCODE_WORK_SWARM_H */
