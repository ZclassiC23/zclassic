/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Composition adapter from the encrypted file-service request gate to the
 * authoritative market-payment application service. */

#include "config/boot_file_market_delivery.h"

#include "config/boot_internal.h"
#include "net/file_market_delivery.h"
#include "platform/time_compat.h"
#include "services/file_market_payment_service.h"
#include "sync/sync_state.h"
#include "util/log_macros.h"
#include "wallet/wallet.h"

#include <string.h>

static enum file_market_delivery_authorization
boot_authorize_file_market_chunk(
    const uint8_t offer_id[32], const uint8_t buyer_pubkey[32],
    uint32_t chunk_index, void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    if (!svc || !svc->node_db || !svc->node_db->open || !svc->state ||
        !svc->wallet || !offer_id || !buyer_pubkey)
        return FILE_MARKET_DELIVERY_UNKNOWN;

    int wallet_projection_height;
    zcl_mutex_lock(&svc->wallet->cs);
    wallet_projection_height = svc->wallet->best_block_height;
    zcl_mutex_unlock(&svc->wallet->cs);

    struct market_payment_authorization authorization;
    struct zcl_result result = market_payment_authorize_chunk(
        svc->node_db, svc->state, sync_get_state() == SYNC_AT_TIP,
        wallet_projection_height, offer_id, buyer_pubkey, chunk_index,
        (int64_t)platform_time_wall_time_t(), &authorization);
    if (!result.ok) {
        LOG_WARN("market", "paid chunk authorization failed: code=%d %s",
                 result.code, result.message);
        return FILE_MARKET_DELIVERY_UNKNOWN;
    }
    if (authorization.authorized &&
        strcmp(authorization.status, "CONFIRMED") == 0)
        return FILE_MARKET_DELIVERY_AUTHORIZED;
    if (strcmp(authorization.status, "UNKNOWN") == 0)
        return FILE_MARKET_DELIVERY_UNKNOWN;
    if (strcmp(authorization.status, "CONFLICTED") == 0)
        return FILE_MARKET_DELIVERY_CONFLICTED;
    if (strcmp(authorization.status, "PENDING") == 0)
        return FILE_MARKET_DELIVERY_PENDING;
    return FILE_MARKET_DELIVERY_REJECTED;
}

void boot_wire_file_market_delivery(struct boot_svc_ctx *svc)
{
    if (!svc || !svc->params) {
        file_market_delivery_reset_handlers();
        return;
    }
    file_market_delivery_set_handlers(
        svc->params->consensus.hashGenesisBlock.data,
        boot_authorize_file_market_chunk,
        NULL, /* owner-registered seller content reader lands next */
        svc);
}
