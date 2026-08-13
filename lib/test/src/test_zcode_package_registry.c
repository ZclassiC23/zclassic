/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: rederive the self-hosted package DAG and its root projection. */

#include "test/test_core.h"

#include "base/hex.h"
#include "services/package_lifecycle.h"
#include "util/spawn.h"
#include "vcs/package_build.h"
#include "vcs/package_checkout.h"
#include "vcs/package_publish.h"
#include "vcs/package_prepare.h"
#include "vcs/package_reproduce.h"
#include "vcs/package_store.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

static const struct registry_expected *registry_named(const char *name)
{
    for (size_t i = 0;
         i < sizeof(registry_packages) / sizeof(registry_packages[0]); i++)
        if (strcmp(registry_packages[i].name, name) == 0)
            return &registry_packages[i];
    return NULL;
}

static bool registry_stranger_build(const char *datadir)
{
    const struct registry_expected *app =
        registry_named("zclassic23/commons-demo");
    const struct registry_expected *base = registry_named("zclassic23/base");
    const struct registry_expected *codec =
        registry_named("zclassic23/codec");
    const struct registry_expected *json = registry_named("zclassic23/json");
    if (!app || !base || !codec || !json)
        return false;

    struct vcs_package_store *store = vcs_package_store_open(
        datadir, UINT64_C(256) * 1024u * 1024u);
    uint8_t app_root[32];
    if (!store || !zcl_hex_decode_lower(app->content_root, app_root,
                                         sizeof(app_root))) {
        vcs_package_store_close(store);
        return false;
    }
    char checkout[1024];
    int n = snprintf(checkout, sizeof(checkout), "%s/stranger-source",
                     datadir);
    struct vcs_package_checkout_metrics metrics;
    enum vcs_package_checkout_result checked = n > 0 &&
            (size_t)n < sizeof(checkout)
        ? vcs_package_checkout(store, app_root, checkout, &metrics)
        : VCS_PACKAGE_CHECKOUT_DESTINATION;
    bool refused_existing = checked == VCS_PACKAGE_CHECKOUT_OK &&
        vcs_package_checkout(store, app_root, checkout, NULL) ==
            VCS_PACKAGE_CHECKOUT_DESTINATION;
    vcs_package_store_close(store);
    if (checked != VCS_PACKAGE_CHECKOUT_OK || !refused_existing ||
        metrics.files != 6u || metrics.chunks != 6u || metrics.bytes == 0)
        return false;

    char source[1024], binary[1024];
    char app_include[1024], base_include[1024], codec_include[1024];
    char json_include[1024], app_archive[1024], base_archive[1024];
    char codec_archive[1024], json_archive[1024];
    if (snprintf(source, sizeof(source), "%s/app/main.c", checkout) <= 0 ||
        snprintf(binary, sizeof(binary), "%s/commons-demo", datadir) <= 0 ||
        snprintf(app_include, sizeof(app_include),
                 "%s/zcode/installed/%s/include", datadir,
                 app->content_root) <= 0 ||
        snprintf(base_include, sizeof(base_include),
                 "%s/zcode/installed/%s/include", datadir,
                 base->content_root) <= 0 ||
        snprintf(codec_include, sizeof(codec_include),
                 "%s/zcode/installed/%s/include", datadir,
                 codec->content_root) <= 0 ||
        snprintf(json_include, sizeof(json_include),
                 "%s/zcode/installed/%s/include", datadir,
                 json->content_root) <= 0 ||
        snprintf(app_archive, sizeof(app_archive),
                 "%s/zcode/installed/%s/lib/libcommons-demo.a", datadir,
                 app->content_root) <= 0 ||
        snprintf(base_archive, sizeof(base_archive),
                 "%s/zcode/installed/%s/lib/libbase.a", datadir,
                 base->content_root) <= 0 ||
        snprintf(codec_archive, sizeof(codec_archive),
                 "%s/zcode/installed/%s/lib/libcodec.a", datadir,
                 codec->content_root) <= 0 ||
        snprintf(json_archive, sizeof(json_archive),
                 "%s/zcode/installed/%s/lib/libjson.a", datadir,
                 json->content_root) <= 0)
        return false;
    if (access(binary, F_OK) == 0)
        return false;

    char app_i[1100], base_i[1100], codec_i[1100], json_i[1100];
    if (snprintf(app_i, sizeof(app_i), "-I%s", app_include) <= 0 ||
        snprintf(base_i, sizeof(base_i), "-I%s", base_include) <= 0 ||
        snprintf(codec_i, sizeof(codec_i), "-I%s", codec_include) <= 0 ||
        snprintf(json_i, sizeof(json_i), "-I%s", json_include) <= 0)
        return false;
    const char *compile_argv[] = {
        "cc", "-std=c23", "-O1", "-D_POSIX_C_SOURCE=200809L",
        app_i, base_i, codec_i, json_i, source, app_archive, json_archive,
        codec_archive, base_archive, "-o", binary, NULL,
    };
    char output[1024];
    int compile_rc = zcl_spawn_capture(compile_argv, output, sizeof(output),
                                       30000);
    if (compile_rc != 0) {
        printf("  zcode_package_registry: stranger compile failed rc=%d %s\n",
               compile_rc, output);
        return false;
    }
    const char *run_argv[] = {binary, NULL};
    int run_rc = zcl_spawn_capture(run_argv, output, sizeof(output), 3000);
    if (run_rc != 0 ||
        strcmp(output, "commons|3|030000000700636f6d6d6f6e73\n") != 0) {
        printf("  zcode_package_registry: stranger run failed rc=%d %s\n",
               run_rc, output);
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
    if (ok)
        ok = registry_stranger_build(builder_b);
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
