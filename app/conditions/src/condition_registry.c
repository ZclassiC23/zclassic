/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "conditions/condition_registry.h"

void register_block_failed_mask_at_tip(void);
void register_contradiction_frozen(void);
void register_chain_integrity_failed(void);
void register_utxo_activation_paused(void);
void register_utxo_drift_detected(void);
void register_header_stall_at_height(void);
void register_sync_state_stuck(void);
void register_download_queue_starved(void);
void register_local_header_refill_needed(void);
void register_peer_floor_violated(void);
void register_sync_violation_lag(void);
void register_tip_wedged_resnapshot(void);
void register_snapshot_receive_stalled(void);
void register_legacy_mirror_stuck(void);
void register_snapshot_offer_ready(void);
void register_snapshot_negotiation_stalled(void);
void register_snapshot_failed_reset(void);
void register_snapshot_complete_resume(void);
void register_body_fetch_missing_have_data(void);
void register_have_data_unreadable(void);
void register_orphan_utxo_above_tip(void);
void register_tip_fork_stale(void);
void register_tip_stall_oracle_rebuild(void);
void register_stale_validate_headers_repair(void);
void register_reducer_frontier_reconcile_light(void);

void condition_registry_register_all(void)
{
    register_block_failed_mask_at_tip();
    register_contradiction_frozen();
    register_chain_integrity_failed();
    register_utxo_activation_paused();
    register_utxo_drift_detected();
    register_header_stall_at_height();
    register_sync_state_stuck();
    register_download_queue_starved();
    register_local_header_refill_needed();
    register_peer_floor_violated();
    register_sync_violation_lag();
    register_tip_wedged_resnapshot();
    register_snapshot_receive_stalled();
    register_legacy_mirror_stuck();
    register_snapshot_offer_ready();
    register_snapshot_negotiation_stalled();
    register_snapshot_failed_reset();
    register_snapshot_complete_resume();
    register_body_fetch_missing_have_data();
    register_have_data_unreadable();
    register_orphan_utxo_above_tip();
    register_tip_fork_stale();
    register_tip_stall_oracle_rebuild();
    register_stale_validate_headers_repair();
    register_reducer_frontier_reconcile_light();
}
