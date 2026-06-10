/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONDITION_REGISTRY_H
#define ZCL_CONDITION_REGISTRY_H

/* Not a Condition itself (no detect/remedy/witness or severity/poll cadence) —
 * this is the aggregator. condition_registry_register_all() calls each
 * register_X() in turn (block_failed_mask_at_tip, contradiction_frozen,
 * chain_integrity_failed, utxo_activation_paused, utxo_drift_detected,
 * header_stall_at_height, sync_state_stuck, download_queue_starved,
 * local_header_refill_needed, peer_floor_violated, sync_violation_lag,
 * tip_wedged_resnapshot, snapshot_receive_stalled, legacy_mirror_stuck,
 * snapshot_offer_ready, snapshot_negotiation_stalled, snapshot_failed_reset,
 * snapshot_complete_resume, body_fetch_missing_have_data, have_data_unreadable,
 * orphan_utxo_above_tip, tip_fork_stale, tip_stall_oracle_rebuild,
 * stale_validate_headers_repair), registering every Condition with the engine. */
void condition_registry_register_all(void);

#endif /* ZCL_CONDITION_REGISTRY_H */
