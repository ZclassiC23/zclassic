/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * block_parse_cache — a tiny single-mutex LRU of recently-read+deserialized
 * block bodies, keyed by (height, block_hash).
 *
 * WHY: the five staged-sync stages (body_persist, script_validate,
 * proof_validate, utxo_apply, tip_finalize_post_step) each independently
 * pread + deserialize the SAME on-disk block body for the height they are
 * processing — five full disk reads + five full deserializations per block.
 * Deserialization (compact-size parse of every tx, every input/output script,
 * every JoinSplit, every Sapling spend/output bundle) dominates the fold's
 * CPU outside crypto. body_persist is the producer (it advances first), so by
 * the time the four downstream stages reach the same height the body is in
 * cache and they get a DEEP COPY instead of re-reading+re-parsing.
 *
 * KEY = (height, block_hash). The hash component is load-bearing: an in-memory
 * header relabel / reorg can put a DIFFERENT block at the same height, and a
 * height-only key would then serve a stale body. A (height,hash) miss falls
 * back to read_block_from_disk_pread off the supplied block_index.
 *
 * RESULT-PRESERVING: every consumer receives a COMPLETE, independent clone
 * (a block_serialize -> block_deserialize round-trip, byte-identical to what
 * read_block_from_disk_pread would have produced from the same on-disk bytes)
 * and owns it — it must block_free its copy exactly as before. The cache owns
 * its own retained copy; nothing aliases across consumers. Thread-safe: all
 * state is under one mutex; the clone returned to the caller is built under the
 * lock and then handed out, so it can outlive any later eviction.
 */
#ifndef STORAGE_BLOCK_PARSE_CACHE_H
#define STORAGE_BLOCK_PARSE_CACHE_H

#include <stdbool.h>
#include <stdint.h>

struct block;
struct block_index;

/* Fetch the block body for (height, block_hash) into `out` (which the caller
 * MUST have block_init'd) as an independent deep copy the caller owns and frees
 * with block_free. On a cache miss the body is read from disk via
 * read_block_from_disk_pread off `bi` (HAVE_DATA guarded inside the reader),
 * the parsed body is retained in the cache, and a deep copy is returned. The
 * `bi`/`datadir` arguments are exactly the ones the default reader uses; `bi`
 * supplies both the disk position (on miss) and is the source of `block_hash`
 * the caller passes for the key.
 *
 * Returns true on success (out is a complete clone). Returns false on a read or
 * clone failure, leaving `out` safe to block_free (it may have been partially
 * built and is left in a free-able state). Matches the success/failure contract
 * of stage_default_block_reader so call sites swap with no logic change. */
bool block_parse_cache_get(int32_t height, const uint8_t block_hash[32],
                           const struct block_index *bi, const char *datadir,
                           struct block *out);

/* Drop all cached entries (e.g. on shutdown or a full reindex). Safe to call
 * with an empty cache. */
void block_parse_cache_clear(void);

#endif /* STORAGE_BLOCK_PARSE_CACHE_H */
