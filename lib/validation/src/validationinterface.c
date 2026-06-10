/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "validation/validationinterface.h"
#include "util/log_macros.h"
#include <string.h>

static struct validation_signals g_signals;

void validation_signals_init(struct validation_signals *vs)
{
    memset(vs, 0, sizeof(*vs));
}

struct validation_signals *get_main_signals(void)
{
    return &g_signals;
}

bool validation_register(struct validation_signals *vs,
                          const struct validation_callbacks *cb)
{
    if (vs->num_listeners >= MAX_VALIDATION_LISTENERS)
        LOG_FAIL("validation", "listener registration failed: max %d listeners reached",
                 MAX_VALIDATION_LISTENERS);
    vs->listeners[vs->num_listeners++] = *cb;
    return true;
}

bool validation_unregister(struct validation_signals *vs, void *ctx)
{
    for (size_t i = 0; i < vs->num_listeners; i++) {
        if (vs->listeners[i].ctx == ctx) {
            vs->listeners[i] = vs->listeners[vs->num_listeners - 1];
            vs->num_listeners--;
            return true;
        }
    }
    LOG_FAIL("validation", "unregister failed: listener ctx=%p not found", ctx);
}

void validation_unregister_all(struct validation_signals *vs)
{
    vs->num_listeners = 0;
}

void signal_updated_block_tip(const struct validation_signals *vs,
                               int block_height)
{
    for (size_t i = 0; i < vs->num_listeners; i++)
        if (vs->listeners[i].updated_block_tip)
            vs->listeners[i].updated_block_tip(vs->listeners[i].ctx,
                                                block_height);
}

void signal_sync_transaction(const struct validation_signals *vs,
                              const struct transaction *tx,
                              const struct block_header *block)
{
    for (size_t i = 0; i < vs->num_listeners; i++)
        if (vs->listeners[i].sync_transaction)
            vs->listeners[i].sync_transaction(vs->listeners[i].ctx, tx, block);
}

void signal_erase_from_wallet(const struct validation_signals *vs,
                               const struct uint256 *hash)
{
    for (size_t i = 0; i < vs->num_listeners; i++)
        if (vs->listeners[i].erase_from_wallet)
            vs->listeners[i].erase_from_wallet(vs->listeners[i].ctx, hash);
}

void signal_updated_transaction(const struct validation_signals *vs,
                                 const struct uint256 *hash)
{
    for (size_t i = 0; i < vs->num_listeners; i++)
        if (vs->listeners[i].updated_transaction)
            vs->listeners[i].updated_transaction(vs->listeners[i].ctx, hash);
}

void signal_set_best_chain(const struct validation_signals *vs,
                            const struct block_locator *locator)
{
    for (size_t i = 0; i < vs->num_listeners; i++)
        if (vs->listeners[i].set_best_chain)
            vs->listeners[i].set_best_chain(vs->listeners[i].ctx, locator);
}

void signal_inventory(const struct validation_signals *vs,
                       const struct uint256 *hash)
{
    for (size_t i = 0; i < vs->num_listeners; i++)
        if (vs->listeners[i].inventory)
            vs->listeners[i].inventory(vs->listeners[i].ctx, hash);
}

void signal_broadcast(const struct validation_signals *vs,
                       int64_t best_block_time)
{
    for (size_t i = 0; i < vs->num_listeners; i++)
        if (vs->listeners[i].broadcast)
            vs->listeners[i].broadcast(vs->listeners[i].ctx, best_block_time);
}

void signal_block_checked(const struct validation_signals *vs,
                           const struct block_header *header,
                           bool valid)
{
    for (size_t i = 0; i < vs->num_listeners; i++)
        if (vs->listeners[i].block_checked)
            vs->listeners[i].block_checked(vs->listeners[i].ctx, header, valid);
}
