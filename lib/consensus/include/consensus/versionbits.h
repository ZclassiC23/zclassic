/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * versionbits — miner-signaled Equihash parameter activation.
 *
 * BIP8-flavored, deterministic from headers alone. Blocks signal by
 * setting bit ehUpgrade.nSignalBit of nVersion. Heights are grouped
 * into fixed nWindow-block windows aligned to multiples of nWindow;
 * a window PASSES when at least nThreshold of its blocks signal.
 * After nConsecutiveWindows passing windows in a row (a failing
 * window resets the streak) the deployment is LOCKED_IN at that
 * window boundary, and the upgraded parameters become mandatory
 * nGraceBlocks later (ACTIVE). No expiry.
 *
 * Evaluation walks pprev within each window and caches the per-window
 * verdict keyed by the BOUNDARY BLOCK HASH, so two competing branches
 * across a reorg get independent, correct states with no cache
 * invalidation. All state derives from block_index entries already in
 * RAM — no disk reads, no clocks. */

#ifndef ZCL_CONSENSUS_VERSIONBITS_H
#define ZCL_CONSENSUS_VERSIONBITS_H

#include "consensus/params.h"
#include "util/result.h"
#include <stdbool.h>
#include <stdint.h>

struct block_index;

enum vbits_state {
    VBITS_DISABLED = 0,  /* deployment off in chainparams */
    VBITS_DEFINED,       /* signaling open, streak below target */
    VBITS_LOCKED_IN,     /* streak reached; waiting out the grace */
    VBITS_ACTIVE,        /* upgraded parameters are mandatory */
};

struct vbits_info {
    enum vbits_state state;     /* as of the block AFTER pindex_prev */
    int     streak;             /* consecutive passing windows so far */
    int     last_boundary_height;  /* top of the last full window, -1 if none */
    int     locked_in_height;   /* boundary where lock-in occurred, -1 */
    int     active_height;      /* locked_in + grace, -1 until locked */
    int64_t window_signal_count;   /* signals in the current PARTIAL window */
};

/* Evaluate the deployment state for the block that would follow
 * `pindex_prev`. Fails (with reason) when the ancestry needed to score
 * a window is incomplete — e.g. a sparse fast-sync tail; callers fall
 * back to the static epoch table in that case. */
struct zcl_result versionbits_eh_query(const struct consensus_params *p,
                                       const struct block_index *pindex_prev,
                                       struct vbits_info *out);

/* True when the deployment is ACTIVE for the block after pindex_prev;
 * stores the activation height in *active_height_out (or -1). Returns
 * false on disabled, not-yet-active, or unevaluable ancestry. */
bool versionbits_eh_active(const struct consensus_params *p,
                           const struct block_index *pindex_prev,
                           int *active_height_out);

/* Drop every cached window verdict. For tests and post-reorg paranoia;
 * correctness never requires it (cache keys are boundary block hashes). */
void versionbits_cache_reset(void);

/* Stable lowercase name for a state ("disabled", "defined",
 * "locked_in", "active") — for RPC / zcl_state output. */
const char *vbits_state_name(enum vbits_state s);

#endif /* ZCL_CONSENSUS_VERSIONBITS_H */
