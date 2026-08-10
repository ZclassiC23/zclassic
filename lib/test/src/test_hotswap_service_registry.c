/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: born-red contract for pure service-island publication. */

#include "test/test_helpers.h"

#include "base/hex.h"
#include "hotswap/hotswap_service.h"
#include "command/native_command.h"
#include "json/json.h"
#include "services/market_purchase_view_service.h"
#include "services/market_moderation_view_service.h"
#include "services/zcode_package_view_service.h"
#include "services/zcode_moderation_view_service.h"

#include <stdatomic.h>
#include <string.h>

struct arithmetic_vtable {
    uint64_t (*sum)(uint64_t, uint64_t);
};

static uint64_t sum_builtin(uint64_t a, uint64_t b) { return a + b; }
static uint64_t sum_candidate(uint64_t a, uint64_t b) { return a + b + 7u; }

static const struct arithmetic_vtable k_builtin = {sum_builtin};
static const struct arithmetic_vtable k_candidate = {sum_candidate};

static bool frozen_kat(const void *vtable, char *why, size_t why_sz)
{
    const struct arithmetic_vtable *v = vtable;
    if (!v || !v->sum || v->sum(20, 22) != 49) {
        if (why && why_sz) snprintf(why, why_sz, "frozen vector 20+22 failed");
        return false;
    }
    return true;
}

static const struct zcl_hotswap_service_contract k_contract = {
    .service_id = "test.arithmetic.v1",
    .source_tu = "app/services/src/test_arithmetic.c",
    .abi_version = ZCL_HOTSWAP_SERVICE_ABI_V1,
    .vtable_size = sizeof(struct arithmetic_vtable),
    .abi_fingerprint = "abi-1",
    .schema_fingerprint = "schema-1",
    .wire_fingerprint = "wire-1",
    .kat_fingerprint = "kat-1",
    .frozen_kat = frozen_kat,
};

static struct zcl_hotswap_service_candidate candidate(void)
{
    return (struct zcl_hotswap_service_candidate) {
        .service_id = "test.arithmetic.v1",
        .source_tu = "app/services/src/test_arithmetic.c",
        .abi_version = ZCL_HOTSWAP_SERVICE_ABI_V1,
        .vtable_size = sizeof(struct arithmetic_vtable),
        .abi_fingerprint = "abi-1",
        .schema_fingerprint = "schema-1",
        .wire_fingerprint = "wire-1",
        .kat_fingerprint = "kat-1",
        .vtable = &k_candidate,
    };
}

static int t_publish_and_lease(void)
{
    int failures = 0;
    TEST("service publication is atomic, versioned, and reader-quiescent") {
        zcl_hotswap_service_reset();
        struct zcl_hotswap_service_candidate c = candidate();
        struct zcl_hotswap_service_report report = {0};
        ASSERT(zcl_hotswap_service_publish(&k_contract, &c, true, &report));
        ASSERT(report.activated);
        ASSERT(report.probed);
        ASSERT_EQ(report.generation, 1u);

        struct zcl_hotswap_service_lease lease = {0};
        const struct arithmetic_vtable *active =
            zcl_hotswap_service_acquire("test.arithmetic.v1", &lease);
        ASSERT(active != NULL);
        ASSERT_EQ(active->sum(1, 2), 10u);
        ASSERT(zcl_hotswap_service_publish(&k_contract, &c, true, &report));
        ASSERT_EQ(report.generation, 2u);
        ASSERT(!zcl_hotswap_service_all_retired_quiesced());

        c.vtable = &k_builtin;
        ASSERT(!zcl_hotswap_service_publish(&k_contract, &c, true, &report));
        ASSERT_EQ(strcmp(report.stage, "kat"), 0);
        ASSERT_EQ(zcl_hotswap_service_generation(), 2u);
        zcl_hotswap_service_release(&lease);
        ASSERT(zcl_hotswap_service_all_retired_quiesced());
        PASS();
    } _test_next:;
    return failures;
}

static int t_contract_drift_restarts(void)
{
    int failures = 0;
    TEST("ABI, schema, wire, or KAT drift routes to DEV_RESTART") {
        const char *fields[] = {"abi", "schema", "wire", "kat"};
        for (size_t i = 0; i < 4; i++) {
            struct zcl_hotswap_service_candidate c = candidate();
            if (i == 0) c.abi_fingerprint = "changed";
            if (i == 1) c.schema_fingerprint = "changed";
            if (i == 2) c.wire_fingerprint = "changed";
            if (i == 3) c.kat_fingerprint = "changed";
            struct zcl_hotswap_service_report report = {0};
            ASSERT(!zcl_hotswap_service_publish(&k_contract, &c, true,
                                                &report));
            ASSERT(report.dev_restart);
            ASSERT_EQ(strcmp(report.stage, fields[i]), 0);
            ASSERT(!report.activated);
        }
        PASS();
    } _test_next:;
    return failures;
}

static int t_manifest_mapping(void)
{
    int failures = 0;
    TEST("service source and private header map to one resident-owned probe") {
        ASSERT_STR_EQ(zcl_hotswap_service_source_for_path(
                          "app/services/src/zcode_c23_corpus_service.c"),
                      "app/services/src/zcode_c23_corpus_service.c");
        ASSERT(zcl_hotswap_service_source_for_path(
                   "app/services/include/services/zcode_c23_corpus_service.h")
               == NULL);
        ASSERT_STR_EQ(zcl_hotswap_service_contract_source_for_path(
                          "app/services/include/services/zcode_c23_corpus_service.h"),
                      "app/services/src/zcode_c23_corpus_service.c");
        ASSERT(zcl_hotswap_service_contract_source_for_path(
                   "app/services/src/zcode_c23_corpus_service.c") == NULL);
        ASSERT_STR_EQ(zcl_hotswap_service_probe_for_source(
                          "app/services/src/zcode_c23_corpus_service.c"),
                      "zcode.commons.corpus.show");
        ASSERT_STR_EQ(zcl_hotswap_service_source_for_path(
                          "app/services/src/zcode_c23_economics_service.c"),
                      "app/services/src/zcode_c23_economics_service.c");
        ASSERT_STR_EQ(zcl_hotswap_service_contract_source_for_path(
                          "app/services/include/services/zcode_c23_economics_service.h"),
                      "app/services/src/zcode_c23_economics_service.c");
        ASSERT_STR_EQ(zcl_hotswap_service_source_for_path(
                          "app/services/src/zcode_c23_economics_internal.h"),
                      "app/services/src/zcode_c23_economics_service.c");
        ASSERT_STR_EQ(zcl_hotswap_service_probe_for_source(
                          "app/services/src/zcode_c23_economics_service.c"),
                      "zcode.commons.economics.status");
        ASSERT_STR_EQ(zcl_hotswap_service_source_for_path(
                          "app/services/src/market_purchase_view_service.c"),
                      "app/services/src/market_purchase_view_service.c");
        ASSERT(zcl_hotswap_service_source_for_path(
                   "app/services/include/services/market_purchase_view_service.h")
               == NULL);
        ASSERT_STR_EQ(zcl_hotswap_service_contract_source_for_path(
                          "app/services/include/services/market_purchase_view_service.h"),
                      "app/services/src/market_purchase_view_service.c");
        ASSERT_STR_EQ(zcl_hotswap_service_probe_for_source(
                          "app/services/src/market_purchase_view_service.c"),
                      "app.market.purchase.guide");
        ASSERT_STR_EQ(zcl_hotswap_service_source_for_path(
                          "app/services/src/market_moderation_view_service.c"),
                      "app/services/src/market_moderation_view_service.c");
        ASSERT_STR_EQ(zcl_hotswap_service_source_for_path(
                          "app/services/src/market_moderation_view_internal.h"),
                      "app/services/src/market_moderation_view_service.c");
        ASSERT(zcl_hotswap_service_source_for_path(
                   "app/services/include/services/market_moderation_view_service.h")
               == NULL);
        ASSERT_STR_EQ(zcl_hotswap_service_contract_source_for_path(
                          "app/services/include/services/market_moderation_view_service.h"),
                      "app/services/src/market_moderation_view_service.c");
        ASSERT_STR_EQ(zcl_hotswap_service_probe_for_source(
                          "app/services/src/market_moderation_view_service.c"),
                      "app.market.moderation.guide");
        ASSERT_STR_EQ(zcl_hotswap_service_source_for_path(
                          "app/services/src/zcode_package_view_service.c"),
                      "app/services/src/zcode_package_view_service.c");
        ASSERT(zcl_hotswap_service_source_for_path(
                   "app/services/include/services/zcode_package_view_service.h")
               == NULL);
        ASSERT_STR_EQ(zcl_hotswap_service_contract_source_for_path(
                          "app/services/include/services/zcode_package_view_service.h"),
                      "app/services/src/zcode_package_view_service.c");
        ASSERT_STR_EQ(zcl_hotswap_service_probe_for_source(
                          "app/services/src/zcode_package_view_service.c"),
                      "zcode.package.guide");
        ASSERT_STR_EQ(zcl_hotswap_service_source_for_path(
                          "app/services/src/zcode_moderation_view_service.c"),
                      "app/services/src/zcode_moderation_view_service.c");
        ASSERT(zcl_hotswap_service_source_for_path(
                   "app/services/include/services/zcode_moderation_view_service.h")
               == NULL);
        ASSERT_STR_EQ(zcl_hotswap_service_contract_source_for_path(
                          "app/services/include/services/zcode_moderation_view_service.h"),
                      "app/services/src/zcode_moderation_view_service.c");
        ASSERT_STR_EQ(zcl_hotswap_service_probe_for_source(
                          "app/services/src/zcode_moderation_view_service.c"),
                      "zcode.moderation.status");
        ASSERT(zcl_hotswap_service_source_for_path(
                   "lib/storage/src/storage.c") == NULL);
        PASS();
    } _test_next:;
    return failures;
}

static bool candidate_market_guide(
    struct market_purchase_guide_result_v1 *out)
{
    if (!market_purchase_view_service_builtin()->render_guide(out))
        return false;
    snprintf(out->next_command, sizeof(out->next_command), "%s",
             "candidate marketplace service generation is active");
    return true;
}

static int t_market_purchase_view(void)
{
    int failures = 0;
    TEST("marketplace calculations publish while payment authority stays static") {
        zcl_hotswap_service_reset();
        const struct market_purchase_view_service_v1 *builtin =
            market_purchase_view_service_builtin();
        struct market_purchase_error_result_v1 classified;
        char commit[192];
        ASSERT(builtin->classify_error("COMMIT_BUSY", &classified));
        ASSERT(classified.known);
        ASSERT_EQ(classified.error_class, MARKET_PURCHASE_ERROR_TRANSIENT);
        ASSERT(builtin->render_commit_input(
            "prod",
            "ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789",
            commit, sizeof(commit)));
        ASSERT(strstr(commit, "\"wallet_scope\":\"prod\"") != NULL);
        ASSERT(strstr(commit,
            "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789")
            != NULL);

        struct market_purchase_view_service_v1 candidate_service = *builtin;
        candidate_service.render_guide = candidate_market_guide;
        struct zcl_hotswap_service_candidate service_candidate = {
            .service_id = MARKET_PURCHASE_VIEW_SERVICE_ID,
            .source_tu = "app/services/src/market_purchase_view_service.c",
            .abi_version = ZCL_HOTSWAP_SERVICE_ABI_V1,
            .vtable_size = sizeof(candidate_service),
            .abi_fingerprint = MARKET_PURCHASE_VIEW_ABI_FINGERPRINT,
            .schema_fingerprint = MARKET_PURCHASE_VIEW_SCHEMA_FINGERPRINT,
            .wire_fingerprint = MARKET_PURCHASE_VIEW_WIRE_FINGERPRINT,
            .kat_fingerprint = MARKET_PURCHASE_VIEW_KAT_FINGERPRINT,
            .vtable = &candidate_service,
        };
        struct zcl_hotswap_service_report report = {0};
        ASSERT(zcl_hotswap_service_publish(
            zcl_native_market_purchase_view_service_contract(),
            &service_candidate, true, &report));
        ASSERT(report.probed);

        struct json_value input;
        json_init(&input);
        json_set_object(&input);
        struct zcl_command_request request = {.input = &input};
        struct zcl_command_reply reply;
        zcl_command_reply_init(&reply, "zcl.app_market_purchase_guide.v1");
        zcl_native_handle_market_purchase_guide(&request, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_EQ(json_get_int(json_get(&reply.data, "service_generation")),
                  1);
        ASSERT(!json_get_bool(json_get(&reply.data, "effects_swappable")));
        ASSERT(json_get_bool(json_get(&reply.data,
                                      "payment_authority_static")));
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "next_command")),
                      "candidate marketplace service generation is active");
        zcl_command_reply_free(&reply);
        json_free(&input);
        zcl_hotswap_service_reset();
        PASS();
    } _test_next:;
    return failures;
}

static bool candidate_moderation_guide(
    struct market_moderation_guide_result_v1 *out)
{
    if (!market_moderation_view_service_builtin()->render_guide(out))
        return false;
    snprintf(out->next_command, sizeof(out->next_command), "%s",
             "candidate moderation service generation is active");
    return true;
}

static int t_market_moderation_view(void)
{
    int failures = 0;
    TEST("market moderation visibility swaps while policy authority stays static") {
        zcl_hotswap_service_reset();
        const struct market_moderation_view_service_v1 *builtin =
            market_moderation_view_service_builtin();
        struct market_moderation_decision_result_v1 decision;
        ASSERT(builtin->decide(MARKET_MODERATION_PROFILE_DEFAULT,
                               MARKET_REVIEW_REVIEWED_OK, &decision));
        ASSERT(decision.valid);
        ASSERT(decision.visible);
        ASSERT(decision.local_view_only);
        ASSERT(decision.wire_unchanged);
        ASSERT(builtin->decide(MARKET_MODERATION_PROFILE_DEFAULT,
                               MARKET_REVIEW_SENSITIVE, &decision));
        ASSERT(decision.valid);
        ASSERT(!decision.visible);

        struct market_moderation_view_service_v1 candidate_service = *builtin;
        candidate_service.render_guide = candidate_moderation_guide;
        struct zcl_hotswap_service_candidate service_candidate = {
            .service_id = MARKET_MODERATION_VIEW_SERVICE_ID,
            .source_tu =
                "app/services/src/market_moderation_view_service.c",
            .abi_version = ZCL_HOTSWAP_SERVICE_ABI_V1,
            .vtable_size = sizeof(candidate_service),
            .abi_fingerprint = MARKET_MODERATION_VIEW_ABI_FINGERPRINT,
            .schema_fingerprint = MARKET_MODERATION_VIEW_SCHEMA_FINGERPRINT,
            .wire_fingerprint = MARKET_MODERATION_VIEW_WIRE_FINGERPRINT,
            .kat_fingerprint = MARKET_MODERATION_VIEW_KAT_FINGERPRINT,
            .vtable = &candidate_service,
        };
        struct zcl_hotswap_service_report report = {0};
        ASSERT(zcl_hotswap_service_publish(
            zcl_native_market_moderation_view_service_contract(),
            &service_candidate, true, &report));
        ASSERT(report.probed);

        struct json_value input;
        json_init(&input);
        json_set_object(&input);
        struct zcl_command_request request = {.input = &input};
        struct zcl_command_reply reply;
        zcl_command_reply_init(&reply,
                               "zcl.app_market_moderation_guide.v1");
        zcl_native_handle_market_moderation_guide(&request, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_EQ(json_get_int(json_get(&reply.data, "service_generation")),
                  1);
        ASSERT(json_get_bool(json_get(&reply.data, "policy_authority_static")));
        ASSERT(json_get_bool(json_get(&reply.data, "wire_unchanged")));
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "next_command")),
                      "candidate moderation service generation is active");
        zcl_command_reply_free(&reply);
        json_free(&input);
        zcl_hotswap_service_reset();
        PASS();
    } _test_next:;
    return failures;
}

static bool candidate_package_guide(
    struct zcode_package_guide_result_v1 *out)
{
    if (!zcode_package_view_service_builtin()->render_guide(out))
        return false;
    snprintf(out->next_command, sizeof(out->next_command), "%s",
             "candidate package service generation is active");
    return true;
}

static int t_zcode_package_view(void)
{
    int failures = 0;
    TEST("package presentation swaps while package authority stays static") {
        zcl_hotswap_service_reset();
        const struct zcode_package_view_service_v1 *builtin =
            zcode_package_view_service_builtin();
        struct vcs_package_index_entry entry = {0};
        snprintf(entry.release_id_hex, sizeof(entry.release_id_hex), "%064x",
                 1);
        snprintf(entry.package_root_hex, sizeof(entry.package_root_hex),
                 "%064x", 2);
        snprintf(entry.name, sizeof(entry.name), "%s", "alice/ring");
        struct zcode_package_view_entry_v1 rendered;
        ASSERT(builtin->render_entry(&entry, &rendered));
        ASSERT(rendered.valid);
        ASSERT_STR_EQ(rendered.name, "alice/ring");

        struct zcode_package_view_service_v1 candidate_service = *builtin;
        candidate_service.render_guide = candidate_package_guide;
        struct zcl_hotswap_service_candidate service_candidate = {
            .service_id = ZCODE_PACKAGE_VIEW_SERVICE_ID,
            .source_tu = "app/services/src/zcode_package_view_service.c",
            .abi_version = ZCL_HOTSWAP_SERVICE_ABI_V1,
            .vtable_size = sizeof(candidate_service),
            .abi_fingerprint = ZCODE_PACKAGE_VIEW_ABI_FINGERPRINT,
            .schema_fingerprint = ZCODE_PACKAGE_VIEW_SCHEMA_FINGERPRINT,
            .wire_fingerprint = ZCODE_PACKAGE_VIEW_WIRE_FINGERPRINT,
            .kat_fingerprint = ZCODE_PACKAGE_VIEW_KAT_FINGERPRINT,
            .vtable = &candidate_service,
        };
        struct zcl_hotswap_service_report report = {0};
        ASSERT(zcl_hotswap_service_publish(
            zcl_native_zcode_package_view_service_contract(),
            &service_candidate, true, &report));
        ASSERT(report.probed);

        struct json_value input;
        json_init(&input);
        json_set_object(&input);
        struct zcl_command_request request = {.input = &input};
        struct zcl_command_reply reply;
        zcl_command_reply_init(&reply, "zcl.zcode_package_guide.v1");
        zcl_native_handle_zcode_package_guide(&request, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_EQ(json_get_int(json_get(&reply.data, "service_generation")),
                  1);
        ASSERT(json_get_bool(json_get(&reply.data, "cas_authority_static")));
        ASSERT(json_get_bool(json_get(&reply.data, "index_reads_static")));
        ASSERT(json_get_bool(json_get(&reply.data, "publication_static")));
        ASSERT(json_get_bool(json_get(&reply.data, "execution_static")));
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "next_command")),
                      "candidate package service generation is active");
        zcl_command_reply_free(&reply);
        json_free(&input);
        zcl_hotswap_service_reset();
        PASS();
    } _test_next:;
    return failures;
}

static bool candidate_moderation_policy(
    const struct vcs_zcode_family_policy_v1 *policy,
    const char *policy_root_hex,
    struct zcode_moderation_policy_view_v1 *out)
{
    if (!zcode_moderation_view_service_builtin()->render_policy(
            policy, policy_root_hex, out))
        return false;
    snprintf(out->policy_summary, sizeof(out->policy_summary), "%s",
             "candidate moderation view generation is active");
    return true;
}

static int t_zcode_moderation_view(void)
{
    int failures = 0;
    TEST("Family policy presentation swaps while enforcement stays static") {
        zcl_hotswap_service_reset();
        const struct zcode_moderation_view_service_v1 *builtin =
            zcode_moderation_view_service_builtin();
        struct vcs_zcode_family_policy_v1 policy;
        struct zcode_moderation_policy_view_v1 view;
        uint8_t root[32];
        char root_hex[65];
        vcs_zcode_family_policy_v1_default(&policy);
        ASSERT_EQ(vcs_zcode_family_policy_v1_root(&policy, root),
                  VCS_ZCODE_COMMONS_V2_OK);
        zcl_hex_encode(root, sizeof(root), root_hex);
        ASSERT(builtin->render_policy(&policy, root_hex, &view));
        ASSERT(view.valid);
        ASSERT_STR_EQ(view.policy_root, root_hex);

        struct zcode_moderation_view_service_v1 candidate_service = *builtin;
        candidate_service.render_policy = candidate_moderation_policy;
        struct zcl_hotswap_service_candidate service_candidate = {
            .service_id = ZCODE_MODERATION_VIEW_SERVICE_ID,
            .source_tu = "app/services/src/zcode_moderation_view_service.c",
            .abi_version = ZCL_HOTSWAP_SERVICE_ABI_V1,
            .vtable_size = sizeof(candidate_service),
            .abi_fingerprint = ZCODE_MODERATION_VIEW_ABI_FINGERPRINT,
            .schema_fingerprint = ZCODE_MODERATION_VIEW_SCHEMA_FINGERPRINT,
            .wire_fingerprint = ZCODE_MODERATION_VIEW_WIRE_FINGERPRINT,
            .kat_fingerprint = ZCODE_MODERATION_VIEW_KAT_FINGERPRINT,
            .vtable = &candidate_service,
        };
        struct zcl_hotswap_service_report report = {0};
        ASSERT(zcl_hotswap_service_publish(
            zcl_native_zcode_moderation_view_service_contract(),
            &service_candidate, true, &report));
        ASSERT(report.probed);

        struct json_value input;
        json_init(&input);
        json_set_object(&input);
        struct zcl_command_request request = {.input = &input};
        struct zcl_command_reply reply;
        zcl_command_reply_init(&reply, "zcl.zcode_moderation_status.v1");
        zcl_native_handle_zcode_moderation_status(&request, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_EQ(json_get_int(json_get(&reply.data,
                                       "view_service_generation")), 1);
        ASSERT(!json_get_bool(json_get(&reply.data,
                                      "enforcement_complete")));
        ASSERT(!json_get_bool(json_get(&reply.data, "effective_default")));
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "policy_summary")),
                      "candidate moderation view generation is active");
        zcl_command_reply_free(&reply);
        json_free(&input);
        zcl_hotswap_service_reset();
        PASS();
    } _test_next:;
    return failures;
}

int test_hotswap_service_registry(void)
{
    int failures = t_publish_and_lease() + t_contract_drift_restarts() +
                   t_manifest_mapping() + t_market_purchase_view() +
                   t_market_moderation_view() + t_zcode_package_view();
    failures += t_zcode_moderation_view();
    zcl_hotswap_service_reset();
    printf("=== hotswap_service_registry: %d failures ===\n", failures);
    return failures;
}
