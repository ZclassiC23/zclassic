/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* msg_blocks.c — Block message processing.
 * Split from msgprocessor.c for maintainability. */

#include "platform/time_compat.h"
#include "net/msg_internal.h"
#include "net/peer_scoring.h"
#include "net/compact_blocks.h"
#include "storage/disk_block_io.h"
#include "validation/process_block.h"
#include "consensus/validation.h"
#include "net/download.h"
#include "net/https_server.h"
#include "event/event.h"
#include "sync/sync_state.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/sync.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

/* Rebuild manifest when chain grows this many blocks beyond the cached one. */
#define MANIFEST_REFRESH_BLOCKS 1000

static struct block_index *best_known_successor(struct main_state *ms,
                                                struct block_index *parent)
{
    struct block_index *best = NULL;
    size_t iter = 0;
    struct block_index *candidate = NULL;

    if (!ms || !parent || !parent->phashBlock)
        return NULL;

    while (block_map_next(&ms->map_block_index, &iter, NULL, &candidate)) {
        if (!candidate || !candidate->phashBlock || !candidate->pprev)
            continue;
        if (candidate->pprev != parent)
            continue;
        if (candidate->nHeight != parent->nHeight + 1)
            continue;
        if (candidate->nStatus & BLOCK_FAILED_MASK)
            continue;
        if (!best ||
            arith_uint256_compare(&candidate->nChainWork,
                                  &best->nChainWork) > 0 ||
            (arith_uint256_compare(&candidate->nChainWork,
                                   &best->nChainWork) == 0 &&
             (candidate->nStatus & BLOCK_HAVE_DATA) &&
             !(best->nStatus & BLOCK_HAVE_DATA))) {
            best = candidate;
        }
    }

    return best;
}

bool process_getblocks(struct msg_processor *mp, struct p2p_node *node,
                       struct byte_stream *s)
{
    struct block_locator locator;
    block_locator_init(&locator);
    if (!block_locator_deserialize(&locator, s)) {
        block_locator_free(&locator);
        LOG_FAIL("net", "failed to deserialize getblocks locator from %s",
                 node->addr_name);
    }

    struct uint256 hash_stop;
    if (!stream_read(s, hash_stop.data, 32)) {
        block_locator_free(&locator);
        LOG_FAIL("net", "failed to read getblocks hash_stop from %s",
                 node->addr_name);
    }

    struct block_index *pindex = NULL;
    struct active_chain *chain = &mp->main_state->chain_active;

    for (size_t i = 0; i < locator.num_hashes; i++) {
        struct block_index *found = block_map_find(
            &mp->main_state->map_block_index, &locator.vhave[i]);
        if (found && (active_chain_contains(chain, found) ||
                      (found->phashBlock &&
                       uint256_eq(found->phashBlock,
                                  &mp->params->consensus.hashGenesisBlock)))) {
            pindex = found;
            break;
        }
    }
    block_locator_free(&locator);

    if (!pindex)
        pindex = block_map_find(&mp->main_state->map_block_index,
                                &mp->params->consensus.hashGenesisBlock);
    if (!pindex)
        pindex = active_chain_at(chain, 0);

    int limit = 500;
    struct block_index *tip = active_chain_tip(chain);

    if (pindex)
        pindex = best_known_successor(mp->main_state, pindex);

    for (; pindex && limit > 0;
         pindex = best_known_successor(mp->main_state, pindex)) {
        if (!pindex || !pindex->phashBlock)
            break;

        struct inv_item inv;
        inv_item_init_typed(&inv, MSG_BLOCK, pindex->phashBlock);
        p2p_node_push_inventory(node, &inv);
        limit--;

        if (!uint256_is_null(&hash_stop) &&
            uint256_eq(pindex->phashBlock, &hash_stop))
            break;

        if (pindex == tip)
            break;
    }

    return true;
}

bool process_getdata(struct msg_processor *mp, struct p2p_node *node,
                     struct byte_stream *s)
{
    if (node->swarm_manifest_sent) {
        printf("Peer %s: deferring getdata while serving snapshot\n",
               node->addr_name);
        return true;
    }

    uint64_t count;
    if (!stream_read_compact_size(s, &count))
        LOG_FAIL("net", "failed to read getdata count from %s",
                 node->addr_name);

    if (count > MAX_INV_SZ) {
        node->disconnect = true;
        LOG_FAIL("net", "getdata count %llu exceeds MAX_INV_SZ from %s",
                 (unsigned long long)count, node->addr_name);
    }

    struct inv_item not_found[64];
    size_t not_found_count = 0;

    for (uint64_t i = 0; i < count; i++) {
        /* Bound the send queue: a single getdata can request up to
         * MAX_INV_SZ (50000) blocks, and a slow-reader peer may never
         * drain its socket, so serving the whole batch could buffer
         * tens of GB of send_segments -> OOM. Once this peer (or the
         * process as a whole) is over the send budget, stop serving and
         * return — the peer is within protocol and will re-request the
         * remaining items later (Core's fPauseSend behaviour). Do NOT
         * disconnect. Whitelisted/trusted peers are exempt (checked
         * inside net_send_over_budget). Any not-found items already
         * accumulated are still flushed below. */
        if (net_send_over_budget(node))
            break;

        struct inv_item inv;
        if (!inv_item_deserialize(&inv, s))
            LOG_FAIL("net", "failed to deserialize getdata inv[%llu] from %s",
                     (unsigned long long)i, node->addr_name);

        bool sent = false;
        if (inv.type == MSG_BLOCK) {
            struct block_index *bi = block_map_find(
                &mp->main_state->map_block_index, &inv.hash);
            if (bi && (bi->nStatus & BLOCK_HAVE_DATA)) {
                struct block blk;
                block_init(&blk);

                if (read_block_from_disk_index(&blk, bi, mp->datadir)) {
                    /* Verify block hash before serving — never send
                     * corrupted data that would get us banned */
                    struct uint256 disk_hash;
                    block_get_hash(&blk, &disk_hash);
                    if (uint256_cmp(&disk_hash, &inv.hash) != 0) {
                        char exp[65], got[65];
                        uint256_get_hex(&inv.hash, exp);
                        uint256_get_hex(&disk_hash, got);
                        LOG_WARN("net", "SAFETY: refusing to serve block h=%d "
                                 "— hash mismatch (requested=%s disk=%s)",
                                 bi->nHeight, exp, got);
                        block_free(&blk);
                        goto skip_block_serve;
                    }

                    struct byte_stream blk_data;
                    stream_init(&blk_data, 1024 * 1024);
                    if (block_serialize(&blk, &blk_data)) {
                        p2p_node_begin_message(node, "block",
                                               mp->params->pchMessageStart);
                        p2p_node_write_message_data(node, blk_data.data,
                                                    blk_data.size);
                        p2p_node_end_message(node);
                        sent = true;
                    }
                    stream_free(&blk_data);
                }
                block_free(&blk);
            }
            skip_block_serve:
            (void)0;
        } else if (inv.type == MSG_TX) {
            struct transaction tx;
            transaction_init(&tx);
            if (tx_mempool_lookup(mp->mempool, &inv.hash, &tx)) {
                struct byte_stream tx_data;
                stream_init(&tx_data, 512);
                transaction_serialize(&tx, &tx_data);

                p2p_node_begin_message(node, "tx",
                                       mp->params->pchMessageStart);
                p2p_node_write_message_data(node, tx_data.data, tx_data.size);
                p2p_node_end_message(node);
                stream_free(&tx_data);
                sent = true;
            }
            transaction_free(&tx);
        }

        if (!sent && not_found_count < 64)
            not_found[not_found_count++] = inv;
    }

    /* Send notfound for items we couldn't serve */
    if (not_found_count > 0) {
        struct byte_stream nf;
        stream_init(&nf, not_found_count * 36 + 8);
        stream_write_compact_size(&nf, not_found_count);
        for (size_t i = 0; i < not_found_count; i++)
            inv_item_serialize(&not_found[i], &nf);

        p2p_node_begin_message(node, "notfound",
                               mp->params->pchMessageStart);
        p2p_node_write_message_data(node, nf.data, nf.size);
        p2p_node_end_message(node);
        stream_free(&nf);
    }

    return true;
}

bool msg_blocks_should_mark_seen(const struct active_chain *chain,
                                  const struct block_index *bi)
{
    if (!chain || !bi) return false;
    return active_chain_contains(chain, bi);
}

bool process_block_msg(struct msg_processor *mp, struct p2p_node *node,
                       struct byte_stream *s)
{
    /* Pre-check: reject oversized block messages before deserialization.
     * Prevents allocation DoS from crafted messages. */
    if (s->size > 2000000) {
        event_emitf(EV_PEER_MISBEHAVE, (uint32_t)node->id,
                    "oversized block msg %zu bytes", s->size);
        peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_BLOCK,
                            "oversized block msg");
        LOG_FAIL("net", "oversized block msg %zu bytes from %s",
                 s->size, node->addr_name);
    }

    struct block blk;
    block_init(&blk);
    if (!block_deserialize(&blk, s)) {
        event_emitf(EV_MSG_DESERIALIZATION_FAIL, (uint32_t)node->id, "block");
        peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_FLOOD,
                            "malformed block");
        block_free(&blk);
        LOG_FAIL("net", "failed to deserialize block from %s",
                 node->addr_name);
    }

    struct uint256 hash;
    block_get_hash(&blk, &hash);

    /* Mark received in download manager (removes from in-flight) */
    struct download_manager *dm = get_download_mgr();
    dl_mark_received(dm, &hash);

    /* Track block bytes for MB/s throughput reporting */
    dl_add_bytes_received(dm, s->size);

    /* Defer block processing while snapshot sync is active (any state).
     * During NEGOTIATING: blocks fail at height 0, accumulate dos points.
     * During RECEIVING: starves P2P socket reads.
     * During VERIFYING: SHA3 computation needs uncontested SQLite. */
    if (msg_processor_snapshot_active(mp)) {
        block_free(&blk);
        return true;
    }

    if (block_already_seen(&hash)) {
        block_free(&blk);
        return true;
    }
    /* do NOT mark seen here. We previously called block_mark_seen() before the
     * synchronous intake path, so a block that was received + indexed but
     * failed to activate (e.g. ACTIVATION_SKIP_ALREADY_RUNNING from controller
     * mutex contention under 6+ peer arrival) got permanently dedup'd and
     * never retried. mark_seen now happens post-processing, only when the block
     * actually made it onto the active chain. */

    struct validation_state state;
    validation_state_init(&state);
    /* Block intake is owned by the app reducer/stage path injected at boot.
     * The verdict in `state` preserves the mark-seen + DoS/getheaders
     * contract below while keeping lib/net protocol handling app-agnostic. */
    if (!mp || !mp->block_submit) {
        block_free(&blk);
        LOG_FAIL("net", "block submit callback not configured");
    }
    (void)mp->block_submit(&blk, &state, mp->block_submit_ctx);

    if (!validation_state_is_valid(&state)) {
        char hex[65];
        uint256_get_hex(&hash, hex);
        event_emitf(EV_BLOCK_REJECTED, (uint32_t)node->id,
                    "hash=%s reason=%s", hex,
                    state.reject_reason[0] ? state.reject_reason : "unknown");

        /* rejected blocks: mark seen so the dedup ring
         * short-circuits subsequent deliveries of the same bad block
         * from other peers. Only the "received but skipped connect"
         * case (SKIP_ALREADY_RUNNING, etc.) must stay UN-marked so
         * it can retry; that path is validation_state_is_valid ==
         * true but with no tip advance, handled below. */
        block_mark_seen(&hash);

        /* When a block fails validation during IBD (likely a fork block),
         * re-request headers from this peer starting at our current tip.
         * This forces the peer to send us the correct chain of headers,
         * which will include the valid block at the failed height. */
        msg_processor_request_invalid_block_headers(mp, node);
    }

    if (validation_state_is_valid(&state)) {
        /* Block accepted — give the peer a decay tick. Peers that mostly
         * behave can work off earlier strikes; peers that only feed valid
         * blocks stay at score 0 forever. Safe on trusted peers. */
        peer_scoring_on_good_interaction(node, peer_scoring_now_ms());

        /* mark seen only after successful activation. If the
         * block is in block_index but NOT in active chain (e.g.
         * activation was skipped), leave it out of the dedup ring so
         * the next arrival retries and the controller has another
         * chance to pick it up once the mutex is free. */
        {
            struct block_index *landed = block_map_find(
                &mp->main_state->map_block_index, &hash);
            if (msg_blocks_should_mark_seen(&mp->main_state->chain_active,
                                             landed))
                block_mark_seen(&hash);
        }

        struct block_index *new_tip = active_chain_tip(
            &mp->main_state->chain_active);
        if (new_tip) {
            struct msg_block_acceptance acceptance;
            node->last_block_time = (int64_t)platform_time_wall_time_t();
            node->blocks_received++;
            msg_processor_plan_valid_block_acceptance(&acceptance, mp, node,
                                                      new_tip);
            event_emitf(EV_BLOCK_CONNECTED, (uint32_t)node->id,
                        "h=%d", new_tip->nHeight);
            /* Let the app runtime refresh its tip-advance observers without
             * making protocol handling depend on app-service ownership. */
            msg_processor_note_block_connected(mp, new_tip->nHeight);

            if (acceptance.reached_peer_tip) {
                if (acceptance.should_set_sync_state) {
                    sync_set_state(acceptance.next_sync_state,
                                   "caught up to peer");
                }
                if (acceptance.should_set_flush_policy)
                    set_flush_policy(3600, 500000, 100);
                if (acceptance.should_update_peer_state) {
                    peer_set_state_checked((uint32_t)node->id, &node->state,
                                           acceptance.next_peer_state,
                                           "chain caught up");
                }
                /* Start deferred HTTPS server now that it's safe */
                https_deferred_check();
                if (acceptance.should_emit_tip_updated)
                    event_emitf(EV_TIP_UPDATED, 0,
                                "AT_TIP height=%d",
                                new_tip->nHeight);
            }

            /* Progress logged by IBD progress timer (every 30s) */

            /* Refresh block manifest when chain grows beyond cached range.
             * Only at tip — during IBD, SQLite is still catching up and
             * manifest build can crash on partial data. We're a client
             * during IBD, not serving pieces to peers. */
            bool should_refresh_manifest = false;
            if (sync_get_state() == SYNC_AT_TIP && new_tip->nHeight > 1000) {
                struct block_piece_manifest header;
                int32_t built_at = 0;
                bool has_manifest =
                    msg_processor_get_block_manifest_header(&header,
                                                            &built_at);
                should_refresh_manifest =
                    !has_manifest ||
                    new_tip->nHeight - built_at >= MANIFEST_REFRESH_BLOCKS;
            }
            if (should_refresh_manifest) {
                /* Rebuild in a detached thread to avoid blocking message processing */
                static _Atomic bool g_manifest_rebuilding = false;
                if (!atomic_exchange(&g_manifest_rebuilding, true)) {
                    struct block_piece_manifest new_m;
                    memset(&new_m, 0, sizeof(new_m));
                    if (block_piece_manifest_build_active_chain(
                            &mp->main_state->chain_active, 1,
                            new_tip->nHeight, &new_m) ||
                        block_piece_manifest_build(mp->datadir, 1,
                            new_tip->nHeight, &new_m)) {
                        uint32_t num_pieces = new_m.num_pieces;
                        msg_processor_publish_block_manifest(
                            &new_m, new_tip->nHeight);
                        event_emitf(EV_SYNC_STATE_CHANGE, 0, "manifest refreshed to h=%d (%u pieces)",
                                    new_tip->nHeight, num_pieces);
                    }
                    atomic_store(&g_manifest_rebuilding, false);
                }
            }

            /* Relay accepted block to all connected peers (not during IBD).
             * At the tip, we act as a full relay node. During IBD, relaying
             * would flood peers with old blocks they already have.
             * BIP 130: peers that sent "sendheaders" get a direct headers
             * message (saves an inv→getheaders round-trip at the tip). */
            if (sync_get_state() == SYNC_AT_TIP && new_tip->phashBlock) {
                struct inv_item blk_inv;
                inv_item_init_typed(&blk_inv, MSG_BLOCK, new_tip->phashBlock);
                if (mp->net_mgr) {
                    zcl_mutex_lock(&mp->net_mgr->cs_nodes);
                    for (size_t pi = 0; pi < mp->net_mgr->num_nodes; pi++) {
                        struct p2p_node *peer = mp->net_mgr->nodes[pi];
                        if (peer->id != node->id &&
                            peer->state >= PEER_HANDSHAKE_COMPLETE &&
                            !peer->disconnect) {
                            if (peer->send_compact) {
                                /* BIP 152: send compact block directly */
                                struct block blk_cmp;
                                block_init(&blk_cmp);
                                if (read_block_from_disk_index(&blk_cmp, new_tip, mp->datadir)) {
                                    struct compact_block_msg cb;
                                    uint64_t nonce = (uint64_t)platform_time_wall_time_t() ^ (uint64_t)peer->id;
                                    if (compact_block_from_block(&cb, &blk_cmp, nonce)) {
                                        struct byte_stream cs;
                                        stream_init(&cs, 4096);
                                        if (compact_block_msg_serialize(&cb, &cs)) {
                                            p2p_node_begin_message(peer, "cmpctblock",
                                                                   mp->params->pchMessageStart);
                                            p2p_node_write_message_data(peer, cs.data, cs.size);
                                            p2p_node_end_message(peer);
                                        }
                                        stream_free(&cs);
                                        compact_block_msg_free(&cb);
                                    }
                                    block_free(&blk_cmp);
                                }
                            } else if (peer->prefer_headers) {
                                /* BIP 130: send headers directly */
                                struct block_header hdr;
                                block_header_init(&hdr);
                                hdr.nVersion = new_tip->nVersion;
                                if (new_tip->pprev && new_tip->pprev->phashBlock)
                                    hdr.hashPrevBlock = *new_tip->pprev->phashBlock;
                                hdr.hashMerkleRoot = new_tip->hashMerkleRoot;
                                hdr.hashFinalSaplingRoot = new_tip->hashFinalSaplingRoot;
                                hdr.nTime = new_tip->nTime;
                                hdr.nBits = new_tip->nBits;
                                hdr.nNonce = new_tip->nNonce;
                                if (new_tip->nSolution && new_tip->nSolutionSize > 0) {
                                    memcpy(hdr.nSolution, new_tip->nSolution, new_tip->nSolutionSize);
                                    hdr.nSolutionSize = new_tip->nSolutionSize;
                                } else {
                                    struct block blk_tip;
                                    if (read_block_from_disk_index(&blk_tip, new_tip, mp->datadir)) {
                                        memcpy(hdr.nSolution, blk_tip.header.nSolution, blk_tip.header.nSolutionSize);
                                        hdr.nSolutionSize = blk_tip.header.nSolutionSize;
                                        block_free(&blk_tip);
                                    }
                                }

                                struct byte_stream hs;
                                stream_init(&hs, 2048);
                                stream_write_compact_size(&hs, 1);
                                block_header_serialize(&hdr, &hs);
                                stream_write_compact_size(&hs, 0); /* tx count */
                                p2p_node_begin_message(peer, "headers",
                                                       mp->params->pchMessageStart);
                                p2p_node_write_message_data(peer, hs.data, hs.size);
                                p2p_node_end_message(peer);
                                stream_free(&hs);
                            } else {
                                p2p_node_push_inventory(peer, &blk_inv);
                            }
                        }
                    }
                    zcl_mutex_unlock(&mp->net_mgr->cs_nodes);
                }
            }
        }
    } else {
        int dos = 0;
        if (validation_state_get_dos(&state, &dos) && dos > 0) {
            event_emitf(EV_BLOCK_REJECTED, (uint32_t)node->id,
                        "dos=%d %s", dos, state.reject_reason);
            printf("Peer %s: invalid block (dos=%d): %s\n",
                   node->addr_name, dos, state.reject_reason);
            /* DoS from validation is graded: treat the common [50, 100]
             * range as the two typed categories so peer_offence_weight()
             * drives the score increment. Anything else (graded 1..49)
             * falls through to the raw peer_misbehaving() path so we
             * still honour the validator's exact grade — a constant
             * enum can't represent it. */
            const char *rr = state.reject_reason[0] ? state.reject_reason
                                                    : "invalid block";
            if (dos >= 100) {
                peer_scoring_record(mp->net_mgr, node,
                                    PEER_OFFENCE_INVALID_BLOCK, rr);
            } else if (dos >= 50) {
                peer_scoring_record(mp->net_mgr, node,
                                    PEER_OFFENCE_INVALID_HEADER, rr);
            } else {
                peer_misbehaving(mp->net_mgr, node, dos, rr);
            }
        } else if (!validation_state_is_valid(&state)) {
            /* DoS=0 but invalid: orphan block or parent-failed.
             * Don't penalize peer — this is normal during sync. */
        }
    }

    block_free(&blk);
    return true;
}
