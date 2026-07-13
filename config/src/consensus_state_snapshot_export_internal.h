/* Internal contracts shared by the contained consensus-state exporter. */

#ifndef ZCL_CONSENSUS_STATE_SNAPSHOT_EXPORT_INTERNAL_H
#define ZCL_CONSENSUS_STATE_SNAPSHOT_EXPORT_INTERNAL_H

#include "config/consensus_state_snapshot_export.h"
#include "storage/consensus_state_bundle_codec.h"

#include <sqlite3.h>
#include <sys/types.h>

#define CONSENSUS_EXPORT_NAME_MAX 256

struct consensus_export_output_binding {
    int dirfd;
    int temp_fd;
    char final_name[CONSENSUS_EXPORT_NAME_MAX];
    dev_t temp_dev;
    ino_t temp_ino;
    sqlite3_vfs vfs;
    sqlite3_vfs *base_vfs;
    char vfs_name[64];
    bool vfs_registered;
    bool abandon_on_close;
};

/* Private SQLite file object that performs all database I/O through the
 * retained anonymous staging inode. */
int consensus_export_fd_file_size(void);
int consensus_export_fd_file_open(sqlite3_file *file, int retained_fd,
                                  int flags, int *out_flags);

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

bool consensus_export_open_temp(struct consensus_export_output_binding *output,
                                sqlite3 **destination,
                                struct consensus_state_export_result *result);

bool consensus_export_finalize_temp(
    struct consensus_export_output_binding *output,
    const struct consensus_state_bundle_manifest *manifest,
    struct consensus_state_export_result *result);

#endif /* ZCL_CONSENSUS_STATE_SNAPSHOT_EXPORT_INTERNAL_H */
