/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_VALIDATIONINTERFACE_H
#define ZCL_VALIDATIONINTERFACE_H

#include "core/uint256.h"
#include "primitives/block.h"
#include <stdbool.h>
#include <stdint.h>

struct transaction;

#define MAX_VALIDATION_LISTENERS 16

struct validation_callbacks {
    void *ctx;
    void (*updated_block_tip)(void *ctx, int block_height);
    void (*sync_transaction)(void *ctx, const struct transaction *tx,
                             const struct block_header *block);
    void (*erase_from_wallet)(void *ctx, const struct uint256 *hash);
    void (*updated_transaction)(void *ctx, const struct uint256 *hash);
    void (*set_best_chain)(void *ctx, const struct block_locator *locator);
    void (*inventory)(void *ctx, const struct uint256 *hash);
    void (*broadcast)(void *ctx, int64_t best_block_time);
    void (*block_checked)(void *ctx, const struct block_header *header,
                          bool valid);
};

struct validation_signals {
    struct validation_callbacks listeners[MAX_VALIDATION_LISTENERS];
    size_t num_listeners;
};

void validation_signals_init(struct validation_signals *vs);

bool validation_register(struct validation_signals *vs,
                          const struct validation_callbacks *cb);

bool validation_unregister(struct validation_signals *vs, void *ctx);

void validation_unregister_all(struct validation_signals *vs);

void signal_updated_block_tip(const struct validation_signals *vs,
                               int block_height);

void signal_sync_transaction(const struct validation_signals *vs,
                              const struct transaction *tx,
                              const struct block_header *block);

void signal_erase_from_wallet(const struct validation_signals *vs,
                               const struct uint256 *hash);

void signal_updated_transaction(const struct validation_signals *vs,
                                 const struct uint256 *hash);

void signal_set_best_chain(const struct validation_signals *vs,
                            const struct block_locator *locator);

void signal_inventory(const struct validation_signals *vs,
                       const struct uint256 *hash);

void signal_broadcast(const struct validation_signals *vs,
                       int64_t best_block_time);

void signal_block_checked(const struct validation_signals *vs,
                           const struct block_header *header,
                           bool valid);

struct validation_signals *get_main_signals(void);

#endif
