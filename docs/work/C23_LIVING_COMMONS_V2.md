# C23 Living Commons v2 — Family Commons and evidence economics

Status: additive pre-genesis protocol foundation. All v2 objects and commands
are simulation-only and not owner-approved. They create no token, transaction,
wallet authority, custody authority, consensus rule, deployment authority, or
claim on a live ZCL balance. Existing Living Commons v1 bytes and projections
retain their original meaning.

## Public contract

The CAS/DHT transport is permissionless. The official `family-c23.v1` view is
not: before official clients index, advertise, replicate, serve, forward,
preview, or execute an object, that exact object and dependency closure need a
current Family Commons admission. New objects begin `PENDING`. Only a current
`SELF_SCREENED` or quorum-pass admission with complete coverage is visible.
Incomplete, opaque, stale, reorged, disputed, or reversed evidence immediately
fails closed.

`family-c23.v1` is an immutable root. A new policy is a new named profile and
root; no owner key can rewrite v1. The closed exclusions are explicit sexual
material, graphic gore, targeted hate, self-harm encouragement, harmful illegal
activity, gratuitous strong profanity, and software whose demonstrated primary
purpose is abuse. Neutral scientific, medical, historical, cybersecurity, and
dual-use education can pass as `CONTEXTUAL_SCIENCE`. Family admission says
nothing about factual accuracy, software quality, or security.

The canonical frozen policy root in this implementation is:

```text
family-c23.v1 = 460d650c5be714f27dde287c368eafb781467026a1c06a8215fbe17dc610ea86
```

The policy initializer domain-hashes the exact profile name and policy text;
the dedicated KAT rejects any drift.

## Package objects

The v2 foundation defines separate root domains for:

- `workspace_manifest.v1`: up to 4,096 canonically sorted module releases,
  source assignments, predecessors, sequences, typed-asset roots, and a sorted
  acyclic dependency graph. Duplicate semantic fingerprints are rejected.
- `typed_asset_manifest.v1`: closed kind, format root, content root, byte
  count, attribution, collection and signer. Only CC0-1.0 and CC-BY-4.0 are
  representable; BY requires attribution. Assets have no creation-award class.
- `module_passport.v1`: independent API, recipe, toolchain, tests, licence,
  semantic fingerprint, workspace lineage, source assignment, quality roots,
  signer and signature. Every authority root is mandatory.
- `quality_profile.v1`: the universal required-check mask plus an optional
  field-specific additive mask/rules root. A field profile cannot remove any
  universal check. Math, cryptography, biology, chemistry, physics, astronomy,
  networking, video, and games are closed field values.
- `mission.v1`: signed publisher, subject-tag, goal, optional patron-task and
  chain-time coordinates. Tags and missions are not economics inputs.
- `contribution_split.v1`: up to 64 sorted recipient bindings, exact atom
  amounts, and every participant signature. Checked addition must equal the
  frozen claim award exactly.

These objects compose the existing package CAS and its 64 MiB package bound;
they do not introduce another package store or database authority.

## Creation economics

`zc23_policy_candidate.v2` is valid only with both `SIMULATION_ONLY` and
`NOT_OWNER_APPROVED`, the exact 8,064-block and 604,800-second maturity rules,
the Family policy root, qualification/backlog roots, and this award schedule:

| Event | Atoms | ZC23 |
|---|---:|---:|
| Original module or first canonical C23 import | 100,000,000 | 1.00000000 |
| Accepted established-defect repair | 50,000,000 | 0.50000000 |
| Independently reproduced security finding | 50,000,000 | 0.50000000 |
| Independent later test/conformance suite | 25,000,000 | 0.25000000 |
| Independent reproduction | 25,000,000 | 0.25000000 |
| Reproduced performance-frontier improvement | 25,000,000 | 0.25000000 |
| New compatibility proof | 25,000,000 | 0.25000000 |
| Unique preservation milestone | 12,500,000 | 0.12500000 |

Assets, ordinary storage, downloads, reviews, moderation, agreement, blocking,
model expense, challenge volume, popularity, stake and patronage never mint.
Bandwidth reciprocity remains local nontransferable ZCODE Credit.

`creation_claim.v2` is structurally separate from epoch selection. A claim is
eligible only when independently matured, current under moderation, and free of
reorg, retraction, duplicate-semantic and moderation-reversal flags.
`epoch_creation_set.v2` selection is input-order invariant:

1. sort each category by `(maturity_height, maturity_mtp, claim_root)`;
2. rotate the first category from the previous epoch root and visit cyclically;
3. select whole claims only;
4. cap each recipient and workspace lineage at
   `min(capacity, max(1 ZC23, floor(capacity/100)))`;
5. defer claims that cross a cap or remaining capacity;
6. exclude invalidated and duplicate-semantic claims; and
7. expire unused capacity.

The result root commits the cutoff, prior epoch, capacity, selected claim order,
awarded atoms, and expired atoms. No MINT transaction is built.

## Decentralized moderation

Classification has two closed axes:

```text
audience = GENERAL | CONTEXTUAL_SCIENCE | MATURE | EXPLICIT | UNKNOWN
behavior = BENIGN | DUAL_USE | MALICIOUS | UNKNOWN
```

Only complete `GENERAL`/`CONTEXTUAL_SCIENCE` plus `BENIGN`/`DUAL_USE` coverage
votes PASS. Coverage must account exactly for metadata, documentation,
comments, strings, examples, media, typed assets, the dependency closure,
object count and bytes. Unsupported, encrypted, opaque, missing, truncated,
mutable, over-budget, or partial extraction votes `UNKNOWN`.

Receipts contain only roots, coverage coordinates, closed labels, reason-code
bits, chain-time coordinates and a signature. They have no fields for source
excerpts, raw prompts/responses, chain-of-thought, credentials, endpoints,
addresses, IPs, or datadir paths.

Panel selection is bound to a future block hash, excludes publisher-related
services, and admits one vote per declared operator group. Within a duplicate
group, the future-hash-minimum ZID is deterministic. Selection is without
replacement, maximizes distinct model families for the first three seats when
possible, and reports actual operator/model counts. Token balance, Score,
stake, popularity, patronage and model vendor are not selection inputs.

| Independent operator groups | Seats and quorum | State tier |
|---:|---:|---|
| 0 | founding self-screen 1/1 | `SELF_SCREENED` |
| 1 | 1/1 | `BOOTSTRAP_PASS` |
| 2 | 2/2 | `PEERED_PASS` |
| 3–4 | all, `ceil(2N/3)` | `EMERGING_PASS` |
| 5–6 | all, `ceil(2N/3)` | `DIVERSE_PASS` |
| 7+ | future-hash sample 7, 5/7 | `RESILIENT_PASS` |

A resilient appeal needs 11 available independent groups and 8/11; otherwise
it remains pending. PASS quorum admits, BLOCK quorum restricts, a mixed
non-quorum is `CONFLICTED`, and all missing/unknown is `UNKNOWN`.
`SELF_SCREENED` is visibly labelled and cannot qualify for issuance. At epoch
selection a claim needs challenge maturity and the highest independently
attainable current tier.

## Typed surfaces and present boundary

The shipped non-creating v2 readers are:

```text
zcode commons economics status
zcode moderation policy list
zcode moderation policy show --input='{"profile":"family-c23.v1"}'
```

The package/workspace plan-commit, asynchronous classification, panel,
challenge, appeal, admission projection, REST resources and cross-surface
Family enforcement remain later additive slices. Until those slices land,
no command claims a package is admitted and existing v1 package behavior is
not reinterpreted. This is a deliberate fail-closed delivery boundary, not a
live moderation service.

## Mechanical evidence

`test_zcode_commons_v2` freezes policy, asset, workspace and receipt KAT roots
and covers award drift, profile weakening, excluded licences, missing tests,
dependency cycles, semantic duplication, split arithmetic, dual maturity,
caps, backlog/expiry, input-order invariance, reorg exclusion, incomplete
coverage, contextual science, malicious labels, one-operator grouping,
future-hash selection, diversity, 1/2/3/5/7/15-service ratchets, 5/7 and 8/11
quorums, stale visibility and the self-screen issuance prohibition.

Consensus parity remains untouched: no file under `core/` or the sealed
block-connection ordering layer is changed by this protocol.
