/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_core.h"
#include "test/test_group_selector.h"
#include "platform/os_proc.h"
#include "test_group_catalog.h"
#include "util/clientversion.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int capture_command(const char *command, char *out, size_t cap)
{
    if (!command || !out || cap == 0)
        return -1;
    out[0] = '\0';
    FILE *pipe = popen(command, "r");
    if (!pipe)
        return -1;
    size_t used = 0;
    unsigned char chunk[4096];
    for (;;) {
        size_t got = fread(chunk, 1, sizeof(chunk), pipe);
        size_t room = cap - used - 1;
        size_t keep = got < room ? got : room;
        if (keep > 0) {
            memcpy(out + used, chunk, keep);
            used += keep;
        }
        if (got == 0)
            break;
    }
    out[used] = '\0';
    int status = pclose(pipe);
    if (status < 0 || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static int test_selector_predicate(void)
{
    int failures = 0;
    TEST("test group selector: exact mode cannot widen to a sibling") {
        ASSERT(test_group_selector_matches("test_api", "api", false));
        ASSERT(test_group_selector_matches("test_native_api_contract", "api",
                                           false));
        ASSERT(!test_group_selector_matches("test_api", "api", true));
        ASSERT(test_group_selector_matches("test_api", "test_api", true));
        ASSERT(!test_group_selector_matches("test_native_api_contract",
                                            "test_api", true));
        ASSERT(!test_group_selector_matches("test_api", "", true));
        ASSERT(!test_group_selector_matches(NULL, "test_api", true));
        ASSERT(test_group_selector_matches_exact_set(
            "test_api", "test_hex_codec,test_api"));
        ASSERT(!test_group_selector_matches_exact_set(
            "test_native_api_contract", "test_hex_codec,test_api"));
        PASS();
    } _test_next:;
    return failures;
}

static int test_registry_exact_resolution(void)
{
    int failures = 0;
    TEST("test group selector: legacy plan id resolves to one canonical id") {
        char out[4096];
        char bounded[64];
        int rc = capture_command("yes x 2>/dev/null | head -c 65536", bounded,
                                 sizeof(bounded));
        ASSERT(rc == 0);
        ASSERT(strlen(bounded) == sizeof(bounded) - 1);

        rc = capture_command(
            "tools/dev/test-group-list.sh --resolve-exact api 2>&1", out,
            sizeof(out));
        ASSERT(rc == 0);
        ASSERT(strcmp(out, "test_api\n") == 0);

        rc = capture_command(
            "tools/dev/test-group-list.sh --resolve-exact test_api 2>&1", out,
            sizeof(out));
        ASSERT(rc == 0);
        ASSERT(strcmp(out, "test_api\n") == 0);

        rc = capture_command(
            "tools/dev/test-group-list.sh --resolve-exact api_missing 2>&1",
            out, sizeof(out));
        ASSERT(rc == 1);
        ASSERT(out[0] == '\0');

        rc = capture_command(
            "tools/dev/test-group-list.sh --check-impact-rules 2>&1", out,
            sizeof(out));
        ASSERT(rc == 0);
        ASSERT(out[0] == '\0');

        char fixture_path[256];
        int fixture_n = snprintf(
            fixture_path, sizeof(fixture_path),
            "test-tmp/impact-rules-negative-%ld.def", (long)getpid());
        ASSERT(fixture_n > 0 && (size_t)fixture_n < sizeof(fixture_path));
        FILE *fixture = fopen(fixture_path, "w");
        ASSERT(fixture != NULL);
        ASSERT(fprintf(fixture,
            "AGENT_IMPACT_RULE(\"lib/test/src/test_hex_codec.c\", \"\")\n"
            "AGENT_IMPACT_RULE(\"lib/test/src/test_api.c\", \"api_missing\")\n"
            "AGENT_IMPACT_RULE(\"lib/test/src/test_hex_codec.c\", \"api\")\n") > 0);
        ASSERT(fclose(fixture) == 0);
        char fixture_command[512];
        int command_n = snprintf(
            fixture_command, sizeof(fixture_command),
            "tools/dev/test-group-list.sh --check-impact-rules %s 2>&1",
            fixture_path);
        ASSERT(command_n > 0 && (size_t)command_n < sizeof(fixture_command));
        rc = capture_command(fixture_command, out, sizeof(out));
        ASSERT(remove(fixture_path) == 0);
        ASSERT(rc == 1);
        ASSERT(strstr(out, "empty impact proof plan") != NULL);
        ASSERT(strstr(out, "non-exact impact proof id: api_missing") != NULL);
        ASSERT(strstr(out, "omits its registered group test_hex_codec") !=
               NULL);

        rc = capture_command(
            "tools/dev/test-group-list.sh --resolve-proof make_lint_gates "
            "api 2>&1", out, sizeof(out));
        ASSERT(rc == 0);
        ASSERT(strstr(out, "test_make_lint_gates\n") != NULL);
        ASSERT(strstr(out, "test_make_lint_gates_shard_01\n") != NULL);
        ASSERT(strstr(out, "test_make_lint_gates_heavy_02\n") != NULL);
        ASSERT(strstr(out, "test_api\n") != NULL);
        ASSERT(strstr(out, "test_native_api_contract\n") != NULL);

        /* Coverage migration is lossless: a legacy family selector becomes
         * exact full IDs, but only after its exact primary is admitted. */
        rc = capture_command(
            "tools/dev/test-group-list.sh --resolve-proof stage_repair 2>&1",
            out, sizeof(out));
        ASSERT(rc == 0);
        ASSERT(strstr(out, "test_stage_repair\n") != NULL);
        ASSERT(strstr(out, "test_stage_repair_coin_backfill\n") != NULL);
        ASSERT(strstr(out, "test_stage_repair_script_refill\n") != NULL);
        ASSERT(strstr(out, "test_stage_repair_tipfin_backfill\n") != NULL);

        rc = capture_command(
            "tools/dev/test-group-list.sh --resolve-proof oracle_policy "
            "2>&1", out, sizeof(out));
        ASSERT(rc == 0);
        ASSERT(strstr(out, "test_oracle_policy\n") != NULL);
        ASSERT(strstr(out, "test_groth16_r1cs_oracle\n") != NULL);
        ASSERT(strstr(out, "test_zclassicd_oracle\n") != NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_native_catalog_resolution(void)
{
    int failures = 0;
    TEST("test group selector: C catalog owns exact proof expansion") {
        ASSERT(zcl_test_group_catalog_count() > 800);
        ASSERT(zcl_test_group_catalog_contains("test_api"));
        ASSERT(zcl_test_group_integration_policy_valid());
        ASSERT(!zcl_test_group_catalog_contains("api"));
        ASSERT(zcl_test_group_source_is_semantic_leaf(
            "lib/test/src/test_stage_repair_coin_backfill.c"));
        ASSERT(!zcl_test_group_source_is_semantic_leaf(
            "lib/test/src/test_api.c"));

        char full[ZCL_TEST_GROUP_FULL_MAX];
        ASSERT(zcl_test_group_resolve_exact("api", full));
        ASSERT(strcmp(full, "test_api") == 0);
        ASSERT(zcl_test_group_resolve_exact("test_api", full));
        ASSERT(strcmp(full, "test_api") == 0);
        ASSERT(!zcl_test_group_resolve_exact("api_missing", full));

        const char *ids[] = { "api", "stage_repair", "oracle_policy" };
        char expanded[16][ZCL_TEST_GROUP_FULL_MAX];
        bool truncated = true;
        size_t total = zcl_test_group_expand_plan(
            ids, sizeof(ids) / sizeof(ids[0]), expanded,
            sizeof(expanded) / sizeof(expanded[0]), &truncated);
        ASSERT(total == 9);
        ASSERT(!truncated);
        bool saw_api = false, saw_native_api = false, saw_stage_coin = false;
        bool saw_groth = false, saw_zclassicd = false;
        for (size_t i = 0; i < total; i++) {
            saw_api |= strcmp(expanded[i], "test_api") == 0;
            saw_native_api |= strcmp(expanded[i],
                                     "test_native_api_contract") == 0;
            saw_stage_coin |= strcmp(expanded[i],
                                      "test_stage_repair_coin_backfill") == 0;
            saw_groth |= strcmp(expanded[i],
                                "test_groth16_r1cs_oracle") == 0;
            saw_zclassicd |= strcmp(expanded[i],
                                    "test_zclassicd_oracle") == 0;
        }
        ASSERT(saw_api && saw_native_api && saw_stage_coin);
        ASSERT(saw_groth && saw_zclassicd);

        char one[1][ZCL_TEST_GROUP_FULL_MAX];
        truncated = false;
        ASSERT(zcl_test_group_expand_plan(ids, 3, one, 1, &truncated) == 9);
        ASSERT(truncated);
        const char *invalid[] = { "api_missing" };
        ASSERT(zcl_test_group_expand_plan(invalid, 1, one, 1, &truncated) ==
               SIZE_MAX);

        const char *lint_ids[] = { "make_lint_gates" };
        char immediate[32][ZCL_TEST_GROUP_FULL_MAX];
        truncated = true;
        size_t immediate_total = zcl_test_group_expand_plan_immediate(
            lint_ids, 1, immediate,
            sizeof(immediate) / sizeof(immediate[0]), &truncated);
        ASSERT(immediate_total == 0);
        ASSERT(!truncated);
        ASSERT(zcl_test_group_is_integration_only(
            "test_make_lint_gates_heavy_01"));
        ASSERT(zcl_test_group_is_integration_only(
            "test_make_lint_gates_heavy_02"));
        ASSERT(zcl_test_group_is_integration_only(
            "test_make_lint_gates_shard_01"));
        ASSERT(zcl_test_group_is_integration_only(
            "test_shielded_payment_gate"));
        ASSERT(zcl_test_group_is_integration_only(
            "test_event_log_kill9"));
        ASSERT(zcl_test_group_is_integration_only(
            "test_event_log_benchmark"));
        ASSERT(!zcl_test_group_is_integration_only("test_event_log"));
        ASSERT(zcl_test_group_catalog_contains(
            "test_zcode_score_receipt_packages"));
        ASSERT(zcl_test_group_catalog_contains(
            "test_zcode_score_receipt_rejections"));
        ASSERT(zcl_test_group_catalog_contains(
            "test_zcode_score_receipt_creation"));
        ASSERT(zcl_test_group_catalog_contains(
            "test_zcode_score_receipt_patronage"));
        ASSERT(zcl_test_group_catalog_contains(
            "test_zcode_score_receipt_reproduction"));
        ASSERT(zcl_test_group_catalog_contains(
            "test_zcode_score_receipt_shadow"));
        for (size_t i = 0; i < immediate_total; i++) {
            ASSERT(strcmp(immediate[i],
                          "test_make_lint_gates_heavy_01") != 0);
            ASSERT(strcmp(immediate[i],
                          "test_make_lint_gates_heavy_02") != 0);
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_latency_group_is_the_only_catalog_exclusive(void)
{
    int failures = 0;
    TEST("command registry latency is isolated without serializing catalog structure") {
        ASSERT(zcl_test_group_catalog_contains(
            "test_command_registry_latency"));
        ASSERT(zcl_test_group_requires_exclusive_run(
            "test_command_registry_latency"));
        ASSERT(!zcl_test_group_requires_exclusive_run(
            "test_command_registry_catalog"));
        ASSERT(!zcl_test_group_plan_selects(
            "command_registry_catalog", "test_command_registry_latency"));
        ASSERT(zcl_test_group_plan_selects(
            "command_registry_latency", "test_command_registry_latency"));
        PASS();
    } _test_next:;
    return failures;
}

static int test_runner_exact_selection(void)
{
    int failures = 0;
    TEST("test group selector: runner exact mode executes exactly one id") {
        /* The make -n dry runs below print the full out-of-date recipe
         * chain, and that chain's SIZE is context-dependent: invoked from a
         * parent make that just built this profile's epoch chain (isolated
         * t-fast) the dry run is a few KB, but invoked cold (the parallel
         * suite, a direct runner process, or after the session lease
         * expires) every stale session/link/stamp recipe prints — measured
         * ~0.6 MB with most objects fresh, bounded by ~2 MB with every
         * object in every chained profile stale. The two `make -n`
         * assertions need BOTH the parse-time admission line (head of
         * output) and the recipe's exact selector (tail), so no head- or
         * tail-truncating capture can work; the buffer must simply exceed
         * the worst case. Static storage: 8 MiB dwarfs the bound, costs no
         * stack, and cannot leak on an assertion-failure exit. */
        static char out[8 * 1024 * 1024];
        char exe[PATH_MAX];
        ASSERT(os_proc_exe_path(exe, sizeof(exe)));
        ASSERT(exe[0] != '\0');
        char command[PATH_MAX + 320];
        int n = snprintf(command, sizeof(command),
                         "\"%s\" --jobs=1 --exact=test_hex_codec "
                         "--no-cache 2>&1", exe);
        ASSERT(n > 0 && (size_t)n < sizeof(command));
        int rc = capture_command(command, out, sizeof(out));
        if (rc != 0)
            fprintf(stderr, "nested exact positive control:\n%s\n", out);
        ASSERT(rc == 0);
        ASSERT(strstr(out, "groups_ran=1") != NULL);
        ASSERT(strstr(out, "groups_failed=0") != NULL);
        ASSERT(strstr(out, "\"schema\":\"zcl.test_phase_receipt.v1\"") !=
               NULL);
        ASSERT(strstr(out, "\"startup_ms\":") != NULL);
        ASSERT(strstr(out, "\"test_body_ms\":") != NULL);

        n = snprintf(command, sizeof(command), "\"%s\" --source-id 2>&1",
                     exe);
        ASSERT(n > 0 && (size_t)n < sizeof(command));
        rc = capture_command(command, out, sizeof(out));
        ASSERT(rc == 0);
        char expected_source[80];
        ASSERT(snprintf(expected_source, sizeof(expected_source), "%s\n",
                        zcl_build_source_id_sha256()) == 65);
        ASSERT(strcmp(out, expected_source) == 0);

        n = snprintf(command, sizeof(command),
                     "\"%s\" --source-record 2>&1", exe);
        ASSERT(n > 0 && (size_t)n < sizeof(command));
        rc = capture_command(command, out, sizeof(out));
        ASSERT(rc == 0);
        char expected_record[160];
        ASSERT(snprintf(expected_record, sizeof(expected_record),
                        "%s 1 %s\n", zcl_build_source_id_sha256(),
                        zcl_build_source_mutation_sha256()) == 132);
        ASSERT(strcmp(out, expected_record) == 0);

        n = snprintf(command, sizeof(command),
                     "\"%s\" --source-id --list 2>&1", exe);
        ASSERT(n > 0 && (size_t)n < sizeof(command));
        rc = capture_command(command, out, sizeof(out));
        ASSERT(rc == 2);
        ASSERT(strstr(out, "Usage:") != NULL);

        n = snprintf(command, sizeof(command),
                     "\"%s\" --jobs=1 "
                     "--exact=test_hex_codec,test_byte_order_codec "
                     "--no-cache 2>&1", exe);
        ASSERT(n > 0 && (size_t)n < sizeof(command));
        rc = capture_command(command, out, sizeof(out));
        ASSERT(rc == 0);
        ASSERT(strstr(out, "groups_ran=2") != NULL);
        ASSERT(strstr(out, "groups_failed=0") != NULL);

        /* "test_api" is a valid exact id. Bare "api" is deliberately not:
         * accepting it here would restore the substring false-green. */
        n = snprintf(command, sizeof(command),
                     "\"%s\" --jobs=1 --exact=api --no-cache 2>&1", exe);
        ASSERT(n > 0 && (size_t)n < sizeof(command));
        rc = capture_command(command, out, sizeof(out));
        ASSERT(rc == 2);
        ASSERT(strstr(out, "--exact contains no registered group: api") !=
               NULL);

        n = snprintf(command, sizeof(command),
                     "\"%s\" --jobs=1 "
                     "--exact=test_hex_codec,api --no-cache 2>&1", exe);
        ASSERT(n > 0 && (size_t)n < sizeof(command));
        rc = capture_command(command, out, sizeof(out));
        ASSERT(rc == 2);
        ASSERT(strstr(out, "--exact contains no registered group: api") !=
               NULL);

        /* Exercise the Make admission seam without running its recipe (and
         * therefore without recursively acquiring the checkout lock). */
        n = snprintf(command, sizeof(command),
                     "make -n t-fast-exact ONLY=api "
                     "TEST_PARALLEL_ARGS= "
                     "TEST_PARALLEL_FAST_CANDIDATE=/bin/true "
                     "TEST_PARALLEL_FAST_ACTIVE=/bin/true "
                     "BUILD_SOURCE_RECORD='%s 1 %s' 2>&1",
                     zcl_build_source_id_sha256(),
                     zcl_build_source_mutation_sha256());
        ASSERT(n > 0 && (size_t)n < sizeof(command));
        rc = capture_command(command, out, sizeof(out));
        ASSERT(rc == 0);
        ASSERT(strstr(out, "resolves to exact set test_api") != NULL);
        ASSERT(strstr(out, "--exact=test_api") != NULL);

        n = snprintf(command, sizeof(command),
                     "make -n t-fast-exact ONLY=api_missing "
                     "TEST_PARALLEL_ARGS= "
                     "TEST_PARALLEL_FAST_CANDIDATE=/bin/true "
                     "TEST_PARALLEL_FAST_ACTIVE=/bin/true "
                     "BUILD_SOURCE_RECORD='%s 1 %s' 2>&1",
                     zcl_build_source_id_sha256(),
                     zcl_build_source_mutation_sha256());
        ASSERT(n > 0 && (size_t)n < sizeof(command));
        rc = capture_command(command, out, sizeof(out));
        ASSERT(rc == 2);
        ASSERT(strstr(out, "ONLY='api_missing' is not a valid exact registered group set") != NULL);
        PASS();
    } _test_next:;
    return failures;
}

int test_test_group_selector(void)
{
    int failures = 0;
    failures += test_selector_predicate();
    failures += test_registry_exact_resolution();
    failures += test_native_catalog_resolution();
    failures += test_latency_group_is_the_only_catalog_exclusive();
    failures += test_runner_exact_selection();
    return failures;
}
