# The sync organism — the whole picture, grounded in code

> Built 2026-06-19 from a 7-lens code map (docs treated as suspect, every claim
> file:line-anchored). This is a CODE MAP, not a live runtime diagnosis —
> re-verify line numbers and live state before acting; specifics rot.

## The one-paragraph truth

In **steady state the node is already ~90% the beautiful thing**: there is ONE
UTXO authority — `coins_kv` (the `coins` table inside `progress.kv`) — written by
exactly ONE live engine, the staged pipeline's `utxo_apply_stage.c`, folding block
bodies forward and co-committing the coin write + stage cursor + `*_log` row in a
single `BEGIN IMMEDIATE` txn. The refusal function `reducer_frontier.c` is provably
SELECT-only: H\* = MIN over each stage log's contiguous `ok=1` prefix; it refuses to
guess. **Keep that core forever.** Every wound is elsewhere.

## Wound 1 — the borrowed foundation (the root of every wedge)

The node does not fold its starting UTXO set from genesis. On a normal boot it
**copies** zclassicd's set and launders it into the single authority:

1. `utxo_recovery_import_ldb` reads zclassicd's chainstate LevelDB
   (`$datadir/chainstate`, else `~/.zclassic/chainstate`) → node.db `utxos` mirror.
   *(config/src/boot.c:2480-2491; utxo_recovery_restore.c:137,141-146)*
2. `coins_kv_seed_from_node_db` COPIES that mirror INTO canonical `coins_kv`.
   *(utxo_recovery_restore.c:369)*
3. Crypto is honest at EXACTLY ONE compiled height (SHA3 checkpoint h=3,056,758,
   `checkpoints.c:86-104`). At any other height the SHA3 stamp is deliberately NOT
   written and the only gate is the row-count heuristic `imported_count > 100000`.
   *(utxo_recovery_restore.c:318-326,391)*
4. `block_index_loader_seed_stages_from_cold_import` then STAMPS the state finalized
   without folding a body: drives `tip_finalize` + 7 upstream cursors via
   `tip_finalize_stage_seed_anchor(H, hash, trusted_seed=true)`.
   *(block_index_loader_rebuild.c:728-743; boot_services.c:1471)*

So the single authority's **initial contents are laundered from zclassicd**, checked
at one self-chosen height (circular self-cert), and stamped forward. A wrong-fork /
orphan coin rides in with the copy; later the honest fold can't re-derive it; it
refuses; **that refusal is the wedge.** Chasing the individual coin is chasing a
symptom — the bug is that a UTXO set is allowed to *exist that was never folded*.

## Wound 2 — the served tip ≠ the provable tip

There are THREE competing notions of "how far am I":
- `active_chain_height` — what `getblockcount` and P2P `start_height` SERVE and
  ADVERTISE. *(blockchain_controller_blocks.c:39; msg_version.c:148)*
- `H*` — the provable fold (`reducer_frontier.c`).
- `coins_applied_height` — coins_kv's co-committed cursor.

**Nothing caps the served/advertised height down to H\*.** So the node can advertise
blocks it cannot fold-prove. The HOLD latch (`chain_linkage_check.c:264-281`) and the
per-case patches in `chain_state_validator` exist to reconcile this divergence — they
are scaffolding around wound 2.

## The stores and engines (why it LOOKS complicated)

- **3 physical UTXO stores:** `coins_kv`/`coins` in progress.kv (THE authority);
  node.db `utxos` (a declared rebuildable PROJECTION — `coins_view_sqlite.c:5-7`,
  its only forward-writer is dead); `utxo` in utxo_projection.db (a seed conduit).
- **4 compiled engines that can author a UTXO set:** (1) staged `utxo_apply_stage`
  → coins_kv [LIVE, sole live author]; (2) legacy `connect_block`/`process_block`
  flush → node.db `utxos` [DEAD — `process_block_flush_if_needed` has zero callers;
  `connect_block()` only via `-reindex-chainstate`, boot_index.c:334]; (3)
  `reindex_epilogue` reseeds coins_kv from the connect_block-rebuilt mirror
  [RECOVERY-only, circular self-cert]; (4) the P2P snapshot path
  [REACTIVE — fires only on a NODE_ZCL23 peer offer, ~never on a zclassicd network;
  verifies received bytes against the PEER's self-reported root, not the checkpoint —
  snapshot_verify.c:131-132].

Almost all of this is **scaffolding propping up the borrowed foundation.** Remove the
borrowing and ~12 subsystems become deletable.

## The beautiful target (one breath)

ONE store (coins_kv), ONE producer (fold bodies forward), **served tip == provable
tip**, and fast-sync is a snapshot whose UTXO root is verified against the **proven
PoW chain** — never a neighbor's word. Then the node cannot lie and cannot silently
wedge, by construction.

## The subtraction plan (delete to reach the target)

1. Make `-refold-staged` (bodies-only refold; boot.c:3312 truncates coins_kv WITHOUT
   reseeding from the mirror, forces the 8 cursors to genesis) the **DEFAULT
   cold-start.** Highest-leverage move — severs the borrowed foundation.
2. DELETE the cold-import LevelDB ladder: `utxo_recovery_import_ldb`,
   `utxo_recovery_ldb_copy.c`, `utxo_recovery_seed_provenance.c`,
   `utxo_recovery_mirror_walk.c`; remove boot.c:2480-2491.
3. DELETE both mirror→canonical seed copiers: `coins_kv_boot_rebuild_if_needed`
   AND `coins_kv_seed_from_node_db` (coins_kv_boot_rebuild.c); remove
   boot_projections.c:109.
4. DELETE the stamp-forward consumer: `block_index_loader_seed_stages_from_cold_import`
   (block_index_loader_rebuild.c:478-768) + boot_services.c:1471.
5. DELETE the dead legacy applier: `connect_block.c` connect-half,
   `process_block_flush_policy.c`, the `-reindex-chainstate` verb + `reindex_epilogue.c`.
6. DELETE the third store (utxo_projection.db `utxo` as a coins_kv conduit).
7. RETIRE the legacy node.db coins view `g_coins_sqlite` (boot.c:131,135,1666) +
   chain_state_validator Case-3a/3b AGREE special-cases.
8. COLLAPSE the served tip into H\*: cap getblockcount + P2P start_height to H\*.
9. REPLACE the hardcoded SHA3 checkpoint with a utxo_root bound INTO the proven chain;
   fix snapshot_verify.c:131-132 to check against THAT, not the peer's offered root
   (HANDOFF.md #1). Add a "forged self-root must REJECT" test.
10. DELETE the dead Commitment MMR ladder (mmr.c commitment half, getmmrroot RPC,
    blockchain_controller.c:201-280 weak-XOR leaf) unless the block-MMR becomes the
    step-9 anchor.
11. DELETE CLI side doors: `--repair` (injects zclassicd RPC rows into utxos),
    `--importchainstate` (src/main.c:1008-1290).
12. UNIFY the twice-transcribed equihash header serialization (check_block.c:96-102
    vs domain/consensus/equihash.c:72-86) onto one shared serializer.

## The performance path (what makes sync FAST)

1. **Height-gate the staged crypto stages.** Full ECDSA + Sapling/Sprout Groth16 run
   on EVERY historical block with NO gate (`script_validate_stage.c:294`,
   `proof_validate_stage.c:323`; the deferred-proof optimization exists ONLY in the
   dead connect_block path). Wire `g_deferred_proof_validation_below_height` into the
   staged stages (defer, re-verify in bg). **Consensus-sensitive — replay full chain
   first (h=478544 lesson).**
2. **Kill the per-block fsync storm.** progress.db is `synchronous=NORMAL`
   (progress_store.c:60) and each stage wraps EVERY block in its own
   BEGIN IMMEDIATE/COMMIT (~5+ fsync commits/block). Batch one txn per stage per tick;
   `synchronous=OFF` during IBD like node.db.
3. **Parallelize + stop the 4× re-decode.** script_validate is single-threaded;
   body_persist/script/proof/utxo_apply each re-read+re-decode the SAME body. Hand one
   decoded block down the pipeline; move connect_block's workpool into the stage; raise
   the batch-100 / 2s-tick cap (~50 blk/s ≈ 17h for 3M blocks today).

## The keystone experiment (turns story → proof)

**Nobody has ever run the bodies-only refold to the tip.** That single run answers
the only question that matters: *can the node fold its own UTXO set genesis→tip to
zclassicd's exact tip, with zero borrowing?*
- YES → the cure is real; the 12 deletions are safe.
- NO → the next real bug is found loudly, on a copy, at an exact failing block.

Gold standard: refolded tip block hash + UTXO commitment match the chain / zclassicd.
Copy-prove on a frozen fixture; never live surgery; do NOT delete the recovery crutch
until the cure is proven hash-identical.

## Honest risks (do not skip)

- ~60s fast-sync is aspirational; proven recovery is the 2-step `--importblockindex`
  + boot (~25 min) = the borrowed crutch. Bodies-only refold-to-tip is UNMEASURED.
- Proof-gating is consensus-sensitive; full-chain replay required before enabling.
- "Cannot lie" is NOT yet true: until the utxo_root is bound into the proven chain
  (step 9), the fold still trusts the borrowed set.
- Lint gates are fail-silent — deletions must remove the WIRING, not just call sites.
- Dead engines are still COMPILED + wired (`set_coins_sqlite_for_commitment`,
  boot.c:1790); if reactivated they author a different store.
