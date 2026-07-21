/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * catchup_cadence — the live-sync catch-up drain-batch override.
 *
 * The supervisor drives each of the eight reducer stages once per tick
 * (period_secs=2, staged_sync_supervisor.c) with a per-stage batch cap. The
 * four tail stages (script_validate, proof_validate, utxo_apply,
 * tip_finalize — see their *_BATCH_PER_TICK macros) are capped at 100, so an
 * ordinary node that fell behind (bodies already on disk, nothing to fetch
 * over P2P, purely reducer-bound) is hard-floored at 100/2s = ~50 blk/s no
 * matter how fast the disk/CPU actually are. This override lifts that floor
 * to 500/2s = ~250 blk/s ONLY while the node is genuinely far behind the
 * network tip, without touching the shared 2s tick period (the supervisor is
 * single-threaded and shared across many domains — shortening the period
 * would starve everything else, unlike refold_cadence's period override,
 * which only ever applies during an OFFLINE from-genesis/from-anchor fold
 * with no live peers/other domains to starve).
 *
 * GAP MEASUREMENT — reuses, with ZERO new lock surface, the exact primitives
 * app/conditions/src/sync_rate_below_floor.c already uses for the same
 * gap computation:
 *   - network tip:  connman_max_peer_height(sync_monitor_connman())
 *   - log head:     tip_finalize_stage_cursor() (lock-free atomic read —
 *                    see "Threading" in lib/util/include/util/stage.h)
 * Neither touches progress_store, coins_kv, or any lock the reducer drive
 * holds (LOCK-ORDER LAW, feedback_reducer_drive_lock_order_law.md) — this
 * module reads the same two already-safe accessors and adds no new ones.
 *
 * SAFETY — the override is GATED on catchup_cadence_active(): true ONLY
 * when peers are connected AND the gap (network_tip - log_head) is at least
 * ZCL_CATCHUP_GAP_THRESHOLD (default 500). On a NORMAL live node at/near tip,
 * or with no peers, this is false, so catchup_cadence_drain_batch() returns
 * its argument UNCHANGED and the live hot path is BYTE-FOR-BYTE unchanged.
 * This is the load-bearing safety property, pinned by test_catchup_cadence:
 * normal mode MUST stay batch=<stage default>.
 *
 * NO TICK-PERIOD OVERRIDE is exposed here on purpose — see the module
 * comment above. Widening the per-drain batch only widens the commit window
 * of the existing per-batch SAVEPOINT/BEGIN IMMEDIATE (one COMMIT per 500
 * instead of per 100); correctness is unchanged and a crash resumes
 * idempotently from the last durable cursor, same as always. The bounded
 * risk is one run_due_ticks() pass taking longer wall-clock while active:
 * Σ(500 * per-block cost) over the 4 tail stages, with utxo_apply the
 * slowest at ~0.2-1.4ms/block, so ~500 * ~1ms ~= 0.5s per stage — well
 * under the multi-minute quiet windows other supervisor domains tolerate.
 *
 * TUNABLE (only while active):
 *   ZCL_CATCHUP_GAP_THRESHOLD  blocks   default  500  clamp [1,100000000]
 *   ZCL_CATCHUP_DRAIN_BATCH    blocks   default  500  clamp [1,1000000]
 */

#ifndef ZCL_JOBS_CATCHUP_CADENCE_H
#define ZCL_JOBS_CATCHUP_CADENCE_H

#include <stdbool.h>
#include <stdint.h>

/* Accelerated-cadence defaults (used when active + the env var is unset).
 * 500 matches the already-shipped BODY_FETCH_BATCH_PER_TICK /
 * BODY_PERSIST_BATCH_PER_TICK / HEADER_ADMIT_BATCH_PER_TICK — not a new
 * magic number. */
#define CATCHUP_CADENCE_DEFAULT_DRAIN_BATCH 500
#define CATCHUP_CADENCE_DEFAULT_GAP_THRESHOLD 500

/* True iff the node has connected peers AND is at least
 * ZCL_CATCHUP_GAP_THRESHOLD blocks behind the max peer-reported height.
 * Cheap: connman_max_peer_height() + one lock-free atomic cursor read, the
 * same two calls sync_rate_below_floor.c already makes every poll. False on
 * a normal at-tip live node or a node with no peers -> every accessor below
 * is inert. */
bool catchup_cadence_active(void);

/* Per-stage drain batch. Returns `normal_batch` UNCHANGED when inactive;
 * when active, returns ZCL_CATCHUP_DRAIN_BATCH (default 500, clamped). */
int catchup_cadence_drain_batch(int normal_batch);

#ifdef ZCL_TESTING
/* Force the log_head (tip_finalize) cursor reader to return `v` instead of
 * calling the real tip_finalize_stage_cursor() — same override shape as
 * sync_rate_below_floor_test_set_log_head_override(), so a unit test does
 * not need a real progress_store-backed tip_finalize_stage_init(). -1 = use
 * the real reader. Peers/network-tip are driven with a real (test-populated)
 * struct connman via sync_monitor_set_context(), same as
 * test_sync_rate_below_floor.c. */
void catchup_cadence_test_set_log_head_override(int64_t v);

/* Reset all test-only state (the log_head override) back to "use the real
 * reader". */
void catchup_cadence_test_reset(void);
#endif

#endif /* ZCL_JOBS_CATCHUP_CADENCE_H */
