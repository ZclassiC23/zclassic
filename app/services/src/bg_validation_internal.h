/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * bg_validation_internal — sibling-private declarations shared between
 * bg_validation_service.c (the thread + public API) and the script/proof
 * verification translation units. This is not a public header. */

#ifndef ZCL_BG_VALIDATION_INTERNAL_H
#define ZCL_BG_VALIDATION_INTERNAL_H

#include "script/script.h"
#include "validation/sighash.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct transaction;

/* ── Parallel script verification (bg_validation_scripts.c) ───────── */

struct script_check_item {
    const struct transaction *tx;
    unsigned int input_index;
    int64_t amount;
    uint32_t branch_id;
    struct precomputed_tx_data txdata;
    struct script script_pub_key;
    uint32_t flags;
};

/* Verify all script items in parallel using num_workers threads.
 * Falls back to serial for small batches. */
bool bg_validation_verify_scripts_parallel(struct script_check_item *items,
                                           size_t count, int num_workers);

/* ── Shielded proof verification (bg_validation_proofs.c) ─────────── */

/* Verifies JoinSplit Ed25519 sigs, Sprout Groth16/PHGR13 proofs,
 * and Sapling spend/output proofs + binding signature.
 * Returns false on verification failure, sets *proofs_out. */
bool bg_validation_verify_shielded_proofs(const struct transaction *tx,
                                          int height, size_t tx_idx,
                                          uint32_t branch_id,
                                          int64_t *proofs_out);

#endif /* ZCL_BG_VALIDATION_INTERNAL_H */
