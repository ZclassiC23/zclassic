# Session handoff — 2026-07-09 (night): mirror-storm fix live; sim next-wave specced

Verify live state before trusting this doc (`zcl_status` on dev/live MCP).

## TL;DR

`origin/main` @ **`0a364358b`**, pushed, **deployed to dev** (build_commit
verified `0a364358b`, at tip, spam gone). Live node untouched, healthy at tip.
simnet_wire step B was already merged before this session (`7fa940ac2`).

## Shipped to origin/main this session

- **S2 utxo_mirror guard fix** (`27502bff4`, merged `4ebbd86fb`) — the mirror
  service called MCP-client-layer `chain_projection_best_header_height()`
  in-process, which always failed → `header_tip=-1` **defeated both near-tip
  guards**, re-running the wholesale ~1.3M-row mirror rebuild every 5s during
  catch-up (the storm class behind the wallet P0 suspicion) + 11k/16k
  node.log spam lines. Fix: new lock-guarded in-memory `csr_header_height()`
  in `chain_state_service.c`; verified live on dev post-deploy — 0 new spam
  lines. **Next agent: re-test `getnewaddress` on dev** (wallet P0
  `project_wallet_nodedb_busy_lock_2026-07-09`) — this fix removes one lock
  writer; the P0 may or may not persist.
- `lib/sim/simnet_wire*` + seed_tape/postmortem **impact-rule mappings**
  (`841083c40`) — sim changes now get focused tests at push time.
- **`docs/work/wire-next-wave-specs.md`** — implementation-ready lanes for wire
  steps D/E/F + app-layer flows. Load-bearing facts: harness egress is pinned
  to peer slot 0 (one honest connection, N adversarial byte sources); per-link
  `wire_link.open` is the right partition primitive (exists, never produced);
  bandwidth tokens exist, just need configurability; HTLC settlement is
  ALREADY simulated (docs were stale — fixed in `docs/SIMULATOR.md`).
- **`docs/work/sapling-sim-spike.md`** — Sapling-in-sim is feasible: a real
  pure-C23 Groth16 PROVER is in-binary (`lib/sapling/src/sapling_prover_c23.c`,
  mmap'd `~/.zcash-params` keys). Gaps: seedable prover RNG, in-sim
  note-commitment tree, sim-local activation profile (value-copy params ONLY —
  consensus-parity-gated otherwise). Lanes A–E specced with tiers.

## In-flight branches (LOCAL only — origin holds only main)

- **`fix/txkit-parallel-segv` @ `4ea3b9a83` — root cause FOUND and fixed,
  needs verification before merge.** `simnet_wallet_default_fee_per_k()`
  put a ~65 MB `struct wallet` on an 8 MB thread stack (only crashed under
  `test_parallel` worker threads). Fix heap-allocates. TO DO: independent
  verify — `make lint`, `make -j`, `test_zcl simnet_txkit` standalone, then a
  FULL `build/bin/test_parallel` run (twice ideally), diff review, merge.
- **`sim/wire-byzantine-c` @ `893a1f391`** — step C byzantine bridge,
  WIP-checkpointed at session end mid-fix (main already merged in at
  `64e093a82`). Known remaining failure modes from the earlier 18: reject_reason
  capture, misbehaving-peer ban, consensus-unchanged monitor tripping
  (suspected observation loop mutating UTXO state — verify before acting).
  Relaunch an Opus lane on this branch; do NOT weaken the monitors.

## Next mission steps (owner priority)

1. Verify + merge `fix/txkit-parallel-segv` (full parallel proof).
2. Finish + merge wire step C, then fire the D/E/F + app-flow wave from
   `docs/work/wire-next-wave-specs.md` (disjoint lanes, haiku+codex implement
   → sonnet verify — pattern in SESSION-HANDOFF-2026-07-09-LATE.md).
3. Sapling-in-sim wave per `docs/work/sapling-sim-spike.md` (after wire wave).
4. Parked owner decision: **SYNC_AT_TIP transition** (diagnosis in
   SESSION-HANDOFF-2026-07-09-LATE.md — do not just flip it).
