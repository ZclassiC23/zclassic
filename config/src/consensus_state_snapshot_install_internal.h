/* Internal contracts for the contained consensus-state candidate builder. */

#ifndef ZCL_CONSENSUS_STATE_SNAPSHOT_INSTALL_INTERNAL_H
#define ZCL_CONSENSUS_STATE_SNAPSHOT_INSTALL_INTERNAL_H

#include "config/consensus_state_snapshot_install.h"
#include "consensus_state_snapshot_export_internal.h"

#include <sqlite3.h>

struct consensus_state_candidate_output {
    struct consensus_export_output_binding binding;
};

/* Serialized immutable read-transaction lease.  A successful begin holds the
 * evidence mutex until end, so one NOMUTEX SQLite handle is never shared by
 * concurrent candidate builders. */
bool consensus_state_artifact_evidence_candidate_lease_begin(
    const struct consensus_state_artifact_evidence *evidence,
    struct consensus_state_bundle_manifest *manifest,
    uint8_t receipt_digest[32], sqlite3 **db);
void consensus_state_artifact_evidence_candidate_lease_end(
    const struct consensus_state_artifact_evidence *evidence);

bool consensus_state_candidate_validate_reopened(
    sqlite3 *candidate,
    const struct consensus_state_bundle_manifest *expected,
    const uint8_t expected_admission_receipt[32],
    struct consensus_state_candidate_result *result);

bool consensus_state_candidate_fail(
    struct consensus_state_candidate_result *result,
    enum consensus_state_candidate_status status,
    const char *fmt, ...);

bool consensus_state_candidate_output_open(
    const struct consensus_state_candidate_request *request,
    struct consensus_state_candidate_output *output,
    struct consensus_state_candidate_result *result);
void consensus_state_candidate_output_cleanup(
    struct consensus_state_candidate_output *output);
bool consensus_state_candidate_output_sqlite_open(
    struct consensus_state_candidate_output *output, sqlite3 **db);
bool consensus_state_candidate_sqlite_close_strict(
    struct consensus_state_candidate_output *output, sqlite3 **db);
bool consensus_state_candidate_output_finalize(
    struct consensus_state_candidate_output *output,
    const struct consensus_state_bundle_manifest *manifest,
    const uint8_t admission_receipt[32],
    enum consensus_state_candidate_failpoint failpoint,
    struct consensus_state_candidate_result *result);

#endif /* ZCL_CONSENSUS_STATE_SNAPSHOT_INSTALL_INTERNAL_H */
