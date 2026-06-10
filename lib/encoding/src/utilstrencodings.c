/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "encoding/utilstrencodings.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char SAFE_DEFAULT[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,;_/:?@()";
static const char SAFE_UA[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,;_?@";

void SanitizeString(const char *str, int rule, char *out, size_t out_size)
{
    const char *safe = (rule == SAFE_CHARS_UA_COMMENT) ? SAFE_UA : SAFE_DEFAULT;
    size_t j = 0;
    for (size_t i = 0; str[i] && j + 1 < out_size; i++) {
        if (strchr(safe, str[i]))
            out[j++] = str[i];
    }
    out[j] = '\0';
}

const signed char p_util_hexdigit[256] =
{ -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  0,1,2,3,4,5,6,7,8,9,-1,-1,-1,-1,-1,-1,
  -1,0xa,0xb,0xc,0xd,0xe,0xf,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,0xa,0xb,0xc,0xd,0xe,0xf,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, };

signed char HexDigit(char c)
{
    return p_util_hexdigit[(unsigned char)c];
}

bool IsHex(const char *str)
{
    size_t len = strlen(str);
    if (len == 0 || len % 2 != 0)
        return false;
    for (size_t i = 0; i < len; i++) {
        if (HexDigit(str[i]) < 0)
            return false;
    }
    return true;
}

size_t ParseHex(const char *psz, unsigned char *out, size_t out_size)
{
    size_t count = 0;
    while (count < out_size) {
        while (isspace((unsigned char)*psz))
            psz++;
        signed char c = HexDigit(*psz++);
        if (c == (signed char)-1)
            break;
        unsigned char n = (unsigned char)(c << 4);
        c = HexDigit(*psz++);
        if (c == (signed char)-1)
            break;
        n |= (unsigned char)c;
        out[count++] = n;
    }
    return count;
}

static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t EncodeBase64(const unsigned char *data, size_t len, char *out, size_t out_size)
{
    size_t j = 0;
    size_t acc = 0, bits = 0;
    for (size_t i = 0; i < len; i++) {
        acc = (acc << 8) | data[i];
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            if (j + 1 < out_size)
                out[j++] = base64_chars[(acc >> bits) & 0x3F];
        }
    }
    if (bits > 0 && j + 1 < out_size)
        out[j++] = base64_chars[(acc << (6 - bits)) & 0x3F];
    while (j % 4 != 0 && j + 1 < out_size)
        out[j++] = '=';
    out[j] = '\0';
    return j;
}

static const int decode64_table[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,
    -1,-1,-1,-1,-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,-1,26,27,28,
    29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
    49,50,51,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

size_t DecodeBase64(const char *p, unsigned char *out, size_t out_size, bool *invalid)
{
    const char *start = p;
    size_t acc = 0, bits = 0, j = 0;

    while (*p) {
        int x = decode64_table[(unsigned char)*p];
        if (x == -1) break;
        acc = (acc << 6) | (unsigned)x;
        bits += 6;
        while (bits >= 8) {
            bits -= 8;
            if (j < out_size)
                out[j++] = (unsigned char)((acc >> bits) & 0xFF);
        }
        p++;
    }

    const char *q = p;
    bool valid = true;
    while (*p) {
        if (*p != '=') { valid = false; break; }
        p++;
    }
    valid = valid && (p - start) % 4 == 0 && p - q < 4;
    if (invalid) *invalid = !valid;
    return j;
}

static const char base32_chars[] = "abcdefghijklmnopqrstuvwxyz234567";

size_t EncodeBase32(const unsigned char *data, size_t len, char *out, size_t out_size)
{
    size_t j = 0, acc = 0, bits = 0;
    for (size_t i = 0; i < len; i++) {
        acc = (acc << 8) | data[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            if (j + 1 < out_size)
                out[j++] = base32_chars[(acc >> bits) & 0x1F];
        }
    }
    if (bits > 0 && j + 1 < out_size)
        out[j++] = base32_chars[(acc << (5 - bits)) & 0x1F];
    while (j % 8 != 0 && j + 1 < out_size)
        out[j++] = '=';
    out[j] = '\0';
    return j;
}

void HexStr(const unsigned char *data, size_t len, bool spaces, char *out, size_t out_size)
{
    static const char hexmap[] = "0123456789abcdef";
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        size_t need = 2 + ((spaces && i > 0) ? 1 : 0);
        if (j + need >= out_size)
            break;
        if (spaces && i > 0)
            out[j++] = ' ';
        out[j++] = hexmap[data[i] >> 4];
        out[j++] = hexmap[data[i] & 0xF];
    }
    out[j] = '\0';
}

static bool parse_prechecks(const char *str)
{
    size_t len = strlen(str);
    if (len == 0) return false;
    if (isspace((unsigned char)str[0]) || isspace((unsigned char)str[len - 1]))
        return false;
    if (len != strlen(str)) return false;
    return true;
}

bool ParseInt32(const char *str, int32_t *out)
{
    if (!parse_prechecks(str))
        return false;
    char *endp = NULL;
    errno = 0;
    long n = strtol(str, &endp, 10);
    if (out) *out = (int32_t)n;
    return endp && *endp == 0 && !errno &&
        n >= INT32_MIN && n <= INT32_MAX;
}

static const int64_t UPPER_BOUND = 1000000000000000000LL - 1LL;

static bool process_mantissa_digit(char ch, int64_t *mantissa, int *mantissa_tzeros)
{
    if (ch == '0') {
        ++(*mantissa_tzeros);
    } else {
        for (int i = 0; i <= *mantissa_tzeros; ++i) {
            if (*mantissa > (UPPER_BOUND / 10LL))
                return false;
            *mantissa *= 10;
        }
        *mantissa += ch - '0';
        *mantissa_tzeros = 0;
    }
    return true;
}

bool ParseFixedPoint(const char *val, int decimals, int64_t *amount_out)
{
    int64_t mantissa = 0;
    int64_t exponent = 0;
    int mantissa_tzeros = 0;
    bool mantissa_sign = false;
    bool exponent_sign = false;
    int ptr = 0;
    int end = (int)strlen(val);
    int point_ofs = 0;

    if (ptr < end && val[ptr] == '-') {
        mantissa_sign = true;
        ++ptr;
    }
    if (ptr < end) {
        if (val[ptr] == '0') {
            ++ptr;
        } else if (val[ptr] >= '1' && val[ptr] <= '9') {
            while (ptr < end && val[ptr] >= '0' && val[ptr] <= '9') {
                if (!process_mantissa_digit(val[ptr], &mantissa, &mantissa_tzeros))
                    return false;
                ++ptr;
            }
        } else return false;
    } else return false;

    if (ptr < end && val[ptr] == '.') {
        ++ptr;
        if (ptr < end && val[ptr] >= '0' && val[ptr] <= '9') {
            while (ptr < end && val[ptr] >= '0' && val[ptr] <= '9') {
                if (!process_mantissa_digit(val[ptr], &mantissa, &mantissa_tzeros))
                    return false;
                ++ptr;
                ++point_ofs;
            }
        } else return false;
    }

    if (ptr < end && (val[ptr] == 'e' || val[ptr] == 'E')) {
        ++ptr;
        if (ptr < end && val[ptr] == '+')
            ++ptr;
        else if (ptr < end && val[ptr] == '-') {
            exponent_sign = true;
            ++ptr;
        }
        if (ptr < end && val[ptr] >= '0' && val[ptr] <= '9') {
            while (ptr < end && val[ptr] >= '0' && val[ptr] <= '9') {
                if (exponent > (UPPER_BOUND / 10LL))
                    return false;
                exponent = exponent * 10 + val[ptr] - '0';
                ++ptr;
            }
        } else return false;
    }
    if (ptr != end)
        return false;

    if (exponent_sign)
        exponent = -exponent;
    exponent = exponent - point_ofs + mantissa_tzeros;

    if (mantissa_sign)
        mantissa = -mantissa;

    exponent += decimals;
    if (exponent < 0)
        return false;
    if (exponent >= 18)
        return false;

    for (int i = 0; i < exponent; ++i) {
        if (mantissa > (UPPER_BOUND / 10LL) || mantissa < -(UPPER_BOUND / 10LL))
            return false;
        mantissa *= 10;
    }
    if (mantissa > UPPER_BOUND || mantissa < -UPPER_BOUND)
        return false;

    if (amount_out)
        *amount_out = mantissa;

    return true;
}

bool ConvertBits(int frombits, int tobits, bool pad,
                 const unsigned char *in, size_t in_len,
                 unsigned char *out, size_t out_size, size_t *out_len)
{
    size_t acc = 0, bits = 0, j = 0;
    size_t maxv = ((size_t)1 << tobits) - 1;
    size_t max_acc = ((size_t)1 << (frombits + tobits - 1)) - 1;

    for (size_t i = 0; i < in_len; i++) {
        acc = ((acc << frombits) | in[i]) & max_acc;
        bits += (size_t)frombits;
        while (bits >= (size_t)tobits) {
            bits -= (size_t)tobits;
            if (j < out_size)
                out[j++] = (unsigned char)((acc >> bits) & maxv);
        }
    }
    if (pad) {
        if (bits && j < out_size)
            out[j++] = (unsigned char)((acc << (tobits - bits)) & maxv);
    } else if (bits >= (size_t)frombits || ((acc << (tobits - bits)) & maxv)) {
        if (out_len) *out_len = j;
        return false;
    }
    if (out_len) *out_len = j;
    return true;
}
