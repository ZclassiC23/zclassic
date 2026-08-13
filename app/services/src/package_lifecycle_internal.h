/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Private contract shared by the three ZCODE install-lifecycle translation
 * units. NOT a public header — nothing outside app/services/src includes it.
 *
 *   package_lifecycle.c          the state machine (plan / commit / rollback)
 *   package_lifecycle_store.c    the READ adapter over <datadir>/zcode: CAS
 *                                bytes, release envelopes, manifests,
 *                                recipes, declared dependencies, and the
 *                                re-hash verification
 *   package_lifecycle_install.c  the WRITE adapter: the isolated build
 *                                worker, output re-hashing, atomic install,
 *                                generation log, atomic activation, pinning
 *
 * Every function here returns struct zcl_result: a failure always carries a
 * message, and the caller copies the named rule into its report. */

#ifndef ZCL_SERVICES_PACKAGE_LIFECYCLE_INTERNAL_H
#define ZCL_SERVICES_PACKAGE_LIFECYCLE_INTERNAL_H

#include "services/package_lifecycle.h"

#include "vcs/package_build.h"
#include "vcs/package_deps.h"
#include "vcs/package_manifest.h"
#include "vcs/package_recipe.h"
#include "vcs/package_release.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PKGL_LOG "zcode.lifecycle"
#define PKGL_PATH_MAX 4400u

/* One open lifecycle context: the paths plus every persisted release
 * envelope, loaded once so a plan or a commit sees one consistent view. */
struct pkgl_ctx {
    char datadir[PKGL_PATH_MAX];
    char zcode_dir[PKGL_PATH_MAX];
    struct vcs_package_release *releases; /* heap, bounded */
    size_t release_count;
};

struct zcl_result pkgl_ctx_open(struct pkgl_ctx *ctx, const char *datadir);
void pkgl_ctx_close(struct pkgl_ctx *ctx);

/* Join <zcode_dir>/<rel> into out; fails rather than truncating. */
struct zcl_result pkgl_join(const struct pkgl_ctx *ctx, const char *rel,
                            char *out, size_t cap);

/* Release lookup. `_for_root` is identity; `_for_name` merely SELECTS a
 * root (the highest semver published under that name), which is why it can
 * never be the identity anything is pinned to. NULL when absent. */
const struct vcs_package_release *pkgl_release_for_root(
    const struct pkgl_ctx *ctx, const uint8_t root[32]);
const struct vcs_package_release *pkgl_release_for_name(
    const struct pkgl_ctx *ctx, const char *name);

/* Loaders. The manifest/recipe outputs are initialized by these calls and
 * must be freed by the caller with their own *_free(). */
struct zcl_result pkgl_load_manifest(const struct pkgl_ctx *ctx,
                                     const uint8_t root[32],
                                     struct vcs_package_manifest *out);
struct zcl_result pkgl_load_recipe(const struct pkgl_ctx *ctx,
                                   const uint8_t recipe_root[32],
                                   struct vcs_package_recipe *out);

/* Parse the package's own zcode-package.json (a manifest member, so the
 * package root commits it) into a declared dependency list. A package with
 * no such file has no dependencies — not an error. */
struct zcl_result pkgl_load_declared_deps(const struct pkgl_ctx *ctx,
                                          const uint8_t root[32],
                                          struct vcs_package_deps *out);

/* CAS presence + byte accounting only (no bytes read, no hashing). */
struct zcl_result pkgl_survey_package(const struct pkgl_ctx *ctx,
                                      const uint8_t root[32],
                                      bool *complete_out,
                                      uint64_t *bytes_out,
                                      uint32_t *chunks_out);

/* Full VERIFIED gate for one root: the release envelope verifies, the
 * manifest re-hashes to the root, the recipe root matches the envelope and
 * every recipe path is a manifest member, and EVERY chunk in the CAS
 * re-hashes to the hash committed at its exact coordinates. A tampered CAS
 * object fails here and never reaches a build. */
struct zcl_result pkgl_verify_package(const struct pkgl_ctx *ctx,
                                      const uint8_t root[32],
                                      char *rule_out, size_t rule_cap);

/* ── filesystem primitives (the only place this layer touches disk) ─── */

struct zcl_result pkgl_mkdir_p(const char *path);
struct zcl_result pkgl_rm_rf(const char *path);
struct zcl_result pkgl_read_file(const char *path, size_t cap, uint8_t **out,
                                 size_t *len_out);
struct zcl_result pkgl_write_atomic(const char *path, const uint8_t *data,
                                    size_t len);
struct zcl_result pkgl_sha3_file(const char *path, uint8_t out[32],
                                 uint64_t *bytes_out);
struct zcl_result pkgl_exists(const char *path, bool *out);

/* <zcode_dir>/installed/<root-hex> */
struct zcl_result pkgl_installed_dir(const struct pkgl_ctx *ctx,
                                     const uint8_t root[32], char *out,
                                     size_t cap);

/* ── the write half ─────────────────────────────────────────────────── */

/* Build + test one already-VERIFIED root in the isolated worker, re-hash
 * every emitted artifact against the worker's receipt, persist the receipt,
 * and atomically install the result under installed/<root-hex>. `dep_roots`
 * (in lock build order) are already-installed dependencies whose include
 * dirs and archives the build may use. On any named rejection the step's
 * rule/detail are set and a non-ok result is returned; nothing is left
 * half-installed (staging is removed). */
struct zcl_result pkgl_build_and_install(
    const struct pkgl_ctx *ctx, const uint8_t root[32],
    const struct vcs_package_release *release, const uint8_t lock_root[32],
    const uint8_t (*dep_roots)[32], size_t dep_count,
    struct package_lifecycle_step *step);

/* Re-open an existing install's canonical receipt, verify its filed copy,
 * every bound package/recipe/dependency input and every installed output.
 * A receipt whose package-scoped lock root equals lock_root is marked reused;
 * a valid legacy receipt carrying a different enclosing lock remains evidence
 * of the installed artifact but is not represented as exact evidence reuse. */
struct zcl_result pkgl_verify_installed_receipt(
    const struct pkgl_ctx *ctx, const uint8_t root[32],
    const struct vcs_package_release *release, const uint8_t lock_root[32],
    const uint8_t (*dep_roots)[32], size_t dep_count,
    struct package_lifecycle_step *step);

/* Atomic, rename-based activation of <name> -> installed/<root-hex>, plus
 * the generation-log append. The previous generation stays on disk. */
struct zcl_result pkgl_activate(const struct pkgl_ctx *ctx, const char *name,
                                const uint8_t root[32], int64_t now_unix,
                                uint8_t prev_root_out[32],
                                bool *had_previous_out);

struct zcl_result pkgl_generations_load(const struct pkgl_ctx *ctx,
                                        const char *name,
                                        struct vcs_package_generations *out);

/* Retain the package in the local store so it can seed to peers (PINS pool,
 * never evicted). A pin refusal is reported, never silently swallowed. */
struct zcl_result pkgl_pin(const struct pkgl_ctx *ctx, const uint8_t root[32]);

#endif /* ZCL_SERVICES_PACKAGE_LIFECYCLE_INTERNAL_H */
