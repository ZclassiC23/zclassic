/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Single-stream Keccak-f[1600] permutation, AVX-512 implementation.
 *
 * This is the permutation behind the node's SHA3-256/512 hot path (UTXO
 * commitment digests, catalog chained digests). The scalar permutation in
 * sha3.c is the always-available reference; this file adds an AVX-512 variant
 * selected at runtime only when CPUID confirms avx512f+avx512vl+avx512dq. The
 * output is bit-identical to the scalar path (differential parity oracle:
 * test group `keccak_avx512`).
 *
 * Layout: the 25-lane state is held as 5 zmm planes, one plane per Keccak row
 *   Py = ( st[5y+0], st[5y+1], st[5y+2], st[5y+3], st[5y+4], -, -, - )
 * in the low 5 of 8 uint64 slots (top 3 dead, never fed into an active lane).
 *
 *   θ  : column parity C = XOR of the 5 planes (vpternlogq 3-input XOR);
 *        D[x] = C[x-1] ^ rotl(C[x+1],1) via vpermq + vprolq; each plane ^= D.
 *   ρ  : per-lane rotate by the fixed offsets r[col][row] via vprolvq.
 *   π  : A'[x][y] = A[(x+3y)%5][x] — a cross-plane diagonal gather (output lane
 *        x of plane y comes from input plane x), built with masked vpermq + OR.
 *   χ  : new[x] = P[x] ^ (~P[x+1] & P[x+2]) via vpternlogq imm 0xD2.
 *   ι  : XOR the round constant into plane-0 lane 0.
 *
 * The function carries __attribute__((target(...))) so it compiles into the
 * x86-64-v3 baseline binary and is reached only through the runtime dispatch
 * in sha3.c after sha3_keccakf_avx512_available() confirms support. */

#include "crypto/sha3.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(__x86_64__)

#include <immintrin.h>
#include <cpuid.h>

/* Round constants (identical to the scalar keccakf). */
static const uint64_t KECCAK_RNDC[24] = {
    0x0000000000000001, 0x0000000000008082, 0x800000000000808a, 0x8000000080008000,
    0x000000000000808b, 0x0000000080000001, 0x8000000080008081, 0x8000000000008009,
    0x000000000000008a, 0x0000000000000088, 0x0000000080008009, 0x000000008000000a,
    0x000000008000808b, 0x800000000000008b, 0x8000000000008089, 0x8000000000008003,
    0x8000000000008002, 0x8000000000000080, 0x000000000000800a, 0x800000008000000a,
    0x8000000080008081, 0x8000000000008080, 0x0000000080000001, 0x8000000080008008
};

/* ρ rotation offsets, per plane (row y): RHO[y][col] = r[col][y]. */
static const uint64_t KECCAK_RHO[5][8] = {
    { 0,  1, 62, 28, 27, 0, 0, 0 },
    {36, 44,  6, 55, 20, 0, 0, 0 },
    { 3, 10, 43, 25, 39, 0, 0, 0 },
    {41, 45, 15, 21,  8, 0, 0, 0 },
    {18,  2, 61, 56, 14, 0, 0, 0 },
};

__attribute__((target("avx512f,avx512vl,avx512dq")))
void sha3_keccakf_avx512(uint64_t st[25])
{
    const __mmask8 lo5 = 0x1F;

    /* Load the state: one plane (row) per register, lanes in slots 0..4. */
    __m512i P0 = _mm512_maskz_loadu_epi64(lo5, &st[0]);
    __m512i P1 = _mm512_maskz_loadu_epi64(lo5, &st[5]);
    __m512i P2 = _mm512_maskz_loadu_epi64(lo5, &st[10]);
    __m512i P3 = _mm512_maskz_loadu_epi64(lo5, &st[15]);
    __m512i P4 = _mm512_maskz_loadu_epi64(lo5, &st[20]);

    /* ρ rotation-count vectors, one per plane. */
    const __m512i RV0 = _mm512_loadu_si512(KECCAK_RHO[0]);
    const __m512i RV1 = _mm512_loadu_si512(KECCAK_RHO[1]);
    const __m512i RV2 = _mm512_loadu_si512(KECCAK_RHO[2]);
    const __m512i RV3 = _mm512_loadu_si512(KECCAK_RHO[3]);
    const __m512i RV4 = _mm512_loadu_si512(KECCAK_RHO[4]);

    /* θ helper permutes: slot x <- C[(x-1)%5] and C[(x+1)%5]. */
    const __m512i IDX_M1 = _mm512_set_epi64(0, 0, 0, 3, 2, 1, 0, 4);
    const __m512i IDX_P1 = _mm512_set_epi64(0, 0, 0, 0, 4, 3, 2, 1);
    /* χ within-plane rotates: slot x <- P[(x+1)%5] and P[(x+2)%5]. */
    const __m512i IDX_C1 = _mm512_set_epi64(0, 0, 0, 0, 4, 3, 2, 1);
    const __m512i IDX_C2 = _mm512_set_epi64(0, 0, 0, 1, 0, 4, 3, 2);

    /* π gather indices per output row y: idx[x] = (x+3y)%5. */
    const __m512i PI0 = _mm512_set_epi64(0, 0, 0, 4, 3, 2, 1, 0); /* y=0: {0,1,2,3,4} */
    const __m512i PI1 = _mm512_set_epi64(0, 0, 0, 2, 1, 0, 4, 3); /* y=1: {3,4,0,1,2} */
    const __m512i PI2 = _mm512_set_epi64(0, 0, 0, 0, 4, 3, 2, 1); /* y=2: {1,2,3,4,0} */
    const __m512i PI3 = _mm512_set_epi64(0, 0, 0, 3, 2, 1, 0, 4); /* y=3: {4,0,1,2,3} */
    const __m512i PI4 = _mm512_set_epi64(0, 0, 0, 1, 0, 4, 3, 2); /* y=4: {2,3,4,0,1} */

    for (int round = 0; round < 24; ++round) {
        /* ── θ ─────────────────────────────────────────────────────── */
        /* C = P0 ^ P1 ^ P2 ^ P3 ^ P4  (two 3-input XORs). */
        __m512i C = _mm512_ternarylogic_epi64(P0, P1, P2, 0x96);
        C = _mm512_ternarylogic_epi64(C, P3, P4, 0x96);
        __m512i Cm1 = _mm512_permutexvar_epi64(IDX_M1, C);
        __m512i Cp1 = _mm512_permutexvar_epi64(IDX_P1, C);
        __m512i D = _mm512_xor_si512(Cm1, _mm512_rol_epi64(Cp1, 1));
        P0 = _mm512_xor_si512(P0, D);
        P1 = _mm512_xor_si512(P1, D);
        P2 = _mm512_xor_si512(P2, D);
        P3 = _mm512_xor_si512(P3, D);
        P4 = _mm512_xor_si512(P4, D);

        /* ── ρ (in-place per-lane rotate) ──────────────────────────── */
        P0 = _mm512_rolv_epi64(P0, RV0);
        P1 = _mm512_rolv_epi64(P1, RV1);
        P2 = _mm512_rolv_epi64(P2, RV2);
        P3 = _mm512_rolv_epi64(P3, RV3);
        P4 = _mm512_rolv_epi64(P4, RV4);

        /* ── π (diagonal gather: out plane y, lane x <- P_x[(x+3y)%5]) ─ */
        #define PI_ROW(IDX) \
            _mm512_or_si512( \
              _mm512_or_si512( \
                _mm512_or_si512(_mm512_maskz_permutexvar_epi64(0x01, (IDX), P0), \
                                _mm512_maskz_permutexvar_epi64(0x02, (IDX), P1)), \
                _mm512_or_si512(_mm512_maskz_permutexvar_epi64(0x04, (IDX), P2), \
                                _mm512_maskz_permutexvar_epi64(0x08, (IDX), P3))), \
              _mm512_maskz_permutexvar_epi64(0x10, (IDX), P4))
        __m512i Q0 = PI_ROW(PI0);
        __m512i Q1 = PI_ROW(PI1);
        __m512i Q2 = PI_ROW(PI2);
        __m512i Q3 = PI_ROW(PI3);
        __m512i Q4 = PI_ROW(PI4);
        #undef PI_ROW

        /* ── χ (within-plane: new[x] = Q[x] ^ (~Q[x+1] & Q[x+2])) ───── */
        P0 = _mm512_ternarylogic_epi64(Q0, _mm512_permutexvar_epi64(IDX_C1, Q0),
                                       _mm512_permutexvar_epi64(IDX_C2, Q0), 0xD2);
        P1 = _mm512_ternarylogic_epi64(Q1, _mm512_permutexvar_epi64(IDX_C1, Q1),
                                       _mm512_permutexvar_epi64(IDX_C2, Q1), 0xD2);
        P2 = _mm512_ternarylogic_epi64(Q2, _mm512_permutexvar_epi64(IDX_C1, Q2),
                                       _mm512_permutexvar_epi64(IDX_C2, Q2), 0xD2);
        P3 = _mm512_ternarylogic_epi64(Q3, _mm512_permutexvar_epi64(IDX_C1, Q3),
                                       _mm512_permutexvar_epi64(IDX_C2, Q3), 0xD2);
        P4 = _mm512_ternarylogic_epi64(Q4, _mm512_permutexvar_epi64(IDX_C1, Q4),
                                       _mm512_permutexvar_epi64(IDX_C2, Q4), 0xD2);

        /* ── ι (round constant into plane-0 lane 0) ────────────────── */
        P0 = _mm512_xor_si512(P0,
                 _mm512_maskz_set1_epi64(0x01, (long long)KECCAK_RNDC[round]));
    }

    _mm512_mask_storeu_epi64(&st[0],  lo5, P0);
    _mm512_mask_storeu_epi64(&st[5],  lo5, P1);
    _mm512_mask_storeu_epi64(&st[10], lo5, P2);
    _mm512_mask_storeu_epi64(&st[15], lo5, P3);
    _mm512_mask_storeu_epi64(&st[20], lo5, P4);
}

/* Runtime capability probe: require avx512f + avx512vl + avx512dq AND OS
 * ZMM state save (XCR0 opmask/hi256/hi16). Detected once; cached. */
bool sha3_keccakf_avx512_available(void)
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

#else /* non-x86: AVX-512 unavailable, dispatch always resolves to scalar. */

void sha3_keccakf_avx512(uint64_t st[25]) { sha3_keccakf_scalar(st); }
bool sha3_keccakf_avx512_available(void) { return false; }

#endif
