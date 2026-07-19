/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Lane A1 — the created_outputs retention prune, DECOUPLED from the utxo_apply
 * kernel co-commit. utxo_apply_stage_drain() calls
 * utxo_apply_created_outputs_prune_post_commit() AFTER the kernel batch has
 * committed and the kernel tx lock has been released; the prune runs in its own
 * transaction on the same progress-store handle. See the .c for the crash
 * argument. The two test-observability entry points
 * (utxo_apply_post_prune_stats, utxo_apply_created_outputs_retain_set_for_test)
 * are declared in jobs/utxo_apply_stage.h. */

#ifndef ZCL_JOBS_UTXO_APPLY_CREATED_OUTPUTS_PRUNE_H
#define ZCL_JOBS_UTXO_APPLY_CREATED_OUTPUTS_PRUNE_H

struct sqlite3;

/* Best-effort post-commit retention prune of the created_outputs projection.
 * `db` is the progress-store handle (may be NULL — no-op). Re-acquires
 * progress_store_tx_lock() for a strictly-sequential (never nested) tx. */
void utxo_apply_created_outputs_prune_post_commit(struct sqlite3 *db);

#endif /* ZCL_JOBS_UTXO_APPLY_CREATED_OUTPUTS_PRUNE_H */
