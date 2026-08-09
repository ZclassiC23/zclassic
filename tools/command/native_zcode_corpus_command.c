/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: fail-closed read surfaces for the verified C23 corpus sprint. */

#include "command/native_command.h"

#include "base/hex.h"
#include "json/json.h"
#include "vcs/zcode_c23_corpus.h"

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

static void corpus_rules_root_hex(char out[65])
{
    struct vcs_zcode_c23_corpus_rules_v1 rules;
    uint8_t root[32] = {0};
    vcs_zcode_c23_corpus_rules_v1_default(&rules);
    (void)vcs_zcode_c23_corpus_rules_v1_root(&rules, root);
    zcl_hex_encode(root, 32, out);
}

void zcl_native_handle_zcode_commons_corpus_status(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply || !corpus_no_keys(request->input)) {
        if (reply) corpus_fail(reply, "BAD_CORPUS_STATUS_INPUT",
            "zcode commons corpus status accepts no input keys");
        return;
    }
    char rules_root[65];
    corpus_rules_root_hex(rules_root);
    (void)json_push_kv_bool(&reply->data, "simulation_only", true);
    (void)json_push_kv_bool(&reply->data, "not_owner_approved", true);
    (void)json_push_kv_str(&reply->data, "rules_root", rules_root);
    (void)json_push_kv_bool(&reply->data, "projection_ready", false);
    (void)json_push_kv_bool(&reply->data,
                            "lower_bound_checkpoint_present", false);
    (void)json_push_kv_int(&reply->data, "admitted_production_loc", 0);
    (void)json_push_kv_int(&reply->data, "admitted_test_loc", 0);
    (void)json_push_kv_int(&reply->data, "admitted_total_loc", 0);
    (void)json_push_kv_int(&reply->data, "durably_hosted_loc", 0);
    (void)json_push_kv_int(&reply->data, "first_milestone_loc",
                           VCS_ZCODE_C23_FIRST_MILESTONE_LOC);
    (void)json_push_kv_int(&reply->data, "second_milestone_loc",
                           VCS_ZCODE_C23_SECOND_MILESTONE_LOC);
    (void)json_push_kv_bool(&reply->data, "global_completeness_claimed",
                            false);
    (void)json_push_kv_str(&reply->data, "blocker",
        "no verified corpus checkpoint projection has been committed");
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
