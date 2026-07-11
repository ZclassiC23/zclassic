# From-genesis refold: fold-rate bottlenecks + fix plan

> **STATUS (2026-07-11): #1 + #3 LANDED — verified in code this session.**
> - **#1 (dominant pprev-walk window) is LANDED.** `tip_finalize` no longer
>   retracts the active-chain coverage window during a refold — the
>   `active_chain_move_window_tip` call is gated on `!refold_in_progress()` at
>   `app/jobs/src/tip_finalize_stage.c:586`. The first extend fills the window
>   once; later extends are O(1) no-ops. Do NOT re-implement this.
> - **#3 (utxo_mirror full wipe+reinsert) is LANDED.** `utxo_mirror_sync_run_once`
>   early-returns `if (refold_in_progress())` at
>   `app/services/src/utxo_mirror_sync_service.c:446` (rebuild once post-refold).
> - Earlier parse-dedup + `coins_kv` batch-apply also shipped (bit-identical).
> - **STILL OPEN: #2 (scheduler ceiling ~50 blk/s) and #4 (latent O(height²)
>   sums scan).** #2 is now the rate ceiling: one supervisor thread, 2s tail
>   period, batch 100 → ~50 blk/s → ~17h ideal to the checkpoint (real-world
>   with the utxo_apply serial gate + overheads: observed a ~67h mint). The
>   tail-period/batch refold override in #2 below is NOT yet wired.
> - The ~3.3 blk/s / ~12-day figures below **predate the speedups — do not quote
>   them.** Re-measure against HEAD before relying on any number here.
>
> Built 2026-06-20 from a live `-refold-staged` run (folding the fixture from
> genesis, no peers) + a source-grounded bottleneck pass. All fixes below are
> gated on `refold_in_progress()` so a normal boot/sync is byte-identical.
> Verify against code before relying; specifics rot.

## The split-brain root cause

`-refold-staged` resets the 8 staged cursors + coins_kv to genesis, but does
NOT reset `pindex_best_header` — it stays pinned at the imported old tip
(~3,151,411) because `header_admit` advances it forward-only
(`app/jobs/src/header_admit_stage.c:420-429`). So every staged step extends the
active-chain window to a target 3.1M above the fold frontier.

## Ranked bottlenecks (dominant first)

### #1 DOMINANT — per-block ~3.1M-node `pprev` walk (CPU/cache-bound) · ✅ LANDED (`tip_finalize_stage.c:586`)
Every stage step calls `reducer_extend_window_to_candidate(ms, true)`
(`app/jobs/include/jobs/stage_helpers.h:139-150`) → `active_chain_extend_window(
&ms->chain_active, ms->pindex_best_header)` → `active_chain_fill_window`
(`lib/validation/src/chainstate.c:350-391`), which walks the pprev chain from
3,151,411 down (`walk_budget = new_height+1`). `tip_finalize` then COLLAPSES the
window back to `next_h+1` every finalize (`tip_finalize_stage.c:475`
`active_chain_move_window_tip`) and re-extends to 3.1M next step
(`tip_finalize_stage.c:593`). Inside one batched drain (`TIP_FINALIZE_BATCH_PER_TICK
=100`) that's up to 100 full 3.1M-node pointer chases/tick — hundreds of millions
of cache-missing dependent loads. Matches the 76% (cache-stall, not 100%) CPU.

FIX (#1) — REJECTED first attempt: routing the hot path through
`active_chain_extend_window_have_data` (`chainstate.c:500`) during refold. That
primitive was UNUSED in production (only test_active_chain_extend.c calls it)
and has TWO defects that a live refold exposes:
  (a) heap buffer overflow → `double free or corruption`: `elig` is sized by the
      HEIGHT span `hi-lo+1` (chainstate.c:520) but filled by BLOCK count
      (`elig[n++]`, :540). The real chain has forks/orphans (multiple blocks at
      one height), so `n > span` writes past the buffer. Tests pass only because
      test chains are fork-free.
  (b) it is NOT bounded: `block_map_next` (:528) scans the ENTIRE block_map
      (~3.1M entries) per call — same O(map) cost it was meant to avoid.
Both must be fixed before that primitive is usable anywhere.

FIX (#1) — the real structural fix: `active_chain`'s `c->height` conflates
window COVERAGE (what `active_chain_at` can see) with finalized-tip AUTHORITY.
`tip_finalize` retracts it to `next_h+1` every block via
`active_chain_move_window_tip` (`tip_finalize_stage.c:475`), so the stages'
`reducer_extend_window_to_candidate` must RE-widen to best_header (3.1M) every
step — `active_chain_fill_window`'s early-out break (`chainstate.c:377`, fires
when `h<=old_height && arr[h]==slot`) can't help because `old_height` is the
collapsed-low value. Authority is ALSO tracked separately by
`g_last_advance_height`/`update_last_advance` (`tip_finalize_stage.c:471,493`),
so the window collapse is redundant for authority. Candidate fix (to design +
adversarially verify, refold-gated): during a refold do NOT retract the window
in tip_finalize (skip/replace the `move_window_tip` retraction), so the first
extend fills `chain[0..3.1M]` ONCE and every later extend is a no-op
(`candidate->nHeight <= c->height`, :470). Per-block cost O(1) instead of
O(3.1M). Must verify tip_finalize still advances its cursor + writes its log,
and that no live reader depends on `c->height==finalized` during a refold (the
watchdog/reconcile are already suspended by refold_in_progress()). NOT a
quick inline change — the first hasty attempt shipped a heap corruption.

### #2 — scheduler ceiling: 1 thread, 2s period, batch 100 → ~50 blk/s
All 8 stages run on ONE supervisor thread (`lib/util/src/supervisor.c:323-340`);
each `on_tick` fires only when `now-last_tick >= period_secs(=2)*1e6`
(`app/supervisors/src/staged_sync_supervisor.c:249`), draining a batch (tail
stages capped 100). Sequential gating (`utxo_apply` idle when `next_h >=
proof_validate`, `utxo_apply_stage.c:302`) makes the slowest tail stage the rate.
Once #1 lands, 100/2s = 50 blk/s is the next ceiling → still ~17h to checkpoint.

FIX (#2): during refold, lower the tail-stage period to sub-second and/or raise
the tail batch (100→~2000). Compile-time consts in `app/jobs/include/jobs/*_stage.h`
today; thread a refold-gated override. Needs batched commits (io-speedups) so a
big batch is one fsync, not 2000.

### #3 — utxo_mirror_sync full wipe+reinsert every 5s (O(n), goes quadratic) · ✅ LANDED (`utxo_mirror_sync_service.c:446`)
`utxo_mirror_sync_run_once` (`app/services/src/utxo_mirror_sync_service.c:307`,
tick 5s) sees drift on every pass during the fold, so `mirror_rebuild_from_coins_kv`
(line 170) does `DELETE FROM utxos` + per-row reinsert of EVERY live coin + a
wallet/address cache rebuild (`app/models/src/utxo.c:330`). Cheap at low N, but
O(total_coins) per pass → quadratic vs fold progress; dominates at high N.

FIX (#3): during refold, skip/greatly-raise the mirror tick (gate on
`refold_in_progress()` or `ZCL_UTXO_MIRROR_TICK_SECONDS`); rebuild once at the end.

### #4 — `utxo_apply_sums_through` O(height) scan/block (latent, O(height²))
`tip_finalize` step calls `SUM(...) FROM utxo_apply_log WHERE height<=? AND ok=1`
per block (`tip_finalize_stage.c:451` → `tip_finalize_log_store.c:73`). Only when
a `live_utxo_count_after` counter is wired (NULL skips). Maintain a running total
incrementally instead.

## Non-factors (ruled out)
- Per-block fsync: with io-speedups it's 1 COMMIT/100-block drain; CPU is 76%
  (busy), not blocked on disk. Real but secondary.
- Watchdog / condition_engine / observer / activation lock contention: those
  paths take neither `progress_store_tx_lock` nor coins_kv; ruled out for a
  peerless refold.

## io-speedups (round1/io-speedups) reorg bug — must fix to land batching
`stage_set_cursor` (`lib/util/src/stage.c:510`) and
`stage_set_named_cursor_if_behind` (`:551`) do an unconditional `BEGIN IMMEDIATE`.
Inside an open batch (the reorg-rewind path `tip_finalize_stage.c:286` calls
set_cursor) SQLite rejects the nested BEGIN → "cannot start a transaction within
a transaction" (test_reducer_ingest_e2e reorg case). Make both batch-aware
(SAVEPOINT/RELEASE/ROLLBACK-TO when `stage_batch_active()`), mirroring
`stage_run_once` (`:413`).

## Order of work
1. Fix the io-speedups reorg bug (unblocks the test suite + batched commits).
2. #1 window bound (the 380x win). Re-measure.
3. #3 mirror gate. #2 batch/period. Re-measure to a feasible rate.
4. Then run the correctness proof to the checkpoint.
