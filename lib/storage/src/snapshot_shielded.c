/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * snapshot_shielded — implementation.  See storage/snapshot_shielded.h for the
 * v3 shielded-section byte layout and its trust scope. */

#include "storage/snapshot_shielded.h"

#include "crypto/sha3.h"
#include "util/log_macros.h"

#include <string.h>

static void put_le32(uint8_t b[4], uint32_t v)
{
    b[0] = (uint8_t)v;         b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}
static void put_le64(uint8_t b[8], uint64_t v)
{
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
}

/* Write `buf` to BOTH the file and the SHA3 sponge, so file bytes == hash
 * input.  On a short write returns false; the caller aborts the temp file. */
static bool emit(FILE *out, struct sha3_256_ctx *ctx,
                 const uint8_t *buf, size_t len)
{
    if (len == 0)
        return true;
    if (fwrite(buf, 1, len, out) != len)
        return false;
    sha3_256_write(ctx, buf, len);
    return true;
}

void snapshot_shielded_pack_nf(uint8_t rec[SNAPSHOT_NF_RECORD_BYTES],
                               uint8_t pool, const uint8_t nf[32],
                               int64_t height)
{
    rec[0] = pool;
    memcpy(rec + 1, nf, 32);
    put_le64(rec + 33, (uint64_t)height);
}

void snapshot_shielded_unpack_nf(const uint8_t rec[SNAPSHOT_NF_RECORD_BYTES],
                                 uint8_t *pool, uint8_t nf[32],
                                 int64_t *height)
{
    if (pool) *pool = rec[0];
    if (nf)   memcpy(nf, rec + 1, 32);
    if (height) {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v |= ((uint64_t)rec[33 + i]) << (8 * i);
        *height = (int64_t)v;
    }
}

bool snapshot_shielded_write(FILE *out, struct sha3_256_ctx *ctx,
                             const struct snapshot_shielded *s)
{
    if (!out || !ctx || !s) {
        LOG_FAIL("snap_shielded", "write: null arg");
        return false;
    }
    if ((s->sapling_len > 0 && !s->sapling) ||
        (s->sprout_len  > 0 && !s->sprout)  ||
        (s->nf_count    > 0 && !s->nf_records)) {
        LOG_FAIL("snap_shielded", "write: nonzero length with null blob");
        return false;
    }

    uint8_t hdr[8];
    /* [u32 sapling_len][sapling] */
    put_le32(hdr, s->sapling_len);
    if (!emit(out, ctx, hdr, 4) ||
        !emit(out, ctx, s->sapling, s->sapling_len)) {
        LOG_FAIL("snap_shielded", "write: sapling section failed");
        return false;
    }
    /* [u32 sprout_len][sprout] */
    put_le32(hdr, s->sprout_len);
    if (!emit(out, ctx, hdr, 4) ||
        !emit(out, ctx, s->sprout, s->sprout_len)) {
        LOG_FAIL("snap_shielded", "write: sprout section failed");
        return false;
    }
    /* [u64 nf_count][records] */
    put_le64(hdr, s->nf_count);
    if (!emit(out, ctx, hdr, 8) ||
        !emit(out, ctx, s->nf_records,
              (size_t)s->nf_count * SNAPSHOT_NF_RECORD_BYTES)) {
        LOG_FAIL("snap_shielded", "write: nullifier section failed");
        return false;
    }
    return true;
}
