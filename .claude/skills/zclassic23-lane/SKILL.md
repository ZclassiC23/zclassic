---
name: zclassic23-lane
description: Use when launching, dispatching, or reviewing an executor lane in the ZClassic23 repo — spinning up a worktree for a subagent, writing a lane prompt, pinning a baseline, deciding which gates to demand, splitting file ownership across parallel lanes, or judging a lane's report before merging. Covers the lane-launch checklist, the versioned executor doctrine (LANE_CONTRACT), and the required report shape (LANE_REPORT). Invoke for "start a lane", "dispatch a subagent on this", "set up a worktree for X", "what gates should this lane run", "how do I split this across lanes", or "is this lane ready to merge".
---

This skill covers the **maintainer's** multi-agent setup: several AI executors
running at once, each in its own git worktree on one checkout. It is not the
project's development process and contributing requires none of it — for a
single developer on a single checkout, use the `zclassic23-dev` skill instead.

The lane-launch checklist is [`docs/agent/LANE_LAUNCH.md`](../../../docs/agent/LANE_LAUNCH.md).
It lives in `docs/` so that every agent — and every documentation-accuracy lint
gate — sees it, not only Claude Code. This file holds the skill frontmatter and
nothing else — do not copy checklist content here; edit `docs/agent/LANE_LAUNCH.md`.

Two tracked companions the checklist points at, both versioned:
[`docs/agent/LANE_CONTRACT.md`](../../../docs/agent/LANE_CONTRACT.md) (the doctrine a
lane prompt references instead of pasting) and
[`docs/agent/LANE_REPORT.md`](../../../docs/agent/LANE_REPORT.md) (the report the
executor owes back).

The import below inlines the checklist. If it did not expand, read
`docs/agent/LANE_LAUNCH.md`.

@docs/agent/LANE_LAUNCH.md
