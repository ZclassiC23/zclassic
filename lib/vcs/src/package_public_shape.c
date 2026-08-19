/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * package_public_shape — see vcs/package_public_shape.h. Reads only the
 * public package_store API (no store internals, no store lock held), so a
 * caller may classify while holding its own lock. */

#include "vcs/package_public_shape.h"

#include "base/log_macros.h"
#include "base/safe_alloc.h"
#include "vcs/blob_store.h"
#include "vcs/package_manifest.h"
#include "vcs/package_publish.h"
#include "vcs/package_store.h"
#include "vcs/package_transport.h"
#include "vcs/source_package_transport.h"
#include "vcs/zcode_lane.h"
#include "vcs/zcode_work_context.h"
#include "vcs/zcode_work_output.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHAPE_LOG "vcs.package.public"
#define STORE_SHAPE_PATH_MAX 4400u

/* The carrier's own source copy of the LICENSE file the release names. */
#define SHAPE_TRANSPORT_LICENSE_PATH \
    VCS_PACKAGE_TRANSPORT_SOURCE_PREFIX VCS_PACKAGE_PUBLISH_LICENSE_PATH

const char *vcs_package_public_shape_string(
    enum vcs_package_public_shape shape)
{
    switch (shape) {
    case VCS_PACKAGE_PUBLIC_REFUSED: return "refused";
    case VCS_PACKAGE_PUBLIC_TRANSPORT: return "transport";
    case VCS_PACKAGE_PUBLIC_RELEASE: return "release";
    case VCS_PACKAGE_PUBLIC_SOURCE_BUNDLE: return "source-bundle";
    case VCS_PACKAGE_PUBLIC_BLOB: return "blob";
    case VCS_PACKAGE_PUBLIC_WORK_CONTEXT: return "work-context";
    case VCS_PACKAGE_PUBLIC_WORK_OUTPUT: return "work-output";
    }
    return "unknown";
}

/* Index of `path` in the manifest, or -1. Paths are unique per manifest. */
static long shape_find(const struct vcs_package_manifest *m, const char *path)
{
    for (size_t i = 0; i < m->count; i++)
        if (strcmp(m->files[i].path, path) == 0)
            return (long)i;
    return -1;
}

/* Reassemble one manifest file from the CAS. Bounded by the manifest wire
 * grammar (1 MiB): the three carrier metadata files are the only callers
 * and every one of them is a grammar-bounded wire. NULL on any gap. */
static uint8_t *shape_read_file(struct vcs_package_store *store,
                                const uint8_t root[32],
                                const struct vcs_package_manifest *m,
                                size_t index, size_t *out_len)
{
    *out_len = 0;
    const struct vcs_package_file *file = &m->files[index];
    if (file->size == 0 || file->size > VCS_PACKAGE_MANIFEST_MAX_WIRE_BYTES)
        return NULL;
    uint8_t *buf = zcl_malloc((size_t)file->size, "vcs_public_shape_file");
    if (!buf)
        return NULL;
    size_t written = 0;
    for (uint32_t c = 0; c < file->chunk_count; c++) {
        uint8_t *chunk = NULL;
        size_t chunk_len = 0;
        if (vcs_package_store_get_chunk_at(store, root, (uint32_t)index, c,
                                           &chunk, &chunk_len) !=
                VCS_PACKAGE_STORE_OK ||
            chunk_len > (size_t)file->size - written) {
            free(chunk);
            free(buf);
            return NULL;
        }
        memcpy(buf + written, chunk, chunk_len);
        written += chunk_len;
        free(chunk);
    }
    if (written != (size_t)file->size) {
        free(buf);
        return NULL;
    }
    *out_len = written;
    return buf;
}

/* Re-derive the whole carrier closure from the bytes we hold and require it
 * to hash back to this exact root. vcs_package_transport_build() verifies
 * the signature, enforces the frozen SPDX allowlist, runs the publication
 * rules (LICENSE text included), and binds release <-> recipe <-> inner
 * manifest; the root comparison binds all of that to the bytes a peer would
 * actually receive. A stapled envelope therefore proves nothing. */
static bool shape_transport_closes(struct vcs_package_store *store,
                                   const uint8_t root[32],
                                   const struct vcs_package_manifest *m,
                                   long release_index, const char **rule_out)
{
    long recipe_index = shape_find(m, VCS_PACKAGE_TRANSPORT_RECIPE_PATH);
    long manifest_index = shape_find(m, VCS_PACKAGE_TRANSPORT_MANIFEST_PATH);
    if (recipe_index < 0 || manifest_index < 0) {
        *rule_out = "carrier-metadata-missing";
        return false;
    }
    if (shape_find(m, SHAPE_TRANSPORT_LICENSE_PATH) < 0) {
        *rule_out = "license-text-missing";
        return false;
    }
    size_t release_len = 0, recipe_len = 0, inner_len = 0;
    uint8_t *release = shape_read_file(store, root, m, (size_t)release_index,
                                       &release_len);
    uint8_t *recipe = shape_read_file(store, root, m, (size_t)recipe_index,
                                      &recipe_len);
    uint8_t *inner = shape_read_file(store, root, m, (size_t)manifest_index,
                                     &inner_len);
    bool closed = false;
    const char *rule = "carrier-metadata-unreadable";
    if (release && recipe && inner) {
        struct vcs_package_transport transport;
        vcs_package_transport_init(&transport);
        enum vcs_package_transport_result r = vcs_package_transport_build(
            release, release_len, recipe, recipe_len, inner, inner_len,
            &transport);
        if (r != VCS_PACKAGE_TRANSPORT_OK) {
            rule = r == VCS_PACKAGE_TRANSPORT_ERR_RELEASE
                       ? "release-unverified"
                       : r == VCS_PACKAGE_TRANSPORT_ERR_BINDING
                             ? "release-binding-failed"
                             : "carrier-closure-failed";
        } else if (memcmp(transport.transport_root, root, 32) != 0) {
            rule = "carrier-root-mismatch";
        } else if (!vcs_package_release_license_allowed(
                       transport.release.license)) {
            /* Unreachable while the envelope grammar owns the allowlist;
             * asserted anyway so the licence rule is stated where it is
             * enforced rather than inherited silently. */
            rule = "spdx-license-not-allowlisted";
        } else {
            closed = true;
        }
        vcs_package_transport_free(&transport);
    }
    free(release);
    free(recipe);
    free(inner);
    if (!closed)
        *rule_out = rule;
    return closed;
}

/* Does a persisted release envelope name and sign exactly these bytes?
 *
 * The store files a committed manifest under manifests/<root-hex> only
 * after re-deriving that root from the wire, so "the envelope's
 * package_root equals this root" is already the statement that the
 * publisher signed the bytes a peer would receive — no second binding
 * step is needed here. Verification (signature, low-S, and the frozen
 * SPDX allowlist the envelope grammar owns) runs on the candidate itself.
 * Scans releases/ and stops at the first envelope that matches and
 * verifies. */
static bool shape_release_signs(struct vcs_package_store *store,
                                const uint8_t root[32])
{
    const char *zcode_dir = vcs_package_store_root_dir(store);
    if (!zcode_dir)
        return false;
    char dir[STORE_SHAPE_PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/releases", zcode_dir);
    if (n < 0 || (size_t)n >= sizeof(dir))
        return false;
    DIR *d = opendir(dir);
    if (!d)
        return false; /* no releases yet: nothing is publicly releasable */
    uint8_t *wire = zcl_malloc(VCS_PACKAGE_RELEASE_MAX_WIRE_BYTES,
                               "vcs_public_shape_release");
    bool signed_here = false;
    struct dirent *de;
    while (wire && !signed_here && (de = readdir(d)) != NULL) {
        char path[STORE_SHAPE_PATH_MAX];
        n = snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(path))
            continue;
        FILE *f = fopen(path, "rb");
        if (!f)
            continue;
        size_t len = fread(wire, 1, VCS_PACKAGE_RELEASE_MAX_WIRE_BYTES, f);
        bool trailing = !feof(f);
        fclose(f);
        struct vcs_package_release release;
        if (trailing ||
            vcs_package_release_parse(wire, len, &release) !=
                VCS_PACKAGE_RELEASE_OK ||
            memcmp(release.package_root, root, 32) != 0)
            continue;
        signed_here =
            vcs_package_release_verify(&release) == VCS_PACKAGE_RELEASE_OK &&
            vcs_package_release_license_allowed(release.license);
    }
    free(wire);
    closedir(d);
    return signed_here;
}

/* The ZVCS source carrier: LICENSE text plus a lane receipt signed by the
 * key it names. Signature-against-embedded-key is the same standard the
 * release envelope is held to; what this does NOT do is walk the
 * accepted-work authority chain, which the consumer verifies on checkout. */
static bool shape_source_bundle(struct vcs_package_store *store,
                                const uint8_t root[32],
                                const struct vcs_package_manifest *m,
                                const char **rule_out)
{
    long lane = shape_find(m, VCS_SOURCE_PACKAGE_LANE_PATH);
    if (lane < 0 || shape_find(m, VCS_SOURCE_PACKAGE_AUTHORITY_PATH) < 0) {
        *rule_out = "source-bundle-authority-missing";
        return false;
    }
    long license = shape_find(m, VCS_SOURCE_PACKAGE_LICENSE_PATH);
    if (license < 0 || m->files[license].size == 0) {
        *rule_out = "license-text-missing";
        return false;
    }
    size_t lane_len = 0;
    uint8_t *lane_wire = shape_read_file(store, root, m, (size_t)lane,
                                         &lane_len);
    if (!lane_wire) {
        *rule_out = "lane-receipt-unreadable";
        return false;
    }
    struct vcs_zcode_lane_receipt_v1 receipt;
    memset(&receipt, 0, sizeof(receipt));
    bool signed_ok =
        vcs_zcode_lane_receipt_parse(lane_wire, lane_len, &receipt) ==
            VCS_ZCODE_DEV_OK &&
        vcs_zcode_lane_receipt_validate(&receipt) == VCS_ZCODE_DEV_OK &&
        vcs_zcode_lane_receipt_verify(&receipt, receipt.signer_pubkey) ==
            VCS_ZCODE_DEV_OK;
    free(lane_wire);
    if (!signed_ok)
        *rule_out = "lane-receipt-unverified";
    return signed_ok;
}

static bool shape_is_blob(const struct vcs_package_manifest *m)
{
    return m->count == 1 && strcmp(m->files[0].path, VCS_BLOB_PATH) == 0 &&
           m->files[0].chunk_count == 1 &&
           m->files[0].size <= (uint64_t)VCS_BLOB_MAX_BYTES;
}

static bool shape_is_work_output(const struct vcs_package_manifest *m)
{
    return m->count == 2 &&
           shape_find(m, VCS_ZCODE_WORK_OUTPUT_ACTION_PATH) >= 0 &&
           shape_find(m, VCS_ZCODE_WORK_OUTPUT_BYTES_PATH) >= 0;
}

enum vcs_package_public_shape vcs_package_public_shape_classify(
    struct vcs_package_store *store, const uint8_t package_root[32],
    const char **rule_out)
{
    const char *rule = "not-tracked";
    enum vcs_package_public_shape shape = VCS_PACKAGE_PUBLIC_REFUSED;
    if (!store || !package_root) {
        if (rule_out) *rule_out = "null-input";
        return VCS_PACKAGE_PUBLIC_REFUSED;
    }
    /* Incomplete first: a partial download must never leave this node, not
     * even as the manifest that names what is still missing. */
    struct vcs_package_store_status status;
    if (!vcs_package_store_package_status(store, package_root, &status) ||
        !status.tracked) {
        if (rule_out) *rule_out = rule;
        return VCS_PACKAGE_PUBLIC_REFUSED;
    }
    if (!status.complete) {
        if (rule_out) *rule_out = "package-incomplete";
        return VCS_PACKAGE_PUBLIC_REFUSED;
    }

    uint8_t *wire = NULL;
    size_t wire_len = 0;
    if (vcs_package_store_get_manifest_wire(store, package_root, &wire,
                                            &wire_len) !=
        VCS_PACKAGE_STORE_OK) {
        free(wire);
        if (rule_out) *rule_out = "manifest-unreadable";
        return VCS_PACKAGE_PUBLIC_REFUSED;
    }
    struct vcs_package_manifest m;
    bool parsed = vcs_package_manifest_parse(wire, wire_len, &m);
    free(wire);
    if (!parsed) {
        if (rule_out) *rule_out = "manifest-unparseable";
        return VCS_PACKAGE_PUBLIC_REFUSED;
    }

    long release_index = shape_find(&m, VCS_PACKAGE_TRANSPORT_RELEASE_PATH);
    if (release_index >= 0) {
        rule = "release-unverified";
        if (shape_transport_closes(store, package_root, &m, release_index,
                                   &rule)) {
            shape = VCS_PACKAGE_PUBLIC_TRANSPORT;
            rule = vcs_package_public_shape_string(shape);
        }
    } else if (shape_find(&m, VCS_SOURCE_PACKAGE_MARKER_PATH) >= 0) {
        rule = "source-bundle-authority-missing";
        if (shape_source_bundle(store, package_root, &m, &rule)) {
            shape = VCS_PACKAGE_PUBLIC_SOURCE_BUNDLE;
            rule = vcs_package_public_shape_string(shape);
        }
    } else if (shape_is_blob(&m)) {
        shape = VCS_PACKAGE_PUBLIC_BLOB;
        rule = vcs_package_public_shape_string(shape);
    } else if (shape_find(&m, VCS_ZCODE_WORK_CONTEXT_PATH) >= 0) {
        shape = VCS_PACKAGE_PUBLIC_WORK_CONTEXT;
        rule = vcs_package_public_shape_string(shape);
    } else if (shape_is_work_output(&m)) {
        shape = VCS_PACKAGE_PUBLIC_WORK_OUTPUT;
        rule = vcs_package_public_shape_string(shape);
    } else if (!shape_release_signs(store, package_root)) {
        rule = "no-verified-release";
    } else if (shape_find(&m, VCS_PACKAGE_PUBLISH_LICENSE_PATH) < 0) {
        rule = "license-text-missing";
    } else {
        shape = VCS_PACKAGE_PUBLIC_RELEASE;
        rule = vcs_package_public_shape_string(shape);
    }
    vcs_package_manifest_free(&m);
    if (rule_out)
        *rule_out = rule;
    return shape;
}
