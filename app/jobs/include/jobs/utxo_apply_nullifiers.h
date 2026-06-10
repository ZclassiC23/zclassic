/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_apply_nullifiers — the reducer's port of zclassicd's shielded
 * double-spend gate (C-3), split out of utxo_apply_stage.c along the same
 * seam as utxo_apply_delta*.c (lint gate E1 file-size ceiling).
 *
 * Not a public API — only utxo_apply_stage.c includes this (same contract
 * as jobs/utxo_apply_delta.h). The durable set itself is the `nullifiers`
 * table in progress.kv (storage/nullifier_kv.h); this module owns the
 * per-block two-pass check-then-insert and the activation-gap blocker. */

#ifndef ZCL_JOBS_UTXO_APPLY_NULLIFIERS_H
#define ZCL_JOBS_UTXO_APPLY_NULLIFIERS_H

#include <stdbool.h>

struct sqlite3;
struct block;
struct delta_summary;

/* The PERMANENT blocker naming the C-3 activation gap (see the refresh
 * contract below). Shared with utxo_apply_stage_shutdown's clear. */
#define UTXO_APPLY_NF_GAP_BLOCKER_ID "utxo_apply.nullifier_backfill_gap"

/* Check every Sprout/Sapling nullifier of `blk` against the durable set +
 * the earlier-tx accumulator (zclassicd's per-tx check-then-set order,
 * main.cpp:2627 HaveShieldedRequirements), then insert all of them at
 * `height` iff the whole block is clean (two-pass — a rejected block never
 * leaves partial rows). Returns false on a STORE error (caller goes
 * JOB_FATAL — fail closed); a consensus hit flips summary->ok with status
 * "shielded_double_spend" and returns true so the regular failure path
 * (counter, log row, JOB_BLOCKED) runs. MUST run inside the stage txn so
 * pass-2 inserts commit atomically with coins + cursor + log row. */
bool utxo_apply_check_and_insert_nullifiers(struct sqlite3 *db,
                                            const struct block *blk,
                                            int height,
                                            struct delta_summary *summary);

/* C-3 ACTIVATION GAP, owner-visible (see storage/nullifier_kv.h): reads
 * the `nullifier_kv.activation_cursor` marker; if > 0 the durable set is
 * MISSING every nullifier revealed at/below that height (no backfill
 * exists yet — a pre-activation shielded double-spend is accepted here,
 * rejected by zclassicd), so this registers/refreshes the PERMANENT
 * blocker UTXO_APPLY_NF_GAP_BLOCKER_ID; a 0/absent marker clears it
 * (from-genesis replays are complete). Called from utxo_apply_stage_init
 * every boot. Best-effort: store errors are logged, never fatal. */
void utxo_apply_nullifier_gap_blocker_refresh(struct sqlite3 *db);

#endif /* ZCL_JOBS_UTXO_APPLY_NULLIFIERS_H */
