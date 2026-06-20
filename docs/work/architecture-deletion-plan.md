# Architecture tree + deletion plan (less is more)

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
