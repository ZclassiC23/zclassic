/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Contained validation of the canonical external consensus-state bundle. */

#ifndef ZCL_CONSENSUS_STATE_SNAPSHOT_INSTALL_H
#define ZCL_CONSENSUS_STATE_SNAPSHOT_INSTALL_H

#include <stdbool.h>
#include <stdint.h>

#include "storage/consensus_state_bundle_codec.h"
#include "util/result.h"

struct sqlite3;
struct consensus_state_artifact_evidence;

/* Legacy USS v1-v3 artifacts may be converted by separate import adapters.
 * They are never active schemas and are never passed directly to this API. */
enum consensus_state_install_failpoint {
    CONSENSUS_INSTALL_FAIL_NONE = 0,
    CONSENSUS_INSTALL_FAIL_AFTER_BUNDLE_OPEN,
    CONSENSUS_INSTALL_FAIL_AFTER_BUNDLE_VALIDATE,
};

struct consensus_state_snapshot_install_request {
    /* Immutable external SQLite bundle. It holds `bundle_meta`, `coins`,
     * `anchors`, and `nullifiers`; rows are streamed by SQLite, never loaded
     * into process memory or staged inside the active progress.kv WAL. */
    const char *bundle_path;
    /* Caller assertion only. It catches selecting the wrong artifact but is
     * not local-chain proof and never authorizes activation. The protected
     * adapter will replace this pair with opaque digest-bound evidence after
     * resolving selected-chain ancestry and the Sapling header root. */
    int32_t expected_height;
    uint8_t expected_block_hash[32];
    enum consensus_state_install_failpoint failpoint;
};

enum consensus_state_install_status {
    CONSENSUS_INSTALL_REFUSED = 0,
    CONSENSUS_INSTALL_INJECTED_FAILURE = 1,
    CONSENSUS_INSTALL_STORE_ERROR = 2,
    CONSENSUS_INSTALL_VERIFIED_CONTAINED = 3,
};

struct consensus_state_install_result {
    enum consensus_state_install_status status;
    bool history_complete;
    bool source_clean;
    uint8_t validation_profile;
    int32_t height;
    char reason[192];
};

/* A candidate is a complete, immutable progress-store generation built from
 * already-admitted artifact evidence. It is deliberately NOT the active
 * progress.kv, progress_store_open() refuses this schema if manually renamed,
 * and this API has no activation/promotion operation. */
#define CONSENSUS_STATE_CANDIDATE_SCHEMA \
    "zcl.consensus_state_candidate.v1"

enum consensus_state_candidate_failpoint {
    CONSENSUS_CANDIDATE_FAIL_NONE = 0,
    CONSENSUS_CANDIDATE_FAIL_AFTER_STAGING_OPEN,
    CONSENSUS_CANDIDATE_FAIL_AFTER_SCHEMA,
    CONSENSUS_CANDIDATE_FAIL_AFTER_COINS,
    CONSENSUS_CANDIDATE_FAIL_AFTER_ANCHORS,
    CONSENSUS_CANDIDATE_FAIL_AFTER_NULLIFIERS,
    CONSENSUS_CANDIDATE_FAIL_AFTER_PROVENANCE,
    CONSENSUS_CANDIDATE_FAIL_AFTER_COMMIT,
    CONSENSUS_CANDIDATE_FAIL_AFTER_REOPEN,
};

enum consensus_state_candidate_status {
    CONSENSUS_CANDIDATE_REFUSED = 0,
    CONSENSUS_CANDIDATE_INJECTED_FAILURE = 1,
    CONSENSUS_CANDIDATE_OUTPUT_ERROR = 2,
    CONSENSUS_CANDIDATE_VERIFIED_CONTAINED = 3,
};

struct consensus_state_candidate_request {
    /* Borrowed directory capability. output_name must be one normalized path
     * component, must not name the active progress.kv family, and must not
     * already exist. Candidate publication uses no-replace. */
    int output_dir_fd;
    const char *output_name;
    enum consensus_state_candidate_failpoint failpoint;
};

struct consensus_state_candidate_result {
    enum consensus_state_candidate_status status;
    bool source_clean;
    uint8_t validation_profile;
    int32_t height;
    uint64_t utxo_count;
    uint64_t anchor_count;
    uint64_t nullifier_count;
    uint8_t artifact_digest[32];
    uint8_t candidate_file_digest[32];
    char reason[192];
};

/* Opaque evidence over one exact, validated bundle file description. The
 * implementation keeps both the O_NOFOLLOW descriptor and its immutable
 * SQLite read transaction open until free. Callers can inspect a copy of the
 * validated manifest, but cannot manufacture evidence from manifest fields.
 *
 * This is artifact admission evidence only. It is deliberately not selected-
 * chain evidence, lane authority, or publication permission. */
struct zcl_result consensus_state_artifact_evidence_open(
    const char *bundle_path,
    struct consensus_state_artifact_evidence **out);
void consensus_state_artifact_evidence_free(
    struct consensus_state_artifact_evidence *evidence);
bool consensus_state_artifact_evidence_manifest_copy(
    const struct consensus_state_artifact_evidence *evidence,
    struct consensus_state_bundle_manifest *out);
bool consensus_state_artifact_evidence_digest(
    const struct consensus_state_artifact_evidence *evidence,
    uint8_t out[32]);
/* Re-hash the complete pinned SQLite file and recheck its descriptor metadata.
 * Required immediately before and after any long evidence-capture operation. */
bool consensus_state_artifact_evidence_revalidate(
    const struct consensus_state_artifact_evidence *evidence);
/* Domain-separated binding of logical artifact digest, complete-file SHA3,
 * and the exact local file identity admitted by the validator. */
bool consensus_state_artifact_evidence_receipt_digest(
    const struct consensus_state_artifact_evidence *evidence,
    uint8_t out[32]);

/* Stream a history-complete admitted bundle into a separate FULL-durable
 * SQLite progress generation.  The builder closes and independently reopens
 * the staged inode, verifies component parity and reducer conventions, then
 * links the read-only candidate at output_name with no replacement.  Every
 * pre-publication failure removes staging and leaves no final output.  The
 * active progress store is never opened or mutated by this operation. */
bool consensus_state_snapshot_candidate_build(
    const struct consensus_state_artifact_evidence *evidence,
    const struct consensus_state_candidate_request *request,
    struct consensus_state_candidate_result *result);

#ifdef ZCL_TESTING
/* One-shot race hook invoked after immutable validation/digest and immediately
 * before the final name/sidecar recheck. */
void consensus_state_snapshot_candidate_test_set_before_link_hook(
    void (*hook)(void *), void *ctx);
#endif

/* Validate the external zcl.consensus_state_bundle.v1 without publishing it.
 * This deliberately returns VERIFIED_CONTAINED for a valid bundle: the final
 * atomic publisher remains unavailable until proof authority, rollback state,
 * and kill/reopen evidence are durably bound. */
bool consensus_state_snapshot_install(
    struct sqlite3 *progress_db,
    const struct consensus_state_snapshot_install_request *request,
    struct consensus_state_install_result *result);

#endif /* ZCL_CONSENSUS_STATE_SNAPSHOT_INSTALL_H */
