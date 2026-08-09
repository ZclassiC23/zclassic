/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: fail-closed read surfaces for the verified C23 corpus sprint. */

#include "command/native_command.h"

#include "hotswap/hotswap_service.h"
#include "json/json.h"
#include "services/zcode_c23_corpus_service.h"
#include "vcs/zcode_c23_corpus.h"

#include <stdio.h>
#include <string.h>

static bool corpus_no_keys(const struct json_value *input)
{
    return input && input->type == JSON_OBJ && input->num_children == 0;
}

static void corpus_fail(struct zcl_command_reply *reply, const char *code,
                        const char *detail)
{
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, code, "validate", false,
                           false, detail, "zcode.commons.corpus");
}

static bool corpus_service_frozen_kat(const void *opaque, char *why,
                                      size_t why_sz)
{
    const struct zcode_c23_corpus_service_v1 *service = opaque;
    struct zcode_c23_corpus_status_result_v1 status;
    if (!service || !service->rules_validate || !service->shard_validate ||
        !service->checkpoint_validate || !service->productivity_validate ||
        !service->render_status || !service->render_rules ||
        !service->render_status(NULL, &status) ||
        status.projection_ready || status.admitted_total_loc != 0 ||
        strcmp(status.rules_root, ZCODE_C23_CORPUS_KAT_FINGERPRINT) != 0) {
        if (why && why_sz)
            (void)snprintf(why, why_sz,
                           "frozen empty-projection/rules-root vector failed");
        return false;
    }
    struct zcode_c23_corpus_rules_result_v1 rules;
    if (!service->render_rules(ZCODE_C23_CORPUS_KAT_FINGERPRINT, &rules) ||
        !rules.found || rules.global_completeness_claimed ||
        strcmp(rules.root, ZCODE_C23_CORPUS_KAT_FINGERPRINT) != 0 ||
        rules.shard_entry_max != VCS_ZCODE_C23_SHARD_ENTRY_MAX ||
        rules.page_max != VCS_ZCODE_C23_PAGE_MAX) {
        if (why && why_sz)
            (void)snprintf(why, why_sz,
                           "frozen exact-root rules rendering vector failed");
        return false;
    }
    return true;
}

static const struct zcl_hotswap_service_contract k_corpus_contract = {
    .service_id = ZCODE_C23_CORPUS_SERVICE_ID,
    .source_tu = "app/services/src/zcode_c23_corpus_service.c",
    .abi_version = ZCL_HOTSWAP_SERVICE_ABI_V1,
    .vtable_size = sizeof(struct zcode_c23_corpus_service_v1),
    .abi_fingerprint = ZCODE_C23_CORPUS_ABI_FINGERPRINT,
    .schema_fingerprint = ZCODE_C23_CORPUS_SCHEMA_FINGERPRINT,
    .wire_fingerprint = ZCODE_C23_CORPUS_WIRE_FINGERPRINT,
    .kat_fingerprint = ZCODE_C23_CORPUS_KAT_FINGERPRINT,
    .frozen_kat = corpus_service_frozen_kat,
};

const struct zcl_hotswap_service_contract *
zcl_native_zcode_corpus_service_contract(void)
{
    return &k_corpus_contract;
}

void zcl_native_handle_zcode_commons_corpus_status(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply || !corpus_no_keys(request->input)) {
        if (reply) corpus_fail(reply, "BAD_CORPUS_STATUS_INPUT",
            "zcode commons corpus status accepts no input keys");
        return;
    }
    struct zcl_hotswap_service_lease lease = {0};
    const struct zcode_c23_corpus_service_v1 *service =
        zcl_hotswap_service_acquire(ZCODE_C23_CORPUS_SERVICE_ID, &lease);
    if (!service) service = zcode_c23_corpus_service_builtin();
    struct zcode_c23_corpus_status_result_v1 status;
    if (!service->render_status(NULL, &status)) {
        zcl_hotswap_service_release(&lease);
        corpus_fail(reply, "CORPUS_SERVICE_FAILED",
                    "the pure corpus calculation service refused its input");
        return;
    }
    (void)json_push_kv_bool(&reply->data, "simulation_only", true);
    (void)json_push_kv_bool(&reply->data, "not_owner_approved", true);
    (void)json_push_kv_str(&reply->data, "service_id",
                           ZCODE_C23_CORPUS_SERVICE_ID);
    (void)json_push_kv_int(&reply->data, "service_generation",
                           zcl_hotswap_service_generation());
    (void)json_push_kv_str(&reply->data, "rules_root", status.rules_root);
    (void)json_push_kv_bool(&reply->data, "projection_ready",
                            status.projection_ready);
    (void)json_push_kv_bool(&reply->data,
        "lower_bound_checkpoint_present",
        status.lower_bound_checkpoint_present);
    (void)json_push_kv_int(&reply->data, "admitted_production_loc",
                           (int64_t)status.admitted_production_loc);
    (void)json_push_kv_int(&reply->data, "admitted_test_loc",
                           (int64_t)status.admitted_test_loc);
    (void)json_push_kv_int(&reply->data, "admitted_total_loc",
                           (int64_t)status.admitted_total_loc);
    (void)json_push_kv_int(&reply->data, "durably_hosted_loc",
                           (int64_t)status.durably_hosted_loc);
    (void)json_push_kv_int(&reply->data, "physical_lines",
                           (int64_t)status.physical_lines);
    (void)json_push_kv_int(&reply->data, "unique_semantic_units",
                           (int64_t)status.unique_semantic_units);
    (void)json_push_kv_int(&reply->data, "first_milestone_loc",
                           VCS_ZCODE_C23_FIRST_MILESTONE_LOC);
    (void)json_push_kv_int(&reply->data, "second_milestone_loc",
                           VCS_ZCODE_C23_SECOND_MILESTONE_LOC);
    (void)json_push_kv_bool(&reply->data, "global_completeness_claimed",
                            status.global_completeness_claimed);
    if (status.blocker[0])
        (void)json_push_kv_str(&reply->data, "blocker", status.blocker);
    zcl_hotswap_service_release(&lease);
}

static bool lowercase_root(const char *root)
{
    if (!root || strlen(root) != 64) return false;
    for (size_t i = 0; i < 64; i++)
        if (!((root[i] >= '0' && root[i] <= '9') ||
              (root[i] >= 'a' && root[i] <= 'f')))
            return false;
    return true;
}

void zcl_native_handle_zcode_commons_corpus_show(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const struct json_value *root_value =
        request && request->input ? json_get(request->input, "root") : NULL;
    const char *root = json_get_str(root_value);
    if (!request || !reply || !request->input ||
        request->input->type != JSON_OBJ ||
        request->input->num_children != 1 || !lowercase_root(root)) {
        if (reply) corpus_fail(reply, "BAD_CORPUS_SHOW_INPUT",
            "zcode commons corpus show requires one lowercase 64-hex root");
        return;
    }
    struct zcl_hotswap_service_lease lease = {0};
    const struct zcode_c23_corpus_service_v1 *service =
        zcl_hotswap_service_acquire(ZCODE_C23_CORPUS_SERVICE_ID, &lease);
    if (!service) service = zcode_c23_corpus_service_builtin();
    struct zcode_c23_corpus_rules_result_v1 rules;
    if (!service->render_rules(root, &rules)) {
        zcl_hotswap_service_release(&lease);
        corpus_fail(reply, "CORPUS_SERVICE_FAILED",
                    "the pure corpus service refused the exact-root read");
        return;
    }
    (void)json_push_kv_bool(&reply->data, "simulation_only", true);
    (void)json_push_kv_bool(&reply->data, "not_owner_approved", true);
    (void)json_push_kv_bool(&reply->data, "found", rules.found);
    (void)json_push_kv_str(&reply->data, "requested_root", root);
    (void)json_push_kv_str(&reply->data, "root", rules.root);
    (void)json_push_kv_str(&reply->data, "kind", "c23_corpus_rules.v1");
    (void)json_push_kv_str(&reply->data, "service_id",
                           ZCODE_C23_CORPUS_SERVICE_ID);
    (void)json_push_kv_int(&reply->data, "service_generation",
                           zcl_hotswap_service_generation());
    (void)json_push_kv_bool(&reply->data, "global_completeness_claimed",
                            rules.global_completeness_claimed);
    if (rules.found) {
        (void)json_push_kv_str(&reply->data, "counted_extensions",
                               ".c,.h,.def");
        (void)json_push_kv_int(&reply->data, "overlap_threshold_bps",
                               rules.overlap_threshold_bps);
        (void)json_push_kv_int(&reply->data, "shard_entry_max",
                               rules.shard_entry_max);
        (void)json_push_kv_int(&reply->data, "checkpoint_shard_max",
                               rules.checkpoint_shard_max);
        (void)json_push_kv_int(&reply->data, "page_max", rules.page_max);
        (void)json_push_kv_int(&reply->data, "publication_batch_max",
                               rules.publication_batch_max);
        (void)json_push_kv_int(&reply->data, "durable_ack_count",
                               rules.durable_ack_count);
        (void)json_push_kv_int(&reply->data,
                               "durable_operator_group_count",
                               rules.durable_operator_group_count);
        (void)json_push_kv_int(&reply->data, "max_file_bytes",
                               (int64_t)rules.max_file_bytes);
        (void)json_push_kv_int(&reply->data, "first_milestone_loc",
                               (int64_t)rules.first_milestone_loc);
        (void)json_push_kv_int(&reply->data, "second_milestone_loc",
                               (int64_t)rules.second_milestone_loc);
    } else {
        (void)json_push_kv_str(&reply->data, "blocker",
            "the requested root is not the resident frozen C23 corpus rules root");
    }
    zcl_hotswap_service_release(&lease);
}

static void render_impact_unshareable(struct json_value *data)
{
    (void)json_push_kv_bool(data, "simulation_only", true);
    (void)json_push_kv_bool(data, "shareable", false);
    (void)json_push_kv_bool(data, "posted_externally", false);
    (void)json_push_kv_str(data, "required_chain",
        "PROVEN work -> human acceptance -> signed release -> independent Family admission -> complete retrievable package");
    (void)json_push_kv_str(data, "blocker",
        "no current signed productivity basis satisfies the complete chain");
}

void zcl_native_handle_zcode_commons_impact_status(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply || !corpus_no_keys(request->input)) {
        if (reply) corpus_fail(reply, "BAD_IMPACT_STATUS_INPUT",
            "zcode commons impact status accepts no input keys");
        return;
    }
    render_impact_unshareable(&reply->data);
}

void zcl_native_handle_zcode_commons_impact_share(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply || !corpus_no_keys(request->input)) {
        if (reply) corpus_fail(reply, "BAD_IMPACT_SHARE_INPUT",
            "zcode commons impact share accepts no input keys");
        return;
    }
    render_impact_unshareable(&reply->data);
    (void)json_push_kv_bool(&reply->data, "slogan_emitted", false);
}
