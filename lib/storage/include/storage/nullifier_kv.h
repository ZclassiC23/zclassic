/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * nullifier_kv — the reducer's consensus shielded-nullifier set as a
 * `nullifiers` table IN progress.kv (C-3).
 *
 * WHY: zclassicd's ConnectBlock rejects any transaction whose Sprout or
 * Sapling nullifier was already revealed (HaveShieldedRequirements,
 * main.cpp:2627 -> "bad-txns-joinsplit-requirements-not-met"; nullifier
 * lookup in coins.cpp:166-180). The reducer connect path (utxo_apply) had no
 * durable nullifier set, so a shielded double-spend that zclassicd rejects
 * would have been accepted here — an opposite-verdict hard fork. This table
 * is the durable set; utxo_apply checks-then-inserts inside its own stage
 * transaction (stage_run_once's BEGIN IMMEDIATE), so nullifiers commit or
 * roll back atomically with coins + cursor + log row, exactly like coins_kv.
 *
 * POOLS: `pool` 0 = Sprout, 1 = Sapling — SEPARATE namespaces. zclassicd
 * keeps DISTINCT nullifier maps per pool (CCoinsViewCache::GetNullifier,
 * coins.cpp:166-180), so the same 32 bytes may legally appear once in EACH
 * pool. A single-column nf primary key would reject that legal cross-pool
 * byte-reuse — the opposite-direction fork risk. Hence PRIMARY KEY(nf,pool).
 *
 * REWIND INVARIANT (load-bearing): every cursor rewind must delete nullifier
 * rows in the rewound range in the same txn — a rewind path that skips this
 * produces stale rows and false shielded_double_spend rejects on re-apply.
 * utxo_apply_delete_rows_above (the shared rewind primitive) deletes
 * nullifiers alongside utxo_apply_log/utxo_apply_delta, which covers the
 * reorg unwind, the value_overflow repair, and the stale-script replay;
 * stage_repair_rewind's downstream delete list includes it belt-and-braces.
 *
 * PERMANENCE: like coins, the nullifier set is permanent consensus state —
 * it is NEVER pruned below finality (unlike utxo_apply_delta rows).
 *
 * ACTIVATION GAP (open, owner-gated follow-up): the set is populated only
 * by blocks applied AFTER the table is created. On a datadir whose
 * utxo_apply cursor was already past genesis at activation (the live node,
 * every FlyClient/SHA3-snapshot cold sync — the snapshot format carries
 * coins but no nullifier set), every nullifier revealed below the
 * `nullifier_kv.activation_cursor` marker is ABSENT, so a double-spend of
 * a pre-activation note is accepted here and rejected by zclassicd — C-3
 * is closed only for from-genesis replays and post-activation notes.
 * utxo_apply_stage_init surfaces this as the PERMANENT blocker
 * `utxo_apply.nullifier_backfill_gap` (zcl_state subsystem=blocker) every
 * boot until a shielded-history backfill walker (or a snapshot-format
 * nullifier extension) lands. Fail-open direction only: no wedge risk.
 *
 * Every function operates on the passed progress.kv handle and therefore
 * participates in whatever transaction the caller already holds open. Raw
 * sqlite3_step calls carry // raw-sql-ok:progress-kv-kernel-store, the
 * sanctioned hatch for the kernel store (same convention as coins_kv.c). */
#ifndef STORAGE_NULLIFIER_KV_H
#define STORAGE_NULLIFIER_KV_H

#include <stdbool.h>
#include <stdint.h>

struct sqlite3;

/* `pool` column values. Must never be renumbered: rows are durable. */
#define NULLIFIER_POOL_SPROUT  0
#define NULLIFIER_POOL_SAPLING 1

/* CREATE TABLE IF NOT EXISTS nullifiers(...) + height index. Idempotent. */
bool nullifier_kv_ensure_schema(struct sqlite3 *db);

/* True iff the `nullifiers` table already exists. Used by utxo_apply_stage
 * init to detect FIRST creation so it can record the diagnostics-only
 * activation-cursor honesty marker (heights below it were never checked). */
bool nullifier_kv_table_exists(struct sqlite3 *db);

/* Point-read one nullifier in one pool. Returns false only on a store error;
 * *found reports presence, *height_out (optional) the revealing height. */
bool nullifier_kv_get(struct sqlite3 *db, const uint8_t nf[32], int pool,
                      bool *found, int64_t *height_out);

/* INSERT OR REPLACE one revealed nullifier at `height`. */
bool nullifier_kv_add(struct sqlite3 *db, const uint8_t nf[32], int pool,
                      int64_t height);

/* DELETE every nullifier revealed in [first_h .. last_h] (both pools) —
 * the rewind primitive (see the rewind invariant above). */
bool nullifier_kv_delete_range(struct sqlite3 *db, int64_t first_h,
                               int64_t last_h);

#endif /* STORAGE_NULLIFIER_KV_H */
