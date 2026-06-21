# Why zclassic23 wedges on forward sync when zclassicd does not — root-cause dissection (2026-06-21)

> Deep parallel dissection (8 readers → synthesis → 4 adversarial refuters), live-verified
> against the wedged node and the zclassicd C++ reference (`/home/rhett/github/zclassic/src`).
> Raw workflow run `wf_151dc753-a03`. This CONFIRMS + SHARPENS the long-standing borrowed-seed
> hypothesis with live row counts, elevates the deeper architectural root, and corrects the
> fix scope.

## TL;DR (plain)

The C node keeps **two** notions of "the tip": the coin set (`coins_kv`) and a **separate
per-height "certified log"** (`utxo_apply_log`) that a second cursor, **H\*** (`reducer_frontier`),
folds into a "provable tip." zclassicd has only **one** — the coins *are* the tip, advanced one
block at a time. On the live node the borrowed-seed cold import **bulk-copied** zclassicd's UTXO
set into `coins_kv` and **stamped the cursor forward to the import tip (3,150,489)** over a span
that has **no per-height log rows**, so H\* pins at the checkpoint anchor (3,056,758) and the node
refuses to finalize any block past it (`block-not-finalized-by-reducer`). The block at the hole
(3,056,759) is **valid and on disk** — the only thing missing is the evidence rows the seed
skipped. **The fix is to delete the borrowed copy and let one writer fold the blocks**, which is
exactly what zclassicd does and what the running from-genesis refold is proving.

## The exact wedge mechanism (live-verified)

```
coins_applied_height / utxo_apply cursor ... 3,150,489   ← coins folded this far
contiguous ok=1 log prefix (= H*) .......... 3,056,758   ← == compiled checkpoint anchor
first hole ................................. 3,056,759   = anchor+1
utxo_apply_log rows ........................ 2,132        over a 93,731-height span (SPARSE)
block 3,056,759 nStatus_raw ................ 29           HAVE_DATA|HAVE_UNDO|VALID_SCRIPTS, nTx=2
                                                          → valid body+undo on disk, on active chain
```

Causal chain:
1. Cold import bulk-copies the coin SET into `coins_kv` with **zero** per-height evidence rows —
   `lib/storage/src/coins_kv_boot_rebuild.c:158-163` (`INSERT OR REPLACE INTO coins SELECT … FROM coinssrc.utxos`).
2. It writes a **single** `'anchor',ok=1` row at the compiled checkpoint —
   `app/jobs/src/tip_finalize_anchor.c:265`.
3. It then stamps **all seven** upstream stage cursors **+** `coins_applied_height` forward to the
   import tip via the **`seed_exempt` early-return** that disables the log-frontier-cap guard
   (the "FIX-3" guard built to prevent exactly this) — `app/jobs/src/stage_anchor.c:153`
   (`if (seed_exempt) return true;`), threaded through all stages at `stage_anchor.c:288-292`,
   called with `seed_exempt=true` from `app/services/src/utxo_recovery_restore.c:240-242`.
4. `reducer_frontier` folds the contiguous `ok=1` prefix from the anchor
   (`app/jobs/src/reducer_frontier.c:198-249`), finds **no row at 3,056,759**, breaks → utxo_apply
   prefix = 3,056,758 → **H\* = MIN over six logs = 3,056,758**.
5. `tip_finalize` is gated on H\*, so it cannot finalize past 3,056,758 → every new block is rejected
   `block-not-finalized-by-reducer`; `validation_pack` I4.3 `window.consistency` raises `operator_needed`.

**Correction from the adversarial pass:** it is *not* "only one log row" and *not* only `utxo_apply_log`.
Live shows 2,132 sparse rows, and the **first** refusal actually binds on `validate_headers_log`
(`guard=G3_missing_evidence binding_log=validate_headers_log height=3056759`) — the **whole 5-log
upstream chain** (validate_headers, body_persist, script_validate, proof_validate, utxo_apply) is
rowless across the span. The gap is "rows absent across the span + cursor stamped above them," so the
cure must reset **all** upstream cursors to the anchor, not rewrite one log.

## Why zclassicd structurally cannot wedge here

ONE UTXO writer, ONE cursor, the **same object**, advanced together per block:
- `UpdateCoins` in `ConnectBlock` writes coins into the transient `CCoinsViewCache`; that view's
  best-block is set inside `ConnectBlock` (`main.cpp:2793`); flushed to `pcoinsTip`
  (`assert(view.Flush())`, `main.cpp:3412`); `chainActive` advanced in the same step
  (`UpdateTip`, `main.cpp:3429`); every dirty coin **and** `DB_BEST_BLOCK` committed in **one**
  atomic LevelDB `CDBBatch` (`txdb.cpp:168-198`, `DB_BEST_BLOCK` at `:191`).
- `ConnectBlock` refuses any block whose parent ≠ `view.GetBestBlock()` (`assert`, `main.cpp:2550`),
  so coins advance strictly one height at a time in chain order — **a hole below the cursor is
  structurally impossible to represent.**
- There is no second "certified log," no contiguity fold, and zclassicd never imports a foreign
  chainstate mid-chain and folds forward — every coin it holds passed that per-block assert.

`getblockcount == chainActive.Height() == pcoinsTip best`, always. The three things that wedge the
C node — a second cursor that can lag, a tearable second evidence record, and a foreign snapshot
seeded without per-block provenance — **do not exist** in zclassicd's model.

## Is it one bug or architectural? — ARCHITECTURAL

The deepest root (cause #3, elevated on refutation) is the **decoupled second authority**: H\* gates
*finalization* on a per-height log fold that is separate from the single coins writer
(`reducer_frontier.c:483-604`). Because `coins_applied_height` can be advanced by **bulk paths that
bypass the per-height co-commit** (`coins_kv_boot_rebuild.c:158-164`; cursor stamps at
`stage_anchor.c:300` and `block_index_loader_rebuild.c:518`; the `seed_exempt` bypass at
`stage_anchor.c:153`), the certified log can lag the coins by 93,731 heights, and the decoupled
authority converts that into an **operator_needed HALT** instead of a self-healing refold.

The same design yields **four** distinct failure surfaces, all confirmed:
- **#1/#2** the borrowed-seed log hole (the live pin).
- **#4** an independent `prevout_unresolved` at the forward edge: a coin spent at 3,150,489
  (tx `5711965c…`, vin 27) is a prevout the at-tip-**unspent** snapshot never contained, so
  `script_validate` cannot resolve it.
- **#5** no automatic recovery: L1 `reducer_frontier_reconcile_light` can only **refuse**
  (`coins_applied_height=3150489 > hstar_cursor=3056759 (L2 required)`,
  `stage_repair_reducer_frontier.c:694`) and hands off to an "L2" that is owner-gated and never runs;
  `tipfin_backfill` writes only `tip_finalize_log` and refuses at G3; the replay-repairs only handle
  single-hole classes. **Nothing rebuilds a multi-log rowless span.**

## Refutation verdicts

| Lens | Verdict |
|---|---|
| would-zclassicd-also-wedge | **stands** — no equivalent moment exists in zclassicd; live state strengthens the cause |
| is-the-fix-actually-a-fix | **partial** — mechanism + cure confirmed; corrected scope (refold ALL upstream logs, not just utxo_apply_log) and the "one row" wording |
| is-it-really-the-seed | **stands** — the "forward fold drops rows without a seed" alternative is refuted by code + live counters |
| wrong-altitude | **partial** — #1's mechanism is right but mis-ranked above its deeper root #3 (the decoupled second authority) |

## The fix — delete the divergent complexity (all 5 causes: `delete-complexity`)

1. **Delete the borrowed-seed bulk-copy** (`coins_kv_seed_from_node_db`, `coins_kv_boot_rebuild.c:158-163`)
   and the **`seed_exempt` bypass** (`stage_anchor.c:153` + the `frontier_exempt=true` call at
   `utxo_recovery_restore.c:240-242`).
2. **Make a bodies-only refold the default cold-start** — reset all upstream cursors to the verified
   anchor and `coins_applied=anchor+1`, then let the **single live fold** re-apply blocks so the SAME
   atomic step (`utxo_apply_stage.c:520,548-563`) co-commits `coins_kv` + `coins_applied_height` + the
   per-height `ok=1` row across **every** log. This is already implemented correctly by
   `-refold-from-anchor` / `boot_refold_staged.c:419-426`; make it the default, not an opt-in repair.
3. **Enforce the invariant:** forbid ANY path (seed, restore, reindex, cursor-stamp) from advancing
   `coins_applied_height` without the matching co-committed log row — collapsing the second writer
   **and** the second authority to zclassicd's one-writer model. Then `coins applied == finalizable`.
4. **Delete what becomes dead:** the L1 tear branch, the never-built L2, the torn-import detector, the
   owner-ack coin_backfill gate, and the `prevout_unresolved` edge case all dissolve once the seed path
   is gone.

**Empirical proof in flight:** the from-genesis refold running on a datadir copy is a *superset* of the
anchor refold (anchor is just a non-genesis start). The refutation confirms it is **a valid existence
proof, not a dodge** — if it climbs H\* to tip, one-writer fold yields the contiguous prefix and the
wedge class is gone.

## What still needs confirmation

- The running from-genesis fold reaching tip (H\* climbs anchor→3,150,488) — the live cure proof.
- That deleting the seed path + defaulting to refold does not regress the documented fast-cold-start
  goal (the refold is ~tens of minutes; acceptable for a recovery/cold path, must not become every-boot).
- Mapping every `coins_applied_height` writer to verify the invariant in fix-step 3 is complete
  (seed, `block_index_loader_rebuild.c:518`, `stage_anchor.c:300`, reindex).
