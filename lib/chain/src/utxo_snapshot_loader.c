/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "chain/utxo_snapshot_loader.h"
#include "crypto/sha3.h"
#include "util/safe_alloc.h"
#include "util/log_macros.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define USS_HEADER_BYTES 104
#define USS_MAGIC        "ZCLUTXO\x00"

struct uss_handle {
    const uint8_t *base;
    size_t         size;       /* total file size */
    size_t         body_off;   /* offset where body starts (= 104) */
    size_t         body_len;
    struct uss_header hdr;
};

#define USS_VERSION_V1 1u
#define USS_VERSION_V2 2u   /* v1 + trailing Sapling-frontier section */

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i]) << (8 * i);
    return v;
}

static void set_err(char *buf, size_t cap, const char *fmt, ...)
{
    if (!buf || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
}

struct uss_handle *uss_open(const char *path,
                            bool verify_full_sha3,
                            const uint8_t *expected_sha3,
                            struct uss_header *hdr_out,
                            char *err, size_t err_sz)
{
    if (!path) {
        set_err(err, err_sz, "null path");
        LOG_NULL("uss", "open: null path");
        return NULL;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        set_err(err, err_sz, "open(%s): %s", path, strerror(errno));
        LOG_NULL("uss", "open failed: %s", strerror(errno));
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < USS_HEADER_BYTES) {
        set_err(err, err_sz, "too small or fstat failed");
        close(fd);
        LOG_NULL("uss", "fstat or size");
        return NULL;
    }
    void *mp = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mp == MAP_FAILED) {
        set_err(err, err_sz, "mmap: %s", strerror(errno));
        LOG_NULL("uss", "mmap failed");
        return NULL;
    }
    posix_madvise(mp, (size_t)st.st_size, POSIX_MADV_SEQUENTIAL);

    const uint8_t *base = (const uint8_t *)mp;
    if (memcmp(base, USS_MAGIC, 8) != 0) {
        set_err(err, err_sz, "bad magic");
        munmap(mp, (size_t)st.st_size);
        LOG_NULL("uss", "bad magic");
        return NULL;
    }

    struct uss_handle *h = zcl_malloc(sizeof(*h), "uss.handle");
    if (!h) {
        set_err(err, err_sz, "oom");
        munmap(mp, (size_t)st.st_size);
        return NULL;
    }
    h->base = base;
    h->size = (size_t)st.st_size;
    h->body_off = USS_HEADER_BYTES;
    h->body_len = h->size - USS_HEADER_BYTES;

    h->hdr.version = rd_le32(base + 8);
    h->hdr.height  = rd_le32(base + 16);
    h->hdr.count   = rd_le64(base + 24);
    h->hdr.total_supply = (int64_t)rd_le64(base + 32);
    memcpy(h->hdr.anchor_block_hash, base + 40, 32);
    memcpy(h->hdr.sha3_hash,         base + 72, 32);

    if (h->hdr.version != USS_VERSION_V1 && h->hdr.version != USS_VERSION_V2) {
        set_err(err, err_sz, "bad version %u", h->hdr.version);
        munmap(mp, (size_t)st.st_size);
        free(h);
        LOG_NULL("uss", "version");
        return NULL;
    }
    if (expected_sha3 &&
        memcmp(h->hdr.sha3_hash, expected_sha3, 32) != 0) {
        set_err(err, err_sz, "expected sha3 mismatch");
        munmap(mp, (size_t)st.st_size);
        free(h);
        LOG_NULL("uss", "expected sha3 mismatch");
        return NULL;
    }
    if (verify_full_sha3) {
        struct sha3_256_ctx ctx;
        sha3_256_init(&ctx);
        sha3_256_write(&ctx, base + h->body_off, h->body_len);
        uint8_t computed[32];
        sha3_256_finalize(&ctx, computed);
        if (memcmp(computed, h->hdr.sha3_hash, 32) != 0) {
            set_err(err, err_sz, "body sha3 mismatch");
            munmap(mp, (size_t)st.st_size);
            free(h);
            LOG_NULL("uss", "body sha3 mismatch");
            return NULL;
        }
    }
    if (hdr_out) *hdr_out = h->hdr;
    return h;
}

void uss_close(struct uss_handle *h)
{
    if (!h) return;
    if (h->base) munmap((void *)h->base, h->size);
    free(h);
}

int64_t uss_iter(struct uss_handle *h, uss_record_cb cb, void *ctx)
{
    if (!h || !cb) return -1;
    const uint8_t *p = h->base + h->body_off;
    const uint8_t *end = h->base + h->size;
    int64_t n = 0;
    for (uint64_t i = 0; i < h->hdr.count; i++) {
        if ((size_t)(end - p) < 32 + 4 + 8 + 4) return -1;
        struct uss_record r;
        r.txid = p; p += 32;
        r.vout = rd_le32(p); p += 4;
        r.value = (int64_t)rd_le64(p); p += 8;
        r.script_len = rd_le32(p); p += 4;
        if ((uint64_t)(end - p) < (uint64_t)r.script_len + 4 + 1) return -1;
        r.script = r.script_len ? p : NULL;
        p += r.script_len;
        r.height = rd_le32(p); p += 4;
        r.is_coinbase = *p++;
        if (!cb(&r, ctx)) return n + 1;
        n++;
    }
    return n;
}

uint32_t uss_version(const struct uss_handle *h)
{
    return h ? h->hdr.version : 0;
}

bool uss_frontier(struct uss_handle *h, const uint8_t **blob_out,
                  uint32_t *len_out)
{
    if (blob_out) *blob_out = NULL;
    if (len_out)  *len_out = 0;
    if (!h) return false;
    if (h->hdr.version != USS_VERSION_V2) return false;

    /* Walk PAST the `count` UTXO records (identical decode to uss_iter) to find
     * the trailing [u32 frontier_len LE][blob] section. The section lives inside
     * the body SHA3 region, so uss_open's verify_full_sha3 already bound it. */
    const uint8_t *p = h->base + h->body_off;
    const uint8_t *end = h->base + h->size;
    for (uint64_t i = 0; i < h->hdr.count; i++) {
        if ((size_t)(end - p) < 32 + 4 + 8 + 4) return false;
        p += 32;            /* txid   */
        p += 4;             /* vout   */
        p += 8;             /* value  */
        uint32_t slen = rd_le32(p); p += 4;
        if ((uint64_t)(end - p) < (uint64_t)slen + 4 + 1) return false;
        p += slen;          /* script */
        p += 4;             /* height */
        p += 1;             /* is_coinbase */
    }
    /* p now points at the frontier section. */
    if ((size_t)(end - p) < 4) return false;
    uint32_t flen = rd_le32(p); p += 4;
    if (flen == 0 || (uint64_t)(end - p) < (uint64_t)flen) return false;
    if (blob_out) *blob_out = p;
    if (len_out)  *len_out  = flen;
    return true;
}
