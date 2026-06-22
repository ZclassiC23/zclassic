/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * block_parse_cache — implementation. See storage/block_parse_cache.h.
 *
 * DESIGN: the cache retains the canonical SERIALIZED bytes of each block body
 * (the exact block_serialize() wire form), NOT a live `struct block`. A parsed
 * block aliases heap pointers through every tx/script/JoinSplit/Sapling bundle;
 * caching the wire bytes instead makes both the store and the hand-out paths a
 * pure block_serialize <-> block_deserialize round-trip, so the body each
 * consumer receives is byte-identical to what read_block_from_disk_pread would
 * have produced — a COMPLETE, independent clone with no cross-consumer aliasing.
 *
 * On a MISS we read+deserialize once off disk, serialize that body into a
 * cache slot, and return the freshly-read body to the caller (no extra parse).
 * On a HIT we deserialize the cached bytes into the caller's block (one parse,
 * but zero disk I/O and zero re-read). The four downstream stages all hit the
 * body that body_persist (the producer, advancing first) primed.
 *
 * Single mutex; LRU via a monotonically increasing use-stamp. Capacity is
 * small (16) — the working set is the few heights in flight across the five
 * stages, never the whole chain.
 */
#include "storage/block_parse_cache.h"

#include "core/serialize.h"
#include "primitives/block.h"
#include "storage/disk_block_io.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#define BPC_CAPACITY 16

struct bpc_entry {
    bool          used;       /* slot occupied */
    int32_t       height;     /* key part 1 */
    uint8_t       hash[32];   /* key part 2 (block hash) */
    unsigned char *bytes;     /* owned serialized block body (block_serialize) */
    size_t        len;        /* length of bytes */
    uint64_t      stamp;      /* LRU recency (higher = more recent) */
};

static pthread_mutex_t g_bpc_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct bpc_entry g_bpc[BPC_CAPACITY];
static uint64_t g_bpc_clock; /* monotonic LRU stamp source (under mutex) */

/* Deserialize a complete block from `bytes` into `out` (caller block_init'd).
 * Returns true iff the full wire form parsed. Leaves `out` free-able on
 * failure. NOT under the cache mutex — the input buffer is caller-owned. */
static bool bpc_decode(const unsigned char *bytes, size_t len, struct block *out)
{
    struct byte_stream s;
    stream_init_from_data(&s, bytes, len);
    bool ok = block_deserialize(out, &s);
    stream_free(&s);
    return ok;
}

/* Serialize `b` into a freshly zcl_malloc'd buffer; caller owns *out_bytes.
 * Returns false (and frees nothing it did not allocate) on any failure. */
static bool bpc_encode(const struct block *b, unsigned char **out_bytes,
                       size_t *out_len)
{
    struct byte_stream s;
    stream_init(&s, 4096);
    if (!block_serialize(b, &s)) {
        stream_free(&s);
        return false;
    }
    unsigned char *buf = zcl_malloc(s.size ? s.size : 1, "bpc_entry_bytes");
    if (!buf) {
        stream_free(&s);
        return false;
    }
    if (s.size)
        memcpy(buf, s.data, s.size);
    *out_bytes = buf;
    *out_len = s.size;
    stream_free(&s);
    return true;
}

/* Store (or refresh) the (height,hash) -> bytes entry. Takes ownership of
 * `bytes` (frees it on overwrite/eviction). MUST hold g_bpc_mutex. */
static void bpc_store_locked(int32_t height, const uint8_t hash[32],
                             unsigned char *bytes, size_t len)
{
    /* Pass 1: refresh an existing matching key in place. */
    for (int i = 0; i < BPC_CAPACITY; i++) {
        if (g_bpc[i].used && g_bpc[i].height == height &&
            memcmp(g_bpc[i].hash, hash, 32) == 0) {
            free(g_bpc[i].bytes);
            g_bpc[i].bytes = bytes;
            g_bpc[i].len = len;
            g_bpc[i].stamp = ++g_bpc_clock;
            return;
        }
    }

    /* Pass 2: pick the first empty slot, else the LRU (smallest stamp) used
     * slot, and install the new entry there. */
    int victim = 0;
    uint64_t lru_stamp = UINT64_MAX;
    for (int i = 0; i < BPC_CAPACITY; i++) {
        if (!g_bpc[i].used) { victim = i; break; }
        if (g_bpc[i].stamp < lru_stamp) { lru_stamp = g_bpc[i].stamp; victim = i; }
    }

    if (g_bpc[victim].used)
        free(g_bpc[victim].bytes);
    g_bpc[victim].used = true;
    g_bpc[victim].height = height;
    memcpy(g_bpc[victim].hash, hash, 32);
    g_bpc[victim].bytes = bytes;
    g_bpc[victim].len = len;
    g_bpc[victim].stamp = ++g_bpc_clock;
}

bool block_parse_cache_get(int32_t height, const uint8_t block_hash[32],
                           const struct block_index *bi, const char *datadir,
                           struct block *out)
{
    if (!out || !block_hash || !bi)
        return false;

    /* ── HIT path: copy the cached wire bytes out under the lock, then
     *    deserialize OUTSIDE the lock (the parse is the slow part). ── */
    unsigned char *snapshot = NULL;
    size_t snapshot_len = 0;
    pthread_mutex_lock(&g_bpc_mutex);
    for (int i = 0; i < BPC_CAPACITY; i++) {
        if (g_bpc[i].used && g_bpc[i].height == height &&
            memcmp(g_bpc[i].hash, block_hash, 32) == 0) {
            g_bpc[i].stamp = ++g_bpc_clock;       /* touch for LRU */
            snapshot = zcl_malloc(g_bpc[i].len ? g_bpc[i].len : 1,
                                  "bpc_hit_snapshot");
            if (snapshot) {
                if (g_bpc[i].len)
                    memcpy(snapshot, g_bpc[i].bytes, g_bpc[i].len);
                snapshot_len = g_bpc[i].len;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_bpc_mutex);

    if (snapshot) {
        bool ok = bpc_decode(snapshot, snapshot_len, out);
        free(snapshot);
        if (ok)
            return true;
        /* A cached-byte decode failure should never happen (we serialized it
         * ourselves); fall through to a fresh disk read rather than fail. */
        block_free(out);
        block_init(out);
        LOG_WARN("block_parse_cache",
                 "cached-body decode failed h=%d; re-reading from disk", height);
    }

    /* ── MISS path: read+parse from disk once, BYTE-IDENTICALLY to what
     *    stage_default_block_reader did — same HAVE_DATA guard, same
     *    (nFile,nDataPos), same read_block_from_disk_pread, and DELIBERATELY
     *    no post-read hash check (the default reader has none; body_persist
     *    does its own hash compare and a reader hash-reject would change its
     *    refetch classification). Then cache a serialized copy and hand the
     *    freshly-read body to the caller (no extra parse). ── */
    if (!(bi->nStatus & BLOCK_HAVE_DATA))
        return false;
    struct disk_block_pos pos;
    disk_block_pos_init(&pos);
    pos.nFile = bi->nFile;
    pos.nPos = bi->nDataPos;
    if (!read_block_from_disk_pread(out, &pos, datadir ? datadir : "")) {
        /* out is left free-able by the reader's failure contract. */
        return false;
    }

    /* Cache a serialized copy of the body for the downstream stages. A cache
     * failure is non-fatal: the caller already has a valid body. */
    unsigned char *bytes = NULL;
    size_t len = 0;
    if (bpc_encode(out, &bytes, &len)) {
        pthread_mutex_lock(&g_bpc_mutex);
        bpc_store_locked(height, block_hash, bytes, len);  /* takes ownership */
        pthread_mutex_unlock(&g_bpc_mutex);
    }
    return true;
}

void block_parse_cache_clear(void)
{
    pthread_mutex_lock(&g_bpc_mutex);
    for (int i = 0; i < BPC_CAPACITY; i++) {
        if (g_bpc[i].used) {
            free(g_bpc[i].bytes);
            g_bpc[i].bytes = NULL;
        }
        g_bpc[i].used = false;
        g_bpc[i].len = 0;
        g_bpc[i].stamp = 0;
    }
    pthread_mutex_unlock(&g_bpc_mutex);
}
