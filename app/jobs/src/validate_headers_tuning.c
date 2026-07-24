/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * validate_headers_tuning — runtime pool-width / forward-batch sizing for the
 * validate_headers reducer stage. Sibling-private (validate_headers_internal.h).
 *
 * Equihash PoW verification is the per-header cost of the stage and it is a
 * PURE function of the header (nonce, solution, nBits) — no coins/UTXO/anchor
 * state. A WIDER pool and a LARGER forward batch therefore verify more heights
 * concurrently with byte-identical per-height verdicts: every
 * validate_headers_log row is still written from its own independent validator
 * result, and the width only changes how many run at once, never WHAT is
 * checked nor WHICH row is written. Cursor semantics, the failure-recheck
 * floor, and the authoritative mark are untouched.
 *
 * SAFETY — gated exactly like refold_cadence (jobs/refold_cadence.h): the
 * override applies ONLY while refold_cadence_active() (a `-mint-anchor` fold
 * ceiling is set, or a `-refold-*` fold is in progress). On a NORMAL live node
 * both accessors return the compile-time VH_POOL_SIZE / VH_BATCH_SIZE
 * regardless of the environment, so the live hot path is byte-for-byte
 * unchanged and no environment variable can widen it. Pinned by
 * test_validate_headers_tuning.
 *
 * TUNABLE (only while a fold is active):
 *   ZCL_VH_POOL   Equihash worker threads   default 16  clamp [1,128]
 *   ZCL_VH_BATCH  heights per forward step  default 256 clamp [1,4096]
 */

#include "validate_headers_internal.h"

#include "jobs/refold_cadence.h"
#include "jobs/validate_headers_stage.h"

#include <stdlib.h>

static int vh_env_clamped(const char *name, int def, int lo, int hi)
{
    const char *v = getenv(name);
    if (!v || !v[0])
        return def;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v)
        return def;
    if (n < lo) n = lo;
    if (n > hi) n = hi;
    return (int)n;
}

int vh_runtime_pool_size(void)
{
    if (!refold_cadence_active())
        return VH_POOL_SIZE;
    return vh_env_clamped("ZCL_VH_POOL", VH_FOLD_POOL_DEFAULT, 1, VH_MAX_POOL);
}

int vh_runtime_batch_size(void)
{
    if (!refold_cadence_active())
        return VH_BATCH_SIZE;
    return vh_env_clamped("ZCL_VH_BATCH", VH_FOLD_BATCH_DEFAULT, 1,
                          VH_MAX_BATCH);
}
