# CODEBASE_MAP.md — where things live + how to do each thing

Fast reference for a fresh agent. Plain and technical. For the *why* (laws,
shapes, doctrine) read `docs/FRAMEWORK.md`; for the concrete feature-slice
contract (REST resources, ActiveRecord, validations, relationships, database
schema, services, MCP/native) read `docs/AGENT_ARCHITECTURE.md`; for *current
live state* read `docs/HANDOFF.md`; for *coding rules* read
`docs/DEFENSIVE_CODING.md`. For the one-page mental model read
`docs/HOW_THE_NODE_WORKS.md`.

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
| Conditions | `app/conditions/` | 53 | (detect, remedy, witness) healers; poll/backoff/page-on-exhaustion | `app/conditions/src/block_failed_mask_at_tip.c` |
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
<!--   condition_registrations = condition_register() calls in app/conditions/src    -->
test_groups: 545
port_interfaces: 12
persistence_adapters: 13
condition_registrations: 31
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

### Add a REST-backed feature/resource
Use `docs/AGENT_ARCHITECTURE.md` as the full checklist. The short path:

1. Name the noun/resource and REST shape first
   (`/api/v1/<resources>`, `/api/v1/<resources>/{id}`,
   `/api/v1/<resources>/{id}/<child_resources>`).
2. Add or migrate schema in `app/models/src/database_schema.c` or
   `app/models/src/database_migrate_features.c`: primary key, `CHECK`
   constraints, relationship columns, and indexes for every exposed list/filter.
3. Add the model in `app/models/include/models/<resource>.h` and
   `app/models/src/<resource>.c`: `DEFINE_MODEL_CALLBACKS`, `validate_*`
   using `validates_*`, `db_<resource>_save` via `AR_*_SAVE`, reads/scopes,
   and relationship helpers such as `db_order_product()` or
   `db_name_text_records()`.
4. Put workflow in `app/services/src/` with `struct zcl_result`; services own
   transactions and call models, but do not parse HTTP/RPC/MCP inputs.
5. Add REST route metadata in `app/controllers/src/api_controller_routes.c`
   or the relevant dynamic/member controller: method, path, resource, action,
   response schema, query filter contract, freshness, alias, privacy.
6. Add typed MCP/native access only after the service/model contract exists.
   MCP routes live in `tools/mcp/controllers/*_controller.c`; terminal agents
   call them with `zclassic23 mcpcall <tool> [json]`.
7. Cover model validation, migration/schema, relationship failure, service
   success/failure, REST contract, and MCP controller behavior with focused
   tests before running `make build-only` and `make lint`.

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

100+ typed tools. Discover them live with `zcl_tools_list`; smoke-test with
`zcl_self_test`; dump schemas with `zcl_openapi`. Source of truth is the
controller `k_routes[]` arrays.

### Start here
- `zclassic23 agentinterface` / `zcl_agent_interface` — preferred AI operator
  interface contract. MCP is the primary interactive API, native CLI JSON is
  the script/human fallback, REST is public read-only, and no Python or
  `tools/z` logic is required. Its `capabilities[]` matrix and
  `machine_contract` block are the programmatic source for agent transport,
  schema, JSON, and compatibility expectations. Capability rows are emitted
  from `agent_contracts.def` via
  `app/controllers/src/agent_contract_capability_registry.c`; v1 compatibility
  aliases are marked with `registry_alias=true` instead of repeating
  schema/tool strings in the controller.
- `zclassic23 servicecatalog [name]` / `zcl_service_catalog(name?)` /
  `GET /api/v1/service-catalog` /
  `GET /api/v1/service-catalog/{service}` /
- `zclassic23 serviceoperations [operation_id|key=value...]` /
  `zclassic23 serviceoperations service=bootstrap write_safety=public_read_only` /
  `zcl_service_operations(operation_id?, service?, write_safety?,
  preferred_interface?, status?, surface?)` /
  `GET /api/v1/service-operations?service=znam_names&surface=rest` /
  `GET /api/v1/service-operations/{operation_id}` /
  `GET /api/v1/names/{name}/services` /
  `GET /api/v1/names/{name}/services?transport=p2p&valid=true&endpoint_only=true`
  — UX-facing sovereign
  service catalog. It answers what this node can host, advertise, verify, or
  construct for a user across names, bootstrap, Tor/onion discovery, P2P,
  files, market, messaging, and script contracts. The top-level
  `sovereign_ux` object gives agents the canonical names→services→Tor/P2P→CRUD
  flow. The top-level `runtime_probes[]` matrix is the compact checklist for
  proving every service on the running node, and member contracts expose
  `depends_on_services`, `read_model`, `write_model`, `runtime_probe`,
  `operation_summary`, and `operations[]` so agents do not infer dependencies,
  live proof routes, or write safety from prose. `runtime_probe` is the
  per-service recipe for verifying the running node: route, expected schema,
  freshness source, success signal, operation contract link, and next action on
  failure. Operation IDs are stable
  `service.operation` strings, such as `znam_names.resolve_name`. Each
  operation also publishes
  `service_catalog_route`, `agent_preferred_interface`, `agent_next_step`, and
  callable booleans so agents can navigate from service intent to the safest
  callable surface without route guessing. Filtered service-operation and
  name-service-directory responses include `filter_contract`
  (`zcl.query_filter_contract.v1`); unknown filter names fail closed with
  structured 400 errors instead of returning accidentally unfiltered
  collections. `/api/v1` route contracts and `/api/v1/openapi` expose the same
  data as `filter_contract` / `x-zcl-filter-contract`, so agents can validate
  query keys without probing an endpoint first. The implementation is split between
  `app/controllers/src/api_controller_service_catalog.c` and
  `app/controllers/src/api_controller_service_operations.c`; `/api/v1/services`
  remains runtime health.
- `zclassic23 status` / `zclassic23 agent` / `zcl_agent` — bounded live status
  from the running node. `status` is a native compatibility alias owned by
  `agent_contracts.def`; it emits the same `zcl.public_status.v1` payload as
  `agent` and `GET /api/v1/agent`. Its `security_posture` object is owned by
  `app/controllers/src/agent_security_posture.c` and names the borrowed
  snapshot/full-history-validation posture plus shielded-nullifier history
  coverage.
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
  such as `diagnose_tool`, `anchor_status_command`, and `peer_incidents_tool`
  live there too as `g_agent_field_surfaces`, while its top-level
  schema/method/native/MCP identity fields come from `agent_contracts.def`.
  The same registry owns
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
  liveness rollup: compact lane identity, observed listeners, supervisor
  counts, background quality counts, direct `overall_liveness`, and next
  drilldowns. Use `agentliveness full` /
  `zcl_agent_liveness(mode="full")` only when embedded runtime-availability
  methods, supervisor domains, and quality lane arrays are needed.
  Top-level schema/method/native/MCP identity fields are registry-owned by
  `agent_contracts.def`.
- `zclassic23 proofbundle [anchor_datadir]` / `zcl_proof_bundle` — read-only
  `zcl.operator_proof_bundle.v1` evidence artifact. It embeds the current
  `agent`, `milestone`/`operator_proofs`, `refold`, `anchorstatus`,
  `agentlanes`, and `agentdevstatus` contracts so agents can capture the
  current MVP/sovereign/dev-lane proof state with one native C command.
- `zclassic23 agentdiagnose` / `zcl_agent_diagnose` — bounded no-jq
  diagnosis packet for first-call work: compact status, peer lifecycle incident
  counts/primary host issue, mirror status, drill-down pointers, and a safe
  next action. Use `agentdiagnose full` / `zcl_agent_diagnose(mode="full")`
  only when embedded `agent`, `healthcheck`, `peer_incidents`, mirror, and
  timeline objects are needed. Its top-level schema/method/native/MCP identity
  fields are also registry-owned.
- `zclassic23 peerincidents` / `zcl_peer_incidents` — compact bounded peer
  incident packet for reconnect storms, duplicate host entries, last
  disconnect reason, flat primary issue fields, host direction/mixed-direction
  classification, services, advertised height plus whether that height is
  bootstrap-trusted, bootstrap/fast-sync readiness, and stability blocker
  verdicts. The native RPC and full-mode embedded
  `agentdiagnose.peer_incidents` payloads add registry-owned `method`,
  `native_command`, `mcp_tool`, and
  `contract_source` fields from `agent_contracts.def`, so help,
  `agentcontracts`, MCP coverage, and API discovery stay in sync with the
  native command. Native CLI and MCP automatically fall back to
  `dumpstate peer_lifecycle incidents` when an older running target lacks the
  direct `peerincidents` RPC, preserving the same schema and marking
  `compatibility_fallback=true`.
- `zclassic23 agentimpact <files...>` / `zcl_agent_impact` — map changed paths
  to risk flags and focused test groups before choosing the verification set.
  The shared routing table lives at
  `app/controllers/include/controllers/agent_impact_rules.def` and is consumed
  by both native `agentimpact` and `make fast-ci`.
- `zclassic23 agentbuild` / `zcl_agent_build` — fast cached build contract:
  `make agent-loop`, `make fast-compile`, `make build-only`, `make dev-bin`,
  `make t-fast`, `make fast-ci`, cache knobs, strict gates, typed `mcpcall`,
  fast dev deploy, dev-lane status commands, and `make ci-reproducible`.
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
  `systems`, `goals`, and `subgoals` bars, the underlying MVP criteria, and
  nested `operator_proofs` (`zcl.mvp_operator_proofs.v1`) that names each
  criterion's proof command, CI regression floor, and current blocker. REST
  serves the same contract at `GET /api/v1/milestone`.
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
`layer`, `source_anchor`, `read_model`, `write_semantics`,
`consensus_boundary`, object types, UX surfaces, projection/reorg behavior,
cryptographic model, transport model, privacy model, and diagnostics surface.
Routes with strict path validators carry `path_param_contract`
(`zcl.path_param_contract.v1`); `/api/v1/openapi` mirrors that as
`x-zcl-path-param-contract`. ZNAM `{name}` routes currently advertise the
`znam_validate_name` lifecycle contract there so agents can reject malformed
names before probing a route.
Routes backed by a REST-callable service operation also carry
`service_contract`, `service_catalog_route`, `service_operation_id`,
`service_operation_route`, and embedded `service_binding`; OpenAPI mirrors that
as `x-zcl-service-binding`. Keep those bindings generated from
`api_controller_service_operations.c`, not duplicated in route docs. `test_api`
checks the full invariant: every REST-callable service operation must bind to a
route contract, and every route `service_binding` must point back to the same
operation.
Keep `/api/v1` and `/api/v1/openapi` generated from that
one contract source and pin representative collection/item/singleton routes in
`test_api` whenever adding a new route shape. Service-operation member routes
must stay read-only metadata lookups unless a separate, operator-authenticated
write surface is deliberately added and tested.

Application protocols such as ZSLP, ZNAM, market, messaging, and future
script-contract workflows should expose noun-shaped REST resources over
chain-derived projections. Treat **ZLSP** as the umbrella for this
application/service layer: ZCL remains the base layer, while zclassic23 exposes
versioned CRUD resources and typed MCP/native JSON methods for services built
from valid ZCL transactions. Reads come from indexed projections at the served
frontier; mutations construct/broadcast explicit transactions or operator-gated
actions and never bypass the base-layer reducer/consensus path with direct
state writes. The machine-readable version of this boundary is
`app/controllers/src/api_controller_app_protocols.c`, surfaced as
`layer_model` in `zclassic23 api` / `/api/v1`, `x-zcl-layer-model` in
`/api/v1/openapi`, and per-route `application_protocol` /
`x-zcl-application-protocol` plus security/projection/UX OpenAPI extensions;
update that C-owned registry before adding wrapper prose or out-of-band docs
for a new application protocol.

Public status/freshness endpoints must get their served height through
`api_served_tip_height()`, not by reading one endpoint-specific cursor. That
helper prefers the published in-memory H* frontier and falls back to the durable
`tip_finalize` cursor during process startup, keeping `/api/status`,
`/api/v1/hodl`, and `/api/v1/factoids` on the same visible-tip contract.
Milestone/version progress lives beside public status in
`api_milestone_status_json()` and is exposed through native RPC
`milestone`/`mvpstatus`, REST `/api/v1/milestone`, and MCP `zcl_milestone`.
Keep strict MRS scoring separate from partial/proxy subgoal progress. When
milestone says `live.source="agent_cached_summary"`, its live height, peer, and
sync fields must match a direct agent-status packet; `test_api` and
`test_syncdiag_rpc` enforce that. If any required agent field is missing, the
endpoint must say `agent_cached_summary_with_fallbacks` and name the fallback
source rather than silently mixing authorities. `operator_proofs` is static
MVP-proof metadata, not a live-health authority; update it with `docs/MVP.md`
when the accepted proof command or blocker for a criterion changes.
The bounded agent fast path may use a cached chain-advance decision only when
its projection fields are internally consistent with the served/tip frontier;
stale cache shapes are named as `cached_status_inconsistent` and leave the
top-level `indexed_height` on the current frontier.

Bootstrap-service readiness is the network-facing public singleton
`/api/v1/bootstrap` (compat alias `/api/v1/bootstrapstatus`) over the shared
`network_bootstrap_status_json()` contract. Keep it schema-identical with RPC
`bootstrapstatus` and MCP `zcl_bootstrapstatus`; do not duplicate bootstrap
field assembly in a REST-only handler. The nested
`snapshot_loader.authority` object is the C-native proof that a fast-start
bundle actually became local durable authority (`coins_kv`,
`coins_applied_height`, reducer H*, and self-folded marker), not just files on
disk.

ZNAM service records bridge chain-projected names into service contracts in
`app/controllers/src/name_controller.c`. Keep `service_records[]` additive and
machine-readable: every endpoint hint should include `service_contract`,
`service_catalog_route`, `recommended_operation_id`, `service_operation_route`,
`service_contract_known`, `service_operation_required`,
`service_operation_known`, `contract_resolution_status`,
`contract_resolution`, `runtime_probe`, `endpoint_validation`,
`endpoint_routing`, `routing_priority`, `endpoint_hint_valid`, and
`next_action` so an agent can go
from a confirmed name to a Tor/P2P endpoint, distinguish canonical service
contracts from arbitrary chain text, prove the linked service is usable on the
running node, reject malformed endpoint hints without hiding them, and then
reach the exact CRUD operation contract without guessing.
`GET /api/v1/names/{name}/services` is the narrow read
subcollection for that same directory; it must stay a copy of the embedded
`service_directory` model plus standalone route metadata, not a second
service-record serializer. The directory-level `routing_plan` summarizes the
preferred transport order and valid/invalid endpoint counts for agents that
need one bounded object before deciding direct P2P vs onion fallback.
The route metadata must keep the ZNAM `{name}` path contract in sync with
`znam_validate_name`, and `test_api` pins the contract in both `/api/v1` and
OpenAPI.

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
| `make fast-changed-compile` | Cheapest guarded edit check; compiles changed node `.c` files directly into `build/dev-obj/`, with safe fallback to `fast-compile`. |
| `make fast-compile` | Fastest no-link dev compile check; cached non-LTO `build/dev-obj` objects. |
| `make build-only` | Strict release-flag `cc -c` with depfile header tracking; use before push/release. |
| `make fast-rebuild` | Fast local node binary alias for `make dev-bin`; cached per-file objects, no LTO, uses `ccache` automatically when installed. |
| `make dev-bin` | Build `build/bin/zclassic23-dev` from cached per-file objects, non-LTO/unstripped, with hot consensus/crypto/script/validation buckets still optimized. Local iteration only; not deploy/release. |
| `make agent-dev-status` | No-build read-only dev-lane status. Reports the explicit worker-lane contract (`role=worker`, `mutation_policy=noncanonical_dev_only`, never live/soak), source/staged binaries, service PID, RPC or pre-RPC recovery, saved deploy state, auto-reindex marker, deploy blocker/reason, stale-marker candidate, and next safe action. Use `ARGS=--json`, native `zclassic23 agentdevstatus`, or MCP `zcl_agent_dev_status` for `zcl.agent_dev_status.v1`. |
| `make agent-clear-stale-dev-reindex` | Clears a proven-stale dev-lane `auto_reindex_request` by archiving it after the dev RPC is up and served height is at or above the marker anchor. Never touches canonical or soak. |
| `make agent-stage-dev` | Build the fast dev binary and atomically stage it at `~/.local/bin/zclassic23-dev` for the next `zcl23-dev` restart without stopping the running service. |
| `make agent-loop` | Default one-command AI/operator loop. Runs `fast-ci`; set `ZCL_AGENT_LOOP_BIN=1` to also build `build/bin/zclassic23-dev`, or `ZCL_AGENT_LOOP_DEPLOY=dev` to run the fast dev-lane hot-swap. |
| `make fast-ci` | Cache-aware edit loop: `lint-fast`, changed compile gate, focused mapped tests, and native live probe. Use `ZCL_FAST_TESTS=...`, `ZCL_FAST_LIVE=0`, `ZCL_FAST_CACHE=0`, `ZCL_FAST_CACHE_RESET=1` as needed. |
| `make test` | Runs `test_parallel` (isolated per-process runner). **Use this**, not test_zcl. Green = regression floor, NOT a liveness proof. |
| `make t ONLY=simnet` | Runs the deterministic simulator harness and the current action coverage matrix documented in `docs/SIMULATOR.md`. |
| `make test-full` | Runs the `test_zcl` monolith (sequential). |
| `make lint` | All 45+ `check-*` gates. Must pass before tests. HARD gates fail the build; RATCHET gates compare to baselines. |
| `make ci` | lint + bench-regress + build + `test_parallel` (retry-once for flakes) + symbol-floor. Pre-push hook runs this. |
| `make deploy` | `rm` stale binary, rebuild fresh, WAL checkpoint, `systemctl restart`, verify running `build_commit` (`deploy_verify.sh`). If RPC stays closed during crash-only recovery, the verifier reports the pre-RPC `reindex-chainstate` progress from `node.log`. |
| `make deploy-dev` | Hot-swap into the dev node (ports 8053/18252) via `tools/dev/deploy-dev-lane.sh`. Never touches live. If `auto_reindex_request` is pending, it refuses by default so a normal code deploy does not consume the marker and disappear into a long pre-RPC chainstate rebuild; `zclassic23 agentdeployguard deploy-dev` reports the same blocker as typed JSON and exits nonzero on refusal. Set `ZCL_DEV_ALLOW_AUTO_REINDEX_DEPLOY=1` only for a deliberate recovery boot. Each run saves `~/.zclassic-c23-dev/agent-deploy.json` with build and verification state. |
| `make deploy-dev-fast` / `make agent-deploy-fast` | Fast dev-lane hot-swap using cached non-LTO `build/bin/zclassic23-dev` by default. Use `ZCL_DEV_DEPLOY_BUILD=strict make deploy-dev` when you intentionally want the production-style binary in the dev lane. The saved deploy record is useful while RPC is still pre-ready. |
| `make lane-health` | Read-only canonical/soak/dev lane status, lag, peers, listeners, memory pressure, and snapshot-loader hints. |
| `make lane-recover LANE=dev` | Plan bounded noncanonical recovery as `zcl.lane_recovery_plan.v1`; set `ZCL_LANE_RECOVERY_APPLY=1` to restart only dev/soak. Canonical/live/main is refused. |
| `build/bin/test_zcl` | Run all tests directly. |
| `make agent-mcp-call TOOL=<tool> [ARGS='{}']` | Fresh source-tree typed MCP smoke call from a terminal agent; refreshes `build/bin/zclassic23-dev` first, for example `make agent-mcp-call TOOL=zcl_status` or `make agent-mcp-call TOOL=zcl_state ARGS='{"subsystem":"supervisor"}'`. |
| `make agent-mcp-call-hot TOOL=<tool> [ARGS='{}']` | No-build typed MCP call through the existing `build/bin/zclassic23-dev`; use for routine read-only status/schema checks when the binary is already current enough. |
| `make agent-mcp-call-dev TOOL=<tool> [ARGS='{}']` | No-build typed MCP call through the installed `~/.local/bin/zclassic23-dev` against the `zcl23-dev` linger lane (`~/.zclassic-c23-dev`, RPC `18252`). |
| `build/bin/zclassic23 mcpcall <tool> [json]` | Direct release-binary typed MCP call after `make zclassic23` or deploy. |
| `build/bin/zcl-rpc <method>` | Legacy/debug RPC helper. Do not build new agent workflows around it; prefer `zclassic23` native commands or `zclassic23 mcpcall`. |
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
