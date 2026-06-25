---
name: zclassic23-dev
description: Use when developing on the ZClassic23 codebase (this repo) — onboarding, understanding the architecture, or making any code change. Covers the node's state-machine model, the eight code shapes / where things live, the inviolable rules (consensus parity, copy-prove before live, defensive-coding gates), build/test/deploy, the parallel-worktree workflow, and the don't-re-chase traps. Invoke for "how does this codebase work", "how do I add or change X here", "be a zclassic23 developer", or before editing zclassic23 source.
version: 1.0.0
---

# Being a ZClassic23 developer

ZClassic23 is one ~15 MB C23 binary that is a full ZClassic node (Equihash 200,9 PoW,
Sapling shielded txs) plus wallet, explorer, embedded Tor, MCP server, and more. It
must stay **bit-for-bit consensus-compatible with `zclassicd`**.

The codebase looks big; the idea underneath is small. This skill is the compressed
operating manual. The **canonical, verified docs** are the source of truth — read them,
don't trust this page's specifics blindly (code moves; docs rot):

- `docs/HOW_THE_NODE_WORKS.md` — the one-page mental model (read FIRST if it feels complex).
- `docs/CODEBASE_MAP.md` — where things live + "I want to X → go here" + commands.
- `docs/AGENT_TRAPS.md` — things that look broken but are intentional/already-done. Read before "fixing."
- `docs/FRAMEWORK.md` — the Prime Directive, the Ten Laws, the eight shapes (the *why*).
- `docs/HANDOFF.md` — current live state (what's fixed, what's in flight). Read before acting.
- `docs/DEFENSIVE_CODING.md` — the mandatory coding gates.
- `docs/CONSENSUS_PARITY_DOCTRINE.md` — the inviolable parity rule.

## The model in four lines

1. One durable append-only log of facts on disk (`progress.kv`). It is the only consensus authority.
2. One kind of worker — a **reducer stage**. Each reads the height its upstream finished, then
   **advances its cursor by one (one log row) OR names a typed blocker**. Eight stages, fixed line:
   `header_admit → validate_headers → body_fetch → body_persist → script_validate → proof_validate → utxo_apply → tip_finalize`.
3. Everything else (wallet, explorer, peers, UTXO set) is a **projection** — a read-only view folded
   from the log, rebuildable, never authoritative.
4. Health is one number: `network_tip − log_head`. A stall is always a named blocker at a known
   height — a silent halt is unrepresentable. `getblockcount` serves `H*` (the provable tip).

## Where things live — the eight shapes

Every `.c` under `app/` is exactly one shape (lint-enforced). Open the folder, know the shape:
`controllers/` (parse→authorize→call one service), `services/` (orchestrate, return `zcl_result`),
`models/` (the only readers/writers of state; AR lifecycle), `jobs/` (the reducer stages,
cursor-stamped, advance-or-block), `supervisors/` (liveness trees), `conditions/`
(`{detect,remedy,witness}` healers), `events/` (reserved-empty), `views/` (explorer templates).
Pure consensus core: `domain/` (no clock/RNG/IO). Primitives: `lib/`. Hexagonal write seam:
`ports/` + `adapters/`. Boot: `config/src/`. Tooling/MCP/lint: `tools/`. Full map + "how to add a
model / healer / MCP tool / reducer stage / lint gate" is in `docs/CODEBASE_MAP.md`.

## The inviolable rules (violating these causes real damage)

1. **Consensus parity is absolute.** Never ship a consensus change (Equihash params, activation
   heights, block/tx validity) to zclassic23 first — not even opt-in. Enforced by `check-consensus-parity`
   (E13) + `test_consensus_parity`. **Validate against the real CHAIN, not the zclassicd source text**
   (the chain contains a 125,811-byte tx at h=478544 the text-copied cap would false-reject). Any
   tightening of a bounded predicate requires a full-history replay first.
2. **Copy-prove before live; never live surgery.** Copy the datadir, reproduce on the copy, prove the
   fix FIRES on the copy, then deploy. **Gate on H\* CLIMB**, not "booted without FATAL." `test_parallel`
   green is a regression floor, not a liveness proof.
3. **Every write goes through the AR lifecycle** (`AR_BEGIN_SAVE`/`AR_FINISH_SAVE` or `AR_ADHOC_SAVE`).
   Raw `sqlite3_step()` in app code is lint-rejected. **Every malloc** uses `zcl_malloc(size,"label")`.
   **Every error return logs context** (`LOG_FAIL`/`LOG_ERR`/`LOG_NULL`). **Every MCP handler sets an
   error body** — never a bare `return -1`. `make lint` enforces these.
4. **Less is more.** Prefer deleting/unifying over adding. A new abstraction is a last resort.
5. **Profile-first for performance.** No unmeasured perf claims. Don't optimize cold paths. Use
   `zcl_profile` / `zcl_benchmark` / the measured bottleneck docs.
6. **Status reporting is plain and technical** — exact height/table/function/file:line. No metaphor.

## Before you change anything

1. Detect your worktree: `pwd` (`main` = orchestrator; `~/github/zclassic23-2` = wt2; `~/github/zclassic23-3` = wt3).
2. Read `docs/HANDOFF.md` (live state) and skim `docs/AGENT_TRAPS.md` (don't re-chase a fixed thing or
   re-propose a shipped optimization or "fix" an intentional parity decision).
3. Check the live node before trusting any doc: `zcl_status`, then `zcl_state subsystem=reducer_frontier`.
   A doc can be stale; the node cannot.

## Build / test / deploy

- `make build-only` — fast parallel compile-check (inner loop).
- `make -j$(nproc)` — full build (`zclassic23`, `test_zcl`, `zclassic-cli`).
- `make test` / `make test-parallel` — the canonical test runner. **Use this, not `test_zcl`.**
- `make lint` — all gates; must pass before tests. `make ci` — lint + build + tests + checks.
- `make deploy` (owner-gated, live) / `make deploy-dev` (dev node). `make deploy` rm's the stale binary
  first (a stale binary was a real multi-day outage) and verifies `build_commit`.
- Gate every change: `make` + `make lint` + `make test-parallel` (read the `N passed, M failed` line).

## The agent surface (MCP)

Start with `zcl_status` / `zcl_kpi`. Three primitives cover most diagnostics: `zcl_state(subsystem=...)`
(generic state dump — ~56 subsystems incl. the 8 stage names + `blocker`, `reducer_frontier`,
`condition_engine`, `service_state`), `zcl_node_log` (regex tail of node.log), `zcl_sql` (SELECT-only).
Escape hatch: `zcl_rpc(method,...)`. Discover all tools with `zcl_tools_list`. Add introspection by
registering one `*_dump_state_json` in `app/controllers/src/diagnostics_registry.c` — no new tool needed.
Note: `mcp__zcl23-dev__*` hit the DEV node; `mcp__zcl23-live__*` / curl 18232 hit LIVE — confirm the target.

## Parallel-worktree workflow

Main repo orchestrates; `wt2`/`wt3` are workers (`cp -a` vendor static libs from main first — fresh
worktrees can't link without the gitignored `vendor/lib/*.a`). When fanning out work across lanes, give
each lane a disjoint file set, prove each on its own datadir copy, and merge in dependency order. See
`docs/work/README.md` + `docs/work/agent-protocol.md`.

## The discipline that matters most

The node's whole reason to exist is one property: **derive every fact by folding a replayable log and
check it against its own cryptographic checkpoints — never serve an unproven value, never halt without
naming the exact block + reason.** Every change should make that more true (more self-derived, fewer
borrowed/cached authorities), or it's off-mission. The current work to finish it is
`docs/work/self-verified-tip-plan.md`.

**Verify fresh. The live code is the only authority; this skill and every doc it links can be stale.**
