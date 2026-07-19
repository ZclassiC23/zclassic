/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * 4-way batched SHA3-256: hash FOUR independent messages, produce FOUR digests.
 *
 * This is the right shape for batching many independent hashes — unlike
 * sha3_512_x4 (a keystream generator: one key/nonce across 4 counters) and
 * unlike the single-stream keccak_avx512.c permutation (measured 0.84-0.99x on
 * Zen 4, dominated by the cross-lane pi gather). Here the four Keccak states are
 * interleaved across the 4 low 64-bit slots of each __m512i, so theta/rho/pi/chi
 * are embarrassingly lane-parallel (NO cross-lane shuffle) — this is where a
 * double-pumped 512-bit unit genuinely amortizes: 4 hashes for ~1 permutation's
 * front-end cost.
 *
 * Runtime dispatch mirrors keccak_avx512.c: the AVX-512 lane carries
 * __attribute__((target(...))) so it compiles into the x86-64-v3 baseline and is
 * reached only when sha3_keccakf_avx512_available() confirms avx512f/vl/dq. The
 * scalar fallback (four sha3_256 calls) is the always-available reference and the
 * differential parity oracle (test group `sha3_256_x4`) proves the AVX-512 lane
 * is byte-for-byte identical to it.
 *
 * SHA3-256 rate = 1088 bits = 136 bytes = 17 uint64 words. Each lane may have a
 * different length; the absorb walks max(blockcount) blocks, XORing each lane's
 * block only while that lane still has one, and captures each lane's 4-word
 * digest at the permutation that follows its final (padded) block. */

#include "crypto/sha3.h"
#include "crypto/common.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SHA3_256_RATE_BYTES 136u  /* 17 * 8 */

/* Scalar reference: four independent one-shot SHA3-256 hashes. Always safe. */
static void sha3_256_x4_scalar(const uint8_t *const msgs[4], const size_t lens[4],
                               uint8_t out[4][32])
{
    for (int i = 0; i < 4; ++i)
        sha3_256(msgs[i], lens[i], out[i]);
}

#if defined(__x86_64__)

#include <immintrin.h>

/* 4-way (8-lane-capable) Keccak-f[1600]. st[25], each a __m512i whose 8 uint64
 * slots are 8 independent Keccak instances (we drive the low 4). Every step is
 * lane-parallel: word index selects among the 25 registers, rho is a fixed
 * per-word rotate applied uniformly to all slots — no cross-lane permute. This
 * is the reference 4-way permute (sha3_avx512.c) lifted under a target attribute
 * so it lands in the baseline binary. */
__attribute__((target("avx512f,avx512vl,avx512dq")))
static void keccakf_x4(__m512i st[25])
{
    static const uint64_t RNDC[24] = {
        0x0000000000000001, 0x0000000000008082, 0x800000000000808a, 0x8000000080008000,
        0x000000000000808b, 0x0000000080000001, 0x8000000080008081, 0x8000000000008009,
        0x000000000000008a, 0x0000000000000088, 0x0000000080008009, 0x000000008000000a,
        0x000000008000808b, 0x800000000000008b, 0x8000000000008089, 0x8000000000008003,
        0x8000000000008002, 0x8000000000000080, 0x000000000000800a, 0x800000008000000a,
        0x8000000080008081, 0x8000000000008080, 0x0000000080000001, 0x8000000080008008
    };

    #define ROL4(x, n) _mm512_rol_epi64((x), (n))

    for (int round = 0; round < 24; ++round) {
        /* theta */
        __m512i bc0 = _mm512_ternarylogic_epi64(_mm512_xor_si512(st[0], st[5]),
                                                 _mm512_xor_si512(st[10], st[15]), st[20], 0x96);
        __m512i bc1 = _mm512_ternarylogic_epi64(_mm512_xor_si512(st[1], st[6]),
                                                 _mm512_xor_si512(st[11], st[16]), st[21], 0x96);
        __m512i bc2 = _mm512_ternarylogic_epi64(_mm512_xor_si512(st[2], st[7]),
                                                 _mm512_xor_si512(st[12], st[17]), st[22], 0x96);
        __m512i bc3 = _mm512_ternarylogic_epi64(_mm512_xor_si512(st[3], st[8]),
                                                 _mm512_xor_si512(st[13], st[18]), st[23], 0x96);
        __m512i bc4 = _mm512_ternarylogic_epi64(_mm512_xor_si512(st[4], st[9]),
                                                 _mm512_xor_si512(st[14], st[19]), st[24], 0x96);

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

        /* rho + pi */
        t = st[1];
        __m512i tmp;
        tmp = st[10]; st[10] = ROL4(t, 1);  t = tmp;
        tmp = st[7];  st[7]  = ROL4(t, 3);  t = tmp;
        tmp = st[11]; st[11] = ROL4(t, 6);  t = tmp;
        tmp = st[17]; st[17] = ROL4(t, 10); t = tmp;
        tmp = st[18]; st[18] = ROL4(t, 15); t = tmp;
        tmp = st[3];  st[3]  = ROL4(t, 21); t = tmp;
        tmp = st[5];  st[5]  = ROL4(t, 28); t = tmp;
        tmp = st[16]; st[16] = ROL4(t, 36); t = tmp;
        tmp = st[8];  st[8]  = ROL4(t, 45); t = tmp;
        tmp = st[21]; st[21] = ROL4(t, 55); t = tmp;
        tmp = st[24]; st[24] = ROL4(t, 2);  t = tmp;
        tmp = st[4];  st[4]  = ROL4(t, 14); t = tmp;
        tmp = st[15]; st[15] = ROL4(t, 27); t = tmp;
        tmp = st[23]; st[23] = ROL4(t, 41); t = tmp;
        tmp = st[19]; st[19] = ROL4(t, 56); t = tmp;
        tmp = st[13]; st[13] = ROL4(t, 8);  t = tmp;
        tmp = st[12]; st[12] = ROL4(t, 25); t = tmp;
        tmp = st[2];  st[2]  = ROL4(t, 43); t = tmp;
        tmp = st[20]; st[20] = ROL4(t, 62); t = tmp;
        tmp = st[14]; st[14] = ROL4(t, 18); t = tmp;
        tmp = st[22]; st[22] = ROL4(t, 39); t = tmp;
        tmp = st[9];  st[9]  = ROL4(t, 61); t = tmp;
        tmp = st[6];  st[6]  = ROL4(t, 20); t = tmp;
        st[1] = ROL4(t, 44);

        /* chi: new[x] = s[x] ^ (~s[x+1] & s[x+2]) via ternary logic imm 0xD2 */
        for (int j = 0; j < 25; j += 5) {
            __m512i s0 = st[j], s1 = st[j+1], s2 = st[j+2];
            __m512i s3 = st[j+3], s4 = st[j+4];
            st[j]   = _mm512_ternarylogic_epi64(s0, s1, s2, 0xD2);
            st[j+1] = _mm512_ternarylogic_epi64(s1, s2, s3, 0xD2);
            st[j+2] = _mm512_ternarylogic_epi64(s2, s3, s4, 0xD2);
            st[j+3] = _mm512_ternarylogic_epi64(s3, s4, s0, 0xD2);
            st[j+4] = _mm512_ternarylogic_epi64(s4, s0, s1, 0xD2);
        }

        /* iota */
        st[0] = _mm512_xor_si512(st[0], _mm512_set1_epi64((long long)RNDC[round]));
    }
    #undef ROL4
}

__attribute__((target("avx512f,avx512vl,avx512dq")))
void sha3_256_x4_avx512(const uint8_t *const msgs[4], const size_t lens[4],
                        uint8_t out[4][32])
{
    /* Per-lane geometry. blockcount = full_blocks + 1 (the +1 is the pad block,
     * present even for len%136==0 and len==0). */
    size_t full_blocks[4], rem[4], blockcount[4], maxblocks = 1;
    uint8_t padbuf[4][SHA3_256_RATE_BYTES];
    for (int i = 0; i < 4; ++i) {
        full_blocks[i] = lens[i] / SHA3_256_RATE_BYTES;
        rem[i]         = lens[i] % SHA3_256_RATE_BYTES;
        blockcount[i]  = full_blocks[i] + 1;
        if (blockcount[i] > maxblocks) maxblocks = blockcount[i];

        /* Build the final (padded) rate block for lane i: trailing rem bytes of
         * message, domain byte 0x06 at offset rem, pad10*1 terminator 0x80 at
         * the last rate byte (135). When rem==135 the two collapse to 0x86. */
        memset(padbuf[i], 0, SHA3_256_RATE_BYTES);
        if (rem[i] > 0)
            memcpy(padbuf[i], msgs[i] + full_blocks[i] * SHA3_256_RATE_BYTES, rem[i]);
        padbuf[i][rem[i]] |= 0x06;
        padbuf[i][SHA3_256_RATE_BYTES - 1] |= 0x80;
    }

    __m512i st[25];
    for (int i = 0; i < 25; ++i) st[i] = _mm512_setzero_si512();

    for (size_t b = 0; b < maxblocks; ++b) {
        /* Resolve each lane's 136-byte block for this index (NULL = lane done). */
        const uint8_t *blk[4];
        for (int i = 0; i < 4; ++i) {
            if (b < full_blocks[i])
                blk[i] = msgs[i] + b * SHA3_256_RATE_BYTES;
            else if (b == full_blocks[i])
                blk[i] = padbuf[i];
            else
                blk[i] = NULL;
        }

        for (int w = 0; w < 17; ++w) {
            uint64_t slot[8] __attribute__((aligned(64))) = {0};
            for (int i = 0; i < 4; ++i)
                if (blk[i])
                    slot[i] = ReadLE64(blk[i] + w * 8);
            st[w] = _mm512_xor_si512(st[w], _mm512_load_si512((const void *)slot));
        }

        keccakf_x4(st);

        /* A lane finishes at b == blockcount[i]-1 == full_blocks[i]; its 32-byte
         * digest is words 0..3 of the state right after THIS permutation. */
        bool any_done = false;
        for (int i = 0; i < 4; ++i)
            if (full_blocks[i] == b) { any_done = true; break; }
        if (any_done) {
            uint64_t w0[8], w1[8], w2[8], w3[8];
            _mm512_store_si512((void *)w0, st[0]);
            _mm512_store_si512((void *)w1, st[1]);
            _mm512_store_si512((void *)w2, st[2]);
            _mm512_store_si512((void *)w3, st[3]);
            for (int i = 0; i < 4; ++i) {
                if (full_blocks[i] != b) continue;
                WriteLE64(out[i] + 0,  w0[i]);
                WriteLE64(out[i] + 8,  w1[i]);
                WriteLE64(out[i] + 16, w2[i]);
                WriteLE64(out[i] + 24, w3[i]);
            }
        }
    }
}

#else /* non-x86: no AVX-512 lane; dispatch always resolves to scalar. */

void sha3_256_x4_avx512(const uint8_t *const msgs[4], const size_t lens[4],
                        uint8_t out[4][32])
{
    sha3_256_x4_scalar(msgs, lens, out);
}

#endif

/* ── Dispatch ──────────────────────────────────────────────────────────
 *
 * Default: use the AVX-512 lane when the CPU supports it — this is the genuine
 * multi-stream win (see the bench in the `sha3_256_x4` test group; flip
 * SHA3_256_X4_AVX512_DEFAULT_ENABLED to 0 to ship scalar if a host measures a
 * loss). The parity oracle / bench force a path via sha3_256_x4_select_impl.
 * Setting a function pointer is not torn on any supported target; do not call
 * the selector concurrently with active batched hashing. */
#ifndef SHA3_256_X4_AVX512_DEFAULT_ENABLED
#define SHA3_256_X4_AVX512_DEFAULT_ENABLED 1
#endif

static void (*g_x4)(const uint8_t *const[4], const size_t[4], uint8_t[4][32]) =
    sha3_256_x4_scalar;
static int g_x4_inited = 0;

static void x4_init_default(void)
{
    if (SHA3_256_X4_AVX512_DEFAULT_ENABLED && sha3_keccakf_avx512_available())
        g_x4 = sha3_256_x4_avx512;
    else
        g_x4 = sha3_256_x4_scalar;
    g_x4_inited = 1;
}

int sha3_256_x4_select_impl(enum sha3_impl which)
{
    switch (which) {
    case SHA3_IMPL_SCALAR:
        g_x4 = sha3_256_x4_scalar;
        g_x4_inited = 1;
        return SHA3_IMPL_SCALAR;
    case SHA3_IMPL_AVX512:
        if (sha3_keccakf_avx512_available()) {
            g_x4 = sha3_256_x4_avx512;
            g_x4_inited = 1;
            return SHA3_IMPL_AVX512;
        }
        g_x4 = sha3_256_x4_scalar;
        g_x4_inited = 1;
        return SHA3_IMPL_SCALAR;
    case SHA3_IMPL_AUTO:
    default:
        x4_init_default();
        return (g_x4 == sha3_256_x4_avx512) ? SHA3_IMPL_AVX512 : SHA3_IMPL_SCALAR;
    }
}

void sha3_256_x4(const uint8_t *const msgs[4], const size_t lens[4],
                 uint8_t out[4][32])
{
    if (!g_x4_inited) x4_init_default();
    g_x4(msgs, lens, out);
}
