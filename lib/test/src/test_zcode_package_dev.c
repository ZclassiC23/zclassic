/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: hermetic preparation, capsule, and detached sealing proofs. */

#include "test/test_core.h"
#include "base/hex.h"
#include "command/native_command.h"
#include "json/json.h"
#include "vcs/package_capsule.h"
#include "vcs/package_prepare.h"

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
        "include/x.h", "tests/test.c",
    };
    char path[512];
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        (void)snprintf(path, sizeof(path), "%s/%s", root, files[i]);
        (void)unlink(path);
    }
    static const char *const dirs[] = { "src", "include", "tests" };
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
                   zpd_test_fail_closed(pubkey);
    secp256k1_context_destroy(ctx);
    return failures;
}
