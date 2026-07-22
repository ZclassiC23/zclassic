/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Bounded, monotonic reducer hot-stage measurements.  This is observability
 * only: writers add elapsed/work values, diagnostics take read-only snapshots.
 */
#ifndef ZCL_UTIL_REDUCER_STAGE_PROFILE_H
#define ZCL_UTIL_REDUCER_STAGE_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

struct json_value;

enum reducer_profile_domain {
    REDUCER_PROFILE_BODY_PERSIST = 0,
    REDUCER_PROFILE_SCRIPT_VALIDATE,
    REDUCER_PROFILE_DOMAIN_COUNT
};

enum reducer_profile_field {
    RPF_BLOCKS = 0,
    RPF_TOTAL_US,
    RPF_UPSTREAM_US,
    RPF_CACHE_HITS,
    RPF_CACHE_MISSES,
    RPF_CACHE_PROBES,
    RPF_CACHE_LOCK_WAIT_US,
    RPF_DISK_READ_US,
    RPF_PARSE_US,
    RPF_DEEP_CLONES,
    RPF_DEEP_CLONE_BYTES,
    RPF_BLOCK_HASH_US,
    RPF_MERKLE_US,
    RPF_MERKLE_ALLOCS,
    RPF_MERKLE_BYTES,
    RPF_EVENT_ENCODE_US,
    RPF_EVENT_APPEND_US,
    RPF_CREATED_INDEX_BLOCKS,
    RPF_CREATED_INDEX_TXS,
    RPF_CREATED_INDEX_OUTPUTS,
    RPF_CREATED_INDEX_PREPARES,
    RPF_CREATED_INDEX_STEPS,
    RPF_CREATED_INDEX_US,
    RPF_CONTEXTUAL_US,
    RPF_TX_PRECOMPUTE_US,
    RPF_JOB_ARRAY_ALLOCS,
    RPF_JOB_ARRAY_BYTES,
    RPF_PREVOUT_CREATED_LOOKUPS,
    RPF_PREVOUT_COINS_FALLBACKS,
    RPF_PREVOUT_PREPARES,
    RPF_PREVOUT_HITS,
    RPF_PREVOUT_MISSES,
    RPF_PREVOUT_US,
    RPF_POOL_SETUP_US,
    RPF_POOL_WAKE_US,
    RPF_VERIFY_SCRIPT_CPU_US,
    RPF_WORKER_WAIT_US,
    RPF_ORDERED_REDUCTION_US,
    RPF_HEADER_EVENT_US,
    RPF_STAGE_LOG_CURSOR_US,
    RPF_FIELD_COUNT
};

void reducer_stage_profile_add(enum reducer_profile_domain domain,
                               enum reducer_profile_field field,
                               uint64_t value);
/* Add a duration and one sample to the bounded log2 histogram used for p50/p95
 * diagnostics. Use this rather than `_add` for elapsed-time fields. */
void reducer_stage_profile_observe_us(enum reducer_profile_domain domain,
                                      enum reducer_profile_field field,
                                      uint64_t value);
void reducer_stage_profile_reset(void);
bool reducer_stage_profile_dump_state_json(struct json_value *out,
                                           const char *key);

#endif /* ZCL_UTIL_REDUCER_STAGE_PROFILE_H */
