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
- `report-NNNNNN.json` — the KPI report: admitted/excluded LOC (production
  and test separately), downstream-used LOC (admitted packages pinned by
  another scope's dependency closure), growth deltas against the previous
  sequence's report (`--previous-report`), per-scope breakdown, missing
  evidence bits, and the honesty disclosures.

## Live status

`zcode commons corpus status` reads the resident signed checkpoint at
`<datadir>/zcode/corpus/checkpoint.hex` when the census was run with
`--install <datadir>` (or `CORPUS_INSTALL=<datadir>` via the make target).
The reply carries `resident_checkpoint`: `loaded` (decoded, signature and
shape re-validated), `rejected` (present but failed validation — logged,
never trusted), or `missing` (the historical `checkpoint_missing`
rendering).

## Re-run

```bash
make corpus-census            # builds the driver, runs a SMOKE census
                              # into build/corpus-census/ (never corpus/)
```

Advancing the committed sequence is explicit:

```bash
make corpus-census CORPUS_OUT=corpus CORPUS_SEQUENCE=<n> \
    CORPUS_PREDECESSOR_ROOT=<seq n-1 checkpoint root> \
    CORPUS_CUTOFF_HEIGHT=3050000 CORPUS_CUTOFF_MTP=1754000000 \
    CORPUS_QUALITY_ATTESTED=1 \
    CORPUS_PREVIOUS_REPORT=corpus/report-<n-1>.json \
    CORPUS_INSTALL=<datadir>
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

## Admitting a published Commons package

`scopes.def` has a second line form for packages published through the C23
Commons package store:

```
package <name> | root <64hex> | store <datadir> | kind <human|ai|import> | spdx <id>
```

Unlike repo scopes (enumerated via `git ls-files`), a package scope is
enumerated from the **package store** at `<datadir>/zcode`, so every
evidence bit binds the exact published bytes, not a working tree:

- `root` is the package manifest root. The census loads the stored manifest
  and re-derives the root; a mismatch refuses the scope.
- Exactly one signature-verified release under `releases/` may name the
  root; its declared license must equal the `spdx` field, and the census
  re-derives the recipe root from `recipes/<recipe-root-hex>`.
- Files are reassembled from the chunk-hash-verified CAS, read-only — the
  census never opens the store through `vcs_package_store_open`, so no
  recovery sweep, GC, or access-count mutation happens as a side effect.
- REPRODUCIBLE requires >= 2 DISTINCT byte-identical confined build
  receipts filed under `<datadir>/zcode/receipts/`; QUALITY maps to
  "confined build+test receipt green". The assignment author binding is the
  release publisher's pubkey.

The one pipeline that produces all of this evidence is the package factory
(`tools/package_factory.c`, `build/bin/package-factory`): it gates the
package layout, prepares/seals/signs the release, publishes into two
independent local stores, files the second distinct confined-build receipt
(quick + standard flag profiles on one host — disclosed), verifies
reproduction, and optionally registers the scope line
(`--register-corpus --census-def corpus/scopes.def`). Prove it end to end
with:

```bash
make package-factory-selftest
```

After registering a package scope, re-run the census (above) so the signed
checkpoint binds the new package evidence.

## Honesty disclosures (carried in every report)

- **Founding self-screen admission.** Every scope's admission is a
  self-signed `SELF_SCREENED` `commons_admission.v1` (tier 0); zero
  independent operator groups have participated.
- **Reproduction** is dual-worktree source rederivation (`git worktree`
  at HEAD, byte-identical release roots), NOT independent build
  reproduction. Scopes with uncommitted content honestly lose the bit.
  Package scopes use the stronger receipt binding (>= 2 distinct
  byte-identical confined build receipts) — but on ONE host; independent
  operator reproduction is future work.
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
