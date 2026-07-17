/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Internal header shared between the msgprocessor split files.
 * NOT part of the public API — only included by msg_*.c files. */

#ifndef ZCL_NET_MSG_INTERNAL_H
#define ZCL_NET_MSG_INTERNAL_H

#include "net/msgprocessor.h"
#include "net/net.h"
#include "net/p2p_message.h"
#include "core/serialize.h"
#include "sync/sync_state.h"

/* ── Forward declarations for split message handlers ──────────── */

struct sync_getheaders_action;

struct msg_block_acceptance {
    bool reached_peer_tip;
    bool should_emit_tip_updated;
    bool should_set_sync_state;
    enum sync_state next_sync_state;
    bool should_set_flush_policy;
    bool should_update_peer_state;
    enum peer_state next_peer_state;
};

/* msg_version.c — version/verack handshake */
void push_version(struct msg_processor *mp, struct p2p_node *node);
void push_verack(struct msg_processor *mp, struct p2p_node *node);
void msg_version_build(struct version_message *ver,
                       const struct msg_processor *mp,
                       const struct p2p_node *node,
                       int start_height);
bool msg_version_learn_advertised_addr(struct net_manager *nm,
                                       const struct p2p_node *node,
                                       const struct version_message *ver);
bool msg_version_should_save_peer(const struct p2p_node *node);
bool process_version(struct msg_processor *mp, struct p2p_node *node,
                     struct byte_stream *s);
bool process_verack(struct msg_processor *mp, struct p2p_node *node);

/* msg_headers.c — header sync messages */
bool process_getheaders(struct msg_processor *mp, struct p2p_node *node,
                        struct byte_stream *s);
bool process_headers(struct msg_processor *mp, struct p2p_node *node,
                     struct byte_stream *s);
void push_getheaders(struct msg_processor *mp, struct p2p_node *node);
void push_getheaders_from(struct msg_processor *mp,
                          struct p2p_node *node,
                          struct block_index *from);
void exec_getheaders_action(struct msg_processor *mp,
                            struct p2p_node *node,
                            const struct sync_getheaders_action *action);

/* NET-3 range-parallel header acquisition (msg_headers.c).
 *
 * push_getheaders_span: issue a getheaders whose locator forks at
 * `start_hash` and whose hash_stop is `stop_hash` (NULL = unbounded
 * forward). Used to bound a peer to its assigned span.
 *
 * msg_try_range_parallel_getheaders: when >=2 fast-sync-capable peers are
 * connected and the missing-header gap exceeds one wire batch, partition
 * the range into disjoint checkpoint-anchored spans and drive THIS peer's
 * span. Returns true iff it issued the request (caller then skips the
 * normal single-peer getheaders). Returns false — leaving the existing
 * path untouched — when the conditions don't hold, so single-peer sync
 * behaves exactly as before. Never fires while a header-band hole is open
 * (the band backfill owns the anchor then). */
void push_getheaders_span(struct msg_processor *mp, struct p2p_node *node,
                          const struct uint256 *start_hash,
                          const struct uint256 *stop_hash);
bool msg_try_range_parallel_getheaders(struct msg_processor *mp,
                                       struct p2p_node *node,
                                       int our_height, int64_t now_seconds);

/* msg_blocks.c — block handling */
bool process_block_msg(struct msg_processor *mp, struct p2p_node *node,
                       struct byte_stream *s);
bool process_getdata(struct msg_processor *mp, struct p2p_node *node,
                     struct byte_stream *s);
bool process_getblocks(struct msg_processor *mp, struct p2p_node *node,
                       struct byte_stream *s);

/* msg_tx.c — transaction relay */
bool process_tx_msg(struct msg_processor *mp, struct p2p_node *node,
                    struct byte_stream *s);
bool process_inv(struct msg_processor *mp, struct p2p_node *node,
                 struct byte_stream *s);
bool process_mempool(struct msg_processor *mp, struct p2p_node *node);

/* Mempool sync-on-connect: after a successful handshake, pull the peer's
 * mempool inventory ONCE by sending an outbound "mempool" message (no
 * payload) — the peer answers via its own process_mempool() above. Gated
 * on node->relay_txes (no point asking a peer that told us it won't relay)
 * and on not being deep in IBD (bulk historical sync has no use for
 * mempool inventory). node->mempool_requested makes this idempotent per
 * peer regardless of how many times the caller invokes it (e.g. a
 * misbehaving peer resending verack). Returns true iff the request was
 * actually queued this call. */
bool msg_tx_maybe_request_mempool(struct msg_processor *mp,
                                  struct p2p_node *node);

/* classification outcome for an incoming `tx` message. The
 * handler needs to differentiate malicious rejections (apply peer
 * ban-score) from non-malicious rejections (orphan, duplicate,
 * rate-limit) and success. Exposed to tests so regression cases can
 * exercise the classifier without re-entering Dandelion + wallet
 * side effects that `process_tx_msg` does after acceptance. */
enum tx_accept_result {
    TX_ACCEPT_OK = 0,
    TX_ACCEPT_INVALID,          /* failed check_transaction / coinbase */
    TX_ACCEPT_DUPLICATE,        /* already in mempool */
    TX_ACCEPT_CONFLICT,         /* double-spend vs current mempool */
    TX_ACCEPT_BELOW_FEE,        /* fee < min_relay_fee */
    TX_ACCEPT_MISSING_INPUTS,   /* unknown inputs (orphan) */
    TX_ACCEPT_NONFINAL,         /* nLockTime policy rejection */
    TX_ACCEPT_EXPIRING_SOON,    /* expiry-height policy rejection */
    TX_ACCEPT_INTERNAL_ERROR,   /* mempool full / OOM */
};

/* Classify + add-or-reject a transaction, applying peer scoring for
 * malicious outcomes. Does NOT dedupe against the tx_already_seen
 * cache (the handler does that first). Does NOT relay (Dandelion /
 * wallet sync stays in `process_tx_msg`). */
enum tx_accept_result msg_tx_accept(struct msg_processor *mp,
                                    struct p2p_node *node,
                                    struct transaction *tx);

/* msg_compact.c — compact blocks (BIP152) */
bool process_sendcmpct(struct msg_processor *mp, struct p2p_node *node,
                       struct byte_stream *s);
bool process_cmpctblock(struct msg_processor *mp, struct p2p_node *node,
                        struct byte_stream *s);
bool process_getblocktxn(struct msg_processor *mp, struct p2p_node *node,
                         struct byte_stream *s);
bool process_blocktxn(struct msg_processor *mp, struct p2p_node *node,
                      struct byte_stream *s);

/* ── Shared helpers (remain in msgprocessor.c) ────────────────── */

/* Access the download manager singleton. */
struct download_manager *msg_get_download_mgr(void);
#define get_download_mgr() msg_get_download_mgr()

/* Access the cached snapshot offer/manifest. */
void send_snapshot_offer_msg(struct p2p_node *node,
                             const struct snapshot_offer *offer,
                             const unsigned char *msg_start);
void push_manifest(struct msg_processor *mp, struct p2p_node *node);
void push_block_manifest(struct msg_processor *mp, struct p2p_node *node);

/* Block/tx dedup ring buffers. */
bool block_already_seen(const struct uint256 *hash);
void block_mark_seen(const struct uint256 *hash);
void block_clear_seen(const struct uint256 *hash);
bool tx_already_seen(const struct uint256 *hash);
void tx_mark_seen(const struct uint256 *hash);

/* decide whether a freshly processed block may safely be added to the dedup
 * ring. Historically, every received block was marked seen BEFORE the
 * synchronous block-intake path; if intake SKIP'd (e.g.
 * ACTIVATION_SKIP_ALREADY_RUNNING from controller mutex contention), the block
 * was indexed-but-not-connected AND permanently dedup'd, leaving it stuck in
 * block_index forever.
 *
 * Returns true only when the block is in the active chain —
 * i.e. has actually been activated, not just received and
 * indexed. Any other state (NULL pindex, orphan, skipped) returns
 * false so the dedup ring does NOT short-circuit subsequent
 * arrivals. */
struct active_chain;
struct block_index;
struct validation_state;
bool msg_block_validation_is_retryable(const struct validation_state *state);
bool msg_processor_enqueue_p2p_block(struct msg_processor *mp,
                                     const struct block *blk,
                                     const struct uint256 *hash,
                                     uint32_t peer_id,
                                     struct validation_state *out);
bool msg_blocks_should_mark_seen(const struct active_chain *chain,
                                  const struct block_index *bi);
bool msg_processor_snapshot_active(const struct msg_processor *mp);
struct block_index *msg_processor_snapshot_anchor(const struct msg_processor *mp);
void msg_processor_set_snapshot_anchor(const struct msg_processor *mp,
                                       struct block_index *anchor);
void msg_processor_request_activation(const struct msg_processor *mp,
                                      enum msg_activation_request_source source);
void msg_processor_clear_activation_anchor(const struct msg_processor *mp,
                                           const char *reason);
void msg_processor_repair_post_activation_anchor(const struct msg_processor *mp);
int msg_processor_scan_block_files(const struct msg_processor *mp);
bool msg_processor_block_index_heights_repaired(const struct msg_processor *mp);
bool msg_processor_commit_header_tip(const struct msg_processor *mp,
                                     struct block_index *header_tip);
bool msg_processor_recommit_snapshot_anchor(const struct msg_processor *mp,
                                            struct block_index *anchor,
                                            int from_height);
void msg_processor_note_block_connected(const struct msg_processor *mp,
                                        int height);
void msg_processor_record_peer_header_vote(const struct msg_processor *mp,
                                           uint32_t peer_id,
                                           int height,
                                           const char hash_hex[65]);
void msg_processor_request_invalid_block_headers(struct msg_processor *mp,
                                                 struct p2p_node *node);
void msg_processor_plan_valid_block_acceptance(
    struct msg_block_acceptance *out,
    const struct msg_processor *mp,
    const struct p2p_node *node,
    const struct block_index *new_tip);

/* Shared accessors. */
struct node_db *msg_node_db(const struct msg_processor *mp);
struct snapshot_sync_service *msg_snapshot_sync(const struct msg_processor *mp);
struct snapshot_sync_service *msg_snapshot_sync_ensure(const struct msg_processor *mp);

/* Dandelion state (in msg_tx.c). */
struct dandelion_state;
extern struct dandelion_state g_dandelion;
extern bool g_dandelion_init;

#endif /* ZCL_NET_MSG_INTERNAL_H */
