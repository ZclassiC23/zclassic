/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Pedersen hash for Sapling Merkle tree — pure C23 implementation.
 * Uses Jubjub curve with BLS12-381 scalar field. */

#ifndef ZCL_SAPLING_PEDERSEN_HASH_H
#define ZCL_SAPLING_PEDERSEN_HASH_H

#include <stdint.h>
#include <stddef.h>
#include "sapling/fr.h"

/* Compute Sapling Merkle tree hash: PedersenHash(MerkleTree(depth), a || b)
 * Inputs: depth (0-62), a and b are 32-byte LE field elements.
 * Output: 32-byte LE field element (x-coordinate of resulting Jubjub point). */
void pedersen_merkle_hash(size_t depth,
                           const uint8_t a[32],
                           const uint8_t b[32],
                           uint8_t result[32]);

/* Generic Pedersen hash over pre-assembled bits (personalization already included).
 * Returns the resulting Jubjub point (not just x-coordinate). */
void pedersen_hash_bits(const uint8_t *bits, int nbits,
                         struct jub_point *result);

/* Sapling tree uncommitted value: Fr::one() = 1 as 32 bytes LE */
void sapling_uncommitted(uint8_t out[32]);

#endif
