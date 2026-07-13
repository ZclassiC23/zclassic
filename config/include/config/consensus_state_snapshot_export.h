/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Full-history consensus-state bundle exporter. */

#ifndef ZCL_CONSENSUS_STATE_SNAPSHOT_EXPORT_H
#define ZCL_CONSENSUS_STATE_SNAPSHOT_EXPORT_H

#include <stdbool.h>
#include <stdint.h>

struct sqlite3;

enum consensus_state_export_status {
    CONSENSUS_EXPORT_REFUSED = 0,
    CONSENSUS_EXPORT_MISSING_PROOF = 1,
    CONSENSUS_EXPORT_STORE_ERROR = 2,
    CONSENSUS_EXPORT_OUTPUT_ERROR = 3,
    CONSENSUS_EXPORT_EXPORTED = 4,
};

struct consensus_state_snapshot_export_request {
    /* Borrowed directory descriptor plus one-component final name. The
     * exporter duplicates the descriptor, builds through an anonymous
     * O_TMPFILE and an fd-only SQLite VFS, then atomically links that exact
     * validated inode into the pinned directory. The caller retains ownership
     * of output_dir_fd. Existing names, symlinks, slash-containing names, dot,
     * and dot-dot are refused. */
    int output_dir_fd;
    const char *output_name;
    /* Assertions against the frozen reducer view. They prevent a caller from
     * exporting the wrong quiesced generation, but are not chain authority. */
    int32_t expected_height;
    uint8_t expected_block_hash[32];
};

struct consensus_state_export_result {
    enum consensus_state_export_status status;
    bool history_complete;
    int32_t height;
    uint64_t utxo_count;
    uint64_t anchor_count;
    uint64_t nullifier_count;
    uint8_t artifact_digest[32];
    char reason[192];
};

/* Export one zcl.consensus_state_bundle.v1 from an already-quiesced
 * progress.kv handle. This function additionally owns progress_store_tx_lock
 * and one read transaction while deriving and copying the source generation.
 *
 * Success is deliberately narrow: durable coins only, exact H-star/tip generation,
 * convention-correct stage cursors, explicit genesis anchor/nullifier coverage,
 * and complete successful reducer proof rows are all required. Missing proof
 * returns MISSING_PROOF and leaves
 * no final artifact. On success the file has been independently reopened and
 * validated, fsynced, chmod 0400, atomically linked without replacement, and
 * its directory
 * fsynced. It does not mutate or publish node state. */
bool consensus_state_snapshot_export(
    struct sqlite3 *progress_db,
    const struct consensus_state_snapshot_export_request *request,
    struct consensus_state_export_result *result);

#ifdef ZCL_TESTING
/* Deterministic adversarial hooks around directory binding and anonymous
 * staging-inode creation. */
void consensus_state_snapshot_export_test_set_after_output_bind_hook(
    void (*hook)(void *), void *ctx);
void consensus_state_snapshot_export_test_set_after_staging_create_hook(
    void (*hook)(void *), void *ctx);
#endif

#endif /* ZCL_CONSENSUS_STATE_SNAPSHOT_EXPORT_H */
