/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_NODE_DB_CATCHUP_INTERNAL_H
#define ZCL_NODE_DB_CATCHUP_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

uint8_t *node_db_catchup_mmap_block_file_quiet(const char *datadir,
                                               int file_num,
                                               size_t *out_size,
                                               int *out_errno);

#endif /* ZCL_NODE_DB_CATCHUP_INTERNAL_H */
