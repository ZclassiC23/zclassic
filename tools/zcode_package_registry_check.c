/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: rederive and verify the checked-in ZCODE package root projection. */

#include "base/hex.h"
#include "vcs/package_prepare.h"

#include <stdio.h>
#include <string.h>

struct registry_row {
    const char *name, *dir;
    uint64_t sequence;
    const char *content, *release, *recipe, *lock, *capsule, *publisher;
    const char *signature;
};

#define ZCODE_PACKAGE(name, dir, sequence, content, release, recipe, lock, capsule, publisher, signature) \
    {name, dir, sequence, content, release, recipe, lock, capsule, publisher, signature},
static const struct registry_row rows[] = {
#include "../config/zcode_package_registry.def"
};
#undef ZCODE_PACKAGE

static bool root_equal(const uint8_t root[32], const char *expected)
{
    uint8_t decoded[32];
    return zcl_hex_decode_lower(expected, decoded, sizeof(decoded)) &&
           memcmp(root, decoded, sizeof(decoded)) == 0;
}

static void print_derived_roots(const struct vcs_package_prepared *prepared)
{
    char content[65], release[65], recipe[65], lock[65], capsule[65];
    zcl_hex_encode(prepared->package_root, 32, content);
    zcl_hex_encode(prepared->signing_digest, 32, release);
    zcl_hex_encode(prepared->recipe_root, 32, recipe);
    zcl_hex_encode(prepared->lock_root, 32, lock);
    zcl_hex_encode(prepared->capsule_root, 32, capsule);
    fprintf(stderr,
            "  derived content=%s release=%s recipe=%s lock=%s capsule=%s\n",
            content, release, recipe, lock, capsule);
}

int main(void)
{
    if (sizeof(rows) / sizeof(rows[0]) < 8)
        return 1;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        uint8_t pubkey[33];
        if (!zcl_hex_decode_lower(rows[i].publisher, pubkey,
                                  sizeof(pubkey)))
            return 1;
        struct vcs_package_prepare_options options = {
            .dir = rows[i].dir, .publisher_sequence = rows[i].sequence,
            .reward_address = "", .chain_id = "zclassic-main",
        };
        memcpy(options.publisher_pubkey, pubkey, sizeof(pubkey));
        struct vcs_package_prepared prepared; char detail[256] = {0};
        enum vcs_package_prepare_error err = vcs_package_prepare(
            &options, &prepared, detail, sizeof(detail));
        bool ok = err == VCS_PACKAGE_PREPARE_OK &&
            strcmp(prepared.release.name, rows[i].name) == 0 &&
            root_equal(prepared.package_root, rows[i].content) &&
            root_equal(prepared.signing_digest, rows[i].release) &&
            root_equal(prepared.recipe_root, rows[i].recipe) &&
            root_equal(prepared.lock_root, rows[i].lock) &&
            root_equal(prepared.capsule_root, rows[i].capsule) &&
            zcl_hex_decode_lower(rows[i].signature,
                prepared.release.signature,
                sizeof(prepared.release.signature)) &&
            vcs_package_release_verify(&prepared.release) ==
                VCS_PACKAGE_RELEASE_OK;
        if (ok) {
            ok = prepared.lock.count >= 1 &&
                 prepared.lock.nodes[prepared.lock.count - 1u].depth == 0 &&
                 prepared.lock.nodes[prepared.lock.count - 1u].direct_deps ==
                     prepared.lock.count - 1u;
            for (size_t d = 0; ok && d + 1u < prepared.lock.count; d++) {
                bool found = false;
                for (size_t p = 0; p < i; p++)
                    found = found || root_equal(
                        prepared.lock.nodes[d].root, rows[p].content);
                ok = found && prepared.lock.nodes[d].depth == 1;
            }
        }
        if (!ok) {
            fprintf(stderr, "zcode registry mismatch: %s (%s: %s)\n",
                    rows[i].name, vcs_package_prepare_error_string(err),
                    detail);
            if (err == VCS_PACKAGE_PREPARE_OK)
                print_derived_roots(&prepared);
            vcs_package_prepared_free(&prepared);
            return 1;
        }
        vcs_package_prepared_free(&prepared);
    }
    printf("zcode package registry: %zu roots and exact dependency DAG rederived\n",
           sizeof(rows) / sizeof(rows[0]));
    return 0;
}
