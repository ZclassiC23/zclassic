# ZClassic23 Architecture Roadmap

> Built 2026-06-20 from a 6-dimension parallel architecture audit (execution/concurrency,
> consensus-state-core, storage/data-model, code-organization, security/sync, operator-UX),
> each grounded in file:line and compared to Bitcoin Core / zcashd-Zebra / Erigon-reth.
> The objective it optimizes for: a fully-SOVEREIGN node that verifies EVERYTHING itself,
> is FAST by saturating all cores, cold-starts in seconds from a SELF-MINTED checkpoint,
> never serves an unproven value, stays bit-for-bit consensus-compatible with zclassicd.
> Verify against code before executing; specifics rot.

## THE TWO LOAD-BEARING CHANGES (LB-1 strictly precedes LB-2)

### LB-1 — A core-saturating verification engine OWNS the hot path; the supervisor OBSERVES.
Today the liveness layer DRIVES compute on a 2s timer (staged_sync_supervisor.c:249) and
the two most expensive verifications — ECDSA scripts (script_validate_stage.c) and
Groth16/PHGR proofs (proof_validate_stage.c) — run SERIAL (0 pthread_create), inside a step
that holds the global `progress_store_tx_lock` across the crypto (stage_helpers.h:200). On a
32-core box ~30 cores idle → the 3–14 blk/s wall.

Target: one persistent `(nproc-1)`-worker pool (a CCheckQueue analog) in `lib/validation/`
(thread_pool.c + verify_queue.c). script_validate + proof_validate SUBMIT work and join the
pool; subsume the two ad-hoc pools (`vh_pool` validate_headers_stage.c, the per-batch
spawn/join in bg_validation_scripts.c) into it — delete the duplicates. **Move crypto OUT
from under `progress_store_tx_lock`** (verify lock-free; acquire the lock only for the short
atomic *_log + cursor UPSERT). Batch Groth16 (Zebra/librustzcash BatchValidator) with a
per-proof serial fallback. Size from `-par` (default `nproc-1`; `-par=1` = serial bisection
oracle). `utxo_apply` stays serial (ordered mutation). Promote the refold driver in
`reducer_ingest_service.c` to the default IBD driver (the `refold_driver_main` symbol/line
cited here has moved/changed since 2026-06-20 — locate it fresh); demote the supervisor to
observer (keep heartbeats + named stalls, drop `period_secs=2` as the throughput gate).

### LB-2 — Self-mint the checkpoint in-binary; stop borrowing zclassicd's chainstate.
Today coins_kv is SEEDED by `cp -a` of zclassicd's LevelDB chainstate (utxo_recovery_restore.c),
trusted at ONE height against a checkpoint whose provenance is literally "Verified bit-for-bit
against zclassicd" (checkpoints.c:73). FlyClient verifies PoW samples against THE PEER'S OWN
offered `mmb_root` (snapshot_offer.c) — circular trust. ~12,051 LOC of recovery/repair/reconcile
exists SOLELY to compensate for the borrowed, unverified seed.

Target: mint the checkpoint by full from-genesis validation (every Equihash 200,9 + ECDSA +
Groth16/PHGR), once, compiled in. Cold-start = load coins_kv's own SHA3-committed snapshot +
FlyClient PoW-sample verified against the IN-BINARY root (never the peer's). Pin the anchor
hash (reject any P2P snapshot whose block_hash != in-binary checkpoint). Use real
`CheckProofOfWork` for samples; retire the weaker `fc_check_pow` (flyclient.c:21). Mandatory
background genesis→anchor re-validation converges to full-node security (the assumeutxo
property — currently absent). Self-minting makes coins_kv canonical-by-construction → ~12k LOC
of divergence-repair collapses to ~2–3k.

## KEEP — already best-possible; protect from the rewrite
- `reducer_frontier.c` — pure SELECT-only fold, H*=MIN over each log's contiguous ok=1 prefix,
  refuses to advance past the first hole. This is what makes a hole NAMEABLE. Do NOT replace
  with a single hashBestBlock. The over-engineering is the repair fabric, NOT the fold.
- `utxo_apply_stage` atomic co-commit (coin delta + inverse + log + cursor + applied_height in
  ONE BEGIN IMMEDIATE). LB-1 must preserve this — move only the CRYPTO out of the lock, keep
  the WRITE transactional.
- coins_kv as the single committed authority (coins+cursor+nullifier co-committed).
- `domain/` purity + inward-only lib-layering — promote from RATCHET to HARD.
- The three MCP primitives (zcl_sql/zcl_node_log/zcl_state) + zcl_rpc escape hatch + the
  EV_OPERATOR_NEEDED named-blocker latch.
- The consensus verify functions themselves (verify_script, Sapling/Sprout, Equihash) — never
  touched; parity preserved by construction.

## FIX — right shape, needs work (ranked by leverage)
1. **Wire H\* to served APIs** (DONE — H* now served at blockchain_controller_blocks.c:45 and
   msg_version.c:155 via `reducer_frontier_provable_tip_cached`). (Highest correctness leverage,
   low risk, INDEPENDENT.) `getblockcount` (blockchain_controller_blocks.c) + P2P `start_height`
   (msg_version.c) previously served `active_chain_height` (window/header tip), which can sit
   ABOVE the proven frontier → would serve an unproven height (violates the north star). H* is
   now wired through `reducer_frontier_provable_tip_cached` at both call sites. Split at the type
   level: provable_tip=H* (external readers) vs window_tip=c->height (internal lookahead only).
2. **Collapse the 7 redundant UTXO representations** to coins_kv + replayable indexes. Delete
   utxo_mirror_sync_service.c (O(n) DELETE+reinsert on every drift tick), the abandoned
   utxo_projection.db, and the consensus_snapshot.db clone export. (Mostly downstream of LB-2.)
3. **Add a PUSH/long-poll operator path** — `zcl_wait_*` (waitforhalt/waitforheight/waitforblocker,
   shutdown-aware) + wire the EV_* stream to an MCP notifications out-channel. Event-driven, not
   poll-driven.
4. **In-process MCP transport** — `-mcp` is a separate proxy re-marshaling JSON twice over a fresh
   TCP socket per call (rpc_client.c:142). Host the MCP loop on a node thread; batch the
   aggregators server-side.
5. **Replace the 800-line hard cap with a cohesion gate.** The cap is line-golfed (5 files at exactly
   800; *_accessors.c splinters ADD coupling to stay under it) and miscalibrated (exempts lib/+domain/
   consensus density). Keep check-long-functions (≤500 LOC/fn); make any size signal a WARN applied
   uniformly. Don't "fix" it by splitting more files — delete the pressure.
6. **Collapse duplicate-name consensus pairs** — one definition each of equihash/base58/bech32/upgrades
   in domain/, delete the lib/ wrappers. One consensus rule = one file = one diff vs zclassicd.

## RETHINK — wrong shape for the objective
1. Execution/concurrency model → LB-1.
2. Cold-start/sovereignty trust model → LB-2.
3. "Eight shapes" as the FOLDER taxonomy → make DOMAIN the primary partition (vertical
   `consensus/<concern>/`), shape a filename suffix. One UTXO-apply concern currently shatters
   across 30+ files / 5 layers / 4 shapes. Demotion of the axis, not deletion; fix-grade execution.
4. Default-launch WebKit GUI (wallet_gui.c, launches with no args) → contradicts the no-GUI /
   AI-as-interface north star + costs binary size. Gate behind a flag; headless/MCP default.

## SEQUENCING (each step independently verifiable; parity never broken; node always bootable)
Dependencies: LB-1 gates LB-2's mint (can't self-mint from genesis in acceptable time serially);
LB-2's mint gates the recovery-LOC deletion (the fabric is the safety net). H*-serving is independent.

- **Phase 0 (ship now, no LB dependency):** FIX-1 (wire H*; replay-check advertised height first) —
  DONE (H* served at both call sites via `reducer_frontier_provable_tip_cached`); FIX-5 (drop line
  cap), FIX-6 (dedup), promote domain-purity+lib-layering to HARD, FIX-3 reads.
- **Phase 1 — LB-1 (parallel engine), behind default-on `-par`, `-par=1` oracle:** build the pool +
  queue with a determinism test (parallel result == serial over a fixed height range) + the
  test_consensus_parity goldens; subsume the ad-hoc pools; shrink the tx-lock scope (crypto out,
  write transactional); promote refold_driver to default IBD driver / supervisor to observer
  (keep the EV_OPERATOR_NEEDED naming). GATE: full-history replay vs the REAL chain (h=478544
  lesson) doubles as the parallel-vs-serial verdict-equivalence proof.
- **Phase 2 — LB-2 (self-mint), copy-proven before live:** run the parallel from-genesis full
  validation; diff the self-minted SHA3 vs the baked checkpoint (a mismatch is a BUG, never a
  fork); compile it in. Break the circular FlyClient root (verify vs in-binary root; pin the anchor;
  real CheckProofOfWork only) — these only TIGHTEN acceptance, replay-confirm zero false-rejects
  first. Make background re-validation mandatory + visible.
- **Phase 3 — subtraction, LAST, copy-proven on the frozen wedge fixture:** only after the
  self-minted seed cold-starts to tip hash-identical to zclassicd at multiple heights, delete
  utxo_recovery_*/stage_repair_*/chain_restore_*/utxo_projection.db/the LevelDB copy path/
  utxo_mirror_sync_service.c/consensus_snapshot.db export (~12k → ~2–3k LOC). Never live-surgery.
- **Phase 4 — UX + organization (parity-inert):** in-process MCP + push channel; onion `/mcp`
  bearer-gated behind `-mcp-onion`; domain-axis reorg; GUI default-launch fix.

## NON-GOALS / TRAPS
- NEVER skip validation for speed — the win is PARALLELISM (where checks run), not OMISSION.
  The assumeutxo bargain is "relaxed briefly, then converges to full security"; the mandatory
  background re-validation is what MAKES the brief relaxation acceptable — dropping it is the trap.
- NEVER ship a parity-tightening predicate without a full real-chain replay (h=478544).
- No big-bang rewrite — every step behind a flag or copy-proven on the wedge fixture; the serial
  path stays as the `-par=1` oracle; the node stays bootable at every commit.
- Don't replace reducer_frontier.c with a single hashBestBlock — the MIN-prefix is the feature.
- Don't delete the recovery LOC before the self-minted seed is proven (it's the safety net).
- Don't batch-verify Groth16 without a per-proof serial fallback (identify the bad block).
- Don't "fix" the file-size cap by splitting more files — delete the pressure.
- Don't expose /mcp over the onion without bearer + destructive-rate-limit, defaulting read-only.

**Bottom line:** two load-bearing changes — (1) a core-saturating verification engine that owns
the hot path with the supervisor as observer, and (2) a self-minted in-binary checkpoint that ends
the borrowed-copy dependency and lets ~12k LOC of compensation be deleted. (1) precedes (2). Wiring
H* to the served APIs was the cheap independent correctness win and has landed (H* now served at
blockchain_controller_blocks.c:45 and msg_version.c:155).
