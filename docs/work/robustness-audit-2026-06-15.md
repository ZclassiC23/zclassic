# Robustness audit — 2026-06-15

A 6-surface adversarial audit of the node's critical path — reducer engine,
repair ladder, coin/UTXO apply+backfill, boot/import/recovery, defensive-coding
surface, concurrency/lifecycle — every finding verified by reading the cited code.

## Architecture verdict — SOUND, no refactor

The log-as-truth / single-reducer architecture holds. Every load-bearing
invariant was verified first-hand:

- **H\*** never rewinds below the compiled SHA3 checkpoint (`reducer_frontier.c`
  hard-clamps `hs` up to the anchor); H\* = MIN over each log's contiguous ok=1
  prefix, capped by hash-agreement (C3).
- **tip_finalize cursor floor** = the served tip's OWN height, never +1
  (`tip_finalize_anchor.c`, `tip_finalize_stage.c`).
- the **coin-tear check is a diagnostic** measured against utxo_apply's OWN
  contiguous frontier (`ua_contig`), not the global MIN H\* — it reports and
  refuses, never papers over (the never-built "L2" was correctly never built).
- the **phashBlock UAF class is closed** (per-node `hashBlock` storage).
- the **LOCK-ORDER LAW** is respected on the reducer drive.

**The recurring failure is not architectural.** **Torn UTXO sets are INSTALLED at
import time with no cryptographic proof**, then DETECTED ~80k blocks later when
forward apply spends a missing coin. The cure is **write-time prevention** (verify
imports vs the compiled checkpoint) plus, longer-term, an **incremental
order-independent rolling UTXO commitment** so a tear is caught at the apply that
creates it — NOT a per-block full rescan, and NOT more repair rungs. The
~6,040-LOC repair ladder should be **deleted leaf-first** once the write-time gate
makes torn coins uninstallable.

## Landed this session (origin/main, all non-consensus, no live-deploy)

| Commit | Findings | What |
|---|---|---|
| `12a8196ef` | RED-1, DEF-1, DEF-3, DEF-5, COI-1 | fail-loud: page masked reducer FATALs on the 2s-budget exit; log swallowed coins_kv prepare/step/schema errors; distinguish coins_applied read-error from absent-key; persist the durable re-lost-coin marker the boot torn-gate reads |
| `08b788a51` | **BOO-1 (P1)** | verify the P2P snapshot UTXO set against the compiled SHA3 checkpoint **before install** (hard-reject on mismatch); drop the misleading "(SHA3 verified)" block-file print |
| `169cb02a1` | COI-4 | correct the `apply_coins_kv` comment: adds-before-spends ORDER IS load-bearing (spends-first → phantom UTXO) |

Gate at each step: lint 37/37 clean, full LTO build clean, `test-parallel` 0/424.

## Correction to the audit — REP-1 was WRONG

`stage_reconcile_clamp_tip_finalize_to_floor` (`app/jobs/src/stage_repair.c`) is
**NOT dead.** There is a **live production caller at `config/src/boot.c:3260-3274`**.
**Do not delete this TU.** Any ladder-deletion sequencing must re-derive the
caller graph with `config/` in scope.

## Deferred — safe, but want their own tested pass (NOT started)

These are real and well-specified; they were held back deliberately (boot-path
risk / multi-site lock changes / P3 severity) rather than rushed.

- **BOO-3 (P2, startup speed):** eliminate the unconditional ~3.1M-entry
  `block_map` walk on warm boot in `block_index_loader_topup.c:258-330` — fold
  disk-recovery-candidate detection into the existing `topup_row_cb` iterate,
  skip the full walk when empty (mirror the nChainTx skip-guard).
  **Validation:** copy-prove on `~/.zclassic-c23-livetear-fixture-20260613`
  (isolated ports), confirm identical recovery-candidate set + faster warm boot.
- **BOO-7 (P3, boot correctness window):** reorder the cold-import
  `coins_applied` raise to AFTER the seed-anchor step succeeds
  (`block_index_loader_rebuild.c:683-722`) to drop a spurious C4 tear WARN on
  partial failure. **Validation:** boot-path → copy-prove on the fixture first
  (this is wedge-adjacent code; never live).
- **BOO-2 + COI-3 (both P3, diagnostic specificity):** give
  `find_lowest_prevout_unresolved_hole_unlocked` a wanted-status parameter so a
  lower transient `internal_error` row cannot mask a higher genuine
  `prevout_unresolved` tear — at BOTH call sites (`torn_gate.c:151` AND
  `stage_repair_coin_backfill.c:466`, else the marker writer still masks).
  Make `resolve_creator` (`stage_repair_coin_backfill.c:177-262`) a **whole-set**
  verdict: track the strongest class across all outpoints instead of breaking on
  the first reason, so a terminal sibling behind a retryable one still earns its
  durable refused marker. Add tests (a lower internal_error + higher refused
  tear; a delta-window-first + terminal-second set).
- **CON-1 (P2):** make `db_maintenance_dump_state_json` non-blocking — trylock in
  the **dump only** (not `db_maintenance_status_snapshot`), emit `busy:true` +
  atomic `loop_ticks` on EBUSY so diagnostics stay responsive mid-VACUUM.
  First verify the worker actually holds `g_dbm.lock` through the VACUUM.
- **CON-2 (P2):** serialize `sync_monitor` `g_local_recovery` cross-thread
  access behind a dedicated static mutex, snapshotting to locals first to avoid
  nesting with `cs_nodes` (LOCK-ORDER LAW — done wrong this re-introduces the
  ABBA class). **Do carefully.**
- **CON-3 (P3):** static-initialize the two `tip_finalize_stage` dump mutexes
  (`PTHREAD_MUTEX_INITIALIZER`); currently zero live reachability — harmless.
- **CON-5 (P2):** brief `cs_main` snapshot in `block_index_dump_state_json`
  (incl. multi-word `nChainWork`) before formatting JSON.

## Owner-gated (need a live action / replay)

- **Clear the live wedge:** the node is still on the OLD binary, wedged at
  h=3,145,594 on the coin tear. Deploying the merged binary + wipe + two-step
  cold re-import (copy-prove on the fixture first) is the standing remedy.
  Unblocks MVP C3/C6/C8. BOO-1's gate protects FUTURE imports only.
- **Ladder-deletion steps 2-7:** delete coin_backfill (3 TUs, ~1798 LOC) ONLY
  after the write-time SHA3 import gate is confirmed live (it is the runtime
  compensator for the exact import-skip that produced the tear); preserve the
  durable `coin_backfill.refused` marker emission (the boot torn-gate reads it).
  Then the replay rungs / tipfin backfill / non-canonical purge / refill clamps /
  poison rewind / orchestrator LAST — each needs a clean resync first.

## Consensus-surface — REPORT ONLY (needs full-history replay)

- **COI-2 (P1, forward fork-risk):** a within-block **cross-transaction
  double-spend** is silently accepted on the live reducer path
  (`utxo_apply_delta.c:162-256` builds the spend set with no within-block
  consumed-set; the second `coins_kv_spend` is a no-op). zclassicd's ConnectBlock
  rejects these; the canonical catch `update_coins_with_undo` (`update_coins.c:57-63`)
  is dead (only `connect_block`, which has no production caller, reaches it).
  History-safe (no such block exists on the immutable chain), but a crafted
  forward block would fork. The fix RESTORES parity (scan recorded spent[] for a
  matching outpoint before recording; `delta_fail` with a **non-repairable**
  status — NOT `value_overflow`, which the ladder treats as repairable). Per the
  Consensus Parity Doctrine this **must be validated by a full-history replay**
  against the real chain and pinned by a `test_consensus_parity` golden before
  shipping. Do not land without the replay.
