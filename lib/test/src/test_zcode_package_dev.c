/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: hermetic preparation, capsule, and detached sealing proofs. */

#include "test/test_core.h"
#include "base/hex.h"
#include "command/native_command.h"
#include "json/json.h"
#include "platform/time_compat.h"
#include "util/file_tree_ops.h"
#include "vcs/package_capsule.h"
#include "vcs/package_prepare.h"
#include "vcs/vcs.h"

#include <secp256k1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool zpd_write(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t len = strlen(text);
    bool ok = fwrite(text, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    return ok;
}

static bool zpd_pubkey(secp256k1_context *ctx, const uint8_t secret[32],
                       uint8_t pubkey[33])
{
    secp256k1_pubkey parsed;
    size_t len = 33;
    return secp256k1_ec_pubkey_create(ctx, &parsed, secret) == 1 &&
           secp256k1_ec_pubkey_serialize(ctx, pubkey, &len, &parsed,
                                        SECP256K1_EC_COMPRESSED) == 1 &&
           len == 33;
}

static bool zpd_signature(secp256k1_context *ctx, const uint8_t secret[32],
                          const uint8_t digest[32], uint8_t out[64])
{
    secp256k1_ecdsa_signature signature, normalized;
    if (secp256k1_ecdsa_sign(ctx, &signature, digest, secret, NULL, NULL) != 1)
        return false;
    (void)secp256k1_ecdsa_signature_normalize(ctx, &normalized, &signature);
    return secp256k1_ecdsa_signature_serialize_compact(ctx, out,
                                                       &normalized) == 1;
}

static void zpd_fixture_cleanup(const char *root)
{
    static const char *const files[] = {
        "link", "special", "LICENSE", "zcode-package.json", "src/x.c",
        "include/x.h", "tests/test.c", ".zvcs/control", ".codeindex/control",
    };
    char path[512];
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        (void)snprintf(path, sizeof(path), "%s/%s", root, files[i]);
        (void)unlink(path);
    }
    static const char *const dirs[] = {
        "src", "include", "tests", ".zvcs", ".codeindex",
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        (void)snprintf(path, sizeof(path), "%s/%s", root, dirs[i]);
        (void)rmdir(path);
    }
    (void)rmdir(root);
}

static bool zpd_fixture(const char *root, bool unknown_key)
{
    char path[512];
    zpd_fixture_cleanup(root);
    if (mkdir(root, 0700) != 0) return false;
    static const char *const dirs[] = { "src", "include", "tests" };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        (void)snprintf(path, sizeof(path), "%s/%s", root, dirs[i]);
        if (mkdir(path, 0700) != 0) return false;
    }
    (void)snprintf(path, sizeof(path), "%s/LICENSE", root);
    if (!zpd_write(path, "MIT\n")) return false;
    (void)snprintf(path, sizeof(path), "%s/src/x.c", root);
    if (!zpd_write(path, "int x(void) { return 1; }\n")) return false;
    (void)snprintf(path, sizeof(path), "%s/include/x.h", root);
    if (!zpd_write(path, "int x(void);\n")) return false;
    (void)snprintf(path, sizeof(path), "%s/tests/test.c", root);
    if (!zpd_write(path, "int main(void) { return 0; }\n")) return false;
    (void)snprintf(path, sizeof(path), "%s/zcode-package.json", root);
    return zpd_write(path, unknown_key
        ? "{\"schema\":1,\"name\":\"zclassic23/fixture\",\"semver\":\"0.1.0-dev.1\",\"language\":\"c23\",\"license\":\"MIT\",\"include_dir\":\"include\",\"source_dir\":\"src\",\"dependencies\":[],\"smuggled\":true}\n"
        : "{\"schema\":1,\"name\":\"zclassic23/fixture\",\"semver\":\"0.1.0-dev.1\",\"language\":\"c23\",\"license\":\"MIT\",\"include_dir\":\"include\",\"source_dir\":\"src\",\"dependencies\":[]}\n");
}

static bool zpd_benchmark_project(const char *root, const char *name,
                                  int value)
{
    ZCL_IGNORE_RESULT(zcl_tree_remove(root), "benchmark fixture reset");
    if (mkdir(root, 0700) != 0) return false;
    char path[512], text[512];
    static const char *const dirs[] = {"src", "include", "tests"};
    for (size_t i = 0; i < 3; i++) {
        (void)snprintf(path, sizeof(path), "%s/%s", root, dirs[i]);
        if (mkdir(path, 0700) != 0) return false;
    }
    (void)snprintf(path, sizeof(path), "%s/LICENSE", root);
    if (!zpd_write(path, "MIT\n")) return false;
    (void)snprintf(path, sizeof(path), "%s/include/x.h", root);
    if (!zpd_write(path, "int x(void);\n")) return false;
    (void)snprintf(text, sizeof(text), "int x(void) { return %d; }\n", value);
    (void)snprintf(path, sizeof(path), "%s/src/x.c", root);
    if (!zpd_write(path, text)) return false;
    (void)snprintf(path, sizeof(path), "%s/tests/test.c", root);
    if (!zpd_write(path, "int main(void) { return 0; }\n")) return false;
    (void)snprintf(text, sizeof(text),
        "{\"schema\":1,\"name\":\"fixture/%s\",\"semver\":\"0.1.0\","
        "\"language\":\"c23\",\"license\":\"MIT\",\"include_dir\":"
        "\"include\",\"source_dir\":\"src\",\"dependencies\":[]}\n",
        name);
    (void)snprintf(path, sizeof(path), "%s/zcode-package.json", root);
    return zpd_write(path, text);
}

struct zpd_benchmark_case {
    const char *kind;
    const char *goal;
    uint8_t project;
    bool refused;
};

static int zpd_test_twelve_task_benchmark(void)
{
    static const struct zpd_benchmark_case cases[] = {
        {"seeded_repair", "Repair seeded parser branch A", 0, false},
        {"seeded_repair", "Repair seeded parser branch B", 1, false},
        {"seeded_repair", "Repair seeded parser branch C", 2, false},
        {"seeded_repair", "Repair seeded return regression", 0, false},
        {"bounded_api", "Add bounded API behavior A", 1, false},
        {"bounded_api", "Add bounded API behavior B", 2, false},
        {"bounded_api", "Add bounded API behavior C", 0, false},
        {"malformed_ub", "Repair malformed input handling", 1, false},
        {"malformed_ub", "Repair portability boundary", 2, false},
        {"malformed_ub", "Repair undefined behavior guard", 0, false},
        {"impossible", "Modify LICENSE outside the write scope", 1, true},
        {"impossible", "Replace package identity outside scope", 2, true},
    };
    int failures = 0;
    TEST("zcode development benchmark: 12 frozen tasks across 3 projects") {
        int64_t benchmark_started = platform_time_monotonic_us();
        char roots[3][256];
        for (int p = 0; p < 3; p++) {
            (void)snprintf(roots[p], sizeof(roots[p]),
                           "test-tmp/zcode-benchmark-%ld-%d",
                           (long)getpid(), p);
            char name[32];
            (void)snprintf(name, sizeof(name), "benchmark-%d", p);
            ASSERT(zpd_benchmark_project(roots[p], name, p + 1));
        }
        uint8_t source_roots[3][32], source_after[32];
        for (int p = 0; p < 3; p++)
            ASSERT(vcs_tree_capture_path(roots[p], source_roots[p]) == VCS_OK);
        size_t compiling = 0, profile = 0, refused = 0, accepted = 0;
        uint64_t selected_bytes = 0, total_bytes = 0, context_us = 0;
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            struct json_value input;
            json_init(&input); json_set_object(&input);
            ASSERT(json_push_kv_str(&input, "workspace",
                                    roots[cases[i].project]));
            ASSERT(json_push_kv_str(&input, "goal", cases[i].goal));
            ASSERT(json_push_kv_str(&input, "profile", "quick"));
            struct zcl_command_request request = {.input = &input};
            struct zcl_command_reply reply;
            zcl_command_reply_init(&reply, "zcl.zcode_benchmark_start.v1");
            zcl_native_handle_zcode_work_start(&request, &reply);
            if (reply.status != ZCL_COMMAND_STATUS_PASSED)
                printf("benchmark case %zu start: %s %s\n", i,
                       reply.error.code, reply.error.message);
            ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
            const struct json_value *context =
                json_get(&reply.data, "selected_context");
            ASSERT(context && context->type == JSON_OBJ);
            selected_bytes += (uint64_t)json_get_int(
                json_get(context, "selected_context_bytes"));
            total_bytes += (uint64_t)json_get_int(
                json_get(context, "total_source_bytes"));
            context_us += (uint64_t)json_get_int(
                json_get(context, "generation_us"));
            char work_id[32];
            (void)snprintf(work_id, sizeof(work_id), "%s",
                json_get_str(json_get(&reply.data, "work_id")));
            zcl_command_reply_free(&reply); json_free(&input);

            json_init(&input); json_set_object(&input);
            ASSERT(json_push_kv_str(&input, "workspace",
                                    roots[cases[i].project]));
            ASSERT(json_push_kv_str(&input, "work", work_id));
            ASSERT(json_push_kv_str(&input, "adapter", "manual"));
            request.input = &input;
            zcl_command_reply_init(&reply, "zcl.zcode_benchmark_run.v1");
            zcl_native_handle_zcode_work_run(&request, &reply);
            if (reply.status != ZCL_COMMAND_STATUS_PASSED)
                printf("benchmark case %zu handoff: %s %s\n", i,
                       reply.error.code, reply.error.message);
            ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
            char candidate[4500];
            (void)snprintf(candidate, sizeof(candidate), "%s",
                json_get_str(json_get(&reply.data, "candidate_workspace")));
            zcl_command_reply_free(&reply); json_free(&input);
            char changed[4600], contents[128];
            if (cases[i].refused) {
                (void)snprintf(changed, sizeof(changed), "%s/LICENSE",
                               candidate);
                ASSERT(zpd_write(changed, "Proprietary\n"));
            } else {
                (void)snprintf(changed, sizeof(changed), "%s/src/x.c",
                               candidate);
                (void)snprintf(contents, sizeof(contents),
                               "int x(void) { return %zu; }\n", i + 10u);
                ASSERT(zpd_write(changed, contents));
            }
            json_init(&input); json_set_object(&input);
            ASSERT(json_push_kv_str(&input, "workspace",
                                    roots[cases[i].project]));
            ASSERT(json_push_kv_str(&input, "work", work_id));
            ASSERT(json_push_kv_str(&input, "adapter", "manual"));
            request.input = &input;
            zcl_command_reply_init(&reply, "zcl.zcode_benchmark_admit.v1");
            zcl_native_handle_zcode_work_run(&request, &reply);
            if (cases[i].refused) {
                ASSERT(reply.status == ZCL_COMMAND_STATUS_FAILED);
                ASSERT(strcmp(reply.error.code, "PATCH_OUTSIDE_SCOPE") == 0);
                refused++;
            } else {
                ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
                ASSERT(strcmp(json_get_str(json_get(&reply.data, "state")),
                              "EVIDENCE_READY") == 0);
                compiling++;
            }
            zcl_command_reply_free(&reply); json_free(&input);

            if (!cases[i].refused) {
                json_init(&input); json_set_object(&input);
                ASSERT(json_push_kv_str(&input, "workspace",
                                        roots[cases[i].project]));
                ASSERT(json_push_kv_str(&input, "work", work_id));
                request.input = &input;
                zcl_command_reply_init(&reply,
                                       "zcl.zcode_benchmark_accept.v1");
                zcl_native_handle_zcode_work_accept(&request, &reply);
                ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
                ASSERT(strcmp(json_get_str(json_get(&reply.data, "state")),
                              "PROVEN") == 0);
                profile++; accepted++;
                zcl_command_reply_free(&reply); json_free(&input);

                json_init(&input); json_set_object(&input);
                ASSERT(json_push_kv_str(&input, "workspace",
                                        roots[cases[i].project]));
                ASSERT(json_push_kv_str(&input, "work", work_id));
                request.input = &input;
                zcl_command_reply_init(&reply,
                                       "zcl.zcode_benchmark_status.v1");
                zcl_native_handle_zcode_work_status(&request, &reply);
                ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
                ASSERT(strcmp(json_get_str(json_get(&reply.data, "goal")),
                              cases[i].goal) == 0);
                ASSERT(strcmp(json_get_str(json_get(&reply.data, "state")),
                              "PROVEN") == 0);
                ASSERT(json_get(&reply.data, "next_safe_command") != NULL);
                zcl_command_reply_free(&reply); json_free(&input);
            }
            char *attempt = strrchr(candidate, '/');
            ASSERT(attempt != NULL); *attempt = '\0';
            ASSERT(zcl_tree_remove(candidate).ok);
        }
        ASSERT(compiling == 10);
        ASSERT(profile == 10);
        ASSERT(accepted == 10);
        ASSERT(refused == 2);
        ASSERT(selected_bytes > 0 && selected_bytes < total_bytes);
        ASSERT(context_us > 0);
        int64_t benchmark_elapsed =
            platform_time_monotonic_us() - benchmark_started;
        ASSERT(benchmark_elapsed > 0);
        printf("benchmark: tasks=12 projects=3 compiling=%zu profile=%zu "
               "accepted=%zu refused=%zu context=%llu/%llu bytes "
               "context_us=%llu elapsed_us=%llu\n",
               compiling, profile, accepted, refused,
               (unsigned long long)selected_bytes,
               (unsigned long long)total_bytes,
               (unsigned long long)context_us,
               (unsigned long long)benchmark_elapsed);
        for (int p = 0; p < 3; p++) {
            ASSERT(vcs_tree_capture_path(roots[p], source_after) == VCS_OK);
            ASSERT(memcmp(source_roots[p], source_after,
                          sizeof(source_after)) == 0);
            ASSERT(zcl_tree_remove(roots[p]).ok);
        }
        PASS();
    } _test_next:;
    return failures;
}

static int zpd_test_base(secp256k1_context *ctx, const uint8_t secret[32],
                         const uint8_t pubkey[33])
{
    int failures = 0;
    TEST("zcode package dev prepare: base tree derives all canonical roots") {
        struct vcs_package_prepare_options options = {
            .dir = "lib/base", .publisher_sequence = 1,
            .reward_address = "", .chain_id = "zclassic-main",
        };
        memcpy(options.publisher_pubkey, pubkey, 33);
        struct vcs_package_prepared prepared;
        char detail[256];
        ASSERT(vcs_package_prepare(&options, &prepared, detail,
                                   sizeof(detail)) == VCS_PACKAGE_PREPARE_OK);
        ASSERT(prepared.manifest.count > 10);
        ASSERT(prepared.recipe.sources.count > 0);
        ASSERT(prepared.capsule.count > 0);
        ASSERT(prepared.lock.count == 1 &&
               prepared.lock.nodes[0].depth == 0 &&
               prepared.lock.nodes[0].direct_deps == 0);
        ASSERT(prepared.release_body_len > 64);
        char root_hex[65];
        zcl_hex_encode(prepared.package_root, 32, root_hex);
        printf("base package root: %s\n", root_hex);

        char pubkey_hex[67];
        zcl_hex_encode(pubkey, 33, pubkey_hex);
        struct json_value prepare_input;
        json_init(&prepare_input); json_set_object(&prepare_input);
        ASSERT(json_push_kv_str(&prepare_input, "dir", "lib/base"));
        ASSERT(json_push_kv_str(&prepare_input, "publisher_pubkey",
                                pubkey_hex));
        ASSERT(json_push_kv_int(&prepare_input, "publisher_sequence", 1));
        struct zcl_command_request prepare_request = {
            .input = &prepare_input,
        };
        struct zcl_command_reply prepare_reply;
        zcl_command_reply_init(&prepare_reply,
                               "zcl.zcode_package_dev_test.v1");
        zcl_native_handle_zcode_package_dev_prepare(&prepare_request,
                                                    &prepare_reply);
        ASSERT(prepare_reply.status == ZCL_COMMAND_STATUS_PASSED);
        const struct json_value *prepared_root =
            json_get(&prepare_reply.data, "package_root");
        ASSERT(prepared_root &&
               strcmp(json_get_str(prepared_root), root_hex) == 0);
        zcl_command_reply_free(&prepare_reply);
        json_free(&prepare_input);

        struct vcs_package_capsule parsed;
        ASSERT(vcs_package_capsule_parse(prepared.capsule_wire,
                                         prepared.capsule_wire_len,
                                         &parsed) == VCS_PACKAGE_CAPSULE_OK);
        uint8_t parsed_root[32];
        ASSERT(vcs_package_capsule_root(&parsed, parsed_root) ==
               VCS_PACKAGE_CAPSULE_OK);
        ASSERT(memcmp(parsed_root, prepared.capsule_root, 32) == 0);
        for (size_t n = 0; n < prepared.capsule_wire_len; n++)
            ASSERT(vcs_package_capsule_parse(prepared.capsule_wire, n,
                                             &parsed) !=
                   VCS_PACKAGE_CAPSULE_OK);
        uint8_t *trailing = malloc(prepared.capsule_wire_len + 1u); // raw-alloc-ok:test-fixture
        ASSERT(trailing != NULL);
        memcpy(trailing, prepared.capsule_wire, prepared.capsule_wire_len);
        trailing[prepared.capsule_wire_len] = 0;
        ASSERT(vcs_package_capsule_parse(trailing,
                                         prepared.capsule_wire_len + 1u,
                                         &parsed) ==
               VCS_PACKAGE_CAPSULE_ERR_TRAILING);
        free(trailing);

        uint8_t signature[64];
        ASSERT(zpd_signature(ctx, secret, prepared.signing_digest, signature));
        char *body_hex = malloc(prepared.release_body_len * 2u + 1u); // raw-alloc-ok:test-fixture
        char signature_hex[129];
        ASSERT(body_hex != NULL);
        zcl_hex_encode(prepared.release_body, prepared.release_body_len,
                       body_hex);
        zcl_hex_encode(signature, sizeof(signature), signature_hex);
        struct json_value input;
        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "release_body_hex", body_hex));
        ASSERT(json_push_kv_str(&input, "signature_hex", signature_hex));
        struct zcl_command_request request = { .input = &input };
        struct zcl_command_reply reply;
        zcl_command_reply_init(&reply, "zcl.zcode_package_dev_test.v1");
        zcl_native_handle_zcode_package_dev_seal(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        const struct json_value *status = json_get(&reply.data,
                                                    "signature_status");
        ASSERT(status && strcmp(json_get_str(status), "verified") == 0);
        zcl_command_reply_free(&reply); json_free(&input); free(body_hex);

        struct vcs_package_prepare_options sha_options = {
            .dir = "lib/sha3", .publisher_sequence = 2,
            .reward_address = "", .chain_id = "zclassic-main",
        };
        memcpy(sha_options.publisher_pubkey, pubkey, 33);
        struct vcs_package_prepared sha_prepared;
        ASSERT(vcs_package_prepare(&sha_options, &sha_prepared, detail,
                                   sizeof(detail)) == VCS_PACKAGE_PREPARE_OK);
        ASSERT(sha_prepared.lock.count == 2);
        ASSERT(sha_prepared.lock.nodes[0].depth == 1 &&
               sha_prepared.lock.nodes[0].direct_deps == 0 &&
               memcmp(sha_prepared.lock.nodes[0].root,
                      prepared.package_root, 32) == 0);
        ASSERT(sha_prepared.lock.nodes[1].depth == 0 &&
               sha_prepared.lock.nodes[1].direct_deps == 1);
        zcl_hex_encode(sha_prepared.package_root, 32, root_hex);
        printf("sha3 package root: %s\n", root_hex);
        vcs_package_prepared_free(&sha_prepared);
        vcs_package_prepared_free(&prepared);
        PASS();
    } _test_next:;
    return failures;
}

static int zpd_test_control_stores(const uint8_t pubkey[33])
{
    int failures = 0;
    TEST("zcode package dev prepare: local control stores do not alter package roots") {
        char root[256], path[320];
        (void)snprintf(root, sizeof(root),
                       "test-tmp/zcode-package-control-%ld", (long)getpid());
        ASSERT(zpd_fixture(root, false));
        struct vcs_package_prepare_options options = {
            .dir = root, .publisher_sequence = 1,
            .reward_address = "", .chain_id = "zclassic-main",
        };
        memcpy(options.publisher_pubkey, pubkey, 33);
        struct vcs_package_prepared before, after;
        char detail[256];
        ASSERT(vcs_package_prepare(&options, &before, detail,
                                   sizeof(detail)) == VCS_PACKAGE_PREPARE_OK);
        (void)snprintf(path, sizeof(path), "%s/.zvcs", root);
        ASSERT(mkdir(path, 0700) == 0);
        (void)snprintf(path, sizeof(path), "%s/.zvcs/control", root);
        ASSERT(zpd_write(path, "local vcs state\n"));
        (void)snprintf(path, sizeof(path), "%s/.codeindex", root);
        ASSERT(mkdir(path, 0700) == 0);
        (void)snprintf(path, sizeof(path), "%s/.codeindex/control", root);
        ASSERT(zpd_write(path, "derived index state\n"));
        ASSERT(vcs_package_prepare(&options, &after, detail,
                                   sizeof(detail)) == VCS_PACKAGE_PREPARE_OK);
        ASSERT(memcmp(before.package_root, after.package_root, 32) == 0);
        vcs_package_prepared_free(&after);
        vcs_package_prepared_free(&before);
        zpd_fixture_cleanup(root);
        PASS();
    } _test_next:;
    return failures;
}

static int zpd_test_fail_closed(const uint8_t pubkey[33])
{
    int failures = 0;
    TEST("zcode package dev prepare: symlink, special and unknown fields fail closed") {
        char root[256], path[320];
        (void)snprintf(root, sizeof(root),
                       "test-tmp/zcode-package-dev-%ld", (long)getpid());
        ASSERT(zpd_fixture(root, false));
        (void)snprintf(path, sizeof(path), "%s/link", root);
        ASSERT(symlink("src/x.c", path) == 0);
        struct vcs_package_prepare_options options = {
            .dir = root, .publisher_sequence = 1,
            .reward_address = "", .chain_id = "zclassic-main",
        };
        memcpy(options.publisher_pubkey, pubkey, 33);
        struct vcs_package_prepared prepared;
        char detail[256];
        (void)snprintf(path, sizeof(path), "%s/.zvcs", root);
        ASSERT(symlink("src", path) == 0);
        ASSERT(vcs_package_prepare(&options, &prepared, detail,
                                   sizeof(detail)) ==
               VCS_PACKAGE_PREPARE_ERR_FILE_TYPE);
        ASSERT(unlink(path) == 0);
        (void)snprintf(path, sizeof(path), "%s/link", root);
        ASSERT(vcs_package_prepare(&options, &prepared, detail,
                                   sizeof(detail)) ==
               VCS_PACKAGE_PREPARE_ERR_FILE_TYPE);
        ASSERT(unlink(path) == 0);
        (void)snprintf(path, sizeof(path), "%s/special", root);
        ASSERT(mkfifo(path, 0600) == 0);
        ASSERT(vcs_package_prepare(&options, &prepared, detail,
                                   sizeof(detail)) ==
               VCS_PACKAGE_PREPARE_ERR_FILE_TYPE);
        ASSERT(unlink(path) == 0);
        zpd_fixture_cleanup(root);
        ASSERT(zpd_fixture(root, true));
        ASSERT(vcs_package_prepare(&options, &prepared, detail,
                                   sizeof(detail)) ==
               VCS_PACKAGE_PREPARE_ERR_META);
        zpd_fixture_cleanup(root);
        PASS();
    } _test_next:;
    return failures;
}

static int zpd_test_project_inspect(void)
{
    int failures = 0;
    TEST("zcode project inspect: human summary is read-only and root-free by default") {
        char root[256], hidden[320];
        (void)snprintf(root, sizeof(root),
                       "test-tmp/zcode-project-inspect-%ld", (long)getpid());
        ASSERT(zpd_fixture(root, false));
        (void)snprintf(hidden, sizeof(hidden), "%s/.zvcs", root);
        ASSERT(access(hidden, F_OK) != 0);

        struct json_value input;
        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        struct zcl_command_request request = { .input = &input };
        struct zcl_command_reply reply;
        zcl_command_reply_init(&reply, "zcl.zcode_project_inspect_test.v1");
        zcl_native_handle_zcode_project_inspect(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(access(hidden, F_OK) != 0);

        const struct json_value *name = json_get(&reply.data, "name");
        const struct json_value *layout = json_get(&reply.data, "layout");
        const struct json_value *headers = layout
            ? json_get(layout, "public_headers") : NULL;
        const struct json_value *sources = layout
            ? json_get(layout, "sources") : NULL;
        const struct json_value *tests = layout
            ? json_get(layout, "tests") : NULL;
        const struct json_value *profile =
            json_get(&reply.data, "suggested_profile");
        const struct json_value *profile_detail =
            json_get(&reply.data, "proof_profile");
        const struct json_value *exact_policy = profile_detail
            ? json_get(profile_detail, "exact_policy") : NULL;
        const struct json_value *expert = json_get(&reply.data, "expert");
        ASSERT(name && strcmp(json_get_str(name), "zclassic23/fixture") == 0);
        ASSERT(layout && layout->type == JSON_OBJ);
        ASSERT(headers && headers->type == JSON_ARR &&
               headers->num_children == 1);
        ASSERT(sources && sources->type == JSON_ARR &&
               sources->num_children == 1);
        ASSERT(tests && tests->type == JSON_ARR && tests->num_children == 1);
        ASSERT(profile && strcmp(json_get_str(profile), "standard") == 0);
        ASSERT(profile_detail && profile_detail->type == JSON_OBJ);
        ASSERT(json_get(profile_detail, "warning_fatal") &&
               json_get_bool(json_get(profile_detail, "warning_fatal")));
        ASSERT(json_get(profile_detail, "sanitizers") &&
               json_get_bool(json_get(profile_detail, "sanitizers")));
        ASSERT(exact_policy && exact_policy->type == JSON_OBJ);
        ASSERT(json_get(exact_policy, "root") != NULL);
        ASSERT(json_get_int(json_get(exact_policy,
                                     "minimum_compile_receipts")) == 2);
        ASSERT(json_get(&reply.data, "recipe_hex") == NULL);
        ASSERT(json_get(&reply.data, "dependency_lock_hex") == NULL);
        ASSERT(expert && expert->type == JSON_OBJ);
        ASSERT(json_get(expert, "package_root") != NULL);
        ASSERT(json_get(&reply.data, "read_only") &&
               json_get_bool(json_get(&reply.data, "read_only")));
        zcl_command_reply_free(&reply);
        json_free(&input);
        zpd_fixture_cleanup(root);

        char absent[256];
        (void)snprintf(absent, sizeof(absent),
                       "test-tmp/zcode-project-absent-%ld", (long)getpid());
        ASSERT(access(absent, F_OK) != 0);
        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", absent));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_project_inspect_test.v1");
        zcl_native_handle_zcode_project_inspect(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_FAILED);
        ASSERT(access(absent, F_OK) != 0);
        zcl_command_reply_free(&reply);
        json_free(&input);
        PASS();
    } _test_next:;
    return failures;
}

static int zpd_test_project_init(void)
{
    int failures = 0;
    TEST("zcode project init: plan is read-only and commit is exact and exclusive") {
        char root[256], meta[320], added[320];
        (void)snprintf(root, sizeof(root),
                       "test-tmp/zcode-project-init-%ld", (long)getpid());
        ASSERT(zpd_fixture(root, false));
        (void)snprintf(meta, sizeof(meta), "%s/zcode-package.json", root);
        ASSERT(unlink(meta) == 0);

        struct json_value input;
        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        struct zcl_command_request request = { .input = &input };
        struct zcl_command_reply reply;
        zcl_command_reply_init(&reply, "zcl.zcode_project_inspect_test.v1");
        zcl_native_handle_zcode_project_inspect(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(json_get(&reply.data, "existing_package_config") &&
               !json_get_bool(json_get(&reply.data,
                                       "existing_package_config")));
        ASSERT(access(meta, F_OK) != 0);
        zcl_command_reply_free(&reply);

        zcl_command_reply_init(&reply, "zcl.zcode_project_init_plan_test.v1");
        zcl_native_handle_zcode_project_init_plan(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        const char *plan_id = json_get_str(json_get(&reply.data, "plan_id"));
        const char *config =
            json_get_str(json_get(&reply.data, "configuration_text"));
        ASSERT(plan_id && strlen(plan_id) == 64);
        ASSERT(config && strstr(config, "\"name\": \"local/zcode-project-init-") != NULL);
        ASSERT(access(meta, F_OK) != 0);
        char saved_plan[65];
        (void)snprintf(saved_plan, sizeof(saved_plan), "%s", plan_id);
        zcl_command_reply_free(&reply);
        json_free(&input);

        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "name", "example/parser"));
        ASSERT(json_push_kv_str(&input, "semver", "1.2.3"));
        ASSERT(json_push_kv_str(&input, "license", "MIT"));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_project_init_plan_test.v1");
        zcl_native_handle_zcode_project_init_plan(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(strcmp(json_get_str(json_get(&reply.data, "name")),
                      "example/parser") == 0);
        plan_id = json_get_str(json_get(&reply.data, "plan_id"));
        ASSERT(plan_id && strlen(plan_id) == 64);
        (void)snprintf(saved_plan, sizeof(saved_plan), "%s", plan_id);
        zcl_command_reply_free(&reply);
        json_free(&input);

        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "name", "example/parser"));
        ASSERT(json_push_kv_str(&input, "semver", "1.2.3"));
        ASSERT(json_push_kv_str(&input, "license", "MIT"));
        ASSERT(json_push_kv_str(&input, "plan_id", saved_plan));
        ASSERT(json_push_kv_bool(&input, "confirm", true));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_project_init_commit_test.v1");
        zcl_native_handle_zcode_project_init_commit(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(access(meta, F_OK) == 0);
        ASSERT(json_get(&reply.data, "created") &&
               json_get_bool(json_get(&reply.data, "created")));
        zcl_command_reply_free(&reply);

        zcl_command_reply_init(&reply, "zcl.zcode_project_status_test.v1");
        zcl_native_handle_zcode_project_status(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(strcmp(json_get_str(json_get(&reply.data, "state")), "READY") == 0);
        zcl_command_reply_free(&reply);

        zcl_command_reply_init(&reply, "zcl.zcode_project_init_commit_test.v1");
        zcl_native_handle_zcode_project_init_commit(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_FAILED);
        zcl_command_reply_free(&reply);
        json_free(&input);

        ASSERT(unlink(meta) == 0);
        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_project_init_plan_test.v1");
        zcl_native_handle_zcode_project_init_plan(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        plan_id = json_get_str(json_get(&reply.data, "plan_id"));
        ASSERT(plan_id && strlen(plan_id) == 64);
        (void)snprintf(saved_plan, sizeof(saved_plan), "%s", plan_id);
        zcl_command_reply_free(&reply);
        json_free(&input);

        (void)snprintf(added, sizeof(added), "%s/src/added.c", root);
        ASSERT(zpd_write(added, "int added(void) { return 0; }\n"));
        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "plan_id", saved_plan));
        ASSERT(json_push_kv_bool(&input, "confirm", true));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_project_init_commit_test.v1");
        zcl_native_handle_zcode_project_init_commit(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_FAILED);
        ASSERT(access(meta, F_OK) != 0);
        zcl_command_reply_free(&reply);
        json_free(&input);
        ASSERT(unlink(added) == 0);

        char link[320];
        (void)snprintf(link, sizeof(link), "%s/link", root);
        ASSERT(symlink("LICENSE", link) == 0);
        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_project_init_plan_test.v1");
        zcl_native_handle_zcode_project_init_plan(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_FAILED);
        ASSERT(access(meta, F_OK) != 0);
        zcl_command_reply_free(&reply);
        json_free(&input);
        zpd_fixture_cleanup(root);
        PASS();
    } _test_next:;
    return failures;
}

static int zpd_test_work_start(void)
{
    int failures = 0;
    TEST("zcode work start: goal and profile compose existing task owners") {
        char root[256];
        (void)snprintf(root, sizeof(root),
                       "test-tmp/zcode-work-start-%ld", (long)getpid());
        ASSERT(zpd_fixture(root, false));
        uint8_t source_before[32], source_after[32];
        ASSERT(vcs_tree_capture_path(root, source_before) == VCS_OK);

        struct json_value input;
        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "goal", "Fix x"));
        ASSERT(json_push_kv_str(&input, "profile", "quick"));
        struct zcl_command_request request = { .input = &input };
        struct zcl_command_reply reply;
        zcl_command_reply_init(&reply, "zcl.zcode_work_start_test.v1");
        zcl_native_handle_zcode_work_start(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(vcs_tree_capture_path(root, source_after) == VCS_OK);
        ASSERT(memcmp(source_before, source_after, sizeof(source_before)) == 0);
        const struct json_value *work_id = json_get(&reply.data, "work_id");
        const struct json_value *state = json_get(&reply.data, "state");
        const struct json_value *context =
            json_get(&reply.data, "selected_context");
        const struct json_value *expert = json_get(&reply.data, "expert");
        ASSERT(work_id && strncmp(json_get_str(work_id), "work-", 5) == 0);
        char saved_work_id[32];
        (void)snprintf(saved_work_id, sizeof(saved_work_id), "%s",
                       json_get_str(work_id));
        ASSERT(state && strcmp(json_get_str(state),
                               "AWAITING_CANDIDATE") == 0);
        ASSERT(context && context->type == JSON_OBJ);
        ASSERT(strcmp(json_get_str(json_get(context, "symbol")), "x") == 0);
        ASSERT(expert && json_get(expert, "task_root") != NULL);
        zcl_command_reply_free(&reply);
        json_free(&input);

        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "work", "latest"));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_work_status_test.v1");
        zcl_native_handle_zcode_work_status(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(strcmp(json_get_str(json_get(&reply.data, "goal")),
                      "Fix x") == 0);
        ASSERT(strcmp(json_get_str(json_get(&reply.data, "state")),
                      "AWAITING_CANDIDATE") == 0);
        ASSERT(json_get(&reply.data, "next_safe_command") != NULL);
        zcl_command_reply_free(&reply);
        json_free(&input);

        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "work", saved_work_id));
        ASSERT(json_push_kv_str(&input, "adapter", "manual"));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_work_run_test.v1");
        zcl_native_handle_zcode_work_run(&request, &reply);
        if (reply.status != ZCL_COMMAND_STATUS_PASSED)
            printf("work admission failed: %s: %s\n", reply.error.code,
                   reply.error.message);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        const struct json_value *candidate_workspace =
            json_get(&reply.data, "candidate_workspace");
        const struct json_value *packet_path =
            json_get(&reply.data, "adapter_packet_path");
        ASSERT(candidate_workspace &&
               json_get_str(candidate_workspace)[0] == '/');
        char saved_candidate_workspace[4400];
        (void)snprintf(saved_candidate_workspace,
                       sizeof(saved_candidate_workspace), "%s",
                       json_get_str(candidate_workspace));
        ASSERT(packet_path && json_get_str(packet_path)[0] == '/');
        ASSERT(strncmp(json_get_str(packet_path),
                       saved_candidate_workspace,
                       strlen(saved_candidate_workspace)) == 0);
        ASSERT(access(json_get_str(packet_path), F_OK) == 0);
        ASSERT(json_get_int(json_get(&reply.data,
                                     "adapter_packet_bytes")) > 0);
        ASSERT(strcmp(json_get_str(json_get(&reply.data, "authority")),
                      "NONE_MANUAL_HANDOFF") == 0);
        ASSERT(vcs_tree_capture_path(root, source_after) == VCS_OK);
        ASSERT(memcmp(source_before, source_after, sizeof(source_before)) == 0);
        struct stat candidate_stat;
        ASSERT(stat(json_get_str(candidate_workspace), &candidate_stat) == 0 &&
               S_ISDIR(candidate_stat.st_mode));
        zcl_command_reply_free(&reply);
        json_free(&input);

        char candidate_license[4500];
        (void)snprintf(candidate_license, sizeof(candidate_license),
                       "%s/LICENSE", saved_candidate_workspace);
        ASSERT(zpd_write(candidate_license, "proprietary\n"));
        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "work", saved_work_id));
        ASSERT(json_push_kv_str(&input, "adapter", "manual"));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_work_run_test.v1");
        zcl_native_handle_zcode_work_run(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_FAILED);
        ASSERT(strcmp(reply.error.code, "PATCH_OUTSIDE_SCOPE") == 0);
        zcl_command_reply_free(&reply);
        json_free(&input);
        ASSERT(zpd_write(candidate_license, "MIT\n"));

        char candidate_source[4500];
        (void)snprintf(candidate_source, sizeof(candidate_source), "%s/src/x.c",
                       saved_candidate_workspace);
        ASSERT(zpd_write(candidate_source,
                         "int x(void) { return ; broken }\n"));
        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "work", saved_work_id));
        ASSERT(json_push_kv_str(&input, "adapter", "manual"));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_work_run_test.v1");
        zcl_native_handle_zcode_work_run(&request, &reply);
        if (reply.status != ZCL_COMMAND_STATUS_PASSED)
            printf("failed-candidate admission failed: %s: %s\n", reply.error.code,
                   reply.error.message);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(strcmp(json_get_str(json_get(&reply.data, "state")),
                      "REPAIR_NEEDED") == 0);
        ASSERT(json_get(&reply.data, "changed_files") &&
               json_get_int(json_get(&reply.data, "changed_files")) == 1);
        ASSERT(json_get(&reply.data, "candidate_root") != NULL);
        ASSERT(json_get(&reply.data, "work_receipt_root") != NULL);
        ASSERT(strcmp(json_get_str(json_get(&reply.data, "build_result")),
                      "failed") == 0);
        const struct json_value *repair_packet =
            json_get(&reply.data, "repair_packet");
        ASSERT(repair_packet && repair_packet->type == JSON_OBJ);
        ASSERT(strcmp(json_get_str(json_get(repair_packet, "goal")),
                      "Fix x") == 0);
        ASSERT(json_get(repair_packet, "parent_candidate_root") != NULL);
        ASSERT(json_get(repair_packet, "prior_patch_root") != NULL);
        ASSERT(json_get(repair_packet, "selected_excerpts") != NULL);
        const struct json_value *repair_workspace =
            json_get(&reply.data, "candidate_workspace");
        ASSERT(repair_workspace && strstr(json_get_str(repair_workspace),
                                           "/attempt-2") != NULL);
        (void)snprintf(saved_candidate_workspace,
                       sizeof(saved_candidate_workspace), "%s",
                       json_get_str(repair_workspace));
        zcl_command_reply_free(&reply);
        json_free(&input);

        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "work", saved_work_id));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_work_status_test.v1");
        zcl_native_handle_zcode_work_status(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(strcmp(json_get_str(json_get(&reply.data, "state")),
                      "REPAIR_NEEDED") == 0);
        ASSERT(strcmp(json_get_str(json_get(&reply.data, "build_result")),
                      "failed") == 0);
        zcl_command_reply_free(&reply);
        json_free(&input);

        (void)snprintf(candidate_source, sizeof(candidate_source), "%s/src/x.c",
                       saved_candidate_workspace);
        ASSERT(zpd_write(candidate_source,
                         "int x(void) { return 2; }\n"));
        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "work", saved_work_id));
        ASSERT(json_push_kv_str(&input, "adapter", "manual"));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_work_run_test.v1");
        zcl_native_handle_zcode_work_run(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(strcmp(json_get_str(json_get(&reply.data, "state")),
                      "EVIDENCE_READY") == 0);
        ASSERT(strcmp(json_get_str(json_get(&reply.data, "build_result")),
                      "passed") == 0);
        const struct json_value *run_expert = json_get(&reply.data, "expert");
        const struct json_value *run_action = run_expert
            ? json_get(run_expert, "action_id") : NULL;
        ASSERT(run_action && strlen(json_get_str(run_action)) == 64);
        ASSERT(json_write(&reply.data, NULL, 0) < 4096u);
        char saved_action_id[65];
        (void)snprintf(saved_action_id, sizeof(saved_action_id), "%s",
                       json_get_str(run_action));
        zcl_command_reply_free(&reply);
        json_free(&input);

        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "work", saved_work_id));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_work_status_test.v1");
        zcl_native_handle_zcode_work_status(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(strcmp(json_get_str(json_get(&reply.data, "state")),
                      "EVIDENCE_READY") == 0);
        ASSERT(strcmp(json_get_str(json_get(&reply.data, "build_result")),
                      "passed") == 0);
        ASSERT(json_get_int(json_get(&reply.data, "changed_files")) == 1);
        ASSERT(json_get_int(json_get(&reply.data, "added_lines")) == 1);
        ASSERT(json_get_int(json_get(&reply.data, "deleted_lines")) == 1);
        ASSERT(strcmp(json_get_str(json_get(&reply.data,
                                            "public_api_changes")),
                      "none") == 0);
        zcl_command_reply_free(&reply);
        json_free(&input);

        char zbuild_datadir[4400];
        (void)snprintf(zbuild_datadir, sizeof(zbuild_datadir), "%s",
                       saved_candidate_workspace);
        char *attempt_dir = strrchr(zbuild_datadir, '/');
        ASSERT(attempt_dir != NULL);
        (void)snprintf(attempt_dir,
                       (size_t)(zbuild_datadir + sizeof(zbuild_datadir) -
                                attempt_dir), "/zbuild");
        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "datadir", zbuild_datadir));
        ASSERT(json_push_kv_str(&input, "action_id", saved_action_id));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_evidence_test.v1");
        zcl_native_handle_zcode_evidence(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(json_get_bool(json_get(&reply.data, "compile_satisfied")));
        ASSERT(json_get_bool(json_get(&reply.data, "test_satisfied")));
        ASSERT(json_get_bool(json_get(&reply.data, "policy_satisfied")));
        zcl_command_reply_free(&reply);
        json_free(&input);

        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "work", saved_work_id));
        ASSERT(json_push_kv_str(&input, "adapter", "manual"));
        ASSERT(json_push_kv_str(&input, "verdict", "approve"));
        ASSERT(json_push_kv_str(&input, "findings",
                                "Declared build and tests support approval."));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_work_review_test.v1");
        zcl_native_handle_zcode_work_review(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(strcmp(json_get_str(json_get(&reply.data, "verdict")),
                      "approve") == 0);
        ASSERT(json_get_bool(json_get(&reply.data,
                                      "independent_reviewer")));
        ASSERT(json_get_int(json_get(&reply.data, "review_receipts")) == 1);
        ASSERT(strlen(json_get_str(json_get(&reply.data,
                                            "review_root"))) == 64);
        ASSERT(strlen(json_get_str(json_get(&reply.data,
                                            "work_receipt_root"))) == 64);
        zcl_command_reply_free(&reply);
        json_free(&input);

        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "work", saved_work_id));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_work_status_test.v1");
        zcl_native_handle_zcode_work_status(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(strcmp(json_get_str(json_get(&reply.data, "review_verdict")),
                      "approve") == 0);
        ASSERT(strlen(json_get_str(json_get(
                   json_get(&reply.data, "expert"), "review_root"))) == 64);
        zcl_command_reply_free(&reply);
        json_free(&input);

        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "work", saved_work_id));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_work_accept_test.v1");
        zcl_native_handle_zcode_work_accept(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(strcmp(json_get_str(json_get(&reply.data, "state")),
                      "PROVEN") == 0);
        ASSERT(json_get(json_get(&reply.data, "expert"),
                        "lane_receipt_root") != NULL);
        zcl_command_reply_free(&reply);
        json_free(&input);
        ASSERT(vcs_tree_capture_path(root, source_after) == VCS_OK);
        ASSERT(memcmp(source_before, source_after, sizeof(source_before)) == 0);

        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "work", saved_work_id));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_work_accept_test.v1");
        zcl_native_handle_zcode_work_accept(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(strcmp(json_get_str(json_get(&reply.data, "state")),
                      "PROVEN") == 0);
        ASSERT(json_get_bool(json_get(&reply.data, "idempotent")));
        zcl_command_reply_free(&reply);
        json_free(&input);

        char zbuild_db[4500];
        (void)snprintf(zbuild_db, sizeof(zbuild_db), "%s/node.db",
                       zbuild_datadir);
        ASSERT(unlink(zbuild_db) == 0);
        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "work", saved_work_id));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_work_status_test.v1");
        zcl_native_handle_zcode_work_status(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(strcmp(json_get_str(json_get(&reply.data, "state")),
                      "PROVEN") == 0);
        zcl_command_reply_free(&reply);
        json_free(&input);

        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "work", saved_work_id));
        ASSERT(json_push_kv_str(&input, "adapter", "codex"));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_work_run_test.v1");
        zcl_native_handle_zcode_work_run(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_FAILED);
        ASSERT(strcmp(reply.error.code, "ADAPTER_UNAVAILABLE") == 0);
        zcl_command_reply_free(&reply);
        json_free(&input);

        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "work", saved_work_id));
        ASSERT(json_push_kv_str(&input, "adapter", "shell"));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_work_run_test.v1");
        zcl_native_handle_zcode_work_run(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_FAILED);
        ASSERT(strcmp(reply.error.code, "ADAPTER_REFUSED") == 0);
        zcl_command_reply_free(&reply);
        json_free(&input);
        char session_root[4400];
        (void)snprintf(session_root, sizeof(session_root), "%s",
                       saved_candidate_workspace);
        char *attempt = strrchr(session_root, '/');
        ASSERT(attempt != NULL);
        *attempt = '\0';
        ASSERT(zcl_tree_remove(session_root).ok);
        zpd_fixture_cleanup(root);
        PASS();
    } _test_next:;
    return failures;
}

static int zpd_test_standard_profile(void)
{
    int failures = 0;
    TEST("zcode work standard: warning-fatal sanitizer evidence reaches acceptance") {
        char root[256];
        (void)snprintf(root, sizeof(root),
                       "test-tmp/zcode-standard-%ld", (long)getpid());
        ASSERT(zpd_fixture(root, false));

        struct json_value input;
        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "goal", "Make x return two"));
        ASSERT(json_push_kv_str(&input, "profile", "standard"));
        struct zcl_command_request request = { .input = &input };
        struct zcl_command_reply reply;
        zcl_command_reply_init(&reply, "zcl.zcode_standard_start_test.v1");
        zcl_native_handle_zcode_work_start(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        char work_id[32];
        (void)snprintf(work_id, sizeof(work_id), "%s",
                       json_get_str(json_get(&reply.data, "work_id")));
        zcl_command_reply_free(&reply); json_free(&input);

        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "work", work_id));
        ASSERT(json_push_kv_str(&input, "adapter", "manual"));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_standard_run_test.v1");
        zcl_native_handle_zcode_work_run(&request, &reply);
        if (reply.status != ZCL_COMMAND_STATUS_PASSED)
            fprintf(stderr, "standard admission failed: %s: %s\n",
                    reply.error.code, reply.error.message);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        char candidate[4400];
        (void)snprintf(candidate, sizeof(candidate), "%s",
                       json_get_str(json_get(&reply.data,
                                             "candidate_workspace")));
        zcl_command_reply_free(&reply); json_free(&input);

        char source[4500];
        (void)snprintf(source, sizeof(source), "%s/src/x.c", candidate);
        ASSERT(zpd_write(source, "int x(void) { return 2; }\n"));
        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "work", work_id));
        ASSERT(json_push_kv_str(&input, "adapter", "manual"));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_standard_run_test.v1");
        zcl_native_handle_zcode_work_run(&request, &reply);
        if (reply.status != ZCL_COMMAND_STATUS_PASSED)
            fprintf(stderr, "standard execution failed: %s: %s\n",
                    reply.error.code, reply.error.message);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(json_get_int(json_get(&reply.data,
                                     "compile_receipts")) == 2);
        ASSERT(json_get_int(json_get(&reply.data,
                                     "test_receipts")) == 2);
        ASSERT(strcmp(json_get_str(json_get(&reply.data,
                                            "sanitizer_result")),
                      "passed_asan_ubsan") == 0);
        zcl_command_reply_free(&reply); json_free(&input);

        json_init(&input); json_set_object(&input);
        ASSERT(json_push_kv_str(&input, "workspace", root));
        ASSERT(json_push_kv_str(&input, "work", work_id));
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.zcode_standard_accept_test.v1");
        zcl_native_handle_zcode_work_accept(&request, &reply);
        ASSERT(reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(strcmp(json_get_str(json_get(&reply.data, "state")),
                      "PROVEN") == 0);
        zcl_command_reply_free(&reply); json_free(&input);

        char session_root[4400];
        (void)snprintf(session_root, sizeof(session_root), "%s", candidate);
        char *attempt = strrchr(session_root, '/');
        ASSERT(attempt != NULL);
        *attempt = '\0';
        ASSERT(zcl_tree_remove(session_root).ok);
        zpd_fixture_cleanup(root);
        PASS();
    } _test_next:;
    return failures;
}

int test_zcode_package_dev(void)
{
    secp256k1_context *ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    uint8_t secret[32] = {0}; secret[31] = 1;
    uint8_t pubkey[33] = {0};
    if (!ctx || !zpd_pubkey(ctx, secret, pubkey)) {
        if (ctx) secp256k1_context_destroy(ctx);
        return 1;
    }
    int failures = zpd_test_base(ctx, secret, pubkey) +
                   zpd_test_control_stores(pubkey) +
                   zpd_test_fail_closed(pubkey) +
                   zpd_test_project_inspect() +
                   zpd_test_project_init() +
                   zpd_test_work_start() +
                   zpd_test_standard_profile() +
                   zpd_test_twelve_task_benchmark();
    secp256k1_context_destroy(ctx);
    return failures;
}
