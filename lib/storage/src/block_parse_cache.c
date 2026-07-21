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
 * BLOCK_PARSE_CACHE_CAPACITY (2048, see block_parse_cache.h) — sized for the
 * offline refold's batch cadence, not the small live in-flight window this
 * cache was originally tuned for. The refold drives all five stages
 * (body_persist, script_validate, proof_validate, utxo_apply,
 * tip_finalize_post_step) across the SAME batch of up to 2000 heights before
 * advancing; with the old capacity of 16, body_persist (the producer, which
 * runs first) evicted its own entries ~125x over before the last downstream
 * stage ever reached the same range, so four of the five stages re-read and
 * re-parsed every block from disk anyway. Capacity must stay >= the refold
 * batch size so a full batch survives across all five stages; live/steady-
 * state operation (a handful of in-flight heights) is unaffected either way.
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

#define BPC_CAPACITY BLOCK_PARSE_CACHE_CAPACITY

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
 * body, so a missed cache install only forfeits a future hit.
 *
 * CALLER CONTRACT (load-bearing invariant): every entry this function installs
 * MUST already be known to hash to its `hash` key — bpc_store_locked itself
 * does not re-derive or check the hash, it trusts the caller. Both call sites
 * uphold this: the segment path (bpc_segment_try) re-derives and compares
 * before ever reaching here, and the blk*.dat MISS path in
 * block_parse_cache_get() now does the same recompute-and-compare
 * immediately before calling in. This keeps every resident cache entry
 * hash-verified, so a bad disk read can never poison a slot that a later,
 * correctly-read body would then be served from — see the MISS path comment
 * below for the failure mode this closes. */
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
     *    no post-read REJECTION (the default reader has none; body_persist
     *    does its own hash compare and a reader hash-reject would change its
     *    refetch classification, so `out` is always handed back to the caller
     *    exactly as read — the caller's own gate decides accept/reject).
     *
     *    What we DO check, before ever installing into the cache: whether the
     *    body we just read actually hashes to the requested key. A bad disk
     *    read (torn write, stale bytes at a recycled (nFile,nDataPos), a
     *    genuinely corrupt sector) can return bytes that parse fine but don't
     *    belong to `block_hash`. If we cached those under (height,block_hash)
     *    anyway, the consuming stage's own inline hash check would correctly
     *    reject `out` on THIS call — but every subsequent refetch of the same
     *    key would then HIT the poisoned slot and be served the same wrong
     *    bytes forever, a stuck-refetch liveness wedge that only a whole-cache
     *    memory-pressure clear could break. So: cache only when the recomputed
     *    hash matches; on a mismatch `out` is still returned to the caller
     *    (today's contract is unchanged) but the slot is simply never
     *    installed, leaving the key free for a correct read to populate later.
     *    The segment path above already enforces this same equality
     *    (bpc_segment_try re-derives + compares before returning true), so
     *    every resident entry — segment- or disk-sourced — is hash-verified. */
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

    /* Verify-before-store: only cache a deep clone when the body we just read
     * actually hashes to the requested key. See the MISS path comment above
     * for why an unverified store is a liveness hazard. A cache failure (skip
     * or clone OOM) is non-fatal either way: the caller already has a valid
     * `out` (bpc_store_locked leaves the slot unused on a clone failure). */
    struct uint256 got_hash;
    block_get_hash(out, &got_hash);
    if (memcmp(got_hash.data, block_hash, 32) == 0) {
        pthread_mutex_lock(&g_bpc_mutex);
        bpc_store_locked(height, block_hash, out);
        pthread_mutex_unlock(&g_bpc_mutex);
    } else {
        LOG_WARN("block_parse_cache",
                 "MISS body hash mismatch h=%d — not caching (bad read?)",
                 height);
    }
    return true;
}

void block_parse_cache_evict(int32_t height, const uint8_t hash[32])
{
    if (!hash)
        return;
    pthread_mutex_lock(&g_bpc_mutex);
    for (int i = 0; i < BPC_CAPACITY; i++) {
        if (g_bpc[i].used && g_bpc[i].height == height &&
            memcmp(g_bpc[i].hash, hash, 32) == 0) {
            block_free(&g_bpc[i].body);
            g_bpc[i].used = false;
            g_bpc[i].stamp = 0;
            break; /* (height,hash) keys are unique across live slots */
        }
    }
    pthread_mutex_unlock(&g_bpc_mutex);
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
