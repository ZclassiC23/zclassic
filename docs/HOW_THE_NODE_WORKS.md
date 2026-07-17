# How the node works

The codebase looks big. The idea underneath it is small. Read this page once and
you can reason about the whole node.

## 1. The node in four lines

1. There is one durable record on disk: an append-only log of facts (in
   `progress.kv`, a SQLite store). It is the only authority for consensus state.
2. There is one kind of worker — a **reducer stage**. Each stage reads the height
   its upstream stage has finished, then either **advances its own cursor by one
   height** (writing one log row) **or names a typed blocker** saying exactly why
   it cannot. There are eight of these stages in a fixed line.
3. Everything else — the wallet, the block explorer, the peer list, the
   block-index view — is a **projection**: a read-only summary rebuilt by re-reading
   the log. Projections never decide anything; delete one and it rebuilds.
4. Health is **one number**: `network_tip − log_head`. `network_tip` is the best
   block height the network has told us about; `log_head` is the highest height the
   eighth stage (`tip_finalize`) has finalized. If that number is shrinking, the
   node is making progress. If it is stuck, some stage has named a blocker — there
   is no silent stop.

That is the entire mental model. The eight stages are below.

## 2. The state machine — eight stages

Each stage stores a **cursor** in `progress.kv`. For the first seven stages the
cursor is "the next height to process", not "the highest done"; `tip_finalize` is
the one exception — its cursor is the served tip itself (the highest finalized
height), which the frontier code normalizes to the same "next height" frame when
comparing stages (`reducer_frontier.c:149` `frontier_next_cursor` — served tip C is
treated as next-height C+1). A stage may
only run at heights its upstream stage has already finished (the upstream cursor is
its floor). It does one of two
things at that height: advance the cursor by one and write one authoritative log
row keyed by height, or stop and name a blocker. Cursor + log row are written in
the same database transaction, so a crash resumes cleanly at the stored cursor.

| # | Stage | What it proves | Cursor at height N means | What "stuck" looks like |
|---|-------|----------------|--------------------------|-------------------------|
| 1 | `header_admit` | A block-index entry exists for the height and is linked to its parent | Heights `[0,N-1]` admitted; N is next | Blocker `missing_parent` — the previous block's linkage is absent |
| 2 | `validate_headers` | Proof-of-work + Equihash are valid (or solution is missing but back-fillable) | Headers `[0,N-1]` checked, each logged ok/fail | Idle, parked on a repairable row (e.g. missing solution); a terminal reject moves the floor on |
| 3 | `body_fetch` | The block body is present on disk | Bodies `[0,N-1]` seen on disk or skipped as invalid | Idle until the body arrives; blocker `header_solution_missing` if the header was never validated |
| 4 | `body_persist` | The body reads back, hashes to its header, and rebuilds its merkle root | Bodies `[0,N-1]` verified readable + merkle-consistent | Idle — it clears the body and re-fetches on a read/hash/merkle failure |
| 5 | `script_validate` | Every input script verifies | Scripts `[0,N-1]` checked (ok, script-invalid, or internal error) | Idle until upstream is ready or the Sapling params are loaded |
| 6 | `proof_validate` | Shielded proofs verify (Groth16 / PHGR13 / Sapling / binding sig) | Proofs `[0,N-1]` checked (ok or rejected) | Idle until upstream is ready or Sapling params are loaded |
| 7 | `utxo_apply` | The coin changes (added/spent, transparent + shielded, nullifiers) are consensus-consistent | A verdict row exists for `[0,N-1]` | Blocker `apply_failed` (transient) — e.g. the upstream verdict row is missing |
| 8 | `tip_finalize` | Height N is the canonical finalized tip and the chain extends linearly into N+1 | The finalized tip is N | Idle at the frontier, or transient `successor_pending` if N+1 isn't body-ready / script-valid yet |

`log_head` from the four-line summary is the `tip_finalize` cursor — the maximum
finalized height. External readers (`getblockcount`, the height we advertise to
peers) report the height that `tip_finalize` has published. During process
startup, the public REST/native status surfaces may read the durable
`tip_finalize` cursor before the in-memory H* cache has been published, so the
website does not briefly fall back to height 0 while the node is already at tip.

A **reorg** is just a disconnect: `utxo_apply` saved the inverse of each coin
change, so the node replays those backward to the fork point, then re-applies the
winning branch forward. Nothing special, same machinery.

A blocker is **transient** (solvable — retry with backoff, then a bounded number of
attempts) or **permanent** (needs the operator, or a condition + a supervised
restart). Either way it is named.

## 3. Watch the machine live

These are typed operator calls. Prefer the native `zclassic23` command and stop
when you have your answer.

| Call | Shows |
|------|-------|
| `zclassic23 agentinterface` | Preferred AI/operator interface contract. Typed native CLI JSON is first, the legacy MCP `zcl_agent_interface` route remains during W2/W3 migration, and REST is read-only. No external wrapper logic is required. |
| `zclassic23 api` | Native API discovery from the running node. Same `zcl.rest_index.v1` body as `GET /api` and `GET /api/v1`: version, base path, resource routes, CRUD conventions, `layer_model` for the ZCL L1 / zclassic23 application-layer boundary, and first native/MCP/REST calls. Start here when choosing an interface. |
| `zclassic23 appprotocols` | Native application-protocol catalog. Same contract as MCP `zcl_app_protocols` and `GET /api/v1/protocols`: ZSLP, ZNAM, market, messaging, and script-contract overlay services, their CRUD/read models, anchors, and consensus boundary. |
| `zclassic23 agentlanes` | Native canonical/soak/dev topology and deployment-safety contract. Same contract as MCP `zcl_agent_lanes`; use it before choosing a deploy or restart target. |
| `zclassic23 agentliveness` | Compact lane/service/supervisor/background-quality liveness. Same contract as MCP `zcl_agent_liveness`; use it when deciding whether a lane is active, stalled, missing quality verdicts, or only being inspected from a static binary. Use `agentliveness full` only for embedded method/lane/domain arrays. |
| `zclassic23 status` | The native first check: one line by default, or a bounded `zcl.result.v1` / `zcl.core_status_brief.v1` document with H*, gap, peers, health, and the causal blocker. Use `zclassic23 core status` for the larger diagnostic tree. The legacy `zclassic23 agent` / `zcl_agent` / REST document is separate. |
| `zclassic23 milestone` | Node-computed ASCII and JSON progress to v1 MVP. Same contract as `GET /api/v1/milestone` and MCP `zcl_milestone`: live systems bar, strict MRS goals bar, partial-proof subgoals bar, and next blockers. |
| `zcl_status` | The full diagnostic tree: height, peers, sync state, reducer frontier, tip-finalize, condition engine (it stitches the `getpeerinfo` / `syncstate` / `healthcheck` RPCs with the `reducer_frontier` / `tip_finalize` / `condition_engine` dumps). Use after the summary names a drill-down. |
| `zcl_syncdiag` | Sync state, header-sync counters, watchdog (= condition-engine health), chain/header heights, peer max height, download stats. **It does NOT list the eight stage cursors** — use `reducer_frontier` for those. |
| `zcl_state subsystem=reducer_frontier` | The eight stage cursors, `H*` (deepest provably-consistent height — the tip `getblockcount` serves), and the success-checked log frontiers (the contiguous ok=1 prefix per log) |
| `zcl_state subsystem=blocker` | Active blockers with deadlines and escape actions |
| `zcl_state subsystem=condition_engine` | Self-heal engine: active vs cleared conditions |
| `zcl_state subsystem=service_state` | Operational mode: boot / restore / reconcile / degraded_serving / syncing / healthy / repairing |
| `zcl_state subsystem=chain_evidence` | Native chain evidence: tips, cursors, evidence flags, any contradiction reason |

`zcl_state` is a generic dispatcher — pass any of ~56 subsystem names. The eight
stage names work directly as subsystems too: `header_admit`, `validate_headers`,
`body_fetch`, `body_persist`, `script_validate`, `proof_validate`, `utxo_apply`,
`tip_finalize`. For drilling deeper: `zcl_node_log` (server-side regex tail of
node.log), `zcl_sql` (SELECT-only over node.db), `zcl_probe_zclassicd` (compare
our block-index against the local zclassicd at a random height).

The complete subsystem list is one array in code:
`app/controllers/src/diagnostics_registry.c` (`g_dumpers[]`). Adding a new
introspectable subsystem is one entry there plus one `*_dump_state_json` function —
no new tool, no MCP plumbing.

## 4. What is real vs what is being deleted

**Real (load-bearing, stays):**
- The append-only fact log + per-stage success rows (ok=1). `H*` = the longest
  contiguous ok=1 prefix from the anchor. This is what makes a silent halt
  impossible to represent.
- The eight-stage reducer pipeline (advance-cursor-or-name-blocker).
- Consensus validation: PoW (Equihash 200,9), script signatures, shielded proofs,
  ZIP-209.
- Reorg handling via the saved inverse coin changes.
- The eight code "shapes" (controller / service / model / job / supervisor /
  condition / event / storage-adapter). Seven live one-folder-each under `app/`
  (`controllers`, `services`, `models`, `jobs`, `supervisors`, `conditions`,
  `events`); the Storage Adapter shape lives in the top-level `adapters/` + `ports/`
  trees (`app/views/` holds explorer templates and is not one of the eight). Shape
  placement is lint-enforced; per `docs/FRAMEWORK.md` Model/Condition/Job and the
  Storage Adapter are real and enforced, Supervisor is partial, Controller/Service
  still carry legacy debt.

**Being deleted (transient, ≈3330 production lines per the plan):** today the coin
set can be seeded on boot from a near-tip snapshot minted by an external
`zclassicd`. Its payload SHA3 authenticates the file bytes and its anchor hash
must match a validated local header. That proves the selected chain location,
not the derivation of UTXO or shielded state: ZClassic headers commit none of
the UTXO, Sapling/Sprout frontier, or nullifier roots. The state is therefore
**borrowed**, not consensus-bound or re-derived from genesis. The plan
(`docs/work/self-verified-tip-plan.md`, and `docs/work/archive/sync-fix-plan-2026-06-21.md`
for the ordered steps) replaces it with a **self-verified UTXO anchor rebuild**:
the internal boot path is `-refold-from-anchor`
(`app/jobs/src/refold_progress.c`, `app/services/src/anchor_selfmint.c`), which
rebuilds the coin set from that compiled checkpoint forward, then deletes the
borrowed-seed path and the older recovery-import code that fed it (≈3330 LOC
production fully deletable, plus PRUNE-not-delete tear paths that keep their
crash-recovery slice). Canonical is currently wedged below tip because
its historical shielded anchors/nullifiers are incomplete; a complete atomic
state install and copy proof must precede live cutover. (Verify the live H\*
and whether the cure has shipped via `zcl_status` / `dumpstate
reducer_frontier`, `docs/HANDOFF.md`, and the live node — do not assume from
this page.)

## 5. START HERE — fresh agent

1. `pwd` — confirm which worktree you are in (`main`, `wt2`, `wt3`).
2. Read, in order: **`docs/HANDOFF.md`** (current live state, what is fixed, what
   is in flight) → **`docs/MVP.md`** (the v1 acceptance bar) →
   **`docs/work/FORWARD_PLAN.md`** (the plan). `docs/FRAMEWORK.md` is the canonical
   architecture; this page is its plain-language summary. **`docs/AGENT_TRAPS.md`**
   lists things that look broken but are not (don't re-chase them);
   **`docs/CODEBASE_MAP.md`** is where-things-live + how-to-do-each-thing.
3. Look at the live node before trusting any doc: start with
   `zclassic23 agentmap` or `zcl_agent_map` for the code/docs/test map,
   `zclassic23 agentlanes` or `zcl_agent_lanes` for canonical/soak/dev safety,
   `zclassic23 agentliveness` or `zcl_agent_liveness` for the current lane's
   listener/supervisor/quality rollup,
   `zclassic23 agentbuild` or `zcl_agent_build` for the cached build loop,
   `zclassic23 api` for interface discovery, and `zclassic23 appprotocols` or
   `zcl_app_protocols` for the ZCL L1 / zclassic23 application-layer catalog.
   Then use native `zclassic23 status` for compact live state; the larger
   compatibility document remains at `zclassic23 agent` / `zcl_agent`. Use `zclassic23 milestone` or
   `zcl_milestone` for v1 progress bars. Drill down with `zcl_status` and
   `zcl_state subsystem=reducer_frontier` only if needed. A doc can be stale;
   the node cannot.
4. To understand one stage, open its file — `app/jobs/src/<stage>_stage.c`. Each is
   one `step_*` function that does exactly the advance-or-name-a-blocker contract
   described in section 2.

One log, one kind of worker, one health number. Everything else is a view over the
log or a stage in the line.
