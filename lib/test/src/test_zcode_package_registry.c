/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: rederive the self-hosted package DAG and its root projection. */

#include "test/test_core.h"

#include "base/hex.h"
#include "vcs/package_prepare.h"

#include <stdio.h>
#include <string.h>

struct registry_expected {
    const char *name;
    const char *dir;
    uint64_t sequence;
    const char *content_root;
    const char *release_root;
    const char *recipe_root;
    const char *lock_root;
    const char *capsule_root;
    const char *dependency_root;
    const char *signature;
};

#define ZCODE_PACKAGE(name, dir, sequence, content, release, recipe, lock, capsule, dependency, signature) \
    {name, dir, sequence, content, release, recipe, lock, capsule, dependency, signature},
static const struct registry_expected registry_packages[] = {
#include "../../../config/zcode_package_registry.def"
};
#undef ZCODE_PACKAGE

int test_zcode_package_registry(void)
{
    int failures = 0;
    TEST("zcode package registry: base, sha3 and codec roots rederive") {
        static const char pubkey_hex[] =
            "03448effe2ae40eb4053acfceb9839163c881b22affff9572283caddeee9207ce4";
        uint8_t pubkey[33];
        ASSERT(zcl_hex_decode_lower(pubkey_hex, pubkey, sizeof(pubkey)));
        ASSERT_EQ(sizeof(registry_packages) / sizeof(registry_packages[0]),
                  3);
        for (size_t i = 0;
             i < sizeof(registry_packages) / sizeof(registry_packages[0]);
             i++) {
            const struct registry_expected *expected = &registry_packages[i];
            struct vcs_package_prepare_options options = {
                .dir = expected->dir,
                .publisher_sequence = expected->sequence,
                .reward_address = "", .chain_id = "zclassic-main",
            };
            memcpy(options.publisher_pubkey, pubkey, sizeof(pubkey));
            struct vcs_package_prepared prepared; char detail[256];
            ASSERT_EQ(vcs_package_prepare(&options, &prepared, detail,
                                          sizeof(detail)),
                      VCS_PACKAGE_PREPARE_OK);
            ASSERT_STR_EQ(prepared.release.name, expected->name);
            char content[65], release[65], recipe[65], lock[65], capsule[65];
            zcl_hex_encode(prepared.package_root, 32, content);
            zcl_hex_encode(prepared.signing_digest, 32, release);
            zcl_hex_encode(prepared.recipe_root, 32, recipe);
            zcl_hex_encode(prepared.lock_root, 32, lock);
            zcl_hex_encode(prepared.capsule_root, 32, capsule);
            ASSERT_STR_EQ(content, expected->content_root);
            ASSERT_STR_EQ(release, expected->release_root);
            ASSERT_STR_EQ(recipe, expected->recipe_root);
            ASSERT_STR_EQ(lock, expected->lock_root);
            ASSERT_STR_EQ(capsule, expected->capsule_root);
            ASSERT(zcl_hex_decode_lower(expected->signature,
                       prepared.release.signature,
                       sizeof(prepared.release.signature)));
            ASSERT_EQ(vcs_package_release_verify(&prepared.release),
                      VCS_PACKAGE_RELEASE_OK);
            if (i == 0) {
                ASSERT_STR_EQ(expected->dependency_root, "");
                ASSERT_EQ(prepared.lock.count, 1);
            } else {
                char dependency[65];
                zcl_hex_encode(prepared.lock.nodes[0].root, 32, dependency);
                ASSERT_STR_EQ(dependency, expected->dependency_root);
                ASSERT_EQ(prepared.lock.count, 2);
            }
            vcs_package_prepared_free(&prepared);
        }
        PASS();
    } _test_next:;
    printf("=== zcode_package_registry: %d failures ===\n", failures);
    return failures;
}
