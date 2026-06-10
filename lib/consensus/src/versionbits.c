/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * versionbits — miner-signaled Equihash activation. See
 * consensus/versionbits.h for the state machine contract and
 * docs/design/equihash-200-9-versionbits.md for the rationale. */

#include "consensus/versionbits.h"
#include "chain/chain.h"
#include "core/uint256.h"
#include "util/log_macros.h"

#include <pthread.h>
#include <string.h>

/* Per-boundary verdict cache. Keyed by the hash of the last block of a
 * completed window, so verdicts are immutable facts about a specific
 * branch: a reorg simply queries different keys. Fixed-size open
 * addressing with bounded probes; a full neighborhood overwrites the
 * probe-end slot (it is a cache — recompute is always possible). */
#define VBITS_CACHE_SLOTS 4096
#define VBITS_CACHE_PROBES 8

struct vbits_cache_entry {
    struct uint256 key;          /* boundary block hash */
    int32_t streak;              /* consecutive passing windows through here */
    int32_t locked_in_height;    /* -1 until locked */
    bool occupied;
};

static struct vbits_cache_entry g_cache[VBITS_CACHE_SLOTS];
static pthread_mutex_t g_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t cache_slot_base(const struct uint256 *h)
{
    uint64_t v;
    memcpy(&v, h->data, sizeof(v));
    return (size_t)(v & (VBITS_CACHE_SLOTS - 1));
}

static bool cache_get(const struct uint256 *key, int32_t *streak,
                      int32_t *locked_in_height)
{
    bool hit = false;
    pthread_mutex_lock(&g_cache_lock);
    size_t base = cache_slot_base(key);
    for (size_t i = 0; i < VBITS_CACHE_PROBES; i++) {
        struct vbits_cache_entry *e =
            &g_cache[(base + i) & (VBITS_CACHE_SLOTS - 1)];
        if (e->occupied && uint256_eq(&e->key, key)) {
            *streak = e->streak;
            *locked_in_height = e->locked_in_height;
            hit = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_cache_lock);
    return hit;
}

static void cache_put(const struct uint256 *key, int32_t streak,
                      int32_t locked_in_height)
{
    pthread_mutex_lock(&g_cache_lock);
    size_t base = cache_slot_base(key);
    struct vbits_cache_entry *victim =
        &g_cache[(base + VBITS_CACHE_PROBES - 1) & (VBITS_CACHE_SLOTS - 1)];
    for (size_t i = 0; i < VBITS_CACHE_PROBES; i++) {
        struct vbits_cache_entry *e =
            &g_cache[(base + i) & (VBITS_CACHE_SLOTS - 1)];
        if (!e->occupied || uint256_eq(&e->key, key)) {
            victim = e;
            break;
        }
    }
    victim->key = *key;
    victim->streak = streak;
    victim->locked_in_height = locked_in_height;
    victim->occupied = true;
    pthread_mutex_unlock(&g_cache_lock);
}

void versionbits_cache_reset(void)
{
    pthread_mutex_lock(&g_cache_lock);
    memset(g_cache, 0, sizeof(g_cache));
    pthread_mutex_unlock(&g_cache_lock);
}

const char *vbits_state_name(enum vbits_state s)
{
    switch (s) {
    case VBITS_DISABLED:  return "disabled";
    case VBITS_DEFINED:   return "defined";
    case VBITS_LOCKED_IN: return "locked_in";
    case VBITS_ACTIVE:    return "active";
    }
    return "unknown";
}

static bool block_signals(const struct block_index *bi,
                          const struct eh_upgrade_deployment *d)
{
    return (bi->nVersion & (1 << d->nSignalBit)) != 0;
}

/* Count signaling blocks in the window whose LAST block is `top`
 * (heights [top->nHeight - W + 1, top->nHeight]). Fails when the walk
 * runs out of ancestry or heights are non-monotonic. */
static struct zcl_result window_signal_count(const struct block_index *top,
                                             int64_t window,
                                             const struct eh_upgrade_deployment *d,
                                             int64_t *out)
{
    int64_t count = 0;
    const struct block_index *bi = top;
    for (int64_t i = 0; i < window; i++) {
        if (!bi)
            return ZCL_ERR(-71,
                "versionbits: window ancestry incomplete below h=%d "
                "(%lld of %lld blocks walkable)",
                top->nHeight, (long long)i, (long long)window);
        if (bi->pprev && bi->pprev->nHeight != bi->nHeight - 1)
            return ZCL_ERR(-72,
                "versionbits: non-monotonic ancestry at h=%d (parent h=%d)",
                bi->nHeight, bi->pprev->nHeight);
        if (block_signals(bi, d))
            count++;
        bi = bi->pprev;
    }
    *out = count;
    return ZCL_OK;
}

/* Resolve {streak, locked_in_height} as of the boundary block `anchor`
 * (the last block of a completed window). Walks DOWN window by window
 * to the nearest cached boundary (or window 0), then scores forward. */
static struct zcl_result boundary_state(const struct block_index *anchor,
                                        const struct eh_upgrade_deployment *d,
                                        int32_t *streak_out,
                                        int32_t *locked_in_out)
{
    const int64_t W = d->nWindow;

    int32_t streak = 0;
    int32_t locked_in = -1;
    int64_t resume_from;         /* window index to score next */

    int64_t k_anchor = ((int64_t)anchor->nHeight + 1) / W;   /* windows complete */
    const struct block_index *walk = anchor;
    resume_from = 0;
    while (true) {
        if (cache_get(&walk->hashBlock, &streak, &locked_in)) {
            resume_from = ((int64_t)walk->nHeight + 1) / W;
            break;
        }
        if (((int64_t)walk->nHeight + 1) / W <= 1) {
            resume_from = 0;
            streak = 0;
            locked_in = -1;
            break;
        }
        const struct block_index *lower = block_index_get_ancestor(
            (struct block_index *)walk, (int)(walk->nHeight - W));
        if (!lower || lower->nHeight != walk->nHeight - W)
            return ZCL_ERR(-73,
                "versionbits: boundary ancestry incomplete below h=%d",
                walk->nHeight);
        walk = lower;
    }

    for (int64_t k = resume_from; k < k_anchor; k++) {
        const struct block_index *top = block_index_get_ancestor(
            (struct block_index *)anchor, (int)((k + 1) * W - 1));
        if (!top || (int64_t)top->nHeight != (k + 1) * W - 1)
            return ZCL_ERR(-74,
                "versionbits: missing boundary block for window %lld",
                (long long)k);
        if (locked_in < 0) {
            int64_t count = 0;
            struct zcl_result r = window_signal_count(top, W, d, &count);
            if (!r.ok)
                return r;
            streak = (count >= d->nThreshold) ? streak + 1 : 0;
            if (streak >= d->nConsecutiveWindows)
                locked_in = (int32_t)((k + 1) * W);
        }
        cache_put(&top->hashBlock, streak, locked_in);
    }

    *streak_out = streak;
    *locked_in_out = locked_in;
    return ZCL_OK;
}

struct zcl_result versionbits_eh_query(const struct consensus_params *p,
                                       const struct block_index *pindex_prev,
                                       struct vbits_info *out)
{
    if (!p || !out)
        return ZCL_ERR(-70, "versionbits: NULL params/out");
    memset(out, 0, sizeof(*out));
    out->locked_in_height = -1;
    out->active_height = -1;
    out->last_boundary_height = -1;

    const struct eh_upgrade_deployment *d = &p->ehUpgrade;
    if (!d->enabled) {
        out->state = VBITS_DISABLED;
        return ZCL_OK;
    }
    if (d->nWindow <= 0 || d->nThreshold <= 0 ||
        d->nThreshold > d->nWindow || d->nConsecutiveWindows <= 0 ||
        d->nGraceBlocks < 0 || d->nSignalBit < 0 || d->nSignalBit > 30)
        return ZCL_ERR(-75, "versionbits: malformed deployment params");

    out->state = VBITS_DEFINED;
    if (!pindex_prev)
        return ZCL_OK;   /* genesis is next: nothing to tally yet */

    const int64_t W = d->nWindow;
    int64_t next_h = (int64_t)pindex_prev->nHeight + 1;
    int64_t complete = next_h / W;   /* fully scored windows below next_h */

    int32_t streak = 0, locked_in = -1;
    if (complete > 0) {
        const struct block_index *anchor = block_index_get_ancestor(
            (struct block_index *)pindex_prev, (int)(complete * W - 1));
        if (!anchor || (int64_t)anchor->nHeight != complete * W - 1)
            return ZCL_ERR(-76,
                "versionbits: boundary block h=%lld unreachable from h=%d",
                (long long)(complete * W - 1), pindex_prev->nHeight);
        struct zcl_result r = boundary_state(anchor, d, &streak, &locked_in);
        if (!r.ok)
            return r;
        out->last_boundary_height = (int)(complete * W - 1);
    }

    out->streak = streak;
    out->locked_in_height = locked_in;
    if (locked_in >= 0) {
        out->active_height = (int)(locked_in + d->nGraceBlocks);
        out->state = next_h >= out->active_height ? VBITS_ACTIVE
                                                  : VBITS_LOCKED_IN;
    }

    /* Diagnostics only: signals in the current partial window. */
    if (locked_in < 0 && next_h % W != 0) {
        int64_t partial = next_h % W;
        struct zcl_result r = window_signal_count(pindex_prev, partial, d,
                                                  &out->window_signal_count);
        if (!r.ok)
            out->window_signal_count = -1;   /* non-fatal: tally view only */
    }
    return ZCL_OK;
}

bool versionbits_eh_active(const struct consensus_params *p,
                           const struct block_index *pindex_prev,
                           int *active_height_out)
{
    if (active_height_out)
        *active_height_out = -1;
    struct vbits_info info;
    struct zcl_result r = versionbits_eh_query(p, pindex_prev, &info);
    if (!r.ok)
        return false;
    if (active_height_out)
        *active_height_out = info.active_height;
    return info.state == VBITS_ACTIVE;
}
