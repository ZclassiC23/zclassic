/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Sapling spend and output circuit synthesis.
 * Generates R1CS constraints and witness for Groth16 proving. */

#ifndef ZCL_SAPLING_SAPLING_CIRCUIT_H
#define ZCL_SAPLING_SAPLING_CIRCUIT_H

#include "sapling/groth16_prover.h"
#include "sapling/fr.h"
#include <stdint.h>
#include <stdbool.h>

#define SAPLING_MERKLE_DEPTH 32

/* ── Spend Circuit ──────────────────────────────────────────────── */

/* Private witness for a Sapling spend */
struct sapling_spend_witness {
    /* Spending key components */
    uint8_t ak[32];           /* spend validating key (Jubjub compressed) */
    uint8_t nsk[32];          /* nullifier private key (Fs scalar) */

    /* Re-randomization */
    uint8_t ar[32];           /* randomness for rk = ak + ar*G */

    /* Note being spent */
    uint64_t value;           /* note value in zatoshis */
    uint8_t diversifier[11];  /* payment address diversifier */
    uint8_t pk_d[32];         /* diversified transmission key */
    uint8_t rcm[32];          /* note commitment randomness (Fs) */

    /* Merkle tree proof */
    uint8_t auth_path[SAPLING_MERKLE_DEPTH][32]; /* sibling hashes */
    bool auth_path_bits[SAPLING_MERKLE_DEPTH];   /* position bits */

    /* Value commitment randomness */
    uint8_t rcv[32];          /* value commitment randomness (Fs) */
};

/* Public inputs for a Sapling spend (computed from witness) */
struct sapling_spend_inputs {
    uint8_t rk[32];           /* randomized verification key */
    uint8_t cv[32];           /* value commitment */
    uint8_t anchor[32];       /* Merkle root */
    uint8_t nullifier[32];    /* nullifier */
};

/* Synthesize a Sapling spend circuit.
 * Populates cs with all constraints and witness values.
 * Returns true if synthesis succeeds. */
bool sapling_spend_synthesize(struct constraint_system *cs,
                               const struct sapling_spend_witness *wit,
                               const struct sapling_spend_inputs *pub);

/* Parse a caller-supplied Sapling merkle authentication path into
 * the auth_path / auth_path_bits fields of `wit`. Wire layout:
 *     depth (1) || 32 × (sibling (32) || bit (1))  = 1057 bytes
 * Returns false if `witness_len` is below the fixed 1057-byte layout
 * or the depth byte is not 32. The bounds check is load-bearing:
 * callers route untrusted-length buffers (RPC JSON, wallet blobs,
 * fuzz harnesses) into this path. */
bool sapling_spend_parse_witness(const uint8_t *witness,
                                  size_t witness_len,
                                  struct sapling_spend_witness *wit);

/* ── Output Circuit ─────────────────────────────────────────────── */

/* Private witness for a Sapling output */
struct sapling_output_witness {
    uint64_t value;           /* note value in zatoshis */
    uint8_t diversifier[11];  /* recipient diversifier */
    uint8_t pk_d[32];         /* recipient diversified transmission key */
    uint8_t rcm[32];          /* note commitment randomness (Fs) */
    uint8_t esk[32];          /* ephemeral secret key (Fs) */
    uint8_t rcv[32];          /* value commitment randomness (Fs) */
};

/* Public inputs for a Sapling output */
struct sapling_output_inputs {
    uint8_t cv[32];           /* value commitment */
    uint8_t epk[32];          /* ephemeral public key */
    uint8_t cm[32];           /* note commitment */
};

/* Synthesize a Sapling output circuit. */
bool sapling_output_synthesize(struct constraint_system *cs,
                                const struct sapling_output_witness *wit,
                                const struct sapling_output_inputs *pub);

/* ── Full Proving Interface ─────────────────────────────────────── */

/* Create a Sapling spend proof.
 * Loads proving key, synthesizes circuit, generates proof.
 * pk_data/pk_len: raw sapling-spend.params file contents.
 * Returns proof as 192 bytes (A:48 + B:96 + C:48 compressed). */
bool sapling_create_spend_proof(const uint8_t *pk_data, size_t pk_len,
                                 const struct sapling_spend_witness *wit,
                                 const struct sapling_spend_inputs *pub,
                                 uint8_t proof_out[192]);

/* Create a Sapling output proof. */
bool sapling_create_output_proof(const uint8_t *pk_data, size_t pk_len,
                                  const struct sapling_output_witness *wit,
                                  const struct sapling_output_inputs *pub,
                                  uint8_t proof_out[192]);

#endif
