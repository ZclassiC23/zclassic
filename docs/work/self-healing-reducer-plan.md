# Self-healing reducer — epoch-aware rewind-to-clean-frontier + replay

> **STALE / SUPERSEDED (2026-06-07 plan).** L0 (`reducer_frontier_compute_hstar`)
> landed; the live wedge this targeted has since been re-root-caused (multiple
> times) to the **cold-import coin tear**, not a flag/log epoch drift. The whole
> layered L1/L2/L3 **repair-ladder is superseded by**:
> - tenacity-roadmap item 4 **`window_rebuild`** — "one recovery verb, recompute
>   never repair", which deletes this ladder; and
> - the **write-time import correctness gate** (SHA3 verify at import, not at
>   replay).
>
> **Kept below as durable lessons:** the H* definition and the REJECTED /
> RESOLUTION-as-unsafe warnings. Do NOT delete the unsafe-approach warnings —
> they are the load-bearing takeaway. The dense C1–C7 predicate and the
> L1/L2/L3 design are retained for historical reference only.

Forensics ground truth:
`memory/project_tipfinalize_precondition_desync_fix_2026-06-07.md`.

## The provably-consistent frontier H* (DURABLE definition — keep)
H* = the deepest height where ALL hold, computed from progress.kv + the coins
store ONLY, and NEVER from `g_last_advance_height` / cec int / served-tip /
`active_chain[]` (those are the drifted authorities — checked AGAINST H*, never
inputs to it):
- **C1** cursors cover H*: every `stage_cursor.cursor >= H*+1`.
- **C2** contiguous ok=1 prefix UPWARD from trusted_anchor (NOT from 0): every
  success log has a row, ok=1, for all `(trusted_anchor..H*]` (no hole, no ok=0).
- **C3** single-epoch / hash-agree (2-way at-height):
  `validate_headers_log[H].hash == script_validate_log[H].block_hash`; tip_finalize
  hash check is the LOOKAHEAD form folded into C4 (row H stores hash(H+1));
  exclude `is_anchor` rows (they store hash(H)); NULL hash = "no evidence", does
  NOT lower H* (cold-import prefix is healable).
- **C4** coins applied exactly to H*:
  `coins_frontier = MIN(height(coins_best_block HASH), MAX(coins.height))`;
  `tip_finalize_log[coins_frontier].tip_hash == coins_best_block` (lookahead form).
- **C5** block_index flags log-consistent over `[0..H*]`; a flag-only drift
  healable from the hash-bound log does NOT lower H*; a byteless hole (body_persist
  ok=1 but bytes absent, not under a trusted snapshot-anchor prefix) DOES.
- **C6** H* = MIN(all caps) — a PREFIX boundary, never a high-water island above a
  hole.
- **C7** the drifted authorities above are checked against H*, never fed into it.

**trusted_anchor** (sparse-log base): the commitment-verified base below which
coins+block_index are trusted WITHOUT stage-log rows = the SHA3 UTXO checkpoint
height (`get_sha3_utxo_checkpoint()->height = 3,056,758`), or a higher durably-stored
verified coins commitment. It is the FLOOR of H* (H* >= trusted_anchor always); the
C3 hash walk + C2 contiguity only run on `(trusted_anchor..upper]`. Without it,
"contiguous ok=1 prefix from 0" computes H* ≈ genesis on every fast-synced node
(the stage logs are SPARSE — import populates coins + block_index directly and
bypasses them).

**served_floor** = `MAX(height) FROM tip_finalize_log WHERE ok=1` — a SEPARATE
companion. Can be > H* (T9/T10). The public tip authority IS served_floor
(chainstate.c:535).

**Hard guard:** H* >= SHA3 UTXO checkpoint (3,056,758) — never rewind across
irreversible finality.

## REJECTED — FATAL flaws (DURABLE — never ship)
- bulk `DELETE FROM coins WHERE height > H*` (removes created, never restores
  spent → double-spend).
- tip_finalize_log "prefix-preserving relaxation" deleting rows above H* (collapses
  served_floor T9/T10).
- raw 3-way hash_cap `!=` (lookahead convention → H* near genesis → rewinds healthy
  nodes).
- certifying forward progress on a rewound coin set without a commitment
  recompute-match.
- **synthesizing utxo_apply ok=1 from coins presence** — FATALLY unsafe: presence ≠
  value-conservation. `added_blob` stores only txid|vout (no value → cannot recompute
  conservation); "prevout absent" cannot distinguish this-block-spent from
  later-block-spent → a window holding a double-spendable coin finalizes before the
  scalar `utxo_count_diverged` gate trips ("a double-spendable set looks fine by row
  counts", coins_view_sqlite.c:271). Coin trust REQUIRES a content-bearing SHA3
  commitment recompute-match, never a presence check. Re-validating script/proof from
  the present body is necessary but NOT sufficient (valid scripts can still
  double-spend).
- H* under-rewinds: `MAX(coins.height)` is the most-recent SURVIVING coin's creation
  height, NOT a contiguous applied frontier — a missing-fsync interior drop is
  invisible to it. (Superseded by the co-committed `coins_applied_height` cursor.)

## Durable-truth basis
- A stale ok=0 at height h WHERE coins for h exist = the LOG is wrong, not the chain
  — reconcile the log, don't rewind coins.
- tip_finalize SKIPS (marks upstream_failed + advances) on a stale ok=0 utxo_apply row
  (stage.c:343) — so a stale ok=0 makes a GOOD block permanently "failed", holing the
  finalized chain.
- Coins are the durable truth; logs/flags are drifted views. Above the last stored
  verified commitment (SHA3 checkpoint 3056758) coins are trusted-but-unverified
  (delta-internally-consistent, not reindex-proven).
- coin trust = commitment-match ONLY; presence-synthesis is gone.

---

## Layered L1/L2/L3 plan — SUPERSEDED by window_rebuild (historical)
Retained for reference; the recovery path is now the single `window_rebuild` verb
(recompute, never repair) + the write-time import gate.

- **L0 — compute authority (LANDED).** `reducer_frontier_compute_hstar(progress_db,
  coins) -> {hstar, served_floor}` in `app/jobs/src/reducer_frontier.c`, ONE
  SELECT-only read txn under `progress_store_tx_lock`, applying the C3 predicate.
  Regression test `test_reducer_frontier_hstar`.
- **L1 — light self-heal (flag/cursor only; NEVER coins).** Condition
  `reducer_frontier_reconcile_light`: clamp ONLY the tip_finalize cursor to H*+1;
  SWEEP-heal block_index nStatus over `(H*..tip]` from the hash-bound logs; never
  reset utxo_apply when coins are intact (would force coin re-application / resurrect
  spent coins). Never deletes a tip_finalize_log row.
- **L2 — deep coins rewind (genuine coin tear only; commitment-gated or refuse).**
  Condition `reducer_frontier_reconcile_deep`: verify every `utxo_apply_delta` row in
  `(H*,applied]` present+non-malformed; if any missing → REFUSE + EV_OPERATOR_NEEDED,
  NEVER `DELETE FROM coins`. Else emit_inverse_delta + recompute commitment and require
  exact hash+count match vs a stored trusted commitment at H*.
- **L3 — subtraction.** Delete `stage_reconcile_clamp_tip_finalize_to_floor` (boot.c:3344),
  dead `chain_restore_clear_failed_above_tip`; stop `chain_restore_finalize` returning
  ZCL_ERR(-2) on RECONCILABLE; one frontier authority, boot + runtime.

**Consolidation prerequisites (P1–P4), the safety basis for any rewind:**
- **P1** DELETE the projection dual coin-write — coins_kv becomes the SOLE coin store
  (3 stores → 1). Copy-proof: SHA3 commitment EXACT match vs oracle `gettxoutsetinfo`
  + kill-9×10.
- **P2** co-committed `coins_applied_height` cursor inside the coins stage txn — a
  contiguous frontier that cannot hide an interior hole (replaces `MAX(coins.height)`).
- **P3** rolling stored verified commitment at finalized heights
  (`utxo_commitment_sha3_save` at tip_finalize).
- **P4** `synchronous=FULL` on the coins/progress.kv path OR gate post-power-loss boot
  on an L2 recompute.
