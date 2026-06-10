/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Shared EV_BLOCK_HEADER emitter for the reducer stages.
 *
 * The reducer stages that raise in-memory `struct block_index` status emit
 * EV_BLOCK_HEADER through this helper, so block_index_projection persists the
 * live chain-index state from one shared mapping.
 *
 * Sources scalars from the in-memory `struct block_index` (not the
 * disk_block_index that lib/storage/block_index_db.c serializes). hashPrev is
 * the parent's phashBlock (all-zero for genesis). Best-effort/counted, never
 * fatal — exactly the block_index_db.c semantics. Counters are caller-owned
 * (per-stage observability via zcl_state); pass NULL to skip. */

#ifndef ZCL_JOBS_BLOCK_HEADER_EMIT_H
#define ZCL_JOBS_BLOCK_HEADER_EMIT_H

#include "chain/chain.h"
#include "storage/event_log.h"
#include "storage/event_log_payloads.h"
#include "storage/event_log_singleton.h"
#include "util/log_macros.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

static inline void block_index_emit_header_event(
        const struct block_index *bi, const char *tag,
        _Atomic uint64_t *emit_ok, _Atomic uint64_t *emit_fail)
{
    if (!bi || !bi->phashBlock)
        return;

    event_log_t *log = event_log_singleton();
    if (!log) {
        /* Not wired yet (very early boot, or tests). The projection catches
         * up once boot completes — not a hard failure, not counted. */
        return;
    }

    if (bi->nSolutionSize > EV_BLOCK_HEADER_MAX_SOLUTION) {
        LOG_WARN(tag, "[%s] header emit: solution size %zu > max %u for h=%d; "
                 "skipping", tag, bi->nSolutionSize,
                 (unsigned)EV_BLOCK_HEADER_MAX_SOLUTION, bi->nHeight);
        if (emit_fail)
            atomic_fetch_add_explicit(emit_fail, 1, memory_order_relaxed);
        return;
    }

    struct ev_block_header h;
    memset(&h, 0, sizeof(h));
    memcpy(h.hash, bi->phashBlock->data, 32);
    if (bi->pprev && bi->pprev->phashBlock)
        memcpy(h.hashPrev, bi->pprev->phashBlock->data, 32);
    /* else: genesis — hashPrev stays all-zero (memset above) */
    h.height        = bi->nHeight;
    h.nStatus       = bi->nStatus;
    h.nFile         = bi->nFile;
    h.nDataPos      = bi->nDataPos;
    h.nUndoPos      = bi->nUndoPos;
    h.nTime         = bi->nTime;
    h.nBits         = bi->nBits;
    memcpy(h.nNonce, bi->nNonce.data, 32);
    memcpy(h.hashMerkleRoot, bi->hashMerkleRoot.data, 32);
    memcpy(h.hashFinalSaplingRoot, bi->hashFinalSaplingRoot.data, 32);
    h.nVersion      = bi->nVersion;
    h.nTx           = bi->nTx;
    h.nSolutionSize = (uint16_t)bi->nSolutionSize;

    size_t bufcap = ev_block_header_wire_size(h.nSolutionSize);
    uint8_t stackbuf[256 + 1344];  /* fixed 200 + max solution 1344 */
    if (bufcap > sizeof(stackbuf)) {
        if (emit_fail)
            atomic_fetch_add_explicit(emit_fail, 1, memory_order_relaxed);
        return;
    }
    size_t written = 0;
    if (!ev_block_header_serialize(&h, bi->nSolution, stackbuf, bufcap,
                                   &written)) {
        LOG_WARN(tag, "[%s] header emit: serialize failed h=%d",
                 tag, bi->nHeight);
        if (emit_fail)
            atomic_fetch_add_explicit(emit_fail, 1, memory_order_relaxed);
        return;
    }

    uint64_t off = event_log_append(log, EV_BLOCK_HEADER, stackbuf, written);
    if (off == UINT64_MAX) {
        if (emit_fail)
            atomic_fetch_add_explicit(emit_fail, 1, memory_order_relaxed);
        return;
    }
    if (emit_ok)
        atomic_fetch_add_explicit(emit_ok, 1, memory_order_relaxed);
}

#endif /* ZCL_JOBS_BLOCK_HEADER_EMIT_H */
