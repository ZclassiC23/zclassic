/* Internal contracts shared by the contained consensus-state exporter. */

#ifndef ZCL_CONSENSUS_STATE_SNAPSHOT_EXPORT_INTERNAL_H
#define ZCL_CONSENSUS_STATE_SNAPSHOT_EXPORT_INTERNAL_H

#include "config/consensus_state_snapshot_export.h"
#include "storage/consensus_state_bundle_codec.h"

#include <sqlite3.h>

#define CONSENSUS_EXPORT_TEMP_PATH_MAX 1152

bool consensus_export_fail(struct consensus_state_export_result *result,
                           enum consensus_state_export_status status,
                           const char *fmt, ...);

bool consensus_export_prove_source(
    sqlite3 *source,
    const struct consensus_state_snapshot_export_request *request,
    struct consensus_state_bundle_manifest *manifest,
    struct consensus_state_source_receipt *receipt,
    struct consensus_state_bundle_proof_summary
        proofs[CONSENSUS_STATE_BUNDLE_PROOF_COUNT],
    struct consensus_state_export_result *result);

bool consensus_export_write_bundle(
    sqlite3 *source, sqlite3 *destination,
    struct consensus_state_bundle_manifest *manifest,
    const struct consensus_state_source_receipt *receipt,
    const struct consensus_state_bundle_proof_summary
        proofs[CONSENSUS_STATE_BUNDLE_PROOF_COUNT],
    struct consensus_state_export_result *result);

bool consensus_export_open_temp(const char *final_path,
                                char temp_path[CONSENSUS_EXPORT_TEMP_PATH_MAX],
                                sqlite3 **destination,
                                struct consensus_state_export_result *result);

bool consensus_export_finalize_temp(
    const char *temp_path, const char *final_path,
    const struct consensus_state_bundle_manifest *manifest,
    struct consensus_state_export_result *result);

#endif /* ZCL_CONSENSUS_STATE_SNAPSHOT_EXPORT_INTERNAL_H */
