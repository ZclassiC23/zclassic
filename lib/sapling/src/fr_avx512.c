/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Hardware-accelerated field arithmetic for BLS12-381 Fr and Fp.
 *
 * Runtime CPUID detection with graceful degradation:
 *   Tier 1: AVX-512 IFMA — VPMADD52 batch multiply (8 Fr muls at once)
 *   Tier 2: BMI2+ADX — MULX+ADCX+ADOX carry chains
 *   Tier 3: Portable — __int128 fallback (always available)
 *
 * Detection runs once at first use. Binary works on any x86-64 CPU. */

#include "sapling/fr.h"
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <immintrin.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

/* ── CPUID detection ─────────────────────────────────────────────── */

static bool cpu_has_bmi2 = false;
static bool cpu_has_adx = false;
static bool cpu_has_avx512ifma = false;
static bool cpu_detected = false;

static void detect_cpu_features(void)
{
    if (cpu_detected) return;

    uint32_t eax, ebx, ecx, edx;

    /* CPUID leaf 7, subleaf 0: structured extended features */
    __asm__ volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(7), "c"(0));

    cpu_has_bmi2 = (ebx >> 8) & 1;       /* EBX bit 8 */
    cpu_has_adx = (ebx >> 19) & 1;        /* EBX bit 19 */
    cpu_has_avx512ifma = (ebx >> 21) & 1; /* EBX bit 21 (AVX512_IFMA) */

    /* Also check that AVX-512 foundation (F) is present */
    bool has_avx512f = (ebx >> 16) & 1;   /* EBX bit 16 */
    if (!has_avx512f) cpu_has_avx512ifma = false;

    /* Check OS support for AVX-512 (XGETBV: XCR0 bits 5,6,7 must be set) */
    if (cpu_has_avx512ifma) {
        uint32_t xcr0_lo, xcr0_hi;
        __asm__ volatile("xgetbv"
            : "=a"(xcr0_lo), "=d"(xcr0_hi)
            : "c"(0));
        /* bits 5=opmask, 6=ZMM_hi256, 7=Hi16_ZMM */
        if ((xcr0_lo & 0xE0) != 0xE0)
            cpu_has_avx512ifma = false;
    }

    cpu_detected = true;
}

const char *fr_accel_implementation(void)
{
    detect_cpu_features();
    if (cpu_has_avx512ifma)
        return "AVX-512 IFMA (VPMADD52 + MULX+ADCX+ADOX)";
    if (cpu_has_bmi2 && cpu_has_adx)
        return "BMI2+ADX (MULX+ADCX+ADOX)";
    return "portable (__int128)";
}

/* 52-bit radix mask for AVX-512 IFMA */
#define RADIX52_MASK 0x000FFFFFFFFFFFFFULL

/* ================================================================
 * Tier 2: Fr Montgomery multiply — BMI2+ADX (MULX+ADCX+ADOX)
 *
 * Compiled unconditionally with target attribute so the binary
 * runs on any CPU. The function is only called after CPUID confirms
 * BMI2+ADX support.
 * ================================================================ */

__attribute__((target("bmi2,adx")))
static void fr_mont_mul_bmi2(uint64_t r[4], const uint64_t a[4], const uint64_t b[4])
{
#if defined(__x86_64__) || defined(_M_X64)
    /* Include intrinsics header for target-specific functions */
    static const uint64_t P[4] = {
        0xffffffff00000001ULL, 0x53bda402fffe5bfeULL,
        0x3339d80809a1d805ULL, 0x73eda753299d7d48ULL
    };
    static const uint64_t INV = 0xfffffffeffffffffULL;

    uint64_t t[5] = {0, 0, 0, 0, 0};

    for (int i = 0; i < 4; i++) {
        unsigned long long hi;
        unsigned char cf;
        uint64_t carry_mul;

        /* t += a * b[i] */
        uint64_t lo0 = _mulx_u64(a[0], b[i], &hi);
        cf = _addcarryx_u64(0, t[0], lo0, (unsigned long long *)&t[0]);
        carry_mul = hi;

        uint64_t lo1 = _mulx_u64(a[1], b[i], &hi);
        cf = _addcarryx_u64(cf, t[1], lo1, (unsigned long long *)&t[1]);
        unsigned char cf2 = _addcarryx_u64(0, t[1], carry_mul, (unsigned long long *)&t[1]);
        carry_mul = hi + cf2;

        uint64_t lo2 = _mulx_u64(a[2], b[i], &hi);
        cf = _addcarryx_u64(cf, t[2], lo2, (unsigned long long *)&t[2]);
        cf2 = _addcarryx_u64(0, t[2], carry_mul, (unsigned long long *)&t[2]);
        carry_mul = hi + cf2;

        uint64_t lo3 = _mulx_u64(a[3], b[i], &hi);
        cf = _addcarryx_u64(cf, t[3], lo3, (unsigned long long *)&t[3]);
        cf2 = _addcarryx_u64(0, t[3], carry_mul, (unsigned long long *)&t[3]);
        carry_mul = hi + cf2;

        t[4] = carry_mul + cf;

        /* Montgomery reduction */
        uint64_t m = t[0] * INV;

        lo0 = _mulx_u64(m, P[0], &hi);
        cf = _addcarryx_u64(0, t[0], lo0, (unsigned long long *)&t[0]);
        carry_mul = hi + cf;

        lo1 = _mulx_u64(m, P[1], &hi);
        cf = _addcarryx_u64(0, t[1], lo1, (unsigned long long *)&t[1]);
        cf2 = _addcarryx_u64(0, t[1], carry_mul, (unsigned long long *)&t[0]);
        carry_mul = hi + cf + cf2;

        lo2 = _mulx_u64(m, P[2], &hi);
        cf = _addcarryx_u64(0, t[2], lo2, (unsigned long long *)&t[2]);
        cf2 = _addcarryx_u64(0, t[2], carry_mul, (unsigned long long *)&t[1]);
        carry_mul = hi + cf + cf2;

        lo3 = _mulx_u64(m, P[3], &hi);
        cf = _addcarryx_u64(0, t[3], lo3, (unsigned long long *)&t[3]);
        cf2 = _addcarryx_u64(0, t[3], carry_mul, (unsigned long long *)&t[2]);
        carry_mul = hi + cf + cf2;

        t[3] = t[4] + carry_mul;
        t[4] = 0;
    }

    /* Final reduction */
    bool ge = t[4] != 0;
    if (!ge) {
        for (int i = 3; i >= 0; i--) {
            if (t[i] > P[i]) { ge = true; break; }
            if (t[i] < P[i]) break;
            if (i == 0) ge = true;
        }
    }

    if (ge) {
        unsigned char borrow = 0;
        for (int i = 0; i < 4; i++)
            borrow = _subborrow_u64(borrow, t[i], P[i], (unsigned long long *)&r[i]);
    } else {
        memcpy(r, t, 32);
    }
#else
    (void)r; (void)a; (void)b;
#endif
}

/* ================================================================
 * Tier 2: Fp Montgomery multiply — BMI2+ADX
 * ================================================================ */

__attribute__((target("bmi2,adx")))
static void fp_mont_mul_bmi2(uint64_t r[6], const uint64_t a[6], const uint64_t b[6])
{
#if defined(__x86_64__) || defined(_M_X64)
    #include <immintrin.h>

    static const uint64_t Q[6] = {
        0xb9feffffffffaaabULL, 0x1eabfffeb153ffffULL,
        0x6730d2a0f6b0f624ULL, 0x64774b84f38512bfULL,
        0x4b1ba7b6434bacd7ULL, 0x1a0111ea397fe69aULL
    };
    static const uint64_t INV = 0x89f3fffcfffcfffdULL;

    uint64_t t[7] = {0};

    for (int i = 0; i < 6; i++) {
        unsigned long long hi;
        unsigned char cf;
        uint64_t carry_mul = 0;

        for (int j = 0; j < 6; j++) {
            uint64_t lo = _mulx_u64(a[j], b[i], &hi);
            cf = _addcarryx_u64(0, t[j], lo, (unsigned long long *)&t[j]);
            unsigned char cf2 = _addcarryx_u64(0, t[j], carry_mul,
                                                (unsigned long long *)&t[j]);
            carry_mul = hi + cf + cf2;
        }
        t[6] = carry_mul;

        uint64_t m = t[0] * INV;
        carry_mul = 0;

        uint64_t lo0 = _mulx_u64(m, Q[0], &hi);
        cf = _addcarryx_u64(0, t[0], lo0, (unsigned long long *)&t[0]);
        carry_mul = hi + cf;

        for (int j = 1; j < 6; j++) {
            uint64_t lo = _mulx_u64(m, Q[j], &hi);
            cf = _addcarryx_u64(0, t[j], lo, (unsigned long long *)&t[j]);
            unsigned char cf2 = _addcarryx_u64(0, t[j], carry_mul,
                                                (unsigned long long *)&t[j - 1]);
            carry_mul = hi + cf + cf2;
        }
        t[5] = t[6] + carry_mul;
        t[6] = 0;
    }

    bool ge = t[6] != 0;
    if (!ge) {
        for (int i = 5; i >= 0; i--) {
            if (t[i] > Q[i]) { ge = true; break; }
            if (t[i] < Q[i]) break;
            if (i == 0) ge = true;
        }
    }

    if (ge) {
        unsigned char borrow = 0;
        for (int i = 0; i < 6; i++)
            borrow = _subborrow_u64(borrow, t[i], Q[i], (unsigned long long *)&r[i]);
    } else {
        memcpy(r, t, 48);
    }
#else
    (void)r; (void)a; (void)b;
#endif
}

/* ================================================================
 * Tier 3: Portable Montgomery multiply (__int128)
 * Always compiled. Used as fallback on older CPUs.
 * ================================================================ */

static const uint64_t FR_P_PORT[4] = {
    0xffffffff00000001ULL, 0x53bda402fffe5bfeULL,
    0x3339d80809a1d805ULL, 0x73eda753299d7d48ULL
};
static const uint64_t FR_INV_PORT = 0xfffffffeffffffffULL;

static bool port_gte4(const uint64_t a[4], const uint64_t b[4])
{
    for (int i = 3; i >= 0; i--) {
        if (a[i] > b[i]) return true;
        if (a[i] < b[i]) return false;
    }
    return true;
}

static void port_sub4(uint64_t r[4], const uint64_t a[4], const uint64_t b[4])
{
    unsigned __int128 borrow = 0;
    for (int i = 0; i < 4; i++) {
        unsigned __int128 tmp = (unsigned __int128)a[i] - b[i] - borrow;
        r[i] = (uint64_t)tmp;
        borrow = (tmp >> 127) & 1;
    }
}

static void fr_mont_mul_portable(uint64_t r[4], const uint64_t a[4], const uint64_t b[4])
{
    uint64_t t[5] = {0};
    for (int i = 0; i < 4; i++) {
        unsigned __int128 carry = 0;
        for (int j = 0; j < 4; j++) {
            unsigned __int128 prod = (unsigned __int128)a[j] * b[i] + t[j] + carry;
            t[j] = (uint64_t)prod;
            carry = prod >> 64;
        }
        t[4] = (uint64_t)carry;

        uint64_t m = t[0] * FR_INV_PORT;
        carry = 0;
        unsigned __int128 prod0 = (unsigned __int128)m * FR_P_PORT[0] + t[0];
        carry = prod0 >> 64;
        for (int j = 1; j < 4; j++) {
            unsigned __int128 prod = (unsigned __int128)m * FR_P_PORT[j] + t[j] + carry;
            t[j - 1] = (uint64_t)prod;
            carry = prod >> 64;
        }
        unsigned __int128 sum = (unsigned __int128)t[4] + carry;
        t[3] = (uint64_t)sum;
        t[4] = (uint64_t)(sum >> 64);
    }
    if (t[4] || port_gte4(t, FR_P_PORT))
        port_sub4(r, t, FR_P_PORT);
    else
        memcpy(r, t, 32);
}

static const uint64_t FP_Q_PORT[6] = {
    0xb9feffffffffaaabULL, 0x1eabfffeb153ffffULL,
    0x6730d2a0f6b0f624ULL, 0x64774b84f38512bfULL,
    0x4b1ba7b6434bacd7ULL, 0x1a0111ea397fe69aULL
};
static const uint64_t FP_INV_PORT = 0x89f3fffcfffcfffdULL;

static bool port_gte6(const uint64_t a[6], const uint64_t b[6])
{
    for (int i = 5; i >= 0; i--) {
        if (a[i] > b[i]) return true;
        if (a[i] < b[i]) return false;
    }
    return true;
}

static void port_sub6(uint64_t r[6], const uint64_t a[6], const uint64_t b[6])
{
    unsigned __int128 borrow = 0;
    for (int i = 0; i < 6; i++) {
        unsigned __int128 tmp = (unsigned __int128)a[i] - b[i] - borrow;
        r[i] = (uint64_t)tmp;
        borrow = (tmp >> 127) & 1;
    }
}

static void fp_mont_mul_portable(uint64_t r[6], const uint64_t a[6], const uint64_t b[6])
{
    uint64_t t[7] = {0};
    for (int i = 0; i < 6; i++) {
        unsigned __int128 carry = 0;
        for (int j = 0; j < 6; j++) {
            unsigned __int128 prod = (unsigned __int128)a[j] * b[i] + t[j] + carry;
            t[j] = (uint64_t)prod;
            carry = prod >> 64;
        }
        t[6] = (uint64_t)carry;

        uint64_t m = t[0] * FP_INV_PORT;
        carry = 0;
        unsigned __int128 prod0 = (unsigned __int128)m * FP_Q_PORT[0] + t[0];
        carry = prod0 >> 64;
        for (int j = 1; j < 6; j++) {
            unsigned __int128 prod = (unsigned __int128)m * FP_Q_PORT[j] + t[j] + carry;
            t[j - 1] = (uint64_t)prod;
            carry = prod >> 64;
        }
        unsigned __int128 sum = (unsigned __int128)t[6] + carry;
        t[5] = (uint64_t)sum;
        t[6] = (uint64_t)(sum >> 64);
    }
    if (t[6] || port_gte6(t, FP_Q_PORT))
        port_sub6(r, t, FP_Q_PORT);
    else
        memcpy(r, t, 48);
}

/* ================================================================
 * Runtime dispatch — function pointers, set once at first call
 * ================================================================ */

typedef void (*fr_mul_fn)(uint64_t r[4], const uint64_t a[4], const uint64_t b[4]);
typedef void (*fp_mul_fn)(uint64_t r[6], const uint64_t a[6], const uint64_t b[6]);

static fr_mul_fn g_fr_mont_mul = NULL;
static fp_mul_fn g_fp_mont_mul = NULL;

static void init_dispatch(void)
{
    detect_cpu_features();
#if defined(__x86_64__) || defined(_M_X64)
    if (cpu_has_bmi2 && cpu_has_adx) {
        g_fr_mont_mul = fr_mont_mul_bmi2;
        g_fp_mont_mul = fp_mont_mul_bmi2;
    } else
#endif
    {
        g_fr_mont_mul = fr_mont_mul_portable;
        g_fp_mont_mul = fp_mont_mul_portable;
    }
}

/* Public dispatchers — called by fr_mul/fp_mul via extern */
void fr_mont_mul_accel(uint64_t r[4], const uint64_t a[4], const uint64_t b[4])
{
    if (__builtin_expect(!g_fr_mont_mul, 0))
        init_dispatch();
    g_fr_mont_mul(r, a, b);
}

void fp_mont_mul_accel(uint64_t r[6], const uint64_t a[6], const uint64_t b[6])
{
    if (__builtin_expect(!g_fp_mont_mul, 0))
        init_dispatch();
    g_fp_mont_mul(r, a, b);
}

/* ================================================================
 * Tier 1: AVX-512 IFMA batch multiply (8 independent Fr muls)
 * Only called after CPUID confirms IFMA support.
 * ================================================================ */

#if defined(__x86_64__) || defined(_M_X64)

__attribute__((target("avx512ifma,avx512f")))
void fr_mul_batch8_ifma(struct fr r[8], const struct fr a[8], const struct fr b[8])
{
    #include <immintrin.h>

    static const uint64_t P52[5] = {
        0x000FFFFF00000001ULL, 0x000FFE5BFEFFFFFFULL,
        0x00009A1D80553BDAULL, 0x000D483339D80809ULL,
        0x00073EDA753299D7ULL
    };
    static const uint64_t INV52 = 0x000FFFFEFFFFFFFUL;

    uint64_t a52[8][5], b52[8][5];
    for (int k = 0; k < 8; k++) {
        a52[k][0] = a[k].d[0] & RADIX52_MASK;
        a52[k][1] = ((a[k].d[0] >> 52) | (a[k].d[1] << 12)) & RADIX52_MASK;
        a52[k][2] = ((a[k].d[1] >> 40) | (a[k].d[2] << 24)) & RADIX52_MASK;
        a52[k][3] = ((a[k].d[2] >> 28) | (a[k].d[3] << 36)) & RADIX52_MASK;
        a52[k][4] = a[k].d[3] >> 16;

        b52[k][0] = b[k].d[0] & RADIX52_MASK;
        b52[k][1] = ((b[k].d[0] >> 52) | (b[k].d[1] << 12)) & RADIX52_MASK;
        b52[k][2] = ((b[k].d[1] >> 40) | (b[k].d[2] << 24)) & RADIX52_MASK;
        b52[k][3] = ((b[k].d[2] >> 28) | (b[k].d[3] << 36)) & RADIX52_MASK;
        b52[k][4] = b[k].d[3] >> 16;
    }

    __m512i t0 = _mm512_setzero_si512();
    __m512i t1 = _mm512_setzero_si512();
    __m512i t2 = _mm512_setzero_si512();
    __m512i t3 = _mm512_setzero_si512();
    __m512i t4 = _mm512_setzero_si512();
    __m512i t5 = _mm512_setzero_si512();

    __m512i mask52 = _mm512_set1_epi64((long long)RADIX52_MASK);
    __m512i vp[5], vinv;
    for (int j = 0; j < 5; j++)
        vp[j] = _mm512_set1_epi64((long long)P52[j]);
    vinv = _mm512_set1_epi64((long long)INV52);

    __m512i va[5], vb[5];
    for (int j = 0; j < 5; j++) {
        uint64_t ta[8], tb[8];
        for (int k = 0; k < 8; k++) { ta[k] = a52[k][j]; tb[k] = b52[k][j]; }
        va[j] = _mm512_loadu_si512((__m512i *)ta);
        vb[j] = _mm512_loadu_si512((__m512i *)tb);
    }


    for (int i = 0; i < 5; i++) {
        __m512i vbi = vb[i];

        /* Multiply-accumulate low parts */
        t0 = _mm512_madd52lo_epu64(t0, va[0], vbi);
        t1 = _mm512_madd52lo_epu64(t1, va[1], vbi);
        t2 = _mm512_madd52lo_epu64(t2, va[2], vbi);
        t3 = _mm512_madd52lo_epu64(t3, va[3], vbi);
        t4 = _mm512_madd52lo_epu64(t4, va[4], vbi);

        /* High parts */
        __m512i c[5];
        c[0] = _mm512_madd52hi_epu64(_mm512_setzero_si512(), va[0], vbi);
        c[1] = _mm512_madd52hi_epu64(_mm512_setzero_si512(), va[1], vbi);
        c[2] = _mm512_madd52hi_epu64(_mm512_setzero_si512(), va[2], vbi);
        c[3] = _mm512_madd52hi_epu64(_mm512_setzero_si512(), va[3], vbi);
        c[4] = _mm512_madd52hi_epu64(_mm512_setzero_si512(), va[4], vbi);

        /* Carry propagation */
        __m512i carry;
        carry = _mm512_srli_epi64(t0, 52); t0 = _mm512_and_si512(t0, mask52);
        t1 = _mm512_add_epi64(_mm512_add_epi64(t1, carry), c[0]);
        carry = _mm512_srli_epi64(t1, 52); t1 = _mm512_and_si512(t1, mask52);
        t2 = _mm512_add_epi64(_mm512_add_epi64(t2, carry), c[1]);
        carry = _mm512_srli_epi64(t2, 52); t2 = _mm512_and_si512(t2, mask52);
        t3 = _mm512_add_epi64(_mm512_add_epi64(t3, carry), c[2]);
        carry = _mm512_srli_epi64(t3, 52); t3 = _mm512_and_si512(t3, mask52);
        t4 = _mm512_add_epi64(_mm512_add_epi64(t4, carry), c[3]);
        carry = _mm512_srli_epi64(t4, 52); t4 = _mm512_and_si512(t4, mask52);
        t5 = _mm512_add_epi64(_mm512_add_epi64(t5, carry), c[4]);

        /* Montgomery reduction */
        __m512i vm = _mm512_and_si512(
            _mm512_madd52lo_epu64(_mm512_setzero_si512(), t0, vinv), mask52);

        /* t += m * p */
        t0 = _mm512_madd52lo_epu64(t0, vm, vp[0]);
        __m512i mc[5];
        mc[0] = _mm512_madd52hi_epu64(_mm512_setzero_si512(), vm, vp[0]);
        t1 = _mm512_madd52lo_epu64(t1, vm, vp[1]);
        mc[1] = _mm512_madd52hi_epu64(_mm512_setzero_si512(), vm, vp[1]);
        t2 = _mm512_madd52lo_epu64(t2, vm, vp[2]);
        mc[2] = _mm512_madd52hi_epu64(_mm512_setzero_si512(), vm, vp[2]);
        t3 = _mm512_madd52lo_epu64(t3, vm, vp[3]);
        mc[3] = _mm512_madd52hi_epu64(_mm512_setzero_si512(), vm, vp[3]);
        t4 = _mm512_madd52lo_epu64(t4, vm, vp[4]);
        mc[4] = _mm512_madd52hi_epu64(_mm512_setzero_si512(), vm, vp[4]);

        /* Shift right by one 52-bit limb (Montgomery division by 2^52) */
        carry = _mm512_srli_epi64(t0, 52);
        __m512i new_t0 = _mm512_and_si512(
            _mm512_add_epi64(_mm512_add_epi64(t1, carry), mc[0]), mask52);
        carry = _mm512_srli_epi64(
            _mm512_add_epi64(_mm512_add_epi64(t1, carry), mc[0]), 52);

        __m512i new_t1 = _mm512_and_si512(
            _mm512_add_epi64(_mm512_add_epi64(t2, carry), mc[1]), mask52);
        carry = _mm512_srli_epi64(
            _mm512_add_epi64(_mm512_add_epi64(t2, carry), mc[1]), 52);

        __m512i new_t2 = _mm512_and_si512(
            _mm512_add_epi64(_mm512_add_epi64(t3, carry), mc[2]), mask52);
        carry = _mm512_srli_epi64(
            _mm512_add_epi64(_mm512_add_epi64(t3, carry), mc[2]), 52);

        __m512i new_t3 = _mm512_and_si512(
            _mm512_add_epi64(_mm512_add_epi64(t4, carry), mc[3]), mask52);
        carry = _mm512_srli_epi64(
            _mm512_add_epi64(_mm512_add_epi64(t4, carry), mc[3]), 52);

        __m512i new_t4 = _mm512_add_epi64(_mm512_add_epi64(t5, carry), mc[4]);

        t0 = new_t0; t1 = new_t1; t2 = new_t2; t3 = new_t3; t4 = new_t4;
        t5 = _mm512_setzero_si512();
    }

    /* Scalar final reduction per lane */
    uint64_t r0[8], r1[8], r2[8], r3[8], r4[8];
    _mm512_storeu_si512((__m512i *)r0, t0);
    _mm512_storeu_si512((__m512i *)r1, t1);
    _mm512_storeu_si512((__m512i *)r2, t2);
    _mm512_storeu_si512((__m512i *)r3, t3);
    _mm512_storeu_si512((__m512i *)r4, t4);

    for (int k = 0; k < 8; k++) {
        uint64_t t52[5] = {r0[k], r1[k], r2[k], r3[k], r4[k]};

        bool ge = false;
        for (int i = 4; i >= 0; i--) {
            if (t52[i] > P52[i]) { ge = true; break; }
            if (t52[i] < P52[i]) break;
            if (i == 0) ge = true;
        }
        if (ge) {
            uint64_t borrow = 0;
            for (int i = 0; i < 5; i++) {
                int64_t diff = (int64_t)t52[i] - (int64_t)P52[i] - (int64_t)borrow;
                if (diff < 0) { t52[i] = (uint64_t)(diff + ((int64_t)1 << 52)); borrow = 1; }
                else { t52[i] = (uint64_t)diff; borrow = 0; }
            }
        }

        r[k].d[0] = (t52[0] & RADIX52_MASK) | (t52[1] << 52);
        r[k].d[1] = (t52[1] >> 12) | (t52[2] << 40);
        r[k].d[2] = (t52[2] >> 24) | (t52[3] << 28);
        r[k].d[3] = (t52[3] >> 36) | (t52[4] << 16);
    }
}

#endif /* x86_64 */

/* Public batch8 dispatcher */
void fr_mul_batch8(struct fr r[8], const struct fr a[8], const struct fr b[8])
{
#if defined(__x86_64__) || defined(_M_X64)
    detect_cpu_features();
    if (cpu_has_avx512ifma) {
        fr_mul_batch8_ifma(r, a, b);
        return;
    }
#endif
    /* Fallback: 8 scalar multiplies */
    for (int k = 0; k < 8; k++)
        fr_mul(&r[k], &a[k], &b[k]);
}

#pragma GCC diagnostic pop
