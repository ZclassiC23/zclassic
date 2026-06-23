/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * 4-way parallel Keccak-f[1600] using AVX-512.
 * Processes 4 independent SHA3 states simultaneously.
 * Each __m512i holds 4 × uint64_t lanes (one per state).
 *
 * For SHA3 stream cipher: generates 4 × 64 = 256 bytes of keystream
 * per batch, 4x faster than scalar for the file transfer service. */

#include "crypto/sha3.h"
#include <immintrin.h>
#include <string.h>
#include <stdbool.h>

#ifdef __AVX512F__

/* 4-way parallel Keccak-f[1600].
 * st[25] where each element is 4 packed uint64_t lanes. */
static void keccakf_4way(__m512i st[25])
{
    static const uint64_t RNDC[24] = {
        0x0000000000000001, 0x0000000000008082, 0x800000000000808a, 0x8000000080008000,
        0x000000000000808b, 0x0000000080000001, 0x8000000080008081, 0x8000000000008009,
        0x000000000000008a, 0x0000000000000088, 0x0000000080008009, 0x000000008000000a,
        0x000000008000808b, 0x800000000000008b, 0x8000000000008089, 0x8000000000008003,
        0x8000000000008002, 0x8000000000000080, 0x000000000000800a, 0x800000008000000a,
        0x8000000080008081, 0x8000000000008080, 0x0000000080000001, 0x8000000080008008
    };

    #define ROL4(x, n) _mm512_or_si512(_mm512_slli_epi64((x), (n)), \
                                        _mm512_srli_epi64((x), 64-(n)))

    for (int round = 0; round < 24; round++) {
        /* θ step */
        __m512i bc0 = _mm512_xor_si512(_mm512_xor_si512(st[0], st[5]),
                      _mm512_xor_si512(st[10], _mm512_xor_si512(st[15], st[20])));
        __m512i bc1 = _mm512_xor_si512(_mm512_xor_si512(st[1], st[6]),
                      _mm512_xor_si512(st[11], _mm512_xor_si512(st[16], st[21])));
        __m512i bc2 = _mm512_xor_si512(_mm512_xor_si512(st[2], st[7]),
                      _mm512_xor_si512(st[12], _mm512_xor_si512(st[17], st[22])));
        __m512i bc3 = _mm512_xor_si512(_mm512_xor_si512(st[3], st[8]),
                      _mm512_xor_si512(st[13], _mm512_xor_si512(st[18], st[23])));
        __m512i bc4 = _mm512_xor_si512(_mm512_xor_si512(st[4], st[9]),
                      _mm512_xor_si512(st[14], _mm512_xor_si512(st[19], st[24])));

        __m512i t;
        t = _mm512_xor_si512(bc4, ROL4(bc1, 1));
        st[0] = _mm512_xor_si512(st[0], t); st[5] = _mm512_xor_si512(st[5], t);
        st[10] = _mm512_xor_si512(st[10], t); st[15] = _mm512_xor_si512(st[15], t);
        st[20] = _mm512_xor_si512(st[20], t);
        t = _mm512_xor_si512(bc0, ROL4(bc2, 1));
        st[1] = _mm512_xor_si512(st[1], t); st[6] = _mm512_xor_si512(st[6], t);
        st[11] = _mm512_xor_si512(st[11], t); st[16] = _mm512_xor_si512(st[16], t);
        st[21] = _mm512_xor_si512(st[21], t);
        t = _mm512_xor_si512(bc1, ROL4(bc3, 1));
        st[2] = _mm512_xor_si512(st[2], t); st[7] = _mm512_xor_si512(st[7], t);
        st[12] = _mm512_xor_si512(st[12], t); st[17] = _mm512_xor_si512(st[17], t);
        st[22] = _mm512_xor_si512(st[22], t);
        t = _mm512_xor_si512(bc2, ROL4(bc4, 1));
        st[3] = _mm512_xor_si512(st[3], t); st[8] = _mm512_xor_si512(st[8], t);
        st[13] = _mm512_xor_si512(st[13], t); st[18] = _mm512_xor_si512(st[18], t);
        st[23] = _mm512_xor_si512(st[23], t);
        t = _mm512_xor_si512(bc3, ROL4(bc0, 1));
        st[4] = _mm512_xor_si512(st[4], t); st[9] = _mm512_xor_si512(st[9], t);
        st[14] = _mm512_xor_si512(st[14], t); st[19] = _mm512_xor_si512(st[19], t);
        st[24] = _mm512_xor_si512(st[24], t);

        /* ρ and π steps */
        t = st[1];
        __m512i tmp;
        tmp = st[10]; st[10] = ROL4(t, 1); t = tmp;
        tmp = st[7];  st[7]  = ROL4(t, 3); t = tmp;
        tmp = st[11]; st[11] = ROL4(t, 6); t = tmp;
        tmp = st[17]; st[17] = ROL4(t, 10); t = tmp;
        tmp = st[18]; st[18] = ROL4(t, 15); t = tmp;
        tmp = st[3];  st[3]  = ROL4(t, 21); t = tmp;
        tmp = st[5];  st[5]  = ROL4(t, 28); t = tmp;
        tmp = st[16]; st[16] = ROL4(t, 36); t = tmp;
        tmp = st[8];  st[8]  = ROL4(t, 45); t = tmp;
        tmp = st[21]; st[21] = ROL4(t, 55); t = tmp;
        tmp = st[24]; st[24] = ROL4(t, 2); t = tmp;
        tmp = st[4];  st[4]  = ROL4(t, 14); t = tmp;
        tmp = st[15]; st[15] = ROL4(t, 27); t = tmp;
        tmp = st[23]; st[23] = ROL4(t, 41); t = tmp;
        tmp = st[19]; st[19] = ROL4(t, 56); t = tmp;
        tmp = st[13]; st[13] = ROL4(t, 8); t = tmp;
        tmp = st[12]; st[12] = ROL4(t, 25); t = tmp;
        tmp = st[2];  st[2]  = ROL4(t, 43); t = tmp;
        tmp = st[20]; st[20] = ROL4(t, 62); t = tmp;
        tmp = st[14]; st[14] = ROL4(t, 18); t = tmp;
        tmp = st[22]; st[22] = ROL4(t, 39); t = tmp;
        tmp = st[9];  st[9]  = ROL4(t, 61); t = tmp;
        tmp = st[6];  st[6]  = ROL4(t, 20); t = tmp;
        st[1] = ROL4(t, 44);

        /* χ step */
        for (int j = 0; j < 25; j += 5) {
            __m512i s0 = st[j], s1 = st[j+1], s2 = st[j+2];
            __m512i s3 = st[j+3], s4 = st[j+4];
            st[j]   = _mm512_xor_si512(s0, _mm512_andnot_si512(s1, s2));
            st[j+1] = _mm512_xor_si512(s1, _mm512_andnot_si512(s2, s3));
            st[j+2] = _mm512_xor_si512(s2, _mm512_andnot_si512(s3, s4));
            st[j+3] = _mm512_xor_si512(s3, _mm512_andnot_si512(s4, s0));
            st[j+4] = _mm512_xor_si512(s4, _mm512_andnot_si512(s0, s1));
        }

        /* ι step */
        st[0] = _mm512_xor_si512(st[0], _mm512_set1_epi64((long long)RNDC[round]));
    }
    #undef ROL4
}

/* Generate 4 SHA3-512 hashes in parallel.
 * Each input is: key(32) || nonce(32) || counter(8) = 72 bytes.
 * SHA3-512 rate = 72 bytes, so exactly one block per input.
 * Output: 4 × 64 = 256 bytes of keystream. */
void sha3_512_x4(const uint8_t key[32], const uint8_t nonce[32],
                   uint64_t counter_base, uint8_t out[256])
{
    /* Build 4 input blocks with consecutive counters */
    uint8_t inputs[4][72];
    for (int i = 0; i < 4; i++) {
        memcpy(inputs[i], key, 32);
        memcpy(inputs[i] + 32, nonce, 32);
        uint64_t ctr = counter_base + (uint64_t)i;
        memcpy(inputs[i] + 64, &ctr, 8);
    }

    /* Initialize 4-way state (all zeros) */
    __m512i st[25];
    for (int i = 0; i < 25; i++)
        st[i] = _mm512_setzero_si512();

    /* Absorb: XOR each 72-byte input into its lane.
     * SHA3-512 rate = 72 bytes = 9 uint64_t words. */
    for (int w = 0; w < 9; w++) {
        uint64_t lanes[4] __attribute__((aligned(64)));
        for (int i = 0; i < 4; i++)
            memcpy(&lanes[i], inputs[i] + w * 8, 8);
        /* Load 4 lanes as one __m512i (each 64-bit element is one state) */
        /* Note: __m512i holds 8 uint64, we use first 4 */
        st[w] = _mm512_xor_si512(st[w], _mm512_loadu_si512(lanes));
    }

    /* SHA3-512 rate = 72 bytes = 9 lanes (words 0..8); words 9..24 are CAPACITY
     * and must never be touched by absorb/pad. The 72-byte input exactly FILLS
     * the rate, so finalization needs TWO permutations — identical to the scalar
     * sha3_512_finalize (sha3.c): permute the absorbed rate block, THEN absorb a
     * pad block (domain byte 0x06 at rate byte 0; pad10*1 terminator 0x80 at rate
     * byte 71 = word 8, bit 63) and permute again. The previous code wrote 0x06
     * into word 9 (capacity) and permuted only once — that is not SHA3-512 and
     * diverged from the scalar fallback, so an AVX-512 build could not exchange
     * file-market frames with a scalar build. */
    keccakf_4way(st);  /* permute the full first (rate) block */
    st[0] = _mm512_xor_si512(st[0], _mm512_set1_epi64(0x06));  /* domain sep, rate byte 0 */
    st[8] = _mm512_xor_si512(st[8],
                _mm512_set1_epi64((long long)0x8000000000000000ULL));  /* pad terminator, rate byte 71 */
    keccakf_4way(st);  /* permute the pad block */

    /* Squeeze: extract first 8 words (64 bytes) from each lane */
    for (int w = 0; w < 8; w++) {
        uint64_t lanes[8] __attribute__((aligned(64)));
        _mm512_storeu_si512(lanes, st[w]);
        for (int i = 0; i < 4; i++)
            memcpy(out + i * 64 + w * 8, &lanes[i], 8);
    }
}

bool sha3_avx512_available(void) { return true; }

#else /* no AVX-512 */

/* Fallback: 4 sequential SHA3-512 calls */
void sha3_512_x4(const uint8_t key[32], const uint8_t nonce[32],
                   uint64_t counter_base, uint8_t out[256])
{
    for (int i = 0; i < 4; i++) {
        struct sha3_512_ctx ctx;
        sha3_512_init(&ctx);
        sha3_512_write(&ctx, key, 32);
        sha3_512_write(&ctx, nonce, 32);
        uint64_t ctr = counter_base + (uint64_t)i;
        sha3_512_write(&ctx, (const unsigned char *)&ctr, 8);
        sha3_512_finalize(&ctx, out + i * 64);
    }
}

bool sha3_avx512_available(void) { return false; }

#endif
