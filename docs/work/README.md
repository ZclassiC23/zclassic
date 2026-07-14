# Work directory and parallel-worktree workflow

This directory contains active plans, designs, dated evidence, and the
parallel-worktree protocol. It is not itself a priority queue:

1. [`../HANDOFF.md`](../HANDOFF.md) owns current live facts.
2. [`FORWARD_PLAN.md`](./FORWARD_PLAN.md) is the sole ordered execution plan.
3. [`../MVP.md`](../MVP.md) owns the v1 acceptance contract.

The active #1 track is the sovereign complete-state cure. Zero-MCP is the
secondary development track. Architecture cleanup remains off the v1 path
unless the owner explicitly promotes an item.

Worktrees are dynamic; never infer current workers from a hard-coded path list.
Inspect them with `git worktree list --porcelain`. The checkout at
`~/github/zclassic23` is normally the orchestrator. Every other registered
checkout is a worker or an isolated quality lane and must be inspected before
removal; dirty worktrees are preserved.

## How a worker session starts

Run `pwd` and `git worktree list --porcelain`, then follow
[`agent-protocol.md`](./agent-protocol.md). The assignment owns the branch,
scope, verification, and completion ritual; directory suffixes are labels, not
a permanent server inventory.

## Active control documents

- **Current work:** [`FORWARD_PLAN.md`](./FORWARD_PLAN.md) (THE plan),
  [`self-verified-tip-plan.md`](./self-verified-tip-plan.md),
  [`CONSENSUS-STATE-BUNDLE.md`](./CONSENSUS-STATE-BUNDLE.md), and
  [`sovereign-cutover-runbook.md`](./sovereign-cutover-runbook.md).
- **Secondary track:** [`MCP-REMOVAL-WORKLIST.md`](./MCP-REMOVAL-WORKLIST.md)
  (the authoritative zero-MCP inventory).
- **Current architecture:** [`../FRAMEWORK.md`](../FRAMEWORK.md) (reference,
  off the v1 path — see `../REFACTOR_STATUS.md` for the debt board, which
  self-labels NOT the v1 path).
- **Session entrypoint:** [`../HANDOFF.md`](../HANDOFF.md).
- **Worker protocol:** this file plus [`agent-protocol.md`](./agent-protocol.md).

## Worker protocol

Each assignment lives at `docs/work/wt<N>-<slug>.md` and contains:

- **Branch name** — exact name to create
- **Scope** — files this assignment owns; files it must NOT touch
- **Dependencies** — other assignments that must complete first
- **Tasks** — ordered, testable steps
- **Acceptance criteria** — concrete tests that prove done
- **Commit + push instructions** — exact git commands
- **Completion ritual** — what to append at the end

## Conflict avoidance

- **Disjoint file scope**: each assignment lists exact files it owns; no other assignment may touch those files until it merges.
- **No concurrent edits to** `REFACTOR_STATUS.md`: only orchestrator writes it. Workers append to their own assignment doc.
- **Integrate deliberately**: follow the current operator skill and assignment
  instructions; do not assume every dirty checkout can safely rebase or push.

## Failure modes

- **Worker discovers assignment is wrong or impossible** → worker appends a `BLOCKED` section to its assignment doc with details, pushes, reports to user. Orchestrator session must respond.
- **Worker's tests fail** → worker does NOT merge; pushes a `WIP` branch + appends `FAILED` section with the failing test output.
- **Two workers touch overlapping files (should not happen)** → second-to-merge rebases, orchestrator session resolves.
