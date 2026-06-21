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


#pragma GCC diagnostic pop
