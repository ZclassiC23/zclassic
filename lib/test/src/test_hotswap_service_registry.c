/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: born-red contract for pure service-island publication. */

#include "test/test_helpers.h"

#include "hotswap/hotswap_service.h"

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
                      "zcode.commons.corpus.status");
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
        ASSERT(zcl_hotswap_service_source_for_path(
                   "lib/storage/src/storage.c") == NULL);
        PASS();
    } _test_next:;
    return failures;
}

int test_hotswap_service_registry(void)
{
    int failures = t_publish_and_lease() + t_contract_drift_restarts() +
                   t_manifest_mapping();
    zcl_hotswap_service_reset();
    printf("=== hotswap_service_registry: %d failures ===\n", failures);
    return failures;
}
