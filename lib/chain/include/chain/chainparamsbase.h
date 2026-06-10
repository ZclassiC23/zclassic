/* Copyright (c) 2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef BITCOIN_CHAINPARAMSBASE_H
#define BITCOIN_CHAINPARAMSBASE_H

#include <stdbool.h>

enum chain_network {
    CHAIN_MAIN,
    CHAIN_TESTNET,
    CHAIN_REGTEST,
    CHAIN_MAX_NETWORK_TYPES
};

struct base_chain_params {
    int nRPCPort;
    const char *strDataDir;
};

const struct base_chain_params *BaseParams(void);
void SelectBaseParams(enum chain_network network);
bool AreBaseParamsConfigured(void);

#endif
