/* Copyright (c) 2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef BITCOIN_CRYPTO_COMMON_H
#define BITCOIN_CRYPTO_COMMON_H

#include <stdint.h>
#include <assert.h>
#include <string.h>

#if defined(NDEBUG)
# error "Zclassic cannot be compiled without assertions."
#endif

static inline uint16_t bswap16(uint16_t x)
{
    return (uint16_t)((x >> 8) | (x << 8));
}

static inline uint32_t bswap32(uint32_t x)
{
    return ((x >> 24) & 0x000000FFu) |
           ((x >>  8) & 0x0000FF00u) |
           ((x <<  8) & 0x00FF0000u) |
           ((x << 24) & 0xFF000000u);
}

static inline uint64_t bswap64(uint64_t x)
{
    return ((x >> 56) & 0x00000000000000FFull) |
           ((x >> 40) & 0x000000000000FF00ull) |
           ((x >> 24) & 0x0000000000FF0000ull) |
           ((x >>  8) & 0x00000000FF000000ull) |
           ((x <<  8) & 0x000000FF00000000ull) |
           ((x << 24) & 0x0000FF0000000000ull) |
           ((x << 40) & 0x00FF000000000000ull) |
           ((x << 56) & 0xFF00000000000000ull);
}

/* Endianness detection at compile time.
 * If none detected, assumes little-endian (x86/ARM default). */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define CRYPTO_BIG_ENDIAN 1
#else
#define CRYPTO_BIG_ENDIAN 0
#endif

static inline uint16_t ReadLE16(const unsigned char *ptr)
{
    uint16_t x;
    memcpy(&x, ptr, 2);
    return CRYPTO_BIG_ENDIAN ? bswap16(x) : x;
}

static inline uint32_t ReadLE32(const unsigned char *ptr)
{
    uint32_t x;
    memcpy(&x, ptr, 4);
    return CRYPTO_BIG_ENDIAN ? bswap32(x) : x;
}

static inline uint64_t ReadLE64(const unsigned char *ptr)
{
    uint64_t x;
    memcpy(&x, ptr, 8);
    return CRYPTO_BIG_ENDIAN ? bswap64(x) : x;
}

static inline void WriteLE16(unsigned char *ptr, uint16_t x)
{
    uint16_t v = CRYPTO_BIG_ENDIAN ? bswap16(x) : x;
    memcpy(ptr, &v, 2);
}

static inline void WriteLE32(unsigned char *ptr, uint32_t x)
{
    uint32_t v = CRYPTO_BIG_ENDIAN ? bswap32(x) : x;
    memcpy(ptr, &v, 4);
}

static inline void WriteLE64(unsigned char *ptr, uint64_t x)
{
    uint64_t v = CRYPTO_BIG_ENDIAN ? bswap64(x) : x;
    memcpy(ptr, &v, 8);
}

static inline uint32_t ReadBE32(const unsigned char *ptr)
{
    uint32_t x;
    memcpy(&x, ptr, 4);
    return CRYPTO_BIG_ENDIAN ? x : bswap32(x);
}

static inline uint64_t ReadBE64(const unsigned char *ptr)
{
    uint64_t x;
    memcpy(&x, ptr, 8);
    return CRYPTO_BIG_ENDIAN ? x : bswap64(x);
}

static inline void WriteBE32(unsigned char *ptr, uint32_t x)
{
    uint32_t v = CRYPTO_BIG_ENDIAN ? x : bswap32(x);
    memcpy(ptr, &v, 4);
}

static inline void WriteBE64(unsigned char *ptr, uint64_t x)
{
    uint64_t v = CRYPTO_BIG_ENDIAN ? x : bswap64(x);
    memcpy(ptr, &v, 8);
}

#endif
