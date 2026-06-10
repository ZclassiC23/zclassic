/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Pedersen hash for Sapling Merkle tree — pure C23 implementation.
 * Windowed scalar multiplication on Jubjub generators with BLS12-381 Fr. */

#include "sapling/pedersen_hash.h"
#include "sapling/sapling.h"
#include "sapling/fr.h"
#include <pthread.h>
#include <string.h>

#ifdef ZCL_TESTING
#include <stdatomic.h>
#endif

#define PEDERSEN_CHUNKS_PER_GENERATOR 63
#define PEDERSEN_NUM_GENERATORS 6

static struct jub_point cached_generators[PEDERSEN_NUM_GENERATORS];
static pthread_once_t generators_once = PTHREAD_ONCE_INIT;

#ifdef ZCL_TESTING
/* Observability for concurrent-first-caller race. Post-fix the
 * pthread_once guarantees exactly one execution of the init body. */
_Atomic int zcl_pedersen_generators_body_runs_for_test = 0;

void zcl_pedersen_generators_reset_for_test(void)
{
    /* Reassigning a pthread_once_t is not specified by POSIX but is
     * the canonical test-only trick on glibc (the type is a plain int
     * with PTHREAD_ONCE_INIT == 0). Only safe to call when no other
     * thread is racing — the race tests join all workers first. */
    generators_once = (pthread_once_t)PTHREAD_ONCE_INIT;
    memset(cached_generators, 0, sizeof(cached_generators));
    atomic_store(&zcl_pedersen_generators_body_runs_for_test, 0);
}
#endif

/* Derive Pedersen hash generators via find_group_hash("Zcash_PH", index).
 * The tag is the 4-byte LE segment index followed by a counter byte. */
static void load_generators(void)
{
#ifdef ZCL_TESTING
    atomic_fetch_add(&zcl_pedersen_generators_body_runs_for_test, 1);
#endif

    const uint8_t pers[8] = {'Z','c','a','s','h','_','P','H'};

    for (int i = 0; i < PEDERSEN_NUM_GENERATORS; i++) {
        /* Tag: 4-byte LE segment index + counter byte */
        uint8_t tag[5];
        tag[0] = (uint8_t)(i & 0xff);
        tag[1] = (uint8_t)((i >> 8) & 0xff);
        tag[2] = (uint8_t)((i >> 16) & 0xff);
        tag[3] = (uint8_t)((i >> 24) & 0xff);

        /* Try counter values until group_hash succeeds */
        for (int c = 0; c < 256; c++) {
            tag[4] = (uint8_t)c;
            if (group_hash(&cached_generators[i], tag, 5, pers))
                break;
        }
    }
}

static void ensure_generators(void)
{
    pthread_once(&generators_once, load_generators);
}


/* Core Pedersen hash over pre-assembled bits (personalization already included) */
void pedersen_hash_bits(const uint8_t *bits, int nbits,
                         struct jub_point *result_pt)
{
    ensure_generators();
    jub_identity(result_pt);

    int bit_pos = 0;
    for (int seg = 0; seg < PEDERSEN_NUM_GENERATORS && bit_pos < nbits; seg++) {
        /* Accumulate scalar in Fs (Jubjub scalar field order), NOT Fr. */
        struct fs acc, cur, tmp;
        fs_zero(&acc);
        fs_one(&cur);

        bool encountered = false;
        for (int chunk = 0; chunk < PEDERSEN_CHUNKS_PER_GENERATOR; chunk++) {
            if (bit_pos >= nbits) break;
            encountered = true;

            uint8_t a_bit = bits[bit_pos++];
            uint8_t b_bit = (bit_pos < nbits) ? bits[bit_pos++] : 0;
            uint8_t c_bit = (bit_pos < nbits) ? bits[bit_pos++] : 0;

            tmp = cur;
            if (a_bit) fs_add(&tmp, &tmp, &cur);
            fs_add(&cur, &cur, &cur);
            if (b_bit) fs_add(&tmp, &tmp, &cur);
            if (c_bit) fs_neg(&tmp, &tmp);
            fs_add(&acc, &acc, &tmp);

            if (chunk < PEDERSEN_CHUNKS_PER_GENERATOR - 1) {
                fs_add(&cur, &cur, &cur);
                fs_add(&cur, &cur, &cur);
                fs_add(&cur, &cur, &cur);
            }
        }

        if (!encountered) break;

        if (!fs_is_zero(&acc)) {
            uint8_t scalar_bytes[32];
            fs_to_bytes(scalar_bytes, &acc);

            struct jub_point scaled;
            jub_scalar_mul(&scaled, &cached_generators[seg], scalar_bytes);
            jub_add(result_pt, result_pt, &scaled);
        }
    }
}

void pedersen_merkle_hash(size_t depth,
                           const uint8_t a[32],
                           const uint8_t b[32],
                           uint8_t result[32])
{
    /* Extract bits: 6 personalization + 255 from a + 255 from b = 516 bits */
    uint8_t bits[516];
    int nbits = 0;

    /* Personalization: depth as 6 LE bits */
    for (int i = 0; i < 6; i++)
        bits[nbits++] = (depth >> i) & 1;

    /* a: 255 bits, LE (bit 0 of byte 0 first) */
    for (int i = 0; i < 255; i++)
        bits[nbits++] = (a[i / 8] >> (i % 8)) & 1;

    /* b: 255 bits, LE */
    for (int i = 0; i < 255; i++)
        bits[nbits++] = (b[i / 8] >> (i % 8)) & 1;

    struct jub_point result_pt;
    pedersen_hash_bits(bits, nbits, &result_pt);

    struct fr x_coord;
    jub_get_x(&x_coord, &result_pt);
    fr_to_bytes(result, &x_coord);
}

void sapling_uncommitted(uint8_t out[32])
{
    memset(out, 0, 32);
    out[0] = 1;
}
