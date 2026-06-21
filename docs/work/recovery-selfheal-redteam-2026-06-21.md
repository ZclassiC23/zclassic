# Recovery / self-heal red-team — daily-driver verdict (2026-06-21)

> Red-team of every corruption/crash class: does the node (a) DETECT it without
> serving bad state, (b) request the RIGHT heal, (c) COMPLETE the heal UNATTENDED?
> 5 agents, cross-checked against the LIVE node. Raw: workflow
> `recovery-selfheal-redteam` (run `wf_82a0127d-dda`).

## Verdict
**Not a stranger's daily driver YET — one blocker.** The node honors "no silent
halt" across ALL four classes (every halt is a named `EV_OPERATOR_NEEDED`/FATAL,
verified live: node up >1 day, `NRestarts=0`, serving-degraded with
`operator_needed=true`). But it does NOT "self-heal without a human" for
torn-cold-import.

| Class | Detect | Request | Completes unattended? |
|-------|--------|---------|----------------------|
| chainstate-corruption (tip holes/mismatches) | ✅ `chain_restore_integrity.c:91` → UNRECOVERABLE | `-reindex-chainstate` (durable sentinel) | ✅ **confirmed live reindexing ~390 blk/s** — replay from blocks/ + `reindex_epilogue_derive` self-checks H* climb; systemd `Restart=always` |
| kill9-mid-fold | ✅ structural (SQLite atomic per-block txn) | implicit crash-only | ✅ durable-tip reseed, forward-only, gated `coins_applied>=tip` |
| torn-cold-import (the live wedge) | ✅ torn gate | auto-arm refold-from-anchor (now wired+armed, fail-SAFE decline) | ❌ **BLOCKED — no reachable snapshot** |
| supervisor restart | — | — | ✅ unit is `Restart=always RestartSec=5..600 StartLimitIntervalSec=0` (never burns out) |

## THE daily-driver blocker (severity: blocks-daily-driver)
**torn-cold-import does not self-heal because no SHA3-verified anchor snapshot
exists at the live datadir.** The repair engine
(`boot_refold_from_anchor_arm_if_torn` → `boot_anchor_seed_from_snapshot`) is
wired, armed (runs unconditionally on every boot), and correct — but
`anchor_snapshot_verified_reachable` (`boot_refold_staged.c:586`) returns false
because `~/.zclassic-c23/utxo-anchor.snapshot` is ABSENT and `ZCL_MINT_ANCHOR_OUT`
is unset, so it declines every boot → honest `operator_needed` halt indefinitely.

**The verified snapshot EXISTS at `/tmp/utxo-anchor-3056758.snapshot` (101 MB,
SHA3-verified, h=3,056,758).** The fix is to make it reachable WITHOUT a human:
- **(a) bake it in-binary** (compiled blob; `anchor_snapshot_verified_reachable`
  resolves to it when no on-disk file) — sovereign, can't be lost. This is the
  "self-minted in-binary checkpoint" (LB-2) from the architecture roadmap.
- **(b) write `<datadir>/utxo-anchor.snapshot` at deploy** (`make deploy` runs
  mint-anchor-fast into the datadir) — quick path.

With either, torn-cold-import becomes: detect → auto-arm → load+verify anchor →
fold anchor→tip → self-clear, fully unattended under `Restart=always`.
**This is the ONE change that flips the daily-driver verdict.**

## "Always-terminates" corner gaps (boot reindex budget)
1. **(degrades) Unbounded reindex loop on stable-tip corrupt blocks/.** At
   `BOOT_AUTO_REINDEX_MAX+1` (`boot_crashonly.c:77`) the handler `boot_auto_reindex_clear()`
   DELETES the only durable record, pages, exits; next restart finds no sentinel,
   re-detects, writes a fresh `count=1` → re-arms forever (one page per 4 boots,
   throttled by RestartSec). **Fix:** rewrite the sentinel with a terminal marker
   (`<anchor> -1`); `consume` treats -1 as "do not re-request"; boot takes
   `SERVICE_STATE_DEGRADED_SERVING` + latches operator_needed (match the correct
   `chain_tip_watchdog` design). Stays up, serves degraded, stops power-cycling.
2. **(minor) Swallowed replay failure.** `reindex_chainstate==false` only prints
   "Warning ... and continue" (`boot.c:2608-2611`) without advancing the budget →
   re-replays the full chain every restart. **Fix:** advance the budget or exit
   into the bounded handler.
3. **(minor) Moving-tip budget bypass.** Budget keys on `tip_h`; a partial replay
   with a different tip each boot resets `count=1` (`boot_auto_reindex.c:45`) →
   never hits the cap. **Fix:** key on a stable identity (min scan_reindex_best /
   monotonic episode counter).
4. **(minor) Stale systemd comment.** `deploy/zclassic23.service:16-27` carries a
   pre-crash-only comment asserting the node is "never auto-restarted", contradicting
   the active `Restart=always` at :58 — an operator who trusts it could break every
   self-heal path. **Fix:** delete/repoint the comment.

## Recommended regression tests (none exist today)
- Full-binary torn-cold-import self-heal: torn fixture + written snapshot → boot
  unattended → assert H* CLIMBS past the hole + operator_needed flips false; twin
  without snapshot → declines + honest page (bounded NRestarts).
- Boot reindex-budget exhaustion+termination unit (`grep BOOT_AUTO_REINDEX_MAX`
  in lib/test is empty today).
- Moving-tip budget-bypass unit; swallowed-replay-failure full-binary test.
- Extend `test_kill9_recovery.c` to fuzz the PRODUCTION fold atomicity (SIGKILL
  mid-batch across `stage_batch_begin`/SAVEPOINT/`stage_batch_end`).
