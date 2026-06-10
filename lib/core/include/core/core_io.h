/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_CORE_IO_H
#define ZCL_CORE_IO_H

#include "json/json.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "core/uint256.h"
#include <stdbool.h>
#include <stddef.h>

bool parse_script(const char *asm_str, struct script *out);

size_t script_to_asm_str(const struct script *s, bool attempt_sighash_decode,
                         char *out, size_t out_size);

bool decode_hex_tx(struct transaction *tx, const char *hex_str);

bool parse_hash_str(const char *hex_str, struct uint256 *out);

bool parse_hash_uv(const struct json_value *v, struct uint256 *out);

size_t encode_hex_tx(const struct transaction *tx, char *out, size_t out_size);

void script_pub_key_to_json(const struct script *script_pub_key,
                            struct json_value *out, bool include_hex);

void tx_to_json(const struct transaction *tx,
                const struct uint256 *hash_block,
                struct json_value *entry);

#endif
