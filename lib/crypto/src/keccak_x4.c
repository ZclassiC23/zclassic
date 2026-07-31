/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Runtime capability probe for the 4-way AVX-512 Keccak permutation in
 * keccak_x4_internal.h. Both batched SHA3 surfaces (sha3_avx512.c,
 * sha3_256_x4.c) gate on this before dispatching to their AVX-512 lane.
 *
 * CPUID alone is not enough: the OS must also have enabled ZMM state, or the
 * first wide instruction raises #UD. The decision lives in the audited
 * predicate crypto/simd_dispatch.h — this file used to check the three ZMM
 * bits directly but executed XGETBV without first confirming OSXSAVE, which is
 * itself a #UD on a host booted with `noxsave`. */

#include "keccak_x4_internal.h"

#if defined(__x86_64__)

#include "crypto/simd_dispatch.h"

bool keccak_x4_available(void)
{
    return simd_host_has_avx512_dq_vl();
}

#else /* non-x86: no AVX-512 lane exists; dispatch always resolves to scalar. */

bool keccak_x4_available(void) { return false; }

#endif
