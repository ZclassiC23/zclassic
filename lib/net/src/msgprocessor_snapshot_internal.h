/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Internal header shared between msgprocessor_snapshot.c and
 * msgprocessor_snapshot_serve.c. NOT part of the public API — only
 * included by those two files. Mirrors the connman.c/connman_dialer.c
 * split shape (see connman_internal.h): a small header for the pieces
 * one file needs to call into the other, promoted from `static` only
 * as needed.
 *
 * The split is by responsibility:
 *   msgprocessor_snapshot.c       — lifecycle (mp_snapshot_init,
 *                                   mp_snapshot_maybe_offer,
 *                                   mp_snapshot_send_tick's client-side
 *                                   swarm/block-swarm coordinators),
 *                                   the mp_handle_zcl23_sync dispatcher,
 *                                   the requester-side push_chunk_request
 *                                   / push_block_piece_request /
 *                                   parse_block_piece_payload_refs /
 *                                   block_payload_submit_all, and the
 *                                   fc_rate_* FlyClient-challenge limiter.
 *   msgprocessor_snapshot_serve.c — the SERVE side: cached offer/manifest
 *                                   /block-manifest publish+accessor
 *                                   APIs, send_snapshot_offer_msg,
 *                                   push_manifest, push_block_manifest,
 *                                   build_block_piece_payloads, the
 *                                   zchunkreq/zblkreq client-puzzle PoW
 *                                   guard, and the per-message-command
 *                                   serve handlers the dispatcher calls
 *                                   into (mp_serve_snapshot_req,
 *                                   mp_serve_chunk_req, mp_serve_block_req)
 *                                   plus the PEER_SNAPSHOT_SERVING chunk
 *                                   send loop (mp_snapshot_send_tick_serve).
 *
 * Everything declared here used to be `static` in msgprocessor_snapshot.c;
 * it is promoted to external linkage (single definition, still in
 * whichever file owns it) purely so the other file can call it — no
 * behavior changed by the split. */

#ifndef ZCL_NET_MSGPROCESSOR_SNAPSHOT_INTERNAL_H
#define ZCL_NET_MSGPROCESSOR_SNAPSHOT_INTERNAL_H

#include "net/msgprocessor.h"
#include "net/msg_internal.h"
#include "core/serialize.h"
#include <stdbool.h>

struct msg_processor;
struct p2p_node;
struct byte_stream;

/* Max on-wire bytes for one serialized block inside a zblkdata response —
 * shared because both the server (build_block_piece_payloads, this file's
 * MSG_BLOCK_REQ response) and the client (parse_block_piece_payload_refs,
 * msgprocessor_snapshot.c's MSG_BLOCK_DATA intake) must agree on the same
 * cap. */
#define BLOCK_PIECE_MAX_BLOCK_BYTES 2000000u

/* ── msgprocessor_snapshot_serve.c: called from the mp_handle_zcl23_sync
 * dispatcher and mp_snapshot_send_tick in msgprocessor_snapshot.c ──── */

/* Serve a zsnapreq (peer asking for our full snapshot). */
void mp_serve_snapshot_req(struct msg_processor *mp, struct p2p_node *node,
                           struct byte_stream *s);

/* Serve a zchunkreq (peer asking for one UTXO chunk by index). */
void mp_serve_chunk_req(struct msg_processor *mp, struct p2p_node *node,
                        struct byte_stream *s);

/* Serve a zblkreq (peer asking for one block piece by index). */
void mp_serve_block_req(struct msg_processor *mp, struct p2p_node *node,
                        struct byte_stream *s);

/* The PEER_SNAPSHOT_SERVING half of mp_snapshot_send_tick: streams
 * snapshot chunks to a peer we're actively serving. Returns true if the
 * caller must return immediately (matches the original inline `return;`
 * on a stale-offer reset — the swarm/block-swarm coordinator sections
 * below it in mp_snapshot_send_tick must NOT run in that case), false to
 * fall through to them normally. */
bool mp_snapshot_send_tick_serve(struct msg_processor *mp,
                                 struct p2p_node *node);

#endif /* ZCL_NET_MSGPROCESSOR_SNAPSHOT_INTERNAL_H */
