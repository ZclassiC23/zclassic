/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_apply_nullifiers — implementation. See jobs/utxo_apply_nullifiers.h.
 *
 * The reducer's port of zclassicd's shielded double-spend gate (C-3),
 * extracted from utxo_apply_stage.c along the utxo_apply_delta*.c seam.
 * Writes only the `nullifiers` table (via storage/nullifier_kv) inside the
 * caller's stage transaction. */

#include "jobs/utxo_apply_nullifiers.h"
#include "jobs/utxo_apply_delta.h"

#include "primitives/block.h"
#include "primitives/transaction.h"
#include "storage/nullifier_kv.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NF_SUBSYS "utxo_apply"

/* One entry of the per-block nullifier accumulator: the pass-1 same-block
 * check set and the pass-2 insert list. pool 0 = Sprout, 1 = Sapling —
 * SEPARATE namespaces (zclassicd keeps distinct per-pool maps,
 * coins.cpp:166-180; see storage/nullifier_kv.h). */
struct nf_entry {
    uint8_t nf[32];
    int pool;
};

static bool nf_seen(const struct nf_entry *arr, size_t n,
                    const uint8_t nf[32], int pool)
{
    for (size_t i = 0; i < n; i++)
        if (arr[i].pool == pool && memcmp(arr[i].nf, nf, 32) == 0)
            return true;
    return false;
}

/* Check one nullifier against the durable set + the EARLIER-tx accumulator,
 * then append it. Returns false on a STORE error (caller fails closed);
 * a consensus hit flips summary->ok with zclassicd's exact reject string. */
static bool nf_check_one(sqlite3 *db, const uint8_t nf[32], int pool,
                         const struct uint256 *txid, size_t tx_first,
                         struct nf_entry *acc, size_t *n,
                         struct delta_summary *summary)
{
    bool found = false;
    if (!nullifier_kv_get(db, nf, pool, &found, NULL))
        return false;  /* store error already logged by nullifier_kv */
    /* zclassicd per-tx check-then-set order (main.cpp:2627
     * HaveShieldedRequirements, then SetNullifiers in UpdateCoins): a tx is
     * checked against the durable set AND the nullifiers of EARLIER txs of
     * this block ([0, tx_first)), never its own (same-tx duplicates are
     * rejected upstream by tx_structural, matching CheckTransaction). */
    if (found || nf_seen(acc, tx_first, nf, pool)) {
        summary->ok = false;
        summary->status = "shielded_double_spend";
        summary->failure_kind = "bad-txns-joinsplit-requirements-not-met";
        memset(summary->failure_detail, 0, sizeof(summary->failure_detail));
        memcpy(summary->failure_detail, txid->data, 32);
        return true;   /* consensus reject — nothing was inserted */
    }
    memcpy(acc[*n].nf, nf, 32);
    acc[*n].pool = pool;
    (*n)++;
    return true;
}

/* NOT inside utxo_apply_compute_block_delta on purpose: repair dry-runs call
 * the delta builder in throwaway txns where in-range nullifier rows still
 * exist and would false-fail an otherwise-valid repair. TWO-PASS so a
 * rejected block never leaves partial rows even before the txn rollback:
 * pass 1 checks every nullifier of every tx (durable set + earlier-tx
 * accumulator); pass 2 inserts only if the whole block is clean. */
bool utxo_apply_check_and_insert_nullifiers(struct sqlite3 *db,
                                            const struct block *blk,
                                            int height,
                                            struct delta_summary *summary)
{
    size_t total = 0;
    for (size_t ti = 0; ti < blk->num_vtx; ti++) {
        const struct transaction *tx = &blk->vtx[ti];
        total += tx->num_joinsplit * ZC_NUM_JS_INPUTS;
        total += tx->num_shielded_spend;
    }
    if (total == 0)
        return true;

    struct nf_entry *acc =
        zcl_calloc(total, sizeof(*acc), "utxo_apply_nullifiers");
    if (!acc) {
        LOG_WARN(NF_SUBSYS,
                 "[utxo_apply] nullifier accumulator alloc failed h=%d "
                 "(%zu entries)", height, total);
        return false;
    }

    size_t n = 0;
    for (size_t ti = 0; ti < blk->num_vtx; ti++) {
        const struct transaction *tx = &blk->vtx[ti];
        size_t tx_first = n;   /* entries below this are EARLIER txs */
        for (size_t i = 0; i < tx->num_joinsplit; i++) {
            for (size_t j = 0; j < ZC_NUM_JS_INPUTS; j++) {
                if (!nf_check_one(db,
                                  tx->v_joinsplit[i].nullifiers[j].data,
                                  NULLIFIER_POOL_SPROUT, &tx->hash,
                                  tx_first, acc, &n, summary)) {
                    free(acc);
                    return false;
                }
                if (!summary->ok) { free(acc); return true; }
            }
        }
        for (size_t i = 0; i < tx->num_shielded_spend; i++) {
            if (!nf_check_one(db,
                              tx->v_shielded_spend[i].nullifier.data,
                              NULLIFIER_POOL_SAPLING, &tx->hash,
                              tx_first, acc, &n, summary)) {
                free(acc);
                return false;
            }
            if (!summary->ok) { free(acc); return true; }
        }
    }

    /* Pass 2 — the whole block is clean: reveal every nullifier at this
     * height. Permanent consensus state, like coins (never pruned). */
    for (size_t i = 0; i < n; i++) {
        if (!nullifier_kv_add(db, acc[i].nf, acc[i].pool, (int64_t)height)) {
            free(acc);
            return false;  /* store error already logged; txn rolls back */
        }
    }
    free(acc);
    return true;
}

void utxo_apply_nullifier_gap_blocker_refresh(struct sqlite3 *db)
{
    char buf[24] = {0};
    size_t len = 0;
    bool found = false;

    if (!db) {
        LOG_WARN(NF_SUBSYS,
                 "[utxo_apply] gap blocker refresh: NULL db handle");
        return;
    }
    if (!progress_meta_get(db, "nullifier_kv.activation_cursor",
                           buf, sizeof(buf) - 1, &len, &found)) {
        LOG_WARN(NF_SUBSYS,
                 "[utxo_apply] nullifier activation marker read failed — "
                 "gap blocker not refreshed");
        return;
    }
    long long act = found ? strtoll(buf, NULL, 10) : 0;
    if (act <= 0) {
        /* 0 == enforcement covers all history (from-genesis replay);
         * absent == marker never stamped on a virgin store. No gap. */
        blocker_clear(UTXO_APPLY_NF_GAP_BLOCKER_ID);
        return;
    }

    struct blocker_record rec;
    char reason[BLOCKER_REASON_MAX];
    snprintf(reason, sizeof(reason),
             "nullifier set is EMPTY at/below activation cursor %lld: a "
             "shielded double-spend of any note revealed there is accepted "
             "here but rejected by zclassicd "
             "(bad-txns-joinsplit-requirements-not-met). Closure needs a "
             "shielded-history backfill or a from-genesis replay "
             "(storage/nullifier_kv.h ACTIVATION GAP).", act);
    /* PERMANENT: no retry fixes this — only an owner-gated backfill (or a
     * resync) does, so it must stay visible until an operator clears it. */
    if (!blocker_init(&rec, UTXO_APPLY_NF_GAP_BLOCKER_ID, NF_SUBSYS,
                      BLOCKER_PERMANENT, reason))
        return;   /* blocker_init logged the overflow */
    blocker_set(&rec);  /* -1 (cap exhausted) already logged by blocker_set */
}
