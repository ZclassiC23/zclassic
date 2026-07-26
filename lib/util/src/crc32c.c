/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * crc32c — shared Castagnoli CRC-32C implementation.
 *
 * Lifted verbatim from lib/storage/src/event_log.c, where it had been the
 * only copy in the tree. The LevelDB reader needs the identical function
 * for its record and block trailers, and two independently maintained
 * spellings of a checksum that gates on-disk acceptance is exactly the
 * kind of cloned authority this codebase removes on sight.
 *
 * See util/crc32c.h for the contract.
 */

#include "util/crc32c.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__)
#include <nmmintrin.h>
#endif

static uint32_t g_crc32c_table[256];
static pthread_once_t g_crc32c_once = PTHREAD_ONCE_INIT;
static bool g_crc32c_use_hw = false;

static void crc32c_table_build(void)
{
    /* Castagnoli polynomial reflected: 0x82F63B78. */
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (0x82F63B78u & -(c & 1u));
        g_crc32c_table[i] = c;
    }
}

static uint32_t crc32c_sw(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ g_crc32c_table[(crc ^ p[i]) & 0xFFu];
    return crc ^ 0xFFFFFFFFu;
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("sse4.2")))
static uint32_t crc32c_hw(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
#if defined(__x86_64__)
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, p, sizeof(v));
        crc = (uint32_t)_mm_crc32_u64((uint64_t)crc, v);
        p += 8;
        len -= 8;
    }
#else
    while (len >= 4) {
        uint32_t v;
        memcpy(&v, p, sizeof(v));
        crc = _mm_crc32_u32(crc, v);
        p += 4;
        len -= 4;
    }
#endif
    while (len > 0) {
        crc = _mm_crc32_u8(crc, *p++);
        len--;
    }
    return crc ^ 0xFFFFFFFFu;
}
#endif

static void crc32c_init_once(void)
{
    crc32c_table_build();
#if defined(__x86_64__) || defined(__i386__)
    if (__builtin_cpu_supports("sse4.2")) {
        uint8_t buf[4099];
        for (size_t i = 0; i < sizeof(buf); i++)
            buf[i] = (uint8_t)(i * 31u + 7u);
        bool ok = true;
        for (size_t n = 0; n <= sizeof(buf); n += (n < 64 ? 1 : 257)) {
            if (crc32c_hw(buf, n) != crc32c_sw(buf, n)) {
                ok = false;
                break;
            }
        }
        g_crc32c_use_hw = ok;
        if (!ok) {
            fprintf(stderr,  // obs-ok:event-log-crc-selfcheck
                    "[crc32c] SSE4.2 crc32c self-check failed; "
                    "using software crc32c\n");
        }
    }
#endif
}

uint32_t zcl_crc32c(const void *data, size_t len)
{
    pthread_once(&g_crc32c_once, crc32c_init_once);
#if defined(__x86_64__) || defined(__i386__)
    if (g_crc32c_use_hw)
        return crc32c_hw(data, len);
#endif
    return crc32c_sw(data, len);
}

uint32_t zcl_crc32c_sw(const void *data, size_t len)
{
    pthread_once(&g_crc32c_once, crc32c_init_once);
    return crc32c_sw(data, len);
}

bool zcl_crc32c_hw_available(void)
{
    pthread_once(&g_crc32c_once, crc32c_init_once);
    return g_crc32c_use_hw;
}

const char *zcl_crc32c_impl_name(void)
{
    pthread_once(&g_crc32c_once, crc32c_init_once);
    return g_crc32c_use_hw ? "hardware-sse4.2" : "software-table";
}
