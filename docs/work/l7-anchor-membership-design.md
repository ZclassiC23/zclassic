# L7 — close the shielded anchor-membership loosening (design)

> Parity finding L7 (`docs/work/archive/parity-audit-round2-findings.md`): zcl23 accepts a
> JoinSplit/Spend referencing a note-commitment-tree anchor that never existed in
> chain history. `lib/coins/src/coins_view.c:476` is a literal `return true`
> stub; `sapling_anchors` is written but never read to reject. zclassicd rejects a
> forged anchor via `GetSproutAnchorAt`/`GetSaplingAnchorAt` →
> `bad-txns-joinsplit-requirements-not-met` (DoS 100). **Parity-RESTORING.**
> Full raw design: workflow `l7-anchor-membership-design` (run `wf_c2719dd7-81d`).

## How zclassicd does it (the parity target)
Anchors (Sprout & Sapling post-block tree roots) are stored in the LevelDB chainstate
keyed by the **root hash itself** (prefix `'A'` Sprout, `'Z'` Sapling, `txdb.cpp:23-24`).
Written per-block at connect: `ConnectBlock` appends each note commitment into a scratch
tree and `PushAnchor`s the post-block root (`main.cpp:2703-2720`). `Get*AnchorAt` returns
**false on unknown root** (empty_root short-circuits true). `HaveShieldedRequirements`
(`coins.cpp:565-606`) rejects any spend whose anchor doesn't resolve. **No height
watermark** — any historical tip root on the active chain is valid; rows erased only on
reorg (`BatchWriteAnchors`, `txdb.cpp:144-152`).

## The zcl23 gap
The live reducer fold (`utxo_apply_stage.c`/`utxo_apply_delta.c`) maintains **no**
note-commitment tree and writes **zero** anchor rows — only the `nullifiers` table is
touched. The Sapling tree exists **only** in a standalone replay
(`sync_controller_sapling_tree.c`, for wallet witnesses / the off-by-default
`-enforce-sapling-root`), tracks only the **frontier** (not the set of historical roots),
and there is no runtime Sprout tree. So there is no set to query — hence the stub.

## Design — mirror the existing nullifier-gap subsystem (C-3)
This is a **second instance of an existing pattern**, not new machinery. The predicate
can land first; full coverage is gated on a from-genesis replay (same cure the nullifier
gap waits on). **Fail-OPEN below a completeness watermark — a reject requires positive
proof of completeness, so a legitimate historical spend is never false-rejected.**

| # | Step | Risk |
|---|------|------|
| 1 | `storage/anchor_kv.{c,h}` mirroring `nullifier_kv` (per-pool keyspace; `sapling_anchors` exists, add `sprout_anchors` same shape; `anchor_kv_add/have/delete_range`) | consensus-adjacent |
| 2 | Inline fold writer `app/jobs/src/utxo_apply_anchors.c` (new file — `utxo_apply_stage.c` is ~975 lines, at the E1 ceiling): maintain both incremental trees inline, append this block's `cm`s, compute root, `anchor_kv_add` in the **same stage txn** — mirrors `PushAnchor`. Subsumes the standalone `sapling_tree_rebuild`. | consensus-adjacent |
| 3 | Completeness watermark `anchor_kv.complete_through` (mirror `nullifier_kv.activation_cursor`): stamped = applied height on cold-sync/refold (empty below it); advances only as the writer commits each block's root contiguously; from-genesis replay leaves it 0. | app-only |
| 4 | Replace the `coins_view.c:476` stub with the real per-pool membership probe (Sprout intra-tx `intermediates` + `anchor_kv_have`; Sapling `anchor_kv_have`; empty_root accepts with no lookup). **Gate: enforce only when the height is provably ≤ `complete_through`, else accept (fail-open).** Reject branch already exists at `connect_block.c:477-484`. | consensus-adjacent |
| 5 | Wire the same check into the **reducer** fold (the live path) alongside `utxo_apply_check_and_insert_nullifiers`, with a **distinct** `failure_kind` so the anchor arm is separable from the nullifier double-spend arm (both emit the same string). | consensus-adjacent |
| 6 | Permanent gap blocker `utxo_apply.anchor_backfill_gap` (mirror `nullifier_backfill_gap`): `complete_through>0` → blocker naming the under-enforced height; `==0` clears. Halts honestly about the gap. | app-only |
| 7 | Rewind invariant: `anchor_kv_delete_range` over the rewound `[first..last]` in the same rewind txn; clamp `complete_through` down to the rewind floor (couples to step 3). | consensus-adjacent |
| 8 | From-anchor refold / from-genesis reset rebuild the set: add `sapling_anchors`+`sprout_anchors` to `k_refold_tables`; set `complete_through`=anchor (from-anchor) or 0 (from-genesis). **The fold-forward cure coupling.** | consensus-adjacent |
| 9 | Parity test group: positive (real historical anchor accepted) + negative (forged anchor rejected from the **anchor arm**, unrevealed-nullifier fixture to dodge the string collision). | app-only |

## Risks (the load-bearing ones)
1. **False-reject during IBD/refold = a liveness WEDGE** (the dangerous mode). Airtight mitigation: enforce only when provably within `[.. complete_through]`, which advances only as the inline writer commits each root in the same txn; default direction is ACCEPT.
2. **Watermark staleness across reorg** → clamp `complete_through` down in the same rewind txn that deletes rows.
3. **Frontier persistence drift = silent divergence** → cross-check `incremental_tree_root()` vs the block's `hashFinalSaplingRoot` every block; on mismatch don't write the row, don't advance, raise a blocker (never reject).
4. **String collision** masks a dead test → distinct anchor-arm `failure_kind` + unrevealed-nullifier negative fixture.
5. **Sprout arm load-bearing-ness unverified** (does ZClassic history contain Sprout JoinSplits?) → implement intra-tx intermediates anyway; validate against the real chain before enabling the Sprout reject.
6. **Per-block hot-fold cost** → only fold the root for a pool that changed this block; the deleted standalone `sapling_tree_rebuild` offsets it.
7. **Enabling before full-history replay = fork risk wrong-direction** → seed the anchor-height frontier from the **same verified source as the coin set** (extend the minted snapshot to carry the Sapling/Sprout frontier at the anchor) — never enforce on a borrowed/unverified frontier.

## Disposition
- **Couples to the keystone cure:** the anchor frontier at the anchor height must come from the verified snapshot (the mint must be extended to carry it), and full coverage needs the from-genesis/from-anchor refold that this design hooks into (step 8). So L7 lands **with** the fold-forward cure, not before it.
- Ships **fail-open + honest blocker** — safe to land incrementally (enforces the post-anchor suffix; names the gap below).
- Copy-prove gate: H\* climb + the exact-string reject **from the anchor arm** (not "booted without FATAL").
