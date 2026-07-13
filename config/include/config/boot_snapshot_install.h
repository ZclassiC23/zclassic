/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Interim manual-snapshot crash-convergence journal. */

#ifndef ZCL_BOOT_SNAPSHOT_INSTALL_H
#define ZCL_BOOT_SNAPSHOT_INSTALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct sqlite3;
struct main_state;
struct uss_header;
struct uss_handle;
struct sha3_utxo_checkpoint;

/* This journal does NOT make the multi-store install atomic. It prevents a
 * restart from publishing a partial install while the staged installer is
 * pending. The canonical 64-byte value binds height/count/full payload SHA3. */
#define BOOT_SNAPSHOT_INSTALL_MARKER_KEY \
    "snapshot_install_in_progress.v1"
#define BOOT_SNAPSHOT_INSTALL_MARKER_BYTES 64u

bool boot_snapshot_install_marker_begin(struct sqlite3 *db,
                                        int32_t height, uint64_t count,
                                        const uint8_t payload_sha3[32]);
bool boot_snapshot_install_marker_blocks_resume(
    struct sqlite3 *db, int32_t height, uint64_t count,
    const uint8_t payload_sha3[32], bool *matches);
bool boot_snapshot_install_marker_clear_in_tx(struct sqlite3 *db);
bool boot_snapshot_install_marker_clear(struct sqlite3 *db);
bool boot_snapshot_install_marker_pending(struct sqlite3 *db, bool *pending);

/* If an interrupted install is pending, require `path` to be the exact
 * body-SHA3-verified artifact named by its durable height/count/payload digest.
 * With no pending install this is a successful no-op. */
bool boot_snapshot_install_pending_artifact_matches(
    struct sqlite3 *db, const char *path, bool *pending_out);

bool boot_snapshot_install_resume_allowed(struct sqlite3 *db,
    const struct uss_header *snapshot, int32_t *applied,
    bool *install_pending, bool *marker_matches);

bool boot_snapshot_install_headers_equal(const struct uss_header *a,
                                         const struct uss_header *b);
/* Legacy USS adapter gate: the full payload has already been integrity-checked
 * by uss_open; independently bind its transparent height/hash/root/count/supply
 * to the compiled checkpoint. v2/v3 payload digests are not UTXO roots. */
bool boot_legacy_uss_matches_checkpoint(
    struct uss_handle *snapshot, const struct uss_header *header,
    const struct sha3_utxo_checkpoint *checkpoint,
    char *err, size_t err_size);
void boot_snapshot_install_require_chain_context(
    const struct main_state *main_state);
void boot_snapshot_install_gate_boot(bool progress_open,
                                     const char *loader_path);
/* Fatal if the loader returned while its install journal is still present. */
void boot_snapshot_install_require_complete(void);

#endif /* ZCL_BOOT_SNAPSHOT_INSTALL_H */
