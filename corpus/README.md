# corpus/ — the C23 corpus odometer

This directory holds the canonical outputs of the **C23 corpus census**: a
signed, simulation-only lower-bound checkpoint (`c23_corpus_checkpoint.v1`)
counting the C23 code in this repository plus, eventually, existing Commons
packages, scope by scope. The counting rules live in the pure census core
(`lib/vcs/include/vcs/zcode_c23_corpus_census.h` — the authority); the
offline driver is `tools/corpus_census.c`, built as
`build/bin/corpus-census` and wired as `make corpus-census`.

## Contents

- `scopes.def` — the canonical scope definition (one scope per line;
  header comment documents the format, the file-claim precedence, and the
  declared provenance).
- `checkpoint-NNNNNN.hex` — the signed checkpoint wire (lowercase hex).
- `shard-NNNNNN-<i>.hex` — the corpus shard wires the checkpoint binds
  (≤28 entries each, inside the 8192-byte inline reader bound).
- `evidence-NNNNNN.json` — every per-scope source assignment and admission
  wire, every constructed root, and the exact construction recipes.
- `report-NNNNNN.json` — the KPI report: admitted/excluded LOC, per-scope
  breakdown, missing evidence bits, and the honesty disclosures.

## Re-run

```bash
make corpus-census            # builds the driver, runs the canonical args
```

The cutoff coordinates and the lint attestation are operator inputs:

```bash
make corpus-census CORPUS_CUTOFF_HEIGHT=3050000 \
    CORPUS_CUTOFF_MTP=1754000000 CORPUS_QUALITY_ATTESTED=1
```

The signer seed lives OUTSIDE the repo at
`$HOME/.config/zclassic23/corpus-census-signer.seed` (32 raw bytes, mode
0600; generated from the kernel CSPRNG on first use, with the new pubkey
logged). Same tree + def + seed + cutoff args give byte-identical
artifacts — no wall-clock enters any signed object.

## Verify

```bash
build/bin/zclassic23 zcode commons corpus verify \
    --checkpoint="$(cat corpus/checkpoint-000001.hex)"
build/bin/zclassic23 zcode commons corpus shard verify \
    --shard="$(cat corpus/shard-000001-0.hex)"
```

Both return `"verified":true` after decoding and validating the wires
(signature, canonical order, aggregate consistency). The readers are
fail-closed: anything malformed is rejected with a named reason.

## Honesty disclosures (carried in every report)

- **Founding self-screen admission.** Every scope's admission is a
  self-signed `SELF_SCREENED` `commons_admission.v1` (tier 0); zero
  independent operator groups have participated.
- **Reproduction** is dual-worktree source rederivation (`git worktree`
  at HEAD, byte-identical release roots), NOT independent build
  reproduction. Scopes with uncommitted content honestly lose the bit.
- **Quality** is the operator's `--quality-attested` flag (`make lint`
  pass state at census time), not an independent review. With it unset,
  every entry is excluded `REVIEW_REQUIRED` and admitted LOC is zero —
  the correct first-checkpoint outcome.
- **Durable hosting: none yet.** `possession_root` is recorded in the
  report only; every entry carries `possession_root = 0` and no DURABLE
  flag (nothing is 5-ACK/3-operator-group durable).
- **Simulation-only, not owner-approved.** There is no live ZC23 token
  economics.
- `source_kind` is **declared provenance**: no per-file authorship marker
  exists in-tree, so all scopes are declared `human`.
- `vendor/` and `core/` are out of corpus by design (third-party material
  and the byte-sealed consensus core).
