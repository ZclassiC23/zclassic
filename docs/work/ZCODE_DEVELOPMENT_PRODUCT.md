# ZCODE C23 Development Product

Status: owner-directed v0.1 product specification and measurement ledger,
started 2026-08-07. This is the active contract for turning the existing ZCODE
development-network primitives into one ordinary C23 development loop.

## Mission and product promise

> **ZClassic23 is a metaverse where people and AI create real things together,
> and nobody owns the world they build in.**

For this product, a "real thing" is an exact C23 source change with bounded
context, an isolated candidate, reproducible build and test evidence, review,
and an explicit human decision. The developer owns that decision. An AI may
propose and repair a candidate; it cannot accept, publish, or assign authority
to its own output.

The ordinary interaction is intentionally small:

```text
zclassic23-dev zcode project inspect --input='{"workspace":"."}'
zclassic23-dev zcode work start --input='{
  "workspace":".",
  "goal":"Make the parser reject overflowing lengths",
  "profile":"standard"
}'
zclassic23-dev zcode work run --input='{"work":"latest","adapter":"manual"}'
zclassic23-dev zcode work status --input='{"work":"latest"}'
zclassic23-dev zcode work accept --input='{"work":"latest"}'
```

The happy path accepts no raw roots, canonical wire hex, timestamps, toolchain
hashes, proof-policy wires, or action IDs. Expert commands and full roots remain
available underneath and in an `expert` result section.

## Authority and reuse audit

This is integration, not a new protocol. The following existing owners already
bind every authoritative fact required by v0.1:

| Fact | Existing authority |
|---|---|
| exact source and candidate trees | ZVCS source capture and CAS |
| bounded model context | `zcl.zcode.agent_context.v1` and code index |
| goal, limits, recipe, lock, policy | `zcl.zcode.task.v1` |
| permitted edits | `zcl.zcode.write_scope.v1` |
| source change | `zcl.zcode.patch.v1` and `candidate.v1` |
| fixed build/test/fuzz input | `package_action_input.v1` |
| execution evidence | signed ZBuild `work_receipt.v1` |
| evidence evaluation | `proof_set.v1` |
| review | `review.v1` plus signed review work receipt |
| lifecycle | FRONTIER, CANDIDATE, and PROVEN lane receipts |
| human acceptance/publication | existing acceptance and accepted-lane owners |

Therefore v0.1 adds **no canonical domain**. Project summaries, proof-profile
names, adapter packets, diagnostic capsules, human work IDs, and work-session
status are display or rebuildable local projections. They are never accepted
as authority without reloading and re-verifying the full canonical objects.
No second CAS, scheduler, task authority, package format, proof system,
identity system, worker ledger, transport, or truth database is permitted.

## P0 born-red workflow measurement

The current expert workflow was measured from commit
`32671b9bc8b9413a94a5073e2c4d4764496abe39` in isolated `/tmp` workspaces and
datadirs. No service or live datadir was used. The packages are permissively
licensed: `zclassic23/sha3` (MIT), `zclassic23/codec` (Apache-2.0),
`zclassic23/base` (Apache-2.0 with preserved upstream notices), and the small
MIT `fixture/tiny-lines` standalone library.

The first command exposed a correctness defect before measurement could begin:
the documented numeric `publisher_sequence` was rejected by input
normalization, while a string passed normalization and was rejected by the
handler. The born-red regression was added first. Commit `32671b9bc` fixes the
normalizer and passed `command_input_bounds`, `command_registry_catalog`,
`native_api_contract`, lint, and the 903-group pre-push suite (22 documented
self-skips).

### Exact observed inputs and outputs

`zcode package dev prepare` derived the source authority but required a
compressed publisher key and sequence, and returned release, manifest, recipe,
dependency-lock, and API-capsule wire hex in addition to their roots:

| Project | Project bytes | Files | Public headers | Dependencies | Prepare time |
|---|---:|---:|---:|---:|---:|
| SHA3 | 17,592 | 7 | 1 | 1 | 0.05 s |
| codec | 27,589 | 7 | 1 | 1 | <0.1 s |
| base | 68,638 | 20 | 9 | 0 | <0.1 s |
| tiny-lines | 1,974 | 7 | 1 | 0 | 0.07 s |

`zcode package dev improve` advertises 39 possible keys. Its plan path
required eight expert inputs:

```text
workspace, dependency_lock_hex, write_scope_csv, acceptance_recipe_hex,
model_policy_root, goal, proof_policy_hex, expires_unix
```

In practice it also required `mode=plan`, an isolated datadir, and an exact
`context_symbol` to obtain useful context. The caller manually supplied a
36-byte proof-policy wire, the full recipe and dependency-lock wires, a
64-hex model policy root, write-scope CSV, and an epoch expiry. Plan results
led with nine roots and told the user to perform an adapter handoff that no
command implements.

| Task class / project | Requested symbol | Selected / total bytes | Files | Time | Result |
|---|---|---:|---:|---:|---|
| seeded split-write repair / SHA3 | `sha3_256_write` | 11,304 / 17,592 (64.3%) | 1 | 0.35 s | `AWAITING_CANDIDATE` |
| malformed/overflow repair / codec | `zcl_codec_read_u16_bytes` | 9,776 / 27,589 (35.4%) | 1 | 0.50 s | `AWAITING_CANDIDATE` |
| bounded checked API / base | `zcl_size_mul` | 0 / 68,638 | 0 | 0.64 s | failed: inline public API was not an exact indexed symbol |
| bounded checked API / base retry | `zcl_result_make` | 1,755 / 68,638 (2.6%) | 1 | 0.22 s | `AWAITING_CANDIDATE`, but context did not match the goal |
| null/UB repair / tiny-lines | `tiny_count_lines` | 210 / 1,974 (10.6%) | 1 | 0.36 s | `AWAITING_CANDIDATE` |

Compile/test iterations were zero because the missing adapter stage prevented
candidate admission. No task reached CANDIDATE or PROVEN. The measurement did
not fabricate a candidate or claim an end-to-end result: the attempted current
workflow is born-red specifically because its documented sequence ends at an
unimplemented manual handoff. Even after a caller manually creates a candidate,
the foreground CLI queues ZBuild and exits; it provides no single command that
runs the worker, gathers evidence, reviews it, and returns to the human.

The present goal-to-acceptance path is at least seven product steps—prepare,
plan, external context extraction/edit, admit, worker execution, evidence, and
accept—and exposes roots or wires at every boundary. This establishes the v0.1
baseline: **more than five commands, at least eight expert fields, three manual
wire/root constructions, no adapter front door, and no human result screen.**

## Product invariants

1. The authoritative workspace is read-only until explicit human acceptance.
2. Every candidate attempt is captured in an isolated workspace and a distinct
   canonical candidate tree before execution.
3. Project and work aliases resolve to and re-verify full canonical roots.
4. Profiles only compile into exact existing proof policies; they never weaken
   explicit project requirements.
5. Context selection is deterministic, bounded, explained, and non-authoritative.
6. Fixed build, test, fuzz, and review actions create the only usable evidence.
7. Model identity is provenance, never truth or acceptance authority.
8. Read commands do not create a workspace, CAS, datadir, or projection.
9. Every blocked state names the stage, preserved evidence, retry safety, and
   next safe command.
10. A human alone accepts or rejects the exact candidate and evidence.

## Ordered implementation slices

### P1 — project inspection and initialization

Add `zcode project inspect`, `zcode project init plan|commit`, and
`zcode project status`. Inspection derives package name/layout, headers,
sources, tests, include directories, libraries, existing package metadata,
recipe, target-inclusive lock, likely write scopes, resource ceilings, and a
suggested proof profile without writing. Initialization is plan/commit,
correctable, symlink/special-file rejecting, overwrite refusing, and stores
only the existing canonical objects.

### P2 — named proof profiles

Compile `quick`, `standard`, `strong`, and `release` into exact existing
`proof_policy.v1` fields. Quick means build and declared tests. Standard adds
warning-fatal compile and sanitizers. Strong adds deterministic fuzz and local
reproduction. Release adds distinct review and approved reproduction
requirements. The response always exposes the expanded policy and root.

### P3 — bounded goal context

Tokenize the goal and search indexed symbols, signatures, paths, text, callers,
callees, include edges, and tests in a deterministic order. Exact symbol and
signature matches outrank broad text. Each excerpt says why it was selected;
dropped candidates and budget exhaustion are explicit. Report selected bytes,
total bytes, files, symbols, and generation time. The ordinary target is below
256 KiB; exact-symbol overrides remain available.

### P4 — one work front door

Add `zcode work start|run|show|status|cancel|accept` as a thin orchestration
service over existing owners. Derive state from canonical objects rather than
creating another workflow truth table: PLANNED, AWAITING_CANDIDATE,
CANDIDATE_ADMITTED, BUILDING, REPAIR_NEEDED, EVIDENCE_READY,
READY_FOR_ACCEPTANCE, ACCEPTED_CANDIDATE, PROVEN, BLOCKED, or CANCELLED.

### P5 — model-neutral handoff

The `manual` adapter exports a bounded packet and isolated candidate workspace.
One opt-in installed adapter may run through a fixed executable registry. It
accepts no arbitrary shell, receives a scrubbed allowlisted environment, sees
no wallet/datadir/SSH/node credentials, writes only inside the candidate, has
strict output/time limits, and cannot accept or publish. Unavailable, refused,
and timed-out are typed outcomes.

### P6 — bounded repair

Permit at most three candidate attempts. Each failed fixed action produces a
bounded diagnostic capsule containing the goal, prior patch, relevant compiler
or test diagnostic, and related excerpts—not an unbounded build log. Preserve
parent candidate, changed files, supplied context, adapter identity, elapsed
time, and resources for each separately captured attempt.

### P7 — review authorship

Manual and adapter review consume only the exact candidate, patch, public API
delta, immutable non-review proof set, goal, and policy. Review cannot create
build/test evidence, edit, or accept. Independent profiles reject an
author-self-review; conflicts remain visible.

### P8 — human result

Status leads with goal, state, changed files and lines, API changes, build and
test results, sanitizer and fuzz results, reproduction grade, review verdict,
risks, scope violations, and next safe command. Roots are grouped under
`expert`.

### P9 — self-hosting and benchmark

Use this exact product path to implement and accept at least one subsequent
improvement. Run the frozen twelve-task benchmark across at least three
projects: four seeded repairs, three bounded APIs, three malformed/UB/
portability repairs, and two intentionally impossible or out-of-scope goals.
Record all failures; do not edit tasks after seeing results.

### P10 — fresh-checkout acceptance

Provide a five-minute walkthrough and a hermetic target proving workspace
immutability before acceptance, candidate isolation, scope refusal,
candidate-bound evidence, byte-identical projection rebuild, restart behavior,
exact acceptance binding, and non-reachability of wallet/token/custody/
deployment/consensus paths.

## Implementation ledger

| Slice | Integrated commit | Ordinary inputs removed | Commands removed | Context effect | Production delta | Measured effect |
|---|---|---|---:|---|---:|---|
| P1 inspect | `3a43baeeb5c574cf5199c56b7e44dd2962442eb6` | publisher key, sequence, reward address, chain ID, manifest/recipe/lock wires | 1 expert prepare invocation | reports project bytes without creating `.zvcs` | recorded in commit | one required field: `workspace` |
| P2 profiles | `aa3a4c2c71dd40a704753b1e8159f7b99f0b4c6e` | proof-policy wire and manual policy fields | 0 | none | recorded in commit | `quick|standard|strong|release` expand to inspectable existing policy objects |
| P3 goal context | `20a698e33c184c54c03aaf8fb1a159394a1af458`, `0a28ffb607f3933ce3c22d4a2d432a6f727a2907` | exact symbol on the ordinary path | 0 | codec goal: 9,776 / 15,050 source bytes, 1 file, 8 candidates, 0 dropped, 24,213 us | recorded in commits | deterministic explained selection; exact override remains expert-only |
| P4 start/status | `4a0b7be71085c4d00e6e348564346cdc032585f6` | raw roots, wires, timestamps, toolchain capsule, write-scope CSV, context symbol | at least 5 expert composition/status steps | same codec packet: 65.0% of package source | +593 / -40 | `workspace`, `goal`, optional `profile`; human-first status |
| P5 manual handoff | commit subject `feat(zcode): add contained manual handoff` | candidate path construction and manual context/CAS lookup | 1 undocumented handoff procedure | reuses the exact P3 context with no expansion | +581 / -67 before generated docs/tests | isolated candidate tree and bounded packet; candidate admission/build remains next |

The P5 delta includes promoting the build worker's private CAS-tree materializer
to one shared ZVCS primitive; 59 production lines of duplicate materialization
were removed. No canonical domain was added. The manual adapter is a closed
registry entry, creates no candidate authority, and does not run a model.

## Benchmark acceptance

The frozen targets are: zero ordinary raw roots or wire hex; no more than five
commands; at least 9/12 compiling candidates; at least 8/12 policy-satisfying
candidates; 2/2 impossible requests fail closed; zero writes outside candidate
workspaces; zero silent failures; zero false independence claims; every status
has a concise human summary; and honest context/time measurements.

Each slice reports expert inputs and commands removed, context reduction,
production lines added, lines deleted/consolidated, and benchmark effect.

## Hard boundary

Living Commons O0–O7 is frozen except for correctness. This project performs no
ZC23 issuance, token launch, election-authority, custody, wallet, vault,
transaction, core, consensus, live-datadir, service, deployment, restart, GUI,
web IDE, arbitrary model shell, or multi-package workspace work. It never
executes downloaded source automatically and never lets a model accept or
publish its own result. Genuine independent reproduction requires another
physical machine and remains a separate owner-gated operational task.
