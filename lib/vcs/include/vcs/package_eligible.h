/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * package_eligible — the ZCODE reward eligibility gate list (slice 7).
 * A release earns NOTHING until EVERY gate passes; this layer assembles
 * the frozen gate list from facts the caller gathered (manifest/CAS
 * verification, envelope signature, license, parent lineage, and the
 * slice-6 attestation quorum) and reports eligible=true/false with every
 * failed gate named. Pure evaluation over caller-supplied facts: no
 * filesystem, network, wallet, build, execution, or node-state
 * authority. Deterministic: same facts, same report.
 *
 * The eight gates (frozen order — the report walks them in enum order):
 *   1. package-root-verifies   the manifest parses, its root equals the
 *                              envelope's package_root, and every
 *                              committed chunk was re-verified from the CAS
 *   2. release-signature-verifies  the envelope verifies (fields, low-S,
 *                              ECDSA against the embedded publisher key)
 *   3. license-accepted        the SPDX id is on the v1 allowlist and the
 *                              package carries its LICENSE text file
 *   4. parent-lineage-valid    the named parent release exists, verifies,
 *                              shares the publisher key, and the sequence
 *                              increments by exactly one (a root release
 *                              passes trivially)
 *   5. gcc-build-passes        a counted quorum attestation reports the
 *                              gcc outcome PASS
 *   6. clang-build-passes      a counted quorum attestation reports the
 *                              clang outcome PASS
 *   7. tests-pass              the quorum class is test-pass
 *   8. verifier-quorum         >= 2 approved independent verifier keys
 *                              signed matching attestations (slice 6)
 * Gates 5-7 are read from the SAME counted quorum attestations that gate
 * 8 evaluates; without a verified quorum they fail with "no verified
 * quorum" — a release never earns from a single verifier or from
 * self-verification. */

#ifndef ZCL_VCS_PACKAGE_ELIGIBLE_H
#define ZCL_VCS_PACKAGE_ELIGIBLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VCS_REWARD_GATE_DETAIL_MAX 160u

/* The enum order is frozen: it appears in typed JSON. */
enum vcs_reward_gate {
    VCS_REWARD_GATE_PACKAGE_ROOT = 0,
    VCS_REWARD_GATE_RELEASE_SIGNATURE,
    VCS_REWARD_GATE_LICENSE,
    VCS_REWARD_GATE_PARENT_LINEAGE,
    VCS_REWARD_GATE_GCC_BUILD,
    VCS_REWARD_GATE_CLANG_BUILD,
    VCS_REWARD_GATE_TESTS_PASS,
    VCS_REWARD_GATE_VERIFIER_QUORUM,
    VCS_REWARD_GATE_COUNT /* 8: not a gate */
};

const char *vcs_reward_gate_string(enum vcs_reward_gate gate);

/* Every fact the gate list consumes, gathered by the caller. */
struct vcs_reward_eligibility_input {
    /* Gate 1. chunks_checked false means the CAS was not consulted. */
    bool manifest_parsed;
    bool root_matches;
    bool chunks_checked;
    uint32_t chunks_verified;
    uint32_t chunks_total;
    /* Gate 2 (vcs_package_release_verify == VCS_PACKAGE_RELEASE_OK). */
    bool release_verifies;
    /* Gate 3: envelope license on the allowlist AND the LICENSE text
     * file present in the manifest. */
    bool license_accepted;
    /* Gate 4 (caller-walked; a root release reports true with the detail
     * naming it a root release). */
    bool lineage_valid;
    const char *lineage_detail; /* borrowed; may be NULL */
    /* Gates 5-8, from the slice-6 quorum evaluation over the counted
     * (matching, approved, independent) attestations. */
    bool quorum_verified;
    bool gcc_pass;
    bool clang_pass;
    bool tests_pass;
};

struct vcs_reward_gate_row {
    enum vcs_reward_gate gate;
    bool passed;
    char detail[VCS_REWARD_GATE_DETAIL_MAX];
};

struct vcs_reward_eligibility {
    bool eligible;
    struct vcs_reward_gate_row gates[VCS_REWARD_GATE_COUNT];
    size_t failed_count;
};

/* Evaluate the gate list. Gates are walked in enum order; every gate is
 * reported (no early exit) so the reply names every failed gate. */
void vcs_reward_eligibility_evaluate(
    const struct vcs_reward_eligibility_input *in,
    struct vcs_reward_eligibility *out);

#endif /* ZCL_VCS_PACKAGE_ELIGIBLE_H */
