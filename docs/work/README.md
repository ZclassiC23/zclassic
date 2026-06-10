# Parallel-Worktree Workflow

> Status note, 2026-06-01: this directory contains only the parallel-worktree
> protocol. The current architecture/status sources are `docs/FRAMEWORK.md`,
> `docs/REFACTOR_STATUS.md`, and `docs/HANDOFF.md`. Obsolete cutover/B8
> runbooks, stale design notes, and paused worker assignments were deleted; git
> history is their audit record.

The framework refactor runs in parallel across multiple working
directories ("worktrees" — actually separate clones). Each worktree picks
up an assignment, branches, commits, pushes, and reports completion. A
single **orchestrator** session (typically in the main repo at
`~/github/zclassic23`) merges and dispatches the next round.

## Worktree map

| Path | ID | Role |
|---|---|---|
| `~/github/zclassic23`   | `main` | Orchestrator: writes assignments, merges PRs, updates status board, dispatches next phase |
| `~/github/zclassic23-2` | `wt2`  | Worker |
| `~/github/zclassic23-3` | `wt3`  | Worker |

Worker identity is **derived from the worktree path**: agents check `pwd`
and extract the suffix after `zclassic23-` (e.g., `2` → wt2). If no
suffix, they are the orchestrator.

## How a worker session starts

Human runs in the worker's directory:

```bash
cd ~/github/zclassic23-2
claude   # or claude -p "continue zclassic23 development"
```

The agent's startup ritual (defined in
[`agent-protocol.md`](./agent-protocol.md)):

1. Auto-loaded: `CLAUDE.md`, `MEMORY.md`
2. Read: `docs/FRAMEWORK.md` (architecture)
3. Read: `docs/REFACTOR_STATUS.md` (current phase)
4. Detect worktree ID from `pwd`
5. Read: `docs/work/wt<N>-*.md` (this worker's assignment)
6. Pull latest from origin
7. Branch off main per assignment
8. Execute assignment
9. Commit + push + append completion summary

## Where to look

- **Current work:** [`../REFACTOR_STATUS.md`](../REFACTOR_STATUS.md).
- **Current architecture:** [`../FRAMEWORK.md`](../FRAMEWORK.md).
- **Session entrypoint:** [`../HANDOFF.md`](../HANDOFF.md).
- **Worker protocol:** this file plus [`agent-protocol.md`](./agent-protocol.md).
- **Archived records:** [`done/`](./done/) — point-in-time diagnoses and
  assessments whose work landed or was superseded; kept for the audit trail.

## How assignments are structured

Each assignment lives at `docs/work/wt<N>-<slug>.md` and contains:

- **Branch name** — exact name to create
- **Scope** — files this assignment owns; files it must NOT touch
- **Dependencies** — other assignments that must complete first
- **Tasks** — ordered, testable steps
- **Acceptance criteria** — concrete tests that prove done
- **Commit + push instructions** — exact git commands
- **Completion ritual** — what to append at the end

## How the orchestrator dispatches

When wt2 and wt3 finish their assignments and push branches, the
orchestrator:

1. Pulls the branches
2. Reviews + runs full test suite (`make test_parallel`)
3. Merges to main (fast-forward if possible, merge commit otherwise)
4. Pushes main to origin
5. Updates `REFACTOR_STATUS.md` (conformance count, recently completed, in flight cleared)
6. deletes the finished assignment doc (git history is the record)
7. Writes the next round of assignments
7. Pushes
8. Reports to user: "wt2/wt3 ready for next round"

## Conflict avoidance

- **Disjoint file scope**: each assignment lists exact files it owns; no other assignment may touch those files until it merges.
- **Disjoint branches**: each worker on its own branch, never on `main` directly.
- **No concurrent edits to** `REFACTOR_STATUS.md`: only orchestrator writes it. Workers append to their own assignment doc.
- **Pull-rebase before push**: workers always `git pull --rebase origin main` before push to keep history linear.

## Failure modes

- **Worker discovers assignment is wrong or impossible** → worker appends a `BLOCKED` section to its assignment doc with details, pushes, reports to user. Orchestrator session must respond.
- **Worker's tests fail** → worker does NOT merge; pushes a `WIP` branch + appends `FAILED` section with the failing test output.
- **Two workers touch overlapping files (should not happen)** → second-to-merge rebases, orchestrator session resolves.

## Why this works

- **One source of truth per task** — the assignment doc.
- **Single writer per file** — no merge hell.
- **All progress visible** — `REFACTOR_STATUS.md` is the dashboard.
- **Async, human-paced** — humans run workers when ready; orchestrator catches up.
- **Recoverable** — every step writes to git and pushes; a crashed session resumes from `git log`.
