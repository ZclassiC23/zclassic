/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Hardware-accelerated BN254 (alt-bn128) Fq Montgomery multiply.
 *
 * Fq is the 254-bit base field of BN254, the curve every Sprout Groth16
 * JoinSplit proof verifies over. In a from-genesis mint fold the early chain is
 * dense with Sprout proofs, and every tower/point op there bottoms out in this
 * one 4-limb Montgomery multiply — previously portable __uint128 schoolbook
 * only (bn254.c bn_fq_mont_mul), unlike BLS12-381 Fr which already ships a
 * BMI2+ADX path (fr_avx512.c). This adds the same runtime-dispatched
 * acceleration for BN254 Fq.
 *
 * SPEED path only: every implementation returns a BIT-IDENTICAL canonical
 * Montgomery product to the portable reference, so the accept/reject result of
 * any proof is unchanged. Proven by test_bn254_accel (differential oracle:
 * every path vs portable over a large random corpus + boundary vectors).
 *
 * Runtime CPUID with graceful degradation:
 *   Tier 1: BMI2+ADX — MULX + dual ADCX/ADOX carry chains
 *   Tier 2: portable — __uint128 schoolbook (always available)
 * Detection runs once at first use; the binary runs on any x86-64 CPU. */

#include "sapling/bn254_accel.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

/* BN254 Fq modulus (little-endian limbs) and -q^{-1} mod 2^64. Must match the
 * FQ_Q / FQ_INV in bn254.c exactly — a mismatch is a reduction bug, caught by
 * the differential oracle on the first vector. */
static const uint64_t BN_Q[4] = {
    0x3c208c16d87cfd47ULL, 0x97816a916871ca8dULL,
    0xb85045b68181585dULL, 0x30644e72e131a029ULL
};
static const uint64_t BN_INV = 0x87d20782e4866389ULL;

/* ── CPUID detection ─────────────────────────────────────────────── */

static bool g_cpu_bmi2 = false;
static bool g_cpu_adx = false;
static bool g_cpu_detected = false;

static void bn_detect_cpu(void)
{
    if (g_cpu_detected)
        return;
#if defined(__x86_64__) || defined(_M_X64)
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(7), "c"(0));
    g_cpu_bmi2 = (ebx >> 8) & 1;    /* EBX bit 8  */
    g_cpu_adx = (ebx >> 19) & 1;    /* EBX bit 19 */
#endif
    g_cpu_detected = true;
}

/* ── Tier 2: portable __uint128 schoolbook (canonical reference) ──── */

static void bn_fq_mont_mul_portable(uint64_t r[4], const uint64_t a[4],
                                    const uint64_t b[4])
{
    uint64_t t[8] = {0};

    for (int i = 0; i < 4; i++) {
        __uint128_t carry = 0;
        for (int j = 0; j < 4; j++) {
            carry += (__uint128_t)a[j] * b[i] + t[i + j];
            t[i + j] = (uint64_t)carry;
            carry >>= 64;
        }
        t[i + 4] = (uint64_t)carry;
    }

    for (int i = 0; i < 4; i++) {
        uint64_t m = t[i] * BN_INV;
        __uint128_t carry = 0;
        for (int j = 0; j < 4; j++) {
            carry += (__uint128_t)m * BN_Q[j] + t[i + j];
            t[i + j] = (uint64_t)carry;
            carry >>= 64;
        }
        for (int j = i + 4; j < 8; j++) {
            carry += t[j];
            t[j] = (uint64_t)carry;
            carry >>= 64;
        }
    }

    memcpy(r, t + 4, 32);
    /* Canonical final subtraction: r -= q once if r >= q. */
    bool ge = true;
    for (int i = 3; i >= 0; i--) {
        if (r[i] > BN_Q[i]) { ge = true; break; }
        if (r[i] < BN_Q[i]) { ge = false; break; }
    }
    if (ge) {
        __uint128_t borrow = 0;
        for (int i = 0; i < 4; i++) {
            __uint128_t v = (__uint128_t)r[i] - BN_Q[i] - borrow;
            r[i] = (uint64_t)v;
            borrow = (v >> 64) & 1;
        }
    }
}

/* ── Tier 1: BMI2+ADX (MULX + ADCX/ADOX) ──────────────────────────── */

#if defined(__x86_64__) || defined(_M_X64)
__attribute__((target("bmi2,adx")))
static void bn_fq_mont_mul_bmi2(uint64_t r[4], const uint64_t a[4],
                                const uint64_t b[4])
{
    static const uint64_t Q[4] = {
        0x3c208c16d87cfd47ULL, 0x97816a916871ca8dULL,
        0xb85045b68181585dULL, 0x30644e72e131a029ULL
    };
    static const uint64_t INV = 0x87d20782e4866389ULL;

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
        unsigned char cf2 =
            _addcarryx_u64(0, t[1], carry_mul, (unsigned long long *)&t[1]);
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

        lo0 = _mulx_u64(m, Q[0], &hi);
        cf = _addcarryx_u64(0, t[0], lo0, (unsigned long long *)&t[0]);
        carry_mul = hi + cf;

        lo1 = _mulx_u64(m, Q[1], &hi);
        cf = _addcarryx_u64(0, t[1], lo1, (unsigned long long *)&t[1]);
        cf2 = _addcarryx_u64(0, t[1], carry_mul, (unsigned long long *)&t[0]);
        carry_mul = hi + cf + cf2;

        lo2 = _mulx_u64(m, Q[2], &hi);
        cf = _addcarryx_u64(0, t[2], lo2, (unsigned long long *)&t[2]);
        cf2 = _addcarryx_u64(0, t[2], carry_mul, (unsigned long long *)&t[1]);
        carry_mul = hi + cf + cf2;

        lo3 = _mulx_u64(m, Q[3], &hi);
        cf = _addcarryx_u64(0, t[3], lo3, (unsigned long long *)&t[3]);
        cf2 = _addcarryx_u64(0, t[3], carry_mul, (unsigned long long *)&t[2]);
        carry_mul = hi + cf + cf2;

        t[3] = t[4] + carry_mul;
        t[4] = 0;
    }

    /* Final reduction: subtract Q once if t >= Q (t[4] is a possible overflow
     * limb). */
    bool ge = t[4] != 0;
    if (!ge) {
        for (int i = 3; i >= 0; i--) {
            if (t[i] > Q[i]) { ge = true; break; }
            if (t[i] < Q[i]) break;
            if (i == 0) ge = true;
        }
    }
    if (ge) {
        unsigned char borrow = 0;
        for (int i = 0; i < 4; i++)
            borrow = _subborrow_u64(borrow, t[i], Q[i],
                                    (unsigned long long *)&r[i]);
    } else {
        memcpy(r, t, 32);
    }
}
#endif /* __x86_64__ */

/* ── Runtime dispatch ─────────────────────────────────────────────── */

typedef void (*bn_fq_mul_fn)(uint64_t r[4], const uint64_t a[4],
                             const uint64_t b[4]);

static bn_fq_mul_fn g_bn_fq_mont_mul = NULL;

static void bn_init_dispatch(void)
{
    bn_detect_cpu();
#if defined(__x86_64__) || defined(_M_X64)
    if (g_cpu_bmi2 && g_cpu_adx)
        g_bn_fq_mont_mul = bn_fq_mont_mul_bmi2;
    else
#endif
        g_bn_fq_mont_mul = bn_fq_mont_mul_portable;
}

void bn_fq_mont_mul_accel(uint64_t r[4], const uint64_t a[4],
                          const uint64_t b[4])
{
    if (__builtin_expect(!g_bn_fq_mont_mul, 0))
        bn_init_dispatch();
    g_bn_fq_mont_mul(r, a, b);
}

const char *bn254_accel_implementation(void)
{
    bn_detect_cpu();
#if defined(__x86_64__) || defined(_M_X64)
    if (g_cpu_bmi2 && g_cpu_adx)
        return "BMI2+ADX (MULX+ADCX+ADOX)";
#endif
    return "portable (__int128)";
}

void bn254_accel_mont_mul_portable(uint64_t r[4], const uint64_t a[4],
                                   const uint64_t b[4])
{
    bn_fq_mont_mul_portable(r, a, b);
}

bool bn254_accel_mont_mul_bmi2(uint64_t r[4], const uint64_t a[4],
                               const uint64_t b[4])
{
    bn_detect_cpu();
#if defined(__x86_64__) || defined(_M_X64)
    if (g_cpu_bmi2 && g_cpu_adx) {
        bn_fq_mont_mul_bmi2(r, a, b);
        return true;
    }
#endif
    (void)r; (void)a; (void)b;
    return false;
}
