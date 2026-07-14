# LLM-first C23 App and Game Platform — execution checklist

**Status:** execution companion for Phases 3–5 of
[`SOVEREIGN-NETWORK-ROADMAP.md`](./SOVEREIGN-NETWORK-ROADMAP.md), not a new
priority queue.

[`../HANDOFF.md`](../HANDOFF.md) owns current live facts,
[`FORWARD_PLAN.md`](./FORWARD_PLAN.md) owns immediate execution order, and
[`../MVP.md`](../MVP.md) owns the v1 acceptance bar. The sovereign complete-state
cure remains priority #1 and zero-MCP remains the secondary development track.
Nothing in this checklist authorizes runtime publication, canonical deployment,
or mutation of the protected mint producer.

This document turns the long-term product direction into checkable work:

> An LLM edits a small amount of ordinary C23. ZClassic23 derives the affected
> interfaces and mandatory proofs, automatically records correlated telemetry,
> and returns one compact green receipt or one deterministic failure capsule.
> Third-party Apps and games use the node through bounded capabilities and never
> execute inside the consensus process.

## How to use this checklist

- Complete items only with a commit/source epoch, named proof, and durable
  artifact or command receipt. A document claim by itself is not completion.
- Treat unchecked items as unavailable even when a menu or prototype describes
  them. Verify command availability with `discover describe <path>`.
- A required `SKIP`, stale receipt, omitted dirty path, retry-only green, or
  missing inspection tool is RED, not partial success.
- Keep consensus `progress.kv` authoritative. Development and telemetry stores
  are non-consensus evidence and must never authorize chain state.
- Work in isolated worktrees with disjoint file scope. Never use the dirty main
  worktree as an implicit source-epoch boundary.
- Profile before optimizing. Token, latency, CPU, memory, and telemetry-overhead
  claims require measured artifacts.
- Do not check an item off by weakening parity, proof selection, sandboxing,
  privacy, durability, or rollback requirements.
- Use exactly four availability labels: `ready`, `compat-contained`, `planned`,
  and `blocked`. Generate the long-term availability matrix from the native
  registry instead of maintaining a second hand-written catalog.

Evidence format for a completed item:

```text
- [x] <item>
  Evidence: commit=<sha> source_epoch=<id> proof=<receipt/artifact>
  Tests: <exact groups/commands and result>
  Measurement: <before/after when the item makes a performance claim>
```

## External promotion prerequisites — owned elsewhere

Design, measurement, and hermetic simulation in this checklist may proceed in
isolated lanes. No App/runtime/package promotion may jump these gates, which
remain owned by `HANDOFF.md`, `FORWARD_PLAN.md`, `MVP.md`, and the sovereign
roadmap:

- [ ] Copy-prove the complete transparent, Sapling, Sprout, and nullifier cure.
- [ ] Prove H* climbs, exact same-height/full-state parity, warm restart, kill-9
  resume, and malformed-input no-publish behavior.
- [ ] Obtain explicit owner authorization before canonical deployment.
- [ ] Earn MVP 8/8 and a fresh clean 168-hour exact-candidate soak.
- [ ] Complete zero-MCP consumer migration and prove native mutation, auth,
  audit, event, metric, notification, secret, and rollback parity.
- [ ] Delete the MCP server only after its native replacements and deletion
  inventory pass; do not preserve a future "compact MCP" architecture.
- [ ] Complete commit-bound quality/release evidence before calling an App or
  package stable.

## Architecture decisions — freeze these first

- [ ] Record an ADR for the three authority planes:
  `consensus`, `development/telemetry`, and `sandboxed App runtime`.
- [ ] Freeze the promotion rule for LLM-authored C23/HTML/Markdown:
  `source → validation → proof DAG → content/manifest digest → capability
  grant → publisher signature → optional chain anchor`.
- [ ] State that signatures and chain anchors prove provenance, byte integrity,
  ordering, and revocation—not code correctness. Compiler warnings, model
  validations, deterministic tests, sandboxing, and review remain independent
  release gates.
- [ ] State explicitly that telemetry is evidence, never consensus authority.
- [ ] State explicitly that an in-process native C module is trusted code, not a
  capability sandbox.
- [ ] Keep audited built-ins statically linked; run fetched or third-party C23
  Apps in a separate same-binary worker process.
- [ ] Use one manifest to generate commands, routes, topics, schemas, telemetry,
  simulations, documentation, and proof mappings.
- [ ] Use one native command ontology. Extend existing leaves before adding
  parallel AI-specific commands.
- [ ] Store full logs and traces as content-addressed artifacts; default command
  responses contain bounded semantic projections and artifact references.
- [ ] Use `.zclpkg` / `zcl.package_manifest.v1` from the sovereign roadmap for
  Apps and games. Do not introduce a parallel `.zapp` package format.
- [ ] Require explicit local approval for new capabilities, publisher changes,
  migrations below the rollback floor, and wallet authority.
- [ ] Keep signing keys in Core. Apps submit typed intent through an opaque,
  generation-bound grant; they never choose a raw key, capability bitmask,
  chain ID, App ID, or topic at the signing boundary.
- [ ] Keep dynamic loading on canonical, soak, and release lanes at zero.
- [ ] Name the existing event stores precisely instead of calling all of them
  "the event log":
  - `progress.kv` is consensus/reducer authority;
  - `lib/storage/event_log` is durable application/projection replay;
  - `lib/event` is volatile bounded observability;
  - development receipts and the proposed telemetry ledger are non-consensus
    evidence.
- [ ] Permit information to flow from consensus state into diagnostics, but
  never from telemetry loss, truncation, or contents into consensus validity or
  cursor advancement.

## Current baseline — verify fresh

These checked items are foundations already present, not proof that the target
platform is complete.

- [x] The native registry has typed effect/risk/authority/cost/capability
  metadata and hard response budgets.
  Evidence: `lib/kernel/include/kernel/command_registry.h`.
- [x] The native `code` branch exposes bounded map/group/room/file/symbol/ref/test
  queries.
  Evidence: `lib/codeindex/`, `tools/command/native_code_command.c`.
- [x] The verify-only native dev loop can classify changes, run proofs, persist a
  verdict, and refuse runtime publication.
  Evidence: `tools/dev/`, [`DEV-WATCH-PLAN.md`](./DEV-WATCH-PLAN.md).
- [x] A public App ABI skeleton declares capabilities, routes, topics, state,
  migration, self-test, quiescence, and leases without exposing consensus or
  private keys.
  Evidence: `sdk/include/zclassic23/app.h`.
- [x] `apps/social/app.def` and a deterministic Social simulation prove the
  manifest and seeded-scenario shape.
- [x] The node has a bounded event ring, traces, metrics, state dumpers,
  postmortem capsules, deterministic simnet, and 64-bit replay seeds.
- [x] Runtime generation apply, resident `dlopen` probing, deploy-dev, and
  canonical mutation paths fail closed during Phase-0 containment.

Known gaps that remain unchecked:

- [ ] One authoritative, worktree-scoped development receipt history.
- [ ] Durable failure IDs and a useful `dev.diagnose.show` implementation.
- [ ] One C-owned proof DAG shared by watcher, CLI, CI, and code navigation.
- [ ] Concurrent-reader-safe, daemon-owned semantic index snapshots.
- [ ] Typed, durable, correlated telemetry across node, dev loop, Apps, games,
  and long-running jobs.
- [ ] Strict manifest compiler and generated App catalogs.
- [ ] Generic AppSim executing the same ABI used by production.
- [ ] Host-owned transactional App state/events/projections.
- [ ] Out-of-process App worker and capability broker.
- [ ] Generic App-topic transport and deterministic Game SDK.
- [ ] Source-first signed package build, discovery, upgrade, and rollback.

### Native availability snapshot

This is a 2026-07-14 orientation snapshot, not authority. Re-check the running
registry with `discover describe` because readiness can change by source epoch.

| Status | Current surface |
|---|---|
| `ready` | `discover.*`; `code.*`; `dev app list/describe/plan/simulate`; `dev change plan`; `dev test plan/background`; `app list/inspect`; primary `ops health/diagnose/state/logs/timeline/metrics/selftest/debug/recovery` reads |
| `compat-contained` | verify-only `dev loop ensure/status/wait/stop`; `dev diagnose latest`; generation reads; apply/hot-swap compatibility leaves, which confer no publication authority |
| `planned` | `dev app scaffold/inspect/publish`; `dev loop events`; `dev diagnose show`; `dev test replay`; general `ops jobs`; App package/content commands; out-of-process App runtime; generic Game SDK |
| `blocked` | canonical/dev publication and resident dynamic loading while Phase-0 containment applies |

The current Social App is useful prototype evidence: its deterministic
simulation is ready, but runtime invocation is checkout-only and is not a
sandbox/runtime proof.

## Reference Blog promotion checklist

The working-tree Blog demonstrates the intended MVC shape at `/blog`, but none
of these items is complete until it has a clean source epoch and durable proof
receipt under this document's evidence rule.

- [ ] Manifest: `apps/blog/app.def` is discoverable through `app.list`,
  `app.inspect`, and `dev app describe`, with no undeclared route/topic/grant.
- [ ] Migration: BlogPost and BlogPublicationReceipt schema applies once,
  reopens idempotently, has the relationship indexes, and fails closed on a
  future schema version.
- [ ] ActiveRecord: every write uses lifecycle callbacks; canonical UTF-8,
  slug, identity-key/address, sequence, timestamp, and receipt invariants have
  boundary tests.
- [ ] Relationships: `BlogPost has_many PublicationReceipts`,
  `PublicationReceipt belongs_to BlogPost`, and sequenced posts explicitly
  resolve their previous post; parent drift and re-parenting fail closed.
- [ ] Determinism: all valid event forks are retained by event ID, and nodes
  receiving them in opposite orders select the same canonical slug route.
- [ ] Wallet boundary: malformed requests fail before signing; the Core-owned
  opaque binding revalidates the live grant/generation/revision on every sign;
  private keys never cross the host boundary.
- [ ] Identity: signature/key ID/address verify on every detail and index read;
  ZNAM historical owner epochs are proven at event and anchor heights.
- [ ] Chain: the exact minimal `ZBLG` script is parsed; reorg classification is
  refreshed on production reads; H*, active-slot, and canonical block-body
  proof distinguish projection evidence from finality.
- [ ] Availability: signed event bodies replicate through bounded P2P/onion
  anti-entropy and a late joiner can recover through an alternate relay.
- [ ] Presentation: the same accessible, responsive, escaped page is served at
  `https://zclnet.net/blog` and node onions; unavailable proof storage returns
  503 and never falls back to unsigned legacy content.
- [ ] Publication: a reviewed wallet composer broadcasts the anchor only after
  explicit operator approval, and revocation/transfer behavior is tested.
- [ ] Proof receipt: Blog, dev-platform, migration, model, network, lint,
  build, deterministic simulation, and full regression gates bind to one clean
  commit/source epoch.

## Reference App ladder: Blog → Social → Chat

Do not grow three bespoke protocols. Blog, Social Feed, and Chat consume one
immutable signed `AppEvent` ActiveRecord store, generic topic relay, manifest
compiler, capability broker, telemetry envelope, and HTTPS/onion dispatcher.

### Shared event and relay substrate

- [ ] Add immutable AppEvent persistence keyed by event ID, with indexes for
  app/topic/receive cursor and app/author/sequence/event ID.
- [ ] Express previous/successor/projection relationships as model APIs; retain
  all valid forks and select projections deterministically, never by arrival.
- [ ] Add generic signed `inventory/get/event` anti-entropy for
  manifest-declared topics. Verify scope, size, event ID, signature, replay, and
  rate policy before persistence or relay.
- [ ] Keep network callbacks nonblocking: bounded enqueue to one database owner,
  explicit backpressure/drop telemetry, no peer-lock database work.
- [ ] Do not promote legacy ZMSG into secure Chat: its current P2P payload is
  plaintext/unsigned. Preserve it as compatibility until the signed generic
  relay and end-to-end envelope replace it.

### Social Feed reference

- [ ] Generate Profiles, Posts, Follows, Reactions, and FeedCursors through the
  same MVC/ActiveRecord scaffold used by Blog.
- [ ] Add explicit author/parent/replies/following/followers relationships and
  covering indexes; use keyset cursors, never offset pagination.
- [ ] Keep ranking and moderation deterministic/local policy. A relay may hide
  an event but cannot mutate its ID, signature, or another node's projection.
- [ ] Serve one escaped responsive `/social` resource identically over HTTPS
  and onion; keep public reads separate from authenticated native mutations.
- [ ] Promote the existing Social simulation only when it invokes the same
  production AppEvent storage/broker/relay path.

### High-performance Chat reference

- [ ] Generate Conversations, Memberships, Messages, DeliveryReceipts,
  ReadCursors, and DeviceKeys with explicit relationships and compound indexes
  for conversation/order/event and recipient/state/time.
- [ ] Ship signed public rooms first and label them plaintext. Then add private
  store-and-forward envelopes using Core-owned X25519-safe device keys, HKDF,
  and ChaCha20-Poly1305; hop Noise alone is not end-to-end encryption.
- [ ] Apps never receive wallet or encryption private keys. Core signs/encrypts
  typed intent under exact generation/grant/device policy.
- [ ] Batch high-volume ordering commitments into signed/Merkle roots; do not
  create one blockchain transaction per chat message.
- [ ] Measure before claiming speed: event encode/verify, AR batch/WAL/fsync,
  feed query p50/p95/p99, ingest-to-visible, send-to-ack, partition catch-up,
  saturation/backpressure, bytes/event, RSS, and HTTPS/onion render cost.
- [ ] Pin query plans and record hardware, build/source identity, seed, fixture
  cardinality, WAL mode, and raw artifacts with each performance receipt.
- [ ] Prove malformed/tampered/wrong-recipient/replay rejection, AEAD and
  low-order X25519 KATs, offline delivery, alternate-relay censorship bypass,
  partition/rejoin, late join, route parity, bounded queues, and privacy-safe
  telemetry without IDs/names as metric labels.

## LLM operating checklist — available today

Use this loop while the later platform items remain incomplete.

### 1. Establish authority and scope

- [ ] Run `pwd`, `git status --short`, and `git worktree list --porcelain`.
- [ ] Read [`../HANDOFF.md`](../HANDOFF.md) for live facts and protected
  processes.
- [ ] Read [`FORWARD_PLAN.md`](./FORWARD_PLAN.md) before treating this future
  platform work as the active queue.
- [ ] Read [`../AGENT_TRAPS.md`](../AGENT_TRAPS.md) before fixing an apparent
  defect.
- [ ] For consensus-adjacent work, read
  [`../CONSENSUS_PARITY_DOCTRINE.md`](../CONSENSUS_PARITY_DOCTRINE.md) and
  [`../DEFENSIVE_CODING.md`](../DEFENSIVE_CODING.md).
- [ ] Confirm the assignment owns an isolated file set and cannot disturb the
  protected producer, canonical datadir, soak lane, or another worker.

### 2. Discover before reading files

- [ ] Run `zclassic23-dev discover search <topic>`.
- [ ] Run `zclassic23-dev discover describe <leaf>` and verify
  `availability=ready` before relying on it.
- [ ] Run `zclassic23-dev code room <path>` for purpose, shape, neighbors, and
  test routing.
- [ ] Run `zclassic23-dev code sym <symbol>` for the definition/signature.
- [ ] Run `zclassic23-dev code refs <symbol>` before changing or deleting it.
- [ ] Run `zclassic23-dev code tests <path>` for the focused proof mapping.
- [ ] Fall back to `rg` and bounded file reads only for information the source
  navigator does not index, such as prose, comments, `.def` files, and plans.

### 3. Plan the exact change

- [ ] Run `zclassic23-dev dev change plan --input='{"files":[...]}'`.
- [ ] Run `zclassic23-dev dev test plan --input='{"files":[...]}'`.
- [ ] For an App resource, run
  `zclassic23-dev dev app plan <app_id> <resource>`.
- [ ] Inspect an existing manifest with
  `zclassic23-dev dev app describe <app_id>`.
- [ ] Name any unmapped file or incomplete proof relationship as a blocker; do
  not guess a smaller proof set.

### 4. Edit and verify

- [ ] Ensure the watcher is verify-only:
  `zclassic23-dev dev loop ensure --input='{"root":".","mode":"verify"}'`.
- [ ] Edit only the owned files.
- [ ] Read the bounded verdict with `zclassic23-dev dev loop status`,
  `zclassic23-dev dev status`, or `zclassic23-dev dev loop wait`.
- [ ] On failure, use `zclassic23-dev dev diagnose latest`; retrieve raw output
  only when the bounded capsule is insufficient.
- [ ] Run the mapped focused test groups, `make build-only`, and `make lint` in
  proportion to the change. Use the canonical test runner, never `test_zcl`
  directly.
- [ ] Run deterministic App scenarios with
  `zclassic23-dev dev app simulate <app_id> --seed=<seed>`.
- [ ] Record exact seed, source identity, proof groups, and receipt.
- [ ] Do not call apply, publish, deploy-dev, or resident hot-swap paths; they
  are contained and are not completion evidence.

### 5. Diagnose runtime behavior

- [ ] Start with `zclassic23 status` or `zclassic23-dev ops health`.
- [ ] Use `zclassic23-dev ops diagnose` for the bounded causal rollup.
- [ ] Use `zclassic23-dev ops state <subsystem>` for one subsystem.
- [ ] Use `zclassic23-dev ops timeline` before raw logs.
- [ ] Use `zclassic23-dev ops logs --pattern='<bounded regex>'` only after the
  state/timeline result identifies a drilldown.
- [ ] If shell/process archaeology was necessary, record it as a missing native
  telemetry requirement in the relevant checklist workstream.

### Source-of-truth routing

| Question | First source |
|---|---|
| What is live and protected? | `zclassic23 status` + `docs/HANDOFF.md` |
| What should execute next? | `docs/work/FORWARD_PLAN.md` |
| Why is the code shaped this way? | `docs/FRAMEWORK.md` |
| How is a feature slice built? | `docs/AGENT_ARCHITECTURE.md` |
| What commands exist now? | `discover search/describe` |
| Where is a symbol and who uses it? | `code sym/refs/room` |
| Which proof is required? | `dev test plan` + `code tests` |
| What can an App request? | `sdk/include/zclassic23/app.h` |
| What does an App declare? | `dev app describe` + `apps/<id>/app.def` |
| What apparent bug may be intentional? | `docs/AGENT_TRAPS.md` |

Detailed contracts live in
[`../NATIVE_COMMAND_INTERFACE.md`](../NATIVE_COMMAND_INTERFACE.md),
[`../AGENT_ARCHITECTURE.md`](../AGENT_ARCHITECTURE.md), and the public
[`../../sdk/include/zclassic23/app.h`](../../sdk/include/zclassic23/app.h).
The daily operating loop is
[`../../.claude/skills/zclassic23-dev/SKILL.md`](../../.claude/skills/zclassic23-dev/SKILL.md).

## Target compact protocol — extend, do not fork

Prefer extending the current leaves with common digests, cursors, field
selection, and correlation selectors. Add a new leaf only when the existing
owner cannot express the operation honestly.

| Existing leaf | Target responsibility | Default target budget |
|---|---|---:|
| `dev.loop.ensure/status` | Workspace/session identity, source epoch, registry/index/context digests, latest receipt | 2 KiB |
| `code.room` | Definition, callers/callees, routes, commands, schemas, invariants, owners, tests, confidence gaps | 4 KiB |
| `dev.change.plan` | Complete ordered proof DAG, cost estimate, reusable receipts, coverage gaps | 2 KiB |
| `dev.loop.wait` | One green/red proof receipt for a newer immutable epoch | 1 KiB green / 2 KiB red |
| `dev.diagnose.show` | Normalized failure by durable ID, replay command, bounded artifact cursor | 2 KiB |
| `ops.diagnose` | Causal capsule selected by request/trace/job/app/session/match/height ID | 4 KiB |
| `ops.timeline` | Typed transition deltas with resumable sequence cursor | 8 KiB page |
| `app.inspect` | Manifest, grants, generation, health, resource/session/match state and replay references | 4 KiB |

`dev.diagnose.show` is a target extension and is still `planned`; use the
bounded `dev.diagnose.latest` compatibility read until the durable-ID handler
exists. The other rows are extension targets, not claims that all target fields
already exist.

Every compact response must include, when applicable:

- [ ] Stable schema and digest.
- [ ] Source epoch, artifact/generation identity, lane, and freshness.
- [ ] Summary without duplicated prose copies of structured fields.
- [ ] Explicit completeness/truncation and a resumable cursor.
- [ ] Stable failure or blocker code.
- [ ] Artifact references for full evidence.
- [ ] One executable primary next action and at most two alternatives.

## Dependency order

```text
A. Measure token economics
        |
B. Unify source epochs, proof DAGs, and receipts
        +-------------------+
        |                   |
C. Semantic context     D. Typed causal telemetry
        +-------------------+
                |
E. Manifest compiler + generic AppSim
                |
F. Sandboxed App worker and capability broker
                |
G. Deterministic Game SDK and reference Apps
                |
H. Signed source-first packaging and discovery
                |
I. Permanent quality and budget ratchets
```

No workstream may waive an earlier applicable gate.

## A. Measure LLM development economics

**Depends on:** Phase-0 containment only.

**Produces:** a `zcl.quality_evidence.v1` profile named
`llm_dev_benchmark`, an immutable baseline, and reviewed budgets.

- [ ] Define a representative task corpus covering:
  - command discovery and a read-only handler change;
  - resource/model/service/route creation;
  - compiler failure and focused-test failure;
  - reducer blocker diagnosis and long-job inspection;
  - P2P topic evolution;
  - deterministic game-rule change;
  - App state migration and rollback;
  - runtime App failure replay.
- [ ] Record per task: command count, request/response bytes, artifact bytes,
  source files/bytes read, cache hits, time to first correct edit, save-to-verdict
  latency, selected/executed proofs, retries, and final correctness.
- [ ] Instrument canonical native dispatch so native command path, result code,
  elapsed time, requested/returned bytes, view, budget, truncation, and trace ID
  are measured after zero-MCP.
- [ ] Capture the current baseline without changing behavior.
- [ ] Separate measured bytes from approximate tokenizer-specific token counts.
- [ ] Set budgets for menus, context rooms, plans, green receipts, red receipts,
  and artifact retrieval.
- [ ] Add injected-failure tests proving budgets do not hide required evidence.
- [ ] Add a regression gate only after the baseline and variance are known.

**Exit gate:** the same task corpus can be rerun against an exact source epoch,
and every token/latency reduction claim names the immutable before/after
evidence without weakening the required proof set.

## B. Unify source epochs, proof planning, and receipts

**Depends on:** A.

**Produces:** `zcl.dev_source_epoch.v1`, `zcl.dev_plan.v2`,
`zcl.dev_proof_receipt.v1`, and one worktree-scoped append-only receipt store.

- [ ] Replace ambient dirty-tree identity with an immutable intended source
  epoch containing all modified, deleted, mode-changed, symlink, generated, and
  non-ignored untracked inputs.
- [ ] Scope watcher, locks, socket, receipt history, generation IDs, artifacts,
  and caches by workspace identity.
- [ ] Make one C-owned impact engine select the complete proof DAG for native
  commands, watcher, CI, and code navigation.
- [ ] Remove independent shell reinterpretation after parity tests prove the C
  planner produces the full set of required groups.
- [ ] Use exact test-group IDs; reject substring selection.
- [ ] Normalize compiler, linker, lint, test, sanitizer, and simulation results
  into typed records.
- [ ] Store passing raw logs out of band; return counts/digests/durations only.
- [ ] Assign every red receipt a durable `failure_id` and populate the command
  result envelope's existing failure field.
- [ ] Implement `dev.diagnose.show <failure_id>` with fields/view/cursor and an
  exact epoch/group/seed replay command.
- [ ] Make `dev.diagnose.latest` return the failure ID and summary rather than
  recursively nesting the same cycle.
- [ ] Bind reusable green receipts to source, toolchain, flags, test binary,
  proof set, seed/corpus, relevant environment, and artifact digests.
- [ ] Make a newer edit supersede older work and publish zero stale receipts.
- [ ] Prove concurrent worktrees cannot overwrite each other's latest receipt.
- [ ] Prove every unmapped code change fails closed as `coverage_gap`.

**Exit gate:** an exact source epoch produces one bounded green receipt or one
durable red failure ID; the same receipt can be independently replayed, and
100% of changed code has a complete proof set or explicit coverage gap.

## C. Build semantic, digest-addressed context

**Depends on:** B.

**Produces:** one daemon-owned semantic index snapshot and an expanded,
budget-aware `code.room` response.

- [ ] Give one persistent process ownership of code-index rebuilds; concurrent
  readers use the last complete immutable snapshot.
- [ ] Lock or single-flight rebuilds and prove parallel read queries cannot
  produce zero-file snapshots, SQLite I/O errors, or temp-rename races.
- [ ] Use content digests for freshness; do not trust only path/mtime/size.
- [ ] Index `.c`, `.h`, `.def`, App manifests, migrations, SQL schema ownership,
  Make/build rules, route tables, command declarations, wire schemas, and
  selected documentation metadata.
- [ ] Add relationships:
  - command -> handler -> owner -> tests;
  - route -> controller -> service -> model -> table/index;
  - dumper -> state producer -> owner -> drilldown;
  - event -> emitters -> consumers -> telemetry fields;
  - manifest -> capability -> route/topic/resource/scenario;
  - file/symbol -> callers/callees/includes/reverse-includes/proofs.
- [ ] Preserve handler symbol names in generated registries so joins do not rely
  on opaque function pointers.
- [ ] Return relationship confidence and explicit coverage gaps.
- [ ] Add digest-aware delta responses; unchanged context is not re-emitted.
- [ ] Paginate large groups/files/refs instead of returning
  `RESPONSE_BUDGET_EXCEEDED` when a smaller page is possible.
- [ ] Remove duplicate `card`/`lines` prose in compact JSON when structured data
  already carries the same information.
- [ ] Link tested invariants and traps to their enforcing gates rather than
  preloading whole manuals.

**Exit gate:** at least 90% of corpus tasks reach the first correct edit without
a whole-file read, while the room reports any missing relationship instead of
inventing confidence.

## D. Build typed, causal, privacy-safe telemetry

**Depends on:** B. May proceed in parallel with C.

**Produces:** a non-consensus telemetry ledger, generated event descriptors,
correlated runtime/dev evidence, and compact diagnostic projections.

- [ ] Preserve three storage tiers:
  1. bounded no-allocation per-thread flight recorder;
  2. durable bounded transition/failure/job ledger;
  3. content-addressed full logs, traces, dumps, and replay artifacts.
- [ ] Generate typed event IDs, payload structs, JSON schemas, severity,
  ownership, privacy, units, and cardinality from one descriptor source.
- [ ] Retain free-form `event_emitf` only as measured compatibility debt; new
  platform events use typed payloads.
- [ ] Carry applicable correlation fields: boot, workspace/lane, source epoch,
  generation, request/trace/span, job, app/app-generation, session/match,
  subsystem/operation, height, stable code, monotonic sequence/time.
- [ ] Propagate trace/correlation through native dispatch, HTTP, P2P framing,
  reducer/job boundaries, model transactions, capability IPC, and App callbacks.
- [ ] Automatically measure every App boundary even if the App emits no custom
  telemetry: calls, status, latency, queue depth, CPU/memory/state/network quota,
  worker liveness, restart, migration, and generation.
- [ ] Register every long operation as a job with heartbeat, progress marker,
  target, recent/overall rate, ETA, warnings, source/generation, and artifacts.
- [ ] Make the protected mint producer representable by one job-status response
  without `ps`/systemd/journal/log-file correlation.
- [ ] Make metrics queryable by select/top/diff and generation, not only a large
  rendered Prometheus blob.
- [ ] Add `since`/`diff` state queries that emit only cursor, blocker, health, or
  rate-regime transitions.
- [ ] Classify problem events by stable typed codes, not substring heuristics.
- [ ] Enforce privacy at schema registration and render time. Secrets, wallet
  keys, tokens, message plaintext, and private bodies never enter LLM artifacts.
- [ ] Classify every field as `public`, `operator`, `private`, or
  `forbidden-secret`; redact or reject before insertion into a ring, ledger,
  log, artifact, crash dump, or command response.
- [ ] Treat IPs, onion addresses, local paths, principals, and
  wallet-attributed transaction IDs as private by default.
- [ ] Prevent untrusted Apps from choosing metric names, unbounded labels,
  filesystem paths, or log formats.
- [ ] Seed unique secret canaries and scan every telemetry tier and rendered
  response as a promotion test.
- [ ] Mark truncation, dropped events, ring overwrite, and retention gaps
  explicitly with sequence cursors and counters.
- [ ] Define bounded retention/GC; mutable `latest` is a pointer, never evidence.
- [ ] Make telemetry backpressure drop evidence with an explicit gap instead of
  blocking reducer or consensus progress.
- [ ] Capture the bounded pre-failure flight recorder into crash/failure
  capsules.
- [ ] Benchmark event, trace, persistence, and query overhead before setting a
  hard CPU/memory SLO; then add non-regression gates.
- [ ] Fault-inject ENOSPC, slow drain, corrupt tail, queue/ring overrun, and
  crash/reopen; prove consensus progress remains independent.

**Exit gate:** one `ops.diagnose` query by request, job, App/session/match, or
height ID returns a deterministic causal chain from observed state to the last
transition, blocker/invariant, owning source symbol, proof receipt, artifact,
and executable next action. No secret or silent gap is present.

## E. Compile the complete App surface from one manifest

**Depends on:** B, C, and D.

**Produces:** strict manifest compiler, immutable App catalog, host-owned App
storage, and generic AppSim. Runtime publication remains contained.

- [ ] Replace the ad hoc Social-only parser/discovery with a strict deterministic
  manifest compiler; reject unknown or duplicate directives.
- [ ] Keep package discovery bounded and declarative; it must not execute the C
  preprocessor, constructors, compiler, or App code.
- [ ] Extend the manifest to declare:
  - identity, version, license, publisher, ABI/SDK range, source/content digest;
  - execution trust class, requested capabilities, local grants, and quotas;
  - resources, fields, relationships, indexes, validations, migrations;
  - commands, effects, risk, authority, idempotency, schemas, and budgets;
  - REST/onion/websocket routes, authentication, privacy, freshness, limits;
  - P2P topics, signing/encryption/replay/reliability/rate/size policy;
  - typed events, projections, jobs, assets, ZNAM bindings;
  - required simulations, properties, fuzz targets, and seed domains.
- [ ] Generate ordinary inspectable C for SDK types, validators, AR descriptors,
  migrations, command specs, routes/OpenAPI, topic tables, telemetry, tests,
  impact mappings, docs, and `code.room` edges.
- [ ] Make `dev app scaffold <app> <resource>` generate one conventional,
  compilable slice: model/header + ActiveRecord callbacks/validators/
  relationships, migration/indexes, service, thin REST controller, pure view,
  HTTPS/onion mount, manifest entries, focused tests, simulation seed, impact
  mapping, and Markdown guide.
- [ ] Make scaffold output deterministic, formatted, and transactional: preview
  first; reject existing/dirty targets; write a temporary tree; compile/test;
  then atomically install or leave no partial files.
- [ ] Generate one transport-neutral route function and bind HTTPS and onion to
  it. Authentication, response limits, cache/freshness, and failure behavior
  must be identical on both transports.
- [ ] Build a dynamic immutable App-catalog snapshot beside the immutable Core
  catalog; do not hand-maintain separate command/REST/P2P lists.
- [ ] Separate requested capabilities from operator-granted capabilities.
- [ ] Evolve the ABI through size/version-tagged, IPC-friendly numeric operation
  IDs with generated typed wrappers; avoid open-ended string operations on hot
  or authority-bearing paths.
- [ ] Separate portable package identity from exact host generation/build/proof
  identity.
- [ ] Add host-owned transactional App storage without exposing SQL:
  begin/commit/abort, snapshots, read/write/delete, bounded scan, CAS/version,
  quotas, event append, projection cursor, outbox/inbox, job, and asset handles.
- [ ] Make replicated App state event-derived and projections rebuildable;
  separate local preferences/secrets and ephemeral session state.
- [ ] Include served height/hash, freshness/finality, and reorg cursor in every
  chain-read capability result.
- [ ] Build generic AppSim that invokes the exact production ABI/dispatch path
  with virtual clock, seeded randomness, isolated storage, and queued topics.
- [ ] Cover partition, delay, loss, duplicate, reorder, crash/restart, late join,
  migration, capability denial, quota exhaustion, malformed input, and reorg.
- [ ] Return transcript/state fingerprints, generation/manifest digests, seed,
  bounded last events/packets, and exact replay command.
- [ ] Generate Social from its manifest rather than hardcoding its discovery and
  plan.
- [ ] Materialize Tic-Tac-Toe as the first Game App using the same compiler and
  AppSim host.
- [ ] Keep scaffold/inspect/publish marked `planned` until real handlers and
  proofs exist; menus never overstate availability.

**Exit gate:** a new resource and a new deterministic game rule can be declared,
generated, compiled, inspected, and simulated from one manifest with no manual
catalog edits and no runtime loading.

## F. Run third-party C23 Apps outside the node

**Depends on:** E and the Phase-3 immutable publication transaction.

**Produces:** same-binary App worker, minimal capability broker, immutable
generations, blue/green cutover, and exact rollback.

- [ ] Define two execution classes:
  - audited built-ins, statically linked and reviewed;
  - third-party/fetched Apps, never mapped or executed by the node process.
- [ ] Self-exec the same ZClassic23 binary in an `--app-worker` mode with a fresh
  address space.
- [ ] Enter rootless namespaces and apply Landlock, seccomp, `no_new_privs`, W^X,
  rlimits/cgroups, bounded restart budgets, and closed inherited file
  descriptors before loading App code.
- [ ] Give the worker no node database, wallet/key file, arbitrary filesystem,
  peer socket, network namespace, service manager, or process-control access.
- [ ] Use framed bounded binary capability IPC. Use shared-memory rings/eventfd
  only for profiled high-rate paths and retain explicit backpressure.
- [ ] Enforce manifest grants, principals, deadlines, cancellation, idempotency,
  rate/byte/state/CPU/memory quotas, and audit at the parent broker.
- [ ] Inspect App artifacts before execution: constructors/imports, undefined
  symbols, dependencies, W^X, ABI/SDK identity, manifest/content digest, tests,
  and proof receipts.
- [ ] Load a candidate worker, self-test, shadow migrate/replay, behaviorally
  probe, quiesce, atomically switch routing, drain old leases, then durably
  accept or restore exact last-good.
- [ ] Prove a worker crash, timeout, quota breach, malformed reply, migration
  failure, and kill during every boundary cannot crash or mutate the parent.
- [ ] Add malicious fixtures attempting key/file reads, socket creation,
  process escape, consensus mutation, syscall escalation, and resource
  exhaustion.
- [ ] Keep canonical/soak/release dynamic loading at zero; development worker
  activation remains isolated and owner-gated until promotion.

**Exit gate:** two nodes run the same sandboxed App, converge after partition,
survive worker crash/upgrade/rollback, reject invalid authority, and leave H*,
consensus storage, canonical datadir, and wallet keys unchanged.

## G. Make games deterministic Apps

**Depends on:** E; live multiplayer additionally depends on F.

**Produces:** Game SDK, generic App-topic envelope, durable sessions, replay,
and reference games.

- [ ] Replace the fixed `uint8_t` Ping/Tic-Tac-Toe registry as the extension
  boundary; retain compatibility adapters while peers migrate.
- [ ] Define a generic App-topic envelope with app/protocol/topic/wire version,
  session, sender/principal, sequence/ack, logical tick, payload length/hash,
  signature, replay window, reliability/encryption policy, and budgets.
- [ ] Negotiate and dispatch topics in the host; Apps never receive peer
  sockets.
- [ ] Define the deterministic game contract:
  `init`, `validate_input`, `reduce`, `hash`, `serialize`, `deserialize`, and
  viewer-scoped `render_view`.
- [ ] Use fixed-width integers, canonical endianness and encoding, explicit
  bounds, and no serialized struct padding.
- [ ] Ban host randomness, wall clock, locale, pointer identity, filesystem,
  networking, undefined behavior, and floating-point dependence from reducers.
- [ ] Keep wall clock, entropy, networking, storage, and wallet outside pure
  game reduction; inject logical time and seeded randomness as facts.
- [ ] Add sessions, participants, identities, matchmaking, spectators,
  reconnect/resync, snapshots, state-hash chains, and durable signed input logs.
- [ ] Make every match reproducible from App generation, manifest/rules digest,
  seed, and signed ordered input log.
- [ ] Simulate latency, loss, duplication, reorder, partition, malicious input,
  censorship, late join, crash, migration, and divergent state hash.
- [ ] Add optional settlement through wallet plan/commit/idempotent intents and
  chain-confirmation events; Apps never receive keys.
- [ ] Port Tic-Tac-Toe as the golden reference Game App and prove parity with
  the compatibility wire/API behavior.
- [ ] Add a second non-trivial reference game before freezing the Game SDK.
- [ ] Keep the deterministic core portable enough to compile for a browser WASM
  client later; do not add a WASM runtime to the node without a measured need.

**Exit gate:** two nodes complete a full match through partition/reconnect,
produce the same final state/transcript hash from the replay, reject forged or
replayed inputs, and recover from worker upgrade/rollback without changing
consensus state.

## H. Package, discover, build, upgrade, and roll back Apps

**Depends on:** F and G for executable games.

**Produces:** `zcl.artifact_manifest.v1`, `.zclpkg`,
`zcl.package_manifest.v1`, package commands, and reproducible source-first
distribution.

### H1. P2P source and content plane (`content.v2`)

- [ ] Reuse the proven low-level chunk I/O, SHA3, backpressure, and resume
  mechanics from `lib/net/src/file_service.c`; do not reuse its state-specific
  50 MiB manifest, borrowed snapshot trust claims, or payment gate as the App
  package protocol.
- [ ] Store source as canonical 1 MiB SHA3-256 chunks addressed only by hash.
  A bounded Merkle manifest maps normalized relative path + mode + size to an
  ordered chunk list; peers never submit or retrieve host filesystem paths.
- [ ] Bind the source-root, package manifest, tests/scenarios, dependencies,
  license/SBOM, build recipe, requested capabilities, publisher key, ZNAM name,
  and proof-receipt digest into one signed package identity.
- [ ] Define bounded `inventory`, `want`, `chunk`, `cancel`, and
  proof-of-possession messages with protocol/app version, request ID, offsets,
  length/hash, deadlines, quotas, replay protection, and Noise/Tor privacy.
- [ ] Fetch chunks concurrently from multiple peers, verify before CAS commit,
  resume by bitmap, penalize corrupt/slow sources, and make one censoring peer
  irrelevant. Partial downloads never become discoverable packages.
- [ ] Serve the same read-only manifest/chunk resource through P2P, HTTPS, and
  onion adapters. Open-source packages cost zero; optional premium settlement
  is a policy layered above content identity, never a read-integrity gate.
- [ ] Generate typed native leaves: `app package build/sign/publish/verify`,
  `app content offer/find/fetch/status/pin/unpin`, and bounded source inspection.
  No package is built, installed, or executed merely because it was advertised
  or downloaded.
- [ ] Keep publisher signature, local reproducible-build receipt, sandbox/ELF
  inspection, operator capability grant, and optional chain anchor as separate
  facts in the UI and API.
- [ ] Prove: normalized-tree determinism; traversal/symlink/duplicate rejection;
  corrupt chunk/manifest/signature rejection; partition/resume; opposite peer
  order; alternate-relay censorship bypass; quota/ENOSPC cleanup; two-builder
  byte identity; and no wallet/canonical-datadir/consensus mutation.

- [ ] Package manifest, C23 source, generated code recipe, assets, tests,
  simulations, license, content-only dependency lock, SPDX/CycloneDX SBOM,
  publisher identity/signature, and proof receipts.
- [ ] Use canonical normalized paths, modes, sizes, versioned SHA3 content
  identity, ABI/SDK range, capabilities/quotas, rollback floor, and reproducible
  recipe. Define an explicit migration for any existing `content_sha256`
  identity rather than leaving two ambiguous package identities.
- [ ] Reject traversal, symlink escape, duplicate normalized path, oversized
  member, unpinned network dependency, publisher substitution, downgrade,
  signature failure, license omission, and capability escalation.
- [ ] Build offline in a compiler sandbox from two clean roots/builders and
  require byte-identical output before stable publication.
- [ ] Require local reproducible build before first execution in v1.
- [ ] Generate `dev app.*`, `app package.*`, and `app content.*` native leaves
  from the same registry; legacy transports do not retain duplicate handlers.
- [ ] Advertise package ID/version/content hash/publisher through ZNAM and the
  file/content network without implying trust or triggering automatic install.
- [ ] Keep open-source package bytes fetchable without payment; premium
  settlement is optional policy.
- [ ] Require explicit approval when publisher, protocol, capability grant, or
  rollback floor changes.
- [ ] Prove author -> sign -> publish -> discover -> two-node fetch -> offline
  rebuild -> sandbox activate -> use -> upgrade -> rollback end to end.
- [ ] Prove none of those operations mutates canonical chain state, wallet keys,
  or the serving datadir outside the host-owned App stores.

**Exit gate:** the full Phase-5 promotion scenario in the sovereign roadmap
passes with independent build/signature evidence and exact rollback.

## I. Permanent quality, performance, and token ratchets

**Depends on:** all applicable earlier workstreams.

- [ ] Add CI size gates for root/branch/spec/status/context/receipt/error/list
  responses and fail when compact mode duplicates fields as prose.
- [ ] Require usable input field schemas: types, required/optional, enum, bounds,
  defaults, units, relationships, privacy, and examples.
- [ ] Require every command, route, event, state field, capability, job, and App
  resource to have an owner, schema, privacy class, cost, test, and drilldown.
- [ ] Require every code change to map to a complete proof DAG or fail as
  `coverage_gap`.
- [ ] Require at least 95% of injected failures to produce an exact phase,
  source/test/assertion or runtime owner, durable failure ID, artifact, and
  deterministic replay.
- [ ] Require at least 90% of corpus tasks to reach first edit without whole-file
  reads.
- [ ] Require warm edit-to-green model-visible output <=1 KiB and one post-save
  call; red failure-to-action <=2 KiB and at most two calls.
- [ ] Require task-specific onboarding/context <=4 KiB; unchanged digests return
  deltas rather than repeated context.
- [ ] Preserve the sovereign roadmap Phase-3 latency targets and measure them on
  the reference machine; do not infer activation latency from verify-only runs.
- [ ] Add telemetry CPU, memory, allocation, lock, SQLite, I/O, and storage
  budgets only after a real baseline; prevent regression thereafter.
- [ ] Require no allocation on the hot event-emission path and bounded telemetry
  cardinality/retention.
- [ ] Require identical App generation + seed + inputs to produce identical
  transcript/state hashes across supported builders/platforms.
- [ ] Require the malicious App campaign and at least 10,000 deterministic
  network seeds before stable runtime/package promotion.
- [ ] Require two-builder identity, offline verify/install/upgrade/rollback,
  fuzz/sanitizer/coverage, and independent review before stable publication.
- [ ] Delete compatibility paths only after their replacement and rollback are
  proven; mutable `latest` is never authoritative evidence.

## Recommended first implementation slice

This slice improves current cure and App development without loading Apps,
changing consensus, restarting a node, or touching the protected producer.

- [ ] Capture the LLM task-corpus baseline under `zcl.quality_evidence.v1`.
- [ ] Instrument canonical native dispatch with response bytes, budget,
  truncation, latency, result code, request ID, and trace ID.
- [ ] Select one worktree-scoped append-only receipt store and migrate both
  native and shell-cycle readers toward it.
- [ ] Populate durable `failure_id` and implement useful
  `dev.diagnose.show`/non-recursive `latest` responses.
- [ ] Apply summary/normal/full projection universally so a small requested
  budget reduces data instead of causing an avoidable overflow error.
- [ ] Make code-index rebuild single-flight/concurrent-reader-safe and add
  explicit pagination.
- [ ] Index command/App `.def` files and preserve handler symbol names.
- [ ] Make one C proof-DAG engine accumulate every required group and prove
  parity against the existing runners.
- [ ] Build a read-only typed job projection over the protected producer's
  existing evidence without changing its process, binary, command line, files,
  or lifecycle. Use the same job contract natively for new long operations.
- [ ] Re-run the task corpus and publish exact before/after receipts.
- [ ] Stop there for independent review. Do not add an App loader or runtime
  publication authority in this slice.

## Promotion summary

| Promotion | Required checklist state |
|---|---|
| Low-token verify loop | A–C complete; B receipt gate green |
| Correlated node/App diagnosis | D complete with privacy and gap proofs |
| Offline App development platform | E complete; runtime still contained |
| Public third-party App runtime | Phase 3 transaction + F complete |
| Public deterministic game platform | F + G complete |
| Sovereign package/content network | H complete plus Phase-2 release evidence |
| Permanent stable platform | I complete and applicable Phase-6 deletion gates green |

The governing rule is unchanged: reduce tokens through deterministic routing,
server-side projection, content-addressed reuse, typed causal evidence, and
generated structure—never by hiding uncertainty or weakening proof.
