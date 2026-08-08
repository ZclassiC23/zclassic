/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: atomically reconcile wallet notes spent by canonical tx bodies. */

#include "controllers/sync_controller.h"

#include "models/database.h"
#include "models/db_txn.h"
#include "models/wallet_tx.h"
#include "primitives/transaction.h"
#include "sync_controller_internal.h"
#include "util/log_macros.h"

struct confirmed_sapling_spends_ctx {
    const struct transaction *tx;
    size_t marked;
};

static bool confirmed_sapling_spends_write(struct node_db *ndb, void *opaque)
{
    struct confirmed_sapling_spends_ctx *ctx = opaque;
    if (!ndb || !ndb->open || !ctx || !ctx->tx)
        LOG_FAIL("sync", "confirmed_sapling_spends_write: invalid context");

    DB_TXN_SCOPE(txn, ndb, "wallet.sapling_note_confirmed_spends");
    if (!txn)
        LOG_FAIL("sync",
                 "confirmed_sapling_spends_write: transaction begin failed");

    for (size_t i = 0; i < ctx->tx->num_shielded_spend; i++) {
        enum db_mark_spent_result result = db_sapling_note_mark_spent(
            ndb, ctx->tx->v_shielded_spend[i].nullifier.data,
            ctx->tx->hash.data);
        if (result == DB_MARK_SPENT_ERROR)
            LOG_FAIL("sync",
                     "confirmed_sapling_spends_write: spend %zu failed",
                     i);
        ctx->marked++;
    }
    if (!db_txn_commit(txn))
        LOG_FAIL("sync",
                 "confirmed_sapling_spends_write: transaction commit failed");
    return true;
}

bool node_db_sync_confirmed_sapling_spends(
    struct node_db *ndb, const struct transaction *tx)
{
    if (!ndb || !ndb->open || !tx || tx->num_shielded_spend == 0)
        LOG_FAIL("sync", "confirmed_sapling_spends: invalid args or no spends");
    struct confirmed_sapling_spends_ctx ctx = {
        .tx = tx,
        .marked = 0,
    };
    if (!sync_run_write(ndb, confirmed_sapling_spends_write, &ctx))
        LOG_FAIL("sync", "confirmed_sapling_spends: atomic write failed");
    if (ctx.marked != tx->num_shielded_spend)
        LOG_FAIL("sync", "confirmed_sapling_spends: marked=%zu expected=%zu",
                 ctx.marked, tx->num_shielded_spend);
    return true;
}
