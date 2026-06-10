/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_snapshot_loader: runtime mmap+SHA3-verify+iterate over a
 * UTXO snapshot sidecar produced by `zclassic23 --gen-utxo-snapshot`.
 *
 * Sidecar format (see main.c gen_utxo_snapshot_mode for the writer):
 *
 *   Header (104 bytes, all little-endian):
 *     magic[8]      = "ZCLUTXO\x00"
 *     version u32   = 1
 *     reserved u32
 *     height u32    = anchor height (informational)
 *     reserved u32
 *     count u64     = number of vouts (records) that follow
 *     total_supply i64 = sum of all values in sats
 *     anchor_block_hash[32]
 *     sha3_hash[32] = SHA3-256 over the body bytes
 *
 *   Body: `count` records, each:
 *     txid[32], vout u32 LE, value i64 LE,
 *     script_len u32 LE, script[script_len],
 *     height u32 LE, is_coinbase u8
 *
 * The per-record encoding matches utxo_commitment_sha3_compute()
 * so the body sha3 equals the compile-time checkpoint when the
 * sidecar was generated at the anchor height (h=3,056,758).
 */

#ifndef ZCL_CHAIN_UTXO_SNAPSHOT_LOADER_H
#define ZCL_CHAIN_UTXO_SNAPSHOT_LOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct uss_header {
    uint32_t version;
    uint32_t height;
    uint64_t count;
    int64_t  total_supply;
    uint8_t  anchor_block_hash[32];
    uint8_t  sha3_hash[32];
};

struct uss_record {
    const uint8_t *txid;     /* 32 bytes */
    uint32_t       vout;
    int64_t        value;
    const uint8_t *script;
    uint32_t       script_len;
    uint32_t       height;
    uint8_t        is_coinbase;
};

struct uss_handle;

/* Open a sidecar file. mmap'd MAP_PRIVATE PROT_READ.
 * On success returns a heap-allocated handle and fills *hdr.
 * Caller frees with uss_close().
 *
 * VERIFY MODE: if `verify_full_sha3` is true, the entire body is
 * SHA3-hashed up front and compared to hdr.sha3_hash. This is
 * essentially mandatory for cold-start trust — the call takes
 * ~200 ms on AVX-512 and binds the body before any UTXO is
 * inserted into the database.
 *
 * If `expected_sha3` is non-NULL, additionally compares hdr.sha3_hash
 * to that 32-byte buffer. Useful to bind the sidecar to a
 * compile-time checkpoint. */
struct uss_handle *uss_open(const char *path,
                            bool verify_full_sha3,
                            const uint8_t *expected_sha3,
                            struct uss_header *hdr,
                            char *err, size_t err_sz);

void uss_close(struct uss_handle *h);

typedef bool (*uss_record_cb)(const struct uss_record *r, void *ctx);

/* Iterate every record. Stops early if cb returns false.
 * Returns count emitted, or -1 on truncation. */
int64_t uss_iter(struct uss_handle *h, uss_record_cb cb, void *ctx);

#endif /* ZCL_CHAIN_UTXO_SNAPSHOT_LOADER_H */
