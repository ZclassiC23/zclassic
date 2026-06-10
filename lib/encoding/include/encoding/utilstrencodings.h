/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef BITCOIN_UTILSTRENCODINGS_H
#define BITCOIN_UTILSTRENCODINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum safe_chars {
    SAFE_CHARS_DEFAULT,
    SAFE_CHARS_UA_COMMENT
};

void SanitizeString(const char *str, int rule, char *out, size_t out_size);

extern const signed char p_util_hexdigit[256];
signed char HexDigit(char c);
bool IsHex(const char *str);
size_t ParseHex(const char *psz, unsigned char *out, size_t out_size);

size_t EncodeBase64(const unsigned char *data, size_t len, char *out, size_t out_size);
size_t DecodeBase64(const char *p, unsigned char *out, size_t out_size, bool *invalid);

size_t EncodeBase32(const unsigned char *data, size_t len, char *out, size_t out_size);

void HexStr(const unsigned char *data, size_t len, bool spaces, char *out, size_t out_size);

bool ParseInt32(const char *str, int32_t *out);

bool ParseFixedPoint(const char *val, int decimals, int64_t *amount_out);

bool ConvertBits(int frombits, int tobits, bool pad,
                 const unsigned char *in, size_t in_len,
                 unsigned char *out, size_t out_size, size_t *out_len);

#endif
