/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * package_public_shape — the admission rule for offering a locally held
 * content.v2 root to strangers. Hosting is opt-in (-packagehost=1); this
 * decides what an opted-in host may announce and serve once it is on.
 *
 * DEFAULT REFUSE. The store tracks every root the node has ever admitted:
 * packages it published, carriers it fetched, in-flight downloads, work
 * exchanged with peers, and the inner package roots reconstructed out of a
 * carrier. Completeness alone used to make all of them announceable and
 * serveable. It does not any more. A root reaches the public swarm only by
 * matching one of the closed set of shapes below; everything else is
 * refused by name.
 *
 *   TRANSPORT     a zcode package transport carrier. This is the only
 *                 shape that carries source for other people to build and
 *                 run, so it is the one that must prove who wrote it and
 *                 under what terms: the carrier's own
 *                 zcl.zcode_release.v1 envelope must verify against the
 *                 publisher key it names, its SPDX identifier must be on
 *                 the frozen v1 allowlist, its sources must carry LICENSE
 *                 text, and release, recipe and inner manifest must bind
 *                 to each other and to this exact root. That whole
 *                 closure is re-derived here from the stored bytes with
 *                 vcs_package_transport_build(); nothing is taken on
 *                 trust from the fetch that delivered it.
 *   RELEASE       the inner package a carrier reconstructs into: a plain
 *                 content.v2 root that a persisted zcl.zcode_release.v1
 *                 envelope names and signs. Same three requirements as
 *                 TRANSPORT — the envelope verifies, its SPDX identifier
 *                 is on the allowlist, the manifest carries LICENSE text
 *                 — proved against the envelope rather than against a
 *                 carrier, because the manifest root the store filed
 *                 these bytes under IS the root the publisher signed.
 *   SOURCE_BUNDLE the ZVCS source carrier an accepted work publishes. It
 *                 carries its own top-level LICENSE and a lane receipt
 *                 signed by the key that receipt names, both of which are
 *                 checked here. The full accepted-work authority chain is
 *                 NOT re-derived at serve time — proving it means
 *                 reconstructing the tree
 *                 (vcs_source_package_reconstruct_verify), which is the
 *                 consumer's step on checkout, not a per-WANT one.
 *   BLOB          the frozen one-file 8 KiB blob shape. Bytes only, by
 *                 contract: it carries no authorship claim and cannot
 *                 carry a source tree. lib/zid asks whether the document
 *                 inside is genuine, after the bytes arrive.
 *   WORK_CONTEXT  one fixed build action a requester sent to a worker.
 *   WORK_OUTPUT   the action-bound output a worker returned.
 *
 * The two work shapes are admitted because they move between peers that
 * have already accepted each other's SIGNED work frames, and are fetched
 * directly from that authenticated sender rather than off a broadcast.
 * They are NOT licensed content and this layer does not claim they are:
 * the work node's own admission is what governs them. Stated plainly so
 * nobody reads this rule as more than it is.
 *
 * A refusal is never silence. Every REFUSED verdict comes with a static
 * rule string naming which requirement failed. */

#ifndef ZCL_VCS_PACKAGE_PUBLIC_SHAPE_H
#define ZCL_VCS_PACKAGE_PUBLIC_SHAPE_H

#include <stdbool.h>
#include <stdint.h>

struct vcs_package_store;

enum vcs_package_public_shape {
    VCS_PACKAGE_PUBLIC_REFUSED = 0,  /* never announce, never serve */
    VCS_PACKAGE_PUBLIC_TRANSPORT,    /* signed, permissively licensed */
    VCS_PACKAGE_PUBLIC_RELEASE,      /* the inner package a release names */
    VCS_PACKAGE_PUBLIC_SOURCE_BUNDLE, /* signed ZVCS accepted-work carrier */
    VCS_PACKAGE_PUBLIC_BLOB,         /* one-file bytes-only object */
    VCS_PACKAGE_PUBLIC_WORK_CONTEXT, /* one fixed build action */
    VCS_PACKAGE_PUBLIC_WORK_OUTPUT,  /* action-bound work output */
};

const char *vcs_package_public_shape_string(
    enum vcs_package_public_shape shape);

/* Classify one tracked root against the rule above, reading only bytes the
 * store already holds. Returns REFUSED for an untracked, incomplete or
 * unrecognized root and for a carrier whose closure does not re-derive.
 * `*rule_out` (when non-NULL) always receives a static string: the shape
 * name on admission, the failed requirement on refusal. */
enum vcs_package_public_shape vcs_package_public_shape_classify(
    struct vcs_package_store *store, const uint8_t package_root[32],
    const char **rule_out);

#endif /* ZCL_VCS_PACKAGE_PUBLIC_SHAPE_H */
