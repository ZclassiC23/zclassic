/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* msg_tx.c — Transaction relay message processing.
 * Split from msgprocessor.c for maintainability. */

#include "platform/time_compat.h"
#include "net/msg_internal.h"
#include "net/peer_scoring.h"
#include "net/dandelion.h"
#include "net/download.h"
#include "validation/check_transaction.h"
#include "validation/accept_to_mempool.h"
#include "consensus/validation.h"
#include "consensus/upgrades.h"
#include "event/event.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/sync.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── Dandelion (BIP 156) tx propagation ────────────────────────── */
struct dandelion_state g_dandelion;
bool g_dandelion_init = false;

/* Broadcast inv{MSG_TX} for hash to every relaying peer except
 * `exclude` (DANDELION_NODE_ID_NONE to relay to all). */
static void fluff_relay_inv(struct msg_processor *mp,
                            const struct uint256 *hash,
                            node_id_t exclude)
{
    if (!mp->net_mgr)
        return;
    struct inv_item inv;
    inv_item_init_typed(&inv, MSG_TX, hash);
    zcl_mutex_lock(&mp->net_mgr->cs_nodes);
    for (size_t pi = 0; pi < mp->net_mgr->num_nodes; pi++) {
        struct p2p_node *peer = mp->net_mgr->nodes[pi];
        if (peer->id != exclude &&
            peer->state >= PEER_HANDSHAKE_COMPLETE &&
            !peer->disconnect && peer->relay_txes)
            p2p_node_push_inventory(peer, &inv);
    }
    zcl_mutex_unlock(&mp->net_mgr->cs_nodes);
}

/* Send inv{MSG_DANDELION_TX} immediately to the stem destination
 * (stem-hop latency matters, so bypass the trickle), then record the
 * advert: BIP 156 says a stem tx is served ONLY to the peer it was
 * advertised to. Returns true if the announcement was sent. */
static bool dandelion_announce(struct msg_processor *mp, node_id_t dest,
                               const struct uint256 *hash)
{
    if (!mp->net_mgr || dest == DANDELION_NODE_ID_NONE)
        return false;

    struct inv_item inv;
    inv_item_init_typed(&inv, MSG_DANDELION_TX, hash);

    struct byte_stream msg;
    stream_init(&msg, 40);
    stream_write_compact_size(&msg, 1);
    inv_item_serialize(&inv, &msg);

    bool sent = false;
    zcl_mutex_lock(&mp->net_mgr->cs_nodes);
    for (size_t pi = 0; pi < mp->net_mgr->num_nodes; pi++) {
        struct p2p_node *sp = mp->net_mgr->nodes[pi];
        if (sp->id == dest &&
            sp->state >= PEER_HANDSHAKE_COMPLETE && !sp->disconnect) {
            p2p_node_begin_message(sp, "inv", mp->params->pchMessageStart);
            p2p_node_write_message_data(sp, msg.data, msg.size);
            p2p_node_end_message(sp);
            sent = true;
            break;
        }
    }
    zcl_mutex_unlock(&mp->net_mgr->cs_nodes);
    stream_free(&msg);

    if (sent)
        dandelion_mark_advertised(&g_dandelion, hash, dest);
    return sent;
}

/* Fluff a stem tx from its stored serialized bytes: enter the mempool
 * (the deferred insert BIP 156 requires) and diffuse via normal inv
 * relay. `exclude` is a peer not to relay back to. `from_peer` is
 * attributed in events (0 for none). Returns true if the tx is in the
 * mempool afterwards (newly accepted or already present). */
bool msg_tx_fluff_from_bytes(struct msg_processor *mp,
                             const struct uint256 *hash,
                             const uint8_t *bytes, size_t size,
                             node_id_t exclude, uint32_t from_peer)
{
    if (!mp || !hash || !bytes || size == 0)
        return false;

    struct byte_stream s;
    stream_init_from_data(&s, bytes, size);
    struct transaction tx;
    transaction_init(&tx);
    if (!transaction_deserialize(&tx, &s)) {
        transaction_free(&tx);
        LOG_FAIL("net", "dandelion fluff: failed to deserialize stored tx");
    }
    transaction_compute_hash(&tx);

    enum mempool_accept_result r =
        accept_to_mempool(mp->mempool, mp->coins_tip, mp->main_state,
                          mp->params, &tx);
    bool ok = (r == MEMPOOL_ACCEPT_OK || r == MEMPOOL_ACCEPT_DUPLICATE);

    if (r == MEMPOOL_ACCEPT_OK || r == MEMPOOL_ACCEPT_DUPLICATE) {
        /* DUPLICATE happens for wallet-originated stem txs: the wallet
         * commit already inserted them; the fluff is the (deferred)
         * public announcement. */
        tx_mark_seen(hash);
        if (r == MEMPOOL_ACCEPT_OK)
            event_emit(EV_TX_ACCEPTED, from_peer, hash->data, 32);
        fluff_relay_inv(mp, hash, exclude);
        if (g_dandelion_init)
            g_dandelion.stat_fluffed++;
        if (r == MEMPOOL_ACCEPT_OK && mp->wallet_tx_accepted)
            mp->wallet_tx_accepted(&tx, mp->wallet_tx_accepted_ctx);
    } else if (!ok) {
        char hex[65];
        uint256_get_hex(hash, hex);
        LOG_WARN("net", "dandelion fluff: tx %s rejected by mempool (%d)",
                 hex, (int)r);
    }

    transaction_free(&tx);
    return ok;
}

/* inv{MSG_TX} arrived for a tx sitting in our stempool: it has been
 * fluffed elsewhere, so adopt it into our mempool from the stored
 * bytes instead of re-requesting it. Returns true if adopted (caller
 * can skip the getdata). */
static bool dandelion_adopt_from_stempool(struct msg_processor *mp,
                                          const struct uint256 *hash,
                                          node_id_t from_peer)
{
    struct dandelion_fluff_item it;
    if (!dandelion_stempool_take(&g_dandelion, hash, &it))
        return false;
    bool ok = msg_tx_fluff_from_bytes(mp, hash, it.tx_bytes, it.tx_size,
                                      from_peer, (uint32_t)from_peer);
    free(it.tx_bytes);
    return ok;
}

/* ── incoming `tx` classification + scoring ──────────────
 *
 * Before, this handler silently upserted every deserialised tx,
 * ran no fee policy, and — critically — ran NO cryptographic
 * verification: a tx with a valid shape and existing prevouts but a
 * forged signature or shielded proof was admitted and fluffed
 * network-wide before block-connect rejected it.
 *
 * The acceptance gate (structural + shielded-proof + per-input
 * scriptSig verification + fee/conflict policy) now lives in the ONE
 * shared accept_to_mempool() helper (lib/validation), used by BOTH this
 * P2P path and the RPC sendrawtransaction path so they can never drift.
 * This wrapper only translates the shared result onto the net layer's
 * tx_accept_result, which the handler maps to peer ban-scoring. */
static enum tx_accept_result msg_tx_classify(struct msg_processor *mp,
                                              struct transaction *tx)
{
    if (!mp || !tx || !mp->mempool)
        return TX_ACCEPT_INTERNAL_ERROR;

    enum mempool_accept_result r =
        accept_to_mempool(mp->mempool, mp->coins_tip, mp->main_state,
                          mp->params, tx);

    switch (r) {
    case MEMPOOL_ACCEPT_OK:             return TX_ACCEPT_OK;
    case MEMPOOL_ACCEPT_INVALID:        return TX_ACCEPT_INVALID;
    case MEMPOOL_ACCEPT_DUPLICATE:      return TX_ACCEPT_DUPLICATE;
    case MEMPOOL_ACCEPT_CONFLICT:       return TX_ACCEPT_CONFLICT;
    case MEMPOOL_ACCEPT_BELOW_FEE:      return TX_ACCEPT_BELOW_FEE;
    case MEMPOOL_ACCEPT_MISSING_INPUTS: return TX_ACCEPT_MISSING_INPUTS;
    case MEMPOOL_ACCEPT_INTERNAL_ERROR: return TX_ACCEPT_INTERNAL_ERROR;
    }
    return TX_ACCEPT_INTERNAL_ERROR;
}

enum tx_accept_result msg_tx_accept(struct msg_processor *mp,
                                    struct p2p_node *node,
                                    struct transaction *tx)
{
    enum tx_accept_result ar = msg_tx_classify(mp, tx);

    /* Peer scoring: only clearly-malicious outcomes get ban-score.
     * Missing inputs (orphan), duplicates, and below-relay-fee are
     * either our problem or a rate-limit, not misbehaviour. */
    if (mp && node && mp->net_mgr) {
        switch (ar) {
        case TX_ACCEPT_INVALID:
            peer_scoring_record(mp->net_mgr, node,
                                PEER_OFFENCE_INVALID_MESSAGE,
                                "invalid tx");
            break;
        case TX_ACCEPT_CONFLICT:
            peer_scoring_record(mp->net_mgr, node,
                                PEER_OFFENCE_INVALID_MESSAGE,
                                "double-spend");
            break;
        case TX_ACCEPT_OK:
        case TX_ACCEPT_DUPLICATE:
        case TX_ACCEPT_BELOW_FEE:
        case TX_ACCEPT_MISSING_INPUTS:
        case TX_ACCEPT_INTERNAL_ERROR:
            break;
        }
    }

    return ar;
}

bool process_inv(struct msg_processor *mp, struct p2p_node *node,
                 struct byte_stream *s)
{
    uint64_t count;
    if (!stream_read_compact_size(s, &count))
        LOG_FAIL("net", "failed to read inv count from %s",
                 node->addr_name);

    if (count > MAX_INV_SZ) {
        event_emitf(EV_PEER_MISBEHAVE, (uint32_t)node->id,
                    "inv too large (%llu) from %s",
                    (unsigned long long)count, node->addr_name);
        printf("Peer %s: inv message too large (%llu)\n",
               node->addr_name, (unsigned long long)count);
        node->disconnect = true;
        LOG_FAIL("net", "inv count %llu exceeds MAX_INV_SZ from %s",
                 (unsigned long long)count, node->addr_name);
    }

    struct byte_stream getdata;
    stream_init(&getdata, 256);
    uint64_t request_count = 0;

    for (uint64_t i = 0; i < count; i++) {
        struct inv_item inv;
        if (!inv_item_deserialize(&inv, s)) {
            stream_free(&getdata);
            LOG_FAIL("net", "failed to deserialize inv[%llu] from %s",
                     (unsigned long long)i, node->addr_name);
        }

        p2p_node_add_inventory_known(node, &inv);

        if (inv.type == MSG_BLOCK) {
            if (block_already_seen(&inv.hash))
                continue;
            /* Don't request blocks during snapshot sync */
            if (msg_processor_snapshot_active(mp))
                continue;
            struct block_index *bi = block_map_find(
                &mp->main_state->map_block_index, &inv.hash);
            struct block_index *tip = active_chain_tip(
                &mp->main_state->chain_active);
            /* Match ZClassic C++ IsInitialBlockDownload + inv handling:
             * Request blocks directly when our tip is recent (within
             * PoWTargetSpacing*20 = 75*20 = 1500s of now). During IBD,
             * rely on headers-first sync instead.
             *
             * Old logic used node->starting_height - 1000 which kept
             * the node in headers-only mode too long after catchup. */
            bool in_ibd = !tip || !tip->nTime ||
                          ((int64_t)tip->nTime <
                           (int64_t)platform_time_wall_time_t() - 75 * 20);
            bool need_data = !bi || !(bi->nStatus & BLOCK_HAVE_DATA);
            if (need_data && !in_ibd) {
                /* ZClassic C++: send getheaders FIRST, then getdata.
                 * Headers arrive before (or with) the block, preventing
                 * orphan rejection if the block's parent is unknown. */
                push_getheaders_from(mp, node, tip);
                inv_item_serialize(&inv, &getdata);
                request_count++;
            } else if (need_data && in_ibd) {
                /* Ask for headers instead */
                push_getheaders_from(mp, node, tip);
            }
            if (tip && bi && active_chain_contains(
                    &mp->main_state->chain_active, bi)) {
                node->hash_continue = inv.hash;
            }
        } else if (inv.type == MSG_TX) {
            /* Dandelion: seeing a tx via inv means it's been fluffed.
             * Adopt it into the mempool from our stored stem bytes
             * (cancels the embargo) rather than re-requesting it. */
            if (g_dandelion_init &&
                dandelion_stempool_contains(&g_dandelion, &inv.hash) &&
                dandelion_adopt_from_stempool(mp, &inv.hash, node->id))
                continue;

            if (tx_already_seen(&inv.hash))
                continue;
            if (!tx_mempool_exists(mp->mempool, &inv.hash)) {
                inv_item_serialize(&inv, &getdata);
                request_count++;
            }
        } else if (inv.type == MSG_DANDELION_TX) {
            /* BIP 156 stem announcement: request the stem tx with a
             * dandelion getdata and remember we did, so the arriving
             * "tx" payload is routed down the stem path. */
            if (!g_dandelion_init || !dandelion_enabled())
                continue;
            if (tx_mempool_exists(mp->mempool, &inv.hash))
                continue;
            if (dandelion_stempool_contains(&g_dandelion, &inv.hash))
                continue;
            if (dandelion_request_pending(&g_dandelion, &inv.hash))
                continue;
            inv_item_serialize(&inv, &getdata);
            request_count++;
            dandelion_request_add(&g_dandelion, &inv.hash, node->id);
        }
    }

    if (request_count > 0) {
        struct byte_stream msg;
        stream_init(&msg, getdata.size + 8);
        stream_write_compact_size(&msg, request_count);
        stream_write(&msg, getdata.data, getdata.size);

        p2p_node_begin_message(node, "getdata", mp->params->pchMessageStart);
        p2p_node_write_message_data(node, msg.data, msg.size);
        p2p_node_end_message(node);
        stream_free(&msg);
    }
    stream_free(&getdata);
    return true;
}

/* A "tx" payload answering one of OUR getdata(MSG_DANDELION_TX)
 * requests — the BIP 156 stem receive path. The tx is validated in
 * full (dry-run: no mempool insert), then either forwarded along the
 * stem to this inbound edge's destination (90%) or fluffed (10%, or
 * when no Dandelion-capable destination exists). Stem txs live in the
 * stempool only; the mempool insert happens at fluff. The caller owns
 * and frees tx. */
static bool process_dandelion_tx(struct msg_processor *mp,
                                 struct p2p_node *node,
                                 struct transaction *tx,
                                 const struct uint256 *hash)
{
    g_dandelion.stat_stem_recv++;

    /* Already known (mempool or stempool): nothing to do. */
    if (tx_mempool_exists(mp->mempool, hash) ||
        dandelion_stempool_contains(&g_dandelion, hash))
        return true;

    /* Full validation without insert: an invalid tx must not ride the
     * stem (DoS vector) — same gate as normal relay. */
    enum mempool_accept_result r =
        accept_to_mempool_ex(mp->mempool, mp->coins_tip, mp->main_state,
                             mp->params, tx, true);

    bool stem_chained = false;
    switch (r) {
    case MEMPOOL_ACCEPT_OK:
        break;
    case MEMPOOL_ACCEPT_DUPLICATE:
        return true;
    case MEMPOOL_ACCEPT_MISSING_INPUTS:
        /* Tolerate only stem-chained parents (a parent still under
         * embargo in OUR stempool). Anything else is dropped:
         * dandelion txs are never orphan-pooled (BIP 156). */
        for (size_t i = 0; i < tx->num_vin && !stem_chained; i++) {
            if (dandelion_stempool_contains(&g_dandelion,
                                            &tx->vin[i].prevout.hash))
                stem_chained = true;
        }
        if (!stem_chained)
            return true;
        break;
    case MEMPOOL_ACCEPT_INVALID:
        peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_MESSAGE,
                            "invalid dandelion tx");
        return true;
    case MEMPOOL_ACCEPT_CONFLICT:
        peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_MESSAGE,
                            "dandelion double-spend");
        return true;
    case MEMPOOL_ACCEPT_BELOW_FEE:
    case MEMPOOL_ACCEPT_INTERNAL_ERROR:
        return true;
    }

    dandelion_maybe_rotate_epoch(&g_dandelion, mp->net_mgr);

    /* Relay-hop fluff coin (DANDELION_FLUFF_PROB%). Stem-chained
     * orphans must keep stemming — they cannot enter the mempool until
     * their parent fluffs. */
    bool continue_stem = stem_chained ||
                         dandelion_should_stem(&g_dandelion, node->id);
    node_id_t dest = DANDELION_NODE_ID_NONE;
    if (continue_stem)
        dest = dandelion_get_stem_peer(&g_dandelion, node->id);

    if (dest == DANDELION_NODE_ID_NONE && !stem_chained) {
        /* Fluff here: enter the mempool and diffuse normally. */
        struct byte_stream bs;
        stream_init(&bs, 512);
        bool ok = transaction_serialize(tx, &bs);
        if (ok)
            msg_tx_fluff_from_bytes(mp, hash, bs.data, bs.size,
                                    node->id, (uint32_t)node->id);
        stream_free(&bs);
        if (!ok)
            LOG_FAIL("net", "dandelion: failed to serialize tx for fluff");
        return true;
    }

    /* Continue the stem: hold under a random embargo and forward the
     * dandelion announcement to this edge's destination. If the
     * announce fails (peer raced away), the embargo (and the
     * destination-gone check) fluffs it later — the tx cannot be
     * lost. */
    struct byte_stream bs;
    stream_init(&bs, 512);
    if (!transaction_serialize(tx, &bs)) {
        stream_free(&bs);
        LOG_FAIL("net", "dandelion: failed to serialize stem tx");
    }
    bool added = dandelion_stempool_add(&g_dandelion, hash, node->id, dest,
                                        bs.data, bs.size);
    stream_free(&bs);
    if (!added)
        return true; /* duplicate race or OOM (logged inside) */

    if (dandelion_announce(mp, dest, hash))
        g_dandelion.stat_stem_sent++;
    return true;
}

bool process_tx_msg(struct msg_processor *mp, struct p2p_node *node,
                    struct byte_stream *s)
{
    struct transaction tx;
    transaction_init(&tx);
    if (!transaction_deserialize(&tx, s)) {
        event_emitf(EV_MSG_DESERIALIZATION_FAIL, (uint32_t)node->id, "tx");
        peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_MESSAGE,
                            "malformed tx");
        transaction_free(&tx);
        LOG_FAIL("net", "failed to deserialize tx from %s",
                 node->addr_name);
    }

    struct uint256 hash;
    transaction_compute_hash(&tx);
    hash = tx.hash;

    /* BIP 156: a tx answering one of our dandelion getdata requests is
     * a stem tx — it goes to the stempool path, never to the dedup
     * ring (it must still be processable when it later arrives via
     * normal diffusion). */
    if (g_dandelion_init &&
        dandelion_request_take(&g_dandelion, &hash, node->id)) {
        bool ok = process_dandelion_tx(mp, node, &tx, &hash);
        transaction_free(&tx);
        return ok;
    }

    if (tx_already_seen(&hash)) {
        transaction_free(&tx);
        return true;
    }
    tx_mark_seen(&hash);

    enum tx_accept_result ar = msg_tx_accept(mp, node, &tx);

    if (ar == TX_ACCEPT_OK) {
        event_emit(EV_TX_ACCEPTED, (uint32_t)node->id,
                   hash.data, 32);
        struct inv_item inv;
        inv_item_init_typed(&inv, MSG_TX, &hash);
        p2p_node_add_inventory_known(node, &inv);

        /* Normally-received txs diffuse normally (BIP 156: the stem
         * coin applies only to dandelion txs; pre-BIP this path also
         * stem-routed every relayed tx, which just delayed relay). */
        if (g_dandelion_init)
            g_dandelion.stat_fluffed++;
        fluff_relay_inv(mp, &hash, node->id);

        /* If we were stemming this same tx, it's now public — drop the
         * embargo copy. */
        if (g_dandelion_init)
            dandelion_stempool_remove(&g_dandelion, &hash);

        if (mp->wallet_tx_accepted)
            mp->wallet_tx_accepted(&tx, mp->wallet_tx_accepted_ctx);
    } else {
        event_emit(EV_TX_REJECTED, (uint32_t)node->id,
                   hash.data, 32);
    }

    transaction_free(&tx);
    return true;
}

/* test hook: when non-NULL, process_mempool calls this instead
 * of zcl_malloc for the scratch buffer. Returning NULL simulates OOM.
 * File-scope so the hook can only influence this one call site. */
static void *(*g_process_mempool_alloc_hook)(size_t) = NULL;

void msgprocessor_test_set_mempool_alloc_hook(void *(*hook)(size_t))
{
    g_process_mempool_alloc_hook = hook;
}

bool process_mempool(struct msg_processor *mp, struct p2p_node *node)
{
    size_t bytes = (size_t)MAX_INV_SZ * sizeof(struct uint256);
    struct uint256 *hashes = g_process_mempool_alloc_hook
        ? g_process_mempool_alloc_hook(bytes)
        : zcl_malloc(bytes, "mempool_inv_hashes");
    if (!hashes)
        LOG_FAIL("net", "process_mempool: OOM (%zu bytes)", bytes);

    size_t num = 0;
    tx_mempool_query_hashes(mp->mempool, hashes, MAX_INV_SZ, &num);

    for (size_t i = 0; i < num; i++) {
        /* BIP 156: a tx under stem embargo is not public — keep it out
         * of the BIP35 mempool reply (this is the probe that would
         * otherwise reveal wallet-originated stem txs). */
        if (g_dandelion_init &&
            dandelion_stempool_contains(&g_dandelion, &hashes[i]))
            continue;
        struct inv_item inv;
        inv_item_init_typed(&inv, MSG_TX, &hashes[i]);
        p2p_node_push_inventory(node, &inv);
    }
    free(hashes);
    return true;
}
