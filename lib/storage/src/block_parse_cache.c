/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * block_parse_cache — implementation. See storage/block_parse_cache.h.
 *
 * DESIGN: the cache retains a fully PARSED `struct block` per (height,hash) —
 * deserialized ONCE off disk on the miss — and hands each downstream consumer
 * an independent DEEP COPY of that parsed block via block_clone, instead of
 * re-running block_deserialize from cached wire bytes on every hit. The clone
 * is a struct-level deep copy (header POD copy + transaction_copy per tx), so
 * it touches no stream parse, no compact-size decode, and no per-script bounds
 * check — it is strictly cheaper than the prior cached-bytes re-deserialize
 * while remaining byte-identical under block_serialize to a fresh
 * read_block_from_disk_pread (transaction_copy preserves every wire-affecting
 * field + copies tx->hash verbatim, and block_clone copies the header verbatim).
 *
 * The clone is COMPLETE and INDEPENDENT: it shares no heap with the cache entry
 * or any sibling clone, and the caller frees it with block_free exactly as
 * before. The cache owns its own retained parsed block; eviction frees it
 * independently of any handed-out clone.
 *
 * On a MISS we read+deserialize once off disk into the caller's `out`, then
 * store a clone of that body in a cache slot; the caller keeps the freshly-read
 * body (no extra clone on the producer path). On a HIT we hand back a clone of
 * the retained parsed block (one deep copy, zero disk I/O, zero re-parse). The
 * four downstream stages all hit the body that body_persist (the producer,
 * advancing first) primed.
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
    struct block  body;       /* owned PARSED block body (deep-owned) */
    uint64_t      stamp;      /* LRU recency (higher = more recent) */
};

static pthread_mutex_t g_bpc_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct bpc_entry g_bpc[BPC_CAPACITY];
static uint64_t g_bpc_clock; /* monotonic LRU stamp source (under mutex) */

/* Store (or refresh) the (height,hash) -> parsed-body entry by taking a deep
 * clone of `src` into the slot. The slot's prior body (if any) is block_free'd.
 * MUST hold g_bpc_mutex. A clone failure leaves the slot UNUSED (freed/empty)
 * rather than installing a partial body — the producer already has a valid
 * body, so a missed cache install only forfeits a future hit. */
static void bpc_store_locked(int32_t height, const uint8_t hash[32],
                             const struct block *src)
{
    /* Pass 1: refresh an existing matching key in place. */
    for (int i = 0; i < BPC_CAPACITY; i++) {
        if (g_bpc[i].used && g_bpc[i].height == height &&
            memcmp(g_bpc[i].hash, hash, 32) == 0) {
            block_free(&g_bpc[i].body);
            if (!block_clone(&g_bpc[i].body, src)) {
                /* block_clone left body empty/free-safe; drop the slot. */
                g_bpc[i].used = false;
                g_bpc[i].stamp = 0;
                return;
            }
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
        block_free(&g_bpc[victim].body);
    if (!block_clone(&g_bpc[victim].body, src)) {
        /* block_clone left body empty/free-safe; leave the slot unused. */
        g_bpc[victim].used = false;
        g_bpc[victim].stamp = 0;
        return;
    }
    g_bpc[victim].used = true;
    g_bpc[victim].height = height;
    memcpy(g_bpc[victim].hash, hash, 32);
    g_bpc[victim].stamp = ++g_bpc_clock;
}

bool block_parse_cache_get(int32_t height, const uint8_t block_hash[32],
                           const struct block_index *bi, const char *datadir,
                           struct block *out)
{
    if (!out || !block_hash || !bi)
        return false;

    /* ── HIT path: clone the cached parsed body into `out` under the lock.
     *    The clone is a struct deep copy (no stream parse) and is independent
     *    of the cache entry, so it safely outlives any later eviction. ── */
    pthread_mutex_lock(&g_bpc_mutex);
    for (int i = 0; i < BPC_CAPACITY; i++) {
        if (g_bpc[i].used && g_bpc[i].height == height &&
            memcmp(g_bpc[i].hash, block_hash, 32) == 0) {
            g_bpc[i].stamp = ++g_bpc_clock;       /* touch for LRU */
            bool ok = block_clone(out, &g_bpc[i].body);
            pthread_mutex_unlock(&g_bpc_mutex);
            if (ok)
                return true;
            /* A clone failure (OOM) should be rare; fall back to a fresh disk
             * read rather than fail. block_clone left `out` free-safe. */
            block_free(out);
            block_init(out);
            LOG_WARN("block_parse_cache",
                     "cached-body clone failed h=%d; re-reading from disk",
                     height);
            goto miss;
        }
    }
    pthread_mutex_unlock(&g_bpc_mutex);

miss:
    /* ── MISS path: read+parse from disk once, BYTE-IDENTICALLY to what
     *    stage_default_block_reader did — same HAVE_DATA guard, same
     *    (nFile,nDataPos), same read_block_from_disk_pread, and DELIBERATELY
     *    no post-read hash check (the default reader has none; body_persist
     *    does its own hash compare and a reader hash-reject would change its
     *    refetch classification). Then cache a deep clone and hand the
     *    freshly-read body to the caller (no extra parse, no extra clone). ── */
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

    /* Cache a deep clone of the body for the downstream stages. A cache failure
     * is non-fatal: the caller already has a valid body (bpc_store_locked just
     * leaves the slot unused on a clone failure). */
    pthread_mutex_lock(&g_bpc_mutex);
    bpc_store_locked(height, block_hash, out);
    pthread_mutex_unlock(&g_bpc_mutex);
    return true;
}

void block_parse_cache_clear(void)
{
    pthread_mutex_lock(&g_bpc_mutex);
    for (int i = 0; i < BPC_CAPACITY; i++) {
        if (g_bpc[i].used)
            block_free(&g_bpc[i].body);
        g_bpc[i].used = false;
        g_bpc[i].stamp = 0;
    }
    pthread_mutex_unlock(&g_bpc_mutex);
}
