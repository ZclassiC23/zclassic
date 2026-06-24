/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * snapshot_from_coinskv — write a ZCLUTXO snapshot from an already-seeded
 * coins_kv (progress.kv) at a given height + block hash. Read-only over the
 * coins set; emits the SAME canonical stream as the -mint-anchor ceremony
 * and the anchor self-mint, so the resulting file is loadable by
 * -load-snapshot-at-own-height (which self-SHA3-verifies the body and binds
 * hdr.anchor_block_hash to the in-memory active chain at the seed height).
 *
 * Usage:
 *   snapshot_from_coinskv <datadir> <height> <blockhash_display_hex> <out_path>
 *     datadir   datadir containing progress.kv (the coins_kv home)
 *     height    seed height to stamp (must match the chain's block at that h)
 *     blockhash big-endian DISPLAY hex (as RPC shows it); reversed internally
 *     out_path  snapshot file to write
 */

#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>

#include "storage/progress_store.h"
#include "storage/coins_kv.h"

/* Provided by the node binary's main.c; this standalone tool defines its own
 * so the shared object set links (shutdown is irrelevant for a one-shot run). */
volatile sig_atomic_t g_shutdown_requested = 0;

static int hexnib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr,
                "usage: %s <datadir> <height> <blockhash_display_hex> "
                "<out_path>\n", argv[0]);
        return 2;
    }
    const char *datadir = argv[1];
    int32_t height = (int32_t)strtol(argv[2], NULL, 10);
    const char *hexhash = argv[3];
    const char *out_path = argv[4];

    if (strlen(hexhash) != 64) {
        fprintf(stderr, "blockhash must be 64 hex chars (got %zu)\n",
                strlen(hexhash));
        return 2;
    }
    /* Display hex is big-endian; internal hashBlock is little-endian.
     * Reverse byte order so anchor_block_hash matches block_index.hashBlock. */
    uint8_t anchor_hash[32];
    for (int i = 0; i < 32; i++) {
        int hi = hexnib(hexhash[i * 2]);
        int lo = hexnib(hexhash[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            fprintf(stderr, "bad hex at byte %d\n", i);
            return 2;
        }
        anchor_hash[31 - i] = (uint8_t)((hi << 4) | lo);
    }

    if (!progress_store_open(datadir)) {
        fprintf(stderr, "progress_store_open(%s) failed\n", datadir);
        return 1;
    }
    sqlite3 *pdb = progress_store_db();
    if (!pdb) {
        fprintf(stderr, "progress_store_db() returned NULL\n");
        return 1;
    }

    /* Report what coins_kv holds before writing. */
    int64_t num_txs = 0, count = 0, supply = 0;
    if (coins_kv_setinfo(pdb, &num_txs, &count, &supply))
        fprintf(stderr, "coins_kv: count=%lld supply=%lld\n",
                (long long)count, (long long)supply);

    uint8_t got_sha3[32] = {0};
    uint64_t got_count = 0;
    int64_t got_supply = 0;
    if (!coins_kv_snapshot_write(pdb, out_path, height, anchor_hash,
                                 got_sha3, &got_count, &got_supply)) {
        fprintf(stderr, "coins_kv_snapshot_write FAILED\n");
        return 1;
    }

    char sha3hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(sha3hex + i * 2, 3, "%02x", got_sha3[i]);
    fprintf(stderr,
            "WROTE %s height=%d count=%llu supply=%lld sha3=%s\n",
            out_path, height, (unsigned long long)got_count,
            (long long)got_supply, sha3hex);
    return 0;
}
