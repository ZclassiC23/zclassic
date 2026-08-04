/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: buyer-authenticated paid-file request and delivery gate. */

#ifndef ZCL_NET_FILE_MARKET_DELIVERY_H
#define ZCL_NET_FILE_MARKET_DELIVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FILE_MARKET_DELIVERY_VERSION 1u
#define FILE_MARKET_DELIVERY_BODY_BYTES 142u
#define FILE_MARKET_DELIVERY_WIRE_BYTES 206u
#define FILE_MARKET_DELIVERY_REPLY_BYTES 84u
#define FS_MARKET_REPLY 0x07u

struct fs_session;

/* zfileget.v1 names one chunk from one signed offer. session_id is derived
 * from network genesis plus the initiator/responder handshake nonces. The
 * buyer signature therefore cannot be copied onto another file-service
 * connection. The buyer seed is accepted only by the seal helper and never
 * enters this struct, the wire, persistence, logs, or public output. */
struct file_market_delivery_request {
    uint16_t version;
    uint8_t network_genesis[32];
    uint8_t offer_id[32];
    uint32_t chunk_index;
    uint8_t buyer_pubkey[32];
    uint8_t session_id[32];
    uint8_t buyer_signature[64];
};

enum file_market_delivery_error {
    FILE_MARKET_DELIVERY_OK = 0,
    FILE_MARKET_DELIVERY_ERR_NULL,
    FILE_MARKET_DELIVERY_ERR_VERSION,
    FILE_MARKET_DELIVERY_ERR_WIRE_SIZE,
    FILE_MARKET_DELIVERY_ERR_WIRE_MAGIC,
    FILE_MARKET_DELIVERY_ERR_NETWORK,
    FILE_MARKET_DELIVERY_ERR_OFFER_ID,
    FILE_MARKET_DELIVERY_ERR_BUYER_KEY,
    FILE_MARKET_DELIVERY_ERR_SESSION,
    FILE_MARKET_DELIVERY_ERR_SIGNATURE,
    FILE_MARKET_DELIVERY_ERR_KEY_MISMATCH,
};

const char *file_market_delivery_error_string(
    enum file_market_delivery_error error);
void file_market_delivery_session_id(
    const uint8_t network_genesis[32],
    const uint8_t initiator_nonce[32],
    const uint8_t responder_nonce[32], uint8_t out[32]);
enum file_market_delivery_error file_market_delivery_request_encode(
    const struct file_market_delivery_request *request,
    uint8_t out[FILE_MARKET_DELIVERY_WIRE_BYTES]);
enum file_market_delivery_error file_market_delivery_request_decode(
    const uint8_t *wire, size_t wire_len,
    struct file_market_delivery_request *out);
enum file_market_delivery_error file_market_delivery_request_seal(
    struct file_market_delivery_request *request,
    const uint8_t buyer_seed[32]);
enum file_market_delivery_error file_market_delivery_request_verify(
    const struct file_market_delivery_request *request,
    const uint8_t expected_network_genesis[32],
    const uint8_t expected_session_id[32]);

/* App-layer authorization is deliberately separate from content loading.
 * file_market_delivery_prepare() always calls authorize first and calls load
 * only for FILE_MARKET_DELIVERY_AUTHORIZED. This ordering is the fail-closed
 * boundary that prevents stale, pending, conflicted, or unauthenticated
 * payment state from causing a seller content read. */
enum file_market_delivery_authorization {
    FILE_MARKET_DELIVERY_AUTHORIZED = 0,
    FILE_MARKET_DELIVERY_PENDING,
    FILE_MARKET_DELIVERY_UNKNOWN,
    FILE_MARKET_DELIVERY_CONFLICTED,
    FILE_MARKET_DELIVERY_REJECTED,
};

typedef enum file_market_delivery_authorization
(*file_market_delivery_authorize_fn)(
    const uint8_t offer_id[32], const uint8_t buyer_pubkey[32],
    uint32_t chunk_index, void *ctx);

struct file_market_delivery_chunk {
    uint8_t *data;       /* zcl_malloc-owned; delivery layer frees it */
    uint32_t size;
    uint8_t sha3[32];
};

typedef bool (*file_market_delivery_load_fn)(
    const uint8_t offer_id[32], uint32_t chunk_index,
    struct file_market_delivery_chunk *out, void *ctx);

enum file_market_delivery_status {
    FILE_MARKET_DELIVERY_READY = 0,
    FILE_MARKET_DELIVERY_MALFORMED,
    FILE_MARKET_DELIVERY_UNAUTHENTICATED,
    FILE_MARKET_DELIVERY_PAYMENT_PENDING,
    FILE_MARKET_DELIVERY_PAYMENT_UNKNOWN,
    FILE_MARKET_DELIVERY_PAYMENT_CONFLICTED,
    FILE_MARKET_DELIVERY_PAYMENT_REJECTED,
    FILE_MARKET_DELIVERY_CONTENT_UNAVAILABLE,
    FILE_MARKET_DELIVERY_RESOURCE_LIMIT,
};

struct file_market_delivery_reply {
    uint16_t version;
    enum file_market_delivery_status status;
    uint8_t offer_id[32];
    uint32_t chunk_index;
    uint32_t size;
    uint8_t sha3[32];
};

const char *file_market_delivery_status_string(
    enum file_market_delivery_status status);
bool file_market_delivery_reply_encode(
    const struct file_market_delivery_reply *reply,
    uint8_t out[FILE_MARKET_DELIVERY_REPLY_BYTES]);
bool file_market_delivery_reply_decode(
    const uint8_t *wire, size_t wire_len,
    struct file_market_delivery_reply *out);

/* Immutable-after-boot dependency injection. A NULL load callback keeps the
 * production path fail-closed until owner-registered seller content exists. */
void file_market_delivery_set_handlers(
    const uint8_t expected_network_genesis[32],
    file_market_delivery_authorize_fn authorize,
    file_market_delivery_load_fn load, void *ctx);
void file_market_delivery_reset_handlers(void);

/* Purely recognizes the zfileget magic, including malformed/truncated bodies
 * long enough to carry it. Other FS_REQUEST protocols remain untouched. */
bool file_market_delivery_is_request(const uint8_t *payload, uint32_t plen);

/* Verify + authorize + conditionally load. `out_chunk->data` is non-NULL only
 * for READY and belongs to the caller, which must free it. */
enum file_market_delivery_status file_market_delivery_prepare(
    const struct fs_session *session, const uint8_t *payload, uint32_t plen,
    struct file_market_delivery_reply *out_reply,
    struct file_market_delivery_chunk *out_chunk);

/* Server integration: prepare, send one encrypted typed reply, then send raw
 * authenticated bytes only for READY. Returns false only on transport error. */
bool file_market_delivery_serve(
    struct fs_session *session, const uint8_t client_ip[16],
    const uint8_t *payload, uint32_t plen);

#endif /* ZCL_NET_FILE_MARKET_DELIVERY_H */
