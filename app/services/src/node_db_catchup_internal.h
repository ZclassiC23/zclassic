/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_NODE_DB_CATCHUP_INTERNAL_H
#define ZCL_NODE_DB_CATCHUP_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint8_t *node_db_catchup_mmap_block_file_quiet(const char *datadir,
                                               int file_num,
                                               size_t *out_size,
                                               int *out_errno);

/* Sparse-prefix projection-cursor classifier. Defined in
 * node_db_catchup_sparse.c; see its doc comment for the contract. */
int node_db_catchup_sparse_prefix_target(int indexed,
                                         int total,
                                         int lean_holes,
                                         int first_hole_h,
                                         int start,
                                         int chain_tip,
                                         int suspicious_holes,
                                         int missing_index_holes,
                                         int first_missing_index_h,
                                         bool proven_authority,
                                         int32_t proven_applied);

#endif /* ZCL_NODE_DB_CATCHUP_INTERNAL_H */
