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
#define FS_PADDING   0xFF

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

#endif
