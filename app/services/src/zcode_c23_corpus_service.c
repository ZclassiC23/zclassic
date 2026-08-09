/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Pure C23 corpus calculations. No storage, clock, RNG, wallet, network,
 * process, or node-global authority belongs in this translation unit. */
// one-result-type-ok:pure-vtable-preserves-versioned-vcs-error-enums

#include "services/zcode_c23_corpus_service.h"

#include "base/checked.h"
#include "base/hex.h"
#include "hotswap/hotswap_service.h"

#include <stdio.h>
#include <string.h>

static enum vcs_zcode_c23_error rules_validate(
    const struct vcs_zcode_c23_corpus_rules_v1 *rules)
{
    return vcs_zcode_c23_corpus_rules_v1_validate(rules);
}

static enum vcs_zcode_c23_error shard_validate(
    const struct vcs_zcode_c23_corpus_shard_v1 *shard)
{
    return vcs_zcode_c23_corpus_shard_v1_validate(shard);
}

static enum vcs_zcode_c23_error checkpoint_validate(
    const struct vcs_zcode_c23_corpus_checkpoint_v1 *checkpoint)
{
    return vcs_zcode_c23_corpus_checkpoint_v1_validate(checkpoint);
}

static enum vcs_zcode_c23_error productivity_validate(
    const struct vcs_zcode_productivity_receipt_v1 *receipt)
{
    return vcs_zcode_productivity_receipt_v1_validate(receipt);
}

static bool render_status(
    const struct vcs_zcode_c23_corpus_checkpoint_v1 *checkpoint,
    struct zcode_c23_corpus_status_result_v1 *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    struct vcs_zcode_c23_corpus_rules_v1 rules;
    uint8_t root[32];
    vcs_zcode_c23_corpus_rules_v1_default(&rules);
    if (vcs_zcode_c23_corpus_rules_v1_root(&rules, root) !=
        VCS_ZCODE_C23_OK)
        return false;
    zcl_hex_encode(root, sizeof(root), out->rules_root);
    if (!checkpoint) {
        (void)snprintf(out->blocker, sizeof(out->blocker),
            "no verified corpus checkpoint projection has been committed");
        return true;
    }
    if (vcs_zcode_c23_corpus_checkpoint_v1_validate(checkpoint) !=
        VCS_ZCODE_C23_OK) {
        (void)snprintf(out->blocker, sizeof(out->blocker),
                       "the selected corpus checkpoint is invalid");
        return true;
    }
    uint64_t total = 0;
    if (!zcl_u64_add(checkpoint->production_loc, checkpoint->test_loc,
                     &total)) {
        (void)snprintf(out->blocker, sizeof(out->blocker),
                       "the selected corpus checkpoint count overflows");
        return true;
    }
    out->projection_ready = true;
    out->lower_bound_checkpoint_present = true;
    out->admitted_production_loc = checkpoint->production_loc;
    out->admitted_test_loc = checkpoint->test_loc;
    out->admitted_total_loc = total;
    out->durably_hosted_loc = checkpoint->durable_loc;
    out->physical_lines = checkpoint->physical_lines;
    out->unique_semantic_units = checkpoint->unique_semantic_units;
    return true;
}

static bool render_rules(const char *requested_root,
                         struct zcode_c23_corpus_rules_result_v1 *out)
{
    if (!requested_root || !out) return false;
    memset(out, 0, sizeof(*out));
    struct vcs_zcode_c23_corpus_rules_v1 rules;
    uint8_t root[32];
    vcs_zcode_c23_corpus_rules_v1_default(&rules);
    if (vcs_zcode_c23_corpus_rules_v1_validate(&rules) != VCS_ZCODE_C23_OK ||
        vcs_zcode_c23_corpus_rules_v1_root(&rules, root) != VCS_ZCODE_C23_OK)
        return false;
    zcl_hex_encode(root, sizeof(root), out->root);
    out->found = strcmp(requested_root, out->root) == 0;
    if (!out->found) return true;
    out->overlap_threshold_bps = rules.overlap_threshold_bps;
    out->shard_entry_max = rules.shard_entry_max;
    out->checkpoint_shard_max = rules.checkpoint_shard_max;
    out->page_max = rules.page_max;
    out->publication_batch_max = rules.publication_batch_max;
    out->durable_ack_count = rules.durable_ack_count;
    out->durable_operator_group_count =
        rules.durable_operator_group_count;
    out->max_file_bytes = rules.max_file_bytes;
    out->first_milestone_loc = rules.first_milestone_loc;
    out->second_milestone_loc = rules.second_milestone_loc;
    return true;
}

static const struct zcode_c23_corpus_service_v1 k_builtin = {
    .rules_validate = rules_validate,
    .shard_validate = shard_validate,
    .checkpoint_validate = checkpoint_validate,
    .productivity_validate = productivity_validate,
    .render_status = render_status,
    .render_rules = render_rules,
};

ZCL_HOTSWAP_SERVICE_EXPORT(
    ZCODE_C23_CORPUS_SERVICE_ID, k_builtin,
    ZCODE_C23_CORPUS_ABI_FINGERPRINT,
    ZCODE_C23_CORPUS_SCHEMA_FINGERPRINT,
    ZCODE_C23_CORPUS_WIRE_FINGERPRINT,
    ZCODE_C23_CORPUS_KAT_FINGERPRINT)

const struct zcode_c23_corpus_service_v1 *zcode_c23_corpus_service_builtin(void)
{
    return &k_builtin;
}
