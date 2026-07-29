/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Runtime capability probe for the 4-way AVX-512 Keccak permutation in
 * keccak_x4_internal.h. Both batched SHA3 surfaces (sha3_avx512.c,
 * sha3_256_x4.c) gate on this before dispatching to their AVX-512 lane.
 *
 * CPUID alone is not enough: the OS must also have enabled ZMM state, or the
 * first wide instruction raises #UD. That is what the XCR0 read is for. */

#include "keccak_x4_internal.h"

#if defined(__x86_64__)

#include <cpuid.h>

bool keccak_x4_available(void)
{
    static int cached = -1;
    if (cached >= 0) return cached != 0;

    int ok = 0;
    uint32_t eax, ebx, ecx, edx;
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        bool f  = (ebx >> 16) & 1;  /* AVX512F  */
        bool dq = (ebx >> 17) & 1;  /* AVX512DQ */
        bool vl = (ebx >> 31) & 1;  /* AVX512VL */
        if (f && dq && vl) {
            uint32_t xcr0_lo, xcr0_hi;
            __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
            (void)xcr0_hi;
            /* XCR0 bits 5=opmask, 6=ZMM_Hi256, 7=Hi16_ZMM must all be set. */
            if ((xcr0_lo & 0xE0) == 0xE0)
                ok = 1;
        }
    }
    cached = ok;
    return ok != 0;
}

#else /* non-x86: no AVX-512 lane exists; dispatch always resolves to scalar. */

bool keccak_x4_available(void) { return false; }

#endif
