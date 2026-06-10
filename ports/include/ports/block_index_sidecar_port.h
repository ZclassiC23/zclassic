/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * block_index_sidecar_port — storage interface for the SQLite
 * cross-check the block-index sidecar integrity verifier performs.
 *
 * Background
 * ----------
 * bii_verify() proves block_index.bin against a SHA3 sidecar AND
 * cross-checks the loader's declared tip against the SQLite `blocks`
 * table — the belt-and-braces guard added after the 2026-04-10
 * stale-tip incident (a stale h=60 flat-file entry while SQLite held
 * h=3,073,476). That cross-check is the ONLY thing the verifier does
 * that touches sqlite, and it is one fixed read:
 *
 *   "SELECT height FROM blocks WHERE hash=?"  bound to the declared
 *   tip's 32-byte block hash, returning the stored height for that
 *   hash (or "absent").
 *
 * The port captures exactly that read as a single method:
 *
 *   lookup_block_height(self, hash32, out_height)
 *       Look up the height stored for a 32-byte block hash. Returns a
 *       three-way result so the verifier can keep its EXACT verdict
 *       policy without naming sqlite:
 *         BII_HEIGHT_FOUND       — row exists; *out_height is set.
 *         BII_HEIGHT_NOT_FOUND   — no row for that hash.
 *         BII_HEIGHT_UNAVAILABLE — the read could not run (NULL self /
 *                                  NULL connection / the `blocks`
 *                                  schema is not ready yet). Mirrors
 *                                  the inline "defer to CSR" path that
 *                                  treated a failed prepare as a no-op.
 *
 * No sqlite type appears in this header. The adapter under
 * adapters/outbound/persistence/ is the only thing that includes
 * sqlite for this subsystem. It wraps an already-open sqlite3* via
 * `self` and never closes it.
 *
 * Contract:
 *   - Runs the SELECT verbatim (same SQL text, same blob bind, same
 *     readonly single-shot step) the verifier used inline.
 *   - On a found row sets *out_height to the column-0 int64 and
 *     returns BII_HEIGHT_FOUND.
 *   - On no row returns BII_HEIGHT_NOT_FOUND (*out_height untouched).
 *   - On any precondition that the inline code treated as "skip the
 *     cross-check" (NULL self, NULL connection, prepare failure)
 *     returns BII_HEIGHT_UNAVAILABLE (*out_height untouched).
 */

#ifndef ZCL_PORTS_BLOCK_INDEX_SIDECAR_PORT_H
#define ZCL_PORTS_BLOCK_INDEX_SIDECAR_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum bii_height_lookup {
    BII_HEIGHT_FOUND = 0,    /* row found; *out_height set */
    BII_HEIGHT_NOT_FOUND,    /* no row for that hash */
    BII_HEIGHT_UNAVAILABLE,  /* could not run the read — skip cross-check */
};

struct block_index_sidecar_port {
    void *self;

    /* "SELECT height FROM blocks WHERE hash=?" — bind `hash32` (the
     * declared tip's 32-byte block hash), return the stored height in
     * *out_height when found. See enum above for the three-way result. */
    enum bii_height_lookup (*lookup_block_height)(void *self,
                                                  const uint8_t hash32[32],
                                                  int64_t *out_height);
};

#endif /* ZCL_PORTS_BLOCK_INDEX_SIDECAR_PORT_H */
