/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: reconcile durable transaction intents with chain and expiry state. */
// repair-rung-ok:test_wallet_funds_safety

#include "controllers/vault_intent_controller.h"

#include "controllers/wallet_helpers.h"
#include "chain/chain.h"
#include "models/vault_intent.h"
#include "util/log_macros.h"
#include "validation/contextual_check_tx.h"
#include "validation/main_state.h"
#include "wallet/wallet.h"

#include <string.h>

void vault_intent_refresh_state(struct wallet_rpc_context *ctx,
                                struct vault_intent_row *row, int64_t now)
{
    if (!ctx || !row || !row->has_txid)
        return;
    struct uint256 txid;
    memcpy(txid.data, row->txid, sizeof(row->txid));
    const struct wallet_tx *wtx = wallet_get_tx(ctx->wallet, &txid);
    int32_t height = -1;
    int32_t confirmations = 0;
    if (wtx && !uint256_is_null(&wtx->hash_block) &&
        vault_intent_chain_confirmation(ctx->main_state,
            wtx->hash_block.data, &height, &confirmations)) {
        enum vault_intent_state state = confirmations >= 6
            ? VAULT_INTENT_FINALIZED : VAULT_INTENT_CONFIRMED;
        if (vault_intent_set_confirmation(ctx->node_db, row->plan_id, state,
                height, wtx->hash_block.data, now))
            (void)vault_intent_find(ctx->node_db, row->plan_id, row);
        return;
    }
    if (wtx && row->state == VAULT_INTENT_MEMPOOL_ACCEPTED &&
        is_expired_tx(&wtx->tx,
            active_chain_height(&ctx->main_state->chain_active) + 1)) {
        struct wallet_tx expired;
        memset(&expired, 0, sizeof(expired));
        transaction_init(&expired.tx);
        if (!transaction_copy(&expired.tx, &wtx->tx)) {
            LOG_ERROR("vault_intent",
                      "expired transaction copy failed; reservation retained");
            transaction_free(&expired.tx);
            return;
        }
        expired.used = true;
        struct zcl_result rolled =
            wallet_rollback_persisted_commit(ctx, &expired);
        transaction_free(&expired.tx);
        if (!rolled.ok) {
            LOG_ERROR("vault_intent",
                      "expired transaction rollback failed (code=%d): %s",
                      rolled.code, rolled.message);
            return;
        }
        if (vault_intent_set_state(ctx->node_db, row->plan_id,
                VAULT_INTENT_EXPIRED, row->txid,
                "TX_EXPIRED_UNCONFIRMED", now))
            (void)vault_intent_find(ctx->node_db, row->plan_id, row);
        return;
    }
    if (row->state == VAULT_INTENT_CONFIRMED ||
        row->state == VAULT_INTENT_FINALIZED) {
        if (vault_intent_set_state(ctx->node_db, row->plan_id,
                VAULT_INTENT_REORGED, row->txid,
                "CONFIRMATION_REORGED", now))
            (void)vault_intent_find(ctx->node_db, row->plan_id, row);
    }
}
