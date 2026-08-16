/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Supervisor-only, non-publishing build release qualification. */

#include "services/build_fabric_service.h"

#include "base/hex.h"
#include "crypto/sha3.h"
#include "vcs/build_release_qualification.h"
#include "vcs/vcs_object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct zcl_result bfr_refuse(
    struct build_fabric_release_qualification_report *out,
    const char *invariant)
{
    if (out)
        (void)snprintf(out->first_bad_invariant,
                       sizeof(out->first_bad_invariant), "%s", invariant);
    return ZCL_ERR(-1, "%s", invariant);
}

static void bfr_worker_id(const uint8_t pubkey[32], char out[65])
{
    static const char domain[] = "zcl.build_worker.v1";
    struct sha3_256_ctx sha;
    uint8_t digest[32];
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)domain, sizeof(domain));
    sha3_256_write(&sha, pubkey, 32);
    sha3_256_finalize(&sha, digest);
    zcl_hex_encode(digest, 32, out);
}

static bool bfr_capability(const char *list, const char *wanted)
{
    if (!list || !wanted || !wanted[0]) return false;
    size_t wanted_len = strlen(wanted);
    for (const char *at = list; *at;) {
        while (*at == ',') at++;
        const char *end = strchr(at, ',');
        size_t len = end ? (size_t)(end - at) : strlen(at);
        if (len == wanted_len && memcmp(at, wanted, len) == 0) return true;
        if (!end) break;
        at = end + 1;
    }
    return false;
}

static bool bfr_executor_approved(
    struct node_db *ndb, const struct db_build_receipt *receipt,
    int64_t now, struct db_build_worker *out)
{
    return db_build_worker_find(ndb, receipt->worker_id, out) &&
        out->approved && !out->revoked &&
        out->approved_at <= receipt->created_at &&
        (out->expires_at == 0 || receipt->created_at < out->expires_at) &&
        (out->expires_at == 0 || now < out->expires_at);
}

static bool bfr_raw_sha3_object(const char *workspace,
                                const uint8_t root[32])
{
    uint8_t *wire = NULL, observed[32];
    size_t wire_len = 0;
    if (vcs_object_load_raw_bounded(
            workspace, root, 1024u * 1024u, &wire, &wire_len) != 0)
        return false;
    sha3_256(wire, wire_len, observed);
    free(wire);
    return memcmp(root, observed, 32) == 0;
}

static void bfr_sort_roots(uint8_t roots[3][32])
{
    for (size_t i = 0; i < 2; i++) {
        size_t least = i;
        for (size_t j = i + 1; j < 3; j++)
            if (memcmp(roots[j], roots[least], 32) < 0) least = j;
        if (least != i) {
            uint8_t swap[32];
            memcpy(swap, roots[i], 32);
            memcpy(roots[i], roots[least], 32);
            memcpy(roots[least], swap, 32);
        }
    }
}

static void bfr_proof_set_root(uint8_t roots[3][32], uint8_t out[32])
{
    static const char domain[] = "zcl.build_release_execution_set.v1";
    bfr_sort_roots(roots);
    struct sha3_256_ctx sha;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)domain, sizeof(domain));
    sha3_256_write(&sha, (const uint8_t[]){3}, 1);
    sha3_256_write(&sha, roots[0], 3u * 32u);
    sha3_256_finalize(&sha, out);
}

struct zcl_result build_fabric_release_qualify(
    struct node_db *ndb, const char *workspace,
    const char *confirmation_root_sha3, int64_t now,
    struct build_fabric_release_qualification_report *out)
{
    if (out) memset(out, 0, sizeof(*out));
    uint8_t confirmation_root[32], checked_root[32], *wire = NULL;
    size_t wire_len = 0;
    if (!ndb || !ndb->open || !workspace || !workspace[0] || !out ||
        now <= 0 || !zcl_hex_decode_lower(
            confirmation_root_sha3, confirmation_root, 32))
        return bfr_refuse(out, "release-qualification-input-invalid");
    if (vcs_object_load_raw_bounded(
            workspace, confirmation_root,
            VCS_BUILD_RELEASE_CONFIRMATION_WIRE_BYTES,
            &wire, &wire_len) != 0)
        return bfr_refuse(out, "human-confirmation-absent");
    struct vcs_build_release_confirmation_v1 confirmation;
    bool confirmation_valid =
        vcs_build_release_confirmation_v1_parse(
            wire, wire_len, &confirmation) ==
                VCS_BUILD_RELEASE_EVIDENCE_OK &&
        vcs_build_release_confirmation_v1_root(
            &confirmation, checked_root) ==
                VCS_BUILD_RELEASE_EVIDENCE_OK &&
        memcmp(confirmation_root, checked_root, 32) == 0 &&
        vcs_build_release_confirmation_v1_verify(&confirmation) ==
            VCS_BUILD_RELEASE_EVIDENCE_OK;
    free(wire);
    if (!confirmation_valid)
        return bfr_refuse(out, "human-confirmation-invalid");
    if (confirmation.decision != VCS_BUILD_RELEASE_DECISION_CONFIRM)
        return bfr_refuse(out, "human-cancelled-release");
    if (confirmation.confirmed_unix > now)
        return bfr_refuse(out, "human-confirmation-from-future");
    out->human_confirmed = true;
    zcl_hex_encode(confirmation_root, 32, out->confirmation_root_sha3);

    char action_id[65], artifact_id[65];
    char candidate_id[65], shadow_id[65], reproduction_id[65];
    zcl_hex_encode(confirmation.action_root, 32, action_id);
    zcl_hex_encode(confirmation.artifact_root, 32, artifact_id);
    zcl_hex_encode(confirmation.candidate_receipt_root, 32, candidate_id);
    zcl_hex_encode(confirmation.shadow_receipt_root, 32, shadow_id);
    zcl_hex_encode(confirmation.reproduction_receipt_root, 32,
                   reproduction_id);
    struct db_build_action action;
    struct db_build_receipt candidate, shadow, reproduction;
    if (!db_build_action_find(ndb, action_id, &action) ||
        strcmp(action.state, "ACCEPTED") != 0 ||
        strcmp(action.output_root_sha3, artifact_id) != 0 ||
        !db_build_receipt_find(ndb, candidate_id, &candidate) ||
        !db_build_receipt_find(ndb, shadow_id, &shadow) ||
        !db_build_receipt_find(ndb, reproduction_id, &reproduction) ||
        strcmp(candidate.action_id, action_id) != 0 ||
        strcmp(shadow.action_id, action_id) != 0 ||
        strcmp(reproduction.action_id, action_id) != 0 ||
        strcmp(candidate.output_sha3, artifact_id) != 0 ||
        strcmp(shadow.output_sha3, artifact_id) != 0 ||
        strcmp(reproduction.output_sha3, artifact_id) != 0 ||
        strcmp(candidate.trust_state, "LOCAL_ACCEPTED") != 0)
        return bfr_refuse(out, "candidate-admission-chain-invalid");
    out->candidate_admitted = true;
    (void)snprintf(out->artifact_root_sha3,
                   sizeof(out->artifact_root_sha3), "%s", artifact_id);
    struct db_build_worker candidate_worker, shadow_worker;
    struct db_build_worker reproduction_worker;
    if (!bfr_executor_approved(
            ndb, &candidate, now, &candidate_worker) ||
        !bfr_executor_approved(ndb, &shadow, now, &shadow_worker) ||
        !bfr_executor_approved(
            ndb, &reproduction, now, &reproduction_worker))
        return bfr_refuse(out, "executor-signer-not-approved");
    if (strcmp(candidate_worker.signer_pubkey,
               shadow_worker.signer_pubkey) == 0 ||
        strcmp(candidate_worker.signer_pubkey,
               reproduction_worker.signer_pubkey) == 0 ||
        strcmp(shadow_worker.signer_pubkey,
               reproduction_worker.signer_pubkey) == 0)
        return bfr_refuse(out, "executor-signers-not-distinct");
    out->distinct_executor_signers = true;

    struct build_fabric_shadow_match match = {0};
    if (!build_fabric_clean_shadow_compare(
            ndb, workspace, candidate_id, shadow_id, &match).ok)
        return bfr_refuse(out, match.first_bad_invariant[0]
            ? match.first_bad_invariant : "clean-shadow-invalid");
    out->clean_shadow_match = true;
    if (!build_fabric_clean_shadow_compare(
            ndb, workspace, candidate_id, reproduction_id, &match).ok)
        return bfr_refuse(out, match.first_bad_invariant[0]
            ? match.first_bad_invariant : "independent-reproduction-invalid");
    out->independent_reproduction_match = true;

    const uint8_t *const machine_roots[] = {
        confirmation.candidate_machine_evidence_root,
        confirmation.shadow_machine_evidence_root,
        confirmation.reproduction_machine_evidence_root,
    };
    for (size_t i = 0; i < 3; i++)
        if (!bfr_raw_sha3_object(workspace, machine_roots[i]))
            return bfr_refuse(out, "physical-machine-evidence-invalid");
    out->physical_evidence_present = true;

    char confirmer_id[65];
    struct db_build_worker confirmer;
    bfr_worker_id(confirmation.confirmer_pubkey, confirmer_id);
    if (!db_build_worker_find(ndb, confirmer_id, &confirmer) ||
        !confirmer.approved || confirmer.revoked ||
        (confirmer.expires_at != 0 && now >= confirmer.expires_at) ||
        !bfr_capability(confirmer.capabilities,
                        "release-confirmation.v1") ||
        confirmer.approved_at > confirmation.confirmed_unix ||
        strcmp(confirmer.signer_pubkey,
               candidate_worker.signer_pubkey) == 0 ||
        strcmp(confirmer.signer_pubkey, shadow_worker.signer_pubkey) == 0 ||
        strcmp(confirmer.signer_pubkey,
               reproduction_worker.signer_pubkey) == 0)
        return bfr_refuse(out, "release-confirmer-not-approved-independent");
    out->confirmer_approved = true;

    struct vcs_build_release_qualification_v1 qualification = {
        .schema_version = VCS_BUILD_RELEASE_QUALIFICATION_VERSION,
        .flags = VCS_BUILD_RELEASE_QUAL_REQUIRED_FLAGS,
        .qualified_unix = confirmation.confirmed_unix,
    };
    memcpy(qualification.action_root, confirmation.action_root, 32);
    memcpy(qualification.artifact_root, confirmation.artifact_root, 32);
    uint8_t observation_root[32];
    if (!zcl_hex_decode_lower(candidate.observation_sha3,
                              observation_root, 32))
        return bfr_refuse(out, "candidate-observation-root-invalid");
    memcpy(qualification.observation_root, observation_root, 32);
    memcpy(qualification.candidate_receipt_root,
           confirmation.candidate_receipt_root, 32);
    memcpy(qualification.shadow_receipt_root,
           confirmation.shadow_receipt_root, 32);
    memcpy(qualification.reproduction_receipt_root,
           confirmation.reproduction_receipt_root, 32);
    memcpy(qualification.confirmation_root, confirmation_root, 32);
    uint8_t execution_roots[3][32];
    memcpy(execution_roots[0], confirmation.candidate_receipt_root, 32);
    memcpy(execution_roots[1], confirmation.shadow_receipt_root, 32);
    memcpy(execution_roots[2], confirmation.reproduction_receipt_root, 32);
    bfr_proof_set_root(execution_roots, qualification.proof_set_root);
    uint8_t qualification_wire[VCS_BUILD_RELEASE_QUALIFICATION_WIRE_BYTES];
    uint8_t qualification_root[32];
    if (vcs_build_release_qualification_v1_serialize(
            &qualification, qualification_wire) !=
                VCS_BUILD_RELEASE_EVIDENCE_OK ||
        vcs_build_release_qualification_v1_root(
            &qualification, qualification_root) !=
                VCS_BUILD_RELEASE_EVIDENCE_OK ||
        !vcs_object_put_addressed(
            workspace, qualification_root, qualification_wire,
            sizeof(qualification_wire)))
        return bfr_refuse(out, "qualified-release-cas-write-failed");
    uint8_t *stored = NULL;
    size_t stored_len = 0;
    bool stored_exact = vcs_object_load_raw_bounded(
            workspace, qualification_root, sizeof(qualification_wire),
            &stored, &stored_len) == 0 &&
        stored_len == sizeof(qualification_wire) &&
        memcmp(stored, qualification_wire, sizeof(qualification_wire)) == 0;
    free(stored);
    if (!stored_exact)
        return bfr_refuse(out, "qualified-release-cas-poisoned");
    zcl_hex_encode(qualification_root, 32, out->qualification_root_sha3);
    out->publication_performed = false;
    return ZCL_OK;
}
