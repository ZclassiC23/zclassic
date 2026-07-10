/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* msg_headers.c — Header sync message processing.
 * Split from msgprocessor.c for maintainability. */

#include "net/msg_internal.h"
#include "net/msg_bounds_guard.h"
#include "net/p2p_message.h"
#include "net/peer_scoring.h"
#include "net/fast_sync.h"
#include "sync/sync_planner.h"
#include "storage/disk_block_io.h"
#include "validation/check_block.h"
#include "validation/process_block.h"
#include "net/download.h"
#include "event/event.h"
#include "sync/sync_state.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/timedata.h"
#include "coins/coins_view.h"
#include "chain/equihash.h"
#include "chain/pow.h"
#include "core/arith_uint256.h"
#include <signal.h>
extern volatile sig_atomic_t g_shutdown_requested;
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

/* Header sync stall tracking state is in msg_send_messages (msgprocessor.c).
 * This file handles the receive-side header processing. */

#include <stdatomic.h>

/* ── Sync diagnostic counters ──────────────────────────────────── */

static _Atomic uint64_t g_headers_batches_received = 0;
static _Atomic uint64_t g_headers_total_accepted = 0;
static _Atomic uint64_t g_headers_total_rejected = 0;
static _Atomic uint64_t g_headers_newly_added = 0;
static _Atomic uint64_t g_headers_already_known = 0;

/* Per-peer header advancement tracking (simplified: tracks last peer). */
static _Atomic int g_last_header_tip_height = 0;

static size_t collect_active_tip_successors(struct main_state *ms,
                                            struct uint256 *hashes,
                                            int32_t *heights,
                                            size_t max_collect,
                                            bool *has_data_successor)
{
    struct block_index *parent;
    size_t count = 0;

    if (has_data_successor)
        *has_data_successor = false;
    if (!ms || !hashes || !heights || max_collect == 0)
        return 0;

    parent = active_chain_tip(&ms->chain_active);
    while (parent && parent->phashBlock && count < max_collect) {
        /* O(log) per hop via the shared successor (best-header path
         * above the tip) — the old inline scan here visited the whole
         * block map per collected successor. */
        struct block_index *best_child =
            main_state_best_known_successor(ms, parent);

        if (!best_child || !best_child->phashBlock)
            break;

        if (!(best_child->nStatus & BLOCK_HAVE_DATA)) {
            hashes[count] = *best_child->phashBlock;
            heights[count] = best_child->nHeight;
            count++;
        } else if (has_data_successor) {
            *has_data_successor = true;
        }

        parent = best_child;
    }

    return count;
}

static bool headers_fill_header_from_index(struct msg_processor *mp,
                                           struct block_index *iter,
                                           struct block_header *hdr)
{
    if (!mp || !iter || !hdr)
        return false;

    block_header_init(hdr);
    hdr->nVersion = iter->nVersion;
    if (iter->pprev && iter->pprev->phashBlock)
        hdr->hashPrevBlock = *iter->pprev->phashBlock;
    else
        memset(&hdr->hashPrevBlock, 0, sizeof(hdr->hashPrevBlock));
    hdr->hashMerkleRoot = iter->hashMerkleRoot;
    hdr->hashFinalSaplingRoot = iter->hashFinalSaplingRoot;
    hdr->nTime = iter->nTime;
    hdr->nBits = iter->nBits;
    hdr->nNonce = iter->nNonce;
    if (iter->nSolution && iter->nSolutionSize > 0) {
        if (iter->nSolutionSize > sizeof(hdr->nSolution)) {
            LOG_WARN("headers",
                     "getheaders: oversized in-memory solution h=%d size=%zu",
                     iter->nHeight, iter->nSolutionSize);
            return false;
        }
        memcpy(hdr->nSolution, iter->nSolution, iter->nSolutionSize);
        hdr->nSolutionSize = iter->nSolutionSize;
        return true;
    }

    struct block blk_tmp;
    block_init(&blk_tmp);
    if (read_block_from_disk_index(&blk_tmp, iter, mp->datadir)) {
        if (blk_tmp.header.nSolutionSize > sizeof(hdr->nSolution)) {
            LOG_WARN("headers",
                     "getheaders: oversized on-disk solution h=%d size=%zu",
                     iter->nHeight, blk_tmp.header.nSolutionSize);
            block_free(&blk_tmp);
            return false;
        }
        memcpy(hdr->nSolution, blk_tmp.header.nSolution,
               blk_tmp.header.nSolutionSize);
        hdr->nSolutionSize = blk_tmp.header.nSolutionSize;
        block_free(&blk_tmp);
        return true;
    }
    block_free(&blk_tmp);
    return true;
}

static bool headers_reject_reason_permanent(const char *reason)
{
    return reason &&
        (strcmp(reason, "invalid-solution") == 0 ||
         strcmp(reason, "high-hash") == 0 ||
         strcmp(reason, "bad-equihash-solution-size") == 0 ||
         strcmp(reason, "version-too-low") == 0);
}

static const char *headers_servable_reject_reason(
    struct msg_processor *mp,
    const struct block_index *iter,
    const struct block_header *hdr)
{
    if (!mp || !iter || !hdr)
        return "invalid-args";

    if (hdr->nVersion < MIN_BLOCK_VERSION)
        return "version-too-low";

    if (hdr->nSolutionSize > 0 && iter->nHeight >= 0) {
        unsigned int eh_n = chain_params_equihash_n(mp->params,
                                                    iter->nHeight);
        unsigned int eh_k = chain_params_equihash_k(mp->params,
                                                    iter->nHeight);
        size_t expected = ((size_t)1 << eh_k) *
            (eh_n / (eh_k + 1) + 1) / 8;
        if (hdr->nSolutionSize != expected)
            return "bad-equihash-solution-size";
    }

    if (!check_equihash_solution(hdr, mp->params))
        return "invalid-solution";

    struct uint256 hash;
    block_header_get_hash(hdr, &hash);
    if (!CheckProofOfWork(hash, hdr->nBits, &mp->params->consensus))
        return "high-hash";

    if (block_header_get_time(hdr) > GetAdjustedTime() + 2 * 60 * 60)
        return "time-too-new";

    return NULL;
}

static bool headers_reject_reason_can_retry_disk(const char *reason)
{
    return reason &&
        (strcmp(reason, "invalid-solution") == 0 ||
         strcmp(reason, "bad-equihash-solution-size") == 0 ||
         strcmp(reason, "high-hash") == 0);
}

static void headers_refresh_index_from_header(struct block_index *iter,
                                              const struct block_header *hdr)
{
    if (!iter || !hdr)
        return;

    iter->nVersion = hdr->nVersion;
    iter->hashMerkleRoot = hdr->hashMerkleRoot;
    iter->hashFinalSaplingRoot = hdr->hashFinalSaplingRoot;
    iter->nTime = hdr->nTime;
    iter->nBits = hdr->nBits;
    iter->nNonce = hdr->nNonce;

    if (hdr->nSolutionSize > 0) {
        uint8_t *sol = zcl_malloc(hdr->nSolutionSize,
                                  "headers_refresh_solution");
        if (!sol) {
            LOG_WARN("headers",
                     "getheaders: solution refresh alloc failed h=%d size=%zu",
                     iter->nHeight, hdr->nSolutionSize);
            return;
        }
        memcpy(sol, hdr->nSolution, hdr->nSolutionSize);
        free(iter->nSolution);
        iter->nSolution = sol;
        iter->nSolutionSize = hdr->nSolutionSize;
    }
}

static bool headers_try_disk_header(struct msg_processor *mp,
                                    struct block_index *iter,
                                    struct block_header *hdr_out)
{
    if (!mp || !iter || !hdr_out)
        return false;

    struct block blk;
    block_init(&blk);
    if (!read_block_from_disk_index(&blk, iter, mp->datadir)) {
        block_free(&blk);
        return false;
    }

    struct uint256 disk_hash;
    block_header_get_hash(&blk.header, &disk_hash);
    bool same_hash = iter->phashBlock &&
        uint256_eq(&disk_hash, iter->phashBlock);
    if (!same_hash) {
        char disk_hex[65], index_hex[65];
        uint256_get_hex(&disk_hash, disk_hex);
        if (iter->phashBlock)
            uint256_get_hex(iter->phashBlock, index_hex);
        else
            strcpy(index_hex, "(missing)");
        LOG_WARN("headers",
                 "getheaders: disk header hash mismatch h=%d index=%s disk=%s",
                 iter->nHeight, index_hex, disk_hex);
        block_free(&blk);
        return false;
    }

    *hdr_out = blk.header;
    headers_refresh_index_from_header(iter, hdr_out);
    block_free(&blk);
    return true;
}

static bool headers_index_header_servable(struct msg_processor *mp,
                                          struct block_index *iter,
                                          struct block_header *hdr_out)
{
    struct block_header hdr;
    if (!headers_fill_header_from_index(mp, iter, &hdr))
        return false;

    const char *reason = headers_servable_reject_reason(mp, iter, &hdr);
    if (reason && headers_reject_reason_can_retry_disk(reason)) {
        struct block_header disk_hdr;
        if (headers_try_disk_header(mp, iter, &disk_hdr)) {
            const char *disk_reason =
                headers_servable_reject_reason(mp, iter, &disk_hdr);
            if (!disk_reason) {
                if (hdr_out)
                    *hdr_out = disk_hdr;
                return true;
            }
            reason = disk_reason;
        }
    }
    if (reason) {
        char hex[65] = {0};
        if (iter && iter->phashBlock)
            uint256_get_hex(iter->phashBlock, hex);
        LOG_WARN("headers",
                 "getheaders: refusing to serve header %s h=%d reason=%s",
                 hex[0] ? hex : "(unknown)", iter ? iter->nHeight : -1,
                 reason);
        if (iter && headers_reject_reason_permanent(reason))
            iter->nStatus |= BLOCK_FAILED_VALID;
        return false;
    }

    if (hdr_out)
        *hdr_out = hdr;
    return true;
}

static struct block_index *headers_next_servable_successor(
    struct msg_processor *mp,
    struct block_index *parent)
{
    if (!mp || !parent)
        return NULL;

    for (int guard = 0; guard < 64; guard++) {
        struct block_index *next =
            main_state_best_known_successor(mp->main_state, parent);
        if (!next)
            return NULL;
        if (headers_index_header_servable(mp, next, NULL))
            return next;
        if (!(next->nStatus & BLOCK_FAILED_MASK))
            return NULL;
    }
    LOG_WARN("headers",
             "getheaders: successor guard exhausted at parent h=%d",
             parent->nHeight);
    return NULL;
}

static struct block_index *headers_start_from_locator(
    struct main_state *ms,
    struct active_chain *chain,
    const struct block_locator *locator,
    const struct uint256 *hash_stop,
    const struct chain_params *params)
{
    struct block_index *pindex = NULL;

    if (!ms || !chain || !params)
        return NULL;

    if (locator && locator->num_hashes == 0 && hash_stop) {
        pindex = block_map_find(&ms->map_block_index, hash_stop);
    } else if (locator) {
        for (size_t i = 0; i < locator->num_hashes; i++) {
            struct block_index *found = block_map_find(
                &ms->map_block_index, &locator->vhave[i]);
            if (!found || !found->phashBlock)
                continue;

            if (active_chain_contains(chain, found) ||
                uint256_eq(found->phashBlock,
                           &params->consensus.hashGenesisBlock)) {
                pindex = found;
                break;
            }
        }
    }

    if (pindex)
        return main_state_best_known_successor(ms, pindex);

    pindex = block_map_find(&ms->map_block_index,
                            &params->consensus.hashGenesisBlock);
    if (pindex)
        return pindex;

    return active_chain_at(chain, 0);
}

void msg_headers_get_stats(struct msg_headers_stats *out)
{
    if (!out) return;
    out->batches_received = atomic_load(&g_headers_batches_received);
    out->total_accepted   = atomic_load(&g_headers_total_accepted);
    out->total_rejected   = atomic_load(&g_headers_total_rejected);
    out->newly_added      = atomic_load(&g_headers_newly_added);
    out->already_known    = atomic_load(&g_headers_already_known);
}

bool process_getheaders(struct msg_processor *mp, struct p2p_node *node,
                        struct byte_stream *s)
{
    if (node->state == PEER_SNAPSHOT_SERVING || node->swarm_manifest_sent) {
        printf("Peer %s: deferring getheaders while serving snapshot\n",
               node->addr_name);
        return true;
    }

    struct block_locator locator;
    block_locator_init(&locator);
    if (!block_locator_deserialize(&locator, s)) {
        block_locator_free(&locator);
        LOG_FAIL("net", "failed to deserialize getheaders locator from %s",
                 node->addr_name);
    }

    struct uint256 hash_stop;
    if (!stream_read(s, hash_stop.data, 32)) {
        block_locator_free(&locator);
        LOG_FAIL("net", "failed to read getheaders hash_stop from %s",
                 node->addr_name);
    }

    struct active_chain *chain = &mp->main_state->chain_active;
    struct block_index *iter = NULL;

    iter = headers_start_from_locator(mp->main_state, chain, &locator,
                                      &hash_stop, mp->params);
    block_locator_free(&locator);

    /* Count headers to send.
     *
     * Legacy ZClassic peers (MagicBean / pre-ZCL23) cap inbound headers
     * at MAX_HEADERS_RESULTS=160 and ban senders that exceed it
     * (Misbehaving +20 → disconnect). When we serve a legacy peer with a
     * larger batch, we get banned mid-handshake. The bug was visible on
     * the loopback peer at 127.0.0.1:8034: zclassicd's debug.log shows
     * "ProcessMessages(headers, 1088003 bytes) FAILED" followed by
     * "Misbehaving: 127.0.0.1:<port> (0 -> 20)" right after our version.
     * ZCL23 peers carry NODE_ZCL23 and accept up to 2000 per our own
     * limit at line :237. */
    const int max_headers = peer_supports_fast_sync(node->services) ? 2000 : 160;
    int count = 0;
    struct block_index *count_iter = iter;

    while (count_iter && count < max_headers) {
        if (!headers_index_header_servable(mp, count_iter, NULL)) {
            struct block_index *parent = count_iter->pprev;
            count_iter = parent ?
                headers_next_servable_successor(mp, parent) : NULL;
            continue;
        }
        count++;
        if (!uint256_is_null(&hash_stop) && count_iter->phashBlock &&
            uint256_eq(count_iter->phashBlock, &hash_stop))
            break;
        count_iter = headers_next_servable_successor(mp, count_iter);
    }

    struct byte_stream headers;
    stream_init(&headers, 4096);
    stream_write_compact_size(&headers, (uint64_t)count);

    for (int i = 0; i < count && iter; ) {
        struct block_header hdr;
        if (!headers_index_header_servable(mp, iter, &hdr)) {
            struct block_index *parent = iter->pprev;
            iter = parent ? headers_next_servable_successor(mp, parent)
                          : NULL;
            continue;
        }

        block_header_serialize(&hdr, &headers);
        stream_write_compact_size(&headers, 0);
        i++;
        iter = headers_next_servable_successor(mp, iter);
    }

    p2p_node_begin_message(node, "headers", mp->params->pchMessageStart);
    p2p_node_write_message_data(node, headers.data, headers.size);
    p2p_node_end_message(node);
    stream_free(&headers);
    return true;
}

bool process_headers(struct msg_processor *mp, struct p2p_node *node,
                     struct byte_stream *s)
{
    /* Defer header processing during any snapshot sync state — header parsing
     * and block index updates consume CPU and starve P2P reads.
     * During NEGOTIATING: headers trigger getblocks which compete. */
    if (msg_processor_snapshot_active(mp))
        return true;

    uint64_t count;
    if (!stream_read_compact_size(s, &count))
        LOG_FAIL("net", "failed to read headers count from %s",
                 node->addr_name);

    if (msg_count_exceeds("net", "headers", count, 2000, node->addr_name)) {
        event_emitf(EV_PEER_MISBEHAVE, (uint32_t)node->id,
                    "headers count %llu exceeds 2000 from %s",
                    (unsigned long long)count, node->addr_name);
        peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_FLOOD,
                            "too many headers");
        node->disconnect = true;
        return false;
    }

    struct uint256 last_hash;
    uint256_set_null(&last_hash);
    struct block_index *pindex_last = NULL;
    struct sync_header_processing_plan header_plan = {0};
    size_t accepted = 0;
    size_t newly_added = 0;  /* headers that were NOT already in block index */
    struct uint256 seq_hashes[512];
    int32_t seq_heights[512];
    size_t seq_count = 0;
    int pre_tip_height = active_chain_height(&mp->main_state->chain_active);
    struct block_index *sequence_prev =
        active_chain_tip(&mp->main_state->chain_active);

    for (uint64_t i = 0; i < count; i++) {
        struct block_header hdr;
        block_header_init(&hdr);
        if (!block_header_deserialize(&hdr, s)) {
            event_emitf(EV_HEADERS_REJECTED, (uint32_t)node->id,
                        "malformed header[%llu] from %s",
                        (unsigned long long)i, node->addr_name);
            peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_FLOOD,
                                "malformed header");
            LOG_FAIL("net", "malformed header[%llu] from %s",
                     (unsigned long long)i, node->addr_name);
        }

        uint64_t dummy;
        if (!stream_read_compact_size(s, &dummy)) {
            peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_FLOOD,
                                "truncated header tx count");
            LOG_FAIL("net", "truncated header tx count at header[%llu] from %s",
                     (unsigned long long)i, node->addr_name);
        }

        /* Check if header already in index BEFORE accept_block_header */
        struct uint256 hdr_hash;
        block_header_get_hash(&hdr, &hdr_hash);
        bool was_known = (block_map_find(&mp->main_state->map_block_index,
                                          &hdr_hash) != NULL);

        struct validation_state state;
        validation_state_init(&state);
        struct block_index *pindex = NULL;
        if (accept_block_header(&hdr, &state, mp->main_state,
                                mp->params, &pindex)) {
            accepted++;
            if (!was_known)
                newly_added++;
            if (pindex && sequence_prev && sequence_prev->phashBlock &&
                uint256_eq(&hdr.hashPrevBlock, sequence_prev->phashBlock)) {
                if (pindex->pprev != sequence_prev ||
                    pindex->nHeight != sequence_prev->nHeight + 1) {
                    pindex->pprev = sequence_prev;
                    pindex->nHeight = sequence_prev->nHeight + 1;
                    block_index_build_skip(pindex);
                    struct arith_uint256 proof = GetBlockProof(pindex);
                    arith_uint256_add(&pindex->nChainWork,
                                      &sequence_prev->nChainWork, &proof);
                }
                sequence_prev = pindex;
            }
            pindex_last = pindex;
            if (pindex && pindex->phashBlock)
                last_hash = *pindex->phashBlock;
            if (pindex && pindex->phashBlock &&
                pindex->nHeight > pre_tip_height &&
                pindex->nHeight <= pre_tip_height + 512 &&
                !(pindex->nStatus & BLOCK_HAVE_DATA) &&
                !(pindex->nStatus & BLOCK_FAILED_MASK) &&
                seq_count < 512) {
                seq_hashes[seq_count] = *pindex->phashBlock;
                seq_heights[seq_count] = pindex->nHeight;
                seq_count++;
            }
        } else {
            if (i < 3) {
                char hex[65], prevhex[65];
                struct uint256 hh;
                block_header_get_hash(&hdr, &hh);
                uint256_get_hex(&hh, hex);
                uint256_get_hex(&hdr.hashPrevBlock, prevhex);
                printf("HEADER REJECT[%llu]: hash=%s prev=%s reason=%s\n",
                       (unsigned long long)i, hex, prevhex,
                       state.reject_reason[0] ? state.reject_reason
                                              : "unknown");
                event_emitf(EV_HEADERS_REJECTED, (uint32_t)node->id,
                            "header[%llu] %s reason=%s",
                            (unsigned long long)i, hex,
                            state.reject_reason[0] ? state.reject_reason
                                                   : "unknown");
            }
        }

        /* Detective A2 — score the peer for an objectively-forged header page.
         * accept_block_header admits header-first and defers the PoW/Equihash
         * verdict to the validate_headers stage, so it returns true even for a
         * bad-solution / high-hash header; check_block_header still computes the
         * DoS grade into `state`. A dos>0 here is an objective PoW/Equihash
         * failure (invalid-solution=100, high-hash=50) — penalize INVALID_HEADER
         * so a peer forging the header range (e.g. while a stale-header repair
         * is fetching the canonical bytes) is scored and eventually
         * disconnected, and the forged page never fools the repair. Gated on
         * dos>0 so orphans / failed-parent / obsolete-version (dos 0) — all
         * normal during sync — are never penalized. Trusted/localhost peers are
         * exempted inside peer_misbehaving. Mirrors msg_blocks.c grading. */
        int hdr_dos = 0;
        if (validation_state_get_dos(&state, &hdr_dos) && hdr_dos > 0) {
            peer_scoring_record(mp->net_mgr, node,
                                PEER_OFFENCE_INVALID_HEADER,
                                state.reject_reason[0] ? state.reject_reason
                                                       : "invalid header");
        }
    }

    /* Update diagnostic counters */
    atomic_fetch_add(&g_headers_batches_received, 1);
    atomic_fetch_add(&g_headers_total_accepted, accepted);
    atomic_fetch_add(&g_headers_total_rejected, count - accepted);
    atomic_fetch_add(&g_headers_newly_added, newly_added);
    atomic_fetch_add(&g_headers_already_known, accepted - newly_added);

    /* Per-peer usefulness credit (P1). MUST pass newly_added, NEVER
     * accepted: `accepted` also counts headers we already had in the
     * index (was_known above), so crediting it would let a withholding
     * peer replay known headers forever to refresh
     * last_useful_headers_time (defeating the stale-peer disconnect)
     * and inflate total_headers_delivered (deflecting worst-peer
     * eviction onto honest peers). Only new-to-index headers count. */
    syncsvc_note_headers_received(node, newly_added);

    {
        struct block_index *tip = active_chain_tip(&mp->main_state->chain_active);
        int our_height = tip ? tip->nHeight : 0;
        struct block_index *bi = block_map_find(
            &mp->main_state->map_block_index, &last_hash);
        size_t max_collect = 512;
        struct uint256 *hashes = zcl_malloc(max_collect * sizeof(struct uint256), "blk_req_hashes");
        int32_t *heights = zcl_malloc(max_collect * sizeof(int32_t), "blk_req_heights");

        if (!hashes || !heights) {
            LOG_WARN("sync", "malloc failed for block request arrays "
                     "(%zu entries)", max_collect);
            free(hashes); free(heights);
            hashes = NULL; heights = NULL;
        }

        syncsvc_plan_header_processing(&header_plan, accepted, count,
                                       pindex_last, sync_get_state(),
                                       bi, tip, our_height,
                                       hashes, heights, max_collect);
        if (seq_count > 0 && hashes && heights) {
            memcpy(hashes, seq_hashes, seq_count * sizeof(struct uint256));
            memcpy(heights, seq_heights, seq_count * sizeof(int32_t));
            header_plan.should_queue_needed_blocks = true;
            header_plan.queue_count = seq_count;
            header_plan.should_activate_chain = false;
            header_plan.download.needed_blocks.count = seq_count;
            header_plan.download.needed_blocks.chains_from_tip = true;
        }
        if (our_height > 1000000 && header_plan.should_scan_block_files) {
            printf("headers: skip inline block-file scan at live height h=%d\n",
                   our_height);
            header_plan.should_scan_block_files = false;
        }

        if (header_plan.batch.should_warn_all_rejected) {
            /* All headers rejected — this stalls sync. Log prominently. */
            event_emitf(EV_HEADERS_REJECTED, (uint32_t)node->id,
                        "all %llu headers rejected", (unsigned long long)count);
            printf("WARNING: Peer %s: all %llu headers rejected — sync stalled!\n",
                   node->addr_name, (unsigned long long)count);
        }

        if (header_plan.batch.should_emit_received) {
            event_emitf(EV_HEADERS_RECEIVED, (uint32_t)node->id,
                        "accepted=%zu total=%llu tip=%d",
                        accepted, (unsigned long long)count,
                        pindex_last ? pindex_last->nHeight : -1);

            if (syncsvc_should_log_accepted_headers(node, pindex_last)) {
                int chain_h = active_chain_height(&mp->main_state->chain_active);
                printf("Peer %s: accepted %zu/%llu headers "
                       "(header tip=%d, chain tip=%d, peer=%d)\n",
                       node->addr_name, accepted, (unsigned long long)count,
                       pindex_last ? pindex_last->nHeight : -1,
                       chain_h, node->starting_height);
                /* Stall detection: if we accepted headers but the tip
                 * didn't advance past chain height, something is wrong
                 * with the block index heights. Log loudly. */
                if (pindex_last && accepted > 0 &&
                    pindex_last->nHeight < chain_h &&
                    node->starting_height > chain_h + 100) {
                    LOG_WARN("sync",
                        "STALL DETECTED: accepted %zu headers but "
                        "header tip=%d < chain tip=%d (peer at %d). "
                        "Block index heights may be corrupted.",
                        accepted, pindex_last->nHeight, chain_h,
                        node->starting_height);
                }
            }
        }

        /* The CSR owns both ranking and publication. Always submit the batch
         * candidate; a valid non-winning header is an idempotent no-op. */
        if (pindex_last) {
            if (!msg_processor_commit_header_tip(mp, pindex_last)) {
                LOG_WARN("sync",
                         "best-header promotion rejected h=%d",
                         pindex_last->nHeight);
            }
        }

        if (pindex_last && pindex_last->phashBlock &&
            peer_supports_fast_sync(node->services)) {
            char peer_hash_hex[65];
            uint256_get_hex(pindex_last->phashBlock, peer_hash_hex);
            msg_processor_record_peer_header_vote(mp, (uint32_t)node->id,
                                                  pindex_last->nHeight,
                                                  peer_hash_hex);
        }

        /* Clear snapshot anchor once headers extend past the configured
         * immutable/finality window. The anchor blocks reducer activation.
         * Once the header chain has enough depth for the snapshot policy that
         * accepted it, clear it so blocks can be connected. Set chain tip to
         * the anchor so the reducer starts from the right UTXO state. */
        {
            struct block_index *anc = msg_processor_snapshot_anchor(mp);
            if (syncsvc_should_release_snapshot_anchor(anc, pindex_last)) {
                /* Verify the header chain reaches the anchor via pprev */
                struct block_index *walk = pindex_last;
                while (walk && walk->nHeight > anc->nHeight)
                    walk = walk->pprev;
                if (walk == anc) {
                    printf("Anchor cleared: headers extend %d blocks past "
                           "anchor h=%d — enabling block connection\n",
                           pindex_last->nHeight - anc->nHeight,
                           anc->nHeight);
                    /* Re-anchor active_chain at the snapshot anchor through
                     * the boot-owned chain-state boundary so
                     * block_map/coins_tip/header agree. In production this
                     * is typically a no-op move (active_chain is already at
                     * `anc`), but routing it through the single authority
                     * gives the transition a structured event and guards
                     * against drift since snapshot activation ran. */
                    bool anchor_recommitted = false;
                    if (anc->phashBlock) {
                        int from_height = mp->main_state ?
                            active_chain_height(&mp->main_state->chain_active) : -1;
                        anchor_recommitted =
                            msg_processor_recommit_snapshot_anchor(
                                mp, anc, from_height);
                        if (!anchor_recommitted) {
                            LOG_WARN("sync",
                                "anchor re-commit rejected h=%d",
                                anc->nHeight);
                        }
                    } else {
                        LOG_WARN("sync",
                            "refusing to clear snapshot anchor "
                            "without block hash h=%d", anc->nHeight);
                    }
                    if (anchor_recommitted) {
                        msg_processor_set_snapshot_anchor(mp, NULL);
                        msg_processor_clear_activation_anchor(
                            mp, "headers_past_anchor");
                    }
                }
            }
        }

        /* One-shot block file scan: if block files exist on disk (from
         * file_service) but weren't scanned at boot (empty index at boot),
         * scan them now that we have headers. This marks downloaded blocks
         * as BLOCK_HAVE_DATA so we don't re-download them from P2P. */
        if (header_plan.should_scan_block_files) {
            char bfp[576];
            snprintf(bfp, sizeof(bfp), "%s/blocks/blk00000.dat", mp->datadir);
            struct stat bfst;
            if (stat(bfp, &bfst) == 0 && bfst.st_size > 0) {
                printf("P2P trigger: scanning block files for HAVE_DATA...\n");
                int scan_m = msg_processor_scan_block_files(mp);

                struct sync_chain_activation activation = {0};
                syncsvc_build_block_file_scan_activation(&activation, scan_m);
                if (activation.should_activate && !g_shutdown_requested) {
                    printf("P2P block file scan: %d blocks marked\n", scan_m);
                    msg_processor_request_activation(
                        mp, MSG_ACTIVATE_BLOCK_FILE_SCAN);
                }

                /* Structural repair (block_map heights, active-tip
                 * restore) belongs in block_index_integrity, not this
                 * P2P handler. */
                msg_processor_repair_post_activation_anchor(mp);
            }
        }
        if (header_plan.should_queue_needed_blocks) {
            if (header_plan.should_set_sync_state)
                sync_set_state(header_plan.next_sync_state,
                               "headers ahead, requesting blocks");
            if (!header_plan.download.needed_blocks.chains_from_tip)
                printf("headers: skip block queue — chain doesn't reach "
                       "tip h=%d\n", our_height);

            {
                struct download_manager *dm = get_download_mgr();
                size_t queued = dl_queue_blocks(dm, hashes, heights,
                                                header_plan.queue_count);
                if (queued > 0)
                    event_emitf(EV_BLOCK_REQUESTED, (uint32_t)node->id,
                                "queued=%zu total_needed=%zu",
                                queued, header_plan.queue_count);
            }
        }

        if (hashes && heights && pindex_last &&
            pindex_last->nHeight > our_height &&
            header_plan.queue_count == 0) {
            bool has_data_successor = false;
            size_t fallback_count = collect_active_tip_successors(
                mp->main_state, hashes, heights, max_collect,
                &has_data_successor);
            if (fallback_count > 0) {
                struct download_manager *dm = get_download_mgr();
                size_t queued = dl_queue_blocks(dm, hashes, heights,
                                                fallback_count);
                if (queued > 0) {
                    sync_set_state(SYNC_BLOCKS_DOWNLOAD,
                                   "tip successor fallback");
                    event_emitf(EV_BLOCK_REQUESTED, (uint32_t)node->id,
                                "fallback_queued=%zu total_needed=%zu",
                                queued, fallback_count);
                    LOG_INFO("headers",
                        "fallback queued %zu active-tip "
                        "successor blocks after empty header plan "
                        "(tip=%d header=%d)",
                        queued, our_height, pindex_last->nHeight);
                }
            } else if (has_data_successor) {
                msg_processor_request_activation(
                    mp, MSG_ACTIVATE_HEADERS_ALL_DATA);
            }
        }

        /* Chain activation: if all blocks already have data (e.g. after
         * LDB import with symlinked blk files), needed_blocks.count is 0
         * but should_activate_chain is true.  This MUST run outside the
         * should_queue_needed_blocks guard — that gate requires count>0,
         * which is the opposite of the activation condition. */
        if (header_plan.should_activate_chain) {
            struct sync_chain_activation activation = {0};
            syncsvc_build_header_processing_activation(&activation,
                                                      &header_plan);
            if (activation.should_activate && !g_shutdown_requested) {
                msg_processor_request_activation(
                    mp, MSG_ACTIVATE_HEADERS_ALL_DATA);
            }
        }
        free(hashes);
        free(heights);
    }

    /* Request more headers if we accepted any.
     *
     * If ALL headers in the batch were already known (newly_added == 0),
     * skip ahead to pindex_best_header instead of crawling 160 at a time
     * through millions of known headers.  This happens after snapshot sync
     * when the block index has entries above the chain tip.
     *
     * If some headers were new, use pindex_last — the peer will continue
     * from right after it. */
    if (header_plan.batch.should_request_more_headers) {
        /* Track header advancement rate.  If a full batch of headers
         * didn't advance the tip by at least 100, something may be
         * wrong (e.g., heights still scrambled, bouncing locators). */
        if (pindex_last && accepted >= 100) {
            int prev_tip = atomic_load(&g_last_header_tip_height);
            int cur_tip = pindex_last->nHeight;
            if (prev_tip > 0 && cur_tip - prev_tip < 100 &&
                cur_tip > 0 && prev_tip > 0) {
                LOG_WARN("headers",
                    "SLOW ADVANCE: peer %s sent %zu headers "
                    "but tip only moved from %d to %d",
                    node->addr_name, accepted, prev_tip, cur_tip);
            }
            atomic_store(&g_last_header_tip_height, cur_tip);
        }

        /* Band fill: a below-tip batch that extends the trust-rooted
         * frontier toward an installed-above-frontier island is progress
         * — it must suppress BOTH the restart-from-tip and the
         * best-header skip below, or the band hole never closes. The
         * low-batch gate keeps the ancestry walk off the IBD hot path. */
        bool band_fill = (pindex_last &&
            pindex_last->nHeight <
                active_chain_height(&mp->main_state->chain_active))
            ? syncsvc_header_band_continue(&mp->main_state->chain_active,
                                           pindex_last)
            : false;

        if (syncsvc_should_restart_headers_from_tip(
                accepted, pindex_last, active_chain_height(
                    &mp->main_state->chain_active), node->starting_height,
                band_fill)) {
            LOG_WARN("headers",
                    "low batch from %s ended at h=%d below "
                    "chain tip h=%d; restarting getheaders from tip",
                    node->addr_name,
                    pindex_last ? pindex_last->nHeight : -1,
                    active_chain_height(&mp->main_state->chain_active));
            {
                struct block_index *restart_tip = active_chain_tip(
                    &mp->main_state->chain_active);
                if (restart_tip && restart_tip->phashBlock)
                    push_getheaders_from(mp, node, restart_tip);
                else
                    push_getheaders(mp, node);
            }
        } else if (!band_fill && newly_added == 0 && pindex_last &&
                   msg_processor_block_index_heights_repaired(mp) &&
                   mp->main_state->pindex_best_header &&
                   mp->main_state->pindex_best_header->phashBlock &&
                   mp->main_state->pindex_best_header->nHeight >
                       pindex_last->nHeight &&
                   node->starting_height >
                       mp->main_state->pindex_best_header->nHeight) {
            /* The whole batch was already known and our best header sits
             * far above it (boot restored a deep header chain, or a
             * periodic re-anchor pulled the conversation back to the
             * active tip). Continuing from pindex_last would crawl the
             * known span 160 headers per round trip — with millions of
             * known headers that is a multi-day stall that also burns a
             * core re-accepting known headers. Skip the conversation
             * straight to best_header; the exponential locator still
             * lets the peer pick an earlier fork point if our best
             * header is stale. Gated on repaired heights — with
             * scrambled heights this skip would loop forever.
             * Also gated on the peer's advertised starting_height being
             * ABOVE our best header: an honest peer whose tip is below
             * best_header answers a best_header-anchored getheaders with
             * the same all-known span every time (their fork point never
             * moves), so the skip would ping-pong the identical request
             * with that peer forever. Such peers take the pindex_last
             * continuation below, which terminates at their tip. */
            push_getheaders_from(mp, node, mp->main_state->pindex_best_header);
        } else {
            /* Advance from pindex_last — the actual last header the peer
             * sent.  Using pindex_best_header caused infinite loops after
             * snapshot/LDB import when heights were scrambled. */
            push_getheaders_from(mp, node, pindex_last);
        }
    }

    /* Band closure probe — deliberately OUTSIDE should_request_more_headers
     * (the final band batch can be shorter than 160). No-op without the
     * band blocker. */
    if (accepted > 0)
        syncsvc_header_band_after_batch(mp->main_state, pindex_last);

    return true;
}

void push_getheaders_from(struct msg_processor *mp,
                          struct p2p_node *node,
                          struct block_index *from)
{
    if (from && !from->phashBlock) return;

    /* Don't request headers during any snapshot sync state. */
    if (msg_processor_snapshot_active(mp))
        return;

    /* Build locator for getheaders request.
     * After bulk height repair, heights are trustworthy — use a proper
     * Bitcoin-style exponential locator for better branch identification.
     * Before repair, fall back to the safe 2-hash locator. */
    struct block_locator loc;
    block_locator_init(&loc);
    if (from && from->phashBlock &&
        msg_processor_block_index_heights_repaired(mp)) {
        /* Exponential locator: tip, tip-1, tip-2, tip-4, tip-8, ... genesis.
         * Walk pprev chain with exponentially increasing steps. */
        int max_hashes = 32;
        struct uint256 *tmp = zcl_malloc((size_t)max_hashes * sizeof(struct uint256),
                                         "exp_locator");
        if (!tmp) return;
        int nh = 0;
        struct block_index *walk = from;
        int step = 1;
        while (walk && nh < max_hashes - 1) {
            if (walk->phashBlock)
                tmp[nh++] = *walk->phashBlock;
            /* Walk back 'step' blocks via pprev */
            struct block_index *prev_walk = walk;
            for (int s = 0; s < step && walk->pprev; s++)
                walk = walk->pprev;
            /* Stop when the walk made no progress (pprev exhausted — an
             * island root, not just the first hop): the old `walk == from`
             * test only caught the first iteration, so a detached-island
             * anchor degenerated the locator to [island hashes ×31,
             * genesis]. */
            if (walk == prev_walk) break;
            if (nh >= 10)
                step *= 2;  /* exponential after first 10 entries */
        }
        /* Always end with genesis */
        if (nh > 0 && nh < max_hashes)
            tmp[nh++] = mp->params->consensus.hashGenesisBlock;
        loc.vhave = tmp;
        loc.num_hashes = (size_t)nh;
    } else if (from && from->phashBlock) {
        struct zcl_result _r = syncsvc_build_getheaders_locator(&loc,
                                              &mp->main_state->chain_active,
                                              from,
                                              &mp->params->consensus.hashGenesisBlock);
        if (!_r.ok) {
            fprintf(stderr, "[headers] %s:%d push_getheaders_from: build_locator failed: %s\n",
                    _r.source_file, _r.source_line, _r.message);
            return;
        }
    } else {
        struct zcl_result _r = syncsvc_build_getheaders_locator(&loc,
                                              &mp->main_state->chain_active,
                                              NULL,
                                              &mp->params->consensus.hashGenesisBlock);
        if (!_r.ok) {
            fprintf(stderr, "[headers] %s:%d push_getheaders_from: build_locator failed: %s\n",
                    _r.source_file, _r.source_line, _r.message);
            return;
        }
    }

    struct byte_stream s;
    stream_init(&s, 512);
    if (!getheaders_serialize(&s, &loc, NULL)) {
        stream_free(&s);
        block_locator_free(&loc);
        return;
    }

    /* Debug: log locator hashes to diagnose sync stall */
    if (loc.num_hashes > 0 && loc.num_hashes <= 20) {
        for (size_t li = 0; li < loc.num_hashes && li < 3; li++) {
            char lhex[65];
            uint256_get_hex(&loc.vhave[li], lhex);
            LOG_INFO("headers", "getheaders locator[%zu]: %s", li, lhex);
        }
    }

    p2p_node_begin_message(node, "getheaders", mp->params->pchMessageStart);
    p2p_node_write_message_data(node, s.data, s.size);
    p2p_node_end_message(node);
    stream_free(&s);
    block_locator_free(&loc);
}

void push_getheaders(struct msg_processor *mp, struct p2p_node *node)
{
    if (msg_processor_snapshot_active(mp))
        return;

    /* Use the active-chain locator, including recent ancestors. A locator
     * containing only tip+genesis makes a one-block local fork invisible to
     * peers: they do not know our tip and fall back to genesis, sending old
     * headers instead of the sibling that reorgs us back to the best chain. */
    {
        struct block_locator loc;
        block_locator_init(&loc);
        if (syncsvc_build_getheaders_locator(
                &loc, &mp->main_state->chain_active, NULL,
                &mp->params->consensus.hashGenesisBlock).ok) {
            struct byte_stream s;
            stream_init(&s, 512);
            if (getheaders_serialize(&s, &loc, NULL)) {
                p2p_node_begin_message(node, "getheaders",
                                       mp->params->pchMessageStart);
                p2p_node_write_message_data(node, s.data, s.size);
                p2p_node_end_message(node);
            }
            stream_free(&s);
            block_locator_free(&loc);
            return;
        }
        /* Fall through to snapsync anchor locator path. Builder
         * already logged via ZCL_ERR source/line; no need to dup. */
    }

    /* After snapshot sync, use the snapshot anchor as the locator start. */
    struct block_index *anchor = msg_processor_snapshot_anchor(mp);
    if (anchor && anchor->phashBlock)
        push_getheaders_from(mp, node, anchor);
    else
        push_getheaders_from(mp, node, NULL);
}

void exec_getheaders_action(struct msg_processor *mp,
                            struct p2p_node *node,
                            const struct sync_getheaders_action *action)
{
    struct block_index *tip;

    if (!mp || !node || !action || !action->should_send)
        return;

    tip = active_chain_tip(&mp->main_state->chain_active);

    /* While a header band hole is recorded, every periodic kick drives
     * the band: anchor at the contiguous frontier so the peer forks
     * there and serves the band. Tip-side progress continues via batch
     * continuations and unsolicited header/inv pushes. O(1) when no
     * band fact exists. */
    {
        struct block_index *band_anchor =
            syncsvc_header_band_backfill_anchor(&mp->main_state->chain_active);
        if (band_anchor) {
            push_getheaders_from(mp, node, band_anchor);
            return;
        }
    }

    switch (action->anchor) {
    case SYNC_HEADER_REQUEST_TIP_PARENT:
        if (mp->main_state->pindex_best_header &&
            mp->main_state->pindex_best_header->phashBlock &&
            tip &&
            mp->main_state->pindex_best_header->nHeight > tip->nHeight) {
            push_getheaders_from(mp, node, mp->main_state->pindex_best_header);
        } else if (tip && tip->pprev) {
            push_getheaders_from(mp, node, tip->pprev);
        } else {
            push_getheaders(mp, node);
        }
        break;
    case SYNC_HEADER_REQUEST_TIP:
    case SYNC_HEADER_REQUEST_EXPLICIT:
    default:
        /* When the validated header chain already extends above the
         * active tip, anchor the request at best_header — re-anchoring
         * at the tip makes the peer re-send known headers (160 per
         * round trip through the whole known span). The periodic IBD
         * re-kick fires every 10s, so a tip anchor here permanently
         * resets the conversation below the known frontier. The
         * locator includes lower heights, so a stale best_header still
         * converges on the true fork point. */
        if (msg_processor_block_index_heights_repaired(mp) &&
            mp->main_state->pindex_best_header &&
            mp->main_state->pindex_best_header->phashBlock &&
            tip &&
            mp->main_state->pindex_best_header->nHeight > tip->nHeight) {
            push_getheaders_from(mp, node, mp->main_state->pindex_best_header);
        } else {
            push_getheaders(mp, node);
        }
        break;
    }
}
