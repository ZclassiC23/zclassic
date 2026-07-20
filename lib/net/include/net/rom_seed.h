/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ROM artifact seeding — the free, fast, capped P2P delivery tier for the
 * network's own bootstrap ROM: the consensus-state bundle
 * (zcl.consensus_state_bundle.v1, "consensus-state-bundle-<h>.sqlite") and
 * header-chain seed data. A fresh node fetches these P2P and reaches the
 * compiled checkpoint in minutes instead of folding for hours.
 *
 * Trust model: DELIVERY is untrusted transport. Registration re-derives every
 * digest from the bytes on disk (never a sidecar), and a downloader re-verifies
 * each chunk's SHA3 against the manifest and the whole file against the
 * checkpoint content proof. A malicious seeder wastes bandwidth, never poisons
 * state — so seeding is generous (price 0, no payment gate) but bounded by hard
 * per-peer concurrency + per-peer/global byte-rate caps and served off the
 * file-service's own thread pool so it never starves consensus P2P.
 *
 * This module is pure registry + policy + caps + stats. It owns no threads and
 * no sockets; the boot scan worker (config/) drives registration and the file
 * service (lib/net/file_service.c) drives the serve path through the decision
 * functions below. Every wire-derived field is bounded and validated here. */

#ifndef ZCL_NET_ROM_SEED_H
#define ZCL_NET_ROM_SEED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Layout constants ───────────────────────────────────────────────── */

/* 4 MB serving chunk: a 513 MB consensus bundle is ~129 chunks. Small enough
 * that a lost/corrupt chunk is cheap to re-fetch, large enough that framing
 * overhead is negligible. */
#define ROM_SEED_CHUNK_SIZE       (4u * 1024u * 1024u)

/* Bounds. MAX_CHUNKS * CHUNK_SIZE = 4 GB caps a single artifact; MAX_ARTIFACTS
 * bounds the registry. Both tables are static — never sized from a wire value. */
#define ROM_SEED_MAX_CHUNKS       1024u
#define ROM_SEED_MAX_ARTIFACTS    8u
#define ROM_SEED_NAME_MAX         128u

/* A ROM artifact must be at least one SQLite page and at most MAX_CHUNKS worth
 * of CHUNK_SIZE. Anything outside is not a bundle we will serve. */
#define ROM_SEED_MIN_ARTIFACT_BYTES  4096ull
#define ROM_SEED_MAX_ARTIFACT_BYTES  \
    ((uint64_t)ROM_SEED_MAX_CHUNKS * (uint64_t)ROM_SEED_CHUNK_SIZE)

/* Per-peer accounting table size (bounds memory under an IP flood). */
#define ROM_SEED_PEER_TABLE_CAP   256u

/* Sane default caps (generous but bounded). Overridable via the setters. */
#define ROM_SEED_DEFAULT_MAX_INFLIGHT_PER_PEER  2u
#define ROM_SEED_DEFAULT_PEER_BPS_CAP   (8ull * 1024 * 1024)   /*  8 MB/s / peer  */
#define ROM_SEED_DEFAULT_GLOBAL_BPS_CAP (64ull * 1024 * 1024)  /* 64 MB/s total   */

/* ── Artifact kinds ─────────────────────────────────────────────────── */

enum rom_artifact_kind {
    ROM_ARTIFACT_UNKNOWN = 0,
    ROM_ARTIFACT_CONSENSUS_BUNDLE = 1,  /* consensus-state-bundle-<h>.sqlite */
    ROM_ARTIFACT_HEADER_SEED = 2,       /* header-chain seed (block_index.bin) */
};

/* A registered, content-verified artifact. `chunk_root` (SHA3 over the
 * concatenated per-chunk digests) is the artifact's content identity — it is
 * the root_hash used in gossip and the key serve requests carry. */
struct rom_artifact {
    enum rom_artifact_kind kind;
    char     filename[ROM_SEED_NAME_MAX]; /* basename within the datadir       */
    uint64_t size_bytes;
    uint32_t chunk_size;
    uint32_t num_chunks;
    uint8_t  whole_sha3[32];              /* SHA3-256 of the whole file        */
    uint8_t  chunk_root[32];              /* SHA3-256 over per-chunk digests   */
    uint8_t  chunk_sha3[ROM_SEED_MAX_CHUNKS][32];
    int64_t  registered_at;
    bool     used;
};

/* ── Registration ───────────────────────────────────────────────────── */

enum rom_register_result {
    ROM_REG_OK = 0,
    ROM_REG_ERR_ARGS,          /* NULL / empty / traversal in filename        */
    ROM_REG_ERR_NOT_FOUND,     /* file missing or unopenable                  */
    ROM_REG_ERR_UNKNOWN_KIND,  /* filename matches no known artifact kind     */
    ROM_REG_ERR_TOO_SMALL,     /* below ROM_SEED_MIN_ARTIFACT_BYTES           */
    ROM_REG_ERR_TOO_LARGE,     /* above ROM_SEED_MAX_ARTIFACT_BYTES           */
    ROM_REG_ERR_CORRUPT,       /* structural check failed OR digest mismatch  */
    ROM_REG_ERR_IO,            /* read error mid-stream                        */
    ROM_REG_ERR_FULL,          /* registry full                               */
};

/* Classify an artifact kind from its basename (no I/O). */
enum rom_artifact_kind rom_seed_classify(const char *filename);

/* Structural content check for a kind, given the first `n` header bytes and the
 * total file size. Pure — this is what makes a corrupt/truncated file fail
 * registration BEFORE it is ever offered. */
bool rom_seed_kind_content_ok(enum rom_artifact_kind kind,
                              const uint8_t *header, size_t n,
                              uint64_t size_bytes);

/* Register (or re-register) `filename` inside `datadir`. Computes every digest
 * from the bytes on disk in one bounded pass. If `expected_whole_sha3` is
 * non-NULL it must match the computed whole-file digest or registration is
 * refused as ROM_REG_ERR_CORRUPT. On ROM_REG_OK the artifact is in the registry
 * and (if `out` non-NULL) copied out. `filename` must be a bare basename. */
enum rom_register_result rom_seed_register(const char *datadir,
                                           const char *filename,
                                           const uint8_t *expected_whole_sha3,
                                           struct rom_artifact *out);

/* Bounded datadir scan: register every entry matching a known artifact kind
 * (today: consensus-state-bundle-*.sqlite). Returns the number registered.
 * Bounded by ROM_SEED_MAX_ARTIFACTS and a fixed directory-entry ceiling. */
int rom_seed_scan_datadir(const char *datadir);

/* Supervised, single-shot background scan of `datadir`: registers matching
 * artifacts (one bounded digest pass) and announces each as a price-0 offer
 * into the in-memory market. `fs_port` is stamped on the offers. Idempotent;
 * a no-op when seeding is disabled. Joined by rom_seed_stop_scan(). */
void rom_seed_start_scan(const char *datadir, uint16_t fs_port);
void rom_seed_stop_scan(void);

/* ── Registry queries ───────────────────────────────────────────────── */

void rom_seed_reset(void);                    /* clear registry + caps + stats */
int  rom_seed_count(void);
int  rom_seed_list(struct rom_artifact *out, size_t max);
bool rom_seed_find_by_root(const uint8_t root_hash[32], struct rom_artifact *out);

/* Read chunk `idx` of a registered artifact from disk into `buf` (capacity
 * `buf_cap`), verifying its SHA3 against the registered per-chunk digest.
 * `*out_sz` gets the actual bytes (the last chunk may be short). */
bool rom_seed_read_chunk(const struct rom_artifact *a, const char *datadir,
                         uint32_t idx, uint8_t *buf, uint32_t buf_cap,
                         uint32_t *out_sz);

/* ── Free-tier serve policy ─────────────────────────────────────────── */

enum rom_serve_verdict {
    ROM_SERVE_FREE_OK = 0,     /* registered free artifact, chunk in range     */
    ROM_SERVE_DISABLED,        /* seeding disabled by config                   */
    ROM_SERVE_NOT_ARTIFACT,    /* unknown root — NOT free (payment path owns it)*/
    ROM_SERVE_OUT_OF_RANGE,    /* chunk_index >= num_chunks                    */
};

/* Stateless: is (root_hash, chunk_index) a free ROM chunk we should serve
 * without payment? Fills `out` with the matched artifact on ROM_SERVE_FREE_OK.
 * The caller still applies the concurrency + rate caps below. */
enum rom_serve_verdict rom_seed_serve_lookup(const uint8_t root_hash[32],
                                             uint32_t chunk_index,
                                             struct rom_artifact *out);

/* Convenience: true iff root_hash names a registered (free) ROM artifact. The
 * market payment gate consults this to skip payment verification. */
bool rom_seed_offer_is_free(const uint8_t root_hash[32]);

/* ── Caps (in-memory DDoS bound; nothing here is consensus) ─────────── */

void     rom_seed_set_enabled(bool on);
bool     rom_seed_enabled(void);
void     rom_seed_set_max_inflight_per_peer(uint32_t n);
void     rom_seed_set_peer_bps_cap(uint64_t bytes_per_sec);
void     rom_seed_set_global_bps_cap(uint64_t bytes_per_sec);

/* Per-peer in-flight concurrency. acquire() returns false once the peer holds
 * max_inflight_per_peer active serves; each success MUST be released exactly
 * once. */
bool rom_seed_peer_acquire(const uint8_t peer_ip[16]);
void rom_seed_peer_release(const uint8_t peer_ip[16]);

/* Charge `n` bytes to the peer's and the global rolling-1s byte-rate windows.
 * Returns false (serve should stop) once either window would exceed its cap.
 * Records served-bytes + unique-peer + chunk stats on success. */
bool rom_seed_rate_charge(const uint8_t peer_ip[16], uint64_t n, int64_t now);

/* Note a chunk actually served (for stats). Call once per delivered chunk. */
void rom_seed_note_chunk_served(void);

/* ── Announce + introspection ───────────────────────────────────────── */

struct file_offer;

/* Build a price-0 market offer advertising a registered artifact. root_hash is
 * the artifact's chunk_root. Returns false on NULL args. */
bool rom_seed_build_offer(const struct rom_artifact *a,
                          const uint8_t self_ip[16], uint16_t fs_port,
                          struct file_offer *out);

/* Append the artifacts JSON array body (no enclosing key) to `buf`, e.g.
 *   [{"kind":"consensus_bundle","digest":"..","size":N,"chunks":M}, ...]
 * Returns the number of bytes written (0 on overflow / no artifacts). */
size_t rom_seed_directory_json(char *buf, size_t max);

/* See CLAUDE.md "Adding state introspection". Reentrant-safe. */
struct json_value;
bool rom_seed_dump_state_json(struct json_value *out, const char *key);

/* ── Per-chunk manifest serialization (WF2 artifact-protocol, serve side) ──
 *
 * The seeder already holds every per-chunk SHA3 in RAM (rom_artifact.chunk_sha3);
 * this serializes them for the "RMF" manifest reply:
 *   [u32 version][u32 num_chunks][num_chunks × 32B chunk_sha3]
 * under the existing file-service MAC scheme. STEP-0 STATUS: contract + stub;
 * lane 2A lands the real serializer. */

/* Max serialized manifest-blob size: header (8) + one 32-byte digest per
 * chunk, bounded by ROM_SEED_MAX_CHUNKS. */
#define ROM_SEED_MANIFEST_BLOB_MAX (8u + ROM_SEED_MAX_CHUNKS * 32u)

/* Serialize `a`'s per-chunk digest manifest into `buf` (capacity `cap`).
 * Returns the number of bytes written, or 0 on NULL args / overflow. Pure. */
size_t rom_seed_manifest_blob(const struct rom_artifact *a,
                              uint8_t *buf, size_t cap);

#endif /* ZCL_NET_ROM_SEED_H */
