/* Copyright 2026 Rhett Creighton - Apache License 2.0 */
// one-result-type-ok:internal-reconcile-seam
// This is an internal chain-evidence reconciliation helper, not a new service
// surface. Its single fallible entrypoint already carries the failure reason with the
// failure via the reason_out[192] out-parameter (the caller freezes
// with that precise string), matching the controller's existing
// reconcile contract. struct zcl_result would not improve it.
/*
 * Active-tip evidence reconstruction — the recovery half of the chain
 * evidence controller. After a chain_restore the in-memory active tip can
 * be hash-consistent (tip_hash == coins_best_block == active_tip hash) yet
 * carry no evidence record and/or nChainWork==0. Rather than freeze a
 * recoverable tip, we RECOMPUTE what is missing and publish honest,
 * low-trust LOCAL_IMPORT evidence so the node can advance. Only a tip that
 * genuinely cannot be proven consistent freezes — always with a precise,
 * non-empty reason (the no-silent-halt mandate). Background validation
 * later upgrades trust.
 */

#include "services/chain_evidence_authority_service.h"
#include "services/chain_evidence_persistence_service.h"
#include "chain_evidence_reconstruct.h"

#include "models/database.h"
#include "chain/pow.h"
#include "core/arith_uint256.h"
#include "util/log_macros.h"

#include <stdio.h>
#include <string.h>

static bool cer_u256_equal(const struct uint256 *a, const struct uint256 *b)
{
    return a && b && memcmp(a->data, b->data, 32) == 0;
}

static bool cer_persist_i64(struct chain_evidence_controller *a,
                            const char *key, int64_t v)
{
    return a && a->ndb && node_db_state_set_int(a->ndb, key, v);
}

static bool cer_persist_blob(struct chain_evidence_controller *a,
                             const char *key, const void *value, size_t len)
{
    return a && a->ndb && node_db_state_set(a->ndb, key, value, len);
}

/* Ancestry walk: tip must be canonical in the block map and link, by
 * strictly decreasing height, to genesis (height 0 with no pprev). */
static bool cer_tip_ancestry_linked(struct chain_state_repository *csr,
                                    struct block_index *tip)
{
    if (!tip || !tip->phashBlock)
        return false;
    if (csr && csr->block_map &&
        block_map_find(csr->block_map, tip->phashBlock) != tip)
        return false;
    for (struct block_index *p = tip; p; p = p->pprev) {
        if (!p->phashBlock)
            return false;
        if (csr && csr->block_map &&
            block_map_find(csr->block_map, p->phashBlock) != p)
            return false;
        if (p->nHeight == 0)
            return p->pprev == NULL;
        if (!p->pprev || p->pprev->nHeight != p->nHeight - 1)
            return false;
    }
    return false;
}

/* Recompute nChainWork for `tip` from the deepest ancestor that already
 * carries work (the "anchor") or from genesis, accumulating GetBlockProof
 * forward. Returns false only when an ancestry link breaks before reaching
 * a worked ancestor / genesis — i.e. the chain genuinely cannot be proven.
 * On success tip->nChainWork (and zero-work ancestors above the anchor)
 * are filled in. */
static bool cer_recompute_chainwork_from_ancestry(struct block_index *tip)
{
    if (!tip)
        return false;

    enum { CER_MAX_REWORK_DEPTH = 4096 };
    struct block_index *chainbuf[CER_MAX_REWORK_DEPTH];
    size_t n = 0;
    struct block_index *base = NULL;
    for (struct block_index *p = tip; p; p = p->pprev) {
        if (n >= CER_MAX_REWORK_DEPTH)
            return false;
        chainbuf[n++] = p;
        if (!arith_uint256_is_zero(&p->nChainWork) || p->nHeight == 0) {
            base = p;          /* worked ancestor, or genesis */
            break;
        }
        if (!p->pprev)
            return false;      /* ancestry broken — cannot prove work */
    }
    if (!base || chainbuf[n - 1] != base)
        return false;

    struct block_index *anchor =
        arith_uint256_is_zero(&base->nChainWork) ? NULL : base;
    for (size_t i = n; i-- > 0;) {
        struct block_index *b = chainbuf[i];
        if (b == anchor)
            continue;          /* keep authoritative work */
        struct arith_uint256 proof = GetBlockProof(b);
        if (b->pprev)
            arith_uint256_add(&b->nChainWork, &b->pprev->nChainWork, &proof);
        else
            b->nChainWork = proof;   /* genesis */
    }
    return !arith_uint256_is_zero(&tip->nChainWork);
}

bool cec_reconstruct_active_tip_evidence(
    struct chain_evidence_controller *authority,
    struct block_index *active_tip,
    const struct chain_state_view *csv,
    char reason_out[192])
{
    if (reason_out)
        reason_out[0] = '\0';
    if (!authority || !authority->ndb || !active_tip ||
        !active_tip->phashBlock || !csv || csv->tip_height < 0) {
        if (reason_out)
            snprintf(reason_out, 192, "active_tip_reconstruct_null_arg");
        return false;
    }

    /* The block-index evidence we are about to publish proves the TIP:
     * its PoW-bearing header links to genesis (ancestry) and carries the
     * most accumulated work (chainwork). The csr active-tip hash must
     * match the in-memory tip — that IS part of the tip's identity, so a
     * mismatch is a genuine contradiction we must not publish. */
    if (!cer_u256_equal(&csv->tip_hash, active_tip->phashBlock)) {
        if (reason_out)
            snprintf(reason_out, 192,
                     "active_tip_hash != csr_tip_hash (h=%d)",
                     active_tip->nHeight);
        return false;
    }
    /* The coins (UTXO) best-block cursor is a PROJECTION that trails or,
     * on a torn restart, transiently overshoots the tip — it is NOT part
     * of the tip's block-index evidence and must NOT gate it. reconcile_
     * startup already classifies a coins/active-tip mismatch as recoverable
     * lag ("deferring to next commit"); this once contradicted that policy
     * by hard-freezing here, parking a provable tip behind its own lagging
     * cursor. Publishing the tip is correct regardless of cursor position:
     * if coins is behind it catches up on the next commit; if it overshot
     * (the BIP30 self-write wedge) connect_block's self-write tolerance
     * rewinds it. Log the divergence and proceed. */
    if (!cer_u256_equal(&csv->coins_best_block, active_tip->phashBlock)) {
        LOG_WARN("cec",
                 "[cec] reconstruct: coins_best_block cursor != active tip "
                 "h=%d — recoverable projection lag/overshoot; publishing "
                 "tip evidence and letting the cursor reconcile",
                 active_tip->nHeight);
    }

    /* Ancestry must link to genesis. After a restore it usually does; if
     * it doesn't, we cannot prove the tip — freeze with reason. */
    if (!cer_tip_ancestry_linked(authority->csr, active_tip)) {
        if (reason_out)
            snprintf(reason_out, 192,
                     "active_tip_ancestry_unlinkable (h=%d)",
                     active_tip->nHeight);
        return false;
    }

    /* Recompute chainwork if missing — a restored tip frequently carries
     * nChainWork==0 (loader hadn't propagated work). Recoverable. */
    if (arith_uint256_is_zero(&active_tip->nChainWork) &&
        !cer_recompute_chainwork_from_ancestry(active_tip)) {
        if (reason_out)
            snprintf(reason_out, 192,
                     "active_tip_chainwork_unrecoverable (h=%d)",
                     active_tip->nHeight);
        return false;
    }

    /* Honest classification: recovered from local disk, NOT P2P-selected
     * or byte-verified. Set ONLY the flags actually established (ancestry
     * + chainwork); leave block_bytes_hash_checked /
     * nakamoto_selected_best_work / utxo_sha3_verified FALSE — background
     * validation upgrades them. */
    struct chain_evidence_record reconstructed = {
        .source_class = CEC_SOURCE_CLASS_LOCAL_IMPORT,
        .publish_state = CEC_PUBLISH_LOCAL_EVIDENCE,
        .header_ancestry_linked = true,
        .chainwork_recomputed = true,
    };
    return chain_evidence_controller_mark_block_evidence(
               authority, active_tip->phashBlock, &reconstructed).ok &&
           cer_persist_blob(authority, "cec.active_tip_hash",
                            active_tip->phashBlock->data, 32) &&
           cer_persist_i64(authority, "cec.active_tip_height",
                           active_tip->nHeight) &&
           cer_persist_i64(authority, "cec.coins_best_block_height",
                           chain_evidence_clamp_coins_height_to_frontier(
                               authority, active_tip->nHeight)) &&
           cer_persist_i64(authority, "cec.utxo_max_height",
                           active_tip->nHeight) &&
           cer_persist_i64(authority, "cec.publish_state",
                           CEC_PUBLISH_LOCAL_EVIDENCE) &&
           cer_persist_i64(authority, "cec.active_tip_source_class",
                           CEC_SOURCE_CLASS_LOCAL_IMPORT) &&
           cer_persist_i64(authority, "cec.repaired_active_tip_evidence", 1) &&
           chain_evidence_store_persist(authority,
                            "cec.block_index_evidence_state",
                            &reconstructed).ok &&
           chain_evidence_store_persist(authority, "cec.active_tip_evidence",
                            &reconstructed).ok;
}
