# ZCODE agentic development network

Status: active implementation contract, 2026-08-01. This document supersedes
the payout-first tail of [`ZCODE_PLAN.md`](./ZCODE_PLAN.md). The package-hosting
work remains the foundation; its old slice numbers are no longer the execution
order.

## Mission

ZCODE is a free, requester-coordinated P2P C23 development network: immutable
source discovery and fetch, model-neutral tasks, candidate trees, confined
build/test/fuzz work, independent review, local reproduction, explicit
acceptance, and publication back to the package swarm. No company, account,
payment, proprietary model, global scheduler, or trusted coordinator is a
protocol requirement.

Hashes are authority. ZNAM is an optional mutable name. Codex, Claude, Kimi,
local open models, and future harnesses are adapters selected by the user; an
adapter name never participates in source or proof authority.

Basic discovery, fetch, local development, and a bounded peer bootstrap quota
remain free. Optional reciprocity, reputation, or ZCODE rewards may expand a
peer's local quota later. Tokens are never an access key.

## One object graph, existing owners

There is no second package store, lock resolver, code index, sandbox, build
ledger, worker trust list, or P2P transport.

| Fact | Canonical owner reused by this program | Current truth |
|---|---|---|
| Source tree and chunks | `content.v2` in `lib/vcs/package_manifest.*` and the ZCODE CAS | Live |
| Published release | `lib/vcs/package_release.*` and package-add lifecycle | Live |
| Dependency lock | `vcs_package_lock` in `lib/vcs/package_deps.*` | Live, root-pinned DAG |
| Declarative C23 graph | `vcs_package_recipe` | Live for one library/test package; workspace executables and multi-package targets still need an extension |
| Source snapshot identity | resident dev source CAS plus content.v2 for portable package trees | Live locally; not yet one network task surface |
| Code context | `lib/codeindex/` plus immutable source chunks | Live index; bounded context-capsule publication is not yet wired |
| Fixed build action | `vcs_build_action_v1` | Live codec; V1 is preprocessed-TU compile only |
| Build coordinator ledger | `build_jobs`, `build_actions`, `build_workers`, `build_receipts` | Live schema v43, models, native/REST reads, plan/submit/cancel/trust/lease-bound receipt checks |
| Local package confinement | `zclassic23-package-verify` | Live: declarative recipe, Landlock/seccomp/rlimits, no network |
| ZBuild worker execution | existing build-fabric runtime | Live locally for the fixed preprocessed-TU GCC action when `-buildworker` is enabled: durable identity, bounded leases, full confinement, CAS artifact, signed receipt, and local fallback |
| Package P2P | `package_swarm_node` and `zpkgswm` | Live for immutable package bytes |
| Work P2P | signed work frames over package swarm/CAS | Live `ZCWS` multiplex on existing `zpkgswm` sessions; canonical context fetch, ZBuild admission, cancellation, and signed result return are live. Requester-side durable result indexing and quorum orchestration remain pending |
| Agent authority | metaverse grants and signed receipt chain | Live for scoped property operations; task work must never inherit wallet or canonical-node authority |
| Durability lanes | ZCODE promotion records over source roots | Not live |

The control-plane distinction is load-bearing: a READY command or database row
does not prove an executor exists. `build.worker` now has an actual supervised
thread and fixed compiler action; broader recipe test/fuzz actions and remote
dispatch remain missing.

## Canonical development objects

The first implementation lives in `lib/vcs/include/vcs/zcode_dev.h` and
`lib/vcs/src/zcode_dev.c`. Each object has one fixed-width, closed binary wire,
a version-specific magic, little-endian integers, a domain-separated SHA3-256
identity, exact-length parsing, and named rejection reasons. JSON is display
only.

The domains are:

- `zcl.zcode.task.v1\0`
- `zcl.zcode.candidate.v1\0`
- `zcl.zcode.proof_policy.v1\0`
- `zcl.zcode.review.v1\0`
- `zcl.zcode.work_receipt.v1\0`

The trailing NUL is part of each SHA3 preimage, matching the existing package
manifest, recipe, lock, release, and attestation convention.

### `proof_policy.v1`

The requester chooses the required proof set and quorum. The object binds:

- compile, test, fuzz, review, and local-reproduction requirements;
- minimum receipts for each required class and minimum matching receipts;
- independent-signer and release-byte-identity requirements;
- deterministic fuzz seed count, audit sample in basis points, and maximum
  proof age.

A class not required must carry a zero minimum. A required class must carry a
nonzero minimum. This gives every policy one encoding and prevents a display
adapter from inventing implied defaults.

### `task.v1`

A task binds exactly:

- source root;
- existing `vcs_package_lock` root;
- existing `vcs_toolchain_capsule_v1` root;
- immutable write-scope manifest root;
- immutable acceptance-test manifest root;
- `proof_policy.v1` root;
- user-selected model-policy root;
- immutable goal root;
- the only v1 agent capabilities: source read, candidate-tree write, and fixed
  action execution;
- changed-file, patch-byte, context-byte, CPU, memory, and output ceilings;
- expiry.

Wallet access, canonical-node mutation, arbitrary process execution, arbitrary
shell, package-provided capability escalation, and worker network access are
not values in the v1 capability vocabulary.

### `candidate.v1`

A candidate binds the task, exact base source, patch object, complete candidate
source tree, user-selected adapter/model policy, author public key, sequence,
and creation time. Its author label establishes provenance only. Acceptance is
driven by proofs and an explicit user action, never by model brand.

### `review.v1`

A review binds the task, candidate, proof policy, immutable proof set,
immutable findings, reviewer public key, verdict, sequence, and time. The
verdict is one of approve, request-changes, or reject. Authorship is supplied
by a signed `work_receipt.v1` whose output is the review root; the review object
does not create a second signature system.

### `work_receipt.v1`

A work receipt binds task, candidate, fixed action, exact input and output,
proof policy, toolchain capsule, lease, evidence manifest, achieved confinement
manifest, work kind, status, exit status, start/finish times, and signer key.
Ed25519 signs its domain-separated receipt ID. The verifier pins the expected
signer; trusting the embedded key alone is forbidden.

Cross-object validation refuses stale source, task, candidate, policy,
toolchain, output, or expiry state. A structurally valid old receipt therefore
cannot authorize a moved task.

### `zcode-work-context.v1`

Remote execution adds no source store or transfer protocol. One fixed context
wire is carried as a normal one-file, multi-chunk `content.v2` package at the
canonical path `zcode-work-context.v1`. Its closed binary grammar binds the
existing `task.v1`, `candidate.v1`, and `proof_policy.v1` wires, the candidate
source SHA-256 oracle, build profile, and exact preprocessed TU. Its total size
remains under the package store's existing 64 MiB anti-abuse cap and the task's
smaller context ceiling.

The receiving peer re-derives the task, candidate, policy, input, toolchain,
and `build_action.v1` roots and compares every one to the signed request before
writing the objects to the existing workspace CAS or planning the existing
ZBuild action. It never accepts a caller-supplied path or command.

<!-- claim: file-present lib/vcs/include/vcs/zcode_dev.h # the canonical object contract -->
<!-- claim: symbol-present vcs_zcode_work_receipt_verify lib/vcs/src/zcode_dev.c # signed receipt verification -->
<!-- claim: symbol-present vcs_zcode_work_receipt_validate_for_candidate lib/vcs/src/zcode_dev.c # cross-object staleness gate -->

## Ordered delivery

### A. Canonical object foundation

- [x] Define the five v1 wires and domain-separated identities.
- [x] Bind task source, lock, toolchain, scope, tests, limits, model policy,
  proof policy, and expiry.
- [x] Sign and verify work receipts with pinned Ed25519 signers.
- [x] Add byte KATs, round trips, malformed-wire checks, and stale-root checks.
- [ ] Store these wires through the existing ZCODE CAS and project a local task
  index from CAS objects rather than creating task tables as a second truth.
  CAS storage and the ZBuild projection are live; local search/index projection
  remains.
- [ ] Publish a bounded code-index context capsule whose members resolve to
  the task's immutable source root.

### B. Complete the existing local ZBuild worker

- [x] Add lease owner, lease token, lease expiry, attempt count, heartbeat, and
  cancellation observation to the existing build action ledger through an
  idempotent migration and model-owned writes.
- [x] Claim `QUEUED` actions atomically. A restart requeues an expired lease; a
  live lease cannot be stolen.
- [x] Execute only registered fixed action kinds. V1 continues to use local
  preprocessing followed by fixed `cc -x cpp-output -c`; package recipe
  build/test/fuzz uses the existing package verifier confinement.
- [x] Establish Landlock, seccomp, rlimits, an empty environment allowlist, fixed
  virtual paths, no network, bounded stdout/stderr, cancellation, and a hard
  deadline before untrusted bytes run. Cancellation currently prevents stale
  publication; prompt child termination is still pending.
- [x] Fetch inputs only by immutable CAS root. Recheck task, candidate, policy,
  action input, source, toolchain, fixed flags/environment, signer, and lease
  before execution and again before receipt publication. The dependency-lock
  root is task-bound; portable lock-byte admission remains part of the context
  package work.
- [x] Store output chunks and a build-artifact manifest in the existing CAS,
  then sign `work_receipt.v1` and the existing ZBuild receipt projection. The
  database projection binds the canonical receipt root; the wire remains CAS
  authority.
- [ ] On timeout, crash, cancellation, malformed output, stale state, or sandbox
  failure: publish a named outcome and use the existing local fallback policy;
  never wait forever.

### C. Typed local path

The first typed adapters now exist on the existing `zcode` branch; no
bash-only authority:

- [x] `zcode create` — plan or commit a declarative C23 package through the
  existing signed package publication lifecycle.
- [x] `zcode use` — resolve/fetch/plan/commit an exact root-pinned dependency,
  reusing `zcode package add` and its lock/install receipts.
- [ ] `zcode improve` — plan a task, build a bounded context, invoke a selected
  adapter, admit candidate trees, schedule proofs/reviews, reproduce, explicitly
  accept, and publish.

`zcode improve` now admits canonical task/policy/goal/input/candidate objects in
the existing workspace CAS, captures the GCC capsule, and queues a
candidate-bound compile action. A local worker emits the canonical signed work
receipt. With `remote_peer`, it builds the canonical context package itself and
signs and queues the exact request for that user-selected advertised peer; an
unavailable package store, peer, or capability reports `LOCAL_FALLBACK` and
preserves the local action. Adapter invocation, review, explicit acceptance,
and publication are still separate missing stages and are not claimed by
command discovery.

### D. Requester-coordinated P2P work

- [x] Freeze signed capability/request/result/cancel frames. Requests bind the
  semantic object roots plus one content.v2 context-package root; advertisements
  bind target, capsule, confinement, ceilings, slots, headroom, and expiry.
- [x] Verify exact result bindings and count only approved, distinct signers
  returning one matching output root toward quorum.
- [x] Dispatch those frames over the existing `zpkgswm` peer sessions. The
  bounded adapter authenticates capabilities, rejects replay/unrequested/
  altered frames, preserves requester-selected peers, and schedules the exact
  content.v2 context root through the existing package fetcher.
- [x] Drain a complete, canonical fetched context into the existing ZBuild
  ledger, propagate signed cancellation, and return the accepted action's
  canonical receipt/result. A requester retry after worker restart reattaches
  to the idempotent durable action; no in-memory request queue is authority.
- [ ] Persist verified requester results as untrusted CAS projections and drive
  the selected local-reproduction or distinct-matching-signer quorum decision.
- Extend peer advertisements with bounded action kinds, toolchain capsule,
  target, confinement facts, resource ceilings, queue headroom, and expiry.
- The requester owns job selection, leases, cancellation, quorum, and local
  fallback. There is no global scheduler.
- Reuse package swarm/CAS transfer for immutable task inputs and artifact
  manifests. Add strict work-request/cancel/result frames; never embed a shell
  command or mutable path.
- A peer result is untrusted until the requester either reproduces it locally
  or obtains the selected number of matching independent receipts.
- Replayed, duplicate, revoked, expired, wrong-action, wrong-toolchain, and
  stale-source receipts are named refusals.

### E. Durability lanes

Every source root is independently in one immutable lane:

```text
FRONTIER  ->  CANDIDATE  ->  PROVEN
```

- FRONTIER keeps moving under fast deterministic proof policy.
- CANDIDATE pins one exact source root for multi-node, restart, disruption,
  and chaos evidence.
- PROVEN contains only roots whose long-duration policy is complete.
- A durability failure attaches to and blocks promotion of that root only. It
  publishes a reproducible work receipt and seeds a new task. It never freezes
  unrelated FRONTIER work.

Promotion is a signed projection over immutable source and proof-set roots,
not a mutable branch name. Publication may name lanes for discovery, but names
never replace roots.

## Acceptance demonstrations

The first network proof uses independent nodes and no GitHub or central
service:

1. publish and fetch a declarative C23 package;
2. submit a seeded bug-fix `task.v1`;
3. obtain a remote candidate tree and signed work receipt;
4. obtain independent build and test receipts;
5. reproduce the selected candidate locally;
6. explicitly accept it and publish the new signed release;
7. restart every participant and prove the task, leases, candidate, evidence,
   acceptance, and release remain reconstructible from durable objects.

The self-hosting milestone repeats the path for a real Zclassic23 change.

## Reward ordering

Real token payouts and decentralized custody remain later work. First make
useful development produce trustworthy, reproducible receipts. Then rewards
may batch owner-reviewed facts for accepted patches, defect-catching tests,
independent reproduction, and verified seeding. Self-reported CPU time earns
nothing.
