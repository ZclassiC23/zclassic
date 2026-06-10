# Assessment: import-reset (#10) and write-ordering (#7) — evidence-backed deferral

Status: **assessed, fix deliberately deferred for safety; standing detector in place.**
Date: 2026-06-02. Branch: `finish/self-healing-service`.

Both tracks were carried through design + adversarial critique. The conclusion
for each is the same: the live symptom is **not currently reproducible**, the
existing machinery already holds the invariant, and the proposed "fix" is a
consensus-adjacent change that **cannot be validated without a reproduction** —
so shipping it would risk *introducing* a wedge, repeating the catastrophic
3,130,701 → 47,279 reset. The honest engineering call is to document the
evidence and leave the reproduce-on-copy harness as the standing gate.

## #10 — Import-recovery resets the tip

### Symptom (historical)
After `--importblockindex` recovery the node reached a high tip (~3,132,687,
matching `coins_best`), held briefly, then the reducer drain reset the public
tip to ~199 and re-finalized from near-genesis.

### Root mechanism
`tip_finalize_stage_init` anchors to `active_chain_cached_tip` via
`anchor_cursor_to_authority(existing_tip->nHeight, ...)`. When the active-chain
tip **lags** `coins_best` after an import (empty/behind `tip_finalize_log`,
cursor at 0), the anchor is placed too low, and the Tier-2 public tip
(`SELECT MAX(height) FROM tip_finalize_log WHERE ok=1`) is whatever the low
anchor/drain produced — dragging the published tip down.

### Evidence it is NOT currently reproducible
`make repro-on-copy SLUG=import-reset` (this session) snapshotted the live
datadir to a copy and ran the node on it for 300s:

```
first_tip: 3132687   max_tip: 3132687   post_tip: 3132687
VERDICT:   PASS — tip held/advanced (no regression) over 300s
```

The current datadir + current code boots to tip and **holds** — no regression.
The recent `392f7256d "align trusted reducer anchors"` plus the existing
`init_existing_tip` anchor cover the live case. (Caveat: the live datadir has a
populated `tip_finalize_log`, so it does not exercise the *empty-log* import
path; absence of reproduction is strong but not a proof the empty-log path is
fixed.)

### Why the obvious fix is NOT shipped (the real blocker)
The obvious fix — seed an `ok=1` anchor row at `coins_best` so the public tip
never drops below it — requires getting the `tip_finalize_log.tip_hash`
convention exactly right. That convention was traced this session and is subtle:

- The "finalized" row is written `log_insert(db, next_h, "finalized", true, …,
  new_tip->phashBlock)` where `new_tip = active_chain_at(next_h + 1)`. So a row
  at height **H holds the hash of block H+1** (convention B).
- `finalized_row_active_match` compares row H's `tip_hash` to
  `active_chain_at(row_height + 1)` — **consistent** with convention B.
- `rewind_cursor_if_active_chain_reorged` uses that match to decide whether to
  **rewind the tip_finalize cursor**.

Implication: a seeded anchor whose (height, hash) pairing does not satisfy
convention B would make `finalized_row_active_match` report `matches=false` and
trigger a cursor **rewind** — i.e. the exact reset it was meant to prevent.
Because the bug does not reproduce on the current datadir, this seed cannot be
validated end-to-end, and an unvalidated consensus-adjacent change here is
precisely the class that caused the 47,279 collapse. **Deferred until a
reproduction exists.**

### What IS in place
- The complementary direction (cursor **ahead** of coins) is fixed and
  unit-tested: `stage_reconcile_clamp_tip_finalize_to_floor` +
  `test_stage_reducer_unwedge` (clamps tip_finalize to `coins_best+1`, deletes
  no rows, public tip never drops below `coins_best`).
- `make repro-on-copy` is the standing detector: it FAILS LOUD on any tip
  regression, so a recurrence is caught on a copy, never on the live chain.

### To finish #10 when a reproduction exists
1. `make repro-on-copy SLUG=import-reset` against a datadir snapshotted in the
   broken state (empty `tip_finalize_log` + high `coins_best`); confirm the
   regression reproduces with current code.
2. Design the seed honoring convention B (row at `coins_best-1` holding the hash
   of block `coins_best`, or the anchor form the cold-start path already uses),
   insert-only (never overwrite a real row), cursor advance-only (never rewind).
3. Reset-safe unit test mirroring `test_stage_reducer_unwedge`: assert the
   public tip never drops below `coins_best` AND `finalized_row_active_match`
   stays `matches=true` for the seeded row (no spurious reorg rewind).
4. Re-run `make repro-on-copy` to prove the regression is gone.

## #7 — Write-ordering prevention

### Claim
A kill-9 leaves the durable tip_finalize cursor / `coins_best` marker ahead of
committed coins.db state, because the cursor/marker advance is not ordered after
the coins.db commit.

### Assessment (from the adversarial critique)
- The clean-shutdown path already checkpoints node.db before closing
  progress.kv (the ordering largely holds on graceful stop).
- kill-9 does not flush the WAL anyway; on the unclean-restart path the I2 clamp
  (`stage_reconcile_clamp_tip_finalize_to_floor`) already floors the public tip
  at `coins_best`, closing the operative cursor-ahead-of-coins case.
- A per-block commit barrier would run on the 3.1M-height catch-up path
  (thousands/sec) and is perf-risky; under `synchronous=NORMAL` a passive WAL
  checkpoint does not even guarantee an fsync of the main db, so the naive
  barrier adds cost without establishing durability.

### Decision
Do **not** ship a per-block barrier. The cursor-ahead-of-coins case is already
closed by the I2 clamp; the marginal value of a barrier is only against host
power-loss, and it must be throttled + profiled (core doctrine: profile before a
high-frequency change). Deferred unless profiling shows a real durability gap on
a class the clamp does not already cover.
