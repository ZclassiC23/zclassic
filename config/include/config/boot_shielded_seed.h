/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_shielded_seed — install the v3-snapshot SHIELDED consensus state (the
 * birth-defect cure) when seeding via -load-snapshot-at-own-height.
 *
 * The v3 UTXO snapshot carries the Sapling + Sprout commitment-tree frontiers
 * and the complete nullifier set (storage/snapshot_shielded.h). These helpers
 * capture those regions from the SHA3-verified snapshot and install them into
 * anchor_kv + nullifier_kv, so a fresh seeded node can resolve the first
 * post-seed shielded transaction's anchor WITHOUT borrowing a zclassicd
 * chainstate. Split out of boot_refold_staged.c to keep that mega-module under
 * the file-size ceiling.
 */

#ifndef CONFIG_BOOT_SHIELDED_SEED_H
#define CONFIG_BOOT_SHIELDED_SEED_H

#include <stdbool.h>
#include <stdint.h>

struct sqlite3;
struct main_state;
struct uss_handle;

/* Capture the shielded section from an OPEN, SHA3-verified snapshot handle into
 * heap copies (the caller frees). v2 copies ONLY the Sapling frontier (leaving
 * *shielded_v3 false, so the v3 cure does not engage). v3 copies the Sapling +
 * Sprout frontiers + the nullifier set and, on a full capture, sets
 * *shielded_v3 = true. Sapling is only copied when *sapling is still NULL. On
 * OOM the partial v3 copies are freed and *shielded_v3 stays false (the caller
 * falls back to the cursor-reset / block-replay path). No-op for v1. */
void boot_capture_shielded(struct uss_handle *h, bool load_ok,
                           uint8_t **sapling, uint32_t *sapling_len,
                           bool *shielded_v3,
                           uint8_t **sprout, uint32_t *sprout_len,
                           uint8_t **nfs, uint64_t *nf_count);

/* In the caller's ALREADY-OPEN progress.kv transaction, either:
 *   - (cure) when a v3 shielded section was captured AND its Sapling frontier
 *     ALREADY root-verified against hashFinalSaplingRoot (sapling_verified):
 *     reset the anchor adoption cursor to 0 (history complete), install the
 *     root-verified Sapling frontier row (fail-closed on any mismatch), add the
 *     Sprout frontier, and bulk-add the nullifier set; or
 *   - (default) reset the anchor adoption cursor to seed_h over EMPTY tables
 *     (today's behavior — HISTORY_INCOMPLETE until a body replay backfills).
 * Returns true on success, false (caller rolls back + fails closed) on any
 * error — INCLUDING a Sapling root-verify failure: an unverified/partial
 * shielded state is never seeded. */
bool boot_shielded_cure_or_reset_in_tx(
    struct sqlite3 *rpdb, struct main_state *ms, int seed_h,
    bool sapling_verified, bool shielded_v3,
    const uint8_t *sapling, uint32_t sapling_len,
    const uint8_t *sprout, uint32_t sprout_len,
    const uint8_t *nfs, uint64_t nf_count);

#endif /* CONFIG_BOOT_SHIELDED_SEED_H */
