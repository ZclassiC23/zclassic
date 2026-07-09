# Groth16 pure-C23 Sapling prover — cure status & resume guide

**Status (2026-07-10):** 2 of 3 root-caused defects FIXED + unit-pinned;
positive prover→verifier round-trip NOT yet achieved; spend circuit not started.
All work is on branch **`fix/groth16-c23-prover`** (NOT merged). `origin/main`
ships the interim **librustzcash bridge** instead (see below) so shielded txs
work today; this cure is the ship-target that retires that bridge.

## The rule (owner directive, 2026-07-10)

**Shipped code MUST be pure C23.** Zcash's reference implementations
(librustzcash / bellman / sapling-crypto, MIT/Apache-2.0) are used ONLY as an
offline **comparison oracle** to port from and diff against — never linked into
the binary. Every file with ported/derived logic carries a header naming the
upstream project, **The Zcash developers / Electric Coin Company**, the pinned
commit `06da3b9ac8f278e5d4ae13088cf0a4c03d2c13f5`, and the license. See memory
`c23-only-zcash-attribution`.

## The problem

The in-binary pure-C23 Groth16 prover emits Sapling output/spend proofs that the
(trusted, consensus-parity) C23 **verifier rejects** — the positive round-trip
`sapling_check_output(our-prover proof)` returns **false**. The verifier is
correct (bit-for-bit parity with zclassicd, pinned by `test_snark_kat` KAT B and
the negative controls). The defect is entirely in the prover's proof
construction. Groth16 pairing is all-or-nothing: any single wrong element →
reject, with no partial signal — which is why this needs differential debugging
against a known-good reference, not guesswork.

## Root cause — three defects

1. **Output circuit R1CS mismatched the trusted setup.** `sapling_output_synthesize`
   (`lib/sapling/src/sapling_circuit.c`) emitted 7571 constraints / 7567 aux; the
   proving-key QAP requires **7827 / 7821**. A witness off the QAP can't satisfy
   the pairing. **← FIXED.**
2. **Prover MSM used dense-positional indexing.** `groth16_prove`
   (`lib/sapling/src/groth16_prover.c`) paired query base `k` with `witness[k]`,
   but bellman's `Parameters` file stores the A/B query bases **density-filtered**
   (points at infinity removed), so `a_len,b_len < num_vars` and every scalar past
   the first density gap was mispaired. **← FIXED.**
3. **Spend circuit is a stub.** `sapling_spend_synthesize`
   (`lib/sapling/src/sapling_circuit.c:87`) emits ~dozens of constraints vs the
   reference **98777 / 98638 aux** → spend proofs non-verifying and
   non-deterministic. **← NOT STARTED.**

## What's fixed (committed on the branch)

- **`a1ca4c04a` — MSM density filter.** New `groth16_density_query_map()`
  reconstructs bellman's `DensityTracker` predicate from the constraint matrices
  (a var is dense in matrix M iff it appears with nonzero coeff in any
  constraint's M-LC), and walks the filtered query arrays with correct
  scalar↔base pairing: A = [all inputs, full density] ++ [dense aux];
  B = [dense inputs] ++ [dense aux]. H and L queries were already correct.
  Pinned by `test_groth16_msm_density` (asserts filtered ≠ dense-positional).
  Attribution header on `groth16_prover.c`.
- **`fdf80aa9c` — output circuit R1CS = 7827/7821.** All 14 top-level namespaces
  now match the reference trace section-for-section. Dominant gap: the `g_d`
  `EdwardsPoint::repr` used two **non-strict** 256-bit unpacks instead of the
  strict canonical decomposition. New `gadget_unpack_bits_strict`
  (`lib/sapling/src/circuit_gadgets.c`) ports `num.rs::into_bits_le_strict` + the
  `kary_and` chain proving `a ≤ r−1` (255 bits MSB-aligned, ~132 AND/coord). Five
  booleanization sites (value/rcv/esk/rcm/pk_d-y) switched to `gadget_witness_bits`
  (bits only, no packing constraint) and pk_d-y is now 255 bits (Fr::NUM_BITS),
  not 256. Pinned by `test_sapling_output_circuit_shape`
  (num_constraints==7827, num_aux==7821, inputs==5). Attribution on
  `circuit_gadgets.c` + `sapling_circuit.c`.

Verify state after these: circuit num_aux (7821) now EQUALS pk `l_len` (7821)
— was 7567. `self_verify` is still FALSE (defect #2/#3 residue, next section).

## What remains

### A. Output positive round-trip (branch tip `ddc832a4e`, WIP — interrupted)

`test_groth16_selfverify.c` (run `ZCL_TEST_ONLY=groth16_selfverify`) was extended
into a **stage-by-stage bisection** of the output prover→verifier path — it is
the debugging tool, not a merge gate. It prints, in order:
`pk h/l/a/b/inputs/ic_len`; circuit `num_vars/inputs/constraints`; the density
map (`A_count` vs `pk.a_len`, `B_count` vs `pk.b_len` — **if these still differ,
defect #2's fix isn't fully aligning with this circuit**); **R1CS satisfied?**
(with first unsatisfied constraint index); `g2_msm == naive?`; `self_verify`
(pk.vk + circuit public inputs, no serialize); `REF-PROOF verify` (our verifier
against a **real bellman proof** — must be TRUE, proves the verifier is sound);
and `FULL-WITNESS` / `REF-WITNESS` A/B/C diff against a reference witness.

**Resume method:** run the bisection; the first stage that diverges names the
bug. Likely remaining culprit is the density-map alignment for THIS circuit
(the MSM fix logged `A density 6037 != a_len 6298` on the *old* 7571-constraint
circuit; re-check against the now-correct 7827 circuit) or a witness-assignment
mismatch. Compare intermediates against the reference using the fixed test-only
RNG hooks (`sapling_set_test_rng_hook`, the Groth16 r/s hook) with identical
inputs so our proof and a bellman proof are bit-comparable.

### B. Spend circuit port (not started)

Port `sapling_spend_synthesize` faithfully from sapling-crypto
`circuit/sapling/mod.rs` (the `Spend` struct's `synthesize()`) against the
reference trace: ak validity + spend-auth randomization (rk = ak + [alpha]G),
nk = [nsk]·PROOF_GENERATION_KEY_BASE, ivk = BLAKE2s(ak‖nk), g_d/pk_d, Pedersen
note commitment, value commitment cv, the 32-level Merkle authentication path to
the anchor, nullifier nf = BLAKE2s(nk‖rho) — matching the reference **constraint
order** exactly. The new `gadget_unpack_bits_strict` / `gadget_witness_bits` are
prerequisites it reuses.

## Ground-truth reference traces (preserved on-branch)

Extracted from pinned librustzcash `06da3b9ac8f278e5d4ae13088cf0a4c03d2c13f5` via
a throwaway Rust harness using sapling-crypto's `TestConstraintSystem` (records
named/ordered constraint paths). Preserved as git objects on the branch (they
survive scratchpad deletion) at:

- `reference/groth16-traces/output_circuit.trace` — 7827 named constraints,
  header `num_constraints=7827 num_aux=7821 num_inputs=6` (5 + bellman's ONE).
- `reference/groth16-traces/spend_circuit.trace` — 98777 named constraints.
- `reference/groth16-trace-harness/` — the Rust extraction harness (2 files).

Recover after a scratchpad wipe:
`git show fix/groth16-c23-prover:reference/groth16-traces/output_circuit.trace`
or check out the branch into a fresh worktree (`cp -a vendor/lib` first).

## The interim bridge on origin/main (what ships TODAY)

`origin/main` wallet proving delegates to the SHA-pinned canonical
**librustzcash** (`vendor/lib/librustzcash.a`, commit `06da3b9ac8f2`), gated by a
**fail-closed self-test**: at init a real Spend+Output+binding-sig bundle must
round-trip through the independent pure-C23 consensus verifier before the prover
flips READY (`lib/sapling/src/sapling_prover_c23.c`). Consensus verification is
100% C23. The full t→z path is green end-to-end (`test_shielded_payment_gate`).
This bridge is **interim and oracle-verified** under the C23-only rule — the cure
above replaces it. (Live shielded sends are additionally gated by the wallet
persistence work, separate from proving.)

## Resume checklist

1. Fresh worktree off the branch: `git worktree add <path> fix/groth16-c23-prover
   && cp -a vendor/lib <path>/vendor/` (worktrees can't link without the
   gitignored static libs).
2. Build; run `ZCL_TEST_ONLY=groth16_selfverify build/bin/test_zcl`; read the
   bisection output top-to-bottom; the first diverging stage is the bug.
3. `test_sapling_output_circuit_shape` and `test_groth16_msm_density` must stay
   green (they pin the two landed fixes). `snark_kat` and `shielded_payment_gate`
   must stay green (they pin the verifier + the bridge).
4. On a passing output round-trip: commit, then start the spend circuit against
   `spend_circuit.trace`.
5. Any ported file gets the Zcash attribution header. Rust reference stays in a
   scratch harness, never in the binary build graph.
