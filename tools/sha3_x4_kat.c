/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * sha3_x4_kat — known-answer test for the 4-way SHA3-512 keystream generator
 * sha3_512_x4() (lib/crypto/src/sha3_avx512.c), used by the file-market frame
 * cipher (lib/net/src/file_service.c).
 *
 * Why this exists: the AVX-512 path and the scalar fallback are SEPARATE
 * implementations selected at compile time by __AVX512F__. The default node
 * build targets x86-64-v3 (no AVX-512), so the test suite (test_parallel) only
 * ever links the scalar fallback and cannot exercise the AVX-512 path. A
 * SHA3-256-shaped padding bug in the AVX-512 routine therefore shipped
 * undetected — an AVX-512 build computed a non-SHA3 keystream and could not
 * exchange frames with a scalar build.
 *
 * This standalone harness is compiled WITH -mavx512f by `make test-sha3-x4`,
 * forcing the AVX-512 path, and asserts:
 *   (1) the scalar SHA3-512 matches the RFC/NIST empty-string vector (anchor);
 *   (2) sha3_512_x4 lane i == scalar SHA3-512(key||nonce||(base+i)) for many
 *       inputs — i.e. the AVX-512 keystream is byte-identical to the fallback.
 *
 * It is NOT in any build glob (tools/ root is not wildcarded into the node or
 * test binaries) so it never reaches the -march=x86-64-v3 compile that would
 * reject the AVX-512 intrinsics. Run it via `make test-sha3-x4` (SKIPs cleanly
 * on a host without AVX-512). */
#include "crypto/sha3.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int eq(const uint8_t *a, const uint8_t *b, size_t n) { return memcmp(a, b, n) == 0; }
static void hex(const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) printf("%02x", b[i]); }

int main(void)
{
    int fails = 0;

    /* (1) Anchor the scalar SHA3-512 against the NIST/RFC empty-string vector. */
    static const uint8_t empty_kat[64] = {
        0xa6,0x9f,0x73,0xcc,0xa2,0x3a,0x9a,0xc5,0xc8,0xb5,0x67,0xdc,0x18,0x5a,0x75,0x6e,
        0x97,0xc9,0x82,0x16,0x4f,0xe2,0x58,0x59,0xe0,0xd1,0xdc,0xc1,0x47,0x5c,0x80,0xa6,
        0x15,0xb2,0x12,0x3a,0xf1,0xf5,0xf9,0x4c,0x11,0xe3,0xe9,0x40,0x2c,0x3a,0xc5,0x58,
        0xf5,0x00,0x19,0x9d,0x95,0xb6,0xd3,0xe3,0x01,0x75,0x85,0x86,0x28,0x1d,0xcd,0x26 };
    uint8_t got[64];
    zcl_sha3_512((const unsigned char *)"", 0, got);
    if (!eq(got, empty_kat, 64)) {
        printf("FAIL: scalar SHA3-512(\"\") mismatch\n  got "); hex(got, 64); printf("\n");
        fails++;
    } else {
        printf("OK: scalar SHA3-512(\"\") matches the NIST vector\n");
    }

    /* (2) sha3_512_x4 must equal 4x scalar SHA3-512 over the exact 72-byte input
     *     layout key(32)||nonce(32)||counter_le(8) that file_service.c feeds it. */
    uint8_t key[32], nonce[32];
    for (int i = 0; i < 32; i++) { key[i] = (uint8_t)(0x11 * (i + 1)); nonce[i] = (uint8_t)(0xA5 ^ i); }

    const uint64_t bases[] = { 0, 1, 7, 0x00000000FFFFFFFFull, 0x0123456789ABCDEFull };
    const size_t nb = sizeof(bases) / sizeof(bases[0]);
    for (size_t b = 0; b < nb; b++) {
        uint64_t base = bases[b];
        uint8_t out4[256];
        sha3_512_x4(key, nonce, base, out4);
        for (int lane = 0; lane < 4; lane++) {
            uint8_t in[72];
            memcpy(in, key, 32);
            memcpy(in + 32, nonce, 32);
            uint64_t ctr = base + (uint64_t)lane;
            memcpy(in + 64, &ctr, 8);
            uint8_t exp[64];
            zcl_sha3_512(in, 72, exp);
            if (!eq(out4 + lane * 64, exp, 64)) {
                printf("FAIL: base=%llu lane=%d\n  x4 ", (unsigned long long)base, lane);
                hex(out4 + lane * 64, 64); printf("\n  sc "); hex(exp, 64); printf("\n");
                fails++;
            }
        }
    }
    if (!fails)
        printf("OK: sha3_512_x4 == 4x scalar SHA3-512 over %zu bases x 4 lanes\n", nb);

    printf("\n=== sha3_x4 KAT %s ===\n", fails ? "FAILED" : "PASSED");
    return fails ? 1 : 0;
}
