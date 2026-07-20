/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * PURPOSE: Declare the rolling self-verified rewind-base observability helper
 * used by reducer_frontier_dump.c, plus the programmatic nearest-self-verified
 * base selector the generic recovery driver (rewind_driver.c) consumes. Split
 * out to keep reducer_frontier_dump.c under the file-size ceiling (E1). */
#ifndef ZCL_JOBS_REDUCER_FRONTIER_REWIND_BASES_H
#define ZCL_JOBS_REDUCER_FRONTIER_REWIND_BASES_H

#include <stdbool.h>
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

/* One resolved rewind base. `kind`/`commitment_sha3` are self-contained copies
 * (no borrowed pointers), so the caller may outlive the enumeration. */
struct reducer_frontier_rewind_base {
    int32_t height;
    bool    self_derived;
    bool    ratified;
    char    kind[32];
    char    commitment_sha3[65];
};

/* Resolve the NEAREST SELF-VERIFIED rewind base at or below `at_or_below`.
 *
 * Sovereignty invariant (the reason this exists): a genuinely self-verified
 * rung (compiled SHA3 checkpoint or a self-valid seal_kv coins_sha3 slot,
 * self_derived=true) is ALWAYS preferred over a borrowed provenance marker
 * (finalized_utxo_sha3, self_derived=false), regardless of height. A borrowed
 * base is returned ONLY as a last resort when no self-verified base exists at
 * or below `at_or_below` — so a borrowed root can never become the rewind
 * target while any self-verified rung is available. Among candidates of the
 * same self_derived class the highest (nearest-to-tip, smallest O(delta))
 * height wins.
 *
 * Returns true and fills `*out` when any base was found; false (out zeroed) if
 * none is at or below `at_or_below`. Read-only: acquires no outer transaction;
 * each sub-read takes its own recursive progress_store lock, so the caller must
 * NOT hold an open progress-store transaction. */
bool reducer_frontier_nearest_self_verified_base(
    int32_t at_or_below, struct reducer_frontier_rewind_base *out);

#endif /* ZCL_JOBS_REDUCER_FRONTIER_REWIND_BASES_H */
