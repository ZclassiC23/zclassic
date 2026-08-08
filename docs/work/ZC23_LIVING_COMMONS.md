# ZC23 Living Commons

> User-facing entry point: [`../METAVERSE.md`](../METAVERSE.md); acceptance
> bar: [`../METAVERSE_MVP.md`](../METAVERSE_MVP.md). This document stays the
> authoritative maintainer spec.

Status: owner-directed specification, 2026-08-06. This document freezes the
safe pre-genesis policy and implementation order for creation-backed ZC23
issuance. It authorizes specifications, codecs, validation, rebuildable
projections, read-only views, and simulations only. It does **not** authorize a
live token, GENESIS, MINT, SEND, payout, wallet operation, custody movement,
deployment, service restart, canonical-datadir mutation, or consensus change.

## Purpose and public terminology

The project mission is:

> **ZClassic23 is a metaverse where people and AI create real things together,
> and nobody owns the world they build in.**

Here, "metaverse" means the shared, permissionless creation environment formed
by the ZCODE package library, development network, evidence graph, and public
discovery surfaces. "Real things" means exact retrievable source, executable
C23 packages, tests, reproductions, reviews, repairs, compatibility work, and
preservation records—not engagement counters or a claim that software alone
can judge art.

"Nobody owns the world" does not erase authorship or a contributor's legal
rights. It means that no operator, AI provider, patron, token holder, committee,
package index, or website owns the shared protocol space or gains title to
another contributor's work. Contributors publish under explicit permissive
licences; everyone retains the freedoms those licences grant. A human and an
AI-assisted contributor use the same factual evidence rules. Neither identity
kind receives special truth, scoring, or issuance authority.

ZC23 is planned as a scarce, auditable patronage asset associated with a
growing public commons of executable C23 work. It may later be used as optional
commissioning capital for new work and continuity. Its mechanical promise is:

> Every policy-valid atomic unit of ZC23 exists because the public C23 commons
> grew, was tested, was independently reproduced, or was preserved.

Public names are fixed before token genesis:

| Name | Meaning |
|---|---|
| **ZCODE** | The protocol, network, package library, and development system |
| **ZC23** | The planned transferable ZSLP token ticker |
| **ZCODE Score** | Nontransferable, evidence-derived contribution score |
| **ZCODE Credit** | Local, nontransferable reciprocity/quota credit |
| **ZCODE Badges** | Identity-bound achievements |

Existing `zcl.zcode.*` domains, command roots, and internal ZCODE identifiers
remain. This is a public-language correction, not a destructive rename.
Compatibility surfaces such as `zcode reward score` remain explicitly legacy
and non-credit where their schemas already say so.

## One creation API

The native command tree is the common interface for people, AI agents, local
tools, and presentation adapters. It must make the complete creation loop easy
to discover without granting any adapter hidden authority:

```text
discover -> fetch/inspect -> task -> candidate -> prove/reproduce/review
         -> accept -> publish -> attribute -> preserve or commission again
```

Every shipped leaf is discoverable through `discover help`, searchable through
`discover search <query>`, and has an exact input contract through
`discover schema <leaf>`. Read operations report whether they are complete,
partial, or unknown and give the next safe diagnostic. They remain non-creating
on an absent workspace or datadir. Mutations use explicit plan/commit where
money, publication, installation, or other durable authority is involved.

The implemented foundation already exposes package search/show/recipe/verify,
package fetch and confined add, development create/use/improve/evidence/accept,
lane and task inspection, offline release preparation/sealing, signed ZCODE
Score receipts, and read-only `zcode commons` verification. Patronage and
continuity commands remain planned until their canonical simulation objects and
cross-object validators land. No documentation or adapter may label a planned
leaf ready, a CAS object funded, a local result globally trusted, or a
simulation as a live transfer.

All adapters consume these same objects and services. A website or onion view
is a projection, never a second package catalog, evidence authority, identity
system, or database of truth. Public discovery, retrieval, inspection, build,
reproduction, and verification remain free and cannot require ZC23.

ZC23 conveys no copyright, title, exclusive licence, royalty, dividend, claim
against a contributor, promise of appreciation, or protocol judgment of
artistic merit. Public packages remain permissively licensed, discoverable,
retrievable, inspectable, buildable, reproducible, verifiable, and usable
without holding or paying ZC23. Tokens are never an access key.

## Cultural meaning and mechanical truth

Humans may describe the public collection as a living commons of executable
C23 art. Protocol objects verify objective facts only: authorship, exact
source, lineage, licensing, tests, defect reproduction, fixed-action results,
independent reproduction, review authorship, compatibility, preservation,
challenge maturity, and amount arithmetic.

No canonical wire, database authority, score rule, committee rule, or token
policy may contain or infer subjective fields such as `artistic_value`,
`beauty`, `importance`, `canonical_truth`, or `investment_value`. PageRank,
curation votes, popularity, balance, price, transfer volume, patronage volume,
market activity, storage volume, and model claims are discovery or cultural
signals only. They cannot establish correctness, safety, scientific truth,
proof acceptance, contributor authority, committee weight, score, badges, or
issuance.

## Immutable ZC23 Living Commons covenant

The planned ZC23 genesis policy root commits the following covenant verbatim in
meaning. An incompatible future policy requires a different token ID and
explicit user opt-in; no client may reinterpret the original lineage.

1. **NO CREATION, NO MINT.** An epoch may mint only the quantity assigned to
   challenge-matured, policy-eligible creation-attribution records, up to that
   epoch's cap. Unused capacity expires. It is never minted, carried forward,
   redirected to a treasury, or awarded for participation alone.
2. **COMPLETE SUPPLY ATTRIBUTION.** Every policy-valid minted atomic unit is
   accounted for by exactly one matured creation-attribution entry. The sum of
   an epoch's entries equals its actual MINT quantity exactly. Any
   unattributed unit makes the policy verdict invalid or unknown and can never
   be silently accepted. The initial genesis unit is subject to the same rule
   and must bind a matured founding creation attribution.
3. **THE COMMONS REMAINS FREE.** Holding ZC23 is never required to discover,
   fetch, inspect, build, reproduce, verify, or use public permissively
   licensed packages. Optional paid compute, commissions, maintenance
   contracts, and patronage may exist above the free foundation.
4. **MONEY NEVER BECOMES TRUTH.** ZC23 balance, transfer volume, patronage
   volume, marketplace activity, PageRank, popularity, or storage volume can
   never establish correctness, scientific truth, proof acceptance,
   contributor score, committee weight, or package safety.
5. **MONEY NEVER BUYS REPUTATION.** Transferred ZC23 creates no ZCODE Score,
   committee eligibility, ranking, badge, proof weight, or contributor
   identity. One contribution root may credit one contributor identity once.
6. **PATRONAGE CONTROLS ONLY THE PATRON'S FUNDS.** A holder may direct or lock
   their own ZC23 behind a task or package-continuity policy. That grants no
   control over protocol validity, another person's funds, committee
   selection, local-node policy, or the definition of acceptable evidence.
7. **NO IMPLIED PROPERTY RIGHT.** ZC23 conveys no copyright, exclusive
   licence, package title, release ownership, royalty, dividend, or claim
   against a contributor. A patronage receipt records support, not ownership.
8. **INCOMPATIBLE POLICY MEANS A NEW ASSET.** The genesis policy root remains
   immutable. A future incompatible issuance policy requires a new token ID
   and explicit user opt-in. Clients never reinterpret the original ZC23
   lineage.
9. **ZCLASSIC CONSENSUS REMAINS UNCHANGED.** ZC23 is an application overlay.
   It changes no ZClassic block validity, transaction validity, mining,
   Equihash parameters, activation rule, or proof-of-work consensus.

## Denomination and exact supply arithmetic

The planned denomination is eight decimal places:

```text
decimals                  = 8
atoms_per_ZC23             = 100000000
initial_supply_atoms       = 1 * 100000000
epochs_per_era             = 208
epoch_cap_atoms(era)       = floor(50000 / 2^era) * 100000000
maximum_policy_supply      = 20,798,753.00000000 ZC23
maximum_policy_atoms       = 2,079,875,300,000,000
```

The multiplication occurs after the whole-token floor. There is no
fractional-era tail. Eras stop after the whole-token epoch cap becomes zero.
Summing 208 epochs for whole-token caps `50000, 25000, ... , 1`, plus the one
genesis unit, yields exactly 20,798,753 ZC23. The maximum atomic supply fits in
both signed and unsigned 64-bit arithmetic, but implementations must still use
checked `uint64_t` add and multiply and zero their output on overflow.

The curve is a ceiling, not a promise to issue. For every epoch:

```text
actual_mint_atoms <= epoch_cap_atoms
unissued_atoms == epoch_cap_atoms - actual_mint_atoms
sum(creation_attribution.award_atoms) == actual_mint_atoms
```

Unissued atoms disappear from policy capacity at epoch close. They never enter
a carry pool, treasury, matching fund, later era, patronage budget, or passive
yield schedule.

## Existing owners and trust boundaries

No second truth system is permitted. The implementation reuses:

- package manifests, signed release envelopes, dependency locks, recipes,
  capsules, and the existing ZCODE CAS for public code and evidence bytes;
- `task.v1`, `candidate.v1`, `proof_policy.v1`, `proof_set.v1`,
  `work_receipt.v1`, and `lane_receipt.v1` for contribution evidence;
- the existing ZCODE Score receipt for objective nontransferable units;
- `contributor_binding` plus ZID/ZCL identity binding;
- ZANC/evidence checkpoints for active-chain anchoring and reorg handling;
- generic ZSLP for token transaction validity and the existing wallet
  plan/commit transaction machinery for any future owner-authorized plan;
- `zpkgswm`, signed provider records, DHT discovery, and local sovereignty
  policy for object distribution;
- the existing metaverse `zcode_package` property; and
- ActiveRecord tables only as bounded, wipe-rebuildable indexes.

ZSLP validity and ZC23 policy validity remain separate verdicts. A transaction
can be generically ZSLP-valid while violating the immutable ZC23 schedule or
attribution covenant. Such a lineage is `ZC23_POLICY_INVALID` or `UNKNOWN`,
never silently accepted. Off-schedule minting, baton theft, missing
attributions, or a contradictory active-chain anchor halts the policy lineage;
clients do not invent replacement authority.

## Creation attribution

The first new authority object is planned under the domain
`zcl.zcode.creation_attribution.v1\0`. It is factual, canonical, fixed-width or
strictly bounded, exact-length, little-endian, and domain-separated with a
full SHA3-256 identity. Parsing and validation zero output on failure.

An attribution binds the network genesis, immutable ZC23 policy root, epoch,
contributor-binding root, exact task and candidate, proof policy and proof set,
PROVEN lane receipt, ZCODE Score receipt, package and release, licence text or
licence-evidence root, a closed contribution category or mask, exact award in
atoms, opening anchor, maturity height and median-time-past, optional
predecessor attribution or release-lineage root, and creation time.

Valid categories describe mechanically demonstrated events: new public source,
born-red test plus fix, security repair, benchmark evidence, independent
reproduction, structured review or negative finding, compatibility
maintenance, and long-interval preservation/reproduction. Ordinary upload,
bandwidth, signing, voting, DHT publication, balance, payment, line count,
generated volume, formatting, renaming, version bumps, no-op rebuilds,
delete-and-readd novelty, copied/lightly transformed work, circular review,
or self-funded patronage is not a creation event.

Cross-object validation independently reloads and rederives every referenced
object. It rejects a task/candidate/policy/proof/lane/score mismatch; a lane
below PROVEN; invalid, immature, future, stale, or reorged evidence; absent or
non-permissive licensing; duplicate contribution roots; contributor-binding
mismatch; zero or overflowing awards; epoch mismatch; package/release/recipe/
lock substitution; contradictory source lineage; and any caller assertion not
proved by canonical bytes. One contribution root credits one contributor once.

## Epoch creation set and fungibility

Before adding an epoch wire, implementation must re-audit the planned S10-S12
award/MINT path. At this specification freeze, those units are design-only;
there is no canonical award or epoch-MINT object to duplicate.

If no existing canonical mapping has landed, the minimum epoch creation set
binds network genesis, immutable policy, epoch, previous epoch-creation root,
strictly ordered unique attribution roots, count, epoch cap, awarded/minted
atoms, unissued atoms, committee/evidence snapshot, and opening/maturity
anchors. It validates every attribution and the three amount invariants above
with checked arithmetic. The exact ordered recipient mapping and epoch-creation
root are bound into the existing deterministic MINT transaction plan. Required
on-chain commitments reuse ZANC/evidence checkpoints rather than adding a
second narrative OP_RETURN.

ZC23 is fungible. After transfers and coin selection, a particular UTXO is not
a collectible permanently attached to one package. The valid statement is
aggregate and exact: all policy-valid issued supply is accounted for by
creation attributions. A display must never claim that a mixed token still
belongs to one historical work.

## Patronage, commissions, and continuity

Patronage never creates protocol emission, proof status, score, committee
weight, ownership, or technical truth. It controls only funds the patron is
authorized to direct. V1 is limited to:

1. **EXACT_TASK_COMMISSION** — an existing task, proof policy, amount, expiry,
   and recipient rule; settlement requires the exact accepted candidate,
   complete proof set, PROVEN lane, valid score receipt, and selected challenge
   maturity.
2. **PACKAGE_CONTINUITY** — an exact package lineage, compiler/platform
   transition, proof policy, cycle and amount caps, total cap, and expiry;
   eligible work is demonstrated maintenance, repair, reproduction, or
   preservation rather than code churn.
3. **DIRECT_GIFT** — signed support for a public work or contributor, creating
   no score, committee weight, matching subsidy, proof status, or property
   right.

Before adding `patronage_intent.v1`, `patronage_funding.v1`,
`patronage_settlement.v1`, or `continuity_policy.v1`, audit and extend the
existing task, contract, marketplace, and transaction-plan owners where
possible. A v1 intent binds network genesis and exact ZC23 token ID (or an
explicit simulation placeholder), patron contributor/ZID binding, anonymous-
display choice, closed mode, exact target kind/root, task/policy roots where
required, amount in atoms, creation/expiry, refund height/time, sequence,
maximum ZCL fee, settlement trust mode, and a canonical no-authority flag.

An offer in CAS or DHT is **unfunded** unless a verified funding object and
strict-valid confirmed input prove otherwise. Safe implementation stops at an
unfunded signed offer, a fully simulated funded offer, and a clearly labelled
future 2-of-3 or committee-assisted settlement plan with CLTV refund. ZClassic
script cannot evaluate a ZCODE proof set; conditional proof settlement is not
trustless script enforcement. The committee/cosigner trust and custody gates
must be shown explicitly. Every financial mutation remains plan/commit with
confirmation, exact fee and transaction bytes, expiry, idempotency, reorg
recovery, and named unavailable blockers.

## Rebuildable Living Commons projection

One local projection may index canonical CAS and chain objects for read-only
`zcode commons` (or `zcode canon`) views. It is never authority and must rebuild
byte-identically after a wipe. Read commands remain literally non-creating on
an absent datadir.

Planned views expose policy-valid minted, attributed, and unattributed supply;
challenge-matured creation count; package/release additions; born-red defects;
independent reproductions; security repairs and negative findings;
compatibility-maintained lineages; exact unissued capacity; first integrity
failure; complete/partial/unknown verification; and the next safe diagnostic.
An onion/API presentation comes only after the command/model/service path and
consumes the same projection.

## Ordered safe implementation slices

| Slice | Deliverable | Hard stop |
|---|---|---|
| LC0 | Freeze this covenant, naming, arithmetic, trust boundaries, order, and tests | Specification only |
| LC1 | Pure creation-attribution codec, root, CAS cross-object validation, KATs and parser fuzzing | No database, command, token, or wallet mutation |
| LC2 | Pure epoch creation-set accounting plus simulated deterministic MINT-plan binding | No GENESIS, MINT, SEND, signing share, or live transaction |
| LC3 | One rebuildable projection and non-creating read-only commons commands | No new source of truth or REST silo |
| LC4 | Reused-owner patronage/continuity codecs and unfunded/simulated plan-commit flows | No live funds or custody claim |
| LC5 | Unique continuity-event validation and lineage views | No reward for churn, volume, or self-dealing |

Each code slice begins with born-red adversarial tests, lands as a coherent
green commit, integrates current `origin/main`, and records exact gate
receipts. Heavy lint, uncached suite, sanitizer, LTO, and reproducibility gates
run only when repository coordination permits them; a deferred gate is named,
never implied green.

## Permanent adversarial test plan

Coverage must include repeated-run wire KATs; malformed magic/version/enums/
lengths/trailing bytes; domain confusion; zero/substituted roots; checked
decimal conversion and `uint64_t` overflow; exact maximum supply; attribution
without PROVEN; immature/future/reorged evidence; duplicate contribution;
wrong contributor binding; package/release/recipe/lock or licence substitution;
epoch sums one atom below/above MINT; unattributed MINT; capacity carry-forward;
balance/patronage/rank attempting to affect score or committee weight;
patronage attempting to create emission; unfunded-as-funded display;
cross-task/candidate/policy settlement; refund/settlement/expiry races; reorg
recovery; idempotent commit/rebroadcast; sponsor/worker self-dealing;
projection wipe/rebuild; absent-datadir non-creation; two-node publication/
fetch/rebuild; parser fuzzing; and ASan/UBSan without suppressions.

## Acceptance statement and genesis blockers

The safe foundation is complete only when it can prove, without trusting a
website, committee narrative, or database cache:

> For every policy-valid atomic unit of issued ZC23, there is exactly one
> challenge-matured creation attribution binding it to verifiable public C23
> contribution evidence; total attributed amount equals total policy-valid
> issued supply; unused issuance capacity was never minted; and no token
> balance established technical truth or contributor authority.

Real custody and token genesis remain blocked on completion of the pure policy
and verification slices, challenge-mature founding contributions, green shadow
epochs, exact active-chain/reorg proofs, independent review, custody gates,
owner authorization, and a separately reviewed immutable genesis policy root.
Nothing in this document grants that authorization.

## Implementation ledger

Updated 2026-08-07. This is an implementation record, not token or deployment
authorization.

| State | Slice | Source commit | Integrated `main` | Evidence |
|---|---|---|---|---|
| DONE | LC0 covenant and terminology freeze | `03f13639d` | `1ff4db5a0` | full lint 134/134; pre-push source-wide suite |
| DONE | LC1 fixed creation-attribution wire, identity KAT, checked eight-decimal arithmetic | `0ff09fb68` | `9a8cc8672` | born-red unresolved-symbol gate, focused green, pre-push source-wide suite |
| DONE | LC1 independent CAS cross-object verifier | `f0d1af5a1` | `f9a5c61cb` | focused attribution/Score verticals, full lint 134/134, pre-push source-wide suite |
| DONE | LC2 ordered epoch creation-set wire and cap/no-tail arithmetic | `4cfaf6ebf` | `c41a51de1` | born-red unresolved-symbol gate, root KAT, full lint 134/134, pre-push source-wide suite |
| DONE | LC2 CAS attribution summation and observed-MINT equality gate | `4381781b8` | `36f6f3ae5` | one-atom under/over rejection, focused attribution/Score verticals, full lint 134/134 before integration, combined-tree lint-fast, pre-push source-wide suite |
| DONE | LC3 read-only canonical-CAS projection | `a3424aba6` | `a3424aba6` | absent-workspace non-creation, populated byte-identical rebuild, exact parsed totals, full lint 134/134, pre-push source-wide suite |
| DONE | LC3 `zcode commons` read views | `84a54696a` | `84a54696a` | born-red unresolved handlers; status/epoch/creation/lineage/verify/rebuild green; command-key and generated-reference gates; full lint 134/134; pre-push source-wide suite |
| DONE | LC4 simulation-only signed patronage intent | `94141b969` | `94141b969` | born-red unresolved symbols; exact wire, closed mode/trust/target enums, no-authority and simulation flags, focused green, full lint 134/134, pre-push source-wide suite |
| DONE | LC4 intent CAS authority revalidation | `c1123dbab` | `6da2651dd` | exact patron/recipient binding, task, policy, package/creation target and network reloads; focused green, full lint 134/134, normal pre-push source-wide suite |
| DONE | LC4 fully simulated funding receipt | `c804a20ae` | `c804a20ae` | exact intent reload, deterministic plan root, no-live-funds/no-transaction-bytes gates, focused green, full lint 134/134, pre-push source-wide suite |
| DONE | LC4 pure settlement/refund receipt | `f618eb6c5` | `f618eb6c5` | 504-byte wire KAT, truncation zeroing, closed simulation flags, complete-or-empty evidence, signature mutation, focused green, full lint 134/134, pre-push source-wide suite |
| DONE | LC4 settlement/refund CAS authority revalidation | `264df17ee` | `e2185eb7c` | historical intent/funding reload, full creation-evidence rederivation, exact target and recipient binding, maturity/reorg/refund gates, focused green, full lint 134/134, normal pre-push source-wide suite |
| DONE | Shared people-and-AI mission and API contract | `d9eea8e09` | `d9eea8e09` | exact mission language, same API/evidence rules, no-world-ownership boundary, READY-versus-PLANNED truth, generated API reference, full lint 134/134, normal pre-push source-wide suite |
| DONE | LC4 offer/funding plan-commit and exact-root show commands | `788f93149` | `c9c3e786a` | signed exact-wire input, caller-pinned context, CAS revalidation, absent-workspace plan non-creation, simulation/funding truth labels, planned settlement/refund/list fail closed, focused green and full lint 134/134 |
| DONE | Simulation-only transaction classification | `e90c17ed6` | `e90c17ed6` | exact non-chain declaration for funding CAS commit; API reverse-mapping gate green; 901 active pre-push test groups green |
| DONE | LC5 bounded signed continuity-policy wire | `a9cff6c45` | `d2facab04` | exact wire KAT, closed event/capsule enums, checked cycle and amount caps, anti-churn invariants, focused green, full lint 134/134, normal pre-push source-wide suite |
| DONE | LC5 continuity-policy CAS authority rederivation | `eb258785a` | `d2facab04` | historical package/release/proof-policy/capsule reloads, package-lineage and cap checks, focused green, normal pre-push source-wide suite |
| DONE | LC5 `zcode continuity plan\|commit\|status` commands | `90bb3d3cd` | `90bb3d3cd` | read-only plan on an absent workspace, exact signed CAS commit, exact-root status revalidation, lossless `uint64_t` display, no-funds/no-income/no-score/no-emission labels, focused green, full lint 134/134, normal pre-push source-wide suite |
| DONE | LC5 unique continuity-event validation | `ba9f8d0d0` | `dfa62d73f` | domain-separated event key KAT; one package/capsule transition or defect root credits once; born-red/security category splitting rejected; continuity policy and evidence independently reloaded; focused green, full lint 134/134, normal pre-push source-wide suite |
| DONE | LC4 rebuildable patronage list | `fc0813d2a` | `fc0813d2a` | deterministic CAS projection, historically signed intent/funding/continuity verification, first-failure reporting, absent-workspace non-creation, every row truthfully `funded:false` and `persisted:false`; focused green, full lint 134/134, normal pre-push 910-group suite |
| DONE | Living Commons parser truncation sweeps | `cbaae2112` | `cbaae2112` | every truncation of creation-attribution, epoch-creation, patronage-intent/funding/settlement and continuity-policy wires fails closed; focused GCC groups green; Clang 20 ASan+UBSan green with fail-fast UBSan; full lint 134/134; normal pre-push 910-group suite |
| DONE | Six-arm Living Commons parser fuzzer | `a0cfe7fe4` | `a0cfe7fe4` | shared libFuzzer owner, one seed per canonical parser, ASan+UBSan and leak detection; 1,537,885 mutations in 31 seconds at 49,609/s with no finding; full lint 134/134 |

The `36f6f3ae5` push integrated concurrent `main` commit `00a0c54c8` through
lane merge `4c8e7abe2`; no concurrent file was overwritten. Two complete
pre-push attempts were blocked only by host-variable `test_simnet_perf`
detector measurements. The exact group then passed on the same combined SHA
(clean growth 1065 permille, injected growth 3437 permille), and the final
normal, hook-enabled push passed. The failed attempts are not counted as
passed gates.

The `e90c17ed6` push integrated concurrent `main` commit `8265c423f` through
lane merge `c9c3e786a`; no concurrent file was overwritten. Its first normal
push attempt exposed an uncovered transaction-shaped command in `test_api`.
The command was then explicitly classified as simulation-only/non-chain,
the focused API gate and full lint passed, and the second normal push passed
all 901 active pre-push groups. The failed attempt is not counted as a passed
gate.

Continuity authority integrated concurrent `main` through `def31919d` and
`d2facab04`. Unique continuity evidence later integrated concurrent commit
`64f6a8b66` through `dfa62d73f`; the combined tree exposed a pre-existing
pipefail-sensitive parity shell assertion, fixed narrowly in `db1a0f271`.
No concurrent implementation was overwritten, and the final normal pushes
passed the full source-wide suite.

The permanent parser truncation sweeps passed under GCC focused tests and
under Clang 20 ASan+UBSan with `UBSAN_OPTIONS=halt_on_error=1`. Receipt
`000022` records that the default GCC sanitizer profile is blocked before the
selected tests by the pre-existing Sapling AVX-512 inline assembly failing to
compile with "impossible constraints"; this is not counted as a sanitizer
pass. Clang receipts `000023` and `000024` are passes with no sanitizer
findings. Two different absolute-path builds produced a byte-identical shipped
binary, 22,793,048 bytes with SHA3-256
`29d8305557a31903e0bafca9f85f08bfda1bb26b93bb8d04ae896b83bd7e82e1`
(`000026`). This is `local_reproduction`, not approved independent off-host
reproduction and earns no independent-reproduction unit.

The permanent `fuzz_zcode_commons` target covers creation attribution, epoch
creation, patronage intent, patronage funding, patronage settlement/refund and
continuity policy through the repository's shared libFuzzer object tree.
Receipt `000027` is the born-red missing-target result; `000028` proves the
harness builds, and `000029` records 1,537,885 leak-detecting ASan+UBSan
mutations with no finding. This bounded local run is parser hardening, not a
claim of exhaustive input-space coverage.

The final explicit `--no-cache` source-wide run executed 901 of 910 registered
groups; nine parameter-heavy groups were policy-gated and 22 tests reported
their documented self-skip markers. Its first attempt (`000033`) had one
load-sensitive failure in the runner's nested exact-selector self-test. The
exact group passed alone (`000034`), and the repository-prescribed complete
cold retry passed 901/901 with zero cached groups (`000035`). The failed first
attempt remains recorded and is not counted as a passed gate.

Final evidence closure integrated concurrent `main` commit `58f11e335`
through lane merge `af5865a0fa`; its service-envelope and live-state
documentation was retained without modification.

LC2's canonical set and verifier already own award truth: they independently
reload every ordered attribution, check active-chain maturity/reorg context,
checked-sum award atoms and require exact equality with observed MINT. The
existing generic ZSLP MINT builder has one recipient, while the durable wallet
intent owner binds exact transaction bytes. Therefore the final ordered
recipient/transaction adapter remains owner-gated with real custody and is not
replaced by a parallel canonical object. LC3 intentionally reports `partial`
or `unknown` rather than policy-valid supply until immutable-policy and
active-chain anchor context is wired. LC4 settlement/refund commands remain
planned because their validators require authentic active-chain,
immutable-policy and uniqueness callbacks; caller assertions cannot substitute
for those authorities. LC5 policy, commands and unique continuity validation
are complete.

No live token, GENESIS, MINT, SEND, wallet, canonical datadir, production port,
deployment, service, or consensus path was touched by these slices.
