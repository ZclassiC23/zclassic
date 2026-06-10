/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_ZCASHCONSENSUS_H
#define ZCL_ZCASHCONSENSUS_H

#include <stdint.h>

#define ZCASHCONSENSUS_API_VER 0

typedef enum {
    zcl_consensus_ERR_OK = 0,
    zcl_consensus_ERR_TX_INDEX,
    zcl_consensus_ERR_TX_SIZE_MISMATCH,
    zcl_consensus_ERR_TX_DESERIALIZE
} zcl_consensus_error;

enum {
    zcl_consensus_SCRIPT_FLAGS_VERIFY_NONE                = 0,
    zcl_consensus_SCRIPT_FLAGS_VERIFY_P2SH                = (1U << 0),
    zcl_consensus_SCRIPT_FLAGS_VERIFY_CHECKLOCKTIMEVERIFY = (1U << 9)
};

int zcl_consensus_verify_script(const unsigned char *script_pub_key,
                                unsigned int script_pub_key_len,
                                const unsigned char *tx_to,
                                unsigned int tx_to_len,
                                unsigned int n_in,
                                unsigned int flags,
                                zcl_consensus_error *err);

unsigned int zcl_consensus_version(void);

#endif
