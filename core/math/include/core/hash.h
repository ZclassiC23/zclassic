/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2013 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef BITCOIN_HASH_H
#define BITCOIN_HASH_H

#include "crypto/sha256.h"
#include "crypto/ripemd160.h"
#include "crypto/hmac_sha512.h"
#include "core/uint256.h"

void hash256(const unsigned char *data, size_t len, unsigned char hash[SHA256_OUTPUT_SIZE]);
void hash160(const unsigned char *data, size_t len, unsigned char hash[RIPEMD160_OUTPUT_SIZE]);
uint32_t murmur_hash3(uint32_t seed, const unsigned char *data, size_t len);
void bip32_hash(const unsigned char chain_code[32], uint32_t child,
                unsigned char header, const unsigned char data[32],
                unsigned char output[64]);

#endif
