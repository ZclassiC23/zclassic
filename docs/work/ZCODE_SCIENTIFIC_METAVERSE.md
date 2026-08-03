# ZCODE scientific metaverse and proof-of-contribution network

Status: owner-directed implementation plan, 2026-08-02. This plan extends the
live ZCODE package and agentic-development foundations. It does not displace
the sovereign-node MVP order in [`FORWARD_PLAN.md`](./FORWARD_PLAN.md), change
ZClassic consensus, authorize a deploy, or authorize movement of live funds.

## Mission and truth boundary

ZCODE is an application overlay with this evidence flow:

```text
question -> preregistered study -> confined experiment -> signed evidence
         -> independent reproduction -> review -> local conclusion
         -> optional contribution reward
```

ZClassic PoW supplies active-chain ordering, timestamps, reorg handling,
delayed election randomness, and ZSLP settlement. It does not establish
scientific truth, operator independence, data availability, or anonymity.
Scientific acceptance remains local and evidence-based.

The implementation must reuse the existing owners:

- `content.v2`, `vcs_object`, and the ZCODE package store own bytes and CAS.
- Existing ZVCS manifests own exact source trees.
- `vcs_package_lock`, `vcs_package_recipe`, toolchain capsules, and ZBuild own
  dependencies, build graphs, environments, and confined fixed actions.
- Existing `task.v1`, `candidate.v1`, `proof_policy.v1`, `review.v1`,
  `work_receipt.v1`, and `proof_set.v1` wires remain byte-stable.
- ZID/ZANC own identity and chain-anchored identity statements.
- The `zpkgswm` multiplexer owns package and ZCODE network traffic.
- The existing metaverse property `zcode_package:<root>` remains the only
  ZCODE package property kind.
- Generic ZSLP owns base token validity. ZCODE policy is an additional local
  application verdict, never a consensus predicate.

No second CAS, scheduler, identity system, transaction builder, socket stack,
or metaverse property kind may be added. Keep `core/` sealed. Use repository-
owned permissive C23 and add no mandatory run-time dependency.

## Canonical integrity rules

All new authority is a full domain-separated SHA3-256 root:

```text
chunk -> file manifest -> package/study object -> proof set
      -> signed checkpoint -> ZANC transaction -> PoW block
```

- 64-bit values are request IDs, counters, sequence numbers, heights, and
  times only; they are never roots or authority.
- HASH160 remains legacy transparent-address compatibility only.
- Canonical CAS, study, evidence, DHT, policy, committee, and checkpoint
  roots are 256 bits.
- Sign exact canonical binary wires, never display JSON.
- Prefixes may be displayed or indexed only if lookup resolves and rechecks
  the complete 256-bit root.
- New algorithms require a new version and explicit algorithm identifier;
  stacking a wider hash on SHA3-256 is not additive security.
- Every parser is exact-length, rejects trailing bytes, uses explicit little-
  endian integers, validates closed enums and bounds, and zeroes output on
  failure.

## Scientific object graph

The new canonical objects compose with the existing development graph:

```text
study_spec.v1 -> task.v1 -> candidate.v1
              -> benchmark/reproduce/review work_receipt.v1
              -> proof_set.v1 -> signed ZCODE package
              -> optional ZID/ZANC proof
```

### `study_spec.v1`

Fixed wire binding hypothesis root, null-hypothesis root, exact source,
dependency-lock and toolchain roots, protocol root, workloads/datasets root,
metrics root, estimator/tolerance root, environment-policy root, citations
root, preregistration-policy root, required reproductions, required reviews,
sequence, creation time, and expiry. A hypothesis and its null must be
distinct. The expiry must follow creation. Required counts are bounded and
nonzero. Text and datasets live in the existing CAS; the study wire binds
their roots.

### `benchmark_result.v1`

Fixed wire binding study, task, candidate, fixed action, achieved-environment,
raw-sample, and evidence roots, plus observation status, challenge block
height/hash, sequence, start time, and finish time. It records observations
and deliberately has no `true`, `accepted`, or `correct` field. Null and
negative results are valid statuses, not failures to publish.

### `reproduction.v1`

Fixed wire binding study, original result, reproduced result, comparison
policy, original/reproduced environment, reproducer identity, verdict,
sequence, and time. The closed verdict is `replicated`, `contradicted`, or
`inconclusive`. Original and reproduced result roots must differ.

### `science_findings.v1`

Fixed structured findings binding study, task, candidate, evaluated result,
proof set, methods, limitations, conflicts, optional retraction target, flags,
severity, sequence, and time. The object is formed first; the existing
`review.v1` then binds its root. This avoids a findings-root/review-root hash
cycle and does not introduce a second review signature system. The existing
signed review receipt authors the review.

### `curation_vote.v1`

Fixed signed local-discovery signal binding voter ZID, study/package property,
vote (`useful`, `interesting`, `flag`), sequence, expiry, and network. It is
not proof, money, committee weight, routing authority, or global truth.

### `contributor_binding.v1`

Canonical statement binding an existing ZID Ed25519 identity to a fresh ZCL
secp256k1 address/key. It binds network genesis, ZID, ZCL key/address,
predecessor binding, sequence, issue/expiry time, and active/rotate/revoke
operation. The exact body root is signed by both keys. Verification pins the
expected network and ZID and checks both signatures. Rotation points to the
prior binding; revocation cannot create a replacement key implicitly.

Implementation status (S2, remote coder, 2026-08-02): landed in
`lib/vcs/include/vcs/zcode_contributor_binding.h` +
`lib/vcs/src/zcode_contributor_binding.c` with focused tests appended to the
existing `zcode_contributor` group. The 184-byte body / 312-byte full wire is
domain-separated (`zcl.zcode.contributor_binding.v1` for the dual-signed body
root; `zcl.zcode.contributor_binding.root.v1` for the full-wire root a
successor's predecessor commits). The secp256k1 signature is 64-byte r||s
normalized to low-S, so sealing is byte-deterministic; the ZCL address hash
is validated as `hash160(zcl_pubkey)` at the codec layer. REVOKE carries the
key it retires (staying standalone dual-verifiable) and is terminal:
`vcs_zcode_contributor_binding_validate_successor()` rejects any successor of
a revoked binding, replay/skip sequencing, cross-network/cross-ZID links,
same-key rotations, new-key revocations, and tampered predecessors. S3/S6
consume only `root()` + `verify()`/`validate_successor()`; no
wallet/database/command surfaces were touched. Golden vectors are pinned in
`lib/test/src/test_zcode_contributor.c` (`ZCB_KAT_*`).

Integration hardening (2026-08-02, same lane): `validate_at()` rejects use
before `issued_unix` (`ERR_NOT_YET_VALID`); `seal()` re-derives the Ed25519
public key from the supplied ZID secret and rejects a mismatch;
`validate_successor()` rejects non-increasing `issued_unix`
(`ERR_TIME_ORDER`). Golden v1 KATs unchanged.

### `contributor_binding.v2`

Three-signature rotation and delayed recovery (2026-08-02): 384-byte wire =
192-byte body (v1 fields + `activation_unix`) + ZID + current-ZCL + new-ZCL
signature slots under `zcl.zcode.contributor_binding.v2` /
`.root.v2` domains. ACTIVE signs both ZCL slots with the initial key; ROTATE
requires ZID + OLD ZCL + NEW ZCL; REVOKE keeps and signs with the retiring
key and zeroes the new slot; RECOVER (op 4) zeroes the current slot (old key
presumed lost), signs the new slot, and activates only at
`activation_unix >= issued_unix + 604800` — a separate delayed path, never a
fast rotation. `vcs_zcode_contributor_binding_validate_chain_v2()` adds the
retired-key reuse ban across the whole chain. v1 wire/KATs are frozen; v2
KATs (`ZCB2_KAT_*`) pinned deterministic across three runs.

Science-object hardening (S1 files, owner directive 2026-08-02):
findings/review time order corrected to match this spec (findings formed
first; `review->created_unix` may be LATER than the findings' creation —
rejected only when earlier); "may submit now"
(`vcs_zcode_study_spec_accepts_submission_at()`) is split from "evidence was
valid when created" — cross-object validators no longer consult the study
expiry against `now_unix`, so valid history re-verifies forever while
post-window submissions and future evidence (`ERR_EVIDENCE_FUTURE`) are
rejected; benchmark results must bind a canonical fixed-action root
(`ERR_ACTION_MISMATCH`); reproductions must compare the same
study/task/candidate/action across both results; findings must bind the
evaluated result's task and candidate roots.

## Fixed scientific actions

Extend the closed `vcs_build_action_v1` registry with:

- `c23.benchmark.v1`
- `c23.benchmark.reproduce.v1`
- existing `c23.review.v1` (retain its exact identifier)

Benchmark and reproduction actions use recipe-derived candidate inputs,
pinned toolchain/environment capsules, no network, bounded CPU/RAM/process/
output limits, raw sample manifests, and deterministic result envelopes.
Platform receipts are admissible only where that platform's native
confinement backend passes its escape suite. Downloaded scripts and arbitrary
shell remain forbidden.

AI agents may propose studies, execute fixed confined work, reproduce results,
and author signed reviews under metaverse grants. They receive no wallet keys,
threshold shares, raw-signing API, arbitrary shell, canonical deploy, or
release authority.

## Network overlay

Extend `zpkgswm`; do not add a socket stack.

- Stable node IDs derive from network genesis, a chain-anchored ZID, and
  delayed active-chain block hashes.
- ZID masters delegate online Ed25519 and Noise keys for at most 30 days.
- All ZCODE traffic requires a Noise-authenticated session. Direct and
  optional Tor routes retain the same channel binding.
- Kademlia parameters are fixed at `k=16`, `alpha=3`, at most 1,024 persisted
  contacts, three parallel queries, and a 30-second lookup ceiling.
- Signed record kinds: `NODE` (6 hours), `PROVIDER` (2 hours), `POINTER`, and
  public `ANNOUNCEMENT` (7 days). Preserve conflicting valid records as
  equivocation evidence; do not hash-tie-break them into false agreement.
- A package targets eight providers. `durable` requires five signed storage
  acknowledgements across three declared owner groups. The API must call this
  declared diversity, never proof of different operators.
- Fetch order is local CAS, connected advertisers, DHT providers, then the
  exact existing manifest/chunk verifier.
- Persisted contacts, normal ZClassic peers, addrman/DNS/fixed seeds,
  ZENDP/ZDIR, manual peers, and optional Tor are additive hints. None owns an
  authoritative DHT signing key.
- Publishing over a direct route warns that peers observe IP, timing,
  requested roots, and volume. Tor does not unlink stable ZID signatures.

Native surfaces:

```text
zcode.network.status|peers|find|providers|publish|pin|unpin
zcode.evidence.anchor|verify
ops state --subsystem=zcode_dht
```

Read resources:

```text
/api/v1/zcode/providers
/api/v1/zcode/dht-records
/api/v1/zcode/replication-receipts
/api/v1/zcode/evidence-checkpoints
```

Canonical objects remain CAS truth. ActiveRecord rows are rebuildable,
bounded projections and caches. Every write uses the AR lifecycle.

## Science commands and discovery ranking

```text
zcode.science.study.plan|commit|show|list
zcode.science.work.plan|commit|status|receipt
zcode.science.review.submit
zcode.science.vote.submit
zcode.science.rank
```

Writes use expiring exact plans, `confirm:true`, durable idempotency, and stdin
for bodies or sensitive inputs. Existing `zcode.package.dev.*` and
`metaverse.build.*` become adapters to the same services rather than separate
implementations.

Personalized PageRank is deterministic and discovery-only:

- Nodes are ZCODE study/package properties; canonical citations are edges.
- Locally trusted signed curation votes influence personalization.
- Reproductions and reviews are local evidence filters.
- Integer mass is `10^12`, damping is `85/100`, iteration count is 32,
  ordering is full-root byte order, and remainders go to the earliest
  canonical nodes.
- Output binds algorithm version, graph root, seed-set root, filter-policy
  root, coverage, and truncation.
- Never rank people. Never use rank, votes, balances, or service volume for
  proof acceptance, committee authority, or rewards.

The pure S5 core is implemented in
`lib/vcs/include/vcs/zcode_discovery_rank.h`. It accepts
only full property roots, canonical citation edges, locally aggregated seed
weights, and a filter-policy root. It normalizes all input order, rejects
duplicate or missing graph members, conserves exactly `10^12` integer mass,
and emits a canonical result ordered by mass then full root. The result binds
the graph, seed set, algorithm version, filter policy, returned coverage mass,
and truncation. Projection and the `zcode.science.rank` adapter remain
S3-dependent; this core has no person, proof-acceptance, wallet, reward,
database, network, or command input.

## Proof of contribution

### Bootstrap credential

A `c23.seed.v1` credential requires a permissively licensed public package,
frozen dependency lock, novel canonical semantic fingerprint, two pinned
independent C23 compiler capsules on one declared target, warnings-fatal
network-disabled compile/link success, dual ZID/ZCL signatures, durable DHT
replication, PoW anchoring, and a seven-day challenge period. Vendored or
generated code receives no credit. One credential is allowed per ZID.

The credential contributes selection weight 1, claims neither usefulness nor
safety, and earns no token by itself. Challenge-matured code, tests, fixes,
benchmarks, reproductions, negative findings, and structured reviews add
evidence points. Storage, signing, votes, PageRank, and transferred ZCODE add
no committee weight. One contribution root credits one identity.

### Committee election

- Epoch length: 8,064 active-chain blocks.
- Freeze candidates at the midpoint after compact `ZVAL` readiness records
  and referenced evidence are final.
- Election seed: next 64 ordered active-chain block hashes, after the existing
  finality policy.
- Weight: `1 +` challenge-matured evidence from the prior 26 epochs, linearly
  decayed and capped at 10,000.
- Sample distinct ZIDs without replacement using SHA3-derived 64-bit rejection
  sampling over canonical cumulative integer weights.
- Publish committee order, evidence snapshot root, seed heights/hashes,
  weights, concentration metrics, and policy root.
- One ZID gets at most one seat. Pseudonyms are not proof of different humans.
- At 100 seats, terms are four epochs and exactly 25 seats expire per epoch.
  Subjective liveness pings never change membership mid-epoch.

### Progressive custody

Do not create the transferable asset until three candidates are challenge-
mature and four shadow elections are green. Grow custody monotonically:

```text
3 candidates  -> 2-of-3 P2SH
5 candidates  -> 3-of-5 P2SH
9 candidates  -> 5-of-9 P2SH
15 candidates -> 8-of-15 P2SH
```

Replace approximately one quarter per epoch. Missing members produce
`quorum_unavailable`; thresholds never fall automatically. Honest signers
require individually signed approvals from two thirds of the committee over
the exact deterministic transaction. Script threshold remains the theft
boundary and the API must say that a compromised Script majority can bypass
the software certificate rule.

### Issuance and policy validity

Ticker `ZCODE`, decimals 0, initial supply 1. Epoch emission is
`floor(50000 / 2^era)`, with 208 epochs per era. Maximum policy-compliant
supply is 20,798,753 including genesis.

Per-epoch caps are 70% code/tests/mechanically proven security, 15% benchmark
and reproduction, 10% rigorous reviews/negative findings, and 5% verified
storage/DKG/signing. Integer remainder goes to code/tests. Mint only matured
awards; unused capacity is not minted or carried.

The outgoing committee's MINT pays the epoch output and transfers the baton to
the incoming committee. Confirm it before sharding treasury UTXOs; do not
build unconfirmed token chains. Burn the baton at zero emission.

ZCODE-aware clients expose a second verdict beside strict generic ZSLP:

```text
ZSLP_VALID + ZCODE_POLICY_VALID
ZSLP_VALID + ZCODE_POLICY_INVALID
UNKNOWN
HALTED_POLICY_VIOLATION
```

An off-schedule but ZSLP-valid mint or stolen baton halts the ZCODE lineage;
clients never invent a replacement. Awards mature after both 8,064 additional
active-chain blocks and 604,800 seconds median-time-past. Reorg of the opening
anchor restarts maturity. Subjective review never changes payouts.

The genesis policy root is immutable. An incompatible policy requires a new
asset/version and explicit opt-in.

Every transaction plan binds policy/token/epoch, evidence, committee,
active-chain anchor, strict-valid confirmed inputs, scripts, quantities, fee,
expiry, and exact transaction bytes. Persist raw bytes and entry mapping
before relay; retry returns or rebroadcasts the same txid. Award state and
payout state are separate.

## Threshold ECDSA research boundary

Remain at 8-of-15 unless all activation gates pass: at least 150 challenge-
mature READY contributors for four epochs, two independent cryptographic
audits, three green 100-node DKG/sign/handoff rotations, and complete custody,
strict-ZSLP, reorg, backup, and sovereign-chain gates.

The selected target remains 51-of-100 ECDSA with a public 67-member Ed25519
certificate before honest nodes release shares. Fifty-one colluding shares
can steal by definition. Use a fresh aggregate secp256k1 key and dealerless
DKG each epoch; no immortal key and no rolling resharing.

CGGMP20 identifiable-abort work, if begun, is a clean-room permissive C23
research module with repository-owned constant-time bigint, Paillier, and ZK.
Do not copy or link GPL/OpenSSL implementations. It stays disabled for custody
until public vectors, differential checks, malicious-party tests, audits, and
WAN benchmarks pass. A failed gate names its blocker and leaves custody at
8-of-15; it never deploys experimental cryptography or lowers quorum.

## Ordered landing units

Each unit lands independently with focused adversarial tests, parallel
`build-only`, full link, `make lint`, uncached `test-parallel`, deterministic
projection rebuild checks where applicable, and no deployment.

| ID | Landing unit | Dependency | State / owner |
|---|---|---|---|
| S0 | Freeze this specification and coordination boundaries | existing ZCODE foundation | complete 2026-08-02, primary |
| S1 | Canonical science codecs, roots, cross-object validation, fixed benchmark/reproduction action identities | S0 | implemented and gate-verified 2026-08-02, primary |
| S2 | Dual-signed `contributor_binding.v1`, rotation/revocation/network replay gates | S0 | implemented 2026-08-02, remote coder; integration hardening pending |
| S3 | CAS storage, rebuildable science projection, study/work/review/vote plan-commit services and commands | S1, S2 | claimed 2026-08-02, main session — scope: `lib/vcs/src/zcode_science_index.c(+h)`, `app/services/src/zcode_science_service.c(+h)`, `app/models` science projection tables (schema bump 48→49 + validator pin 26→27), `tools/command/native_zcode_science_command.c`, `config/commands/zcode_science.def`, `lib/test/src/test_zcode_science_store.c` <!-- doc-path-ok: S3 is claimed remote work; these planned files have not landed yet. --> |
| S4 | Closed benchmark/reproduction executors and environment/raw-sample receipts | S1, recipe-derived build graph | claimed 2026-08-03, main session — scope: `lib/vcs/src/hardware_profile.c(+h)`, `lib/vcs/src/benchmark_method.c(+h)`, `app/services/src/zcode_benchmark_executor.c(+h)`, reproduction executor, `zcode.science.work.execute` command (additive in `config/commands/zcode_science.def` or new aggregated def), `lib/test/src/test_zcode_benchmark_exec.c` <!-- doc-path-ok: S4 is claimed remote work; these planned files have not landed yet. --> |
| S5 | Deterministic discovery PageRank and golden graphs | S1, S3 | pure core implemented 2026-08-02, primary; projection/command adapter claimed 2026-08-03, main session — scope: `lib/vcs/src/zcode_discovery_projection.c(+h)`, `zcode.science.discover` + `zcode.science.rank.snapshot` commands (additive def), `lib/test/src/test_zcode_discovery_projection.c` — pure core files untouched |
| S6 | Read-only Noise-bound DHT, persisted contacts, diagnostic dumper | S2 | unclaimed |
| S7 | Provider STORE, signed acknowledgements, replication/publish/fetch adapters and REST views | S6 | unclaimed |
| S8 | Evidence checkpoints and ZANC anchors | S2, S7 | unclaimed |
| S9 | Seed credential, semantic novelty, maturity and challenge engine | S3, S4, S7, S8 | unclaimed |
| S10 | Shadow evidence scoring, deterministic elections, rotation and concentration reporting | S9 | unclaimed |
| S11 | Progressive P2SH transaction planning/signing in simulation only | S10 | owner-gated implementation |
| S12 | Owner-authorized native ZCODE genesis and one-epoch exposure | four green shadow epochs + custody gates | owner-gated launch |
| S13 | Clean-room CGGMP research primitives/protocol and public artifacts | independent research gates | disabled research |
| S14 | 51-of-100 transition | all activation gates | owner-gated, blocked by design |

### Parallel ownership at publication

Primary lane owns for S1:

```text
lib/vcs/include/vcs/zcode_science.h
lib/vcs/src/zcode_science.c
lib/vcs/include/vcs/build_action.h
lib/vcs/src/build_action.c
lib/test/src/test_zcode_science.c
lib/test/src/test.c
tools/dev/test_group_catalog.def
app/controllers/include/controllers/agent_impact_rules.def
docs/work/ZCODE_SCIENTIFIC_METAVERSE.md
docs/work/ZCODE_DEVELOPMENT_NETWORK.md
docs/work/README.md
```

Primary lane additionally owns the S5 pure-core files:

```text
lib/vcs/include/vcs/zcode_discovery_rank.h
lib/vcs/src/zcode_discovery_rank.c
lib/test/src/test_zcode_discovery_rank.c
```

The remote coder may claim S2 without touching those files:

```text
lib/vcs/include/vcs/zcode_contributor_binding.h
lib/vcs/src/zcode_contributor_binding.c
lib/test/src/test_zcode_contributor.c
```

S2 should reuse the existing ZID Ed25519 and wallet/secp256k1 primitives,
produce exact body/full-wire KATs, pin the expected genesis and ZID during
verification, reject trailing/cross-network/replay/invalid-rotation wires,
and make no wallet/database/command changes. It may run the existing
`zcode_contributor` group; the primary lane will integrate any additional
central test registration after merge. Before starting another unit, update
this table on `main` to claim it and list an exact disjoint file scope.

## Required adversarial coverage by phase

- Codecs/identity: malformed and trailing wires, wrong magic/version/network,
  domain confusion, signature replay, rotation/revocation, full-root lookup,
  and hash-prefix collisions.
- Science: hypothesis/result separation, raw-sample integrity, incompatible
  environments, null/negative results, contradictory reproductions, stale
  reviews, and retractions.
- Credentials: copied/renamed/delete-add farming, generated/vendor credit,
  compiler disagreement, dependency drift, license failure, and replay.
- DHT: poisoning, eclipse, churn, partitions, lying providers, corrupt
  chunks, gossip storms, quota exhaustion, route fallback, and restart rebuild.
- Ranking: golden graphs, input-order invariance, cycles, dangling nodes,
  rounding, seed changes, vote spam, and mechanical proof that ranking cannot
  affect evidence acceptance or money.
- Committee: deterministic sampling, contribution splitting, rotations,
  readiness loss, concentration, conflicting 51-quorums, 67-certificate
  behavior, and boundary reorgs.
- Token: forged/off-schedule mints, baton theft/halt, duplicate payouts,
  concurrent commits, crash-before-relay, sharding, partial batches, and
  payout reorgs.
- CGGMP: malformed moduli/range proofs, malicious parties, identifiable abort,
  nonce reuse/rollback, complaints, 49 unavailable members, 51-collusion
  assumptions, coordinator failure, and cross-platform constant-time checks.
- Platform: Linux/macOS/Windows codec parity; execution receipts only after
  that platform's confinement escape tests pass.

## Non-negotiable rollout boundaries

- Public reproducible C23 is v1. Private data, embargoes, arbitrary stats
  scripts, GPUs, and network benchmarks are later versions.
- No live funds move automatically during development.
- No canonical node restart or deployment is part of these landing units.
- Direct Noise protects payloads, not IP/timing/volume metadata. Tor remains
  optional and does not unlink signed identity.
- Keys, addresses, signatures, and owner-group labels do not prove distinct
  humans, machines, or operators.
- ZCODE and committee activity are public transparent-ledger metadata; ZSLP
  has no private mode.
- Application anchoring and committee selection never modify ZClassic block
  or transaction validity.

References: the Tor distinction follows the public
[directory consensus specification](https://spec.torproject.org/dir-spec/computing-consensus.html);
discovery ranking follows the original
[PageRank paper](https://courses.cs.duke.edu/common/compsci092/papers/google/pagerank.pdf);
threshold research must account for the
[GG20 revisions](https://eprint.iacr.org/2020/540),
[Alpha-Rays attacks](https://eprint.iacr.org/2021/1621), and the
[NIST CGGMP preview/licensing record](https://csrc.nist.gov/csrc/media/Projects/threshold-cryptography/documents/TCall-1/Fireblocks-c-PW01.pdf).
