# Full-history replay substrate — design (2026-06-21)

> The gate the consensus-parity doctrine (h=478544) requires: before TIGHTENING a
> bounded predicate to restore parity with zclassicd, replay the ENTIRE real chain
> (genesis→tip) with the candidate tightening ON and confirm **zero** false-rejects
> (no real already-mined block the looser zcl23 accepts gets newly rejected).
> 0 = safe to land the parity restore; ≥1 = the chain depends on the looser rule
> (the h=478544 class) and the fix MUST NOT ship. Raw: workflow
> `replay-substrate-design` (run `wf_0e563659-fee`).

## Infrastructure already exists (reuse, don't rebuild)
- **WALK 1 — `bg_validation_service.c`** walks genesis→tip read-only over real bodies
  (`read_block_from_disk_index_pread`, thread-safe), runs header/PoW/difficulty +
  proof + script predicates; crash-resume + parallel workers. Live state: complete,
  verified_height=3,150,488 (proof the walk completes). Does NOT apply UTXO deltas.
- **WALK 2 — `reindex_chainstate` (boot_index.c)** replays `connect_block` over every
  block (exercises the apply-path predicates), stops on first failure.
- **`replay_verify_service.c`** — genesis→tip, **non-stopping** (counts + records
  `first_fail_height`), MCP-exposed (`zcl_replay_verify`), mutates no consensus state.
  The model for the counting harness.
- **Block source CONFIRMED:** `~/.zclassic-c23/blocks/` = full real chain (51 blk + 51
  rev, 6.8 GB, genesis→3,155,342, undo present both ends). **h=478544 (the lesson
  block, 125,811-byte tx) is on disk with undo** — the substrate contains the exact
  block that proves the doctrine. zclassicd's `~/.zclassic` is separate — never use it.

## First target: D2 coinbase-maturity (the most consequential live loosening)
zcl23's live fold accepts a premature coinbase spend (`utxo_apply_delta.c:59`
`g_enforce_coinbase_maturity=false`); zclassicd always rejects. D2 is first because
it has BOTH a clean per-block hook AND a from-genesis walk that already maintains the
state the predicate reads (the spent coin's `restore_height` + `restore_coinbase`,
`utxo_apply_delta.c:281-285`), AND an existing toggle (`-enforce-coinbase-maturity`,
`main.c:1835-1846`).

### Build plan
1. **Copy the datadir** — `cp -a ~/.zclassic-c23 ~/.zclassic-c23-replay-d2` (the fold
   writes coins_kv; never run against the live datadir). [tooling]
2. **Coverage pre-flight (anti-false-0):** assert h=0 readable (mirror `boot_index.c:262`)
   AND `blocks_replayed == tip+1` contiguous — `legacy_iter_from` silently `continue`s
   past holes (`block_log_legacy.c:134`), so a sparse/cold-import set walks a subset and
   reports a FALSE 0. A short walk is a **gate FAILURE, not a pass**. [tooling]
3. **Run the from-GENESIS fold** `-refold-staged -enforce-coinbase-maturity` — NOT
   `-refold-from-anchor` (which seeds genesis→anchor from the snapshot WITHOUT folding
   it and pins the cursors to the anchor h=3,056,758, so the predicate never fires
   below the anchor → the h=478544-class offender is INVISIBLE and the walk can never
   be contiguous from genesis), and NOT `-load-verify-boot` (no-ops on a stamped
   coins_kv = false-green). `-refold-staged` (`boot_refold_staged.c:47`) resets all 8
   stage cursors + coins_kv to genesis, so the predicate fires on EVERY block from h=0.
   The predicate fires at `utxo_apply_delta.c:281` per spend. [consensus-adjacent]
4. **Count-and-continue mode** (env-gated `ZCL_REPLAY_COUNT_ONLY`): today a `delta_fail`
   flips `summary.ok` → JOB_BLOCKED and the frontier halts at the first fire. In
   count-only mode, **log each fire (height + spent outpoint) and continue WITHOUT
   authoring/blocking**, accumulating a total. **SAFETY: strictly read/log/continue,
   NO coins authored when counting** (else it corrupts the copy's coins_kv). [consensus-adjacent]
5. **Structured summary** `{first_offending_height, total_newly_rejected, blocks_replayed,
   tip, target_tip, genesis_readable, contiguous, reached_target, gate_pass}` (model
   `replay_verify_service.c:264-285`). `target_tip` is the `header_admit` cursor (top of
   the staged pipeline = header tip); `reached_target` (tip+1==target_tip) closes the
   "proof_validate stalls below chaintip" hole — a contiguous-but-truncated walk is NOT a
   pass. [tooling]
6. **`make replay-gate-d2 DATADIR=<copy>`** — greps the summary, exits non-zero unless
   `total==0 && contiguous && reached_target` (i.e. `gate_pass`). [tooling]
7. **Lock the result:** only after a 0-count genesis→tip pass, flip the default at
   `utxo_apply_delta.c:59` + add a parity lock-in test. [consensus-adjacent]

### The gate
Replay full chain with `-enforce-coinbase-maturity` ON, count-and-continue: assert
`total_newly_rejected == 0` AND `first_offending_height == -1` AND a contiguous
genesis→FULL-header-tip walk (`genesis_readable && blocks_replayed == tip+1 &&
reached_target`). ~tens of minutes (from-genesis refold authors coins_kv,
~cold-sync-apply cost). Read-only over blk*.dat; writes only the copy's coins_kv; runs
alongside the live node; zclassicd untouched.

> **Convergence (2026-06-21):** the from-genesis `-refold-staged` fold is ALSO the live
> wedge cure — a clean re-derivation with no borrowed chainstate seed has no
> non-best-chain coin for the frontier to refuse, so H\* climbs from the anchor to tip.
> A 0-offender gate run therefore doubles as proof the wedge is curable by re-derivation.

## Deferred (NOT in the first gate)
- **D1 difficulty precedence** — a STRUCTURAL boolean (`pow.c:52-56`), no flag; needs
  two compiled builds diffed per-height, and ONLY on a **testnet/regtest** log spanning
  the BUTTERCUP window [78856,78873). Mainnet collapses both paren-forms to byte-identical
  → a mainnet replay always shows 0 diff and would FALSELY "prove" safety.
- **Sapling-root** — `sapling_root_matches` returns cannot-decide when the pre-block
  frontier `g_sapling_tree` is NULL (`connect_block.c:137-138`); a 0-reject run is a
  FALSE pass until a per-height incremental_merkle_tree frontier is threaded through.
- **L7 anchor-membership** — `coins_view.c:468-476` is a literal `return true;` stub,
  no toggle; parity-RESTORING gated on building the Sprout/Sapling anchor set first
  (see `docs/work/l7-anchor-membership-design.md`), not a replay-with-flag target.

## Top risks
- **False 0 from a partial set** (the #1 risk) — mitigated by the mandatory contiguity
  + genesis-readable gate clause.
- D2 needs UTXO context → use the from-genesis FOLD, never the PoW-only sweep (would
  skip the predicate → false-green).
- Count-only mode implemented sloppily could author past a real reject and corrupt the
  copy → strictly no-author-when-counting.
- 3.15M-block perf/RAM + 6.8 GB copy — budget tens of minutes, use the fold's flush
  cadence + malloc_trim, run on the copy.
