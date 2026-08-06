/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: rederive and verify the checked-in ZCODE package root projection. */

#include "base/hex.h"
#include "vcs/package_prepare.h"

#include <stdio.h>
#include <string.h>

struct registry_row {
    const char *name, *dir;
    uint64_t sequence;
    const char *content, *release, *recipe, *lock, *capsule, *dependency;
    const char *signature;
};

#define ZCODE_PACKAGE(name, dir, sequence, content, release, recipe, lock, capsule, dependency, signature) \
    {name, dir, sequence, content, release, recipe, lock, capsule, dependency, signature},
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

int main(void)
{
    static const char pubkey_hex[] =
        "03448effe2ae40eb4053acfceb9839163c881b22affff9572283caddeee9207ce4";
    uint8_t pubkey[33];
    if (sizeof(rows) / sizeof(rows[0]) != 3 ||
        !zcl_hex_decode_lower(pubkey_hex, pubkey, sizeof(pubkey)))
        return 1;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
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
        if (ok && i == 0)
            ok = rows[i].dependency[0] == '\0' && prepared.lock.count == 1;
        else if (ok)
            ok = prepared.lock.count == 2 &&
                 root_equal(prepared.lock.nodes[0].root, rows[i].dependency);
        vcs_package_prepared_free(&prepared);
        if (!ok) {
            fprintf(stderr, "zcode registry mismatch: %s (%s: %s)\n",
                    rows[i].name, vcs_package_prepare_error_string(err),
                    detail);
            return 1;
        }
    }
    puts("zcode package registry: 3 roots and exact dependency DAG rederived");
    return 0;
}
