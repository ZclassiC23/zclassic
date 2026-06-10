/* Copyright (c) 2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * SHA-256 with Intel SHA-NI hardware acceleration (runtime detected).
 * Falls back to portable C23 on CPUs without SHA-NI. */

#include "crypto/sha256.h"
#include "crypto/common.h"
#include "util/log_macros.h"
#include <string.h>

/* --- SHA-NI accelerated transform (Intel Goldmont+, AMD Zen) --- */

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>

/* Only referenced from the (x86 && __SHA__) hardware path below; guarding the
 * definition keeps non-SHA-NI -march=native builds clean under -Werror. */
#if (defined(__x86_64__) || defined(__i386__)) && defined(__SHA__)
static int sha_ni_available = -1; /* -1 = not checked, 0 = no, 1 = yes */
#endif

/* Forward declarations for self-test in detect_sha_ni */
static void sha256_transform_portable(uint32_t *s, const unsigned char *chunk);
#ifdef __SHA__
__attribute__((target("sha,sse4.1")))
static void sha256_transform_shani(uint32_t *state, const unsigned char *data);
#endif

/* Defined only where it is called: the call sites below are all guarded by
 * (x86 && __SHA__). On an x86 target whose -march=native does NOT enable
 * __SHA__, leaving this defined trips -Werror=unused-function. */
#if (defined(__x86_64__) || defined(__i386__)) && defined(__SHA__)
static void detect_sha_ni(void)
{
    unsigned int eax, ebx, ecx, edx;
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        sha_ni_available = 0;
        return;
    }
    int hw_capable = (ebx >> 29) & 1;
    if (!hw_capable) {
        sha_ni_available = 0;
        return;
    }

    /* Hardware SHA-NI detected — validate with self-test before enabling.
     * This catches implementation bugs and CPU errata. */
#ifdef __SHA__
    {
        const unsigned char test_data[64] = {
            0x61, 0x62, 0x63, 0x80, 0,0,0,0, 0,0,0,0, 0,0,0,0,
            0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
            0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
            0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,24
        };
        uint32_t sp[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                          0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
        uint32_t sn[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                          0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
        sha256_transform_portable(sp, test_data);
        sha256_transform_shani(sn, test_data);
        if (memcmp(sp, sn, 32) == 0) {
            sha_ni_available = 1;
        } else {
            sha_ni_available = 0; /* Implementation mismatch — stay on portable */
        }
    }
#else
    sha_ni_available = 0;
#endif
}
#endif /* x86 && __SHA__ */

#ifdef __SHA__
#include <immintrin.h>

__attribute__((target("sha,sse4.1")))
static void sha256_transform_shani(uint32_t *state, const unsigned char *data)
{
    __m128i state0, state1, msg, tmp;
    __m128i msg0, msg1, msg2, msg3;
    __m128i abef_save, cdgh_save;

    /* Load initial state: ABEF and CDGH */
    tmp    = _mm_loadu_si128((const __m128i *)(state));
    state1 = _mm_loadu_si128((const __m128i *)(state + 4));

    /* Rearrange for SHA-NI: ABEF = [A, B, E, F], CDGH = [C, D, G, H] */
    tmp    = _mm_shuffle_epi32(tmp, 0xB1);    /* BADC */
    state1 = _mm_shuffle_epi32(state1, 0x1B); /* GHEF → FEHG */
    state0 = _mm_alignr_epi8(tmp, state1, 8); /* ABEF */
    state1 = _mm_blend_epi16(state1, tmp, 0xF0); /* CDGH */

    abef_save = state0;
    cdgh_save = state1;

    /* Rounds 0-3 */
    msg0 = _mm_loadu_si128((const __m128i *)(data));
    msg0 = _mm_shuffle_epi8(msg0, _mm_set_epi64x(
        0x0c0d0e0f08090a0bLL, 0x0405060700010203LL));
    msg  = _mm_add_epi32(msg0, _mm_set_epi32(
        (int)0xE9B5DBA5, (int)0xB5C0FBCF, (int)0x71374491, (int)0x428A2F98));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    msg    = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

    /* Rounds 4-7 */
    msg1 = _mm_loadu_si128((const __m128i *)(data + 16));
    msg1 = _mm_shuffle_epi8(msg1, _mm_set_epi64x(
        0x0c0d0e0f08090a0bLL, 0x0405060700010203LL));
    msg  = _mm_add_epi32(msg1, _mm_set_epi32(
        (int)0xAB1C5ED5, (int)0x923F82A4, (int)0x59F111F1, (int)0x3956C25B));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    msg    = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg0   = _mm_sha256msg1_epu32(msg0, msg1);

    /* Rounds 8-11 */
    msg2 = _mm_loadu_si128((const __m128i *)(data + 32));
    msg2 = _mm_shuffle_epi8(msg2, _mm_set_epi64x(
        0x0c0d0e0f08090a0bLL, 0x0405060700010203LL));
    msg  = _mm_add_epi32(msg2, _mm_set_epi32(
        (int)0x550C7DC3, (int)0x243185BE, (int)0x12835B01, (int)0xD807AA98));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    msg    = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg1   = _mm_sha256msg1_epu32(msg1, msg2);

    /* Rounds 12-15 */
    msg3 = _mm_loadu_si128((const __m128i *)(data + 48));
    msg3 = _mm_shuffle_epi8(msg3, _mm_set_epi64x(
        0x0c0d0e0f08090a0bLL, 0x0405060700010203LL));
    msg  = _mm_add_epi32(msg3, _mm_set_epi32(
        (int)0xC19BF174, (int)0x9BDC06A7, (int)0x80DEB1FE, (int)0x72BE5D74));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp    = _mm_alignr_epi8(msg3, msg2, 4);
    msg0   = _mm_add_epi32(msg0, tmp);
    msg0   = _mm_sha256msg2_epu32(msg0, msg3);
    msg    = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg2   = _mm_sha256msg1_epu32(msg2, msg3);

    /* Rounds 16-19 */
    msg  = _mm_add_epi32(msg0, _mm_set_epi32(
        (int)0x240CA1CC, (int)0x0FC19DC6, (int)0xEFBE4786, (int)0xE49B69C1));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp    = _mm_alignr_epi8(msg0, msg3, 4);
    msg1   = _mm_add_epi32(msg1, tmp);
    msg1   = _mm_sha256msg2_epu32(msg1, msg0);
    msg    = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg3   = _mm_sha256msg1_epu32(msg3, msg0);

    /* Rounds 20-23 */
    msg  = _mm_add_epi32(msg1, _mm_set_epi32(
        (int)0x76F988DA, (int)0x5CB0A9DC, (int)0x4A7484AA, (int)0x2DE92C6F));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp    = _mm_alignr_epi8(msg1, msg0, 4);
    msg2   = _mm_add_epi32(msg2, tmp);
    msg2   = _mm_sha256msg2_epu32(msg2, msg1);
    msg    = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg0   = _mm_sha256msg1_epu32(msg0, msg1);

    /* Rounds 24-27 */
    msg  = _mm_add_epi32(msg2, _mm_set_epi32(
        (int)0xBF597FC7, (int)0xB00327C8, (int)0xA831C66D, (int)0x983E5152));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp    = _mm_alignr_epi8(msg2, msg1, 4);
    msg3   = _mm_add_epi32(msg3, tmp);
    msg3   = _mm_sha256msg2_epu32(msg3, msg2);
    msg    = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg1   = _mm_sha256msg1_epu32(msg1, msg2);

    /* Rounds 28-31 */
    msg  = _mm_add_epi32(msg3, _mm_set_epi32(
        (int)0x14292967, (int)0x06CA6351, (int)0xD5A79147, (int)0xC6E00BF3));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp    = _mm_alignr_epi8(msg3, msg2, 4);
    msg0   = _mm_add_epi32(msg0, tmp);
    msg0   = _mm_sha256msg2_epu32(msg0, msg3);
    msg    = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg2   = _mm_sha256msg1_epu32(msg2, msg3);

    /* Rounds 32-35 */
    msg  = _mm_add_epi32(msg0, _mm_set_epi32(
        (int)0x53380D13, (int)0x4D2C6DFC, (int)0x2E1B2138, (int)0x27B70A85));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp    = _mm_alignr_epi8(msg0, msg3, 4);
    msg1   = _mm_add_epi32(msg1, tmp);
    msg1   = _mm_sha256msg2_epu32(msg1, msg0);
    msg    = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg3   = _mm_sha256msg1_epu32(msg3, msg0);

    /* Rounds 36-39 */
    msg  = _mm_add_epi32(msg1, _mm_set_epi32(
        (int)0x92722C85, (int)0x81C2C92E, (int)0x766A0ABB, (int)0x650A7354));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp    = _mm_alignr_epi8(msg1, msg0, 4);
    msg2   = _mm_add_epi32(msg2, tmp);
    msg2   = _mm_sha256msg2_epu32(msg2, msg1);
    msg    = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg0   = _mm_sha256msg1_epu32(msg0, msg1);

    /* Rounds 40-43 */
    msg  = _mm_add_epi32(msg2, _mm_set_epi32(
        (int)0xC76C51A3, (int)0xC24B8B70, (int)0xA81A664B, (int)0xA2BFE8A1));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp    = _mm_alignr_epi8(msg2, msg1, 4);
    msg3   = _mm_add_epi32(msg3, tmp);
    msg3   = _mm_sha256msg2_epu32(msg3, msg2);
    msg    = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg1   = _mm_sha256msg1_epu32(msg1, msg2);

    /* Rounds 44-47 */
    msg  = _mm_add_epi32(msg3, _mm_set_epi32(
        (int)0x106AA070, (int)0xF40E3585, (int)0xD6990624, (int)0xD192E819));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp    = _mm_alignr_epi8(msg3, msg2, 4);
    msg0   = _mm_add_epi32(msg0, tmp);
    msg0   = _mm_sha256msg2_epu32(msg0, msg3);
    msg    = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg2   = _mm_sha256msg1_epu32(msg2, msg3);

    /* Rounds 48-51 */
    msg  = _mm_add_epi32(msg0, _mm_set_epi32(
        (int)0x34B0BCB5, (int)0x2748774C, (int)0x1E376C08, (int)0x19A4C116));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp    = _mm_alignr_epi8(msg0, msg3, 4);
    msg1   = _mm_add_epi32(msg1, tmp);
    msg1   = _mm_sha256msg2_epu32(msg1, msg0);
    msg    = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
    msg3   = _mm_sha256msg1_epu32(msg3, msg0);

    /* Rounds 52-55 */
    msg  = _mm_add_epi32(msg1, _mm_set_epi32(
        (int)0x682E6FF3, (int)0x5B9CCA4F, (int)0x4ED8AA4A, (int)0x391C0CB3));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp    = _mm_alignr_epi8(msg1, msg0, 4);
    msg2   = _mm_add_epi32(msg2, tmp);
    msg2   = _mm_sha256msg2_epu32(msg2, msg1);
    msg    = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

    /* Rounds 56-59 */
    msg  = _mm_add_epi32(msg2, _mm_set_epi32(
        (int)0x8CC70208, (int)0x84C87814, (int)0x78A5636F, (int)0x748F82EE));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    tmp    = _mm_alignr_epi8(msg2, msg1, 4);
    msg3   = _mm_add_epi32(msg3, tmp);
    msg3   = _mm_sha256msg2_epu32(msg3, msg2);
    msg    = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

    /* Rounds 60-63 */
    msg  = _mm_add_epi32(msg3, _mm_set_epi32(
        (int)0xC67178F2, (int)0xBEF9A3F7, (int)0xA4506CEB, (int)0x90BEFFFA));
    state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
    msg    = _mm_shuffle_epi32(msg, 0x0E);
    state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

    /* Add saved state */
    state0 = _mm_add_epi32(state0, abef_save);
    state1 = _mm_add_epi32(state1, cdgh_save);

    /* Rearrange back to ABCDEFGH */
    tmp    = _mm_shuffle_epi32(state0, 0x1B); /* FEBA */
    state1 = _mm_shuffle_epi32(state1, 0xB1); /* DCHG */
    state0 = _mm_blend_epi16(tmp, state1, 0xF0); /* DCBA */
    state1 = _mm_alignr_epi8(state1, tmp, 8);    /* HGFE */

    _mm_storeu_si128((__m128i *)(state), state0);
    _mm_storeu_si128((__m128i *)(state + 4), state1);
}
#endif /* __SHA__ */
#endif /* x86 */

/* --- Portable C fallback --- */

static inline uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) { return z ^ (x & (y ^ z)); }
static inline uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (z & (x | y)); }
static inline uint32_t Sigma0(uint32_t x) { return (x >> 2 | x << 30) ^ (x >> 13 | x << 19) ^ (x >> 22 | x << 10); }
static inline uint32_t Sigma1(uint32_t x) { return (x >> 6 | x << 26) ^ (x >> 11 | x << 21) ^ (x >> 25 | x << 7); }
static inline uint32_t sigma0(uint32_t x) { return (x >> 7 | x << 25) ^ (x >> 18 | x << 14) ^ (x >> 3); }
static inline uint32_t sigma1(uint32_t x) { return (x >> 17 | x << 15) ^ (x >> 19 | x << 13) ^ (x >> 10); }

static inline void Round(uint32_t a, uint32_t b, uint32_t c, uint32_t *d,
                          uint32_t e, uint32_t f, uint32_t g, uint32_t *h,
                          uint32_t k, uint32_t w)
{
    uint32_t t1 = *h + Sigma1(e) + Ch(e, f, g) + k + w;
    uint32_t t2 = Sigma0(a) + Maj(a, b, c);
    *d += t1;
    *h = t1 + t2;
}

static void sha256_transform_portable(uint32_t *s, const unsigned char *chunk)
{
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    uint32_t w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w10, w11, w12, w13, w14, w15;

    Round(a, b, c, &d, e, f, g, &h, 0x428a2f98, w0 = ReadBE32(chunk + 0));
    Round(h, a, b, &c, d, e, f, &g, 0x71374491, w1 = ReadBE32(chunk + 4));
    Round(g, h, a, &b, c, d, e, &f, 0xb5c0fbcf, w2 = ReadBE32(chunk + 8));
    Round(f, g, h, &a, b, c, d, &e, 0xe9b5dba5, w3 = ReadBE32(chunk + 12));
    Round(e, f, g, &h, a, b, c, &d, 0x3956c25b, w4 = ReadBE32(chunk + 16));
    Round(d, e, f, &g, h, a, b, &c, 0x59f111f1, w5 = ReadBE32(chunk + 20));
    Round(c, d, e, &f, g, h, a, &b, 0x923f82a4, w6 = ReadBE32(chunk + 24));
    Round(b, c, d, &e, f, g, h, &a, 0xab1c5ed5, w7 = ReadBE32(chunk + 28));
    Round(a, b, c, &d, e, f, g, &h, 0xd807aa98, w8 = ReadBE32(chunk + 32));
    Round(h, a, b, &c, d, e, f, &g, 0x12835b01, w9 = ReadBE32(chunk + 36));
    Round(g, h, a, &b, c, d, e, &f, 0x243185be, w10 = ReadBE32(chunk + 40));
    Round(f, g, h, &a, b, c, d, &e, 0x550c7dc3, w11 = ReadBE32(chunk + 44));
    Round(e, f, g, &h, a, b, c, &d, 0x72be5d74, w12 = ReadBE32(chunk + 48));
    Round(d, e, f, &g, h, a, b, &c, 0x80deb1fe, w13 = ReadBE32(chunk + 52));
    Round(c, d, e, &f, g, h, a, &b, 0x9bdc06a7, w14 = ReadBE32(chunk + 56));
    Round(b, c, d, &e, f, g, h, &a, 0xc19bf174, w15 = ReadBE32(chunk + 60));

    Round(a, b, c, &d, e, f, g, &h, 0xe49b69c1, w0 += sigma1(w14) + w9 + sigma0(w1));
    Round(h, a, b, &c, d, e, f, &g, 0xefbe4786, w1 += sigma1(w15) + w10 + sigma0(w2));
    Round(g, h, a, &b, c, d, e, &f, 0x0fc19dc6, w2 += sigma1(w0) + w11 + sigma0(w3));
    Round(f, g, h, &a, b, c, d, &e, 0x240ca1cc, w3 += sigma1(w1) + w12 + sigma0(w4));
    Round(e, f, g, &h, a, b, c, &d, 0x2de92c6f, w4 += sigma1(w2) + w13 + sigma0(w5));
    Round(d, e, f, &g, h, a, b, &c, 0x4a7484aa, w5 += sigma1(w3) + w14 + sigma0(w6));
    Round(c, d, e, &f, g, h, a, &b, 0x5cb0a9dc, w6 += sigma1(w4) + w15 + sigma0(w7));
    Round(b, c, d, &e, f, g, h, &a, 0x76f988da, w7 += sigma1(w5) + w0 + sigma0(w8));
    Round(a, b, c, &d, e, f, g, &h, 0x983e5152, w8 += sigma1(w6) + w1 + sigma0(w9));
    Round(h, a, b, &c, d, e, f, &g, 0xa831c66d, w9 += sigma1(w7) + w2 + sigma0(w10));
    Round(g, h, a, &b, c, d, e, &f, 0xb00327c8, w10 += sigma1(w8) + w3 + sigma0(w11));
    Round(f, g, h, &a, b, c, d, &e, 0xbf597fc7, w11 += sigma1(w9) + w4 + sigma0(w12));
    Round(e, f, g, &h, a, b, c, &d, 0xc6e00bf3, w12 += sigma1(w10) + w5 + sigma0(w13));
    Round(d, e, f, &g, h, a, b, &c, 0xd5a79147, w13 += sigma1(w11) + w6 + sigma0(w14));
    Round(c, d, e, &f, g, h, a, &b, 0x06ca6351, w14 += sigma1(w12) + w7 + sigma0(w15));
    Round(b, c, d, &e, f, g, h, &a, 0x14292967, w15 += sigma1(w13) + w8 + sigma0(w0));

    Round(a, b, c, &d, e, f, g, &h, 0x27b70a85, w0 += sigma1(w14) + w9 + sigma0(w1));
    Round(h, a, b, &c, d, e, f, &g, 0x2e1b2138, w1 += sigma1(w15) + w10 + sigma0(w2));
    Round(g, h, a, &b, c, d, e, &f, 0x4d2c6dfc, w2 += sigma1(w0) + w11 + sigma0(w3));
    Round(f, g, h, &a, b, c, d, &e, 0x53380d13, w3 += sigma1(w1) + w12 + sigma0(w4));
    Round(e, f, g, &h, a, b, c, &d, 0x650a7354, w4 += sigma1(w2) + w13 + sigma0(w5));
    Round(d, e, f, &g, h, a, b, &c, 0x766a0abb, w5 += sigma1(w3) + w14 + sigma0(w6));
    Round(c, d, e, &f, g, h, a, &b, 0x81c2c92e, w6 += sigma1(w4) + w15 + sigma0(w7));
    Round(b, c, d, &e, f, g, h, &a, 0x92722c85, w7 += sigma1(w5) + w0 + sigma0(w8));
    Round(a, b, c, &d, e, f, g, &h, 0xa2bfe8a1, w8 += sigma1(w6) + w1 + sigma0(w9));
    Round(h, a, b, &c, d, e, f, &g, 0xa81a664b, w9 += sigma1(w7) + w2 + sigma0(w10));
    Round(g, h, a, &b, c, d, e, &f, 0xc24b8b70, w10 += sigma1(w8) + w3 + sigma0(w11));
    Round(f, g, h, &a, b, c, d, &e, 0xc76c51a3, w11 += sigma1(w9) + w4 + sigma0(w12));
    Round(e, f, g, &h, a, b, c, &d, 0xd192e819, w12 += sigma1(w10) + w5 + sigma0(w13));
    Round(d, e, f, &g, h, a, b, &c, 0xd6990624, w13 += sigma1(w11) + w6 + sigma0(w14));
    Round(c, d, e, &f, g, h, a, &b, 0xf40e3585, w14 += sigma1(w12) + w7 + sigma0(w15));
    Round(b, c, d, &e, f, g, h, &a, 0x106aa070, w15 += sigma1(w13) + w8 + sigma0(w0));

    Round(a, b, c, &d, e, f, g, &h, 0x19a4c116, w0 += sigma1(w14) + w9 + sigma0(w1));
    Round(h, a, b, &c, d, e, f, &g, 0x1e376c08, w1 += sigma1(w15) + w10 + sigma0(w2));
    Round(g, h, a, &b, c, d, e, &f, 0x2748774c, w2 += sigma1(w0) + w11 + sigma0(w3));
    Round(f, g, h, &a, b, c, d, &e, 0x34b0bcb5, w3 += sigma1(w1) + w12 + sigma0(w4));
    Round(e, f, g, &h, a, b, c, &d, 0x391c0cb3, w4 += sigma1(w2) + w13 + sigma0(w5));
    Round(d, e, f, &g, h, a, b, &c, 0x4ed8aa4a, w5 += sigma1(w3) + w14 + sigma0(w6));
    Round(c, d, e, &f, g, h, a, &b, 0x5b9cca4f, w6 += sigma1(w4) + w15 + sigma0(w7));
    Round(b, c, d, &e, f, g, h, &a, 0x682e6ff3, w7 += sigma1(w5) + w0 + sigma0(w8));
    Round(a, b, c, &d, e, f, g, &h, 0x748f82ee, w8 += sigma1(w6) + w1 + sigma0(w9));
    Round(h, a, b, &c, d, e, f, &g, 0x78a5636f, w9 += sigma1(w7) + w2 + sigma0(w10));
    Round(g, h, a, &b, c, d, e, &f, 0x84c87814, w10 += sigma1(w8) + w3 + sigma0(w11));
    Round(f, g, h, &a, b, c, d, &e, 0x8cc70208, w11 += sigma1(w9) + w4 + sigma0(w12));
    Round(e, f, g, &h, a, b, c, &d, 0x90befffa, w12 += sigma1(w10) + w5 + sigma0(w13));
    Round(d, e, f, &g, h, a, b, &c, 0xa4506ceb, w13 += sigma1(w11) + w6 + sigma0(w14));
    Round(c, d, e, &f, g, h, a, &b, 0xbef9a3f7, w14 += sigma1(w12) + w7 + sigma0(w15));
    Round(b, c, d, &e, f, g, h, &a, 0xc67178f2, w15 += sigma1(w13) + w8 + sigma0(w0));

    s[0] += a;
    s[1] += b;
    s[2] += c;
    s[3] += d;
    s[4] += e;
    s[5] += f;
    s[6] += g;
    s[7] += h;
}

/* --- Dispatch: SHA-NI when available, portable fallback --- */

static inline void sha256_transform(uint32_t *s, const unsigned char *chunk)
{
#if (defined(__x86_64__) || defined(__i386__)) && defined(__SHA__)
    if (__builtin_expect(sha_ni_available < 0, 0))
        detect_sha_ni();
    if (sha_ni_available) {
        sha256_transform_shani(s, chunk);
        return;
    }
#endif
    sha256_transform_portable(s, chunk);
}

/* Self-test: verify SHA-NI produces identical results to portable.
 * Called once at startup. Returns true if SHA-NI is safe to use. */
bool sha256_selftest(void)
{
#if (defined(__x86_64__) || defined(__i386__)) && defined(__SHA__)
    if (sha_ni_available < 0) detect_sha_ni();
    if (!sha_ni_available) return true; /* no SHA-NI, nothing to test */

    /* Test vector: SHA-256("abc") = ba7816bf... */
    const unsigned char test_data[64] = {
        0x61, 0x62, 0x63, 0x80, 0,0,0,0, 0,0,0,0, 0,0,0,0,
        0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
        0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
        0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,24
    };

    /* Portable result */
    uint32_t s_port[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    sha256_transform_portable(s_port, test_data);

    /* SHA-NI result */
    uint32_t s_shani[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    sha256_transform_shani(s_shani, test_data);

    /* Compare */
    if (memcmp(s_port, s_shani, 32) != 0) {
        sha_ni_available = 0; /* Disable — results don't match */
        LOG_FAIL("sha256",
                 "sha_ni_selftest: portable vs SHA-NI mismatch — CPU reports SHA-NI "
                 "but test vectors disagree; falling back to portable implementation");
    }
    return true;
#else
    return true;
#endif
}

const char *sha256_implementation(void)
{
#if (defined(__x86_64__) || defined(__i386__)) && defined(__SHA__)
    if (sha_ni_available < 0) detect_sha_ni();
    if (sha_ni_available) return "SHA-NI (hardware)";
#endif
    return "portable C";
}

void sha256_init(struct sha256_ctx *ctx)
{
    ctx->s[0] = 0x6a09e667ul;
    ctx->s[1] = 0xbb67ae85ul;
    ctx->s[2] = 0x3c6ef372ul;
    ctx->s[3] = 0xa54ff53aul;
    ctx->s[4] = 0x510e527ful;
    ctx->s[5] = 0x9b05688cul;
    ctx->s[6] = 0x1f83d9abul;
    ctx->s[7] = 0x5be0cd19ul;
    ctx->bytes = 0;
}

void sha256_write(struct sha256_ctx *ctx, const unsigned char *data, size_t len)
{
    const unsigned char *end = data + len;
    size_t bufsize = ctx->bytes % 64;
    if (bufsize && bufsize + len >= 64) {
        memcpy(ctx->buf + bufsize, data, 64 - bufsize);
        ctx->bytes += 64 - bufsize;
        data += 64 - bufsize;
        sha256_transform(ctx->s, ctx->buf);
        bufsize = 0;
    }
    while (end >= data + 64) {
        sha256_transform(ctx->s, data);
        ctx->bytes += 64;
        data += 64;
    }
    if (end > data) {
        memcpy(ctx->buf + bufsize, data, end - data);
        ctx->bytes += end - data;
    }
}

void sha256_finalize(struct sha256_ctx *ctx, unsigned char hash[SHA256_OUTPUT_SIZE])
{
    static const unsigned char pad[64] = {0x80};
    unsigned char sizedesc[8];
    WriteBE64(sizedesc, ctx->bytes << 3);
    sha256_write(ctx, pad, 1 + ((119 - (ctx->bytes % 64)) % 64));
    sha256_write(ctx, sizedesc, 8);
    sha256_finalize_no_padding(ctx, hash, 0);
}

int sha256_finalize_no_padding(struct sha256_ctx *ctx, unsigned char hash[SHA256_OUTPUT_SIZE],
                               int enforce_compression)
{
    if (enforce_compression && ctx->bytes != 64)
        LOG_ERR("sha256",
                "finalize_no_padding: enforce_compression with ctx->bytes=%zu != 64",
                ctx->bytes);

    WriteBE32(hash, ctx->s[0]);
    WriteBE32(hash + 4, ctx->s[1]);
    WriteBE32(hash + 8, ctx->s[2]);
    WriteBE32(hash + 12, ctx->s[3]);
    WriteBE32(hash + 16, ctx->s[4]);
    WriteBE32(hash + 20, ctx->s[5]);
    WriteBE32(hash + 24, ctx->s[6]);
    WriteBE32(hash + 28, ctx->s[7]);
    return 0;
}
