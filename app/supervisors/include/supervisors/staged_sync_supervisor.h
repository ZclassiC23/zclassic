/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Staged-sync supervisor children — declarative liveness registration.
 *
 * Owns the eight staged-sync reducer children, each a
 * supervised on_tick that drains a bounded batch and publishes its cursor:
 *   staged.header_admit
 *   staged.validate_headers
 *   staged.body_fetch
 *   staged.body_persist
 *   staged.script_validate
 *   staged.proof_validate
 *   staged.utxo_apply
 *   staged.tip_finalize
 * All registered in the `chain` domain (g_chain_sup), in this order. A
 * stage whose _init fails (e.g. progress_store didn't open) is skipped so
 * boot doesn't loop on a perma-IDLE child. */

#ifndef ZCL_STAGED_SYNC_SUPERVISOR_H
#define ZCL_STAGED_SYNC_SUPERVISOR_H

struct main_state;

/* Register all staged-sync supervisor children, in pipeline order.
 * Idempotent per-stage. `ms` is the live chainstate each stage binds to. */
void staged_sync_supervisor_register(struct main_state *ms);

#endif /* ZCL_STAGED_SYNC_SUPERVISOR_H */
