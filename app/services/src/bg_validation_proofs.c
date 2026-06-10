/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright (c) 2014-2017 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 */
// one-result-type-ok:hot-proof-verifier
// Internal background-validation verifier, not a new service surface. A false
// return is turned into a logged per-block validation failure (with
// height/tx/index) by the bg_validation service thread; the fprintf trail
// already carries the precise failure reason.
/*
 * bg_validation_proofs: shielded zk-SNARK / Ed25519 proof verification for the
 * background full-validation pass. */

#include "bg_validation_internal.h"

#include "validation/sighash.h"
#include "validation/main_constants.h"
#include "primitives/transaction.h"
#include "crypto/ed25519.h"
#include "sapling/sprout.h"
#include "sapling/bn254.h"
#include "sapling/sapling_prover.h"
#include "core/uint256.h"
#include "util/log_macros.h"

#include <stdatomic.h>
#include <stdio.h>

/* ── Shielded proof verification for a single transaction ────── */

/* Verifies JoinSplit Ed25519 sigs, Sprout Groth16/PHGR13 proofs,
 * and Sapling spend/output proofs + binding signature.
 * Returns false on verification failure, sets *proofs_out. */
bool bg_validation_verify_shielded_proofs(const struct transaction *tx,
                                          int height, size_t tx_idx,
                                          uint32_t branch_id,
                                          int64_t *proofs_out)
{
    struct uint256 data_to_be_signed;
    uint256_set_null(&data_to_be_signed);
    struct script empty_script = { .size = 0 };
    struct sighash_type ht = { .raw = 1 }; /* SIGHASH_ALL */

    if (!signature_hash(&empty_script, tx, NOT_AN_INPUT, ht, 0,
                        branch_id, NULL, &data_to_be_signed)) {
        fprintf(stderr, "[bg-valid] sighash FAILED h=%d tx=%zu\n",
                height, tx_idx);
        return false;
    }

    /* JoinSplit Ed25519 signature */
    if (tx->num_joinsplit > 0) {
        if (!ed25519_verify(tx->joinsplit_sig, data_to_be_signed.data,
                            32, tx->joinsplit_pubkey.data)) {
            fprintf(stderr, "[bg-valid] Ed25519 JoinSplit sig FAILED "
                    "h=%d tx=%zu\n", height, tx_idx);
            return false;
        }
    }

    /* Sprout JoinSplit zk-SNARK proofs */
    for (size_t j = 0; j < tx->num_joinsplit; j++) {
        const struct js_description *js = &tx->v_joinsplit[j];
        uint8_t h_sig[32];
        sprout_h_sig(js->random_seed.data, js->nullifiers[0].data,
                     js->nullifiers[1].data, tx->joinsplit_pubkey.data,
                     h_sig);

        if (!js->use_groth) {
            /* PHGR13 proof (pre-Sapling Sprout, blocks 0-581876) */
            if (!sprout_verify_phgr13(js->proof,
                    js->anchor.data, h_sig,
                    js->macs[0].data, js->macs[1].data,
                    js->nullifiers[0].data, js->nullifiers[1].data,
                    js->commitments[0].data, js->commitments[1].data,
                    (uint64_t)js->vpub_old, (uint64_t)js->vpub_new)) {
                /* Non-fatal when VK not loaded — sprout_verify_phgr13
                 * returns false when phgr_vk==NULL. */
                static _Atomic int phgr_warn = 0;
                if (atomic_load(&phgr_warn) < 3) {
                    atomic_fetch_add(&phgr_warn, 1);
                    LOG_WARN("bg-valid", "SKIPPED h=%d tx=%zu js=%zu (VK not " "loaded)", height, tx_idx, j);
                }
                continue;
            }
        } else {
            if (!sprout_verify_groth16(js->proof,
                    js->anchor.data, h_sig,
                    js->macs[0].data, js->macs[1].data,
                    js->nullifiers[0].data, js->nullifiers[1].data,
                    js->commitments[0].data, js->commitments[1].data,
                    (uint64_t)js->vpub_old, (uint64_t)js->vpub_new)) {
                fprintf(stderr, "[bg-valid] Sprout Groth16 proof FAILED "
                        "h=%d tx=%zu js=%zu\n", height, tx_idx, j);
                return false;
            }
        }
        (*proofs_out)++;
    }

    /* Sapling spend/output proofs + binding sig */
    if (tx->num_shielded_spend == 0 && tx->num_shielded_output == 0)
        return true;

    void *sctx = zclassic_sapling_verification_ctx_init();
    if (!sctx)
        LOG_FAIL("bg_validation", "verify_shielded_proofs: sapling ctx init failed h=%d tx=%zu",
                 height, tx_idx);

    for (size_t j = 0; j < tx->num_shielded_spend; j++) {
        const struct spend_description *sd = &tx->v_shielded_spend[j];
        if (!zclassic_sapling_check_spend(
                sctx, sd->cv.data, sd->anchor.data,
                sd->nullifier.data, sd->rk.data,
                sd->zkproof, sd->spend_auth_sig,
                data_to_be_signed.data)) {
            fprintf(stderr, "[bg-valid] Sapling spend check FAILED "
                    "h=%d tx=%zu spend=%zu\n", height, tx_idx, j);
            zclassic_sapling_verification_ctx_free(sctx);
            return false;
        }
        (*proofs_out)++;
    }

    for (size_t j = 0; j < tx->num_shielded_output; j++) {
        const struct output_description *od = &tx->v_shielded_output[j];
        if (!zclassic_sapling_check_output(
                sctx, od->cv.data, od->cm.data,
                od->ephemeral_key.data, od->zkproof)) {
            fprintf(stderr, "[bg-valid] Sapling output check FAILED "
                    "h=%d tx=%zu output=%zu\n", height, tx_idx, j);
            zclassic_sapling_verification_ctx_free(sctx);
            return false;
        }
        (*proofs_out)++;
    }

    if (!zclassic_sapling_final_check(
            sctx, tx->value_balance,
            tx->binding_sig, data_to_be_signed.data)) {
        fprintf(stderr, "[bg-valid] Sapling binding sig FAILED "
                "h=%d tx=%zu\n", height, tx_idx);
        zclassic_sapling_verification_ctx_free(sctx);
        return false;
    }
    zclassic_sapling_verification_ctx_free(sctx);
    return true;
}
