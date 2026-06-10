---
description: Coordinator drive-loop — sync, preserve worker WIP, manage agents, deploy if green, report
---

You are the coordinator for **zclassic23**. Goal: node STICKY + STRONG + ROBUST, live tip in sync with zclassicd. Rhett is coordinator-only; Agent-2 (`~/zclassic23-2`) and Agent-3 (`~/zclassic23-3`) write the code. This command is re-invocable; each turn does ONE cycle.

Use parallel tool calls where independent. Never run destructive git in worker worktrees without preserving WIP first.

## 1. Sync

Parallel:
- `git -C /home/rhett/zclassic23 fetch origin && git -C /home/rhett/zclassic23 pull --ff-only`
- `git -C /home/rhett/zclassic23-2 fetch origin && git -C /home/rhett/zclassic23-2 log --oneline origin/main..HEAD` (unpushed Agent-2 commits)
- `git -C /home/rhett/zclassic23-3 fetch origin && git -C /home/rhett/zclassic23-3 log --oneline origin/main..HEAD` (unpushed Agent-3 commits)
- `git -C /home/rhett/zclassic23-2 status --porcelain` (WIP state)
- `git -C /home/rhett/zclassic23-3 status --porcelain` (WIP state)
- `ls -la ~/.codex/logs_?.sqlite` (Codex activity timestamps)

## 2. Preserve at-risk WIP

For each worker worktree with untracked files or unpushed commits:
- If new untracked `lib/test/src/test_*.c` or tracked edits sit ≥30 min old AND Codex idle → at risk of kickoff-reset. Push to `wip/agent-<N>-<row>-red` side branch from that worktree (see past pattern in commits 7140999a8, 84595fa6a).
- If unpushed commits exist and tests pass → rebase onto origin/main and push to main on agent's behalf (see P24.11 preservation commits ab1e88a1b + ffad7cf7d).
- NEVER force-push main. NEVER `git reset --hard` in a worker worktree without stashing first.

## 3. Live node state (MCP)

Parallel:
- `mcp__zcl23__zcl_status` — height, header_height, peer count, sync state
- `mcp__zcl23__zcl_events` — recent events (failed flushes, peer bans, reorgs)
- `zclassic-cli -rpcport=8232 getblockcount` — legacy zclassicd height (for gap)

Note: the live node may be stuck (P24.18 stall at h=3,078,014 until Agent-2 lands the fix). Report the gap; don't try to manually bootstrap unless explicitly asked.

## 4. If agents landed green commits — canary + deploy

If `git log` shows a worker commit matching `P\d+[a-z]?:?\s*(GREEN|done|landed)`:
1. `make -j$(nproc) test_zcl` in `/home/rhett/zclassic23`
2. `build/bin/test_zcl 2>&1 | grep -E "FAIL|ALL TESTS|SOME TESTS"` — verify green
3. If touches live-node paths (lib/validation, app/services, app/controllers): `make deploy`
4. Arm a Monitor (`until curl -sf ... 18232`) to await RPC-up; the 30s deploy_verify timeout is known-bad (fixed by P24.19).
5. After RPC up: canary tools affected by the row (e.g. P24.14 → `zcl_listunspent`, `zcl_walletaudit`). Confirm no SIGABRT in `node.log`.
6. Update AGENT.md rollup (CRITICAL/HIGH counts) + owner state.

## 5. Rotate NOW on landing + push

When a worker row lands:
- Update AGENT-<N>.md `## Current status — NOW = ...` line to next queued row from the wave.
- Update AGENT.md owner state with the landing + next NOW.
- Commit: `agents: P<row> landed <sha> — rotate Agent-<N> NOW to P<next>`
- Push.

Once P25 lands, this step becomes `zcl_coord_rotate_now(agent, closed_row)` — no more manual rotation commits.

## 6. Signal agents (only when value > noise)

Touch AGENT-<N>.md only if:
- They lost WIP (add RESUME HINT block pointing at the side branch you just pushed).
- They've been idle >30 min on a simple row (push a shorter, more concrete ACTION LIST).
- A new CRITICAL bug blocks their current row (file a new row in AGENT.md, update their queue).

Do NOT:
- Edit AGENT-<N>.md while they're mid-flight (merge conflicts).
- Re-file rows they're already working on.

## 7. Coordinator-lane work (only if all agents fed)

If both agents have live in-flight work AND you have spare capacity, file new rows or extend the wave:
- Diagnostic MCP tools (P24.25-pattern)
- Observability gaps (P24.20-pattern)
- Structural hardening (P24.22/P24.24-pattern)

Never land coordinator code in the stall-fix path — that hides bugs from the systemic fix.

## 8. Report (≤5 lines)

- **Live gap:** zclassic23 at h=X, zclassicd at h=Y, **gap Z blocks**.
- **Workers:** A2 `<status/NOW>`, A3 `<status/NOW>`.
- **Landed this cycle:** `<commits>` / or `none`.
- **Deployed?** yes / no / pending.
- **Next check-in verifies:** `<specific measurable thing — e.g. "Agent-2 pushes P24.18 GREEN"`.

## 9. (Optional) Self-schedule

If user said "keep working" or "autonomous" → `ScheduleWakeup` at 900-1800s passing the `/drive` prompt back. Skip if user is pair-driving.

## Worktree layout

- `/home/rhett/zclassic23` — coordinator, never worker commits (except my rebased rescue pushes).
- `/home/rhett/zclassic23-2` — Agent-2 (wallet / storage / app-layer / net / validation).
- `/home/rhett/zclassic23-3` — Agent-3 (crypto / sapling / consensus-crypto / tests).

## Available MCP tools

- **Node-ops (live):** `zcl_status`, `zcl_events`, `zcl_peers`, `zcl_rpc`, `zcl_logtail`, `zcl_syncstate`, `zcl_validationstatus`, `zcl_kpi`, `zcl_dataintegrity`, `zcl_self_test`, plus 50+ more via `zcl_tools_list`.
- **Coordination (P25 — not built yet):** `zcl_coord_dashboard`, `zcl_coord_agent_now`, `zcl_coord_mail_*`, `zcl_coord_gate_check`, `zcl_coord_rotate_now`. Until P25 lands, coordination goes through AGENT.md + git.

## Memory pointers

- `memory/feedback_pulse_command.md` — similar coordinator sweep (older pattern)
- `memory/feedback_no_hotfix_repro_first.md` — no fix without RED test first
- `memory/feedback_never_destroy_wallet.md` — critical safety rule
- `memory/project_mcp_tools.md` — MCP tool inventory
