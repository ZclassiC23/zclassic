/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ROM artifact fetching — the CLIENT half of docs/ROM_DELIVERY.md. Downloads
 * a ROM artifact (today: the consensus-state bundle) from a seeding peer over
 * the file-service transport and verifies it BY CONTENT against digests the
 * caller committed to before the first byte was requested.
 *
 * Trust model: delivery is untrusted transport. This module verifies bytes —
 * per-chunk transport MACs on the wire, then a whole-file re-hash against the
 * committed (chunk_root, whole_sha3, size) after the download — and nothing
 * more. It NEVER installs, activates, or validates consensus state: the only
 * activation door is the unified installer's RECEIPT / CHECKPOINT_CONTENT
 * authority (config/src/boot_install_consensus_bundle.c), which an operator
 * runs separately against the verified file this module leaves on disk.
 *
 * Wire protocol (mirrors the serve side, lib/net/src/file_service.c):
 *   - TCP connect to the peer's file-service port; fs_handshake with an
 *     all-zero utxo_root (the ROM serve path keys its sessions on zeros —
 *     see fs_handle_client_fd, file_service.c:984-986).
 *   - One FS_REQUEST frame, body ["ROM"(3)][chunk_root(32)][chunk_index(4 LE)]
 *     (FS_ROM_REQUEST_SIZE, net/file_service.h).
 *   - Success reply: raw stream [4-byte size LE][data][32-byte MAC] with
 *     MAC = SHA3(key || counter || chunk_sha3 || data). The client learns the
 *     chunk's content digest only by hashing the received bytes — per-chunk
 *     digests are not on the wire today, so CONTENT verification happens at
 *     whole-file granularity (chunk_root fold + whole_sha3), exactly as
 *     docs/ROM_DELIVERY.md's trust model specifies.
 *   - Refusal reply: an FS_DONE frame — indistinguishable from a corrupt
 *     stream at this layer, so it surfaces as a chunk failure the caller may
 *     retry. Fail-closed either way.
 *
 * This module owns no threads and no sockets beyond one short-lived
 * connection per rom_fetch_chunk() call. The caller drives pacing,
 * parallelism, and resume. */

#ifndef ZCL_NET_ROM_FETCH_H
#define ZCL_NET_ROM_FETCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "net/rom_seed.h" /* ROM_SEED_* bounds shared with the serve side */

/* Bounds on the manifest table a caller holds while discovering artifacts. */
#define ROM_FETCH_MAX_ARTIFACTS  ROM_SEED_MAX_ARTIFACTS
#define ROM_FETCH_NAME_MAX       ROM_SEED_NAME_MAX

/* Staging suffix for the in-progress download; renamed to the committed
 * filename only after whole-file verification passes. */
#define ROM_FETCH_PART_SUFFIX    ".part"

/* A manifest the caller has COMMITTED to before downloading. Every field is
 * either operator-supplied or parsed (and range-checked) from a peer's
 * directory document; the digests are the whole trust anchor of the
 * download. */
struct rom_fetch_manifest {
    bool     used;
    char     filename[ROM_FETCH_NAME_MAX]; /* bare basename, sanitized     */
    uint64_t size_bytes;
    uint32_t chunk_size;                   /* must be ROM_SEED_CHUNK_SIZE  */
    uint32_t num_chunks;
    uint8_t  chunk_root[32];               /* serve-request key            */
    uint8_t  whole_sha3[32];               /* whole-file digest            */
};

/* Progress callback for rom_fetch_download: called after each chunk lands
 * (chunks_done of num_chunks). Returning false aborts the download. */
typedef bool (*rom_fetch_progress_cb)(uint32_t chunks_done,
                                      uint32_t num_chunks,
                                      uint64_t bytes_done, void *ctx);

/* ── Manifest validation + discovery parse ──────────────────────────── */

/* Validate internal consistency + sanitize `filename` of a caller-filled
 * manifest: chunk_size == ROM_SEED_CHUNK_SIZE, num_chunks ==
 * ceil(size/chunk_size), size within [ROM_SEED_MIN_ARTIFACT_BYTES,
 * ROM_SEED_MAX_ARTIFACT_BYTES], filename a bare basename (no separators, no
 * traversal). An empty filename is allowed (directory.json entries carry no
 * name; the caller assigns one before downloading). Pure, does not mutate. */
bool rom_fetch_manifest_sane(const struct rom_fetch_manifest *m);

/* Parse the "artifacts" array of a peer's /directory.json body into out[]
 * (capacity max, capped at ROM_FETCH_MAX_ARTIFACTS). Each entry is
 * range-checked with rom_fetch_manifest_sane(); malformed entries are
 * skipped, not fatal. Returns the number of valid manifests, or -1 if the
 * JSON itself is unparseable. Peer-supplied: the digests are the ONLY thing
 * carried forward. */
int rom_fetch_parse_directory(const char *json_body,
                              struct rom_fetch_manifest *out, size_t max);

/* ── Verified chunk fetch ───────────────────────────────────────────── */

/* Fetch chunk `idx` of the artifact keyed by `chunk_root` from
 * peer_addr:port over the file-service transport. buf must hold at least
 * ROM_SEED_CHUNK_SIZE bytes; *out_sz gets the actual bytes (the last chunk
 * may be short). Transport-MAC-verified; content is NOT digest-checked here
 * (see the header comment). Returns false on connect/handshake/transport/
 * refusal/MAC failure. One connection per call; 120 s socket timeouts. */
bool rom_fetch_chunk(const char *peer_addr, uint16_t port,
                     const uint8_t chunk_root[32], uint32_t idx,
                     uint8_t *buf, uint32_t buf_cap, uint32_t *out_sz);

/* ── Whole-file verification + download driver ──────────────────────── */

/* Re-hash `path` in one bounded streaming pass and require ALL of:
 * file size == m->size_bytes, per-chunk SHA3 fold == m->chunk_root,
 * whole-file SHA3 == m->whole_sha3. Pure disk + SHA3; no network. */
bool rom_fetch_verify_file(const char *path,
                           const struct rom_fetch_manifest *m);

/* Sequential download: fetch every chunk of `m` from peer_addr:port into
 * <out_dir>/<filename>.part (pwrite at chunk offsets), then
 * rom_fetch_verify_file and rename to <out_dir>/<filename>. On a digest
 * mismatch the .part file is UNLINKED (no partial trust); on a transport
 * failure the .part is left in place for a future resume. A refused chunk
 * (seeder rate window / in-flight cap — both clear in ~1 s) is retried a
 * bounded number of times with backoff before it counts as failed; the
 * download therefore self-paces to a stock seeder's default 8 MB/s window
 * instead of dying at the second chunk. The installed file is delivered
 * read-only (mode 0444) — the unified installer's immutable admission
 * accepts it with no manual chmod. Returns false with a logged reason
 * on any failure. The optional cb runs after each chunk. Every attempt is
 * recorded in the fetch status below. */
bool rom_fetch_download(const char *peer_addr, uint16_t port,
                        const struct rom_fetch_manifest *m,
                        const char *out_dir,
                        rom_fetch_progress_cb cb, void *cb_ctx);

/* ── Parallel multi-seeder download ─────────────────────────────────── */

/* One seeder endpoint (host + file-service port). */
struct rom_fetch_peer {
    char     addr[128];
    uint16_t port;
};

/* Hard cap on worker threads per parallel download. The serve-side per-peer
 * in-flight cap is small (default 2), so the caller should size workers at
 * ~2x the peer count; more workers than that just queue on refusals. */
#define ROM_FETCH_MAX_WORKERS 8u

/* Parallel download: worker threads pull chunk indices from a shared queue;
 * chunk i is first tried on peers[i % npeers], and on failure retried on
 * each subsequent peer (round-robin) — a chunk is declared failed only
 * after EVERY peer refused/failed it. Each chunk is transport-MAC-verified
 * on arrival and pwrite()n at its offset into <out_dir>/<filename>.part;
 * the whole-file content proof + atomic rename (or unlink on mismatch) is
 * identical to rom_fetch_download. `workers` is clamped to
 * [1, ROM_FETCH_MAX_WORKERS]. Returns false (leaving .part for resume) if
 * any chunk failed on all peers. */
bool rom_fetch_download_parallel(const struct rom_fetch_peer *peers,
                                 size_t npeers,
                                 const struct rom_fetch_manifest *m,
                                 const char *out_dir, uint32_t workers,
                                 rom_fetch_progress_cb cb, void *cb_ctx);

/* ── Fetch status (observability; powers dumpstate rom_fetch) ───────── */

struct rom_fetch_status {
    bool     ever_attempted;
    bool     in_progress;
    bool     last_ok;
    char     peer[80];        /* "addr:port" of the last attempt          */
    char     filename[ROM_FETCH_NAME_MAX];
    char     detail[192];     /* installed path or a short failure reason */
    uint64_t size_bytes;
    uint32_t num_chunks;
    uint32_t chunks_done;
    uint64_t bytes_done;
    int64_t  started_unix;
    int64_t  finished_unix;
    /* Cumulative counters over the process lifetime. */
    uint64_t attempts;
    uint64_t successes;
    uint64_t failures;
    uint64_t bytes_total;     /* verified-and-installed bytes             */
};

/* Copy out a consistent snapshot of the fetch status. Safe from any thread. */
void rom_fetch_status_snapshot(struct rom_fetch_status *out);

/* See CLAUDE.md "Adding state introspection". Reentrant-safe. */
struct json_value;
bool rom_fetch_dump_state_json(struct json_value *out, const char *key);

#endif /* ZCL_NET_ROM_FETCH_H */
