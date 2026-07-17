/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Fast File Service — SHA3-encrypted, direct TCP, max bandwidth.
 *
 * Protocol: direct TCP connection, SHA3-CTR encrypted stream.
 * - Handshake: exchange nonces, derive key from blockchain UTXO root
 * - Transfer: 64KB frames, all same size, SHA3-authenticated
 * - Chunks: 50MB blocks of chain data, addressed by SHA3 hash
 *
 * Frame format (64KB fixed):
 *   [4-byte type][4-byte payload_len][payload][padding][32-byte MAC]
 *   Total: always exactly 65536 bytes. Indistinguishable from random.
 *
 * Frame types:
 *   0x01 HELLO     — nonce exchange (32-byte nonce)
 *   0x02 MANIFEST  — chunk list (sha3 hashes + sizes)
 *   0x03 REQUEST   — request chunk by sha3 hash
 *   0x04 DATA      — chunk data (may span multiple frames)
 *   0x05 DONE      — transfer complete
 *   0xFF PADDING   — keepalive / anti-analysis padding */

#ifndef ZCL_FILE_SERVICE_H
#define ZCL_FILE_SERVICE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "net/fast_sync.h"

#define FS_FRAME_SIZE    65536
#define FS_MAC_SIZE      32
#define FS_HEADER_SIZE   8     /* type(4) + payload_len(4) */
#define FS_MAX_PAYLOAD   (FS_FRAME_SIZE - FS_HEADER_SIZE - FS_MAC_SIZE)
#define FS_CHUNK_SIZE    (50 * 1024 * 1024)  /* 50 MB */
#define FS_PORT          18034               /* dedicated file service port */

/* Frame types */
#define FS_HELLO     0x01
#define FS_MANIFEST  0x02
#define FS_REQUEST   0x03
#define FS_DATA      0x04
#define FS_DONE      0x05
#define FS_CHALLENGE 0x06    /* PoW challenge (server→client) / challenge req */
#define FS_PADDING   0xFF

/* ── PoW-gated admission + resource caps (in-memory DDoS defense) ──────
 *
 * The bulk block/index stream (ALL/RNG) is public data, but streaming
 * multi-GB to any unauthenticated connection is a denial-of-service lever.
 * Before committing a worker to a large stream the server requires a valid
 * solution to a rotating, adaptive PoW challenge (fast_sync_pow_gate), then
 * bounds the spend with the caps below. None of this touches any consensus
 * predicate or wire consensus rule — the served bytes are byte-identical. */

/* PoW solution carried in a gated FS_REQUEST, prefixed before the request
 * body: [peer_token(32)][ts(8, LE)][nonce(8, LE)]. peer_token MUST equal the
 * handshake nonce the connection presented, binding the solution to this
 * connection. */
#define FS_POW_SOLUTION_SIZE 48

/* FS_CHALLENGE frame payload the server sends back:
 * [seed(32)][difficulty_bits(1)][server_time(8, LE)]. */
#define FS_CHALLENGE_PAYLOAD_SIZE 41

/* Per-IP concurrent large-serve cap. The honest client opens FS_NWORKERS (8)
 * parallel range connections to ONE seed, so this must clear that plus a
 * little slack for retries/manifest. */
#define FS_MAX_CONCURRENT_PER_IP 12

/* Per-connection ceilings — a single connection cannot stream forever. */
#define FS_CONN_MAX_BYTES   (4ULL * 1024 * 1024 * 1024) /* 4 GB / connection */
#define FS_CONN_MAX_SECONDS 1800                        /* 30 min / connection */

/* Per-IP volume ceiling. One full chain sync is ~7 GB across 8 workers; this
 * clears that plus retries while bounding a single abuser's uplink draw. */
#define FS_IP_MAX_BYTES_PER_HOUR (16ULL * 1024 * 1024 * 1024)

/* Per-IP accounting table size. */
#define FS_IP_TABLE_CAP 512

/* Admission verdict for a parsed large-serve request. */
enum fs_admit_result {
    FS_ADMIT_SERVE = 0,     /* valid puzzle within caps → stream the range   */
    FS_ADMIT_CHALLENGE,     /* missing/invalid/stale puzzle → send a challenge */
    FS_ADMIT_REFUSED_CAP    /* a resource cap tripped → refuse this serve     */
};

struct fs_session {
    int              fd;              /* TCP socket */
    uint8_t          key[32];         /* SHA3-derived session key */
    bool             key_established; /* true after HELLO exchange */
    uint8_t          our_nonce[32];
    uint8_t          peer_nonce[32];
    uint64_t         send_counter;    /* monotonic frame counter (nonce) */
    uint64_t         recv_counter;
    uint64_t         bytes_sent;
    uint64_t         bytes_received;
    int64_t          start_time;      /* for MB/s calculation */
    uint8_t          recv_payload[FS_MAX_PAYLOAD];
};

/* Initialize a session on an existing TCP socket. */
void fs_session_init(struct fs_session *s, int fd);

/* Perform HELLO handshake — exchange nonces, derive shared key.
 * utxo_root is the SHA3 of the UTXO set (proves both nodes on same chain).
 * is_initiator: true if we opened the connection. */
bool fs_handshake(struct fs_session *s, const uint8_t utxo_root[32],
                   bool is_initiator);

/* Send a frame (encrypts, pads to 64KB, MACs, sends). */
bool fs_send_frame(struct fs_session *s, uint8_t type,
                    const uint8_t *payload, uint32_t payload_len);

/* Receive a frame (reads 64KB, verifies MAC, decrypts).
 * type_out and payload_out are set. payload_out points into the
 * session-local receive buffer and is valid until the next
 * fs_recv_frame call on the same session. */
bool fs_recv_frame(struct fs_session *s, uint8_t *type_out,
                    const uint8_t **payload_out, uint32_t *payload_len_out);

/* High-level: serve files on configured port. Runs in its own thread. */
void fs_server_start(const char *datadir, uint16_t port);
void fs_server_stop(void);
bool fs_server_is_running(void);
uint16_t fs_server_get_port(void);
bool fs_server_refresh_manifest(void);

/* High-level: connect to peer and download all chunks. */
bool fs_client_sync(const char *peer_addr, uint16_t port,
                     const char *datadir, const uint8_t utxo_root[32]);

/* Get transfer speed stats. */
double fs_session_mbps(const struct fs_session *s);

/* ── PoW-gated admission + resource caps (testable, pure decisions) ────── */

/* The process-wide file-service PoW gate. */
struct fast_sync_pow_gate *fs_pow_gate(void);

/* Reset the gate + per-IP cap table (tests + a clean server start). */
void fs_pow_reset_state(void);

/* Parse a (possibly gated) FS_REQUEST payload. On return, *is_all / *is_rng
 * report the request kind, *puzzle points at the FS_POW_SOLUTION_SIZE-byte
 * solution prefix (or NULL if the request carried none), and for a range
 * request *rng_start / *rng_end are filled. Returns true if the payload is a
 * recognizable ALL or RNG request. */
bool fs_parse_serve_request(const uint8_t *payload, uint32_t plen,
                            bool *is_all, bool *is_rng,
                            const uint8_t **puzzle,
                            uint16_t *rng_start, uint16_t *rng_end);

/* Decide whether to admit a large serve. peer_token is the handshake nonce
 * this connection presented (the solution must be bound to it). On
 * FS_ADMIT_CHALLENGE, out_seed/out_bits/out_server_time are filled with a
 * fresh challenge to hand back to the client. Pure w.r.t. the wire — the
 * caller does the framing. */
enum fs_admit_result fs_admit_serve_pow(const uint8_t *puzzle,
                                        const uint8_t peer_token[32],
                                        uint8_t out_seed[32], int *out_bits,
                                        int64_t *out_server_time);

/* Per-IP concurrency cap. acquire() returns false if the IP already holds
 * FS_MAX_CONCURRENT_PER_IP active serves; every successful acquire MUST be
 * matched by exactly one release. */
bool fs_ip_serve_acquire(const uint8_t ip[16]);
void fs_ip_serve_release(const uint8_t ip[16]);

/* Charge n bytes to the IP's rolling hour budget. Returns false once the IP
 * exceeds FS_IP_MAX_BYTES_PER_HOUR (caller should stop serving). */
bool fs_ip_bytes_charge(const uint8_t ip[16], uint64_t n);

/* Per-connection budget predicate: false once the connection exceeds its
 * byte or wall-time ceiling. */
bool fs_conn_budget_ok(uint64_t bytes_sent, int64_t start_time, int64_t now);

/* ── Free-tier ROM artifact serving ──────────────────────────────────
 *
 * A ROM artifact chunk request rides the same fs_session transport but is
 * recognized independently of the ALL/RNG bulk-stream path:
 *   body = ["ROM"(3)][root_hash(32)][chunk_index(4, LE)]  (39 bytes)
 * ROM chunks are served FREE (no payment gate) but bounded by the rom_seed
 * per-peer concurrency + per-peer/global byte-rate caps — never the PoW/ALL
 * gate. Parsing is pure/testable. Returns true on a well-formed ROM request. */
#define FS_ROM_REQUEST_SIZE 39
bool fs_parse_rom_request(const uint8_t *payload, uint32_t plen,
                          uint8_t root_out[32], uint32_t *idx_out);

#endif
