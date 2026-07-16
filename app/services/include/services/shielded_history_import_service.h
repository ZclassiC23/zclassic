/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * shielded_history_import_service — COMPLETE, ATOMIC historical anchor +
 * nullifier import from an operator's zclassicd chainstate LevelDB.
 *
 * WHY: a snapshot/seed datadir holds only the CURRENT shielded frontier, so
 * every historical Sprout/Sapling anchor root and every historical nullifier
 * below the reducer cursor is absent. anchor_kv/nullifier_kv fail those lookups
 * CLOSED (HISTORY_INCOMPLETE) while their activation cursors are positive, which
 * pins H* at the wedge height (utxo_apply.anchor_backfill_gap +
 * utxo_apply.nullifier_backfill_gap). zclassicd's tip chainstate DOES persist
 * the complete active-chain set (additive-only, never pruned forward). This
 * service imports that complete set — both pools, full history — and flips BOTH
 * activation cursors to 0, in ONE transaction.
 *
 * CONSENSUS-CRITICAL / ALL-OR-NOTHING: flipping a cursor to 0 converts a SAFE
 * halt (missing root = HISTORY_INCOMPLETE, block held) into an ACCEPT (missing
 * root = MISSING, block false-rejected → re-wedge / fork). Therefore the import
 * is atomic: ANY read/deserialize/verify/write anomaly rolls the WHOLE
 * transaction back and leaves both cursors POSITIVE (the safe wedge intact).
 * The cursors flip only after the complete set committed and the tip Sapling
 * frontier root was bound to the block header's hashFinalSaplingRoot.
 *
 * This is an owner-gated, copy-prove-gated path (never auto-run on a live
 * datadir) — see src/main.c -import-complete-shielded and §6 of
 * docs/work/fast-sync-to-tip-plan-2026-07-16.md.
 */
#ifndef ZCL_SERVICES_SHIELDED_HISTORY_IMPORT_SERVICE_H
#define ZCL_SERVICES_SHIELDED_HISTORY_IMPORT_SERVICE_H

#include "core/uint256.h"

#include <stdbool.h>
#include <stdint.h>

struct sqlite3;

/* Per-run outcome. Populated on both success and refusal (partial counts on a
 * refusal reflect what was iterated before the anomaly; nothing was committed
 * unless `committed` is true). */
struct shielded_import_report {
    int64_t sapling_anchors;     /* verified Sapling anchor rows imported */
    int64_t sprout_anchors;      /* verified Sprout anchor rows imported */
    int64_t sapling_nullifiers;  /* Sapling nullifiers imported */
    int64_t sprout_nullifiers;   /* Sprout nullifiers imported */
    int64_t anchor_boundary;     /* positive anchor cursor value before the flip */
    int64_t nullifier_boundary;  /* positive nullifier cursor value before the flip */
    bool tip_anchor_bound;       /* best Sapling root == expected tip header root */
    bool committed;              /* true iff the atomic transaction COMMITted */
};

/* Import the COMPLETE historical Sprout+Sapling anchor and nullifier sets from
 * a point-in-time copy / fixture of a zclassicd chainstate LevelDB at
 * `chainstate_src_path` into anchor_kv + nullifier_kv held in `progress_db`,
 * atomically, then flip both activation cursors to 0.
 *
 * `expected_tip_sapling_root` is the block header hashFinalSaplingRoot at the
 * chainstate tip height `expected_tip_height` (the chain-committed bind, §3):
 * the chainstate's best Sapling anchor ('z') MUST be present, MUST be among the
 * imported anchors, and MUST equal this root, or the import refuses.
 *
 * All-or-nothing: on ANY anomaly (torn record, deserialize/root mismatch,
 * tip-frontier bind failure, empty required pool, non-positive/unequal cursors,
 * or any write error) it ROLLS BACK, writes NOTHING, leaves both cursors
 * POSITIVE (safe wedge) and returns false. Returns true only when the complete
 * set committed and both cursors are durably zero. `out` may be NULL. */
bool shielded_history_import_from_chainstate(
    struct sqlite3 *progress_db,
    const char *chainstate_src_path,
    int64_t expected_tip_height,
    const struct uint256 *expected_tip_sapling_root,
    struct shielded_import_report *out);

#endif /* ZCL_SERVICES_SHIELDED_HISTORY_IMPORT_SERVICE_H */
