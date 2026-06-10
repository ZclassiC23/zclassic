# Miner-signaled Equihash 200,9 sidegrade (no fixed hardfork height)

## Summary

ZClassic has used Equihash 192,7 (400-byte solutions) since the Bubbles fork at height 585,318. This design returns the chain to Equihash 200,9 (1344-byte solutions) without a flag-day activation height. Validators tally miner signaling in block versions; the change locks in after a sustained signaling majority and becomes mandatory after a fixed grace period. The activation height is deterministic and machine-readable about one month before the switch.

This is a **sidegrade**, not an upgrade: a lateral parameter change with different trade-offs rather than a technical improvement. Identifiers containing "upgrade" (`ehUpgrade`, `eh_upgrade`, `-signal-eh-upgrade`) reuse the deployment vocabulary of the network-upgrade tables.

The approach fits the existing codebase: `nVersion` is stored in every `block_index` entry, so header-only ancestor tallies are cheap; all `nVersion` checks here and in the elder zclassicd/MagicBean nodes are `>= 4`, so a signal bit relays through the existing network; PoW verification already dispatches on solution size (a 1344-byte solution verifies as 200,9 today); and the single consensus gate pinning the allowed solution size per height is the expected-size check in `contextual_check_block_header`. No versionbits machinery existed in the tree before this change. The feature ships in the first release, so every C23 node carries it from day one. Elder zclassicd carries the same height-pinned size gate but reads it from a static table with no versionbits awareness, so elders need an update before activation — see "Elder nodes" below.

**Decisions:** sustained-majority tally (consecutive passing windows, BIP8-style with no expiry); ~1-month grace between LOCKED_IN and ACTIVE; graduated difficulty relief at the switch (reusing `scaleDifficultyAtUpgradeFork`); the built-in miner signals by default and solves post-activation work with the generic reference solver.

## Mining infrastructure

- An open-source Equihash 200,9 miner will be available for both AMD and Nvidia GPUs.
- A 0-fee pool will be available as infrastructure immediately.
- GPU and ASIC miners driven by `getblocktemplate` (directly or through pools) are the intended mining targets. The node's built-in `equihash_basic_solve` is a reference solver used for regtest and validation, not for competitive mainnet mining.

## State machine (deterministic from headers only)

- Signal: `nVersion & (1<<16)`; miners set `nVersion = 0x10004`. Serialization is a lossless int32, the Equihash seal commits all 4 bytes, and nothing in the tree does equality or switch checks on `nVersion`, so the bit is safe to set and relay.
- Heights are grouped into fixed windows of W blocks aligned to multiples of W. A window passes when at least `threshold` of its blocks signal. A streak of `consecutive` passing windows reaches **LOCKED_IN** (sticky; a failing window before that resets the streak). **ACTIVE** begins at `H_a = locked_in_boundary + grace`. From `H_a`, the expected parameters are the sidegrade set.
- Evaluation is per window boundary with a mutex-guarded in-memory cache keyed by the boundary block hash, which makes it reorg-safe with no invalidation. A full recompute after restart is ~730 boundaries × 4,320 parent hops — milliseconds.

| Network | Window | Threshold | Consecutive | Grace | Switch |
|---------|--------|-----------|-------------|-------|--------|
| Mainnet | 4,320 (~3.75 d at 75 s) | 2,204 (51.02%) | 8 (≈1 month) | 34,560 (= 8·W) | 192,7 → 200,9 |
| Testnet | 1,440 | 735 | 4 | 2,880 | 192,7 → 200,9 |
| Regtest | 16 | 9 | 4 | 32 | 48,5 → 96,5 |

Regtest flips 48,5 → 96,5 (68-byte solutions, already in the verifier's dispatch map) so `generate`-driven end-to-end tests can walk the whole state machine.

## Implementation steps (ordered; each compiles + gates green)

0. **Branch + design doc** — branch `feature/equihash-200-9-versionbits`; this document is committed as part of the PR.
1. **Deployment params** — `lib/consensus/include/consensus/params.h`: `struct eh_upgrade_deployment { enabled, nSignalBit, nWindow, nThreshold, nConsecutiveWindows, nGraceBlocks }` in `consensus_params`; per-network values (table above) and `nEquihashUpgradeN/K` in `lib/chain/src/chainparams.c`.
2. **New module** `lib/consensus/src/versionbits.c` + `include/consensus/versionbits.h`: `versionbits_eh_query(params, pindex_prev, struct vbits_info *out)` (returns `struct zcl_result`; state/streak/locked_in_height/active_height/window_signal_count), `versionbits_eh_active(params, pindex_prev, int *h_a)`, `versionbits_cache_reset()`. Uses `block_index_get_ancestor`; iterative boundary walk; incomplete or non-monotonic ancestry returns a failing result (snapshot tails). No clocks.
3. **Param getters** — `chain_params_equihash_n_at/k_at(params, pindex_prev, height)`: if the deployment is ACTIVE and `height >= H_a`, return the sidegrade parameters; otherwise fall back to the static epoch table. The bare 2-arg getters remain for callers without chain context.
4. **Consensus gate** — `contextual_check_block_header` (`lib/validation/src/check_block.c`) resolves the expected solution size through the `_at` getters. This covers live header accept and background re-validation; the sparse-snapshot `skip_contextual` bypass keeps fast-sync tails working.
5. **Difficulty** — `GetNextWorkRequired` (`lib/chain/src/pow.c`) extends the graduated fork-scaling window to `[H_a, H_a + nPowAveragingWindow)`, the same relief used at the DiffAdj and Buttercup forks.
6. **Miner** — `create_new_block` sets the signal bit (default on; `-signal-eh-upgrade=0/1`). `mine_block_pow_at` takes `pindex_prev` and solves with the `_at` parameters. `try_solve_equihash` keeps the Tromp solver for 192,7 and routes all other real parameter sets through `equihash_basic_solve` (the previous non-192,7 branch produced no solution at all). `getblocktemplate` inherits the bit and parameters for external miners automatically.
7. **Reducer hardening** — the `validate_headers` reducer rejects `nSolutionSize != expected_at(...)` (`bad-equihash-solution-size`) whenever `pprev` resolves; it stays permissive when ancestry is incomplete (re-checked contextually downstream).
8. **Observability** — `getdeploymentinfo` RPC (every deployment, evaluated for the next block) and the `zcl_state versionbits` dumper.
9. **Tests** — `lib/test/src/test_versionbits.c` with synthetic `block_index` chains and regtest-tiny parameters: exact threshold pass/fail, streak reset, LOCKED_IN stickiness, exact grace boundary, reorg across a boundary, cache-reset coherence, incomplete-ancestry fallback, graduated-difficulty window, contextual rejection of wrong-size solutions. E2e on regtest: signaling blocks to LOCKED_IN, through grace to ACTIVE, then a 68-byte-solution block extends the tip and a 36-byte block at `H_a` is rejected.

## Verification

`make -j$(nproc)` + `make lint` + `build/bin/test_parallel` per step; `make t ONLY=versionbits` for the new test group; regtest e2e via the `generate` RPC; `zcl_state versionbits` / `getdeploymentinfo` on a live regtest node showing the state transitions.

## ASIC implications of returning to 200,9

Equihash 200,9 is the most widely deployed ASIC-mined Equihash variant (Zcash and its forks; Antminer Z9/Z11/Z15-class hardware). The Bubbles move to 192,7 was an ASIC-resistance decision; returning to 200,9 opens the chain to existing 200,9 hardware.

- **Hashrate and security:** the chain joins a much deeper hashrate market. The majority-attack model shifts from rentable GPU hashrate to redirectable ASIC hashrate; the absolute cost of an attack grows with ZClassic's own 200,9 difficulty over time.
- **Miner transition:** 192,7 GPU miners become uncompetitive at `H_a`. Reaching the signaling threshold requires coordination with current pools and miners, since the tally is cast by the blocks they produce. The open-source GPU miner and the 0-fee pool give both GPU and ASIC operators a ready path on the new parameters.
- **Difficulty transients:** a hashrate change at `H_a` in either direction is absorbed by the 17-block averaging window (clamped per block) plus the graduated relief in step 5. Both directions are transient and bounded.
- The larger solution (400 → 1344 bytes) adds ~1 KB per header; bandwidth and storage impact is negligible.

## Exchange and pool coordination (the grace period)

The grace between LOCKED_IN and ACTIVE is the coordination window, and it is deterministic: from the moment of lock-in, `getdeploymentinfo` and `zcl_state versionbits` publish the exact activation height about one month ahead, so infrastructure can schedule against a known block height.

- **Pools:** switch stratum/solver backends from 192,7 to 200,9 work at exactly `H_a`. Templates from an upgraded node carry the correct version bits and, from `H_a`, the new parameters, so template-driven pools only schedule the solver-side switch. The 0-fee pool is available as a ready 200,9 target from the start.
- **Exchanges:** run an upgraded node before `H_a`; raise deposit confirmation requirements through the transition window and consider pausing deposits/withdrawals around `H_a` itself.
- **Elder nodes — fork off at `H_a` without an update (verified against the zclassicd source):** the elder's *intrinsic* PoW check (`CheckEquihashSolution`, `src/pow.cpp`) does derive (N,K) from solution size, so a 1344-byte solution verifies as 200,9 there. But `ContextualCheckBlockHeader` (`src/main.cpp`) — present since commit `d57bf7a5e` ("Deep Reorg Protection #25", 2019-08-30) — pins the expected solution size per height from the static `EquihashUpgradeInfo` table, which is 192,7 (400 bytes) for every epoch from Bubbles onward and has no versionbits awareness. A 1344-byte solution at a post-Bubbles height is rejected with `bad-equihash-solution-size` at DoS 100, which refuses the header *and bans the peer that sent it*. Any elder able to follow the chain past Buttercup (height 707,000, shipped after that commit) necessarily runs a build with this gate, so every live elder rejects post-`H_a` 200,9 blocks, bans upgraded peers, and stays on (or stalls at) the remaining 192,7 chain. Signaling itself relays fine (their version rule is `>= 4`); only post-activation blocks are affected. Elder operators must move to a C23 node — or a zclassicd patched to resolve the expected size through this deployment — before `H_a`, and the grace period is the campaign window for that. Every C23 node carries this feature from the first release, so there is no older C23 population to fork off.
- **Communication milestones,** each a block height with a date estimate: deployment ships (signaling open); streak building in `getdeploymentinfo` (ecosystem heads-up); LOCKED_IN (one-month countdown with exact height); `H_a` (switch).

## Cross-node behavior matrix (C23 vs elder zclassicd)

Both nodes verify a solution intrinsically by demuxing (N,K) from its byte length — identical 4-entry maps (`lib/crypto/src/equihash.c::equihash_solution_params` here; `src/pow.cpp::CheckEquihashSolution` in the elder): 1344 → 200,9; 400 → 192,7; 68 → 96,5; 36 → 48,5; anything else rejected. The fork is enforced one layer up, and that layer is where the two nodes diverge:

| | C23 (this tree) | Elder zclassicd |
|---|---|---|
| Expected-size source | static epoch table **+** `ehUpgrade` versionbits, via `chain_params_equihash_n_at/k_at` | static epoch table only (192,7 from Bubbles onward) |
| Gate location | `contextual_check_block_header` (`lib/validation/src/check_block.c`) | `ContextualCheckBlockHeader` (`src/main.cpp`, since `d57bf7a5e`, 2019-08-30) |
| On mismatch | DoS 100 in `validation_state`; header admission is lenient (log + reject event), hard enforcement at the `validate_headers` reducer gate and bg-validation — the block never connects | DoS 100 at header accept: header refused outright, sending peer banned |

What happens when a valid **200,9 (1344-byte)** block arrives, by height regime (mainnet):

| Height regime | C23 | Elder zclassicd |
|---|---|---|
| h < 585,318 (pre-Bubbles) | accepted (historical chain) | accepted |
| h ≥ 585,318, deployment not ACTIVE | rejected (`bad-equihash-solution-size`) | rejected + peer banned |
| LOCKED_IN → grace | rejected until `H_a` | rejected + peer banned (signal bit itself relays fine) |
| h ≥ `H_a` (ACTIVE) | **accepted and required**; 400-byte 192,7 blocks now rejected | **still rejected + peer banned** → forks off |

## Risks

- **Fast-sync tails:** a node snapshot-synced past a future `H_a` cannot walk the windows until headers backfill; it falls back to the static table until then. Once mainnet locks in, ship a checkpoint row at `H_a` as belt-and-braces.
- **Elder network assumption — resolved, negative:** the earlier claim that zclassicd "accepts 1344-byte solutions by calculating parameters from solution size" held only for its intrinsic check. Source cross-reference (zclassicd `git blame`: commit `d57bf7a5e`, 2019-08-30) shows its `ContextualCheckBlockHeader` pins solution size per height from a static table with DoS 100, so elders reject post-`H_a` 200,9 blocks and ban the peers serving them — see the elder-nodes bullet above. The sidegrade therefore requires an explicit elder-upgrade campaign during the grace period; without it, un-upgraded elders (and the exchanges/explorers running them) split onto a 192,7 remnant chain. Confirming against a live zclassicd remains worthwhile, but the source is unambiguous.
- `mine_block_pow` signature change ripples into 3 test files (mechanical, same commit).
