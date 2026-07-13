/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * PURPOSE: Declare exact hash-evidence checks used by the reducer frontier. */
#ifndef ZCL_JOBS_REDUCER_FRONTIER_EVIDENCE_H
#define ZCL_JOBS_REDUCER_FRONTIER_EVIDENCE_H

#include <stdbool.h>
#include <stdint.h>

struct sqlite3;

bool reducer_frontier_apply_hash_agreement(struct sqlite3 *db,
                                           int32_t anchor,
                                           int32_t *hstar);

#endif /* ZCL_JOBS_REDUCER_FRONTIER_EVIDENCE_H */
