/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * anchor_selfmint — SELF-MINT the SHA3-verified anchor snapshot during a
 * NORMAL fold, so a verified <datadir>/utxo-anchor.snapshot becomes reachable
 * WITHOUT an operator running the offline `-mint-anchor` ceremony.
 *
 * WHY: the torn-import auto-arm self-heal (boot_anchor_seed_from_snapshot,
 * config/src/boot_refold_staged.c) re-seeds coins_kv from the MINTED snapshot
 * artifact when a durable cold-import tear is detected. On a plain install no
 * such snapshot exists at the path mint_snapshot_path resolves
 * ($ZCL_MINT_ANCHOR_OUT, else <datadir>/utxo-anchor.snapshot), so the self-heal
 * has nothing to re-seed from. The project vision is a ~15 MB self-contained
 * binary, so the 101 MB snapshot CANNOT be baked in-binary; instead the node
 * mints it from its OWN already-validated fold the first time the fold lands the
 * compiled checkpoint height.
 *
 * CONTRACT (OBSERVE-ONLY, BEST-EFFORT — exactly like seal_service):
 *   - This NEVER changes any consensus predicate or the fold result. It only
 *     PERSISTS a snapshot of already-validated state (coins_kv at the moment it
 *     provably holds the applied-through-anchor set).
 *   - void return: a write/verify failure MUST NOT fail the block. It is logged
 *     (LOG_WARN) and dropped, never propagated.
 *   - One-shot at the EXACT checkpoint height: fires only when
 *     next_cursor == get_sha3_utxo_checkpoint()->height (the apply that lands the
 *     anchor; next_cursor == cursor_in + 1). At that instant coins_kv holds the
 *     applied-through-anchor set inside the caller's stage txn.
 *   - Idempotent: if a valid SHA3-verified snapshot already exists at the path,
 *     it is left untouched (no rewrite).
 *   - HARD-VERIFY before trust: the writer streams atomically (tmp + rename);
 *     after the write the body SHA3 + count are checked against the compiled
 *     checkpoint, and a MISMATCH unlinks the file (never leaves an unverified
 *     artifact a later -refold-from-anchor could load).
 *
 * The hook runs INSIDE the utxo_apply step_apply BEGIN IMMEDIATE (beside the
 * seal candidate hook), so the coins it scans are exactly the committed
 * anchor set. The one-time ~1 s 101 MB scan happens once per node lifetime at
 * the single anchor-crossing height (the seal hook already does a ~1 s in-txn
 * coins scan at its grid points), and only when no verified snapshot exists yet.
 */

#ifndef ZCL_SERVICES_ANCHOR_SELFMINT_H
#define ZCL_SERVICES_ANCHOR_SELFMINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct sqlite3;

/* Self-mint hook. Call from the utxo_apply step_apply txn after the cursor +
 * applied-height are stamped (beside seal_candidate_hook_in_tx). `datadir` is
 * the node's data directory (used to resolve <datadir>/utxo-anchor.snapshot when
 * $ZCL_MINT_ANCHOR_OUT is unset); may be NULL/empty (then "." is used).
 * `next_cursor` is the just-applied frontier (cursor_in + 1). No-op unless
 * next_cursor == the compiled SHA3 checkpoint height. Best-effort, void. */
void anchor_selfmint_hook_in_tx(struct sqlite3 *db, const char *datadir,
                                int32_t next_cursor);

/* Pure path resolver, shared with the boot reachability probe and the test:
 * $ZCL_MINT_ANCHOR_OUT, else <datadir>/utxo-anchor.snapshot (datadir NULL/empty
 * -> "."). Returns true and fills buf on success. */
bool anchor_selfmint_resolve_path(const char *datadir, char *buf, size_t cap);

#endif /* ZCL_SERVICES_ANCHOR_SELFMINT_H */
