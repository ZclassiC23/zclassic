/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: rederive the self-hosted package DAG and its root projection. */

#include "test/test_core.h"

#include "base/hex.h"
#include "services/package_lifecycle.h"
#include "vcs/package_build.h"
#include "vcs/package_publish.h"
#include "vcs/package_prepare.h"
#include "vcs/package_reproduce.h"
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
#include "../../../config/zcode_c23_commons_app.def"
};
#undef ZCODE_PACKAGE

static bool registry_prepare(const struct registry_expected *expected,
                             struct vcs_package_prepared *prepared)
{
    uint8_t pubkey[33];
    if (!zcl_hex_decode_lower(expected->publisher_pubkey, pubkey,
                              sizeof(pubkey)))
        return false;
    struct vcs_package_prepare_options options = {
        .dir = expected->dir,
        .publisher_sequence = expected->sequence,
        .reward_address = "",
        .chain_id = "zclassic-main",
    };
    memcpy(options.publisher_pubkey, pubkey, sizeof(pubkey));
    char detail[256];
    if (vcs_package_prepare(&options, prepared, detail, sizeof(detail)) !=
        VCS_PACKAGE_PREPARE_OK)
        return false;
    if (!zcl_hex_decode_lower(expected->signature,
                              prepared->release.signature,
                              sizeof(prepared->release.signature))) {
        vcs_package_prepared_free(prepared);
        return false;
    }
    return true;
}

static bool registry_publish_store(
    struct vcs_package_store *store,
    const struct vcs_package_prepared *prepared, const char *source_dir)
{
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
    return ok;
}

static bool registry_publish_scratch(
    const struct vcs_package_prepared *prepared, const char *source_dir)
{
    char scratch[256];
    test_make_tmpdir(scratch, sizeof(scratch), "zcode_registry", "package");
    struct vcs_package_store *store = vcs_package_store_open(
        scratch, UINT64_C(256) * 1024u * 1024u);
    if (!store)
        return false;
    bool ok = registry_publish_store(store, prepared, source_dir);
    vcs_package_store_close(store);
    test_rm_rf(scratch);
    return ok;
}

static bool registry_read_receipt(
    const char *datadir, const char *root_hex,
    struct vcs_package_build_receipt *receipt)
{
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/zcode/installed/%s/build-report",
                     datadir, root_hex);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return false;
    uint8_t wire[VCS_PACKAGE_BUILD_MAX_WIRE_BYTES];
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    size_t wire_len = fread(wire, 1, sizeof(wire), file);
    bool ok = wire_len > 0 && feof(file) && !ferror(file);
    fclose(file);
    return ok && vcs_package_build_parse(wire, wire_len, receipt) ==
                     VCS_PACKAGE_BUILD_OK;
}

static bool registry_build_one(const char *datadir, const char *root_hex,
                               int64_t now_unix)
{
    struct package_lifecycle_plan_report plan;
    struct zcl_result planned =
        package_lifecycle_plan(datadir, root_hex, now_unix, &plan);
    if (!planned.ok || !plan.ready) {
        printf("  zcode_package_registry: plan %s failed rule=%s detail=%s "
               "message=%s\n", root_hex, plan.rule, plan.detail,
               planned.message);
        return false;
    }
    struct package_lifecycle_commit_report commit;
    struct zcl_result committed = package_lifecycle_commit(
        datadir, plan.plan_id, now_unix + 1, &commit);
    if (!committed.ok || !commit.installed) {
        printf("  zcode_package_registry: commit %s failed rule=%s detail=%s "
               "message=%s\n", root_hex, commit.rule, commit.detail,
               committed.message);
        return false;
    }
    return true;
}

static bool registry_independent_reproduction(size_t *reproduced_out)
{
    *reproduced_out = 0;
    char builder_a[256], builder_b[256];
    test_make_tmpdir(builder_a, sizeof(builder_a), "zcode_registry",
                     "builder_a");
    test_make_tmpdir(builder_b, sizeof(builder_b), "zcode_registry",
                     "builder_b");
    struct vcs_package_store *store_a = vcs_package_store_open(
        builder_a, UINT64_C(256) * 1024u * 1024u);
    struct vcs_package_store *store_b = vcs_package_store_open(
        builder_b, UINT64_C(256) * 1024u * 1024u);
    bool ok = store_a && store_b;
    for (size_t i = 0;
         ok && i < sizeof(registry_packages) / sizeof(registry_packages[0]);
         i++) {
        struct vcs_package_prepared prepared;
        ok = registry_prepare(&registry_packages[i], &prepared);
        if (ok) {
            bool published_a = registry_publish_store(
                store_a, &prepared, registry_packages[i].dir);
            bool published_b = registry_publish_store(
                store_b, &prepared, registry_packages[i].dir);
            ok = published_a && published_b;
            if (!ok)
                printf("  zcode_package_registry: publish %s failed "
                       "builder_a=%s builder_b=%s\n",
                       registry_packages[i].name,
                       published_a ? "ok" : "failed",
                       published_b ? "ok" : "failed");
            vcs_package_prepared_free(&prepared);
        } else {
            printf("  zcode_package_registry: prepare %s failed\n",
                   registry_packages[i].name);
        }
    }
    if (store_a)
        vcs_package_store_close(store_a);
    if (store_b)
        vcs_package_store_close(store_b);

    for (size_t i = 0;
         ok && i < sizeof(registry_packages) / sizeof(registry_packages[0]);
         i++) {
        const struct registry_expected *expected = &registry_packages[i];
        int64_t now = INT64_C(1700001000) + (int64_t)i * 4;
        ok = registry_build_one(builder_a, expected->content_root, now) &&
             registry_build_one(builder_b, expected->content_root, now);
        struct vcs_package_build_receipt a, b;
        if (ok)
            ok = registry_read_receipt(builder_a, expected->content_root,
                                       &a) &&
                 registry_read_receipt(builder_b, expected->content_root,
                                       &b);
        struct vcs_reproduce_verdict verdict;
        if (ok) {
            vcs_package_reproduce_compare(&a, &b, &verdict);
            ok = verdict.reproduced && a.output_count > 0 &&
                 b.output_count == a.output_count &&
                 strcmp(a.flags, VCS_PACKAGE_BUILD_FLAGS_QUICK_V1) == 0 &&
                 strcmp(b.flags, VCS_PACKAGE_BUILD_FLAGS_QUICK_V1) == 0 &&
                 a.test_ran && b.test_ran &&
                 a.result_class == VCS_PACKAGE_BUILD_RESULT_TEST_PASS &&
                 b.result_class == VCS_PACKAGE_BUILD_RESULT_TEST_PASS;
            if (!ok)
                printf("  zcode_package_registry: reproduction %s failed "
                       "rule=%s detail=%s\n", expected->name,
                       vcs_reproduce_rule_string(
                           (enum vcs_reproduce_rule)verdict.rule),
                       verdict.detail);
        }
        if (ok)
            (*reproduced_out)++;
    }
    test_rm_rf(builder_a);
    test_rm_rf(builder_b);
    return ok;
}

static int registry_test_independent_reproduction(void)
{
    int failures = 0;
    TEST("zcode package registry: two isolated builders reproduce all artifacts") {
        size_t reproduced = 0;
        ASSERT(registry_independent_reproduction(&reproduced));
        ASSERT_EQ(reproduced,
                  sizeof(registry_packages) / sizeof(registry_packages[0]));
        PASS();
    } _test_next:;
    return failures;
}

int test_zcode_package_registry(void)
{
    int failures = 0;
    TEST("zcode package registry: C23 Commons Alpha roots and DAG rederive") {
        ASSERT_EQ(sizeof(registry_packages) / sizeof(registry_packages[0]),
                  10);
        for (size_t i = 0;
             i < sizeof(registry_packages) / sizeof(registry_packages[0]);
             i++) {
            const struct registry_expected *expected = &registry_packages[i];
            struct vcs_package_prepared prepared;
            ASSERT(registry_prepare(expected, &prepared));
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
    failures += registry_test_independent_reproduction();
    printf("=== zcode_package_registry: %d failures ===\n", failures);
    return failures;
}
