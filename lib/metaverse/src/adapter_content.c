/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * The CONTENT adapter: generic content/blob property over lib/vcs's
 * blob_store shape. Authority source: vcs.blob_store.
 *
 * WHAT A BLOB PROVES, AND WHAT IT DOES NOT. A blob root is the manifest
 * root of the frozen one-file/one-chunk package shape, so it commits the
 * length and the SHA3-256 of the bytes and NOTHING ELSE: no publisher, no
 * signature, no chain anchor. blob_store.h calls this the authentication
 * split and preserves it deliberately. This adapter preserves it too:
 *
 *   evidence grade  local_content_hash — this node re-derived the root
 *                   from the manifest wire it holds. Byte identity only.
 *   owner principal "" with owner_principal_kind = "none", because the
 *                   authority records none. That is a FACT about content,
 *                   not a failed lookup, and it is why TRANSFER and
 *                   PUBLISH_REVISION are absent from the action set: there
 *                   is no title to move and no signed descriptor to
 *                   supersede. A caller who wants those wants a ZCODE
 *                   package.
 *   freshness       none. Content has no chain anchor, so stamping a tip
 *                   height beside it would be a false freshness claim.
 *
 * SELL/DELIVER are offered when the bytes are present because what is sold
 * is delivery of bytes the seller holds, not title to them — and only then,
 * because a seller that cannot produce the bytes cannot deliver. */

#include "metaverse_priv.h"

#include "metaverse/property_adapter.h"

#include "base/hex.h"
#include "base/safe_alloc.h"
#include "vcs/blob_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One CAS-present blob supports these. Availability, not authority. */
#define MV_CONTENT_ACTIONS_PRESENT                                           \
    (METAVERSE_ACTION_INSPECT | METAVERSE_ACTION_HOST |                      \
     METAVERSE_ACTION_LIST | METAVERSE_ACTION_SELL |                         \
     METAVERSE_ACTION_DELIVER | METAVERSE_ACTION_LEASE |                     \
     METAVERSE_ACTION_ACCEPT_PAYMENT)

/* The manifest is known but its byte is not in the CAS. Nothing can be
 * hosted, sold, or delivered from an object whose bytes are absent. */
#define MV_CONTENT_ACTIONS_INCOMPLETE (METAVERSE_ACTION_INSPECT)

static const char k_content_provenance[] =
    "blob root commits length+SHA3-256 of the bytes only; no publisher, "
    "signature, or chain anchor";

/* Fill a view from a successfully read blob-shaped manifest. */
static void content_fill(struct metaverse_property_view *out,
                         const struct mv_manifest_read *m)
{
    bool complete = m->chunks_present == m->chunk_total;

    out->has_content_root = true;
    memcpy(out->content_root, m->manifest.files[0].chunk_hashes, 32);
    out->has_descriptor_root  = false; /* content carries no descriptor */
    out->owner_principal[0]   = '\0';
    out->owner_principal_kind = "none";
    out->has_revision         = false;
    out->has_freshness_height = false;
    out->total_bytes          = m->total_bytes;
    out->file_count           = m->file_count;
    out->chunk_total          = m->chunk_total;
    out->chunks_present       = m->chunks_present;
    out->status = complete ? METAVERSE_STATUS_PRESENT
                           : METAVERSE_STATUS_INCOMPLETE;
    out->actions = complete ? MV_CONTENT_ACTIONS_PRESENT
                            : MV_CONTENT_ACTIONS_INCOMPLETE;
    snprintf(out->provenance, sizeof(out->provenance), "%s",
             k_content_provenance);
    (void)metaverse_view_determined(out,
                                    METAVERSE_EVIDENCE_LOCAL_CONTENT_HASH,
                                    "vcs_package_manifest_root");
    if (!complete)
        snprintf(out->reason, sizeof(out->reason),
                 "%u of %u chunk(s) present in the CAS", m->chunks_present,
                 m->chunk_total);
}

static bool content_show(const struct metaverse_adapter_ctx *ctx,
                         const struct metaverse_property_id *id,
                         struct metaverse_property_view *out)
{
    char root_hex[65];
    struct mv_manifest_read m;

    if (!ctx || !id || !out || id->kind != METAVERSE_KIND_CONTENT)
        return false;
    if (!metaverse_view_begin(out, id))
        return false;

    zcl_hex_encode(id->root, 32, root_hex);
    if (!mv_manifest_read(ctx->zcode_dir, root_hex, &m)) {
        /* Asked and answered: the authority holds nothing here. ABSENT is
         * a determined verdict, not a gap — INSPECT stays available so a
         * caller may re-ask, nothing else does. */
        out->status  = METAVERSE_STATUS_ABSENT;
        out->actions = METAVERSE_ACTION_INSPECT;
        snprintf(out->provenance, sizeof(out->provenance), "%s",
                 k_content_provenance);
        snprintf(out->reason, sizeof(out->reason),
                 "no manifest at this root in the local content store");
        (void)metaverse_view_determined(
            out, METAVERSE_EVIDENCE_LOCAL_CONTENT_HASH, "mv_manifest_read");
        return true;
    }
    if (!m.root_matches_name) {
        metaverse_view_undetermined(
            out, "stored manifest re-derives a different root than its own "
                 "filename; refusing to project it");
        mv_manifest_free(&m);
        return true;
    }
    if (!mv_manifest_is_blob(&m.manifest)) {
        /* A real object, just not this kind. Saying "absent" would be a
         * lie; the bytes exist as a zcode_package. */
        metaverse_view_undetermined(
            out, "root is a %u-file package, not the frozen one-file blob "
                 "shape; ask for kind zcode_package",
            m.file_count);
        mv_manifest_free(&m);
        return true;
    }
    content_fill(out, &m);
    mv_manifest_free(&m);
    return true;
}

static size_t content_list(const struct metaverse_adapter_ctx *ctx,
                           struct metaverse_property_view *out,
                           size_t out_cap, size_t *total_out,
                           bool *truncated_out)
{
    char (*names)[65];
    size_t seen = 0;
    size_t scanned;
    size_t written = 0;
    size_t matched = 0;
    bool scan_truncated = false;

    if (total_out)
        *total_out = 0;
    if (truncated_out)
        *truncated_out = false;
    /* out_cap == 0 is the legal count-only call; `out` is then unused. */
    if (!ctx || (!out && out_cap > 0))
        return 0;

    names = zcl_malloc(MV_MANIFEST_SCAN_MAX * sizeof(*names),
                       "mv_content_names");
    if (!names)
        return 0; /* the caller reports the kind as truncated/incomplete */
    scanned = mv_manifest_names(ctx->zcode_dir, names, MV_MANIFEST_SCAN_MAX,
                                &seen, &scan_truncated);

    for (size_t i = 0; i < scanned; i++) {
        struct mv_manifest_read m;
        struct metaverse_property_id id;

        if (!mv_manifest_read(ctx->zcode_dir, names[i], &m))
            continue;
        if (!m.root_matches_name || !mv_manifest_is_blob(&m.manifest)) {
            mv_manifest_free(&m);
            continue;
        }
        matched++;
        if (written >= out_cap) {
            mv_manifest_free(&m);
            continue; /* keep counting: `total` must be the real inventory */
        }
        if (metaverse_property_id_make(METAVERSE_KIND_CONTENT, m.root, &id) &&
            metaverse_view_begin(&out[written], &id)) {
            content_fill(&out[written], &m);
            written++;
        }
        mv_manifest_free(&m);
    }
    free(names);

    if (total_out)
        *total_out = matched;
    if (truncated_out)
        *truncated_out = scan_truncated || written < matched;
    return written;
}

const struct metaverse_adapter *metaverse_adapter_content(void)
{
    static const struct metaverse_adapter k_adapter = {
        .kind = METAVERSE_KIND_CONTENT,
        .unavailable_reason = NULL,
        .list = content_list,
        .show = content_show,
        .store_ready = mv_zcode_store_ready,
    };
    return &k_adapter;
}
