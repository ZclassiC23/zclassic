/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * PURPOSE: Declare the rolling self-verified rewind-base observability helper
 * used by reducer_frontier_dump.c. Split out to keep reducer_frontier_dump.c
 * under the file-size ceiling (E1). */
#ifndef ZCL_JOBS_REDUCER_FRONTIER_REWIND_BASES_H
#define ZCL_JOBS_REDUCER_FRONTIER_REWIND_BASES_H

#include <stdint.h>

struct json_value;

/* Append `rewind_bases` (array) + the nearest-to-tip summary fields onto
 * `out` — see reducer_frontier_rewind_bases.c for the full contract. `hstar`
 * is the caller's already-computed H*. Called with the caller's
 * progress_store_tx_lock already held; every sub-call re-enters it
 * recursively (or takes none), so nesting is safe. Read-only: adds no
 * storage, mutates nothing. */
void reducer_frontier_push_rewind_bases_json(struct json_value *out,
                                             int32_t hstar);

#endif /* ZCL_JOBS_REDUCER_FRONTIER_REWIND_BASES_H */
