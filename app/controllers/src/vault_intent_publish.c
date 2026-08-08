/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: restart-safe publication of a prepared vault-intent transaction. */

#include "controllers/vault_intent_publish.h"

#include "controllers/sync_controller.h"
#include "controllers/wallet_helpers.h"
#include "json/json.h"
#include "models/database.h"
#include "models/vault_intent.h"
#include "models/wallet_tx.h"
#include "net/connman.h"
#include "util/log_macros.h"
#include "wallet/wallet.h"

static void vipub_error(struct json_value *out, const char *code,
                        const char *message)
{
    json_set_object(out);
    (void)json_push_kv_bool(out, "ok", false);
    (void)json_push_kv_str(out, "code", code);
    (void)json_push_kv_str(out, "message", message);
}

static bool vipub_preflight_sapling_notes(struct wallet_rpc_context *ctx,
                                          const uint8_t id[32],
                                          const struct wallet_tx *wtx,
                                          int64_t now,
                                          struct json_value *result)
{
    for (size_t i = 0; i < wtx->tx.num_shielded_spend; i++) {
        enum db_sapling_note_reservation_state state =
            db_sapling_note_reservation_probe(
                ctx->node_db, wtx->tx.v_shielded_spend[i].nullifier.data,
                wtx->tx.hash.data);
        if (state == DB_NOTE_RESERVATION_AVAILABLE ||
            state == DB_NOTE_RESERVATION_SAME_TX)
            continue;
        if (state == DB_NOTE_RESERVATION_MISSING) {
            (void)vault_intent_set_state(ctx->node_db, id,
                VAULT_INTENT_FAILED, wtx->tx.hash.data,
                "PREPARED_NOTE_MISMATCH", now);
            vipub_error(result, "PREPARED_NOTE_MISMATCH",
                "prepared transaction names no current wallet note; create a fresh plan");
            return false;
        }
        if (state == DB_NOTE_RESERVATION_CONFLICT) {
            (void)vault_intent_set_state(ctx->node_db, id,
                VAULT_INTENT_CONFLICTED, wtx->tx.hash.data,
                "PREPARED_NOTE_CONFLICT", now);
            vipub_error(result, "PREPARED_NOTE_CONFLICT",
                "prepared transaction note is reserved by another transaction");
            return false;
        }
        vipub_error(result, "NOTE_RESERVATION_FAILED",
                    "shielded-note reservation state is temporarily unreadable");
        return false;
    }
    return true;
}

bool vault_intent_publish_prepared(struct wallet_rpc_context *ctx,
                                   const uint8_t id[32],
                                   struct wallet_tx *wtx, int64_t now,
                                   struct json_value *result)
{
    if (wtx->tx.num_shielded_spend > 0 &&
        !vipub_preflight_sapling_notes(ctx, id, wtx, now, result))
        return false;
    bool already_durable = wallet_get_tx(ctx->wallet, &wtx->tx.hash) != NULL;
    if (!already_durable) {
        struct zcl_result r = wallet_commit_from_context(ctx, wtx);
        if (!r.ok) {
            (void)vault_intent_set_state(ctx->node_db, id,
                VAULT_INTENT_FAILED, NULL, "MEMPOOL_REJECTED", now);
            vipub_error(result, "MEMPOOL_REJECTED", r.message);
            return false;
        }
        r = wallet_persist_commit_before_relay(ctx, wtx);
        if (!r.ok) {
            (void)vault_intent_set_state(ctx->node_db, id,
                VAULT_INTENT_FAILED, NULL, "PERSISTENCE_FAILED", now);
            vipub_error(result, "PERSISTENCE_FAILED", r.message);
            return false;
        }
    }

    /* A prepared Sapling spend is not publishable until its nullifiers are
     * reserved durably. This runs on retries too: a crash after wallet
     * persistence but before intent-state update resumes the same raw tx. */
    if (wtx->tx.num_shielded_spend > 0 &&
        !node_db_sync_wallet_sapling_spends(ctx->node_db, &wtx->tx)) {
        if (!already_durable) {
            struct zcl_result compensated =
                wallet_rollback_persisted_commit(ctx, wtx);
            if (!compensated.ok)
                LOG_ERROR("vault_intent", "Sapling reservation compensation "
                          "failed (code=%d): %s", compensated.code,
                          compensated.message);
        }
        (void)vault_intent_set_state(ctx->node_db, id, VAULT_INTENT_PROVING,
            wtx->tx.hash.data, "NOTE_RESERVATION_FAILED", now);
        vipub_error(result, "NOTE_RESERVATION_FAILED",
                    "prepared transaction remains durable but shielded notes could not be reserved; retry the same plan");
        return false;
    }
    if (wtx->tx.num_shielded_spend > 0)
        wallet_mark_sapling_nullifiers_spent(ctx->wallet, &wtx->tx);

    if (wallet_ctx_db_ready(ctx) &&
        !node_db_sync_wallet_tx(ctx->node_db, &wtx->tx, ctx->wallet, 0))
        LOG_WARN("vault_intent", "wallet projection write failed for prepared tx");
    if (ctx->connman)
        connman_relay_transaction(ctx->connman, &wtx->tx.hash);
    if (!vault_intent_set_state(ctx->node_db, id,
            VAULT_INTENT_MEMPOOL_ACCEPTED, wtx->tx.hash.data, "", now)) {
        vipub_error(result, "INTENT_STATE_FAILED",
                    "transaction is durable but intent state update failed; retry the same plan");
        return false;
    }
    return true;
}
