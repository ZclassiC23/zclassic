# docs/ — documentation map

Curated entry map, not an exhaustive file inventory. Start with the entry
points in order; dated evidence and backlog material are intentionally omitted.

## Entry points (read in this order)

- [`HANDOFF.md`](./HANDOFF.md) — current state; read this FIRST on a fresh session.
- [`MVP.md`](./MVP.md) — the v1 contract: 8 binary acceptance criteria (v1 = MRS 8/8).
- [`work/FORWARD_PLAN.md`](./work/FORWARD_PLAN.md) — THE plan, MVP-anchored.

## Develop here — the fast loop (read before making changes)

- [`.claude/skills/zclassic23-dev/SKILL.md`](../.claude/skills/zclassic23-dev/SKILL.md) — **the efficient-AI-C23-developer playbook**: the native dev loop (drop-in-C watcher, `dev change apply`), hot-swap tiers, typed-commands-over-bash, workflows of tiered subagents, the push traps (impact-rules mapping, pre-push SIGPIPE), and the inviolable rules. Auto-loads as the `zclassic23-dev` skill; start here for any change.
- [`NATIVE_COMMAND_INTERFACE.md`](./NATIVE_COMMAND_INTERFACE.md) — the native command registry (`core.*`/`app.*`/`ops.*`/`dev.*`/`discover.*`), the only agent interface going forward.
- [`work/HOTSWAP.md`](./work/HOTSWAP.md) — Tier-1 hot-swap (`config/hotswap_eligible.def`) + the dev loop + ZVCS auto-anchor.
- [`ZVCS.md`](./ZVCS.md) — in-binary VCS; `dev vcs revert` is a one-command
  source-tree revert. Relinking the *running binary* to the reverted
  generation is not wired (`relink_generation=true` returns `VCS_ENOTIMPL`),
  and the canonical/release binary refuses `dev.*` entirely
  (`DEV_BUILD_REQUIRED`) — live rollback of a running/canonical node is
  `make deploy` of a prior commit, not a ZVCS relink.

## Mental model (read before touching code)

- [`HOW_THE_NODE_WORKS.md`](./HOW_THE_NODE_WORKS.md) — the node as a state machine: append-only log → reducer stages → projections → health.
- [`ROM.md`](./ROM.md) — the L0-L3 trust machine: what the compiled checkpoint commits, what each layer guarantees, how to read `dumpstate rom`.
- [`CODEBASE_MAP.md`](./CODEBASE_MAP.md) — where things live + how to do each thing.
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

- `work/archive/CODEBASE-CONSOLIDATION-REVIEW-2026-07-14.md` — full-tree code,
  tooling, documentation, and developer-experience reduction audit; evidence,
  not a competing plan. Removed from the tree; recover with
  `git log --follow -- docs/work/archive/CODEBASE-CONSOLIDATION-REVIEW-2026-07-14.md`.

## Architecture (reference, off the v1 path)

- [`FRAMEWORK.md`](./FRAMEWORK.md) — canonical architecture: Prime Directive, Ten Laws of Beauty, the eight shapes, and (§9) the architecture debt board (live debt only; NOT the v1 path).
- [`ARCHITECTURE_DIAGRAMS.md`](./ARCHITECTURE_DIAGRAMS.md) — Mermaid diagrams of the current boot sequence and subsystem topology.
- [`adr/0001-personal-sovereignty-stack.md`](./adr/0001-personal-sovereignty-stack.md) — ADR for the 2026-05-22 Personal Sovereignty Stack pivot.

## Operations

- [`RUNBOOK.md`](./RUNBOOK.md) — symptom-driven operator troubleshooting.
- [`TENACITY.md`](./TENACITY.md) — recovery invariants (copy-prove, never live surgery).
- [`SYNC.md`](./SYNC.md) — how a fresh node reaches chain tip; legacy bootstrap paths.
- [`work/fast-path.md`](./work/fast-path.md) — the diagnosis algorithm: repro on a datadir COPY, never live.
- [`work/README.md`](./work/README.md) — parallel-worktree workflow (orchestrator + wt2/wt3 workers).
- [`work/agent-protocol.md`](./work/agent-protocol.md) — worker startup/completion contract.
- [`CHAOS_HARNESS.md`](./CHAOS_HARNESS.md) — the two chaos-testing tools and their make targets.

## Measurement

- [`BENCHMARKS_LOG.md`](./BENCHMARKS_LOG.md) — append-only ledger of measured benchmark values (never estimates).
- [`USER_BENCHMARKS.md`](./USER_BENCHMARKS.md) — the five user-facing numbers every decision is judged against.

## Specs & validation

- [`SECURITY_AND_INTEGRITY.md`](./SECURITY_AND_INTEGRITY.md) — operator safety boundary, security model, integrity controls, and reviewer checklist.
- [`spec/power-node-contract.md`](./spec/power-node-contract.md) — stable architecture/observability contract for a power node.
- [`validation/VALIDATION_MATRIX.md`](./validation/VALIDATION_MATRIX.md) — validation coverage matrix.

## Contributor docs

- [`../CLAUDE.md`](../CLAUDE.md) — project instructions for AI agents; native command setup; build/run quick reference.
- [`DEFENSIVE_CODING.md`](./DEFENSIVE_CODING.md) — mandatory coding standards, enforced by `make lint`.
- [`ATTRIBUTIONS.md`](./ATTRIBUTIONS.md) — concept/code attributions (companion to the root `NOTICE`).
- [`BOOT_INVARIANTS.md`](./BOOT_INVARIANTS.md) — boot stage ordering invariants (`enum boot_stage`).
- [`LEGACY_LIFECYCLE.md`](./LEGACY_LIFECYCLE.md) — which `legacy_*` paths are active vs deprecated.
- [`../README.md`](../README.md) — public-facing project README.
