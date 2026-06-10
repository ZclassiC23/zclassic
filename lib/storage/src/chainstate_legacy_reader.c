/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * chainstate_legacy_reader.c — see header for the API contract.
 *
 * On-disk format (Bitcoin Core 0.8+, used unchanged by zclassicd):
 *
 *   key:   'c' || <32-byte txid in raw byte order>
 *   value: VARINT(version)
 *          VARINT(code)          -- bits: cb=1, avail0=2, avail1=4,
 *                                          rest -> nMaskCode
 *          <mask bytes>          -- one byte per 8 vouts past index 1
 *                                   (continues until nMaskCode non-zero
 *                                    mask bytes have been seen)
 *          for each available vout (in ascending vout-index order):
 *              VARINT(compressed_amount)
 *              VARINT(nSize)
 *              if nSize < 6  : GetSpecialSize(nSize) bytes (compressed
 *                              P2PKH / P2SH / P2PK form)
 *              else           : nSize - 6 raw script bytes
 *          VARINT(height)
 *
 * Best-block hash is stored under the single-byte key 'B'.
 *
 * We deobfuscate via the existing dbwrapper layer (it auto-reads the
 * "obfuscate_key" record on open), reuse compressor.c's amount + script
 * decompression, and reuse serialize.c's Bitcoin-style varint reader.
 */

#include "storage/chainstate_legacy_reader.h"

#include "coins/compressor.h"
#include "core/serialize.h"
#include "script/script.h"
#include "storage/dbwrapper.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <stdlib.h>
#include <string.h>

/* --- handle --- */

struct chainstate_legacy_handle {
    struct db_wrapper db;
    /* Decode scratch reused across records to avoid alloc churn. */
    struct legacy_coins_vout *vouts_buf;
    size_t vouts_cap;
    /* Decompressed script storage — each vout points into here. */
    struct script *scripts_buf;
    size_t scripts_cap;
    /* Per-vout-index availability bitset (dynamic). */
    bool *avail_buf;
    size_t avail_cap;
};

/* --- helpers --- */

static bool ensure_vouts_cap(struct chainstate_legacy_handle *h, size_t n)
{
    if (n <= h->vouts_cap) return true;
    size_t cap = h->vouts_cap ? h->vouts_cap : 4;
    while (cap < n) cap *= 2;
    struct legacy_coins_vout *p =
        zcl_realloc(h->vouts_buf, cap * sizeof(*p),
                    "chainstate_legacy_vouts");
    if (!p) return false;
    struct script *q =
        zcl_realloc(h->scripts_buf, cap * sizeof(*q),
                    "chainstate_legacy_scripts");
    if (!q) return false;
    h->vouts_buf = p;
    h->scripts_buf = q;
    h->vouts_cap = cap;
    h->scripts_cap = cap;
    return true;
}

static bool ensure_avail_cap(struct chainstate_legacy_handle *h, size_t n)
{
    if (n <= h->avail_cap) return true;
    size_t cap = h->avail_cap ? h->avail_cap : 16;
    while (cap < n) cap *= 2;
    bool *p = zcl_realloc(h->avail_buf, cap * sizeof(*p),
                          "chainstate_legacy_avail");
    if (!p) return false;
    h->avail_buf = p;
    h->avail_cap = cap;
    return true;
}

/* Decode a single CCoins record into the handle's scratch.  Returns
 * true on success and fills *out (which borrows from the handle's
 * scratch — valid until the next decode). */
static bool decode_record(struct chainstate_legacy_handle *h,
                          const uint8_t *val, size_t vlen,
                          struct legacy_coins *out)
{
    struct byte_stream s;
    stream_init_from_data(&s, val, vlen);

    uint64_t version_u = 0;
    if (!stream_read_varint(&s, &version_u))
        LOG_FAIL("chainstate_legacy", "read version varint");

    uint64_t code = 0;
    if (!stream_read_varint(&s, &code))
        LOG_FAIL("chainstate_legacy", "read code varint");

    bool coinbase = (code & 1u) != 0u;
    bool avail0   = (code & 2u) != 0u;
    bool avail1   = (code & 4u) != 0u;
    /* nMaskCode = (code/8) + ((avail0 || avail1) ? 0 : 1)
     * — see Bitcoin Core coins.h. */
    unsigned int nMaskCode =
        (unsigned int)(code >> 3) + ((avail0 || avail1) ? 0u : 1u);

    /* Scan mask bytes: vAvail starts with [avail0, avail1].  Each mask
     * byte contributes 8 more avail bits (LSB-first).  Loop continues
     * until nMaskCode non-zero mask bytes have been consumed.
     *
     * This shares the CCoins mask format with coins_db.c but is deliberately
     * NOT the same routine: this import path reads a trusted, locally-produced
     * zclassicd chainstate and grows avail_buf without a fixed cap so it never
     * truncates a legitimately large record; the live node.db read path in
     * coins_db.c instead caps at 4096 vouts and rejects nMaskCode > 10000 to
     * bound untrusted-row memory. Do not merge the two — see the matching note
     * there. */
    if (!ensure_avail_cap(h, 2))
        LOG_FAIL("chainstate_legacy", "alloc avail scratch");
    size_t num_avail = 0;
    h->avail_buf[num_avail++] = avail0;
    h->avail_buf[num_avail++] = avail1;

    while (nMaskCode > 0) {
        uint8_t mask_byte = 0;
        if (!stream_read_u8(&s, &mask_byte))
            LOG_FAIL("chainstate_legacy", "read mask byte (truncated value)");
        if (!ensure_avail_cap(h, num_avail + 8))
            LOG_FAIL("chainstate_legacy", "grow avail scratch");
        for (int b = 0; b < 8; b++)
            h->avail_buf[num_avail++] = ((mask_byte >> b) & 1u) != 0u;
        if (mask_byte != 0)
            nMaskCode--;
    }

    /* Count available vouts and ensure scratch capacity. */
    size_t live = 0;
    for (size_t i = 0; i < num_avail; i++)
        if (h->avail_buf[i]) live++;
    if (!ensure_vouts_cap(h, live ? live : 1))
        LOG_FAIL("chainstate_legacy", "alloc vouts scratch");

    size_t live_idx = 0;
    for (size_t i = 0; i < num_avail; i++) {
        if (!h->avail_buf[i]) continue;
        /* Decompress one CTxOut. */
        uint64_t namt = 0;
        if (!stream_read_varint(&s, &namt))
            LOG_FAIL("chainstate_legacy", "read amount varint");
        uint64_t nsize = 0;
        if (!stream_read_varint(&s, &nsize))
            LOG_FAIL("chainstate_legacy", "read script-size varint");

        struct script *sc = &h->scripts_buf[live_idx];
        script_init(sc);

        if (nsize < 6) {
            unsigned int spec = script_compress_special_size((unsigned int)nsize);
            uint8_t buf[64];
            if (spec == 0 || spec > sizeof(buf))
                LOG_FAIL("chainstate_legacy", "bad special script size");
            if (!stream_read_bytes(&s, buf, spec))
                LOG_FAIL("chainstate_legacy", "read special script bytes");
            if (!script_decompress(sc, (unsigned int)nsize, buf, spec))
                LOG_FAIL("chainstate_legacy", "script_decompress failed");
        } else {
            uint64_t raw_len = nsize - 6;
            if (raw_len > MAX_SCRIPT_SIZE) {
                /* Bitcoin Core replaces with OP_RETURN and skips.  Do
                 * the same — preserves byte stream alignment so the
                 * trailing VARINT(height) still parses. */
                if (s.read_pos + raw_len > s.size)
                    LOG_FAIL("chainstate_legacy", "oversize script truncated");
                sc->data[0] = OP_RETURN;
                sc->size = 1;
                s.read_pos += (size_t)raw_len;
            } else {
                if (!stream_read_bytes(&s, sc->data, (size_t)raw_len))
                    LOG_FAIL("chainstate_legacy", "read raw script bytes");
                sc->size = (size_t)raw_len;
            }
        }

        h->vouts_buf[live_idx].n = (unsigned int)i;
        h->vouts_buf[live_idx].value = (int64_t)decompress_amount(namt);
        h->vouts_buf[live_idx].script = sc->data;
        h->vouts_buf[live_idx].script_len = sc->size;
        live_idx++;
    }

    uint64_t height_u = 0;
    if (!stream_read_varint(&s, &height_u))
        LOG_FAIL("chainstate_legacy", "read height varint");

    /* Trailing bytes are tolerated (compatible with Cleanup() that
     * drops trailing fully-spent vouts on re-serialize) but not
     * expected for a clean DB. */

    out->version  = (int)version_u;
    out->coinbase = coinbase;
    out->height   = (int)height_u;
    out->vouts    = h->vouts_buf;
    out->num_vouts = live;
    return true;
}

/* --- public API --- */

bool chainstate_legacy_open(const char *chainstate_path, void **out_handle)
{
    if (!chainstate_path || !out_handle)
        LOG_FAIL("chainstate_legacy", "open: NULL arg");

    struct chainstate_legacy_handle *h =
        zcl_malloc(sizeof(*h), "chainstate_legacy_handle");
    if (!h) LOG_FAIL("chainstate_legacy", "malloc handle");
    memset(h, 0, sizeof(*h));

    /* cache_size=8MB, memory=false, wipe=false.  No write traffic from
     * us — read-only walk. */
    if (!db_wrapper_open(&h->db, chainstate_path, 8u << 20, false, false)) {
        free(h);
        LOG_FAIL("chainstate_legacy", "db_wrapper_open failed");
    }

    /* Pre-size scratch for the common case (one vout). */
    if (!ensure_vouts_cap(h, 4)) {
        db_wrapper_close(&h->db);
        free(h);
        LOG_FAIL("chainstate_legacy", "init scratch alloc");
    }

    *out_handle = h;
    return true;
}

void chainstate_legacy_close(void *handle)
{
    if (!handle) return;
    struct chainstate_legacy_handle *h = handle;
    db_wrapper_close(&h->db);
    free(h->vouts_buf);
    free(h->scripts_buf);
    free(h->avail_buf);
    free(h);
}

bool chainstate_legacy_get_best_block(void *handle, struct uint256 *out)
{
    if (!handle || !out)
        LOG_FAIL("chainstate_legacy", "get_best_block: NULL arg");
    struct chainstate_legacy_handle *h = handle;

    const char key = 'B';
    char *val = NULL;
    size_t vlen = 0;
    if (!db_read(&h->db, &key, 1, &val, &vlen) || !val) {
        if (val) free(val);
        LOG_FAIL("chainstate_legacy", "'B' key not found");
    }
    if (vlen != 32) {
        free(val);
        LOG_FAIL("chainstate_legacy", "'B' value not 32 bytes");
    }
    memcpy(out->data, val, 32);
    free(val);
    return true;
}

int64_t chainstate_legacy_iter(void *handle,
                               legacy_chainstate_cb cb,
                               void *ctx)
{
    if (!handle || !cb)
        return -1;
    struct chainstate_legacy_handle *h = handle;

    struct db_iterator it;
    db_iter_init(&it, &h->db);

    /* Seek to the start of the 'c' keyspace. */
    const char seek_key = 'c';
    db_iter_seek(&it, &seek_key, 1);

    int64_t count = 0;
    while (db_iter_valid(&it)) {
        size_t klen = 0;
        const char *k = db_iter_key(&it, &klen);
        if (klen < 1 || k[0] != 'c') break;
        if (klen != 33) {
            /* malformed 'c' record — skip but don't fail the whole
             * scan (gives operators a chance to inspect). */
            db_iter_next(&it);
            continue;
        }

        struct uint256 txid;
        memcpy(txid.data, k + 1, 32);

        size_t vlen = 0;
        const char *v = db_iter_value(&it, &vlen);
        if (!v || vlen == 0) {
            db_iter_next(&it);
            continue;
        }

        struct legacy_coins coins = (struct legacy_coins){0};
        if (!decode_record(h, (const uint8_t *)v, vlen, &coins)) {
            db_iter_free(&it);
            return -1;
        }

        if (!cb(&txid, &coins, ctx)) {
            db_iter_free(&it);
            return count + 1; /* callback "consumed" this record */
        }

        count++;
        db_iter_next(&it);
    }

    db_iter_free(&it);
    return count;
}
