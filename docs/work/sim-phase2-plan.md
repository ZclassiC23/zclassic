# Phase-2 Plan — In-Memory Simulation Network + Deterministic Testing

Status: DESIGN (2026-07-09, Opus plan pass). Phase-1 slices (simnet_mint_txs, ZSLP,
ZName, minimal multi-node) are being implemented on `sim/zslp-zname-slices`; this
plan starts after they land. Copy to `docs/work/sim-phase2-plan.md` at integration.

## Baseline (exists — do not rebuild)
- `lib/sim/src/simnet.c` — single-node RAM harness over real `connect_block(..., expensive_checks=false)`; private `sim_mint_block()`; base height 100 (pre-Sapling).
- `lib/sim/src/seed_tape.c` (`sim/seed_tape.h`) — xoshiro256++ RNG + virtual mono/wall clock from one uint64, ordered action log, record/replay, versioned serialize (`ZCLTAPE!`), installs into `platform_rng`/`platform_clock`. THE "bugs become 64-bit seeds" spine — reuse verbatim.
- `lib/sim/src/postmortem.c` — tape → crash capsule; list/load/replay.
- `tools/sim/chaos.c` + `tools/sim/sim_peer.c` — chaos binary + `.scenario` DSL. GAP: sim_peer is bookkeeping-only (counters), never drives real connect_block. Unify in Item 3.
- `lib/test/src/test_reducer_stage_fuzz.c` — reference seed-fuzz pattern (splitmix64 sub-seeds, repro seed printed on failure, typed-outcome invariant).
- Reorg primitives real: `disconnect_block(...)` exists; `lib/test/src/test_reorg_parity.c` shows undo capture (`update_coins_with_undo` → `block_undo` → `disconnect_block`).
- Test registration: `X(name)` at `lib/test/src/test_parallel.c:177` + decl in `test_helpers.h` + `lib/test/src/test_<name>.c` + `AGENT_IMPACT_RULE` line. `lib/sim/src/*.c` auto-globbed into ALL_SRCS.

Consensus stays untouched throughout; `expensive_checks=false` is the existing escape hatch.

## Item 1 — Multi-node cluster + reorg substrate (TOP-3 #1, size L)
`lib/sim/{include/sim/simnet_cluster.h,src/simnet_cluster.c}`: `simnet_cluster_init(n, uint64 seed)`, `_mint_on`, `_broadcast`, `_deliver_pending()` (deterministic per-link latency/reorder from seed_tape RNG), `_tip_hash`, `_coins_digest`. Fork-choice by nChainWork; tie-break first-seen then hash.
Per-node retained store `lib/sim/src/simnet_chain.c`: `{block, block_undo, block_index}` per height (undo captured via the test_reorg_parity pattern in a `connect_retained` helper mirroring `sim_mint_block` — layer BESIDE simnet.c, don't fork it). Reorg = real `disconnect_block` back to fork point + real `connect_block` forward.
Test `test_simnet_cluster.c` group `simnet_cluster`: 2-node convergence; competing equal-length chains → deterministic winner, equal tip hash + coins digest on all nodes; 6–12-block reorg → coins view byte-identical to direct-build.
Deps: phase-1. Blocks Items 2–4.

## Item 2 — Byzantine peers + typed-blocker assertions (TOP-3 #3, size M)
Invariant per rejection class: rejects, names a typed blocker, tip does NOT advance, next honest block still accepted (no silent halt).
Tier 1 (real connect_block at expensive_checks=false): merkle mismatch, bad-cb-amount, BIP30 dup txid, missing/immature spend, negative/overflow outputs, oversize vtx — assert typed `vs.reject_reason` → blocker class.
Tier 2 (header-only classes connect_block skips: invalid_pow, bad_bits, bad_timestamp): thin cluster admission gate mirroring the real header_admit/validate_headers predicates, rejecting BEFORE connect_block as the live pipeline does. Back the existing sim_peer malformed taxonomy with real predicates.
Files: `lib/sim/{include/sim/simnet_byzantine.h,src/simnet_byzantine.c}`; test group `simnet_byzantine`. Checker gets a mutation self-check ("rejected but tip advanced" must flag).

## Item 3 — Deterministic seed fuzzing over the cluster (TOP-3 #2, size M)
`lib/sim/{include/sim/simnet_scenario.h,src/simnet_scenario.c}`: generator consumes installed seed_tape RNG → action stream (mint-on-node, relay, partition/heal, byzantine inject, reorg trigger); actions recorded to tape; failing seed serializes via `postmortem_capture_write` to a replayable `.cap`.
Test `test_simnet_fuzz.c` group `simnet_fuzz` (modeled on test_reducer_stage_fuzz.c): `ZCL_SIMNET_FUZZ_ITERS` iterations, derived sub-seeds; invariants = cluster convergence, no un-escaped blocker, coin conservation, per-node tip monotonic. On failure: `printf("SIMNET REPRO SEED=0x%016llx\n", subseed)` + capsule dump.
Chaos unification: add a `simnet` mode to tools/sim/chaos.c DSL driving the REAL cluster (retire/bridge sim_peer bookkeeping). `make simnet-repro SEED=0x...` target reusing the CHAOS_SEEDS sweep shape (Makefile:797-813).

## Item 4 — Contract-overlay sim: escrow/HTLC/ZSWP (size M–L; after phase-1 ZSLP/ZName + Wave-3 contract_state projection)
Needed primitives:
- RAM mempool `lib/sim/src/simnet_mempool.c`: accept-tx → hold → include-in-next-mint.
- Multi-block absolute timelocks: `simnet_mint_to_height(h)` + deterministic block nTime from virtual clock so CLTV refund windows open on schedule (all timelocks absolute, roadmap §7 — no OP_CSV).
- P2SH HTLC txs via existing `lib/script/htlc.c` builders; assert the contract_state PROJECTION transitions (PENDING→FUNDED→REDEEMED/REFUNDED/EXPIRED), not script exec (skipped at expensive_checks=false).
- Sapling memo overlays (ZMSG/dead-drop): separate later sub-slice `simnet_sapling.c` (needs post-activation height + real incremental_merkle_tree via connect_block_set_sapling_tree). NOT on the escrow/HTLC critical path.
Files: `simnet_mempool.c`, `simnet_contract.c`, test group `simnet_contract` (happy path fund→redeem→SETTLED; refund path via mint_to_height past CLTV → REFUNDED with the window surfacing as a named condition).

## Item 5 — CI gating (size S)
Fast/hermetic in `make ci` via test_parallel (each ≲2s): simnet_cluster (3 nodes, ≤3-block reorg), simnet_byzantine (one per class), simnet_fuzz (ZCL_SIMNET_FUZZ_ITERS≈128), simnet_contract (happy+refund).
Nightly (NOT in ci; mirror fuzz-ci + zclassic23-*.timer + background_quality_lane.sh): `make simnet-fuzz-sweep` (10k–100k seeds, deep-reorg depth sweeps, ≥32-node clusters) aggregated as `make simnet-nightly`. Both tiers print SIMNET REPRO SEED on failure; `make simnet-repro SEED=` replays; capsules replay exact tapes.
Seed-count env knobs; no wall-clock/entropy anywhere.

## Ordering
1 → 2 → 3 → (4 after prereqs); 5 lands incrementally per group.
Top-3 first: Item 1 (substrate; brings reorg under multi-node coverage), Item 3 (realizes bugs-become-seeds at low marginal cost on the seed_tape spine), Item 2 (defends never-silently-halt against adversarial input; feeds Item 3's vocabulary).
