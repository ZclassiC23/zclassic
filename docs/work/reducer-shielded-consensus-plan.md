# Reducer shielded-consensus enforcement — design (NOT yet implementation-ready)

> **Status: DESIGN, NOT implementation-ready.** Architecture is sound but
> consensus-exactness is not: all 3 reviewers returned `consensus_safe=false`.
> The three §8 gaps MUST close before any code. The enforce flip is
> owner-gated and copy-proved on a datadir COPY, never deployed blind.

## 1. The gap

The live-tip reducer (`body_persist → script_validate → proof_validate →
utxo_apply → tip_finalize`) **never calls `connect_block`**
(`lib/validation/src/connect_block.c`), the only site with shielded-consensus
enforcement. `coins_view_cache_have_joinsplit_requirements`
(`lib/coins/src/coins_view.c:477-483`) is a hardcoded `return true;` called from
`connect_block.c:406`, which the reducer never reaches.
`proof_validate_stage.c` only verifies the zk-SNARK against the anchor the tx
*claims* (`sd->anchor` at :230, `js->anchor` at :273/:284) — it never checks:

- **(a) nullifier double-spend** (Sapling `sd->nullifier`, Sprout `js->nullifiers[0..1]`),
- **(b) anchor membership** (is the claimed treestate root real?),
- **(c) ZIP-209 turnstile** (`nChainSproutValue`/`nChainSaplingValue` must never go negative).

So a forward-path block that double-spends a shielded nullifier, proves against
a fabricated anchor, or drives a pool negative **is finalized → inflation**.

## 2. Load-bearing atomicity fact

The reducer's atomic unit is **`progress.kv`** (`progress_store.c:28`), written
under `progress_store_tx_lock()` + `stage_run_once` `BEGIN IMMEDIATE`
(`utxo_apply_stage.c:355-365`). `utxo_apply_log` / `utxo_apply_delta` /
`stage_cursor` all live there. The pre-existing `sapling_nullifiers` /
`sapling_anchors` tables (`database_schema.c:90-97`) live in **node.db — a
different sqlite handle**, and are wallet/index-scoped (written post-finalize by
`tip_finalize_post_step.c:99-101` + sync_controller).

**Putting the consensus gate in node.db splits the atom**: a crash between the
progress.kv cursor commit and the node.db insert desyncs the set from the
cursor. ∴ the new **consensus** shielded set MUST live in progress.kv beside the
delta. Do NOT reuse the node.db wallet/index tables for the gate.

## 3. New consensus state — four progress.kv tables

Created by a new `ensure_shielded_schema()` (called from `utxo_apply_stage_init`
beside `ensure_log_schema`/`ensure_delta_schema`):

1. `shielded_nullifiers(nf BLOB, pool INT, height INT, PK(nf,pool))` — the
   consensus spent-nullifier set (pool 0=sprout, 1=sapling). Permanent, never
   pruned (like `utxos`).
2. `shielded_anchors(root BLOB, pool INT, height INT, PK(root,pool))` — known
   historical treestate roots; membership = anchor validity. Pre-seed at genesis
   with the empty-tree root per pool. Never pruned. **(see gap §8.1/§8.2)**
3. `shielded_chain_value(height INT PK, sprout_total INT, sapling_total INT)` —
   per-height cumulative pool totals (the persisted equivalent of the dead
   `block_index nChainSprout/SaplingValue`). Pruned with `utxo_apply_delta`.
4. `shielded_apply_delta(height INT PK, branch_hash BLOB, nf_added_blob BLOB,
   anchor_added_blob BLOB, prev_sprout_total INT, prev_sapling_total INT)` — the
   per-block INVERSE record, **same shape/keying as `utxo_apply_delta`**
   (`branch_hash = bi->phashBlock`, the same value used at
   `utxo_apply_stage.c:252`). Pruned below the finality floor with
   `utxo_apply_delta`.

## 4. Single enforcement point

A new sibling pass `utxo_apply_check_shielded(db, &blk, next_h, bi, &res)` called
inside `step_apply` (`utxo_apply_stage.c`) **after** the transparent delta
succeeds (after the `if (summary.ok)` block at :251) and **before**
`utxo_apply_delta_persist` at :252 — inside the same stage txn. Keep
`utxo_apply_compute_block_delta` transparent-only. Per tx, block order
(mirroring connect_block):

- **A. NULLIFIER** (replaces the `coins_view.c:482` stub): reject if any
  `sd->nullifier`/`js->nullifiers[i]` is already in `shielded_nullifiers` OR seen
  earlier in this block (intra-block dup, mirroring
  `utxo_apply_delta.c:166`). On pass, stage `(nf,pool)` for insert.
- **B. ANCHOR**: reject unless `sd->anchor`/`js->anchor` ∈ `shielded_anchors`.
  proof_validate already proved the spend *against* that anchor — this closes
  "is the anchor real?".
- **C. TURNSTILE**: verbatim port of `connect_block.c:175-217`
  (`sprout_value += vpub_old - vpub_new`, `sapling_value += value_balance`),
  new totals = prev + delta, **reject if either pool total < 0**.

A reject sets `summary.ok=false` → `upstream_failed` → `tip_finalize` does not
finalize (the existing blocked-cursor mechanism; no new reject plumbing).

## 5. Reorg insert/unwind symmetry

Ride `utxo_apply_reorg_unwind_if_needed` (`utxo_apply_delta_reorg.c:245-394`)
verbatim — no new fork-decision logic (same `branch_hash`, same
`reorg_is_allowed(C-1,fork)` finality gate at :336). Three reverse edits, all
inside the existing single atomic unwind txn (:366-384):

1. `emit_inverse_delta` (:122-192): add a third inverse block that reads
   `shielded_apply_delta[height]`, DELETEs each `nf_added_blob` nf from
   `shielded_nullifiers`, DELETEs each `anchor_added_blob` root from
   `shielded_anchors`, restores `shielded_chain_value` to `(prev_*)`.
2. `delete_rows_above` (:195-217): a third DELETE pass for `shielded_apply_delta`
   in `[fork+1, C-1]`.
3. Cursor rewind (:375) unchanged; step_apply re-applies the winning branch,
   re-inserting exactly what the winner spends/adds.

## 6. Commitment folding

New `shielded_commitment_sha3_compute(progress_db, out[32])` SHA3s the progress.kv
tables in canonical order (`shielded_nullifiers ORDER BY pool,nf`;
`shielded_anchors ORDER BY pool,root`; the committed `shielded_chain_value` row)
→ a `shielded_sha3`. Persist beside `utxo_sha3` at the same finalized-tip save
points (`blockchain_controller_chain.c:406`, `snapshot_apply.c:143`),
height-stamped identically. Extend `coins_reconcile_stale_anchor`
(`coins_view_sqlite.c:287`) to recompute-and-match `shielded_sha3` too, so the
commitment-validated self-heal and the parity service (C8) cover shielded
consensus state byte-for-byte. Kept separate from `utxo_sha3` (do not fold into
the single trusted UTXO commitment).

## 7. Sequencing

1. **After the live-wedge fix** — shadow mode needs a chain that finalizes
   forward so a log-only gate can ride a healthy tip and be diffed vs zclassicd.
   Adding a reject path while tip_finalize already can't advance is untestable
   and risks compounding the oscillation.
2. **After fork single-sourcing** — this rides `reorg_is_allowed` /
   `branch_hash` / `active_chain_at`; land after any chain[]-window/fork-decision
   unification so the reorg rail is stable.

## 8. OPEN fork-risk gaps — close these BEFORE coding

These are the three §8 gaps behind the `consensus_safe=false` verdict:

1. **Interstitial / intermediate anchors (Sprout).** A Sprout JoinSplit can
   anchor against a treestate that exists only *within* a tx/block (the tree is
   updated note-by-note; a later JoinSplit can anchor on a root including an
   earlier JoinSplit's commitments in the same tx). Growing `shielded_anchors`
   with only the **final per-block root** would false-reject valid history → a
   fork on the historical chain. The refinement must add intra-block/intra-tx
   intermediate roots to the accepted anchor set, matching zcashd ConnectBlock.
2. **Sprout vs Sapling asymmetry.** node.db has only `sapling_nullifiers` /
   `sapling_anchors` — **no Sprout nullifier or anchor table**, and the legacy
   tree is Sapling-only (`connect_block.c:80-84` has `g_sapling_tree`, no Sprout
   tree, and never appends). The Phase-4 "seed from legacy tree" plan has no
   Sprout source. The refinement must specify how Sprout anchors are
   built/seeded (the reducer owning the Sprout incremental tree from genesis).
3. **Reorg-correctness split under author gating.** Forward insert is
   author-gated (`utxo_apply_stage.c:242`, `author==UTXO_AUTHOR_STAGE`) but the
   consensus *check* must run regardless of authorship → in follower /
   `UTXO_AUTHOR_LEGACY` mode the forward add and reverse restore land on
   different gating boundaries, leaking a stale spent-nullifier. The refinement
   must run the gate unconditionally while keeping the persisted-set mutation
   consistent with the cursor in every author mode.

## 9. Phasing (after §8 closed)

- **Phase 0** — schema + `utxo_apply_check_shielded` in **shadow/log-only**
  (`g_shielded_enforce=false`): compute all three checks, on a would-be reject
  emit a diagnostic event + `_Atomic g_shielded_shadow_reject_total`, but keep
  `summary.ok=true` (block finalizes as today). Still INSERT the set + persist
  `shielded_apply_delta` (reorg-correct). Copy-proof: **zero** shadow-rejects on
  honest history; fixtures fire the counter.
- **Phase 1** — wire `shielded_commitment_sha3` + parity/self-heal coverage.
- **Phase 2** — flip to enforce (owner-gated, copy-proved vs zclassicd).
- **Phase 3+** — reducer fully owns the Sprout+Sapling incremental trees
  (anchor ownership, the largest step), lands last.

## 10. Test plan

New `lib/test/src/test_shielded_consensus_stage.c` (model on
`test_connect_block_self_write.c` + the reorg/self-heal tests), in-process with
injected reader/lookup:

1. **double-spend nullifier** (Sapling + Sprout, + intra-block dup) → `ok=0`,
   counter==1, tip_finalize writes `upstream_failed`, no finalize.
2. **fabricated anchor** (proof_validate passes against the claimed anchor, but
   anchor ∉ `shielded_anchors`) → reject.
3. **turnstile** (`value_balance` drives a pool negative) → reject.
4. **reorg symmetry**: forward-add then disconnect → assert the nullifier/root
   are removed and totals restored (no stale-spent leak), both author modes.
5. **copy-proof**: reducer over a datadir COPY snapshot→tip, assert
   `shielded_sha3` + `utxo_sha3` byte-match zclassicd `gettxoutsetinfo` + a
   nullifier-set digest, zero shadow-rejects on honest history.
