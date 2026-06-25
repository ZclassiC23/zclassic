# Self-heal terminal rung — "re-fold coins from a header-verified trusted base"

Implementation-ready design (read-only audit, 2026-06-25). Cures the torn-`coins_kv`
class: H\* freezes when a block's `utxo_apply` verdict is `ok=0` on a
network-canonical block, leaving coins folded onto state it refuses to advance
past. This is the live wedge at 3157647. The escalator runs
`retry → targeted_rederive → resnapshot → reindex` but **none re-folds coins from
a clean base**, so the tear never clears.

**Depends on `sticky/sapling-frontier` landing first** — the remedy reuses its
`boot_load_snapshot_at_own_height_reset` seed+verify path. Do not implement before
that branch is on the integration line. Copy-prove only; gate on H\* CLIMB.

## The rung already half-exists
`STICKY_RUNG_SELF_MINT_REFOLD` (index 4) is a stub at
`app/services/src/sticky_escalator.c:157-161` returning `NOT_IMPLEMENTED`. Making
it real is the task. Register the real action via the existing seam
`sticky_escalator_register_rung(STICKY_RUNG_SELF_MINT_REFOLD, rung_refold_from_base)`
(`sticky_escalator.h:70`, impl `sticky_escalator.c:310-315`) — no dispatch-core
edit. Witness window already sized: `[STICKY_RUNG_SELF_MINT_REFOLD]=1800s`
(`sticky_escalator.c:63`). Reached only after rungs 0-3 fail to move H\*.

## (b) Detect predicate (read under `progress_store_tx_lock()`)
```
armed && rung == SELF_MINT_REFOLD
 && hstar == reducer_frontier_provable_tip_cached()
 && header_tip > hstar+1                                   // H on PoW header chain
 && active_chain_at(chain, hstar+1) != NULL                // (after active_chain_extend_window)
 && coins_applied_height == hstar+1 && utxo_apply.cursor == hstar+1   // pinned, not lagging
 && (proof_validate_log[hstar+1].ok==0 || script_validate_log[hstar+1].ok==0)
 && hstar+1 > REDUCER_FRONTIER_TRUSTED_ANCHOR             // stay above finality (3056758)
```
Corroborating: `tip_finalize_stage_last_blocked_reason()==successor_pending` and
`blocked_at_utxo_frontier_total>0`.

## (c) Remedy — arm a durable next-boot request, then restart (mirrors the reindex rung)
The seed+verify+cursor-reset is boot-time-only, so the rung does NOT do live
surgery. Runtime: pick trusted base `B` (highest verified snapshot at/below H\*,
≥ anchor; probe `boot_load_verify_snapshot_eligible`); write a fsync-durable
sentinel `<datadir>/refold_from_base_request = "<B> <count>"` (new file, clone of
`lib/storage/src/boot_auto_reindex.c` — bounded count, TERMINAL marker);
`event_emitf(EV_RECOVERY_ACTION, ...)`; return `PROGRESSING` (holds 1800s) or
`FAILED` on TERMINAL (ladder goes deeper — never wedge).

Boot (next boot, EXISTING path): in `config/src/boot.c` just before line ~3470,
if the sentinel is pending set `ctx->load_snapshot_at_own_height = <artifact for B>`
and clear it → routes into the wired `boot_load_snapshot_at_own_height_reset`
(`config/src/boot_refold_staged.c:646`, on `sticky/sapling-frontier`):
self-verifies body SHA3; `coins_kv_reset_for_reseed`; re-seeds via `uss_iter`;
consensus-binds the base (`boot_snapshot_anchor_hash_matches`, FATAL on mismatch);
installs the v2 Sapling frontier and **skips `sapling_tree_rebuild`**; two-tier
cursor reset (header/body stages stay at MAX(current,seed_h); coins stages
`script_validate/proof_validate/utxo_apply/tip_finalize` forced down to seed_h +
their derived tables deleted); `coins_kv_set_applied_height_in_tx(seed_h+1)`;
drops the trusted-base markers. Then `app_init_services` runs the NORMAL reducer
folding `seed_h → tip` over on-disk bodies. No new fold engine.

## (d) Witness = H\* CLIMB (not "booted w/o FATAL")
Reuse the escalator progress gate (`sticky_escalator.c:259-264`): episode clears
when `provable_tip >= entry_tip + STICKY_PROGRESS_MARGIN(2)`. Assert the re-fold
landed contiguous: `coins_applied_height == provable_tip+1` and
`utxo_apply.frontier_eq_cursor==true`. Reporting via the existing
`EV_RECOVERY_ACTION` / `clear_episode()` / `EV_OPERATOR_NEEDED terminal=0` channels.

## (e) Safety / termination
Bounded attempts + TERMINAL (sentinel clone of `boot_auto_reindex.c`,
`BOOT_REFOLD_FROM_BASE_MAX≈3`); idempotent (deterministic reseed; count keyed on
base/episode); base `B ≥ REDUCER_FRONTIER_TRUSTED_ANCHOR` and consensus-bound
(FATAL on forged anchor); **no validity rule lowered** (re-runs the same stages —
E13 safe); the sentinel is never in any derived-state wipe set, and the
validate_headers/nStatus clear happens once at boot before stages start, so it
cannot race the live 300s `watchdog_check_stuck` BLOCK_FAILED churn.

## (g) Top risks
1. **The root is a `validate_headers` height-splice (`header-source-hash-mismatch`
   at H), not only the coins tear.** If the rung reseeds coins but keeps
   `validate_headers` at MAX(current,seed_h), the fold re-derives the identical
   `ok=0` at H and re-wedges — burning the budget. The rung MUST also clear the
   stale `validate_headers_log` failure row + the relabeled `block_index` nStatus
   at H so the corrected (full 1344-byte) header re-validates on the way up.
   Reuse the failure-clear from `stale_validate_headers_repair.c:248` /
   `chain_restore_repair.c:367`. **This is the one piece of genuinely new logic.**
2. **Branch dependency.** The whole seed/verify/two-tier-cursor remedy lives on
   `sticky/sapling-frontier`, not `main`. On `main` today the loader is v1-only and
   `boot_load_snapshot_at_own_height_reset` doesn't exist — fallback would be
   anchor-only (`boot_refold_from_anchor_reset`, seeds at 3056758 → ~100k-block
   re-fold). Sequence the merge.
3. **Restart-to-apply coupling.** `chain_tip_watchdog` deliberately skips restart
   on a deterministic stall (`chain_tip_watchdog.c:362-375`), so the armed sentinel
   may sit un-consumed. Either trigger a bounded self-restart (as the reindex path
   does) or teach the deterministic-stall classifier that a `refold_from_base`
   request is pending and a restart will clear it — else the rung is
   armed-but-never-applied (silent no-op = violates auto-terminating).

## Copy-prove recipe
`cp -a` live → confirm the copy reproduces the wedge (`utxo_apply.cursor=3157647`,
H\* frozen at 3157646) → mint the base artifact (`tools/mint_v2_snapshot <copy> <B>
<out>`) → prove the boot reset alone (`-load-snapshot-at-own-height=<out>`, watch
the climb) → prove the FULL rung autonomously (let the escalator advance through
the ladder, write the sentinel, restart, consume it, H\* climbs 3157646 → past
3157647 → toward header tip with `coins_applied==H*+1`) → prove bounded/idempotent
(corrupt artifact → TERMINAL after MAX, ladder advances, no crash-loop) →
`make lint && make -j && make test-parallel` + a unit test for the sentinel + rung
transitions.

## Critical files
- `app/services/src/sticky_escalator.c` — add `rung_refold_from_base`; register at :310
- `config/src/boot_refold_staged.c` — the seed+verify remedy (sapling-frontier); ADD the validate_headers/nStatus clear at H
- `config/src/boot.c` — consume the sentinel before ~:3470, set `ctx->load_snapshot_at_own_height`
- `lib/storage/src/boot_auto_reindex.c` — clone to `boot_refold_from_base.c`
- `app/jobs/include/jobs/reducer_frontier.h` — H\*/applied-height/anchor vocabulary
