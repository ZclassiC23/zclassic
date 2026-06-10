/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_CHECK_TRANSACTION_H
#define ZCL_CHECK_TRANSACTION_H

#include "consensus/validation.h"
#include "primitives/transaction.h"
#include <stdbool.h>

bool check_transaction(const struct transaction *tx,
                       struct validation_state *state);

#endif
