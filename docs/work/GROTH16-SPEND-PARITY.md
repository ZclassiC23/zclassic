# Groth16 SPEND-circuit differential parity — scoreboard (present tense)

The native pure-C23 Sapling **spend** circuit is being ported gadget-by-gadget
to match bellman's `Spend::synthesize` (pinned librustzcash commit
`06da3b9ac8f278e5d4ae13088cf0a4c03d2c13f5`). Pairing is all-or-nothing, so a
partial port yields **no** green Groth16 spend round-trip yet — the value landed
so far is a *proven-at-parity prefix*, guarded by a standing differential oracle.

This doc is the honest scoreboard: what is proven at parity vs the reference,
and what is pending.

## The gates (all under the `groth16_selfverify` test group)

| Lane | File | What it proves |
|------|------|----------------|
| H2 | `lib/test/src/groth16_spend_oracle.c` | Native `nsk_to_nk` / `crh_ivk` / `ivk_to_pkd` / `compute_cm` / `compute_nf` == librustzcash, byte-for-byte, for one pinned KAT witness. |
| H3 | `lib/sapling/src/sapling_circuit.c` + shape gate in `test_groth16_selfverify.c` | Ported spend sections **1..7** synthesize with per-section cumulative constraint counts equal to the reference trace; in-circuit `nk`/`rk` wires carry reference-correct points; synthesis deterministic. Single witness. |
| **H4** | `lib/test/src/groth16_spend_parity.c` | **Standing** differential parity oracle over a **corpus** of witnesses — generalizes H2+H3 and auto-tightens as H3 ports more sections. |

Run: `make t-fast ONLY=groth16_selfverify` (params-free; no `~/.zcash-params`,
no proving key required). `make t-fast ONLY=snark_kat` is the sibling KAT gate.

## What the H4 parity oracle proves (per witness, whole corpus)

For every witness in a deterministic corpus (index 0 = the pinned H2 KAT
witness; 1..N distinct canonical scalars):

- **(A) Section-boundary parity, auto-tightening.** Each recorded section's
  cumulative constraint count equals the pinned 28-entry reference trace table
  (`REF_SECTIONS[]` in `groth16_spend_parity.c`). The oracle diffs only the
  sections the native circuit *actually records* (`n_sections` from the traced
  synthesis), so when the H3 port advances from 7 sections to 8, 9, … the new
  section's boundary is validated against `REF_SECTIONS[i]` **with no edit to
  the oracle** — it tightens itself off the reference boundaries.
- **(B) Structural invariance.** An R1CS circuit's shape must not depend on the
  witness. Every corpus witness must produce a byte-identical section shape
  (constraints/vars/inputs per section) to witness 0 — a class of unsoundness
  the single-witness H2/H3 gates cannot see.
- **(C) Per-wire value parity vs the reference archive.** The in-circuit `nk`
  wire (`[nsk] ProofGenerationKeyGenerator`, section 7) is compared
  byte-for-byte to `librustzcash_nsk_to_nk` **for every witness** — the
  differential against ground truth, not just the KAT. The `ak` (section 1) and
  `rk` (section 4) wires are cross-checked against the native scalar derivations
  (the reference archive exports no `ak`/`rk` FFI: `ak` is a private circuit
  input, `rk = ak + [ar]G` is internal).
- **(D) Determinism.** Re-synthesizing an identical witness yields a
  byte-identical witness vector.

On the first divergence in any category the oracle prints the offending
`(witness index, section name, expected vs actual)` — it flags, never hides.
A negative-control (corrupting a reference boundary) turns the gate RED with a
`FIRST DIVERGENCE` line, so the gate is not hollow.

## Coverage — PROVEN vs PENDING

- **PROVEN at parity:** reference sections **1..7** — cumulative **2032 / 98777**
  constraints (~2.1%). ak on-curve/not-small-order, ar/nsk bit decompositions,
  the two fixed-base multiplications (`[ar]SpendAuthGenerator`,
  `[nsk]ProofGenerationKeyGenerator`), `rk = ak + [ar]G`, and rk inputize — all
  byte-identical to the reference trace across the whole corpus, with the `nk`
  wire pinned to librustzcash.
- **PENDING (H3 port):** sections **8..28** — EdwardsPoint::repr helpers, the two
  blake2s hashes (`ivk` §10 and `nf` §27, ~21006 constraints each), variable-base
  `pk_d` (§13), value commitment (§14), note-commitment Pedersen hash (§17), the
  32-level Merkle path (§21, 44224 constraints), and nullifier packing (§28).
  Target: **98777** constraints, **98638** aux, **8** inputs (7 public + ONE).

The oracle re-reports this scoreboard (`parity coverage: N/28 sections,
C/98777 constraints`) on every run, so the number moves the moment H3 lands the
next section — no edit to H4 required.
