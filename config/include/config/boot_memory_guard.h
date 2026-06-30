/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONFIG_BOOT_MEMORY_GUARD_H
#define ZCL_CONFIG_BOOT_MEMORY_GUARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOOT_BLOCK_INDEX_DEFAULT_ESTIMATE_COUNT 3000000LL

int64_t boot_block_index_estimate_count(int64_t persisted_height);
size_t boot_block_index_estimate_bytes(int64_t entry_count);
bool boot_block_index_memory_should_warn(size_t estimate_bytes,
                                         size_t system_ram_bytes);
void boot_block_index_memory_warn(int64_t persisted_height);
void boot_block_index_memory_log_loaded(size_t entry_count,
                                        size_t map_capacity);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_CONFIG_BOOT_MEMORY_GUARD_H */
