# Session handoff — 2026-07-10 (simulator coverage push)

Verify live/dev state before trusting this doc (`zcl_status` via MCP). This
session was pure simulator/test work; the live and dev nodes were untouched.

## TL;DR

`origin/main` @ **`a4f4ce1dd`** (pushed), **503 test groups green**. Twelve
deterministic simulator advances shipped this session, all gated by the local
pre-push CI (`make lint` + focused tests). **Codex is OUT — Claude-only** (owner
directive 2026-07-10; persisted in global CLAUDE.md + memory).

## Shipped to origin/main this session (12)

1. txkit SEGV fix (heap-alloc 65 MB `struct wallet` in `simnet_wallet.c`).
2. `simnet_wire` **step C** byzantine bridge (8 invalid-block classes +
   invalid-header; the mode-3 fix routed the consensus-unchanged monitor's
   observation through the const `coins_view_cache_recompute_commitment`).
3. **Step F** `tools/sim/wire_sweep.c` nightly seed-fuzzer +
   `check-wire-harness-security-gate` (proven teeth). Found + (with Step E)
   fixed a real `BAD_HANDSHAKE_GARBAGE_AFTER_VERACK` hang.
4. wire **Step D1** per-link partition/recovery scenarios.
5. **Sapling Lane A** sim-local activation profile (`simnet_activate_sapling_at`,
   params value-copy only, no mainnet leak, empty-tree-root stamp).
6. **Sapling Lane B** seedable prover RNG (`#ifdef ZCL_TESTING`
   `sapling_set_test_rng_hook`, default-NULL, `nm`-verified production-safe).
7. **ZNAM** SET_RECORD/SET_TEXT simnet-level negative coverage.
8. wire **Step E** (replay/reorder/bandwidth adversaries + the bad-handshake
   ingress idle-gate fix; root cause: `simnet_wire_idle` treated an incomplete
   recv as busy).
9. transparent **double-spend** rejection (`test_simnet_doublespend.c`).
10. intra-block **chained-tx** ordering (`test_simnet_chained_tx.c`).
11. block **sigop-limit** rejection (`test_simnet_block_sigops.c`, `bad-blk-sigops`).
12. **duplicate-input**-in-tx rejection (`test_simnet_duplicate_input.c`,
    `bad-txns-inputs-duplicate`).

## 🚨 CRITICAL FINDING — the Groth16 prover is broken (do NOT z-send on live)

Sapling Lane C proved out the whole in-sim shielded pipeline (in-sim depth-32
note-commitment tree + anchor/witness, real `hashFinalSaplingRoot` stamp, t→z
and z→z built with the REAL prover and driven through the REAL verifier,
memo/rcm decrypt round-trip, **real nullifier durable-set + shielded
double-spend reject**, seeded txid determinism, Groth16 r,s test-only RNG
hook). In doing so it surfaced:

**The in-binary pure-C23 Groth16 PROVER emits Sapling output/spend proofs the
consensus VERIFIER REJECTS.** The positive round-trip
`sapling_check_output(real-prover proof)` returns **false**, independently
confirmed by `test_snark_kat` KAT B (which only pins the NEGATIVE cases).
**zclassic23 cannot currently build a valid shielded tx** — a real z-address
send on live ZCL would produce an invalid proof the network rejects. Verifier is
trusted (consensus parity with zclassicd); the defect is in the prover's proof
construction. Full detail: memory `project_groth16_prover_verifier_gap_2026-07-10`.
An Opus deep-crypto lane (agent `a26e5003`) was launched to root-cause it.

## Workflow-1 branches — ALL 4 MERGED + PUSHED (done)

All four Workflow-1 (value/structure) results are now on origin/main
(@`3f2367674`, 507 groups green): `sim/value_inflation`
(`bad-txns-in-belowout`), `sim/fee_range` (negative-fee → `bad-txns-in-belowout`;
the literal fee-negative/outofrange strings are unreachable via simnet — shadowed
/ need >MAX_MONEY funding — documented in the test), `sim/empty_vin_vout`
(`bad-txns-vin-empty`/`bad-txns-vout-empty`), `sim/input_value_range`
(`bad-txns-inputvalues-outofrange`).

**Registration-merge recipe (for the Lane C branch below, and future lanes):**
```
git merge --no-ff <branch>
# additive conflicts in: lib/test/src/test_parallel.c (X-macro line — keep ALL
#   X(simnet_*) tokens on the ONE line), lib/test/src/test.c (dispatch),
#   lib/test/include/test/test_helpers.h (decl), agent_impact_rules.def (rule),
#   docs/SIMULATOR.md (row). Resolve as UNION (keep both sides).
# docs/CODEBASE_MAP.md test_groups: set to `make check-doc-counts` code value.
make -j$(nproc) build-only && make t ONLY=<group> && make check-doc-counts
git commit --no-edit && git push origin main   # pre-push CI is the gate
```

## Groth16 prover — ROOT-CAUSED + isolation test LANDED

The Groth16 lane definitively root-caused the prover↔verifier gap (memory
`project_groth16_prover_verifier_gap_2026-07-10` has the full evidence):
1. Output circuit R1CS mismatches the trusted setup — `sapling_output_synthesize`
   (`lib/sapling/src/sapling_circuit.c` ~266) emits 7571 constraints / num_aux
   7567 vs the required 7827 / 7821 → witness off the QAP → pairing can't hold.
2. Dense positional MSM vs bellman's density-filtered A/B queries
   (`lib/sapling/src/groth16_prover.c` :903/:917/:931) — needs density-aware multiexp.
3. Spend circuit (`sapling_spend_synthesize` ~77) is a STUB (~dozens of
   constraints vs ~100K) → spend proofs non-verifying AND non-deterministic.
A self-verify isolation test (`lib/test/src/test_groth16_selfverify.c`, run
`ZCL_TEST_ONLY=groth16_selfverify`) is **merged to main** (opt-in, not in the
default pool). Fixing the prover is a multi-week circuit reimplementation, not a
patch — pairing is all-or-nothing.

## Sapling Lane C — NOT merged (branch `sapling-lane-c-shielded-send`)

The Lane-C test-fix lane made the shielded-send test's txid-determinism robust
(3 dedicated test-only RNG hooks: sapling r, Groth16 r/s, RedJubjub nonce — all
`#ifdef ZCL_TESTING`, nm-verified production-safe; asserts txid-determinism IFF
spend-proof-determinism, auto-tightening when the prover is fixed). That test is
green standalone AND in full `test_parallel`.

**BUT merging Lane C onto main is BLOCKED by a real regression:** Lane C's
`simnet.c` change adds a **per-tx contextual-verifier drive** to the mint path,
and `test_simnet_input_value_range`'s `value_in == MAX_MONEY` block then
**SIGABRTs deterministically under `test_parallel`** (aborts right after "build
value_in == MAX_MONEY spend"; each `test_parallel` group is a separate forked
process, so this is the shared `simnet.c` code path, not cross-group state). The
Lane C merge was made + reverted this session (main restored to green 507).
**To land Lane C, the next agent must:** (a) fix the contextual-drive so a
MAX_MONEY input doesn't abort (guard/handle the boundary), AND (b) move the
params-heavy `simnet_sapling_shielded_send` (and consider `snark_kat` /
`sapling_prover_rng_determinism`) OUT of the default fast `test_parallel` pool
into an opt-in params gate, per `docs/work/sapling-sim-spike.md` (real Groth16
proving is seconds and memory-heavy — it doesn't belong in the fast pool). Then
re-merge, resolve the additive registration/doc-count conflicts, verify green in
BOTH contexts, push.

## Architectural note for future simnet coverage lanes

`simnet` drives **`connect_block`/`check_block` only**, NOT
`contextual_check_block`. So finality (`bad-txns-nonfinal`), Overwinter expiry
(`tx-overwinter-expired`), and other contextual checks are OUT of simnet's reach
(they run in the reducer stage `script_validate_contextual.c`); they're covered
by `test_bip113_bip65.c` / `test_transaction.c`. Don't propose those as simnet
lanes. Negative consensus cases at the `check_block` level are largely covered by
the byzantine bridge (`simnet_byzantine.c`: bad_merkle, bad_cb_amount, bip30_dup,
missing_spend, immature_spend, negative/overflow_output, oversize_vtx, bad_bits,
bad_timestamp, invalid_pow). Investigate-first before adding — several proposed
lanes correctly returned PREMISE_FALSE.

## Workflow

Orchestrator (this session) + native Claude subagent lanes (Opus hard/crypto,
Sonnet scoped, all in isolated git worktrees). Each lane rebases onto current
main first (`git merge main`) to avoid stale-base registration friction, adds
its own `agent_impact_rules.def` mapping, commits a local branch, does NOT push;
the orchestrator merges + gates + pushes. Keep that pattern.
