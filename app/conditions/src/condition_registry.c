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
void register_tip_label_divergence(void);
void register_state_window_inconsistent(void);
void register_mirror_divergence_located(void);
void register_replay_canary_failed(void);

void register_disk_full_pause(void);
void register_clock_skew_reconcile(void);
void register_sapling_anchor_frontier_unavailable(void);
void register_blocker_stall_meta_detector(void);
void register_reducer_drive_watchdog(void);
void register_batch_fsync_slow(void);

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
    register_tip_label_divergence();
    register_state_window_inconsistent();
    register_mirror_divergence_located();
    register_replay_canary_failed();
    register_disk_full_pause();
    register_clock_skew_reconcile();
    register_sapling_anchor_frontier_unavailable();
    register_blocker_stall_meta_detector();
    register_reducer_drive_watchdog();
    register_batch_fsync_slow();
}
