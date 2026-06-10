/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZCL Messaging (ZMSG) — Encrypted P2P messaging.
 *
 * Two modes:
 *   Off-chain: direct P2P messages between connected nodes (instant, free)
 *   On-chain:  Sapling memo field in shielded transaction (permanent, paid)
 *
 * P2P messages:
 *   zmsg    — encrypted message delivery
 *   zmsgack — delivery acknowledgment */

#ifndef ZCL_NET_ZMSG_H
#define ZCL_NET_ZMSG_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* P2P message commands */
#define MSG_ZMSG     "zmsg"
#define MSG_ZMSG_ACK "zmsgack"

/* Limits */
#define ZMSG_MAX_BODY    4096
#define ZMSG_MAX_ADDR    128
#define ZMSG_MAX_STORED  1000

/* Direction */
#define ZMSG_INBOUND  0
#define ZMSG_OUTBOUND 1

/* Channel */
#define ZMSG_CHANNEL_ONCHAIN 0
#define ZMSG_CHANNEL_P2P     1

/* On-chain memo magic */
#define ZMSG_MEMO_MAGIC_0  0x5A  /* 'Z' */
#define ZMSG_MEMO_MAGIC_1  0x4D  /* 'M' */

/* ── P2P Message Struct ─────────────────────────────────────────── */

struct zmsg_message {
    uint8_t  msg_id[32];        /* SHA3(timestamp || sender || body) */
    int      direction;         /* ZMSG_INBOUND / ZMSG_OUTBOUND */
    int      channel;           /* ZMSG_CHANNEL_* */
    char     sender[ZMSG_MAX_ADDR];
    char     recipient[ZMSG_MAX_ADDR];
    char     body[ZMSG_MAX_BODY];
    int64_t  timestamp;
    uint8_t  txid[32];          /* non-zero for on-chain */
    bool     read;
};

/* ── Serialization ──────────────────────────────────────────────── */

struct byte_stream;

/* Write msg onto stream s in wire form (msg_id, timestamp, then
 * length-prefixed sender/recipient/body). Caller must pass non-NULL
 * msg and s; neither is null-checked. sender/recipient are capped at
 * 127 bytes and body at ZMSG_MAX_BODY on the wire. Returns true only
 * if every field was written. */
bool zmsg_serialize(const struct zmsg_message *msg, struct byte_stream *s);

/* Read a message from stream s into msg (zeroed first). Caller must
 * pass non-NULL msg and s; neither is null-checked. Peer-controlled
 * length prefixes are bounds-checked against the fixed-size fields.
 * Returns true only if every field was read and in range; false on a
 * short/over-long stream. */
bool zmsg_deserialize(struct zmsg_message *msg, struct byte_stream *s);

/* Compute msg_id from message contents */
void zmsg_compute_id(const struct zmsg_message *msg, uint8_t out[32]);

/* ── In-Memory Message Store ────────────────────────────────────── */

/* Add a message to the store. Returns true if new. */
bool zmsg_store_add(const struct zmsg_message *msg);

/* Get messages. Returns count written to out (up to max). */
int zmsg_store_list(struct zmsg_message *out, size_t max,
                    bool unread_only);

/* Mark a message as read by msg_id. */
bool zmsg_store_mark_read(const uint8_t msg_id[32]);

/* Get message count. */
int zmsg_store_count(void);

/* ── SQLite Persistence ─────────────────────────────────────────── */

struct node_db;

bool db_zmsg_save(struct node_db *ndb, const struct zmsg_message *msg);
int db_zmsg_list(struct node_db *ndb, struct zmsg_message *out,
                 size_t max, bool unread_only);
bool db_zmsg_mark_read(struct node_db *ndb, const uint8_t msg_id[32]);

#endif
