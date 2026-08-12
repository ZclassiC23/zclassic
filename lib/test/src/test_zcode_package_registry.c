/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: rederive the self-hosted package DAG and its root projection. */

#include "test/test_core.h"

#include "base/hex.h"
#include "vcs/package_publish.h"
#include "vcs/package_prepare.h"
#include "vcs/package_store.h"

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
    const char *publisher_pubkey;
    const char *signature;
};

#define ZCODE_PACKAGE(name, dir, sequence, content, release, recipe, lock, capsule, publisher, signature) \
    {name, dir, sequence, content, release, recipe, lock, capsule, publisher, signature},
static const struct registry_expected registry_packages[] = {
#include "../../../config/zcode_package_registry.def"
};
#undef ZCODE_PACKAGE

static bool registry_publish_scratch(
    const struct vcs_package_prepared *prepared, const char *source_dir)
{
    char scratch[256];
    test_make_tmpdir(scratch, sizeof(scratch), "zcode_registry", "package");
    struct vcs_package_store *store = vcs_package_store_open(
        scratch, UINT64_C(256) * 1024u * 1024u);
    if (!store) return false;
    uint8_t admitted[32];
    bool ok = vcs_package_store_put_manifest(
        store, prepared->manifest_wire, prepared->manifest_wire_len,
        admitted) == VCS_PACKAGE_STORE_OK &&
        memcmp(admitted, prepared->package_root, 32) == 0;
    uint8_t chunk[VCS_PACKAGE_CHUNK_BYTES];
    for (size_t i = 0; ok && i < prepared->manifest.count; i++) {
        const struct vcs_package_file *file = &prepared->manifest.files[i];
        for (uint32_t j = 0; ok && j < file->chunk_count; j++) {
            size_t len = 0; enum vcs_package_publish_rule rule;
            ok = vcs_package_publish_read_chunk(
                source_dir, file, j, chunk, &len, &rule) &&
                vcs_package_store_put_chunk(store, prepared->package_root,
                    file->path, j, chunk, len) == VCS_PACKAGE_STORE_OK;
        }
    }
    uint8_t recipe_root[32]; enum vcs_package_accept_result accepted;
    ok = ok && vcs_package_store_put_recipe(
        store, prepared->recipe_wire, prepared->recipe_wire_len,
        recipe_root) == VCS_PACKAGE_STORE_OK &&
        memcmp(recipe_root, prepared->recipe_root, 32) == 0 &&
        vcs_package_store_put_release(store, &prepared->release, &accepted) ==
            VCS_PACKAGE_STORE_OK && accepted == VCS_PACKAGE_ACCEPT_OK &&
        vcs_package_store_verify_possession(store, prepared->package_root,
                                            false);
    vcs_package_store_close(store); test_rm_rf(scratch);
    return ok;
}

int test_zcode_package_registry(void)
{
    int failures = 0;
    TEST("zcode package registry: C23 Commons Alpha roots and DAG rederive") {
        ASSERT_EQ(sizeof(registry_packages) / sizeof(registry_packages[0]),
                  9);
        for (size_t i = 0;
             i < sizeof(registry_packages) / sizeof(registry_packages[0]);
             i++) {
            const struct registry_expected *expected = &registry_packages[i];
            uint8_t pubkey[33];
            ASSERT(zcl_hex_decode_lower(expected->publisher_pubkey, pubkey,
                                        sizeof(pubkey)));
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
            ASSERT(registry_publish_scratch(&prepared, expected->dir));
            ASSERT(prepared.lock.count >= 1);
            ASSERT_EQ(prepared.lock.nodes[prepared.lock.count - 1u].depth, 0);
            ASSERT_EQ(prepared.lock.nodes[prepared.lock.count - 1u].direct_deps,
                      prepared.lock.count - 1u);
            for (size_t d = 0; d + 1u < prepared.lock.count; d++) {
                bool found = false;
                for (size_t p = 0; p < i; p++) {
                    uint8_t prior[32];
                    ASSERT(zcl_hex_decode_lower(
                        registry_packages[p].content_root, prior,
                        sizeof(prior)));
                    found = found || memcmp(prepared.lock.nodes[d].root,
                                             prior, sizeof(prior)) == 0;
                }
                ASSERT(found);
                ASSERT_EQ(prepared.lock.nodes[d].depth, 1);
            }
            vcs_package_prepared_free(&prepared);
        }
        PASS();
    } _test_next:;
    printf("=== zcode_package_registry: %d failures ===\n", failures);
    return failures;
}
