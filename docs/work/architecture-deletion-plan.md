# Architecture tree + deletion plan (less is more)

> **STATUS (2026-06-23): the forward-sync wedge is FIXED** by commit `ab512d577`
> (bind a snapshot above coins-best by extending the active-chain window): the
> loader seeds a COMPLETE SHA3-verified snapshot at **h=3,156,809** and folds
> forward past the former wedge (3,156,171) to the network tip — `docs/HANDOFF.md`.
> (The older torn 3,151,901 stopgap actually HARD-WEDGED — see the LIVE-VERIFIED
> correction block below — so the prior "consolidated stopgap reaches tip" framing
> was misleading.) This is **retained as the deletion map for
> the sovereign cure** — the file-by-file deletion order for the borrowed-seed /
> repair machinery the cure removes, plus the KEPT/deletable classification. Any
> dated "LIVE-VERIFIED" block below (node pinned, L1 refused N reps, the 1-coin
> wrong-fork bind) is **historical**, not the live node. Re-derive the delete-set
> + re-verify line numbers against the current tree before deleting.
>
> Built 2026-06-20 from a 4-subagent code map + synthesis, grounded in file:line.
> Verify against code before deleting; specifics rot.

## Headline

One borrowed UTXO set — copied from zclassicd, crypto-checked at a single hardcoded
height **h=3,056,758** — is the root of **~11,000 LOC** of import / seed / provenance /
repair machinery. **~282 LOC is dead right now.** Two small changes (flip the
already-built bodies-only refold to default + replace the hardcoded anchor with a
self-derived one, ~15 LOC) turn **~8,990 LOC dead-by-construction.**
**Grand total removable: ~9,550 LOC / ~27 files.**

## The live tree (problem nodes marked)

```
main()  src/main.c
├── verb one-shots
│   ├── --importchainstate  main.c:1291          ⚠ borrowed-data ingress
│   ├── --repair (zclassicd RPC → node.db utxos)  main.c:1008  ⚠
│   └── --importblockindex / -mcp / --gui
└── app_init() boot  config/src/boot.c:1121
    ├── coins-integrity gate (FATAL on torn anchor)  boot.c:1631   <= copied set
    ├── coins_kv_boot_rebuild_if_needed  boot_projections.c:109    (seed A; live caller now only snapshot_apply — KEEP)
    ├── -rebuildfromlog → boot_try_rebuild_block_index_from_projection  boot.c:1853
    │        ⚠ THE CURE — already wired, OFF by default (main.c:1730)
    ├── utxo_recovery_import_ldb  boot.c:2487 → utxo_recovery_restore.c:86   (seed B; RUNS on normal boot)
    │   │    ⚠ falls back to $HOME/.zclassic/chainstate (restore.c:141-146) = circular self-cert
    │   ├── cp -a a live foreign LevelDB  ldb_copy.c:61   ⚠
    │   ├── SHA3 verify == checkpoint @3,056,758  restore.c:308-321   ⚠ ONE height; all else stamped forward
    │   ├── coins_kv_seed_from_node_db  coins_kv_boot_rebuild.c:123
    │   └── tip_finalize_stage_seed_anchor → REDUCER_TRUSTED_BASE keys  tip_finalize_anchor.c:193   ⚠ manufactures the 88k log-less span
    ├── reindex_chainstate (-reindex-chainstate)  boot_index.c:238
    │   └── reindex_epilogue_derive (reseed coins_kv FROM node.db utxos)  reindex_epilogue.c:56   ⚠ circular reseed (wrong-fork mirror)
    ├── block_index_loader_seed_stages_from_cold_import  boot_services.c:1471   ⚠ pure dependent of the copied seed
    └── app_init_services  boot_services.c:568
        ├── reducer_ingest_block (the drive)  reducer_ingest_service.c:272   ★ KEEP
        ├── reducer_frontier_reconcile_light  reconcile_light.c:418  (L1/L2 self-repair)
        │   └── stage_repair_reducer_frontier{,_coin,_refill,_tipfin,_purge}.c   ~3,955 LOC
        │       └── stage_repair_coin_backfill (inserts coins the node never folded)  ~1,817 LOC  ⚠ consensus mutation
        ├── [DEAD] process_block_flush_coins / persist_sapling_tree  process_block.c:140  (0 callers)
        ├── [DEAD] Commitment-MMR proof (mmr_prove_from_leaves/mmr_verify)  mmr.c:153,252  (0 non-test callers)
        └── ★ L0 reducer_frontier — pure SELECT-only fold (KEEP FOREVER)
            └── compute_hstar  reducer_frontier.c:442  (clamp H* >= anchor :554)   <= ANCHOR 3,056,758 (borrowed trust root)
```

## Issues, worst first

1. A default boot silently leans on the borrowed foundation: `utxo_recovery_import_ldb`
   (boot.c:2487, gate `!rebuilt_from_log && !no_legacy_auto_import`, both default false)
   copies zclassicd's chainstate, falling back to `$HOME/.zclassic/chainstate`. On the
   owner's machine that exists → production cold-start IS the circular self-cert.
2. Whole-chain trust collapses to ONE crypto check at ONE height (restore.c:308-321);
   every other height is stamped forward off that anchor.
3. The cure exists in-tree but is OFF by default (`-rebuildfromlog`, boot.c:1853).
4. Hardcoded `REDUCER_FRONTIER_TRUSTED_ANCHOR=3056758` (reducer_frontier.h:39) hard-clamps
   H* (reducer_frontier.c:554) so the node can never prove [0, 3,056,758] itself.
5. `tip_finalize_anchor.c:193-229` raise-only stamps a trusted base above log coverage,
   manufacturing the 88k log-less span the I4.3 sweep (invariant_sentinel.c:243-249) then
   HOLDs on — the seed is both cause and ~2,500 LOC of "cure".
6. The ~3,955 LOC L1/L2 reconcile tree is steady-state-live, yet every actionable branch
   gates on `coins_applied_height > H*` — a state ONLY the copied/seeded path produces.
7. `stage_repair_coin_backfill.c` does a real coins_kv insert to patch copied-set holes —
   it writes coins the node never folded.
8. `-reindex-chainstate` is circular (reindex_epilogue reseeds from the wrong-fork mirror).
9. `--repair` / `--importchainstate` are 2nd/3rd borrowed-data ingress paths.

## Deletion plan (ordered)

**Dead now (no precondition):**
1. `process_block_flush_coins` / `persist_sapling_tree` wrappers — process_block.c:140 — ~12 LOC.
2. Dead-writer half of process_block_flush_policy.c (`flush_coins_if_needed`,
   `sapling_checkpoint_maybe_flush`, `sapling_tree_persist_once`) — ~120 LOC.
3. Commitment-MMR inclusion-proof half (`mmr_prove_from_leaves`, `mmr_verify`) — ~150 LOC.

**The two unlock changes (~15 LOC net):**
4. Flip bodies-only refold to the default cold-start (src/main.c:1730 / boot.c:2476).
   GATE: new `test_self_folded_anchor` — a fresh datadir (no ~/.zclassic, no node.db utxos)
   folds genesis→tip and reproduces checkpoints.c SHA3 + 1,354,771 UTXOs; tip hash-identical
   to zclassicd at ≥2 heights.
5. Self-derive the anchor — replace the hardcoded floor with the node's own folded height
   (reducer_frontier.h:39 + reducer_frontier.c:451-457,554; retire the `cp ? cp->height : ANCHOR`
   idiom at 5 sites).

**After 4+5 (dead-by-construction):**
6. `utxo_recovery_import_ldb` + the ~/.zclassic fallback + boot wiring — ~385 LOC.
7. Copy-import support: ldb_copy.c, mirror_walk.c, restore_chain_tip, coins_kv_seed_from_node_db — ~605 LOC.
8. Cold-import seed provenance + stage-seed + torn-import gate — ~600-800 LOC.
9. Trusted-base seed writer + I4.3 sweep + compiled SHA3 checkpoint struct — ~145 LOC.
10. The entire L1/L2 reconcile tree + coin-backfill + the driving condition — **~5,772 LOC / 12 files.**
    (KEEP stage_repair_rewind/header_solution/body_fetch — independent live callers.)
11. reindex/connect_block recovery path + `--repair` + `--importchainstate` — ~1,900 LOC.

## The minimal node (after)

```
main()
├── verb one-shots: --importblockindex, --gen-utxo-snapshot, -mcp, --gui   (NO borrowed-data ingress)
└── app_init() boot
    ├── coins-integrity gate (FATAL on torn anchor)   <= self-folded coins_kv
    ├── ★ bodies-only refold (DEFAULT) — reads block BODIES, seeds coins_kv, derives the anchor by folding
    ├── block index loaders / genesis / activation / finalize
    └── app_init_services
        ├── reducer_ingest_block (the drive)   <= self-derived H* + coins_kv
        ├── ★ L0 reducer_frontier — pure SELECT-only fold (floor = SELF-DERIVED, no compiled clamp)
        ├── stage_repair_rewind / header_solution / body_fetch   (independent live — KEEP)
        ├── snapshot_apply / snapshot_verify (FlyClient+SHA3 verified fast-sync — KEEP)
        ├── utxo_mirror_sync_service (coins_kv → node.db read model — KEEP)
        └── Equihash verify / sighash shim / block_source_policy   (live — KEEP)

    NO copied-data import.  NO cold-import seed/provenance/torn-gate.
    NO L1/L2 reconcile tree.  NO coin-backfill.  NO reindex/connect_block.  NO compiled anchor clamp.
    The node stands on coins it folded itself; coins_applied can never exceed H*;
    the coin-tear / log-less-span failure modes are unreachable by construction.
```

## Carve manifest — file-by-file deletion order + blockers (merged from carve-manifest, 2026-06-19)

> Produced 2026-06-19 by an adversarial multi-agent audit (22 files / 11,262 LOC,
> every "deletable" verdict attacked by a skeptic hunting for a live caller). All
> cites re-readable against the live tree; verify before cutting. The deletion is
> gated behind the cure (B1–B3 in [`never-stuck-plan.md`](./never-stuck-plan.md));
> the live-verified correction below explains why the family runs-attempts-refuses
> rather than remedies, so cure-first gating holds even though the family is NOT a
> working remedy.

**LIVE-VERIFIED CORRECTION (2026-06-19).** The node is ALREADY pinned with all 8
"deletable" files active: `reducer_frontier_reconcile_light` last_outcome=failed;
`coin_backfill` last_status=refused_spent, inserted=0; node.log `L1 refused:
coins_applied_height=3151412 > hstar_cursor=3056759` repeats 1,186+. The family
ENGAGES (`refused_coin_tear=TRUE`) but TERMINALLY REFUSES (`txindex_miss` latched,
node.db txindex tops at 3,151,411, the unfolded gap [3,056,759..3,150,899] has no
fold rows to source coins from). So it RUNS-ATTEMPTS-REFUSES, it does not REMEDY —
the true remedy is the offline from-checkpoint fold. The UTXO-set wrong-fork
corruption is bounded at **exactly 1 coin** (02663FF1 at h=3,151,306, two methods),
so a targeted 1-coin backfill is a viable *liveness* band-aid but leaves the deep
lie (H*=3,056,759 ≪ served 3,151,411) intact.

### Numbers (22 files / 11,262 LOC audited)

| Class | Files | LOC | Meaning |
|-------|------:|----:|---------|
| **Cleanly deletable post-cure** | 8 | 5,895 | Every one pivots on `refused_coin_tear` / `prevout_unresolved`-hole / `value_overflow`-row-below-cursor — states only reachable because import stamped cursors past unfolded logs. The cure makes them impossible **by construction**. |
| **Needs rework (not delete)** | 1 | 799 | `stage_repair_reducer_frontier.c` — the orchestrator. Its tear pivot dies; its reorg/flag-reconcile guts must be re-homed. |
| **Genuine support — KEEP** | 13 | 4,568 | Reorg unwind, header-solution cache/poison-rewind, body-fetch self-heal, legacy/utxo mirror + oracle drift detectors, wallet/explorer read-model feeder. NOT lie-cover. |

The 8 cleanly deletable (post-cure): `stage_repair_reducer_frontier_coin.c` (787),
`_refill.c` (743), `_tipfin.c` (650), `stage_repair_coin_backfill.c` (795),
`_scan.c` (539), `_util.c` (483), `utxo_apply_delta_repair.c` (456),
`quorum_oracle_service.c` (442). Plus `block_index_loader_torn_gate.c` (orphaned by
removing `coin_backfill_util` — forward-declares util symbols, won't link alone;
delete in the same change with its call site `block_index_loader_rebuild.c:596`).

### Deletion order (leaves before orchestrators, tree always links)

1. **`utxo_apply_delta_repair.c`** (456) — leaf. Sever caller
   `stage_repair_reducer_frontier_coin.c:533` (`maybe_repair_value_overflow`);
   delete `maybe_repair_value_overflow` (coin.c:489-550) + dispatch (coin.c:724-736),
   the `value_overflow_repair_*` fields in `struct
   stage_reducer_frontier_reconcile_result` (`stage_repair.h:122-129`) and their
   reads (`reducer_frontier_reconcile_light.c:366-367,507-511`;
   `stage_repair_reducer_frontier.c:503-504`). **KEEP** `utxo_apply_delta_reorg.c`'s
   primitives (`emit_inverse_delta`/`delete_rows_above`/`unwind_write_cursor`) —
   shared with the LIVE reorg path `utxo_apply_stage.c:703`.
2. **`stage_repair_coin_backfill_scan.c`** (539) — sever callers in
   `stage_repair_coin_backfill.c:280,295,650,659`.
3. **`stage_repair_coin_backfill.c`** (795) — sever sole external caller
   `stage_repair_reducer_frontier_coin.c:748`.
4. **`stage_repair_coin_backfill_util.c`** (483) — sever
   `block_index_loader_torn_gate.c:108/111/113` (fwd-decls) + `:156/167/169`; remove
   torn-gate call site `block_index_loader_rebuild.c:596`; remove MCP `zcl_state
   subsystem=coin_backfill` row `diagnostics_registry.c:512`; delete
   `block_index_loader_torn_gate.c` itself.
5. **`stage_repair_reducer_frontier_coin.c`** (787) — sever sole caller
   `stage_repair_reducer_frontier.c:625`. Callees now gone, deletes cleanly.
6. **`stage_repair_reducer_frontier_tipfin.c`** (650) — sever tear-gated caller
   `stage_repair_reducer_frontier.c:650` (if-block 644-658).
7. **`stage_repair_reducer_frontier_refill.c`** (743) — sever tear-gated callers
   `stage_repair_reducer_frontier.c:646` + `:661`. See blocker #2:
   `force_stage_cursor_in_tx` (:452) + `reconcile_refill_cursors` (:709) are UNGATED;
   re-home a reorg-residue cursor-clamp into the KEEP purge TU first.
8. **`quorum_oracle_service.c`** (442) — independent. Sever dead
   `rolling_anchor_service.c:384` ref + the two repair-only callers via
   `process_block_revalidate.c:124` (`block_failed_mask_at_tip.c:117` +
   `chain_supervisor.c:83`); remove MCP `zcl_state subsystem=quorum_oracle` row
   `diagnostics_registry.c:514`. `quorum_oracle_init` is never started in prod.
9. **REWORK `stage_repair_reducer_frontier.c`** (799) — LAST. Remove the dead tear
   branch (295-296 pivot, 644-703 dispatch, 234-296 `read_frontier_snapshot`
   `utxo_apply_contig`). **Preserve / re-home** the genuine self-heal:
   `reconcile_block_index_flags` (311-403, HAVE_DATA/VALID_SCRIPTS/FAILED_MASK), purge
   calls into the KEEP file `stage_repair_reducer_frontier_purge.c`
   (`purge_noncanonical` 580, `purge_stale_reorg_tipfin` 598),
   `reconcile_refill_cursors` (709) + `reconcile_tip_finalize_cursor` (712). Rewrite
   the self-heal Condition `reducer_frontier_reconcile_light`
   (`condition_registry.c:59`) to keep only the reorg-residue/flag-reconcile remedy,
   not the tear remedy.

### Blockers (must clear in this order)

1. **CURE FIRST — precondition for ALL deletes.** Wire `snapshot_verify.c`
   (FlyClient+MMB+SHA3) into the cold-import door and fold anchor→tip, deriving stage
   cursors honestly; remove the synthetic stamp (`block_index_loader_rebuild.c:728,743`,
   `stage_anchor.c:153-154 seed_exempt`). Until this lands, `refused_coin_tear` /
   `prevout_unresolved` holes are REACHABLE.
2. **REORG-RESIDUE RE-HOME** (blocks step 7 + 9): `reconcile_refill_cursors` +
   `force_stage_cursor_in_tx` run UNGATED and consume holes from the genuine
   reorg-residue purge (KEEP). A replacement cursor-clamp must live in the kept
   purge/reconcile TU before `refill.c` is removed, or depth-N reorgs lose their clamp
   consumer.
3. **TORN-GATE COUPLING** (blocks step 4): delete `block_index_loader_torn_gate.c`
   together with `coin_backfill_util` + its call site, or the tree won't link.
4. **CONDITION DEREGISTRATION** (blocks steps 5-9): the Condition
   `reducer_frontier_reconcile_light` (`condition_registry.c:59`) is the sole live
   entry into the whole reducer-frontier repair family — deregister or rewrite it,
   never leave it dangling. Same for the quorum path (`block_failed_mask_at_tip` +
   `chain_supervisor` coord_escalation).

### Zero-feature-loss verdict: TRUE (verified)

No wallet, explorer, mempool, P2P, ZNAM, ZMSG, ZSWP/market, RPC, or block-serving
feature routes through any deletable file. The only losses: (1) the cold-import
coin-tear/log-hole self-heal slice the cure makes unreachable; (2) two read-only MCP
`zcl_state` diagnostic subsystems (`coin_backfill`, `quorum_oracle`) — cosmetic.

**KEPT (look adjacent, survive the cure untouched):** reorg handling
(`utxo_apply_delta_reorg.c` live at `utxo_apply_stage.c:703`,
`invalidateblock`/`zcl_invalidateblock`; reorg-residue purge in
`stage_repair_reducer_frontier_purge.c`; header-solution-poison rewind
`stage_repair_rewind.c` serving `zcl_rebuild_recent`), header PoW re-source
(`stage_repair_header_solution.c`), body recovery (`stage_repair_body_fetch.c`), the
explorer/wallet UTXO read-model feeder (`utxo_mirror_sync_service.c`), all wallet HTML
UI (`wallet_view_projection.c`), and the legacy/utxo mirror + `oracle_policy` +
`zclassicd_oracle` + `mirror_divergence_locator` drift detectors.
