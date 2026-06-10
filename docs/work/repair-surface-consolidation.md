# Repair-surface consolidation plan (2026-06-10)

Source: a 12-agent read-only audit (5 mappers + 3 root-cause lenses + 3 design
proposals + 1 synthesis judge) of the node's repair/recovery surface. Triggered
by the owner asking "are we going in circles?" after a session of stacked
repairs (PRs #3/#4, fortress waves, the −2 height-relabel incident).

## Verdict: yes, for the chain-index/coins/frontier subsystem.

One center, already named by the project's own Prime Directive (FRAMEWORK.md §0):
**the same fact lives in multiple independently-mutable durable stores, so they
drift, and ~5,500 LOC of repair machinery exists to re-agree them.**

Verified redundant authorities:
- `block_index` stores `nHeight`/`nChainWork`/`nChainTx` (`chain.h:103/87/108`)
  beside `pprev` (`chain.h:82`, the only true source). The three scalars are
  pure functions of `pprev`. **The 2026-06-10 −2 relabel was exactly this drift.**
- All 7 stage logs are `height INTEGER PRIMARY KEY` + `INSERT OR REPLACE` with
  the block hash a mere overwritable column, committed in 8 separate txns.
  `compute_hstar` runs a hash-agreement cross-check (`reducer_frontier.c:358`)
  *because the logs can disagree at one height*.
- The coin set lives in ~4 places (projection utxo, node.db `utxos`, `coins_kv`,
  RAM cache) + 3 "applied height"/tip authorities (`coins_applied_height`, the
  `utxo_apply` cursor, H*, `coins_best_block`).

### Going-in-circles loops (evidence-backed)
1. **relabel → purge**: the −2 bug wrote wrong-label rows; `purge_noncanonical`
   exists *only* to delete them. A writer bug creates drift; a Condition that
   *also writes* the reducer's cursors/logs cleans it — the forbidden 2nd writer.
2. **repair hardens against repair**: `block_index_integrity` carries anti-`−2`
   guard code (`bii_is_genesis`, "walk reaches genesis at nonzero height" refusal)
   that exists *only because a prior positional-genesis repair* relabeled the index.
3. **stale-script replay manufactures the holes refill re-heals**: it DELETEs
   log ranges + rewinds 5 cursors; the next 5s tick's refill sees rowless holes
   and tipfin sees a pinned H*, so both re-fill/re-finalize. A→B→A.
4. **tipfin-backfill IS the coin-tear**: `refused_coin_tear` (coins_applied >
   hstar+1) is the rowless `tip_finalize_log` span `try_tipfin_backfill` then
   *forges synthetic `ok=1` rows* to fill. Bookkeeping reconciliation, not safety.
   **Live Exhibit A (2026-06-10): node up at 3,142,739 but coins_applied=3142741
   > hstar=3142739 → "L2 required" refusal, retry-fail loop, tip frozen.**
5. **four boot coins-reconcilers, four heuristics** in one boot
   (`boot.c:2718/2800+/3132/2555`); `chain_state_validator` Case 3b was added
   *to stop another reconciler* from RESET_CHAIN-ing finalized progress.
6. **six height/work/tx fold implementations** all re-derive the same fact.
7. **monitor-only mirror no-op loop**: `legacy_mirror_stuck` Condition's remedy
   "requests catchup" that applies no blocks — Law 7 "a remedy that returns ok
   without moving the symptom is a lie." [DELETED Phase 1]

## Phased plan

- **PHASE 1 [safe-now] DONE 2026-06-10** — deleted the dead `legacy_mirror_stuck`
  Condition + its test (the no-op remedy loop). Kept the lag monitor + the
  supervisor poll (that's the monitor heartbeat). Deferred from Phase 1: the
  `CAC_SOURCE_ZCLASSICD_MIRROR` source-enum removal (wider status/test surface;
  live node cosmetically selects it mid-incident).
- **PHASE 2 [safe-now] mechanical DRY** — route the 6 height/work/tx folds →
  the single `block_index_forward_pass` helper; unify the 3 disk-prev-hash
  readers → 1; delete the duplicate 5s-tick tip_finalize clamp (keep the
  boot/kill-9 one, `stage_repair.c:55-138`); route the 2 flag-from-log copies
  → 1 helper. Gate: central gates + **byte-identical `block_index.bin` before/
  after on a datadir COPY**.
- **PHASE 3 [safe-now]** — unify the 4 boot coins-vs-chain reconcilers → 1 with
  one documented tie-break rule. Cold-boot smoke on a copy.
- **PHASE 4 [owner-gated]** — fix Task #23 (−2 relabel) AT THE WRITER, prove a
  current-binary datadir produces zero non-canonical rows on a copy, THEN delete
  `purge_noncanonical` + the bii anti-`−2` guards (cleanest: do this AS Phase 5).
- **PHASE 5 [owner-gated] — DERIVE-HEIGHT-FROM-PPREV-ONLY (THE #1 lever)**: stop
  persisting `nHeight`/`nChainWork`/`nChainTx`/`pskip` in `block_index.bin`;
  store only disk position; fold from `pprev` once at load (single O(n) pass —
  the multi-pass repairs were "far too slow"). Makes "stored height disagrees
  with pprev-implied height" UNREPRESENTABLE — closes the entire −2 class by
  construction. Deletes `block_index_repair_heights`, `recompute_index_from_
  genesis`, the orphan height tail, `repair_pprev`'s recompute tail, the boot
  nChainTx multipass, `reconcile_block_index_flags`, the boot SQLite-status
  promote. Touches the on-disk index format. Prove-on-copy: byte-exact
  gettxoutsetinfo/utxo_commitment + cold-boot + torn-index + −2-affected copies.
- **PHASE 6 [owner-gated] — KEYSTONE**: one frontier fact (collapse H* /
  tip_finalize cursor / coins_applied_height → one number) + hash-keyed
  append-only stage log (re-key the 7 logs off mutable height onto block_hash;
  reorg = pure append + most-work fold) + atomic per-block co-commit across all
  8 stages (collapse the 8 txns into one). UTXO set becomes a re-foldable
  projection — a missing coin is rebuilt by RE-VALIDATING its creating block
  (FRAMEWORK §0: validation is a left-fold-WITH-feedback, NOT a pure fold — do
  not trust a side-store), not re-minted by the 11-guard backfill scanner.
  Deletes `refused_coin_tear`, `try_tipfin_backfill`, the refill family, the C3
  cross-check, header_admit reorg-rewind, the coin-backfill scanner, Case 3b.
  Largest blast radius in the codebase — split into independently-gateable
  sub-steps (a: hash-key logs; b: single atomic commit; c: collapse frontier
  number; d: delete each now-dead repair). Respect the kill-9 ordering invariant
  (coins.db commits before block_index fsync). Full prove-on-copy (cold-sync,
  kill-9, reorg, restart-resume) + 7-day soak.
- **PHASE 7 [owner-gated]** — after the live datadir is resynced from a correct
  binary (no stale verdicts exist), delete the in-place defusers (value_overflow
  + stale_script replay + stale_validate_headers_repair + clear_failed_above_tip
  + the coins_kv/applied-height migration shims); demote coins_best_block /
  projection-utxo / node.db-utxos to seed-only. RE-TRACE the `utxo_recovery_*`
  import-seam first — cold-start/kill-9 import against the external zclassicd/LDB
  seam is the ONE genuinely-external corruption class; its recovery must survive.
- **PHASE 8 [owner-gated]** — implement the single FRAMEWORK §5
  `tip_not_advancing` liveness Condition (verified ABSENT) as the sole
  EV_OPERATOR_NEEDED emitter; demote `chain_tip_watchdog`, `node_health_service`,
  `reducer_ingest_service`, `stale_validate_headers_repair` to non-paging feeders.
  With one frontier fact, liveness = one number (`network_tip - log_head`).

## Owner decisions (only the owner can make these)
1. **Single ancestry authority**: confirm `pprev` (from the immutable on-disk
   `hashPrevBlock`) as the sole disk authority; height/work/tx become RAM-only
   memoized derivations. (Phase 5 — touches `block_index.bin` format.)
2. **Single frontier authority**: confirm tip_finalize's cursor IS tip ==
   coins_applied == H* (one number); retire the other three. (Phase 6 keystone.)
3. **Recovery story for historical torn datadirs**: commit to "resync from a
   correct binary" (enables deleting the in-place defusers; keep a tagged old
   binary that still heals old datadirs)? Gates Phase 7 + the live/proof5 fate.
4. **Deploy/resync sequencing**: when to take the live node down to resync the
   torn datadir from a correct binary (Task #17 deploy in_progress).
5. **Scheduling vs MVP**: Phases 5-6 are off the v1 MVP critical path; interleave
   with live-wedge/MVP work, or sequence after v1 (Phase 6 warrants a 7-day soak)?

## Honest caveat
Most "dead-looking" repairs are NOT delete-now: the live/proof5 datadirs are
still torn (Task #23 in_progress), so the purge + `−2` guards + in-place defusers
are load-bearing TODAY until a clean resync. Only Phase 1 (the no-op mirror loop)
was genuinely dead. The structural wins (Phases 5-6) are where the ~5,500 LOC
goes away — but they ARE the durable-state/consensus model and need owner go +
prove-on-copy + soak.
