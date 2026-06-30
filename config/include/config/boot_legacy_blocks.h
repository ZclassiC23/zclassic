/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONFIG_BOOT_LEGACY_BLOCKS_H
#define ZCL_CONFIG_BOOT_LEGACY_BLOCKS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct boot_legacy_block_file_import_result {
    bool source_available;
    bool destination_ready;
    bool truncated_path;
    int failures;
};

struct boot_legacy_block_file_link_result {
    bool source_available;
    bool destination_ready;
    bool truncated_path;
    int linked;
};

/* Import legacy zclassicd blk/rev files into <datadir>/blocks, using hardlinks
 * where possible and copy fallback where hardlinks cannot cross filesystems. */
struct boot_legacy_block_file_import_result
boot_legacy_import_block_files(const char *legacy_blocks_dir,
                               const char *datadir,
                               int max_files);

/* Warm-boot helper: hardlink any missing legacy blk/rev files without copying.
 * This is best-effort and preserves the historical "linked blk count" output. */
struct boot_legacy_block_file_link_result
boot_legacy_link_missing_block_files(const char *legacy_blocks_dir,
                                     const char *datadir,
                                     int max_files);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_CONFIG_BOOT_LEGACY_BLOCKS_H */
