/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * event_log — append-only durable event stream primitive.
 *
 * The event log is the durable source for reducer/projection replay.
 * This file ships only the primitive; boot/runtime code wires concrete
 * producers and projections. See `docs/FRAMEWORK.md` for the current
 * reducer authority model.
 *
 * Wire format (per event)
 * -----------------------
 *   [ 4B  payload_length  (LE) ]
 *   [ 4B  event_type      (LE) ]
 *   [ 4B  flags           (LE) ]   <- reserved (0)
 *   [ 4B  payload_crc32c  (LE) ]   <- Castagnoli polynomial 0x1EDC6F41
 *   [ NB  payload              ]
 *   [ 16B fsync_sentinel       ]   <- 8B magic + 8B event_start_offset
 *
 * The fsync sentinel is the load-bearing design choice: it makes torn
 * writes recoverable without ambiguity.
 *
 *   1. Write the header + payload (16 + N bytes).
 *   2. fsync(fd).
 *   3. Write the sentinel (16 bytes: magic + start_offset).
 *   4. fsync(fd).
 *
 * On replay/recovery:
 *   - Scan from end-of-file backward.
 *   - The last event must end with the magic sentinel whose embedded
 *     offset equals the event's start position.
 *   - If the sentinel is absent or wrong (torn write at step 3 or 4 or
 *     somewhere mid-payload), TRUNCATE the file at the start of the
 *     partial event.
 *   - Result: no torn writes are ever observable to readers.
 *
 * Threading
 * ----------
 * The implementation takes a per-log mutex around append; reads use
 * pread and are safe under concurrent readers + a single appender.
 *
 * The append backend may change, but the wire format is frozen and must
 * remain stable. */

#ifndef ZCL_STORAGE_EVENT_LOG_H
#define ZCL_STORAGE_EVENT_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct event_log event_log_t;

/* Event types for the event log. The tag is `event_log_type` (NOT
 * `event_type`) to avoid colliding with `lib/event/event.h`'s system-
 * wide event taxonomy, which is for in-memory observability, not the
 * persistent log. The two namespaces are intentionally separate. */
enum event_log_type {
    EV_BLOCK_HEADER         = 1,
    EV_BLOCK_BODY           = 2,
    EV_TX_ADMIT_MEMPOOL     = 3,
    EV_TX_REMOVE_MEMPOOL    = 4,
    EV_UTXO_ADD             = 5,
    EV_UTXO_SPEND           = 6,
    EV_PEER_OBSERVED        = 7,
    EV_PEER_DROPPED         = 8,
    EV_WALLET_KEY_ADD       = 9,
    EV_WALLET_TX_SEEN       = 10,
    EV_STAGE_CURSOR_ADVANCE = 11,
    /* ZNAM projection events. */
    EV_ZNAM_REGISTER        = 12,
    EV_ZNAM_UPDATE          = 13,
    EV_ZNAM_TRANSFER        = 14,
    EV_ZNAM_RENEW           = 15,
    EV_ZNAM_EXPIRE          = 16,
    /* Wallet-view projection public-only events. */
    EV_WALLET_ADDR_DERIVED  = 17,
    EV_WALLET_NOTE_DECRYPTED = 18,
    EV_WALLET_UTXO_SEEN     = 19,
    /* Small-batch projection events. */
    EV_CONTACT_SET          = 20,
    EV_CONTACT_TOUCHED      = 21,
    EV_CONTACT_DELETE       = 22,
    EV_ONION_ANNOUNCEMENT   = 23,
    EV_HODL_SNAPSHOT        = 24,
    /* ZVCS (lib/vcs) commit log — appended by vcs_snapshot(). */
    EV_VCS_COMMIT           = 25,
    /* Add cautiously — every entry is a permanent wire surface. */
};

/* On-disk magic for the fsync sentinel — printable mnemonic
 * "ZCLEVTLG" (0x5A434C45_56544C47, big-endian); we store little-endian
 * on disk so the byte sequence on disk reads "ZCLEVTLG". */
#define EVENT_LOG_SENTINEL_MAGIC UINT64_C(0x474C5456454C435A)

/* Per-event framing overhead: 16B header (len/type/flags/crc, see wire
 * format above) + 16B fsync sentinel. Given a stream callback's
 * (offset, len), the next event begins at offset + EVENT_LOG_FRAME_OVERHEAD
 * + len. */
#define EVENT_LOG_FRAME_OVERHEAD 32u

/* Maximum payload length (defensive cap — 256 MiB). Larger payloads
 * indicate a bug in the caller and are rejected. */
#define EVENT_LOG_MAX_PAYLOAD ((size_t)(256u * 1024u * 1024u))

/* Open or create the event log at `path`. On open, the file is scanned
 * from the tail to detect and TRUNCATE any partial trailing event
 * (recovery from torn writes). Returns NULL if `path` is NULL or empty,
 * if the file cannot be opened, if recovery fails, or on allocation
 * failure. */
event_log_t *event_log_open(const char *path);

/* Close the log. Flushes any pending data. NULL-safe. */
void event_log_close(event_log_t *log);

/* Append one event. Returns the absolute byte offset at which the
 * event was written (suitable as a stable event id) on success, or
 * UINT64_MAX on error. The on-disk format is fully durable (fsync x2)
 * before this call returns. Errors (all return UINT64_MAX): `log` NULL;
 * `payload` NULL with `payload_len > 0`; `payload_len` exceeds
 * EVENT_LOG_MAX_PAYLOAD; or any pwrite/fsync failure (the lock is
 * released before returning). `payload_len == 0` is valid. */
uint64_t event_log_append(event_log_t *log,
                          enum event_log_type type,
                          const void *payload, size_t payload_len);

/* Read one event at `offset` (which must have been returned by a prior
 * append or stream callback). The payload CRC and fsync sentinel are
 * always verified. On a verified event the true payload length is
 * written to `*out_len` and the type to `*type_out` (both optional —
 * pass NULL to skip), then the payload is copied into `buf` only if
 * `buf` is non-NULL and `buf_cap >= payload_len`. Returns 0 on success,
 * -1 on error: `log` NULL; `offset` out of range or the framed event
 * extends past end-of-log; corrupt length; pread failure; CRC mismatch;
 * bad sentinel; allocation failure; or `buf_cap < payload_len` (caller
 * must size the buffer correctly — note `*out_len`/`*type_out` may have
 * been set before this -1). */
int event_log_read(event_log_t *log, uint64_t offset,
                   enum event_log_type *type_out,
                   void *buf, size_t buf_cap, size_t *out_len);

/* Callback for event_log_stream. Return `true` to continue iteration,
 * `false` to stop. `payload` is owned by the streamer and only valid
 * for the duration of the call. */
typedef bool (*event_log_cb)(uint64_t offset, enum event_log_type type,
                              const void *payload, size_t len,
                              void *user);

/* Stream events starting at `start_offset` (0 = beginning). Stops at
 * EOF or when `cb` returns false. Returns 0 on success, -1 on error.
 * Does not call `cb` for partial trailing events (those are truncated
 * at open). */
int event_log_stream(event_log_t *log, uint64_t start_offset,
                     event_log_cb cb, void *user);

/* SHA3-256 over the on-disk payload bytes of every event in order —
 * excludes per-event headers, CRCs, and sentinels (framing only).
 * Identical content → identical fingerprint. Returns 0 on success. */
int event_log_fingerprint(event_log_t *log, uint8_t out[32]);

/* Current size of the log file in bytes (post-recovery). For tests
 * and observability. */
uint64_t event_log_size(event_log_t *log);

#ifdef ZCL_TESTING
const char *event_log_crc32c_impl(void);
uint32_t event_log_crc32c_test_sw(const void *data, size_t len);
uint32_t event_log_crc32c_test_active(const void *data, size_t len);
bool event_log_crc32c_hw_available(void);
#endif

#endif /* ZCL_STORAGE_EVENT_LOG_H */
