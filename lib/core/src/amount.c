/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "core/amount.h"
#include <stdio.h>

void fee_rate_to_string(const struct fee_rate *r, char *out, size_t out_size)
{
    snprintf(out, out_size, "%lld.%08lld ZCL/kB",
             (long long)(r->satoshis_per_k / COIN),
             (long long)(r->satoshis_per_k % COIN));
}
