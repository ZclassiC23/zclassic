/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONDITIONS_BODY_FETCH_MISSING_HAVE_DATA_H
#define ZCL_CONDITIONS_BODY_FETCH_MISSING_HAVE_DATA_H

/* SYMPTOM: tip has not advanced for >= 60s and the active-frontier candidate
 *   from stage_repair has no observed body and its block_index lacks
 *   BLOCK_HAVE_DATA (the next body to extend the chain is missing).
 * REMEDY: sync_monitor_queue_active_frontier_body(target) — queue the body
 *   fetch; a failed queue returns COND_REMEDY_FAILED.
 * WITNESSED: the body is now observed (stage_repair_body_fetch_observed), OR
 *   the target's block data is readable on disk (target_has_readable_data).
 * COND_CRITICAL; poll_secs=5 (backoff 30s, max_attempts 5). */
void register_body_fetch_missing_have_data(void);

#ifdef ZCL_TESTING
void body_fetch_missing_have_data_test_reset(void);
int body_fetch_missing_have_data_test_remedy_calls(void);
#endif

#endif /* ZCL_CONDITIONS_BODY_FETCH_MISSING_HAVE_DATA_H */
