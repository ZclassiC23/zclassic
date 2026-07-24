/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Groth16 verify timing: naive public-input scalar-mul vs the precomputed
 * fixed-base tables. Both paths run in ONE process against the SAME synthetic
 * verifying key, so the reported ratio is a direct before/after and needs no
 * rebuild, no checkout, and no live node.
 *
 * The public-input count is what the Sapling consensus circuits actually use —
 * 7 for SPEND (sapling.c:664), 5 for OUTPUT (sapling.c:749) — and verify cost
 * is 4 pairings plus one scalar-mul per non-zero input regardless of which
 * curve points the bases are, so a synthetic key measures the real shape.
 *
 * Verdicts are asserted equal on every iteration: a timing run that silently
 * diverged would be worthless. Correctness itself is gated by
 * make check-groth16-parity and test_groth16_msm_parity.
 */

#include "sapling/bls12_381.h"
#include "util/log_level.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const uint8_t G1_GEN_COMPRESSED[48] = {
    0x97,0xf1,0xd3,0xa7,0x31,0x97,0xd7,0x94,0x26,0x95,0x63,0x8c,
    0x4f,0xa9,0xac,0x0f,0xc3,0x68,0x8c,0x4f,0x97,0x74,0xb9,0x05,
    0xa1,0x4e,0x3a,0x3f,0x17,0x1b,0xac,0x58,0x6c,0x55,0xe8,0x3f,
    0xf9,0x7a,0x1a,0xef,0xfb,0x3a,0xf0,0x0a,0xdb,0x22,0xc6,0xbb
};
static const uint8_t G2_GEN_COMPRESSED[96] = {
    0x93,0xe0,0x2b,0x60,0x52,0x71,0x9f,0x60,0x7d,0xac,0xd3,0xa0,
    0x88,0x27,0x4f,0x65,0x59,0x6b,0xd0,0xd0,0x99,0x20,0xb6,0x1a,
    0xb5,0xda,0x61,0xbb,0xdc,0x7f,0x50,0x49,0x33,0x4c,0xf1,0x12,
    0x13,0x94,0x5d,0x57,0xe5,0xac,0x7d,0x05,0x5d,0x04,0x2b,0x7e,
    0x02,0x4a,0xa2,0xb2,0xf0,0x8f,0x0a,0x91,0x26,0x08,0x05,0x27,
    0x2d,0xc5,0x10,0x51,0xc6,0xe4,0x7a,0xd4,0xfa,0x40,0x3b,0x02,
    0xb4,0x51,0x0b,0x64,0x7a,0xe3,0xd1,0x77,0x0b,0xac,0x03,0x26,
    0xa8,0x05,0xbb,0xef,0xd4,0x80,0x56,0xc8,0xc1,0x21,0xbd,0xb8
};

#define MAX_K 8

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Full-width, non-trivial public inputs: a small scalar would understate the
 * naive path, which always walks all 256 bits. */
static void fill_inputs(uint64_t (*inputs)[4], size_t k)
{
    for (size_t i = 0; i < k; i++) {
        inputs[i][0] = 0x9E3779B97F4A7C15ULL ^ (uint64_t)(i + 1);
        inputs[i][1] = 0xBF58476D1CE4E5B9ULL + (uint64_t)i;
        inputs[i][2] = 0x94D049BB133111EBULL ^ (uint64_t)(i * 7 + 3);
        inputs[i][3] = 0x0339d80809a1d805ULL + (uint64_t)i;  /* < r */
    }
}

static int bench_one(const char *name, size_t k, int iters)
{
    struct g1_point G1, O1, ic[MAX_K + 1];
    struct g2_point G2;
    if (!g1_from_compressed(&G1, G1_GEN_COMPRESSED) ||
        !g2_from_compressed(&G2, G2_GEN_COMPRESSED)) {
        fprintf(stderr, "generator decode failed\n");
        return 1;
    }
    g1_identity(&O1);
    ic[0] = O1;
    for (size_t i = 1; i <= k; i++) {
        uint64_t m[4] = { (uint64_t)(7 * i + 1), 0, 0, 0 };
        g1_scalar_mul(&ic[i], &G1, m);
    }

    struct groth16_vk vk = {0};
    vk.alpha_g1 = G1;
    vk.beta_g2 = G2; vk.gamma_g2 = G2; vk.delta_g2 = G2;
    vk.ic = ic; vk.ic_len = k + 1;

    uint64_t inputs[MAX_K][4];
    fill_inputs(inputs, k);

    /* C = -vk_x makes the proof verify, so both paths run the full pairing
     * check rather than short-circuiting on an early reject. */
    struct g1_point vk_x = ic[0];
    for (size_t i = 0; i < k; i++) {
        struct g1_point term;
        g1_scalar_mul(&term, &ic[i + 1], inputs[i]);
        g1_add(&vk_x, &vk_x, &term);
    }
    struct groth16_proof pr;
    pr.a = G1; pr.b = G2;
    g1_neg(&pr.c, &vk_x);

    /* Naive path. */
    vk.ic_combs = NULL;
    if (!groth16_verify(&vk, &pr, (const uint64_t (*)[4])inputs, k)) {
        fprintf(stderr, "%s: naive path rejected a valid proof\n", name);
        return 1;
    }
    uint64_t t0 = now_ns();
    for (int i = 0; i < iters; i++)
        if (!groth16_verify(&vk, &pr, (const uint64_t (*)[4])inputs, k)) {
            fprintf(stderr, "%s: naive verdict flipped mid-run\n", name);
            return 1;
        }
    uint64_t naive_ns = (now_ns() - t0) / (uint64_t)iters;

    /* Fixed-base path (table build is one-time at VK load, excluded). */
    uint64_t b0 = now_ns();
    if (!groth16_vk_build_combs(&vk)) {
        fprintf(stderr, "%s: build_combs failed\n", name);
        return 1;
    }
    uint64_t build_ns = now_ns() - b0;

    if (!groth16_verify(&vk, &pr, (const uint64_t (*)[4])inputs, k)) {
        fprintf(stderr, "%s: fixed-base path rejected a proof the naive path "
                        "accepted — CONSENSUS BREAK\n", name);
        groth16_vk_free_combs(&vk);
        return 1;
    }
    t0 = now_ns();
    for (int i = 0; i < iters; i++)
        if (!groth16_verify(&vk, &pr, (const uint64_t (*)[4])inputs, k)) {
            fprintf(stderr, "%s: fixed-base verdict flipped mid-run — "
                            "CONSENSUS BREAK\n", name);
            groth16_vk_free_combs(&vk);
            return 1;
        }
    uint64_t comb_ns = (now_ns() - t0) / (uint64_t)iters;
    groth16_vk_free_combs(&vk);

    printf("%-22s k=%zu  naive %8.3f ms   fixed-base %8.3f ms   "
           "%.2fx  (-%.1f%%)   one-time table build %.1f ms\n",
           name, k, naive_ns / 1e6, comb_ns / 1e6,
           (double)naive_ns / (double)comb_ns,
           100.0 * (1.0 - (double)comb_ns / (double)naive_ns),
           build_ns / 1e6);
    return 0;
}

int main(int argc, char **argv)
{
    zcl_log_level_set(ZCL_LOG_OFF);
    int iters = (argc > 1) ? atoi(argv[1]) : 30;
    if (iters < 1) iters = 1;
    int rc = 0;
    rc |= bench_one("sapling OUTPUT verify", 5, iters);
    rc |= bench_one("sapling SPEND verify", 7, iters);
    return rc;
}
