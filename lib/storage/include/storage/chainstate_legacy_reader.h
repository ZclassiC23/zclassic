/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * chainstate_legacy_reader: streaming reader for a Bitcoin Core /
 * zclassicd "chainstate" LevelDB.  Walks the entire 'c'-prefixed
 * keyspace (txid -> CCoins record), decodes each record per the
 * Bitcoin Core 0.8+ on-disk format, and emits a struct legacy_coins
 * via callback.
 *
 * Foundation for Option B in plans/make-a-full-detailed-nifty-melody.md:
 * UTXO-snapshot import from a co-located zclassicd.
 *
 * Reentrant-safe: each open handle owns its own LevelDB wrapper +
 * snapshot.  The callback runs on the caller's thread.
 */

#ifndef ZCL_STORAGE_CHAINSTATE_LEGACY_READER_H
#define ZCL_STORAGE_CHAINSTATE_LEGACY_READER_H

#include "core/uint256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct legacy_coins_vout {
    unsigned int n;        /* original vout index (some are skipped, "spent") */
    int64_t      value;    /* satoshis */
    const uint8_t *script; /* decompressed scriptPubKey bytes */
    size_t       script_len;
};

struct legacy_coins {
    int      height;       /* block height that included the funding tx */
    bool     coinbase;
    int      version;
    /* One entry per non-spent output, in ascending vout-index order.
     * Spent outputs are absent. */
    struct legacy_coins_vout *vouts;
    size_t   num_vouts;
};

typedef bool (*legacy_chainstate_cb)(const struct uint256 *txid,
                                     const struct legacy_coins *coins,
                                     void *ctx);

/* Open the chainstate LevelDB at `chainstate_path`.  On success
 * stores an opaque handle in *out_handle and returns true.  The
 * directory must not be locked by another process (copy it first if
 * zclassicd is running). */
bool chainstate_legacy_open(const char *chainstate_path, void **out_handle);

/* Release every resource owned by the handle. */
void chainstate_legacy_close(void *handle);

/* Walk every 'c'-prefixed record.  Invokes `cb` exactly once per
 * record (in LevelDB key order).  If the callback returns false,
 * iteration stops early and the count emitted up to that point is
 * returned.  Returns -1 on a fatal decode error.  The legacy_coins
 * pointer and all of its nested buffers (vouts, script) are owned by
 * the reader and remain valid only for the duration of the callback. */
int64_t chainstate_legacy_iter(void *handle,
                               legacy_chainstate_cb cb,
                               void *ctx);

/* Read the best-block hash stored under the single-byte key 'B'. */
bool chainstate_legacy_get_best_block(void *handle, struct uint256 *out);

/* ── Historical Sapling anchor lookup ─────────────────────────────────
 *
 * A zcashd-lineage chainstate persists every historical Sapling
 * note-commitment tree along the active chain, keyed by its root:
 *   key:   'Z' || <32-byte anchor root, raw byte order>
 *   value: serialized SaplingMerkleTree (the IncrementalMerkleTree
 *          boost::optional wire format: optional<left>, optional<right>,
 *          vector<optional<parent>>)
 * The key-schema knowledge (DB_SAPLING_ANCHOR = 'Z') is ported from the
 * Zcash reference — see the ATTRIBUTION block in the .c file. */
struct incremental_merkle_tree;

enum chainstate_anchor_result {
    CHAINSTATE_ANCHOR_ERROR   = -1, /* read / deserialize / verify failure */
    CHAINSTATE_ANCHOR_MISSING =  0, /* no row for this root */
    CHAINSTATE_ANCHOR_FOUND   =  1, /* tree_out valid, root re-verified */
};

/* Look up the Sapling anchor tree stored under ('Z' || root) and
 * deserialize it into *tree_out (which the caller need not pre-init; this
 * calls sapling_tree_init).  FAIL-CLOSED: recomputes the deserialized
 * tree's own root and returns CHAINSTATE_ANCHOR_ERROR (writing nothing
 * usable) on ANY mismatch against the lookup `root` — a borrowed tree is
 * never returned unverified.  Returns CHAINSTATE_ANCHOR_MISSING when the
 * key is absent. */
enum chainstate_anchor_result chainstate_legacy_get_sapling_anchor(
    void *handle, const struct uint256 *root,
    struct incremental_merkle_tree *tree_out);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_STORAGE_CHAINSTATE_LEGACY_READER_H */
