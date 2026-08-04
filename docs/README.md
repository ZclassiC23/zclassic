# docs/ — documentation map

Curated entry map, not an exhaustive file inventory. Dated evidence and backlog
material are intentionally omitted.

There are two audiences here and they need opposite starting points. Pick yours.

## New here? Start with these three

You want to build it, run it, and change something. In order:

1. [`GETTING_STARTED.md`](./GETTING_STARTED.md) — the fresh-machine path:
   prerequisites, build, run a node, run the block explorer, set up an isolated
   development instance.
2. [`../.github/CONTRIBUTING.md`](../.github/CONTRIBUTING.md) — the contribution
   contract: the dev loop, which walls are hard (the sealed consensus core), what
   the git hooks do, and how a PR is judged.
3. [`HOW_THE_NODE_WORKS.md`](./HOW_THE_NODE_WORKS.md) — the one-page mental
   model: append-only log → reducer stages → projections → health.

Then [`DEVELOPING.md`](./DEVELOPING.md) — the developer operating manual,
described under "Develop here" below. Read it before making changes.

Then [`MVP.md`](./MVP.md) for what "v1" means and an honest readiness account,
and [`AGENT_TRAPS.md`](./AGENT_TRAPS.md) before you "fix" anything that looks
broken — several things that look broken are intentional or already done.

**`HANDOFF.md` is live state for the maintainer's own hosted node.** Run
`zclassic23 status` — if it answers, you are on that node and `HANDOFF.md` is
your first read (that is what `CLAUDE.md`'s fresh-session ritual assumes). If
it does not, skip it: it will tell you nothing about your machine.

## Maintainer entry points (the project's own hosted node)

These describe one specific running node. They are the right starting point if
you operate that host, and misleading otherwise.

- [`HANDOFF.md`](./HANDOFF.md) — the single live-state page: current height,
  blockers, in-flight cure. Read FIRST on a fresh maintainer session; nothing
  else in the tree is allowed to carry a live height claim. Any victory phrase
  on this page must carry machine-checkable evidence — enforced by the
  `check-no-uncited-victory` gate ([`AI_SAFETY_GATES.md`](./AI_SAFETY_GATES.md)).
- [`work/FORWARD_PLAN.md`](./work/FORWARD_PLAN.md) — THE plan, MVP-anchored.
- [`RUNBOOK.md`](./RUNBOOK.md) — symptom-driven operator troubleshooting.
- [`RELEASE_CANDIDATE_PIN.md`](./RELEASE_CANDIDATE_PIN.md) — the candidate triple
  (commit / source id / artifact bytes) that names the build under proof, and the
  drift probe that checks the box still agrees.
- [`PROMOTION_RECEIPTS.md`](./PROMOTION_RECEIPTS.md) — the signed, hash-chained,
  tracked ledger of proof-server promotions: evidence that replicates off this
  machine, cannot be rewritten undetected, and verifies offline without a private
  key. Ships with **zero records and no signing key** — the evidence-signing
  identity is a one-time owner decision, documented there under "Owner setup".
  Nothing has been promoted through it, and inventing a receipt would be a
  fabricated record.
- [`agent/`](./agent/) — how the maintainer runs several AI executor agents at
  once, each in its own worktree on one checkout:
  [`LANE_CONTRACT.md`](./agent/LANE_CONTRACT.md) (the doctrine an executor
  follows), [`LANE_LAUNCH.md`](./agent/LANE_LAUNCH.md) (the dispatcher's
  checklist), [`LANE_REPORT.md`](./agent/LANE_REPORT.md) (what an executor owes
  back), [`EXAMPLE_LANE_PROMPT.md`](./agent/EXAMPLE_LANE_PROMPT.md) (a worked
  before/after). **This is one person's orchestration setup, not the project's
  development process** — contributing requires none of it, and a single
  developer on a single checkout will find most of its rules meaningless. The
  contributor path is the three docs at the top of this page.

## Develop here — the fast loop (read before making changes)

- [`DEVELOPING.md`](./DEVELOPING.md) — **the efficient-AI-C23-developer playbook**: the native dev loop (drop-in-C watcher), hot-swap tiers, typed-commands-over-bash, workflows of tiered subagents, the push traps (impact-rules mapping, pre-push SIGPIPE), and the inviolable rules. Also the body of the `zclassic23-dev` Claude Code skill, whose stub at [`.claude/skills/zclassic23-dev/SKILL.md`](../.claude/skills/zclassic23-dev/SKILL.md) imports it. Start here for any change.
- [`NATIVE_COMMAND_INTERFACE.md`](./NATIVE_COMMAND_INTERFACE.md) — the native command registry (`core.*`/`app.*`/`ops.*`/`dev.*`/`discover.*`), the only agent interface going forward: grammar, tree, and the frozen CLI UX contract.
- [`TRANSACTION_API.md`](./TRANSACTION_API.md) — the semantic transaction
  catalog and AI-safe workflow: base ZCL, Sapling, ZSLP, ZNAM, ZMSG,
  ZID/ZDIR/ZANC, ZCODE and ZBLG anchors, HTLC swaps, and commerce, with a
  one-call guide to exact live command contracts and an explicit line between
  ready, receive-only, contained, and planned paths.
- [`FILE_MARKET_PROTOCOL.md`](./FILE_MARKET_PROTOCOL.md) — the authenticated
  paid-offer wire, exact integer pricing, confirmed-payment authority, complete
  buyer retrieval workflow, and reproducible proof map for `market_purchase`.
- [`API_REFERENCE.md`](./API_REFERENCE.md) — every leaf the registry currently declares. **Generated** from `config/commands/` by `tools/gen_api_reference.c`; edit `API_REFERENCE.md.in` (prose) or the `.def` catalog, then `make docs-api-reference`.
- [`SERVICES.md`](./SERVICES.md) — the declared way to add a service (`zcl.service_binding.v1`): its own commands, its own state namespace, and access derived from a ZSLP token balance at a stated snapshot height — plus the isolation boundary a service cannot declare its way out of. Contract in `lib/kernel/include/kernel/service_binding.h`, catalog in `config/services/bindings.def`.
- [`AGENT_API.md`](./AGENT_API.md) — the practical field-by-field reference for the implemented agent surface (`agentops`, `agentdiagnose`, `healthcheck`, `agentlanes`, service catalog, and more); referenced directly from `app/controllers/src/agent_controller.c` response bodies.
- [`work/HOTSWAP.md`](./work/HOTSWAP.md) — Tier-1 hot-swap (`config/hotswap_eligible.def`) + the dev loop + ZVCS auto-anchor.
- [`ZVCS.md`](./ZVCS.md) — in-binary VCS; `dev vcs revert` is a one-command
  source-tree revert. Relinking the *running binary* to the reverted
  generation is not wired: `relink_generation=true` refuses **before** the
  source revert, with status `BLOCKED` and error
  `RUNTIME_PUBLICATION_CONTAINED` — not `VCS_ENOTIMPL`, which is a
  `lib/vcs` error enum this path never returns,
  and the canonical/release binary refuses `dev.*` entirely
  <!-- claim: symbol-present RUNTIME_PUBLICATION_CONTAINED tools/command/native_dev_command.c # the real refusal code -->
  <!-- claim: symbol-absent VCS_ENOTIMPL tools/command # the revert handler does not use it -->
  (`DEV_BUILD_REQUIRED`) — live rollback of a running/canonical node is
  `make deploy` of a prior commit, not a ZVCS relink.

## Mental model (read before touching code)

- [`HOW_THE_NODE_WORKS.md`](./HOW_THE_NODE_WORKS.md) — the node as a state machine: append-only log → reducer stages → projections → health.
- [`ROM.md`](./ROM.md) — the L0-L3 trust machine: what the compiled checkpoint commits, what each layer guarantees, how to read `dumpstate rom`.
- [`CODEBASE_MAP.md`](./CODEBASE_MAP.md) — where things live + how to do each thing.
- [`EXTENSION_POINTS.md`](./EXTENSION_POINTS.md) — the three surfaces under active construction (vault ownership, big integers, the declarative service manifest): where each lives and how to extend it. Every claim on that page is gate-bound, so it fails the build rather than going stale.
- [`AGENT_TRAPS.md`](./AGENT_TRAPS.md) — things that look broken but are intentional or already-done; read before "fixing" or re-proposing anything.
- [`AGENT_ARCHITECTURE.md`](./AGENT_ARCHITECTURE.md) — the required feature-slice recipe: REST resources, ActiveRecord models, validations, relationships, schema, services, and native command surfaces.

## Active plans

- [`work/self-verified-tip-plan.md`](./work/self-verified-tip-plan.md) — active
  #1 sovereign transparent + shielded state cure.
- [`work/CONSENSUS-STATE-BUNDLE.md`](./work/CONSENSUS-STATE-BUNDLE.md) — atomic
  complete-state artifact contract.
- [`work/sovereign-cutover-runbook.md`](./work/sovereign-cutover-runbook.md) —
  owner-gated copy-proof and cutover sequence.

## Dated review evidence

The full-tree code/tooling/docs reduction audit of 2026-07-14 is **not in the
tree** — neither the file nor `docs/work/archive/` exists. Recover it from
history: <!-- doc-path-ok: recovered from git history, not present on disk -->
`git log --follow --diff-filter=D -- 'docs/work/archive/CODEBASE-CONSOLIDATION-REVIEW-*'`.

## Architecture (reference, off the v1 path)

- [`FRAMEWORK.md`](./FRAMEWORK.md) — canonical architecture: Prime Directive, Ten Laws of Beauty, the eight shapes, and (§9) the architecture debt board (live debt only; NOT the v1 path).
- [`ARCHITECTURE_DIAGRAMS.md`](./ARCHITECTURE_DIAGRAMS.md) — Mermaid diagrams of the current boot sequence and subsystem topology.
- [`adr/0001-personal-sovereignty-stack.md`](./adr/0001-personal-sovereignty-stack.md) — ADR for the 2026-05-22 Personal Sovereignty Stack pivot.
- [`adr/0007-property-settlement-and-trust-roots.md`](./adr/0007-property-settlement-and-trust-roots.md) — ADR for what backs a metaverse property claim: content addressing, proof of work, or local declaration — and the limits of each.

## Operations

- [`RUNBOOK.md`](./RUNBOOK.md) — symptom-driven operator troubleshooting.
- [`TENACITY.md`](./TENACITY.md) — recovery invariants (copy-prove, never live surgery).
- [`SYNC.md`](./SYNC.md) — how a fresh node reaches chain tip; legacy bootstrap paths.
- [`work/fast-path.md`](./work/fast-path.md) — the diagnosis algorithm: repro on a datadir COPY, never live.
- [`work/README.md`](./work/README.md) — parallel-worktree workflow (orchestrator + wt2/wt3 workers).
- [`work/agent-protocol.md`](./work/agent-protocol.md) — worker startup/completion contract.
- [`CHAOS_HARNESS.md`](./CHAOS_HARNESS.md) — the two chaos-testing tools and their make targets.
- [`TELEMETRY_CONTRACT.md`](./TELEMETRY_CONTRACT.md) — the stable shape of the `ops.telemetry` tree: typed snapshots, derived health, and why an unknown value is never silently omitted.

## Measurement

- [`BENCHMARKS_LOG.md`](./BENCHMARKS_LOG.md) — append-only ledger of measured benchmark values (never estimates).
- [`USER_BENCHMARKS.md`](./USER_BENCHMARKS.md) — the five user-facing numbers every decision is judged against.
- [`SIMNET_PERF.md`](./SIMNET_PERF.md) — `make sim-perf`: a CI-cheap, machine-independent detector for algorithmic-complexity regressions on the block-connect/UTXO path, with the injected regression that proves it discriminates. Not a wall-clock sync measurement.

## Specs & validation

- [`SECURITY_AND_INTEGRITY.md`](./SECURITY_AND_INTEGRITY.md) — operator safety boundary, security model, integrity controls, and reviewer checklist.
- [`CUSTODY_MODEL.md`](./CUSTODY_MODEL.md) — what holds the keys, what authorizes a spend, what an agent grant bounds, and what it does not.
- [`spec/power-node-contract.md`](./spec/power-node-contract.md) — stable architecture/observability contract for a power node.
- [`spec/sovereign-identity-layer.md`](./spec/sovereign-identity-layer.md) — design draft: chain-anchored master keys ("write once, sign forever") — onion descriptors via swarm, ZNAM naming, ZDIR relay directory as an application.
- [`validation/VALIDATION_MATRIX.md`](./validation/VALIDATION_MATRIX.md) — validation coverage matrix.

## Contributor docs

- [`../CLAUDE.md`](../CLAUDE.md) — project instructions for AI agents; native command setup; build/run quick reference.
- [`DEFENSIVE_CODING.md`](./DEFENSIVE_CODING.md) — mandatory coding standards, enforced by `make lint`.
- [`AI_SAFETY_GATES.md`](./AI_SAFETY_GATES.md) — the gates that make an AI agent's claims checkable: no uncited victory, no dishonest witness, no hand-pinned rot-prone fact, no undispatched test, and a copy-prove harness with no destructive parameter to pass.
- [`ATTRIBUTIONS.md`](./ATTRIBUTIONS.md) — concept/code attributions (companion to the root `NOTICE`).
- [`BOOT_INVARIANTS.md`](./BOOT_INVARIANTS.md) — boot stage ordering invariants (`enum boot_stage`).
- [`LEGACY_LIFECYCLE.md`](./LEGACY_LIFECYCLE.md) — which `legacy_*` paths are active vs deprecated.
- [`../README.md`](../README.md) — public-facing project README.
