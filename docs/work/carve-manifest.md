# Carve manifest — the lie-cover deletion, pre-proven

> Status: **analysis only — do not execute before the cure lands.** Produced
> 2026-06-19 by an adversarial multi-agent audit (workflow, 22 files audited,
> every "deletable" verdict attacked by a skeptic hunting for a live caller).
> All cites are re-readable against the live tree; verify before cutting.
>
> **LIVE-VERIFIED CORRECTION (2026-06-19, against the running wedged node).** The
> original claim below — "these 8 files are the only live remedy keeping the
> imported node moving; deleting them pre-cure pins the live node" — is **FALSE in
> that strong form.** The node is ALREADY pinned with all 8 active:
> `reducer_frontier_reconcile_light` last_outcome=failed; `coin_backfill`
> last_status=refused_spent, inserted=0, repaired=0; node.log `L1 refused:
> coins_applied_height=3151412 > hstar_cursor=3056759` repeats 1,186+. The family
> ENGAGES this stall (`refused_coin_tear=TRUE`, since coins_applied 3,151,412 ≫
> contiguous utxo_apply ok=1 prefix 3,056,758; the coin-backfill works the
> prevout_unresolved hole) but TERMINALLY REFUSES (`coin_backfill.refused.3151412…
> =txindex_miss` latched) because node.db txindex tops out at 3,151,411 and the
> unfolded gap [3,056,759..3,150,899] has no fold rows to source coins from. So it
> RUNS-ATTEMPTS-REFUSES, it does not REMEDY. Cure-first gating still holds — but for
> the reasons in blockers #2/#3/#4, NOT because the family is a working remedy. The
> node's only true remedy is **B2 (offline from-checkpoint fold)**. Separately, the
> UTXO-set wrong-fork corruption is bounded at **exactly 1 coin** (02663FF1 at
> h=3,151,306; two independent methods), so a targeted 1-coin backfill is a viable
> *liveness* band-aid but leaves the deep lie (H*=3,056,759 ≪ served 3,151,411)
> intact.

## What this is

The recurring wedge has ONE root: cold import **stamps** stage cursors past
unfolded logs instead of folding the chain forward
(`block_index_loader_rebuild.c:728,743` + the `seed_exempt` cap-bypass at
`stage_anchor.c:153-154`). The honest fold engine (`reducer_frontier.c:465-559`,
H\* = MIN over contiguous `ok=1` prefixes) then correctly refuses the borrowed
foundation — that refusal *is* the live wedge.

A large family of "repair the frontier" code exists **only** to mop up after that
lie. This manifest says, file by file, exactly how much of it dies once the wound
is cured, in what order to remove it so the tree always links, and what must be
preserved. It corrects an earlier over-broad memory ("~25–30k LOC deletable,
zero features lost") with measured numbers.

## The numbers (22 files / 11,262 LOC audited)

| Class | Files | LOC | Meaning |
|-------|------:|----:|---------|
| **Cleanly deletable post-cure** | 8 | 5,895 | Every one pivots on `refused_coin_tear` / `prevout_unresolved`-hole / `value_overflow`-row-below-cursor — states only reachable because import stamped cursors past unfolded logs. The cure makes them impossible **by construction**. |
| **Needs rework (not delete)** | 1 | 799 | `stage_repair_reducer_frontier.c` — the orchestrator. Its tear pivot dies; its reorg/flag-reconcile guts must be re-homed. |
| **Genuine support — KEEP** | 13 | 4,568 | Reorg unwind, header-solution cache/poison-rewind, body-fetch self-heal, legacy/utxo mirror + oracle drift detectors, wallet/explorer read-model feeder. NOT lie-cover. |

### The 8 cleanly deletable (post-cure)

| File | LOC |
|------|----:|
| `app/jobs/src/stage_repair_reducer_frontier_coin.c` | 787 |
| `app/jobs/src/stage_repair_reducer_frontier_refill.c` | 743 |
| `app/jobs/src/stage_repair_reducer_frontier_tipfin.c` | 650 |
| `app/jobs/src/stage_repair_coin_backfill.c` | 795 |
| `app/jobs/src/stage_repair_coin_backfill_scan.c` | 539 |
| `app/jobs/src/stage_repair_coin_backfill_util.c` | 483 |
| `app/jobs/src/utxo_apply_delta_repair.c` | 456 |
| `app/services/src/quorum_oracle_service.c` | 442 |

Plus `app/services/src/block_index_loader_torn_gate.c` (**not** in the 22 audited
but orphaned by removing `coin_backfill_util` — it forward-declares util symbols
and won't link alone; delete it in the same change, with its call site
`block_index_loader_rebuild.c:596`).

## Deletion order (leaves before orchestrators, tree always links)

1. **`utxo_apply_delta_repair.c`** (456) — leaf. Sever caller
   `stage_repair_reducer_frontier_coin.c:533` (`maybe_repair_value_overflow`);
   delete `maybe_repair_value_overflow` (coin.c:489-550) + dispatch
   (coin.c:724-736), the `value_overflow_repair_*` fields in
   `struct stage_reducer_frontier_reconcile_result` (`stage_repair.h:122-129`)
   and their reads (`reducer_frontier_reconcile_light.c:366-367,507-511`;
   `stage_repair_reducer_frontier.c:503-504`).
   **KEEP** `utxo_apply_delta_reorg.c`'s primitives
   (`emit_inverse_delta`/`delete_rows_above`/`unwind_write_cursor`) — shared with
   the LIVE reorg path `utxo_apply_stage.c:703`.
2. **`stage_repair_coin_backfill_scan.c`** (539) — sever callers in
   `stage_repair_coin_backfill.c:280,295,650,659`.
3. **`stage_repair_coin_backfill.c`** (795) — sever sole external caller
   `stage_repair_reducer_frontier_coin.c:748`.
4. **`stage_repair_coin_backfill_util.c`** (483) — sever
   `block_index_loader_torn_gate.c:108/111/113` (fwd-decls) + `:156/167/169`;
   remove torn-gate call site `block_index_loader_rebuild.c:596`; remove MCP
   `zcl_state subsystem=coin_backfill` row `diagnostics_registry.c:512`; delete
   `block_index_loader_torn_gate.c` itself.
5. **`stage_repair_reducer_frontier_coin.c`** (787) — sever sole caller
   `stage_repair_reducer_frontier.c:625`. Callees now gone, deletes cleanly.
6. **`stage_repair_reducer_frontier_tipfin.c`** (650) — sever tear-gated caller
   `stage_repair_reducer_frontier.c:650` (if-block 644-658).
7. **`stage_repair_reducer_frontier_refill.c`** (743) — sever tear-gated callers
   `stage_repair_reducer_frontier.c:646` + `:661`. See the REORG-RESIDUE blocker:
   `force_stage_cursor_in_tx` (:452) + `reconcile_refill_cursors` (:709) are
   UNGATED; re-home a reorg-residue cursor-clamp into the KEEP purge TU first.
8. **`quorum_oracle_service.c`** (442) — independent. Sever dead
   `rolling_anchor_service.c:384` ref + the two repair-only callers via
   `process_block_revalidate.c:124` (`block_failed_mask_at_tip.c:117` +
   `chain_supervisor.c:83`); remove MCP `zcl_state subsystem=quorum_oracle` row
   `diagnostics_registry.c:514`. `quorum_oracle_init` is never started in prod —
   cleanest standalone delete.
9. **REWORK `stage_repair_reducer_frontier.c`** (799) — LAST. Remove the dead
   tear branch (295-296 pivot, 644-703 dispatch, 234-296 `read_frontier_snapshot`
   `utxo_apply_contig`). **Preserve / re-home** the genuine self-heal:
   `reconcile_block_index_flags` (311-403, HAVE_DATA/VALID_SCRIPTS/FAILED_MASK),
   purge calls into the KEEP file `stage_repair_reducer_frontier_purge.c`
   (`purge_noncanonical` 580, `purge_stale_reorg_tipfin` 598),
   `reconcile_refill_cursors` (709) + `reconcile_tip_finalize_cursor` (712).
   Rewrite the self-heal Condition `reducer_frontier_reconcile_light`
   (`condition_registry.c:59`) to keep only the reorg-residue/flag-reconcile
   remedy, not the tear remedy.

## Blockers (must clear in this order)

1. **CURE FIRST — precondition for ALL deletes.** Wire `snapshot_verify.c`
   (FlyClient+MMB+SHA3) into the cold-import door and fold anchor→tip, deriving
   stage cursors honestly; remove the synthetic stamp
   (`block_index_loader_rebuild.c:728,743`, `stage_anchor.c:153-154 seed_exempt`).
   Until this lands, `refused_coin_tear` / `prevout_unresolved` holes are
   REACHABLE and these files are the only live remedy keeping the imported node
   moving — **deleting them pre-cure pins the live wedged node.**
2. **REORG-RESIDUE RE-HOME** (blocks step 7 + 9): `reconcile_refill_cursors` +
   `force_stage_cursor_in_tx` run UNGATED and consume holes from the genuine
   reorg-residue purge (KEEP). A replacement cursor-clamp must live in the kept
   purge/reconcile TU before `refill.c` is removed, or depth-N reorgs lose their
   clamp consumer.
3. **TORN-GATE COUPLING** (blocks step 4): delete `block_index_loader_torn_gate.c`
   together with `coin_backfill_util` + its call site, or the tree won't link.
4. **CONDITION DEREGISTRATION** (blocks steps 5-9): the Condition
   `reducer_frontier_reconcile_light` (`condition_registry.c:59`) is the sole live
   entry into the whole reducer-frontier repair family — deregister or rewrite it,
   never leave it dangling. Same for the quorum path
   (`block_failed_mask_at_tip` + `chain_supervisor` coord_escalation).

## Zero-feature-loss verdict: TRUE (verified)

No wallet, explorer, mempool, P2P, ZNAM, ZMSG, ZSWP/market, RPC, or block-serving
feature routes through any deletable file (grep over `tools/mcp`,
`app/controllers`, `app/services` = empty). The only losses are: (1) the
cold-import coin-tear/log-hole self-heal slice the cure makes unreachable; (2) two
read-only MCP `zcl_state` diagnostic subsystems (`coin_backfill`,
`quorum_oracle`) — cosmetic.

**KEPT (look adjacent, but survive the cure untouched):** reorg handling
(`utxo_apply_delta_reorg.c` live at `utxo_apply_stage.c:703`,
`invalidateblock`/`zcl_invalidateblock`; reorg-residue purge in
`stage_repair_reducer_frontier_purge.c`; header-solution-poison rewind
`stage_repair_rewind.c` serving `zcl_rebuild_recent`), header PoW re-source
(`stage_repair_header_solution.c`), body recovery (`stage_repair_body_fetch.c`),
the explorer/wallet UTXO read-model feeder (`utxo_mirror_sync_service.c` — the
only forward writer of node.db utxos), all wallet HTML UI
(`wallet_view_projection.c`), and the legacy/utxo mirror + `oracle_policy` +
`zclassicd_oracle` + `mirror_divergence_locator` drift detectors.

## Honest disagreements (reader vs skeptic)

- `stage_repair_reducer_frontier_refill.c`: reader said needs-rework (ungated
  reorg-residue clamp); skeptic overturned to deletable (post-cure no hole exists
  below an honestly-folded cursor → every clamp is a no-op). Following the skeptic,
  honoring the reader via blocker #2.
- `quorum_oracle_service.c`: reader said needs-rework; skeptic overturned
  (never started in prod; only wedge-only repair callers). Deletable, but its
  removal also drops the `BLOCK_FAILED_VALID`-at-tip auto-heal via
  `process_block_revalidate` — confirm the honest fold (H\* stops at first
  failure) covers that stuck-tip class before severing.

See [`never-stuck-plan.md`](./never-stuck-plan.md) for the cure (B1–B3) this
manifest is gated behind, and the north-star memory `project_frontier_wedge_*`.
