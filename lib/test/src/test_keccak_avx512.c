/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Differential parity oracle + honest benchmark for the AVX-512 single-stream
 * Keccak-f[1600] permutation (lib/crypto/src/keccak_avx512.c).
 *
 * Consensus-critical crypto is permitted an AVX-512 fast path ONLY with a
 * differential oracle proving bit-identical output against the scalar
 * reference. This group is that oracle:
 *
 *   1. FIPS-202 known-answer vectors (SHA3-256 / SHA3-512) run through BOTH
 *      the scalar and AVX-512 permutations.
 *   2. Randomized differential hammer: every input length 0..1100 (crossing
 *      each rate boundary — 136 for SHA3-256, 72 for SHA3-512), one-shot,
 *      scalar vs AVX-512 byte-for-byte.
 *   3. Incremental-absorb differential: random split points, streaming path
 *      vs one-shot, on both permutations.
 *   4. Honest benchmark: MB/s for short (136B), medium (4KB), long (1MB)
 *      inputs on each path. Reported, never gated (perf is measured, not
 *      wished — the shipped default stays on the fastest MEASURED path).
 *
 * When the host lacks AVX-512 the differential steps degrade to scalar-vs-scalar
 * (still exercises the streaming machinery + KAT); this is reported, not failed. */

#define _POSIX_C_SOURCE 200809L

#include "test/test_helpers.h"
#include "crypto/sha3.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Deterministic xorshift64* — no libc rand, reproducible across runs. */
static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;
static uint64_t rng_next(void)
{
    uint64_t x = rng_state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}
static void rng_fill(uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; ++i) buf[i] = (uint8_t)(rng_next() >> 17);
}

static int hex2bin(const char *hex, uint8_t *out, size_t out_len)
{
    for (size_t i = 0; i < out_len; ++i) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

/* Force a permutation, returning the impl actually installed. */
static bool select_avx512(void)
{
    return sha3_select_impl(SHA3_IMPL_AVX512) == SHA3_IMPL_AVX512;
}
static void select_scalar(void) { (void)sha3_select_impl(SHA3_IMPL_SCALAR); }

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);  // platform-ok:keccak-avx512-benchmark-realtime
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int test_keccak_avx512(void)
{
    int failures = 0;
    const bool have_avx = sha3_keccakf_avx512_available();

    printf("keccak_avx512: AVX-512 permutation available on host... %s\n",
           have_avx ? "YES" : "no (differential runs scalar-vs-scalar)");

    /* ── 1. FIPS-202 known-answer vectors through BOTH paths ─────────── */
    struct { const char *msg; size_t msglen; const char *h256; const char *h512; } kat[] = {
        { "", 0,
          "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a",
          "a69f73cca23a9ac5c8b567dc185a756e97c982164fe25859e0d1dcc1475c80a6"
          "15b2123af1f5f94c11e3e9402c3ac558f500199d95b6d3e301758586281dcd26" },
        { "abc", 3,
          "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532",
          "b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e"
          "10e116e9192af3c91a7ec57647e3934057340b4cf408d5a56592f8274eec53f0" },
    };
    for (unsigned k = 0; k < sizeof(kat)/sizeof(kat[0]); ++k) {
        uint8_t want256[32], want512[64];
        hex2bin(kat[k].h256, want256, 32);
        hex2bin(kat[k].h512, want512, 64);
        for (int pass = 0; pass < 2; ++pass) {
            const char *lbl;
            if (pass == 0) { select_scalar(); lbl = "scalar"; }
            else { if (!have_avx) break; select_avx512(); lbl = "avx512"; }

            uint8_t got256[32], got512[64];
            sha3_256((const unsigned char *)kat[k].msg, kat[k].msglen, got256);
            sha3_512((const unsigned char *)kat[k].msg, kat[k].msglen, got512);
            printf("keccak_avx512: KAT[%u] SHA3-256/512 (%s)... ", k, lbl);
            if (memcmp(got256, want256, 32) == 0 && memcmp(got512, want512, 64) == 0)
                printf("OK\n");
            else { printf("FAIL\n"); failures++; }
        }
    }

    /* ── 2. Randomized differential: every length 0..1100 ───────────── */
    printf("keccak_avx512: differential SHA3-256/512, lengths 0..1100... ");
    {
        int diff_fail = 0;
        uint8_t buf[1101];
        for (size_t len = 0; len <= 1100; ++len) {
            rng_fill(buf, len);
            uint8_t s256[32], a256[32], s512[64], a512[64];
            select_scalar();
            sha3_256(buf, len, s256);
            sha3_512(buf, len, s512);
            if (have_avx) { select_avx512(); } /* else compares scalar vs scalar */
            sha3_256(buf, len, a256);
            sha3_512(buf, len, a512);
            if (memcmp(s256, a256, 32) != 0 || memcmp(s512, a512, 64) != 0) {
                if (diff_fail < 3)
                    printf("[len %zu MISMATCH] ", len);
                diff_fail++;
            }
        }
        if (diff_fail == 0) printf("OK\n");
        else { printf("FAIL (%d mismatches)\n", diff_fail); failures++; }
    }

    /* ── 3. Incremental-absorb differential (random split points) ────── */
    printf("keccak_avx512: incremental absorb vs one-shot (both paths)... ");
    {
        int inc_fail = 0;
        uint8_t buf[2048];
        for (int trial = 0; trial < 4000; ++trial) {
            size_t len = (size_t)(rng_next() % 2000) + 1;
            rng_fill(buf, len);
            for (int pass = 0; pass < 2; ++pass) {
                if (pass == 1 && !have_avx) break;
                if (pass == 0) select_scalar(); else select_avx512();

                uint8_t oneshot[32], streamed[32];
                sha3_256(buf, len, oneshot);

                struct sha3_256_ctx ctx;
                sha3_256_init(&ctx);
                size_t off = 0;
                while (off < len) {
                    size_t chunk = (size_t)(rng_next() % 200);
                    if (chunk == 0) chunk = 1;
                    if (off + chunk > len) chunk = len - off;
                    sha3_256_write(&ctx, buf + off, chunk);
                    off += chunk;
                }
                sha3_256_finalize(&ctx, streamed);
                if (memcmp(oneshot, streamed, 32) != 0) inc_fail++;
            }
        }
        if (inc_fail == 0) printf("OK\n");
        else { printf("FAIL (%d)\n", inc_fail); failures++; }
    }

    /* ── 4. Honest benchmark (MB/s per path; informational) ──────────── */
    {
        static const size_t sizes[] = { 136, 4096, 1u << 20 };
        static const char  *names[] = { "short(136B)", "medium(4KB)", "long(1MB)" };
        /* Total bytes hashed per measurement, tuned so each is ~0.1-0.3s. */
        static const size_t total_mb[] = { 64, 256, 512 };
        uint8_t *buf = (uint8_t *)malloc(1u << 20);
        if (!buf) { printf("keccak_avx512: bench alloc failed\n"); failures++; }
        else {
            rng_fill(buf, 1u << 20);
            uint8_t out[32];
            printf("keccak_avx512: --- benchmark (SHA3-256 throughput) ---\n");
            for (int s = 0; s < 3; ++s) {
                size_t insz = sizes[s];
                size_t iters = (total_mb[s] << 20) / insz;
                if (iters == 0) iters = 1;
                double mb_scalar = 0, mb_avx = 0;
                for (int pass = 0; pass < 2; ++pass) {
                    if (pass == 1 && !have_avx) break;
                    if (pass == 0) select_scalar(); else select_avx512();
                    /* warm */
                    for (int w = 0; w < 8; ++w) sha3_256(buf, insz, out);
                    double t0 = now_s();
                    for (size_t i = 0; i < iters; ++i) sha3_256(buf, insz, out);
                    double dt = now_s() - t0;
                    double mbps = ((double)insz * (double)iters) / (1024.0*1024.0) / dt;
                    if (pass == 0) mb_scalar = mbps; else mb_avx = mbps;
                }
                if (have_avx)
                    printf("keccak_avx512:   %-11s  scalar %8.1f MB/s   avx512 %8.1f MB/s   %.2fx\n",
                           names[s], mb_scalar, mb_avx, mb_avx / mb_scalar);
                else
                    printf("keccak_avx512:   %-11s  scalar %8.1f MB/s\n", names[s], mb_scalar);
            }
            printf("keccak_avx512: --- shipped default: scalar (see sha3.c dispatch) ---\n");
            free(buf);
        }
    }

    /* Restore the shipped default for any subsequent in-process hashing. */
    (void)sha3_select_impl(SHA3_IMPL_AUTO);

    printf("keccak_avx512: %d failure(s)\n", failures);
    return failures;
}
