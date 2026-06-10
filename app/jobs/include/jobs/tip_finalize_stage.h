/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * tip_finalize_stage — reducer Job stage.
 *
 * Consumes `utxo_apply_log`; for each height where UTXO apply passed,
 * observes that the live chain has advanced to the next active tip and
 * records the finalize result while publishing the reducer-owned active tip. */

#ifndef ZCL_SERVICES_TIP_FINALIZE_STAGE_H
#define ZCL_SERVICES_TIP_FINALIZE_STAGE_H

#include "util/stage.h"

#include <stdbool.h>
#include <stdint.h>

struct json_value;
struct main_state;

#define TIP_FINALIZE_BATCH_PER_TICK 100

typedef bool (*tip_finalize_utxo_count_fn)(int height_after,
                                           int64_t *out_count,
                                           void *user);

bool tip_finalize_stage_init(struct main_state *ms);
void tip_finalize_stage_shutdown(void);

/* Force the authoritative tip state (height + hash). Used during
 * trusted bypasses (bootstrap/sync). */
void tip_finalize_stage_set_authoritative_tip(int height,
                                              const uint8_t hash[32]);

/* Read the durably-recorded finalized tip hash at `height` from the
 * tip_finalize_log table on `db` (the progress.kv handle). Returns true
 * and fills `out_hash[32]` iff a finalized (ok=1) row with a 32-byte
 * tip_hash exists at that height. Used by boot_rebuild_from_log to seed
 * the active tip from the durable cursor without re-running the stage.
 * `db` must be the progress store handle (progress_store_db()). */
struct sqlite3;
bool tip_finalize_stage_finalized_tip_at(struct sqlite3 *db, int height,
                                         uint8_t out_hash[32]);

/* Cold-start fast_sync seed: durably record the snapshot anchor at
 * `height` (hash) as a finalized tip in tip_finalize_log and advance the
 * stage cursor to height+1, so the next boot_rebuild_from_log restores
 * the tip purely from the log/cursor. Also seeds the runtime
 * authoritative tip. Best-effort, non-fatal: returns false (no mutation
 * beyond the log row) if the progress store / stage are not yet wired.
 * `hash` is the 32-byte snapshot anchor block hash.
 *
 * `trusted_seed` is the FIX-3 cap exemption (stage_anchor.h): true ONLY
 * for an externally-verified trusted base (the SHA3-verified snapshot
 * accept). Runtime re-seeds — the per-ingest at-tip re-anchor, regtest
 * stamps — MUST pass false: a fresh datadir is still exempt via the
 * pre-insert-empty-log prong, while a stamp across a rowless span of
 * tip_finalize_log (or any upstream stage's log) is capped at the first
 * hole so the reducer re-finalizes forward instead of manufacturing the
 * log-hole wedge. */
bool tip_finalize_stage_seed_anchor(int height, const uint8_t hash[32],
                                    bool trusted_seed);

job_result_t tip_finalize_stage_step_once(void);
int tip_finalize_stage_drain(int max_steps);

uint64_t tip_finalize_stage_cursor(void);
int64_t  tip_finalize_stage_last_height(void);
uint64_t tip_finalize_stage_finalized_total(void);
uint64_t tip_finalize_stage_upstream_failed_total(void);
uint64_t tip_finalize_stage_reorg_detected_total(void);
uint64_t tip_finalize_stage_utxo_count_diverged_total(void);
uint64_t tip_finalize_stage_precondition_failed_total(void);
/* Count of LOOKAHEAD-not-ready deferrals (H held via JOB_IDLE until its
 * successor H+1 lands) — distinct from precondition_failed_total which now
 * counts only the genuine competing-fork (chainwork_not_greater) skip. */
uint64_t tip_finalize_stage_successor_pending_total(void);
uint64_t tip_finalize_stage_total_work_added_high(void);
uint64_t tip_finalize_stage_total_work_added_low(void);

void tip_finalize_stage_set_utxo_counter(tip_finalize_utxo_count_fn fn,
                                         void *user);

bool tip_finalize_dump_state_json(struct json_value *out, const char *key);

#endif /* ZCL_SERVICES_TIP_FINALIZE_STAGE_H */
