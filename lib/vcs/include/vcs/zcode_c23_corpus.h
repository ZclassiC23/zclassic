/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: canonical simulation-only C23 corpus census objects. */
#ifndef ZCL_VCS_ZCODE_C23_CORPUS_H
#define ZCL_VCS_ZCODE_C23_CORPUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VCS_ZCODE_SOURCE_ASSIGNMENT_V1_DOMAIN \
    "zcl.zcode.source_assignment.v1"
#define VCS_ZCODE_C23_CORPUS_RULES_V1_DOMAIN \
    "zcl.zcode.c23_corpus_rules.v1"

#define VCS_ZCODE_C23_CORPUS_SIMULATION_ONLY (1u << 0)
#define VCS_ZCODE_C23_CORPUS_NOT_OWNER_APPROVED (1u << 1)
#define VCS_ZCODE_C23_CORPUS_REQUIRED_FLAGS \
    (VCS_ZCODE_C23_CORPUS_SIMULATION_ONLY | \
     VCS_ZCODE_C23_CORPUS_NOT_OWNER_APPROVED)

#define VCS_ZCODE_SOURCE_ASSIGNMENT_WIRE_BYTES 328u
#define VCS_ZCODE_C23_CORPUS_RULES_WIRE_BYTES 156u
#define VCS_ZCODE_C23_MAX_FILE_BYTES (UINT64_C(64) * 1024u * 1024u)
#define VCS_ZCODE_C23_OVERLAP_THRESHOLD_BPS 8000u
#define VCS_ZCODE_C23_SHARD_ENTRY_MAX 4096u
#define VCS_ZCODE_C23_CHECKPOINT_SHARD_MAX 4096u
#define VCS_ZCODE_C23_PAGE_MAX 256u
#define VCS_ZCODE_C23_PUBLICATION_BATCH_MAX 256u
#define VCS_ZCODE_C23_DURABLE_ACKS 5u
#define VCS_ZCODE_C23_DURABLE_OPERATOR_GROUPS 3u
#define VCS_ZCODE_C23_FIRST_MILESTONE_LOC UINT64_C(50000000)
#define VCS_ZCODE_C23_SECOND_MILESTONE_LOC UINT64_C(100000000)

enum vcs_zcode_source_kind_v1 {
    VCS_ZCODE_SOURCE_HUMAN_AUTHORED = 1,
    VCS_ZCODE_SOURCE_AI_AUTHORED = 2,
    VCS_ZCODE_SOURCE_CANONICAL_IMPORT = 3,
    VCS_ZCODE_SOURCE_MECHANICAL_GENERATION = 4,
    VCS_ZCODE_SOURCE_VENDOR_MATERIAL = 5,
};

enum vcs_zcode_c23_extension_v1 {
    VCS_ZCODE_C23_EXTENSION_C = 1u << 0,
    VCS_ZCODE_C23_EXTENSION_H = 1u << 1,
    VCS_ZCODE_C23_EXTENSION_DEF = 1u << 2,
};

#define VCS_ZCODE_C23_EXTENSION_MASK \
    (VCS_ZCODE_C23_EXTENSION_C | VCS_ZCODE_C23_EXTENSION_H | \
     VCS_ZCODE_C23_EXTENSION_DEF)

enum vcs_zcode_c23_evidence_v1 {
    VCS_ZCODE_C23_EVIDENCE_API = UINT64_C(1) << 0,
    VCS_ZCODE_C23_EVIDENCE_RECIPE = UINT64_C(1) << 1,
    VCS_ZCODE_C23_EVIDENCE_TESTS = UINT64_C(1) << 2,
    VCS_ZCODE_C23_EVIDENCE_PERMISSIVE_LICENSE = UINT64_C(1) << 3,
    VCS_ZCODE_C23_EVIDENCE_QUALITY_PROFILE = UINT64_C(1) << 4,
    VCS_ZCODE_C23_EVIDENCE_SOURCE_ASSIGNMENT = UINT64_C(1) << 5,
    VCS_ZCODE_C23_EVIDENCE_REPRODUCIBLE = UINT64_C(1) << 6,
    VCS_ZCODE_C23_EVIDENCE_FAMILY_QUORUM = UINT64_C(1) << 7,
    VCS_ZCODE_C23_EVIDENCE_COMPLETE_POSSESSION = UINT64_C(1) << 8,
};

#define VCS_ZCODE_C23_EVIDENCE_REQUIRED_MASK UINT64_C(0x1ff)

enum vcs_zcode_c23_error {
    VCS_ZCODE_C23_OK = 0,
    VCS_ZCODE_C23_NULL,
    VCS_ZCODE_C23_SIZE,
    VCS_ZCODE_C23_MAGIC,
    VCS_ZCODE_C23_VERSION,
    VCS_ZCODE_C23_FLAGS,
    VCS_ZCODE_C23_ENUM,
    VCS_ZCODE_C23_ROOT,
    VCS_ZCODE_C23_ORDER,
    VCS_ZCODE_C23_POLICY,
    VCS_ZCODE_C23_TIME,
    VCS_ZCODE_C23_SIGNATURE,
};

struct vcs_zcode_source_assignment_v1 {
    uint16_t schema_version;
    uint16_t flags;
    uint16_t source_kind;
    uint16_t reserved;
    uint64_t sequence;
    uint64_t assigned_height;
    int64_t assigned_mtp;
    uint8_t source_root[32];
    uint8_t author_binding_root[32];
    uint8_t upstream_source_root[32];
    uint8_t upstream_author_root[32];
    uint8_t license_root[32];
    uint8_t assignment_evidence_root[32];
    uint8_t signer_pubkey[32];
    uint8_t signature[64];
};

struct vcs_zcode_c23_corpus_rules_v1 {
    uint16_t schema_version;
    uint16_t flags;
    uint16_t extension_mask;
    uint16_t overlap_threshold_bps;
    uint16_t shard_entry_max;
    uint16_t checkpoint_shard_max;
    uint16_t page_max;
    uint16_t publication_batch_max;
    uint8_t durable_ack_count;
    uint8_t durable_operator_group_count;
    uint16_t reserved;
    uint64_t max_file_bytes;
    uint64_t first_milestone_loc;
    uint64_t second_milestone_loc;
    uint64_t required_evidence_mask;
    uint8_t syntax_profile_root[32];
    uint8_t semantic_unitizer_root[32];
    uint8_t permissive_license_policy_root[32];
};

const char *vcs_zcode_c23_error_string(enum vcs_zcode_c23_error error);

bool vcs_zcode_source_kind_counts_v1(uint16_t source_kind);
enum vcs_zcode_c23_error vcs_zcode_source_assignment_v1_validate(
    const struct vcs_zcode_source_assignment_v1 *assignment);
enum vcs_zcode_c23_error vcs_zcode_source_assignment_v1_sign(
    struct vcs_zcode_source_assignment_v1 *assignment,
    const uint8_t signer_seed[32]);
enum vcs_zcode_c23_error vcs_zcode_source_assignment_v1_encode(
    const struct vcs_zcode_source_assignment_v1 *assignment,
    uint8_t *wire, size_t wire_capacity, size_t *wire_len);
enum vcs_zcode_c23_error vcs_zcode_source_assignment_v1_decode(
    struct vcs_zcode_source_assignment_v1 *out,
    const uint8_t *wire, size_t wire_len);
enum vcs_zcode_c23_error vcs_zcode_source_assignment_v1_root(
    const struct vcs_zcode_source_assignment_v1 *assignment,
    uint8_t out[32]);

void vcs_zcode_c23_corpus_rules_v1_default(
    struct vcs_zcode_c23_corpus_rules_v1 *rules);
enum vcs_zcode_c23_error vcs_zcode_c23_corpus_rules_v1_validate(
    const struct vcs_zcode_c23_corpus_rules_v1 *rules);
enum vcs_zcode_c23_error vcs_zcode_c23_corpus_rules_v1_encode(
    const struct vcs_zcode_c23_corpus_rules_v1 *rules,
    uint8_t *wire, size_t wire_capacity, size_t *wire_len);
enum vcs_zcode_c23_error vcs_zcode_c23_corpus_rules_v1_decode(
    struct vcs_zcode_c23_corpus_rules_v1 *out,
    const uint8_t *wire, size_t wire_len);
enum vcs_zcode_c23_error vcs_zcode_c23_corpus_rules_v1_root(
    const struct vcs_zcode_c23_corpus_rules_v1 *rules, uint8_t out[32]);

#endif /* ZCL_VCS_ZCODE_C23_CORPUS_H */
