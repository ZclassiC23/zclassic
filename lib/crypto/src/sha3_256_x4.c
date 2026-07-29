/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * 4-way batched SHA3-256: hash FOUR independent messages, produce FOUR digests.
 *
 * This is the right shape for batching many independent hashes — unlike
 * sha3_512_x4 (a keystream generator: one key/nonce across 4 counters), and
 * unlike a single-stream AVX-512 Keccak, which this tree measured at 0.70x the
 * plain C it would replace and has therefore deleted rather than carried. Here
 * the four Keccak states are interleaved across the 4 low 64-bit slots of each
 * __m512i, so theta/rho/pi/chi are embarrassingly lane-parallel (NO cross-lane
 * shuffle) — this is where a double-pumped 512-bit unit genuinely amortizes:
 * 4 hashes for ~1 permutation's front-end cost. The permutation is shared with
 * sha3_avx512.c and lives in keccak_x4_internal.h.
 *
 * The AVX-512 lane carries __attribute__((target(...))) so it compiles into the
 * x86-64-v3 baseline and is reached only when keccak_x4_available() confirms
 * avx512f/vl/dq. The scalar fallback (four sha3_256 calls) is the
 * always-available reference and the differential parity oracle (test group
 * `sha3_256_x4`) proves the AVX-512 lane is byte-for-byte identical to it.
 *
 * SHA3-256 rate = 1088 bits = 136 bytes = 17 uint64 words. Each lane may have a
 * different length; the absorb walks max(blockcount) blocks, XORing each lane's
 * block only while that lane still has one, and captures each lane's 4-word
 * digest at the permutation that follows its final (padded) block. */

#include "crypto/sha3.h"
#include "crypto/common.h"
#include "keccak_x4_internal.h"

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

        keccak_x4_permute(st);

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
    if (SHA3_256_X4_AVX512_DEFAULT_ENABLED && keccak_x4_available())
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
        if (keccak_x4_available()) {
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
