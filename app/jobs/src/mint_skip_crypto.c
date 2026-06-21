/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * mint_skip_crypto — see jobs/mint_skip_crypto.h. Modeled exactly on
 * mint_fold_ceiling: a single _Atomic, default OFF, set ONLY by the mint
 * driver. */

#include "jobs/mint_skip_crypto.h"

#include <stdatomic.h>

/* Default OFF: a normal boot never calls _set, so both crypto step bodies see
 * `mint_skip_crypto_get() == false` and run the REAL per-block crypto —
 * byte-identical to today. Only the -mint-anchor fast driver flips it true. */
static _Atomic bool g_mint_skip_crypto = false;

void mint_skip_crypto_set(bool skip)
{
    atomic_store_explicit(&g_mint_skip_crypto, skip, memory_order_relaxed);
}

bool mint_skip_crypto_get(void)
{
    return atomic_load_explicit(&g_mint_skip_crypto, memory_order_relaxed);
}
