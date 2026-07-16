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

#include "chain/chain.h"
#include "core/serialize.h"
#include "primitives/block.h"
#include "storage/chain_segment.h"
#include "storage/disk_block_io.h"
#include "util/log_macros.h"
#include "util/mem_pressure.h"
#include "util/safe_alloc.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
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

/* ── mem_pressure sink (Rung 1 follow-on, docs/adr/0003-os-substrate-
 * verdict.md) ──────────────────────────────────────────────────────
 * A REAL shrink target: every entry is a read-through cache of already-
 * persisted disk data (re-derivable by the next block_parse_cache_get()
 * miss), so dropping it under memory pressure loses nothing but a few
 * upcoming cache hits — never consensus state. Registered once, lazily,
 * on the first cache use (pthread_once — this module has no explicit
 * init entry point). */
static void bpc_shrink_for_pressure(enum mem_pressure_level level, void *ctx)
{
    (void)level;
    (void)ctx;
    block_parse_cache_clear();
}

static struct mem_pressure_sink g_bpc_pressure_sink = {
    .name = "block_parse_cache",
    .shrink = bpc_shrink_for_pressure,
    .ctx = NULL,
};

static pthread_once_t g_bpc_pressure_once = PTHREAD_ONCE_INIT;

static void bpc_register_pressure_sink(void)
{
    (void)mem_pressure_register_sink(&g_bpc_pressure_sink);
}

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

/* ── Sealed-segment source (the fold substrate) ──────────────────────────
 * Below the sealed frontier a finalized body is read from an mmap'd, sealed
 * segment (sequential + hash-verified) instead of a blk*.dat pread. The store
 * is opened lazily on the first miss and keyed to a datadir; a datadir change
 * (or the absence of a segments dir) reopens/empties it. Because segments store
 * exactly the block_serialize output, deserializing the segment bytes yields a
 * struct byte-identical to a fresh disk read — and we additionally re-derive
 * the block hash and require it to equal the caller's active-chain hash before
 * serving, so a segment on a different fork can never be substituted. Any miss
 * or mismatch falls through to the unchanged blk*.dat path, so a node with no
 * segments is byte-for-byte unchanged. */
static pthread_mutex_t g_seg_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct chain_segment_store *g_seg_store; /* NULL until first open */
static char g_seg_datadir[3072];
static bool g_seg_open_tried;                   /* store open attempted for g_seg_datadir */

/* Ensure the resident store matches `datadir`. MUST hold g_seg_mutex. */
static void bpc_segment_sync_store_locked(const char *datadir)
{
    if (g_seg_open_tried && strncmp(g_seg_datadir, datadir,
                                    sizeof(g_seg_datadir)) == 0)
        return; /* already opened for this datadir */

    if (g_seg_store) {
        chain_segment_store_close(g_seg_store);
        g_seg_store = NULL;
    }
    snprintf(g_seg_datadir, sizeof(g_seg_datadir), "%s", datadir);
    g_seg_open_tried = true;

    char dir[3200];
    snprintf(dir, sizeof(dir), "%s/segments", datadir);
    char err[256] = {0};
    struct chain_segment_store *s = NULL;
    enum cseg_status st = chain_segment_store_open(dir, &s, err, sizeof(err));
    if (st != CSEG_OK) {
        LOG_WARN("block_parse_cache",
                 "segment store open failed dir=%s: %s (%s) — using blk*.dat",
                 dir, cseg_status_str(st), err);
        return;
    }
    g_seg_store = s; /* may cover nothing; that is fine */
}

/* Try to fill `out` for (height, block_hash) from a sealed segment. Returns
 * true only when a covering segment yielded a body whose re-derived hash equals
 * `block_hash`. On any negative outcome `out` is left free-safe and false is
 * returned so the caller uses the disk path. */
static bool bpc_segment_try(int32_t height, const uint8_t block_hash[32],
                            const char *datadir, struct block *out)
{
    if (!datadir || !datadir[0] || height < 0)
        return false;

    pthread_mutex_lock(&g_seg_mutex);
    bpc_segment_sync_store_locked(datadir);
    struct chain_segment_store *store = g_seg_store;
    if (!store || !chain_segment_store_covers(store, (uint32_t)height)) {
        pthread_mutex_unlock(&g_seg_mutex);
        return false;
    }
    uint8_t *raw = NULL;
    size_t rawlen = 0;
    char err[256] = {0};
    enum cseg_status st = chain_segment_store_get_block(
        store, (uint32_t)height, &raw, &rawlen, err, sizeof(err));
    pthread_mutex_unlock(&g_seg_mutex);

    if (st != CSEG_OK || !raw || rawlen == 0) {
        if (st != CSEG_OK)
            LOG_WARN("block_parse_cache",
                     "segment read h=%d: %s (%s) — falling back to blk*.dat",
                     height, cseg_status_str(st), err);
        free(raw);
        return false;
    }

    struct byte_stream s;
    stream_init_from_data(&s, raw, rawlen);
    bool parsed = block_deserialize(out, &s);
    stream_free(&s);
    free(raw);
    if (!parsed) {
        block_free(out);
        block_init(out);
        LOG_WARN("block_parse_cache",
                 "segment body deserialize failed h=%d — falling back", height);
        return false;
    }

    /* Bind the served body to the caller's active-chain hash: a segment on a
     * different fork (or a stale height mapping) must never be substituted. */
    struct uint256 got;
    block_get_hash(out, &got);
    if (memcmp(got.data, block_hash, 32) != 0) {
        block_free(out);
        block_init(out);
        return false;
    }
    return true;
}

bool block_parse_cache_get(int32_t height, const uint8_t block_hash[32],
                           const struct block_index *bi, const char *datadir,
                           struct block *out)
{
    if (!out || !block_hash || !bi)
        return false;

    pthread_once(&g_bpc_pressure_once, bpc_register_pressure_sink);

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
    /* Prefer a sealed segment below the frontier (sequential mmap read, hash-
     * verified, byte-identical to the disk body). A miss/mismatch falls through
     * to the unchanged blk*.dat pread. */
    if (!bpc_segment_try(height, block_hash, datadir, out)) {
        struct disk_block_pos pos;
        disk_block_pos_init(&pos);
        if (!block_index_disk_pos_snapshot(bi, &pos, NULL))
            return false;
        if (!read_block_from_disk_pread(out, &pos, datadir ? datadir : "")) {
            /* out is left free-able by the reader's failure contract. */
            return false;
        }
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
