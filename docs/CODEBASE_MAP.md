# CODEBASE_MAP.md — where things live + how to do each thing

Fast reference for a fresh agent. Plain and technical. For the *why* (laws,
shapes, doctrine) read `docs/FRAMEWORK.md`; for *current live state* read
`docs/HANDOFF.md`; for *coding rules* read `docs/DEFENSIVE_CODING.md`. For the
one-page mental model read `docs/HOW_THE_NODE_WORKS.md`.

File counts below are from `find <dir> -type f | wc -l` (verified
2026-06-24). They drift; re-run if exactness matters.

---

## 1. Where things live

### The eight app shapes — `app/` (637 files)

Every `.c` under `app/` lives in exactly one shape folder, lint-enforced
(`framework_shape_check.sh`). Filename suffix must match the shape
(`check_framework_filename_suffix.sh`).

| Shape | Path | Files | Role | Exemplar |
|-------|------|-------|------|----------|
| Controllers | `app/controllers/` | 142 | parse → authorize → call ONE service → return; no business logic, no raw storage | `app/controllers/src/diagnostics_registry.c` |
| Services | `app/services/` | 171 | orchestrate a workflow; return `zcl_result` (typed code + message) | `app/services/src/` |
| Models | `app/models/` | 71 | ActiveRecord rows; own all reads (Law 5); save via `AR_*_SAVE` | `app/models/src/block.c` + `include/models/block.h` |
| Jobs | `app/jobs/` | 94 | cursor-stamped idempotent stages; the 8 reducer stages live here; advance-or-block | `app/jobs/src/*_stage.c` |
| Supervisors | `app/supervisors/` | 13 | liveness tree; children with `last_tick_age_us`, `progress_marker`, `deadline` | `app/supervisors/src/staged_sync_supervisor.c` |
| Conditions | `app/conditions/` | 47 | (detect, remedy, witness) healers; poll/backoff/page-on-exhaustion | `app/conditions/src/block_failed_mask_at_tip.c` |
| Events | `app/events/` | 1 | reserved-empty by design; impl lives in `lib/event/` + `lib/storage/event_log.c` | `app/events/README.md` |
| Views | `app/views/` | 98 | read-only explorer templates; no persistence writes; served over HTTPS + onion | `app/views/src/explorer_dashboard_view.c` |

### Core consensus — `domain/` (44 files)

Pure modules under `domain/{consensus,encoding,wallet}`. **No clock, no RNG,
no IO** (lint: `check_no_raw_clock_outside_platform.sh`). Replayable from a
64-bit seed. Fronted by thin `lib/` wrappers; sealed by `test_domain_*`.
Never put IO here.

### Primitives — `lib/` (999 files, 33 subdirs)

Framework, `platform/` (the ONLY clock/RNG source: `time_compat.h`,
`random.h`), `storage/` (`event_log.c` + `*_projection.c`), `net`, `crypto`,
`validation`, `chain`, `consensus`, `keys`, `metrics`, `health`, JSON, kernel
utils. Boot stage enum: `lib/util/include/util/boot_phase.h`.

### Hexagonal seam — `ports/` + `adapters/` (39 files)

Outbound-only by design: 12 port interfaces in `ports/include/ports/*_port.h`
+ 13 sqlite/file write impls in `adapters/outbound/persistence/{src,include}/`
(26 files = 13 `.c` + 13 `.h`). Reads are owned by Models (Law 5), so inbound
repository ports are reserved-empty.

<!-- DOC-COUNTS-BEGIN -->
<!-- Canonical code-derived counts (machine-checked by tools/scripts/check_doc_counts.sh). -->
<!-- Update BOTH this block AND any prose that cites these numbers when the code moves. -->
<!--   test_groups          = parallel test groups in lib/test/src/test_parallel.c   -->
<!--   port_interfaces      = ports/include/ports/*.h                                -->
<!--   persistence_adapters = adapters/outbound/persistence/src/*.c                  -->
test_groups: 488
port_interfaces: 12
persistence_adapters: 13
<!-- DOC-COUNTS-END -->

### Composition root — `config/src/` (26 files)

Boot orchestration. `boot.c` (main orchestrator) + fragments
(`boot_services.c` legacy lifecycle, `boot_refold_staged.c` staged consensus
job chain, plus `address_backfill`, `bg_workers`, `bg_verification`,
`block_file_scan`, etc.).

### Agent + dev tooling — `tools/`

`mcp/` (MCP server, controllers in `tools/mcp/controllers/`), `lint/` (gate
shell scripts), `fuzz/`, `soak/`, `sim/` (deterministic replay), `dev/`,
`githooks/`, `scripts/`, `data/` (fixtures).

---

## 2. "I want to X → go here"

### Add a model
1. Struct in `app/models/include/models/X.h`.
2. `DEFINE_MODEL_CALLBACKS` + `db_X_save`/`validate`/`find`/`delete` in
   `app/models/src/X.c`. Use `AR_*_SAVE` macros — raw `sqlite3_step()` is
   lint-rejected in app code (text-scan gate `check_raw_sqlite.sh`).
3. Add a migration in `app/models/src/database_migrate.c` (per-feature tables
   in `database_migrate_features.c`; schema `database_schema.c`; validators
   `database_validators.c`). Migrations auto-run at `BOOT_STAGE_DB_OPEN`; no
   rollback.
4. Wire before/after save hooks (HARD-enforced, E3).

### Add a healer (condition)
1. `app/conditions/src/name.c` with static `detect`/`remedy`/`witness` +
   `struct condition` (set `poll_secs`, `backoff_secs`, `max_attempts`).
2. `condition_register()` at module scope.
3. Forward-decl `void register_name()` and call it in
   `app/conditions/src/condition_registry.c`. Framework handles poll/backoff/
   witness/paging.

### Add an MCP tool
1. Static `int h_tool_name(req, res)` in the matching controller
   (`tools/mcp/controllers/{app,chain,diagnostics,meta,net,ops,wallet}_controller.c`).
   Must set an error body on failure (never bare `return -1`).
2. Add a route (`.name`, `.handler`, `.description`, `.schema`) to that
   controller's `k_routes[]`.
3. The controller's init loop registers all routes via
   `mcp_router_register()`. There is no central route registry file.

### Add a reducer stage (Job)
1. `app/jobs/src/STAGE_stage.c` with `stage_exec()` returning
   `ADVANCED`/`BLOCKED`/`IDLE`/`FATAL`.
2. Persist the cursor in `progress.kv` keyed by stage name (re-run at same
   cursor = no-op).
3. Wire into the pipeline in `config/src/boot_refold_staged.c` (or `boot.c`).
4. E5 gate (`check-typed-blocker`) enforces advance-or-block.

The 8 stages, in order: `header_admit → validate_headers → body_fetch →
body_persist → script_validate → proof_validate → utxo_apply → tip_finalize`.
The reducer is the **only** chain writer.

### Change a reducer stage
1. Locate `app/jobs/src/STAGE_stage.c`; edit the advance path or the
   `blocker_set()` path.
2. If touching validation rules, verify against `domain/consensus/`
   predicates.
3. Run `make lint` (`check-consensus-parity`) + the `test_reducer_*` suite
   (`lib/test/src/test_reducer_*.c`) before shipping.
4. Consensus parity is inviolable — never ship a consensus change to
   zclassic23 first.

### Add a lint gate
Add a shell script under `tools/lint/` (or a `check-*` recipe), then add it as
a dependency of the `lint` target in the `Makefile`. RATCHET gates compare
against a baseline file (e.g. `honest_witness_baseline.txt`,
`no_raw_sqlite_in_controllers_baseline.txt`). Note: many gates can fail-silent
on an empty scan set — use the zero-broadening preflight and verify the gate
actually fires.

---

## 3. The agent surface (MCP)

113 typed tools. Discover them live with `zcl_tools_list`; smoke-test with
`zcl_self_test`; dump schemas with `zcl_openapi`. Source of truth is the
controller `k_routes[]` arrays.

### Start here
- `zclassic23 agentinterface` / `zcl_agent_interface` — preferred AI operator
  interface contract. MCP is the primary interactive API, native CLI JSON is
  the script/human fallback, REST is public read-only, and no Python or
  `tools/z` logic is required. Its `capabilities[]` matrix and
  `machine_contract` block are the programmatic source for agent transport,
  schema, JSON, and compatibility expectations.
- `zclassic23 agentmap` / `zcl_agent_map` — AI-coder map for the native/MCP
  operator surface: where code lives, which docs apply, and which tests cover
  each subsystem. The full contract guide is `docs/AGENT_API.md`.
  First-call method/schema/tool metadata is centralized in
  `app/controllers/include/controllers/agent_contracts.def`; registry-backed
  `agentmap` command rows and telemetry drilldowns are grouped in
  `app/controllers/src/agent_contract_registry.c`
  (`g_agent_command_surfaces`), including generic diagnostic primitives like
  `dumpstate`/`zcl_state`, `getnodelog`/`zcl_node_log`, and
  `dbquery`/`zcl_sql`, plus the raw event ring `eventlog`/`zcl_events`, not as
  local string tables in the controllers. Non-method aliases such as
  `command_center`, `full_status`, and `quality_lanes` also live there as
  direct native/MCP command-surface rows. `agentops` first-call scalar fields
  such as `native_command`, `diagnose_tool`, `anchor_status_command`, and
  `peer_incidents_tool` live there too as `g_agent_field_surfaces`. The same
  registry owns
  `probe_params_json` for parameterized availability probes; nested schema
  rows live in `app/controllers/src/agent_contract_schema_registry.c`
  (`g_agent_schema_surfaces`). REST-index
  operator drilldowns such as
  `healthcheck`/`zcl_health`, `milestone`/`zcl_milestone`, and
  `refold`/`zcl_refold_status` also belong in that registry.
- `zclassic23 agentops` / `zcl_agent_ops` — compact no-`jq` operator command
  center. Its first-call scalar command fields, direct/drilldown commands,
  API-gap list, and top-next-work list are registry-fed from
  `agent_contract_registry.c` (`g_agent_contracts`,
  `g_agent_field_surfaces`, `g_agent_command_surfaces`,
  `g_agent_work_surfaces`) and
  `agent_contract_schema_registry.c` (`g_agent_schema_surfaces`);
  architecture-review objects live in
  `agent_contract_review_registry.c` (`g_agent_review_surfaces`);
  keep controller code focused on assembling live state, not owning ranked
  planning tables. `agentcontracts.contract_summary` reports registry-derived
  native/MCP/REST, review-surface, and schema-surface counts plus separate
  contract/review/schema registry source fields, and MCP tests verify declared
  tools are registered.
- `zclassic23 agentlanes` / `zcl_agent_lanes` — native canonical/soak/dev lane
  topology, `zcl.operator_deployment_safety.v1` policy, and
  `zcl.operator_lane_recovery.v1` boot-recovery sentinel state. Use this before
  deciding where a fresh binary may be deployed or restarted.
- `zclassic23 agentliveness` / `zcl_agent_liveness` — unified current-lane
  liveness rollup: lane identity, observed listeners, supervisor children,
  background quality verdicts, direct `overall_liveness`, and next drilldowns.
- `zclassic23 agentdiagnose` / `zcl_agent_diagnose` — bounded no-jq
  diagnosis packet for first-call work: status, healthcheck, peer lifecycle
  incidents, mirror status, timeline pointers, and a safe next action.
- `zclassic23 peerincidents` / `zcl_peer_incidents` — compact bounded peer
  incident packet for reconnect storms, duplicate host entries, last
  disconnect reason, flat primary issue fields, host direction/mixed-direction
  classification, services, advertised height plus whether that height is
  bootstrap-trusted, bootstrap/fast-sync readiness, and stability blocker
  verdicts. The native RPC and embedded `agentdiagnose.peer_incidents` payloads
  add registry-owned `method`, `native_command`, `mcp_tool`, and
  `contract_source` fields from `agent_contracts.def`, so help,
  `agentcontracts`, MCP coverage, and API discovery stay in sync with the
  native command.
- `zclassic23 agentimpact <files...>` / `zcl_agent_impact` — map changed paths
  to risk flags and focused test groups before choosing the verification set.
  The shared routing table lives at
  `app/controllers/include/controllers/agent_impact_rules.def` and is consumed
  by both native `agentimpact` and `make fast-ci`.
- `zclassic23 agentbuild` / `zcl_agent_build` — fast cached build contract:
  `make build-only`, `make t-fast`, `make fast-ci`, cache knobs, strict gates,
  and `make ci-reproducible`.
- `zclassic23 statecatalog` / `zcl_state_catalog` — machine-readable catalog
  for every `zcl_state` subsystem: name, description, accepted key forms,
  expected cost, freshness, owner shape/file, read-only safety level, focused
  tests, and native/MCP drill-down commands.
- `zclassic23 timeline '{"category":"sync","count":50,"since_secs":3600}'` /
  `zcl_timeline` — versioned semantic event timeline over the structured event
  ring with bounded server-side filters for `since`, `height`, `peer`,
  `reducer_stage`, `condition`, `deploy`, and `lane`. Categories include
  `sync`, `peer`, `message`, `chain`, `validation`, `condition`, `oracle`,
  `mirror`, `boot`, `db`, `wallet`, `disk`, `mcp`, and `net`; responses include
  `head_seq`, `semantic_summary`, `type_counts`, `peer_counts`,
  `log_references`, `recommended_drilldowns`, and `events[].seq` cursor fields.
  The response/category layer is
  `app/controllers/src/event_timeline_controller.c`; object/CLI parsing and
  bounded filter matching live in
  `app/controllers/src/event_timeline_filter_controller.c`.
- `zclassic23 api` — native API discovery from the running node. It returns the
  same `zcl.rest_index.v1` body as REST `GET /api` and `GET /api/v1`, with
  `api_version`, `base_path`, resource routes, CRUD conventions, and the
  recommended native/MCP/REST first calls. Use this instead of wrapper scripts.
- `zcl_agent` — shortest MCP-friendly first check: compact status with stable
  top-level `status`, heights, gap, peer counts, primary blocker, and
  recommended next tool. Same simple contract as native `zclassic23 agent`,
  and REST `GET /api/v1/agent`.
  `zcl_operator_summary` is the longer compatible alias.
- `zclassic23 milestone` / `zcl_milestone` — node-computed progress to the
  next version milestone. Returns `zcl.milestone_status.v1` with ASCII
  `systems`, `goals`, and `subgoals` bars plus the underlying MVP criteria.
  REST serves the same contract at `GET /api/v1/milestone`.
- `zcl_status` — full diagnostic tree: height, peers, sync, onion, health,
  reducer frontier, tip-finalize, condition engine, and chain source scoring.
- `zcl_kpi` — aggregated KPIs (height, peer_count, sync, validation, mempool,
  wallet, chain, network).

`tools/z` is a deprecated compatibility shim for terminal scripts. Keep it
working, but do not add new operator logic there; add native C JSON contracts
and expose them through MCP/REST instead.

### Catalog and primitives (prefer these over a new bespoke tool)
- `zcl_state_catalog()` — discover the `zcl_state` subsystem list and metadata
  before drilling into a subsystem. Same payload as native
  `zclassic23 statecatalog` (`zcl.state_catalog.v1`).
- `zcl_state(subsystem=X)` — generic state dump. Subsystem CSV is
  auto-populated at runtime from the diagnostics registry. Currently ~56
  subsystems wired (supervisor, watchdog, boot, block_index, health,
  chain_evidence, chain_advance_coordinator, legacy_mirror, oracle,
  header_probe, verify_engine, ...).
- `zcl_node_log(pattern, since_secs, max_lines, level)` — server-side reverse
  scan of node.log in 64 KB chunks.
- `zcl_timeline(category, count, since_secs, peer, height, reducer_stage,
  condition, deploy, lane)` — category-filtered structured events with
  `zcl.timeline.v1` metadata, bounded server-side filters, semantic summaries,
  type/peer counts, log references, suggested drill-downs, and seq cursors;
  prefer this before raw `zcl_events` when answering root-cause questions.
- `zcl_sql("SELECT ...")` — SELECT-only, semicolon-rejected, auto-LIMIT, 2 s
  budget, 100-row cap, rate-gated 1 RPS.

### Escape hatch
- `zcl_rpc(method, params)` — any of 85+ RPC methods when no typed tool fits.

### REST API versioning
`/api/v1` is the canonical REST base and `/api` is the compatibility base.
`zclassic23 api` is the native no-HTTP discovery command and must return the
same `zcl.rest_index.v1` body as both REST index paths.
Keep version/schema constants in
`app/controllers/src/api_controller_internal.h`, exact resource routes in
`app/controllers/src/api_controller_routes.c`, and contract tests in
`lib/test/src/test_api.c`. Unsupported version prefixes such as
`/api/v2/agent` must return `zcl.rest_error.v1` with
`error="unsupported_api_version"` and `supported_versions`.

Route contracts must be self-describing for CRUD clients. Every entry emitted
by `api_route_contracts_json()` carries `crud_operation` (`read`, `create`,
`update`, `delete`), `resource_scope` (`collection`, `item`, `singleton`,
`subcollection`, `subresource`), `crud_name` (`read_item`, etc.), and
`id_params`. Application-layer routes also carry `application_protocol`,
`layer`, `source_anchor`, `read_model`, `write_semantics`, and
`consensus_boundary`. Keep `/api/v1` and `/api/v1/openapi` generated from that
one contract source and pin representative collection/item/singleton routes in
`test_api` whenever adding a new route shape.

Application protocols such as ZSLP, ZNAM, market, messaging, and future
script-contract workflows should expose noun-shaped REST resources over
chain-derived projections. Reads come from indexed projections at the served
frontier; mutations construct/broadcast explicit transactions or operator-gated
actions and never bypass the base-layer reducer/consensus path with direct
state writes. The machine-readable version of this boundary is
`app/controllers/src/api_controller_app_protocols.c`, surfaced as
`layer_model` in `zclassic23 api` / `/api/v1`, `x-zcl-layer-model` in
`/api/v1/openapi`, and per-route `application_protocol` /
`x-zcl-application-protocol`; update that C-owned registry before adding
wrapper prose or out-of-band docs for a new application protocol.

Public status/freshness endpoints must get their served height through
`api_served_tip_height()`, not by reading one endpoint-specific cursor. That
helper prefers the published in-memory H* frontier and falls back to the durable
`tip_finalize` cursor during process startup, keeping `/api/status`,
`/api/v1/hodl`, and `/api/v1/factoids` on the same visible-tip contract.
Milestone/version progress lives beside public status in
`api_milestone_status_json()` and is exposed through native RPC
`milestone`/`mvpstatus`, REST `/api/v1/milestone`, and MCP `zcl_milestone`.
Keep strict MRS scoring separate from partial/proxy subgoal progress.

### MCP target gotcha
`mcp__zcl23-dev__*` hit the DEV node (`~/.zclassic-c23-dev`, port 18252).
For LIVE, use `mcp__zcl23-live__*` / curl port 18232 (`~/.zclassic-c23`).
Confirm the target before acting.

### Add state introspection (no new MCP route needed)
1. In the subsystem header:
   `bool <name>_dump_state_json(struct json_value *out, const char *key);`
2. Implement in the subsystem `.c` (caller does `json_set_object(out)` first;
   use `atomic_load` for thread-touched fields; don't allocate).
3. Register one line in `app/controllers/src/diagnostics_registry.c`
   (`g_dumpers[]`). That's it — `zcl_state_catalog` and `zcl_state`
   auto-expose it with owner file, accepted key forms, safety level, tests, and
   drill-down commands. ~30 lines total.

---

## 4. Build / test / deploy

| Command | Effect |
|---------|--------|
| `make -j$(nproc)` | Build `zclassic23`, `test_zcl`, `zclassic-cli`. `-j` only overlaps the 2–3 binaries + LTO link, not per-binary front-end. |
| `make build-only` | 664 genuinely-parallel `cc -c` with depfile header tracking — fast inner loop. |
| `make test` | Runs `test_parallel` (isolated per-process runner). **Use this**, not test_zcl. Green = regression floor, NOT a liveness proof. |
| `make test-full` | Runs the `test_zcl` monolith (sequential). |
| `make lint` | All 45+ `check-*` gates. Must pass before tests. HARD gates fail the build; RATCHET gates compare to baselines. |
| `make ci` | lint + bench-regress + build + `test_parallel` (retry-once for flakes) + symbol-floor. Pre-push hook runs this. |
| `make deploy` | `rm` stale binary, rebuild fresh, WAL checkpoint, `systemctl restart`, verify running `build_commit` (`deploy_verify.sh`). If RPC stays closed during crash-only recovery, the verifier reports the pre-RPC `reindex-chainstate` progress from `node.log`. |
| `make deploy-dev` | Hot-swap into the dev node (ports 8053/18252) via `tools/dev/deploy-dev-lane.sh`. Never touches live. If `auto_reindex_request` is pending, it refuses by default so a normal code deploy does not consume the marker and disappear into a long pre-RPC chainstate rebuild; `zclassic23 agentdeployguard deploy-dev` reports the same blocker as typed JSON and exits nonzero on refusal. Set `ZCL_DEV_ALLOW_AUTO_REINDEX_DEPLOY=1` only for a deliberate recovery boot. |
| `make lane-health` | Read-only canonical/soak/dev lane status, lag, peers, listeners, memory pressure, and snapshot-loader hints. |
| `make lane-recover LANE=dev` | Plan bounded noncanonical recovery as `zcl.lane_recovery_plan.v1`; set `ZCL_LANE_RECOVERY_APPLY=1` to restart only dev/soak. Canonical/live/main is refused. |
| `build/bin/test_zcl` | Run all tests directly. |
| `build/bin/zcl-rpc <method>` | Preferred direct RPC client for zclassic23 terminal checks; honors the project RPC env defaults. |
| `build/bin/zclassic-cli -rpcport=18232 <method>` | Explicit zclassic23 RPC without MCP. Avoid bare `zclassic-cli` for stability diagnosis because local defaults may target another lane. |

### Boot stages (`lib/util/include/util/boot_phase.h`)
12 ordered stages; out-of-order advance aborts:
`INIT → DATADIR_LOCKED → CRYPTO_READY → DB_OPEN → WALLET_LOADED →
BLOCK_INDEX_LOADED → CHAIN_TIP_RESOLVED → NETWORK_READY → SERVICES_RUNNING →
READY → SHUTDOWN_REQUESTED → SHUTDOWN_COMPLETE`. Migrations run at `DB_OPEN`.

### Recovery / deploy doctrine
- Copy-prove on a fixture datadir before any live change.
- Gate on **H\* CLIMB**, never "booted without FATAL."
- Two-step cold-sync (legacy, proven): `build/bin/zclassic23
  --importblockindex $HOME/.zclassic` then a normal boot. Skipping the header
  import is a footgun (leaves a ~3.1M-header hole → pins).
- Validate consensus against the real CHAIN, not the zclassicd source text.

### Frozen vs replaceable
Frozen: the 8 shapes, 10 laws, lint ratchet, folder layout, event-log schema.
Replaceable: C23, SQLite, systemd, Tor v3, crypto algos (behind the agility
ladder). Build to the contract, not the implementation.
