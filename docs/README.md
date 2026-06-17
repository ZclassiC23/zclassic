# docs/ — documentation map

One line per doc. Start with the entry points, in order.

## Entry points (read in this order)

- [`HANDOFF.md`](./HANDOFF.md) — current state; read this FIRST on a fresh session.
- [`MVP.md`](./MVP.md) — the v1 contract: 8 binary acceptance criteria (v1 = MRS 8/8).
- [`work/FORWARD_PLAN.md`](./work/FORWARD_PLAN.md) — THE plan, MVP-anchored.

## Architecture (reference, off the v1 path)

- [`FRAMEWORK.md`](./FRAMEWORK.md) — canonical architecture: Prime Directive, Ten Laws of Beauty, the eight shapes.
- [`ARCHITECTURE_DIAGRAMS.md`](./ARCHITECTURE_DIAGRAMS.md) — Mermaid diagrams of the current boot sequence and subsystem topology.
- [`adr/0001-personal-sovereignty-stack.md`](./adr/0001-personal-sovereignty-stack.md) — ADR for the 2026-05-22 Personal Sovereignty Stack pivot.
- [`REFACTOR_STATUS.md`](./REFACTOR_STATUS.md) — architecture debt board (live debt only; NOT the v1 path).

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

- [`../CLAUDE.md`](../CLAUDE.md) — project instructions for AI agents; MCP setup; build/run quick reference.
- [`DEFENSIVE_CODING.md`](./DEFENSIVE_CODING.md) — mandatory coding standards, enforced by `make lint`.
- [`ATTRIBUTIONS.md`](./ATTRIBUTIONS.md) — concept/code attributions (companion to the root `NOTICE`).
- [`BOOT_INVARIANTS.md`](./BOOT_INVARIANTS.md) — boot stage ordering invariants (`enum boot_stage`).
- [`LEGACY_LIFECYCLE.md`](./LEGACY_LIFECYCLE.md) — which `legacy_*` paths are active vs deprecated.
- [`../README.md`](../README.md) — public-facing project README.
