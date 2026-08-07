/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: human-first, read-only inspection of one C23 package workspace. */

#include "command/native_command.h"

#include "base/checked.h"
#include "base/hex.h"
#include "json/json.h"
#include "vcs/package_prepare.h"
#include "vcs/zcode_dev_product.h"

#include <stdio.h>
#include <string.h>

static const char *zproject_str(const struct json_value *input,
                                const char *key)
{
    const struct json_value *value = input ? json_get(input, key) : NULL;
    return value && value->type == JSON_STR ? json_get_str(value) : NULL;
}

static void zproject_fail(struct zcl_command_reply *reply, const char *code,
                          const char *detail)
{
    zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                           ZCL_COMMAND_EXIT_INVALID, code, "inspect", false,
                           false, detail, "zcode.project.inspect");
}

static bool zproject_push_strings(
    struct json_value *out, const char *key,
    const struct vcs_package_recipe_strings *strings)
{
    struct json_value values;
    json_init(&values);
    json_set_array(&values);
    bool ok = true;
    for (size_t i = 0; ok && i < strings->count; i++) {
        struct json_value value;
        json_init(&value);
        json_set_str(&value, strings->items[i]);
        ok = json_push_back(&values, &value);
        json_free(&value);
    }
    if (ok) ok = json_push_kv(out, key, &values);
    json_free(&values);
    return ok;
}

static bool zproject_push_libraries(struct json_value *out,
                                    const struct vcs_package_recipe *recipe)
{
    struct json_value values;
    json_init(&values);
    json_set_array(&values);
    bool ok = true;
    for (size_t i = 0; ok && i < recipe->library_count; i++) {
        const char *name = vcs_package_recipe_library_name(
            recipe->libraries[i]);
        struct json_value value;
        json_init(&value);
        if (!name) {
            ok = false;
        } else {
            json_set_str(&value, name);
            ok = json_push_back(&values, &value);
        }
        json_free(&value);
    }
    if (ok) ok = json_push_kv(out, "allowed_libraries", &values);
    json_free(&values);
    return ok;
}

static bool zproject_push_root(struct json_value *out, const char *key,
                               const uint8_t root[32])
{
    char hex[65];
    zcl_hex_encode(root, 32, hex);
    return json_push_kv_str(out, key, hex);
}

static bool zproject_render_layout(struct json_value *out,
                                   const struct vcs_package_prepared *prepared)
{
    json_init(out);
    json_set_object(out);
    return zproject_push_strings(out, "public_headers",
                                 &prepared->recipe.public_headers) &&
           zproject_push_strings(out, "sources",
                                 &prepared->recipe.sources) &&
           zproject_push_strings(out, "tests",
                                 &prepared->recipe.test_sources) &&
           zproject_push_strings(out, "include_directories",
                                 &prepared->recipe.include_dirs) &&
           zproject_push_libraries(out, &prepared->recipe);
}

static bool zproject_render_expert(struct json_value *out,
                                   const struct vcs_package_prepared *prepared)
{
    json_init(out);
    json_set_object(out);
    return zproject_push_root(out, "package_root", prepared->package_root) &&
           zproject_push_root(out, "recipe_root", prepared->recipe_root) &&
           zproject_push_root(out, "dependency_lock_root",
                              prepared->lock_root) &&
           zproject_push_root(out, "api_capsule_root",
                              prepared->capsule_root);
}

static bool zproject_render_profile(struct json_value *out)
{
    struct vcs_zcode_dev_profile profile;
    uint8_t root[32];
    if (!vcs_zcode_dev_profile_expand("standard", &profile) ||
        vcs_zcode_proof_policy_root(&profile.policy, root) !=
            VCS_ZCODE_DEV_OK)
        return false;
    struct json_value exact;
    json_init(&exact);
    json_set_object(&exact);
    bool ok = zproject_push_root(&exact, "root", root) &&
        json_push_kv_int(&exact, "required_proofs",
                         profile.policy.required_proofs) &&
        json_push_kv_int(&exact, "minimum_compile_receipts",
                         profile.policy.minimum_compile_receipts) &&
        json_push_kv_int(&exact, "minimum_test_receipts",
                         profile.policy.minimum_test_receipts) &&
        json_push_kv_int(&exact, "minimum_fuzz_receipts",
                         profile.policy.minimum_fuzz_receipts) &&
        json_push_kv_int(&exact, "minimum_reviews",
                         profile.policy.minimum_reviews) &&
        json_push_kv_int(&exact, "minimum_matching_receipts",
                         profile.policy.minimum_matching_receipts) &&
        json_push_kv_int(&exact, "maximum_proof_age_seconds",
                         profile.policy.maximum_proof_age_seconds);
    json_init(out);
    json_set_object(out);
    if (ok) {
        ok = json_push_kv_str(out, "name", profile.name) &&
             json_push_kv_bool(out, "package_build", true) &&
             json_push_kv_bool(out, "declared_tests", true) &&
             json_push_kv_bool(out, "warning_fatal",
                               profile.warning_fatal) &&
             json_push_kv_bool(out, "sanitizers", profile.sanitizers) &&
             json_push_kv_bool(out, "deterministic_fuzz",
                               profile.deterministic_fuzz) &&
             json_push_kv_bool(out, "local_reproduction",
                               profile.local_reproduction) &&
             json_push_kv_bool(out, "separate_review",
                               profile.separate_review) &&
             json_push_kv_bool(out, "approved_reproduction",
                               profile.approved_reproduction) &&
             json_push_kv(out, "exact_policy", &exact);
    }
    json_free(&exact);
    return ok;
}

static bool zproject_total_bytes(const struct vcs_package_manifest *manifest,
                                 uint64_t *out)
{
    uint64_t total = 0;
    for (size_t i = 0; i < manifest->count; i++) {
        if (!zcl_u64_add(total, manifest->files[i].size, &total)) {
            *out = 0;
            return false;
        }
    }
    *out = total;
    return true;
}

void zcl_native_handle_zcode_project_inspect(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    if (!request || !reply) return;
    const char *workspace = zproject_str(request->input, "workspace");
    if (!workspace || !workspace[0]) {
        zproject_fail(reply, "BAD_WORKSPACE",
                      "workspace must name one existing C23 package directory");
        return;
    }
    static const uint8_t inspection_pubkey[33] = {
        0x02, 0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb,
        0xac, 0x55, 0xa0, 0x62, 0x95, 0xce, 0x87, 0x0b,
        0x07, 0x02, 0x9b, 0xfc, 0xdb, 0x2d, 0xce, 0x28,
        0xd9, 0x59, 0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17,
        0x98,
    };
    struct vcs_package_prepare_options options = {
        .dir = workspace,
        .publisher_sequence = 1,
        .reward_address = "",
        .chain_id = "zclassic-main",
    };
    memcpy(options.publisher_pubkey, inspection_pubkey,
           sizeof(inspection_pubkey));
    struct vcs_package_prepared prepared;
    char detail[256] = {0};
    enum vcs_package_prepare_error err = vcs_package_prepare(
        &options, &prepared, detail, sizeof(detail));
    if (err != VCS_PACKAGE_PREPARE_OK) {
        char message[384];
        (void)snprintf(message, sizeof(message), "%s: %s",
                       vcs_package_prepare_error_string(err), detail);
        zproject_fail(reply, "PROJECT_INSPECT_FAILED", message);
        return;
    }

    uint64_t total_bytes = 0;
    struct json_value layout, expert, profile, limits, scopes;
    json_init(&layout);
    json_init(&expert);
    json_init(&profile);
    bool ok = zproject_total_bytes(&prepared.manifest, &total_bytes) &&
              zproject_render_layout(&layout, &prepared) &&
              zproject_render_expert(&expert, &prepared) &&
              zproject_render_profile(&profile);
    json_init(&limits); json_set_object(&limits);
    json_init(&scopes); json_set_array(&scopes);
    if (ok) {
        const struct vcs_package_recipe_strings *lists[] = {
            &prepared.recipe.include_dirs,
        };
        for (size_t i = 0; ok && i < lists[0]->count; i++) {
            struct json_value value;
            json_init(&value); json_set_str(&value, lists[0]->items[i]);
            ok = json_push_back(&scopes, &value);
            json_free(&value);
        }
        struct json_value src, tests;
        json_init(&src); json_set_str(&src, "src");
        json_init(&tests); json_set_str(&tests, "tests");
        if (ok) ok = json_push_back(&scopes, &src);
        if (ok && prepared.recipe.test_sources.count != 0)
            ok = json_push_back(&scopes, &tests);
        json_free(&src); json_free(&tests);
    }
    if (ok) {
        ok = json_push_kv_int(&limits, "maximum_test_seconds",
                              prepared.recipe.maximum_test_seconds) &&
             json_push_kv_int(&limits, "maximum_memory_bytes",
                              (int64_t)prepared.recipe.maximum_memory_bytes) &&
             json_push_kv_str(&reply->data, "name", prepared.release.name) &&
             json_push_kv_str(&reply->data, "semver",
                              prepared.release.semver) &&
             json_push_kv_str(&reply->data, "license",
                              prepared.release.license) &&
             json_push_kv_int(&reply->data, "file_count",
                              (int64_t)prepared.manifest.count) &&
             json_push_kv_int(&reply->data, "total_project_bytes",
                              (int64_t)total_bytes) &&
             json_push_kv(&reply->data, "layout", &layout) &&
             json_push_kv(&reply->data, "likely_write_scopes", &scopes) &&
             json_push_kv(&reply->data, "resource_ceilings", &limits) &&
             json_push_kv_str(&reply->data, "suggested_profile", "standard") &&
             json_push_kv(&reply->data, "proof_profile", &profile) &&
             json_push_kv_bool(&reply->data, "existing_package_config", true) &&
             json_push_kv_bool(&reply->data, "read_only", true) &&
             json_push_kv_str(&reply->data, "next_safe_command",
                              "zcode work start") &&
             json_push_kv(&reply->data, "expert", &expert);
    }
    json_free(&scopes); json_free(&limits); json_free(&profile);
    json_free(&expert);
    json_free(&layout);
    vcs_package_prepared_free(&prepared);
    if (!ok)
        zproject_fail(reply, "PROJECT_INSPECT_OUTPUT",
                      "the bounded project summary could not be rendered");
}
