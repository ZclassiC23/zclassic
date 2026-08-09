/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Pure calculation ABI for the verified C23 corpus projection. */

#ifndef ZCL_SERVICES_ZCODE_C23_CORPUS_SERVICE_H
#define ZCL_SERVICES_ZCODE_C23_CORPUS_SERVICE_H

#include "vcs/zcode_c23_corpus.h"

#include <stdbool.h>
#include <stdint.h>

#define ZCODE_C23_CORPUS_SERVICE_ID "zcode.c23.corpus.v1"
#define ZCODE_C23_CORPUS_ABI_FINGERPRINT "zcode.c23.corpus.abi.v1:50d4d388"
#define ZCODE_C23_CORPUS_SCHEMA_FINGERPRINT \
    "zcode.c23.corpus.schemas.v3:status.v1+show.v1+shard-page.v1"
#define ZCODE_C23_CORPUS_WIRE_FINGERPRINT "c23-rules+shard+checkpoint+productivity.v1"
#define ZCODE_C23_CORPUS_KAT_FINGERPRINT \
    "ae0c059c8c925464a7d9376b17687b207027833f5337dc49944bcd1b55d3be23"

struct zcode_c23_corpus_status_result_v1 {
    bool projection_ready;
    bool lower_bound_checkpoint_present;
    bool global_completeness_claimed;
    uint64_t admitted_production_loc;
    uint64_t admitted_test_loc;
    uint64_t admitted_total_loc;
    uint64_t durably_hosted_loc;
    uint64_t physical_lines;
    uint64_t unique_semantic_units;
    char rules_root[65];
    char blocker[160];
};

struct zcode_c23_corpus_rules_result_v1 {
    bool found;
    bool global_completeness_claimed;
    uint16_t overlap_threshold_bps;
    uint16_t shard_entry_max;
    uint16_t checkpoint_shard_max;
    uint16_t page_max;
    uint16_t publication_batch_max;
    uint8_t durable_ack_count;
    uint8_t durable_operator_group_count;
    uint64_t max_file_bytes;
    uint64_t first_milestone_loc;
    uint64_t second_milestone_loc;
    char root[65];
};

struct zcode_c23_corpus_service_v1 {
    enum vcs_zcode_c23_error (*rules_validate)(
        const struct vcs_zcode_c23_corpus_rules_v1 *rules);
    enum vcs_zcode_c23_error (*shard_validate)(
        const struct vcs_zcode_c23_corpus_shard_v1 *shard);
    enum vcs_zcode_c23_error (*shard_page)(
        const struct vcs_zcode_c23_corpus_shard_v1 *shard,
        const struct vcs_zcode_c23_page_cursor_v1 *cursor, size_t page_size,
        size_t *first_index, size_t *item_count,
        struct vcs_zcode_c23_page_cursor_v1 *next_cursor, bool *has_more);
    enum vcs_zcode_c23_error (*checkpoint_validate)(
        const struct vcs_zcode_c23_corpus_checkpoint_v1 *checkpoint);
    enum vcs_zcode_c23_error (*productivity_validate)(
        const struct vcs_zcode_productivity_receipt_v1 *receipt);
    bool (*render_status)(
        const struct vcs_zcode_c23_corpus_checkpoint_v1 *checkpoint,
        struct zcode_c23_corpus_status_result_v1 *out);
    bool (*render_rules)(const char *requested_root,
                         struct zcode_c23_corpus_rules_result_v1 *out);
};

const struct zcode_c23_corpus_service_v1 *zcode_c23_corpus_service_builtin(void);
struct zcl_hotswap_service_contract;
const struct zcl_hotswap_service_contract *
zcl_native_zcode_corpus_service_contract(void);

#endif /* ZCL_SERVICES_ZCODE_C23_CORPUS_SERVICE_H */
