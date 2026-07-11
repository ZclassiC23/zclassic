# AGENT_API.md — native API for AI coding operators

ZClassic23's agent API is the native binary and the MCP tools backed by the
same native RPC methods. Shell wrappers are compatibility shims only.
Future feature work should follow `docs/AGENT_ARCHITECTURE.md`: REST resource
first, database schema, ActiveRecord model, validations, relationships, service
workflow, REST route contract, then typed MCP/native surface.

## First calls

| need | native command | MCP tool |
|---|---|---|
| No-jq command center | `zclassic23 agentops` | `zcl_agent_ops` |
| Bounded diagnosis | `zclassic23 agentdiagnose` | `zcl_agent_diagnose` |
| Live node status | `zclassic23 status` (`agent` alias) | `zcl_agent` |
| Code/docs/test map | `zclassic23 agentmap` | `zcl_agent_map` |
| Lane topology | `zclassic23 agentlanes` | `zcl_agent_lanes` |
| Unified liveness | `zclassic23 agentliveness` | `zcl_agent_liveness` |
| Changed files to tests/risk | `zclassic23 agentimpact <files...>` | `zcl_agent_impact` |
| Versioned contracts | `zclassic23 agentcontracts` | `zcl_agent_contracts` |
| Fast build contract | `zclassic23 agentbuild` | `zcl_agent_build` |
| Unified save loop | `make dev-watch [MODE=auto\|hotswap\|reload\|stage\|check]` | `zcl_agent_dev_status` reports its latest verdict |
| Compilation database | `make agent-index` | `zcl_agent_build` embeds index freshness |
| Developer-loop benchmark | `make dev-loop-bench` | `zcl_agent_build` embeds the latest artifact status |
| In-process hot-swap status | `zclassic23 dumpstate hotswap` | `zcl_state(subsystem="hotswap")` |
| Persistent dev-process MCP bridge | `dev_hotswap` / `dev_mcp_call` JSON-RPC on the exact dev lane | resident MCP router |
| Read-only fast-lane plan | `make agent-plan` | `zcl_agent_build` advertises it |
| Combined dev doctor | `make agent-doctor` | `zcl_agent_build` advertises it |
| Generic state dump | `zclassic23 dumpstate <subsystem>` | `zcl_state` |
| Registry / tool discovery | `zclassic23 discover help` | `zcl_tools_list` |
| Dev-lane status | `zclassic23 agentdevstatus` (`make agent-dev-status`) | `zcl_agent_dev_status` |
| Anchor producer status | `zclassic23 anchorstatus` | `zcl_rpc(method="anchorstatus")` |
| Operator proof bundle | `zclassic23 proofbundle [anchor_datadir]` | `zcl_proof_bundle(anchor_datadir?)` |
| Application protocol catalog | `zclassic23 appprotocols` | `zcl_app_protocols` |
| Sovereign service catalog | `zclassic23 servicecatalog [name]` | `zcl_service_catalog(name?)` |
| Sovereign operation catalog | `zclassic23 serviceoperations [operation_id|key=value...]` | `zcl_service_operations(operation_id?, service?, write_safety?, preferred_interface?, status?, surface?)` |
| Preferred interface contract | `zclassic23 agentinterface` | `zcl_agent_interface` |
| State subsystem catalog | `zclassic23 statecatalog` | `zcl_state_catalog` |
| Semantic event timeline | `zclassic23 timeline '{"category":"sync","count":50,"since_secs":3600}'` | `zcl_timeline` |
| Peer incident view | `zclassic23 peerincidents` | `zcl_peer_incidents` |
| Deploy/restart guard | `zclassic23 agentdeployguard [action]` | `zcl_agent_deploy_guard` |
| Mirror lag/blocker contract | `zclassic23 getmirrorstatus` | `zcl_mirror_status` |

The native RPC contracts are implemented in `app/controllers/src/agent_controller.c`
for the agent map/build surface, `app/controllers/src/agent_contracts_controller.c`
for the versioned contract registry, and `app/controllers/src/agent_ops_controller.c`
for the focused no-jq command center. `app/controllers/src/agent_diagnose_controller.c`
owns the bounded diagnosis packet. MCP routes in
`tools/mcp/controllers/ops_controller.c` proxy those same native methods. REST
currently exposes the public status contract at `GET /api/v1/agent`.
First-call method/schema/tool metadata lives in the C-owned registry
`app/controllers/include/controllers/agent_contracts.def`; runtime availability,
the contract registry, the interface capability matrix, the `agentops`
direct/drilldown command lists, and the REST API index's native/MCP command
fields consume that table instead of maintaining separate lists. Generic
diagnostic primitives such as `dumpstate` / `zcl_state`, `getnodelog` /
`zcl_node_log`, the bounded SQL primitive `dbquery` / `zcl_sql`, and the raw
event-ring primitive `eventlog` / `zcl_events` are registry rows too, so agent
command catalogs do not hand-copy their native/MCP names. `agentmap` alias rows
that are not one-to-one method contracts, such as `command_center`,
`full_status`, and `quality_lanes`, live in
`app/controllers/src/agent_contract_registry.c` with direct native/MCP fields
instead of local controller tuples. `agentops` first-call envelope fields
(`schema`, `method`, `native_command`, `mcp_tool`, and `contract_source`) plus
scalar fields such as `diagnose_tool`, `timeline_tool`,
`anchor_status_command`, and `peer_incidents_tool` are grouped in the same file as
`g_agent_field_surfaces`, so the compact no-jq command center also reads
method names from registry data instead of hand-copying them. Registry rows
also own `probe_params_json`; parameterized first-call probes such as `dbquery`
and `eventlog` must declare a bounded sample there instead of adding
method-specific CLI branches. Operator drilldowns exposed by the REST index
(`healthcheck` / `zcl_health`, `milestone` / `zcl_milestone`, and `refold` /
`zcl_refold_status`) also live there, so native, MCP, and REST discovery share
the same command/tool/schema metadata. Nested first-call schema rows such as
`zcl.first_call_contract.v1`, `zcl.operator_lane.v1`,
`zcl.security_posture.v1`, and `zcl.node_resources.v1` are also registry-owned in
`agent_contract_schema_registry.c` instead of being hand-listed inside the
`agentcontracts` response builder.
The native `zclassic23 -help` agent/operator command section is generated by
`agent_print_native_usage()` from the same registry; do not hand-copy agent
command rows into `src/main.c` usage text.
`zclassic23 status` is a registry-owned native compatibility alias for
`zclassic23 agent`; it emits the same `zcl.public_status.v1` payload and uses
the same MCP tool (`zcl_agent`) and REST route (`GET /api/v1/agent`).
First-call recommendation arrays should use
`agent_push_contract_native_command_json()` or
`agent_push_contract_mcp_tool_json()` for registry-owned command/tool names;
keep only parameterized, composite, or subsystem-local commands inline.
Structured command arrays such as `agentlanes.commands` should use
`agent_push_contract_command_json()` for registry-owned commands and keep only
external helper scripts such as `tools/scripts/lane_health.sh --json` inline.
The static no-cookie native commands in `src/main.c` use one
`g_cli_static_agent_routes` table of method-to-handler pairs and only dispatch
routes whose method also exists in `agent_contracts.def`. Do not add a second
allowlist or a parallel `if/else` dispatch ladder for agent commands.
REST application-layer protocol metadata lives in
`app/controllers/src/api_controller_app_protocols.c`; the same rows feed
`zclassic23 appprotocols`, `zcl_app_protocols`, `GET /api/v1/protocols`,
`GET /api/v1/protocols/{name}`, `layer_model`, route-contract
`application_protocol` fields, and generated OpenAPI
`x-zcl-application-protocol` / protocol CRUD extensions for ZLSP, ZSLP, ZNAM,
market, messaging, and script-contract resources. The registry also carries
object types, UX surfaces, projection/reorg behavior, cryptographic model,
transport model, privacy model, and diagnostics surface, so agents can reason
about what the node can safely read, construct, rebuild, expose, and explain
without scanning per-feature prose first. Treat
`zcl.application_protocols.index.v1` as the layer-2 overlay catalog: ZCL
remains the base layer; zclassic23 exposes ZLSP-style versioned application
services that read, index, or construct valid ZCL transactions without changing
consensus rules. ZSLP is the token protocol inside this model; ZLSP is the
broader service/protocol umbrella.

The UX-facing service catalog lives in
`app/controllers/src/api_controller_service_catalog.c` and is exposed as
`zclassic23 servicecatalog [name]`, `zcl_service_catalog(name?)`,
`GET /api/v1/service-catalog`, and
`GET /api/v1/service-catalog/{service}`. Operations are first-class too:
`zclassic23 serviceoperations [operation_id|key=value...]`,
`zcl_service_operations(operation_id?, service?, write_safety?,
preferred_interface?, status?, surface?)`, `GET /api/v1/service-operations`,
and `GET /api/v1/service-operations/{operation_id}` list operations, filter
the operation set, or fetch one stable `service.operation` contract such as
`znam_names.resolve_name`. Server-side filters are exact-match:
`service`, `write_safety`, `preferred_interface`, `status`, and `surface`.
Examples: `zclassic23 serviceoperations service=bootstrap
write_safety=public_read_only` and
`GET /api/v1/service-operations?service=znam_names&surface=rest`. The
collection includes `filter_contract` (`zcl.query_filter_contract.v1`), and
unknown filter names fail closed with `400 invalid_service_operation_filter`
instead of returning an accidentally unfiltered operation list. The same
contract is advertised in `/api/v1` route contracts and `/api/v1/openapi` as
`x-zcl-filter-contract`, so agents can validate query keys before making a
call. Use these
surfaces when the question is "what can this node host, advertise, verify, or
construct for a user?" They distinguish the stable service/operation
contracts from `/api/v1/services`, which remains runtime health.
REST route contracts bind back to this registry when a route is owned by a
REST-callable service operation: `/api/v1` emits `service_contract`,
`service_catalog_route`, `service_operation_id`, `service_operation_route`, and
the embedded `service_binding`; `/api/v1/openapi` mirrors the same operation
object as `x-zcl-service-binding`. Prefer these fields over hardcoded
route-to-service maps.
For bootstrap specifically, public peer listing is
`bootstrap.list_peer_projection` (`GET /api/v1/peers`,
`zcl.peers.index.v1`); peer incident analysis remains the operator diagnostic
operation `bootstrap.inspect_peer_bootstrap_readiness` via
`zcl_peer_incidents` / `peerincidents`.
The collection schema is `zcl.service_catalog.v1`; the member schema is
`zcl.service_contract.v1`; the operation collection schema is
`zcl.service_operations.index.v1`; operation members use
`zcl.service_operation.v1`. Together they cover names, bootstrap, Tor/onion
discovery, direct P2P, files, market, messaging, script contracts, CRUD
capabilities, transports, verification model, trust model, privacy model, and a
concrete UX story per service. Each service also carries an `operations[]`
array of operation objects that names the action, CRUD capability, REST route
when public, RPC method, MCP tool, input/output contract, authority, execution
surface, write-safety class, and whether the operation is destructive or
operator-private. Each operation also carries `service_catalog_route`,
`agent_preferred_interface`, `agent_next_step`, and `*_callable` booleans so an
agent can choose REST for public reads, MCP for operator/private or destructive
actions, and RPC only when that is the explicit fallback. This is the
no-guesswork path for agents building a UX from names, bootstrap, Tor/P2P,
market, messaging, and script-contract capabilities. The operation collection
also carries `summary`, `service_facets`, `preferred_interface_facets`, and
`write_safety_facets`; read those first when choosing a workflow or rendering a
command palette, then fetch the specific operation contract only for the action
the user selected. Name service-directory reads are first-class:
`znam_names.resolve_service_directory` maps to public REST
`GET /api/v1/names/{name}/services` and returns
`zcl.names.service_directory.v1` without requiring a client to parse the full
name profile.
The collection also carries `sovereign_ux` (`zcl.sovereign_ux_contract.v1`):
a machine-readable flow from agent status → service catalog → ZNAM resolution
→ endpoint verification → direct P2P/onion routing → versioned CRUD operation.
The collection-level `runtime_probes[]` matrix is the compact first-pass live
verification checklist: one row per service with the REST route, expected
schema, operation contract, freshness source, success signal, and failure next
action. It is generated from the same service contracts as member
`runtime_probe`, and tests require every probe route and operation ID to resolve
through the REST/operation indexes.
Each member contract carries `depends_on_services`, `read_model`, and
`write_model`; use those fields instead of hand-inferring service dependencies.
Each member also carries `operation_summary`, a compact count of public reads,
operator-private calls, destructive calls, callable surfaces, status buckets,
and preferred interfaces for that service's `operations[]`. Each member also
has `runtime_probe` (`zcl.service_runtime_probe.v1`): the concrete REST route,
operation contract link, expected response schema, freshness source, success
signal, and failure next action to prove the service is usable on the running
node. Use it before telling a user that bootstrap, ZNAM, onion discovery, file
services, market, messaging, script contracts, or telemetry are live.

ZNAM resolution responses (`zcl.names.show.v1`) normalize service text records
for agents. In addition to raw `text_records`, read `service_records[]` and
`service_directory`: each service record has schema
`zcl.names.service_record.v1`, `service_name`, `transport`, `endpoint_kind`,
`endpoint`, `chain_verified`, `reachability_proof`, `service_contract`,
`service_catalog_route`, `recommended_operation_id`,
`service_operation_route`, `service_contract_known`,
`service_operation_required`, `service_operation_known`,
`contract_resolution_status`,
`contract_resolution`, `runtime_probe`, `endpoint_validation`,
`endpoint_routing`, `routing_priority`, `endpoint_hint_valid`, and
`next_action`. The nested
`contract_resolution` object is `zcl.names.service_contract_resolution.v1`;
`status=resolved` means the chain-projected service hint maps to a canonical
Zclassic23 service contract and, when present, operation contract. Unknown
service hints remain visible but are explicitly marked `service_unknown` so
agents do not treat arbitrary text records as trusted node capabilities. The
nested `runtime_probe` is the same `zcl.service_runtime_probe.v1` object
exposed by the service catalog member, so agents can verify the live route
without a second lookup. `endpoint_validation`
(`zcl.names.endpoint_validation.v1`) fails closed on malformed hints while
leaving the chain record visible; `endpoint_routing`
(`zcl.names.endpoint_routing.v1`) gives the preferred transport, fallback
transport, and priority for direct P2P vs onion decisions. When a client only
needs routing records, use the
dedicated subcollection `GET /api/v1/names/{name}/services`; it returns
`zcl.names.service_directory.v1` with `name_route`, `self_route`,
`operation_id=znam_names.resolve_service_directory`, concrete
`operation_route`, and the same
`records[]`/`endpoints[]` objects. The directory also publishes
`endpoints[]`, `endpoint_count`, `valid_endpoint_count`,
`invalid_endpoint_count`, `supports_onion`, `supports_direct_p2p`,
`supports_bootstrap`, `routing_plan`
(`zcl.names.service_routing_plan.v1`), the service/operation contract route
templates, the runtime probe schema/field name, and a routing policy. Agents
can ask the server to narrow this projection with exact-match filters:
`service`, `service_contract`, `transport`, `endpoint_kind`, `valid`, and
`endpoint_only`. For example,
`GET /api/v1/names/alice/services?transport=p2p&valid=true&endpoint_only=true`
returns only valid direct-P2P endpoint hints, with counts and routing plan
recomputed for that filtered view. Directory responses include
`filter_contract` (`zcl.query_filter_contract.v1`) with the allowed keys,
accepted aliases, and example call. Unknown filter names fail closed with a
structured `400 invalid_name_service_filter` response instead of returning an
accidentally unfiltered directory. The same contract is available from the
REST route index and from OpenAPI as `x-zcl-filter-contract`. The `{name}`
path parameter is also machine-described there as `path_param_contract` /
`x-zcl-path-param-contract` (`zcl.path_param_contract.v1`), pinned to
`znam_validate_name`: 1-63 lowercase ASCII letters/digits/hyphens, no leading
or trailing hyphen. Agents should
verify the
chain-projected ZNAM record first, inspect the linked service/operation
contract and runtime probe, then prefer direct P2P for low latency and fall
back to onion reachability when NAT or firewall conditions require it.

## Preferred Interface

The best interface for an AI coding operator is MCP with typed JSON tools:
start with `zcl_agent_ops` for the compact no-jq command center, use
`zcl_agent_interface` when checking the full transport contract, then use
`zcl_agent_diagnose`, `zcl_agent`, `zcl_agent_liveness`,
`zcl_agent_lanes`, `zcl_mirror_status`,
`zcl_agent_impact`,
`zcl_agent_build`, `zcl_state_catalog`, `zcl_state`, `zcl_timeline`,
`zcl_app_protocols`, `zcl_service_catalog`, `zcl_node_log`, and `zcl_sql` as
needed. The native binary commands (`zclassic23 agentinterface`,
`zclassic23 appprotocols`, `zclassic23 servicecatalog bootstrap`,
`zclassic23 status` / `zclassic23 agent`, etc.) are the second-best interface for terminal work and
scripts. REST is the public read-only mirror.

For terminal work, keep the operator path inside the same binary: use native
commands such as `build/bin/zclassic23 status`, `build/bin/zclassic23 dumpstate
supervisor`, or `build/bin/zclassic23 discover help`. Against the dev lane,
`build/bin/zclassic23-dev status` queries the installed dev binary at
`~/.zclassic-c23-dev` on RPC port `18252`. The native command registry is the
sole agent interface going forward (zero-MCP track); the `make agent-mcp-call*`
family and `zclassic23 mcpcall <tool>` are the legacy typed-MCP path and are
being removed in zero-MCP W3.
Use `make agent-plan` before a build when you need the exact no-build fast-lane
decision: changed files, selected focused tests, changed-compile plan, cache
hit/miss, dev-lane stage/deploy commands, and MCP shortcuts.
`build/bin/zcl-rpc getblockcount` and an explicit
`build/bin/zclassic-cli -rpcport=18232 getblockcount` are legacy/debug checks,
not the preferred agent interface. Do not use bare `build/bin/zclassic-cli` as
a zclassic23 status oracle: local defaults, cookies, datadirs, or environment
can point it at another RPC target and create a false "zclassic23 is behind"
diagnosis. If a height/peer answer matters, the target lane must be explicit in
the command or supplied by the C-owned agent surface (`zclassic23 agent`,
`zclassic23 agentdiagnose`, `zclassic23 getmirrorstatus`, or the equivalent MCP
tools).

The transport can vary, but the payload should not: AI-facing status surfaces
return stable JSON objects with a `schema` or an explicit command contract, and
they must identify the running node binary with `build_commit` whenever deploy
drift would change the interpretation of state. `getmirrorstatus` follows this
rule so an operator can distinguish a stale runtime binary from current source
or a freshly deployed dev lane.

`getmirrorstatus` also includes `mirror_contract` (`zcl.mirror_status.v1`).
Agents should prefer it over string-scraping top-level legacy fields. It names
that the mirror is advisory-only, the local consensus authority, whether the
mirror is running/reachable, whether lag is known, whether zclassic23 and
zclassicd are at the same height with the same hash, whether a blocker is truly
active, and whether operator action is required. A
same-height same-hash mirror with no active blocker is healthy even if an older
runtime or cached field once mentioned a transient `hash-disagreement`.

`agentliveness` (`zcl.agent_liveness.v1`) is the one-call runtime liveness
rollup. Its default mode is compact: it composes `current_runtime_lane`,
observed runtime listeners, compact `runtime_availability`,
`supervisor_state`, and `background_quality_status` count fields, then adds
direct fields such as `overall_liveness`, `agent_next_action`,
`liveness_summary`, `recommended_drilldowns`, `omitted_sections`, and
`full_mode_command`. Use `agentliveness full` or
`zcl_agent_liveness(mode="full")` when you need embedded
`runtime_availability.methods[]`, supervisor `domains` / `root_orphans`, or
background quality `lanes[]`. Its top-level `schema`, `method`,
`native_command`, `mcp_tool`, and `contract_source` fields are populated from
`agent_contracts.def`, not handwritten in the controller.
`runtime_services` is only the producer process' in-process listener state;
when a native static command has successfully probed a target lane over RPC,
`runtime_availability` marks `target_rpc_reachable=true`,
`liveness_summary.effective_runtime_reachable=true`, and
`liveness_summary.effective_runtime_scope="target_rpc_probe"`. This keeps
`producer_runtime_state="inactive_or_static_probe"` from being mistaken for an
offline target lane. Use it when deciding whether a lane is alive, stalled,
missing quality verdicts, or merely being inspected from a static binary outside
a running node.

`agentdiagnose` (`zcl.agent_diagnose.v1`) is the bounded "what should I look
at next?" packet. The default mode is compact: it uses cheap `agent` status,
peer lifecycle incident summary fields, advisory `getmirrorstatus`, and
explicit drill-down commands while staying inside `zcl.first_call_contract.v1`.
`agentdiagnose full` / `zcl_agent_diagnose(mode="full")` expands the packet with
embedded `agent`, bounded `healthcheck`, `peer_incidents`, mirror, and timeline
objects. Its
top-level `schema`, `method`, `native_command`, `mcp_tool`, and
`contract_source` fields come from `agent_contracts.def`. The response
duplicates the decision fields agents need most (`verdict`, `safe_next_action`,
`gap`, `peer_count`,
`peer_incident_count`, `duplicate_host_group_count`,
`peer_host_incident_count`, `peer_host_count_returned`, `peer_primary_host`,
`peer_primary_host_issue_class`, `peer_primary_host_next_action`,
`peer_primary_host_direction`, `peer_primary_host_mixed_direction`,
`peer_primary_host_bootstrap_readiness`,
`peer_primary_host_fast_sync_readiness`,
`peer_bootstrap_readiness`, `peer_fast_sync_readiness`,
`peer_bootstrap_blocker`, `peer_fast_sync_blocker`,
`peer_primary_host_incident_score`, `peer_primary_host_issue`,
`peer_incident_severity`, `peer_stability_blocker`,
`peer_material_incident_count`, `peer_material_group_count`,
`peer_informational_incident_count`, `peer_incident_summary`,
`mirror_status`, `mirror_severity`, `mirror_advisory_only`, and
`mirror_operator_action_required`) and
marks skipped lower-priority sections as `partial_result=true` instead of
hanging. Use it before raw logs when the node is behaving oddly but still
answers RPC.
`zclassic23 agentdiagnose`, `zclassic23 agentdiagnose brief`, and
`zcl_agent_diagnose(mode="brief")` all use the compact first-call shape: it
preserves the top-level verdict,
safe next action, peer/mirror counts, compact `peer_primary_host_issue`,
findings, and recommended commands while omitting the embedded `agent`,
`healthcheck`, `peer_incidents`, `mirror`, and `timeline` drill-down objects.
The response includes `detail_mode`,
`embedded_drilldowns=false`, `omitted_sections`, and a `full_mode_command`
that expands to `zclassic23 agentdiagnose full`.
For chain status, `agentdiagnose` follows the same `zcl.agent_readiness.v1`
contract as `agent`: a small non-material tip gap remains healthy when
`chain_serving_ready=true`. It also echoes `chain_readiness_status` and
`height_contract_status` so agents can tell normal lookahead/minor lag from a
real serving blocker without re-parsing the embedded `agent` object.
For peer readiness, `peer_bootstrap_readiness=ready` means at least one current
handshaked peer has `NODE_NETWORK` and an advertised height. Any other value
sets `peer_bootstrap_blocker=true`, escalates the peer finding to attention,
and makes `safe_next_action=inspect_peer_lifecycle_bootstrap_readiness` unless
a higher-priority chain/operator issue exists. `peer_fast_sync_blocker=true`
with bootstrap ready means the node has usable peers but no current zclassic23
fast-sync-capable peer; that is `info` unless material reconnect/duplicate
incidents are also present.
Raw peer `advertised_height` values are telemetry, not proof of bootstrap
usefulness. Treat `advertised_height_trust=trusted` /
`advertised_height_trusted=true` as the compact signal that the height came
from a current handshaked `NODE_NETWORK` peer; `untrusted_missing_NODE_NETWORK`
means the peer reported a height but did not advertise the service bit needed
for bootstrap. Host-level `advertised_height_trust` can also be
`split_bootstrap_capabilities` when one current connection has the service bit
and another has the height, but no single connection has both.
Public REST `GET /api/v1/peers` is the lightweight collection for explorers
and simple agents. Each row keeps the persisted projection flag as
`projection_is_zcl23` and adds live lifecycle evidence when available:
`live_peer`, `live_zclassic23`, `bootstrap_readiness`, `fast_sync_useful`,
`live_lifecycle`, and `zclassic23_verified_by`. The row-level `is_zcl23` is
the resolved verdict (`projection_is_zcl23 || live_zclassic23`), and
`zclassic23_projection_stale=true` means the live handshake has already proven
ZClassic23 support even though the persisted peer projection has not refreshed.
`peer_incident_severity=info` means the raw peer lifecycle view still has
forensic detail, but there is no duplicate/reconnect storm and the overall
verdict can remain healthy. `peer_incident_severity=attention` means the
incident view found material duplicate, reconnect, timeout, reject, or no-peer
signals and `safe_next_action` will point at a host-specific
`peer_primary_host_next_action` when the compact host scorer can name one;
otherwise it falls back to the generic peer-lifecycle drill-down.
For a peer-only packet without the rest of `agentdiagnose`, use native
`zclassic23 peerincidents` or MCP `zcl_peer_incidents`; the generic fallback is
`zclassic23 dumpstate peer_lifecycle incidents` / `zcl_state
subsystem=peer_lifecycle key=incidents`. The first-class response schema is
`zcl.peer_incidents.v1` and is bounded by design: it returns aggregate incident
counts, `primary_host_issue`, top per-host incidents, duplicate host groups,
last disconnect reasons, service flags, advertised heights, and bootstrap /
fast-sync usefulness without requiring log scraping. The native/MCP controller
adds registry-owned `method`, `native_command`, `mcp_tool`, and
`contract_source` fields, and the full-mode embedded
`agentdiagnose.peer_incidents` object carries the same identity fields.
Host-level objects expose
`direction`, `mixed_direction`, `current_open_direction`,
`current_handshaked_direction`, and per-direction current open/handshaked
counts so reconnect storms that mix inbound ephemeral ports with outbound
dial attempts are visible without expanding the full peer list.
Host-level objects also split raw advertised-height counts into
`handshaked_trusted_advertised_height_connections` and
`handshaked_untrusted_advertised_height_connections`, and expose
`advertised_height_trust` so agents can see whether the host has a usable
bootstrap height, only an untrusted height report, or split capabilities across
multiple current connections.
Its top-level `bootstrap_readiness`, `fast_sync_readiness`,
`bootstrap_blocked`, `fast_sync_blocked`, `incident_severity`,
`stability_blocker`, and `safe_next_action` are the no-jq verdict fields.
`bootstrap_readiness` uses `ready`, `no_current_open_connection`,
`no_current_handshaked_connection`, or `no_bootstrap_useful_peer`.
`fast_sync_readiness` is `ready`, `no_zclassic23_fast_sync_peer`, or the active
bootstrap blocker. `incident_severity` only scores incident pressure;
`stability_blocker` also becomes true for bootstrap blockers.
Likewise, `mirror_severity=info` means the advisory zclassicd mirror is worth
watching but is not a local-node stability blocker; only
`mirror_operator_action_required=true` escalates the overall diagnosis.
`tools/z mirror --json` mirrors that interpretation only as a compatibility
shim for long-running older binaries: it clears a `hash-disagreement` active
blocker only when the payload itself proves equal height and equal hash.

`anchorstatus` (`zcl.anchor_mint_status.v1`) is the offline/static status
packet for the sovereign UTXO anchor producer. Run
`zclassic23 -datadir=/path/to/anchor-datadir anchorstatus` against an
anchor-mint datadir to read `progress.kv` directly without cookies, jq, Python,
or a running service RPC. It reports the anchor checkpoint, stage cursors,
durable coins frontier, validated backlog, stale header rows above the anchor,
snapshot presence, a compact `summary`, and `agent_next_action`.

`proofbundle` (`zcl.operator_proof_bundle.v1`) is the read-only evidence
artifact command for agents. Run
`zclassic23 proofbundle /path/to/anchor-datadir` or
`zcl_proof_bundle(anchor_datadir="/path/to/anchor-datadir")` to collect live
`agent`, `milestone` / `zcl.mvp_operator_proofs.v1`, `refold`,
`anchorstatus`, `agentlanes`, and `agentdevstatus` payloads into one JSON
object. Redirect stdout when a durable artifact is needed; the command itself
does not mutate services or write files.

`zclassic23 agentinterface` / `zcl_agent_interface` is the machine-readable
entry point for that rule. In addition to the human summary, it emits a
top-level `build_commit`, a `runtime_identity` block for the binary that
produced the interface contract, a `capabilities[]` matrix that names each
first-class agent operation, its schema, and its native/MCP/REST transport,
including the `zcl.mirror_status.v1` mirror lag/blocker contract, plus
a `machine_contract` block declaring that payloads are JSON objects with
required `schema`, `api_version`, and `status` fields. Those nested shapes are
versioned as `zcl.agent_runtime_identity.v1`, `zcl.agent_capability.v1`, and
`zcl.agent_machine_contract.v1`. Future operator APIs should extend that matrix
before adding new wrapper behavior.

`capabilities[]` is registry-owned: canonical rows and append-only v1 aliases
are emitted through `agent_push_contract_capability_json()` from
`agent_contracts.def`, so `method`, `schema`, `native`, `mcp`, `rest`, and
`contract_source` do not drift between `agentinterface`, `agentcontracts`,
MCP, and native CLI help. Alias rows set `registry_alias=true` and name their
`canonical_capability`; keep compatibility aliases there instead of repeating
schema/tool strings in `agent_interface_controller.c`.

`agentinterface`, `agentops`, `agentlanes`, and full-mode `agentliveness` also include
`runtime_availability` (`zcl.agent_runtime_availability.v1`). Native static
first-call commands are
produced by the binary you just ran, but that producer may be newer than the
target lane still serving RPC. The availability block separates those facts:
`producer_build_commit` names the local producer, `availability_scope` says
whether the answer is producer-only or a target RPC probe, and each
`methods[]` entry reports `target_runtime_support` plus the
`probe_params_json` used by the bounded availability probe. If a target returns
`unsupported_method_not_found`, do not call that method on that lane; deploy and
smoke the dev lane first or use methods marked `supported`. If the probe says
`no_cookie` or `connect_failed`, treat target support as unknown instead of
inferring it from source files or local CLI output.

When a native first-call method from the C-owned registry is sent to a target
lane that returns JSON-RPC `-32601`, the CLI prints
`zcl.cli_rpc_diagnostic.v1` instead of a bare "Method not found" line. That
diagnostic includes `producer_build_commit`, `target_datadir`,
`target_rpcport`, `probable_cause`, and the same `runtime_availability` block,
so agents can distinguish a stale runtime lane from a missing source route.
This is expected during dev/canonical skew; it is not evidence that the new
method is absent from the producer binary.

No Python is required to consume the preferred agent API. Contract assembly,
status interpretation, changed-file test mapping, and deploy safety decisions
belong in C under `app/controllers/src/agent_controller.c`,
`app/controllers/src/agent_contracts_controller.c`, and
`app/controllers/src/agent_interface_controller.c`; compact
operator/architecture answers that should not require `jq` belong in
`app/controllers/src/agent_ops_controller.c`, then get exposed through
MCP/native/REST. Registry-backed command groupings for `agentmap` and
telemetry live in `app/controllers/src/agent_contract_registry.c`
(`g_agent_command_surfaces`); `agentinterface` capability rows are emitted
from `app/controllers/src/agent_contract_capability_registry.c`, while
registry-backed nested schema rows live in
`app/controllers/src/agent_contract_schema_registry.c`
(`g_agent_schema_surfaces`). Registry-backed review objects such as
`agentops.architecture_review` live in
`app/controllers/src/agent_contract_review_registry.c`
(`g_agent_review_surfaces`); keep only composite/local commands in the
controller that assembles the response. Ranked `agentops` planning lists
(`api_gaps` and `top_next_work`) live beside them in `g_agent_work_surfaces`
so the compact command center is data-owned by registries, not by response
assembly code. `agentcontracts` also exposes `contract_summary`, generated
from the same registries, so agents can read native/MCP/REST declaration
counts plus review/schema-surface counts without scanning the full contract
array. `registry_source` names the contract/command registry and
`review_registry_source` / `schema_registry_source` name the review and nested
schema registries, so API clients can pin reviews to the owning C tables. MCP
tests verify every non-empty registry `mcp` tool resolves in the live router.

## Command Center

For architecture and operator planning, the first call is `zcl_agent_ops`
through MCP, or `zclassic23 agentops` from the native binary. It returns
`zcl.agent_ops.v1`: direct decision fields, `no_jq_required=true`, current lane
and runtime build contracts, background quality summary fields, named
drill-down commands, direct scalar pointers such as `peer_incidents_command` /
`peer_incidents_tool`, API gaps, the registry-owned `workflow` for the expected
agent loop, and the top next architecture work list. `api_ux` names the
preferred drill-down primitives (`zcl_state`, `zcl_node_log`, `zcl_sql`, and
`zcl_timeline`) so agents can keep one-off diagnostics simple before adding new
typed routes. Do not pipe larger discovery payloads through `jq` to build this
answer by hand; add a field to `agentops` when an agent repeatedly needs the
same decision.

The first-call operator status view is `zcl_agent` through MCP, or
`zclassic23 status` / `zclassic23 agent` through the native binary. It returns the stable status,
the running binary `build_commit`, height/gap, peer summary, active blockers,
next action, and recommended drill-down tools. The compact packet also includes
`security_posture` (`zcl.security_posture.v1`), which separates liveness from
security review: `bootstrap_model` names whether the node is still on a
borrowed-but-consensus-bound snapshot path, `snapshot_full_validation_complete`
names whether that seed has been independently validated from history, and
`nullifier_history_complete` / `nullifier_activation_cursor` name whether old
shielded nullifiers are fully covered or need the shielded-history backfill /
from-genesis refold path. A node can be `serving=true` while
`security_posture.review_required=true`; treat that as security backlog, not a
peer/liveness outage. The compact packet also includes
`provable_tip_published` and `indexer.block_source_status_cached` so agents can
tell when the first-call fast path intentionally avoided blocking projection
reads during startup or catch-up; use `zcl_status`, `zcl_state`, or
`getmirrorstatus` for heavier detail instead of making `agent` wait on SQLite.
The same fast path uses cached mirror state and an internal optional-detail
budget. If that budget is already spent, `agent.partial_result=true` and
`first_call.partial_result=true`; the core status/readiness/height/peer/mirror
fields remain present, while lower-priority detail such as `resources` and
`restart_watchdog` can be deferred. Follow `first_call.full_mode_command`
(`zclassic23 healthcheck`) when the omitted detail matters.
`zcl_operator_summary` remains a longer MCP compatibility aggregate with raw
drill-down payloads, not the canonical bounded status contract.
`runtime_build` (`zcl.runtime_build.v1`), which compares the running binary's
commit against the deploy-installed expected commit
(`ZCL_AGENT_EXPECT_BUILD_COMMIT`, written by `make deploy` / `make deploy-dev`).
It also includes `operator_latch` (`zcl.operator_latch.v1`), `conditions`
(`zcl.condition_engine_summary.v1`), and `mirror_contract`
(`zcl.mirror_status.v1`). `operator_latch.active` names whether an
`EV_OPERATOR_NEEDED` page is still latched; `operator_action_required` is the
machine decision agents should use before interrupting work. Mirror-only stale
hash-disagreement latches can be marked
`suppressed_by_mirror_contract=true` when the mirror contract proves there is
no active advisory blocker. `conditions` gives cheap active/unresolved counts
and points to `zcl_state subsystem=condition_engine` for the full registered
condition list, attempts, thresholds, and detail. The MCP
`zcl_operator_summary` `mirror` object exposes `contract_trusted`,
`blocker_active`, and `operator_action_required`; agents should key on those
booleans before any legacy `blocker` string. When `getmirrorstatus` includes
`mirror_contract.blocker_active=false`, `zcl_operator_summary` suppresses stale
top-level mirror blocker strings.

`healthcheck` is also a first-call API, but its default shape is bounded:
`zclassic23 healthcheck` returns `zcl.healthcheck.v1` with
`result_completeness="bounded"`, `partial_result=true`, cached fast fields, and
an embedded `agent` summary. Use `zclassic23 healthcheck full` or
`{"mode":"full"}` only when a diagnostic needs the heavier chain evidence,
condition-engine, and chain-advance dumps. Agents should rely on the explicit
`result_completeness` field instead of assuming the default response is the
full evidence tree.
The bounded response duplicates the most important height/readiness fields at
top level and under `checks`: `readiness_status`, `chain_serving_ready`,
`height_contract_status`, `normal_lookahead`, and `sync_fsm_at_tip`. In bounded
mode, `checks.synced=true` means the served frontier is current or in normal
one-block lookahead and the chain surface is serving; `sync_fsm_at_tip` is the
raw legacy sync-state predicate for callers that specifically need it.

`agent`, default `healthcheck`, `agentliveness`, and `agentdiagnose` also include
`first_call` (`zcl.first_call_contract.v1`): `api`,
`result_completeness`, `partial_result`, `source`, `budget_ms`,
`elapsed_ms`, and `budget_exceeded`. `agent` and `agentliveness` use that
budget to return valid partial JSON responses instead of continuing into
optional detail work after their first-call budget is spent. Default
`agentliveness` sets `partial_result=true` because it intentionally omits
the high-cardinality method, supervisor-domain, and quality-lane arrays; the
full mode restores them for drilldown work.
Default bounded `healthcheck` still preserves top-level deployment contract
fields (`consensus_authority`, `candidate_source`, `candidate_trust`) so
deploy verification and first-call clients do not need to parse nested
diagnostic objects for the node authority posture.

`milestone` (`zcl.milestone_status.v1`) is the v1 progress view, not a second
health authority. Its `live` block is derived from the bounded
`zcl.public_status.v1` agent summary when available and names that with
`live.source="agent_cached_summary"` / `live.source_schema="zcl.public_status.v1"`.
When the agent packet is available, `live.agent_fields_complete=true` means the
milestone live fields are copied from that same agent contract and are regression
tested against a direct `/api/v1/agent` read. If any required first-call field is
missing, milestone sets `live.source="agent_cached_summary_with_fallbacks"`,
`live.agent_fields_complete=false`, and names `live.fallback_source`; if the
agent contract is unavailable entirely, it falls back to the older node-health
snapshot and says so in `live.source`.
The same response embeds `operator_proofs`
(`zcl.mvp_operator_proofs.v1`): one row per MVP criterion with
`proof_command`, `ci_gate`, `proof_scope`, `primary_blocker`,
`local_dependency_required`, and `ci_regression_protected`. This is the
machine-readable version of the `docs/MVP.md` proof table: agents use it to
choose the next MRS-moving command without scraping docs. It does not change
the score; `mvp_readiness_score` still counts only accepted full operator
proofs.

The bounded agent packet may read a cached chain-advance decision for speed, but
it must reject internally inconsistent projection cache data. A stale decision
such as `projection_height=0`, empty `projection_state`, and zero lag while the
served/tip frontier is far above zero is surfaced as
`indexer.projection_state="cached_status_inconsistent"` with
`block_source_status_stale`; it must not lower top-level `indexed_height` or
make milestone/health disagree about a node that is effectively at tip.

When `runtime_build.stale=true`, the node is still useful to observe but its
behavior predates the expected deployed source; use the lane safety contract
before deciding whether to deploy dev or request an operator-gated canonical
restart. If no expected commit is installed, `runtime_build.freshness` is
`unknown`, not a proof that the binary is current. The same packet includes
reducer frontier telemetry, download queue/in-flight/throughput counters,
recent error state, and precise download age fields:
`download.oldest_in_flight_age_seconds`,
`download.oldest_in_flight_height`,
`download.oldest_in_flight_peer_id`,
`download.overdue_in_flight`, and
`download.in_flight_peer_count`. It also reports
`download.queue_peer_avoid_count` and
`download.queue_peer_avoid_max_seconds` when timed-out block bodies are queued
for immediate retry by other peers while temporarily avoiding the peer that just
failed that exact hash. It reports `download.catchup_stalled` when a
lagging node has active download work but the served frontier has not advanced
for the stall window. It reports `download.dispatch_idle`,
`download.dispatch_stalled`, and `download.dispatch_idle_seconds` when queued
block work is waiting but no block request is currently in flight, so the AI
operator can tell dispatch starvation from ordinary catch-up. Assignment-loop
telemetry is in `download.assign_attempts`, `download.assign_successes`,
`download.assign_zero_results`, and the `download.last_assign_*` fields; the
string `download.last_assign_result` names the last planner result
(`assigned`, `no_queue`, `peer_window_full`, `global_window_full`, `no_slot`,
etc.). `download.dispatch_wakes` counts gap-fill wakeups of the connman message
dispatcher after queue refill or timeout requeue work. Stale in-flight requests
are swept both by the peer send loop and by the supervised gap-fill worker; when
the worker requeues timed-out blocks it wakes the C-native peer dispatcher, so a
connected-but-silent peer cannot own block requests forever. If gap-fill sees
queued body work with zero in-flight requests on a duplicate/no-op refill pass,
it also wakes the dispatcher; queued work is never allowed to sit idle because a
previous wake raced the message-handler wait. `download.message_cycles`,
`download.message_send_calls`, `download.message_process_calls`,
`download.message_recv_ready`, `download.message_idle_waits`, and
`download.message_wakes` expose the connman loop itself. The peer message cycle
sends outbound work before inbound processing, then yields from inbound
processing after a bounded batch (`ZCL_MSG_PROCESS_MAX_PER_CYCLE`) so the
outbound send/assignment phase keeps running even under a large receive backlog
or slow local reducer work. Use `zcl_status`
for the larger health packet, `zcl_state_catalog` to discover every state
subsystem and its accepted keys, cost, freshness, owner file, safety level,
tests, and drill-down commands, `zcl_state` for subsystem internals,
`zcl_timeline` for category-filtered structured event history with bounded
server-side filters, semantic summaries, log-reference hints, type/peer counts,
recommended drill-downs, and seq cursors,
`zcl_node_log` for bounded log search, `zcl_sql` for SELECT-only database
inspection, and `zcl_events` for the raw recent event ring.

Every new subsystem that has runtime state should expose it through the
diagnostics registry and become reachable through `zcl_state`. The same
registry feeds `zclassic23 statecatalog` / `zcl_state_catalog`
(`zcl.state_catalog.v1`), so agents can discover the subsystem name,
description, owner shape/file, expected cost, freshness, accepted keys, safety
level, focused tests, and drill-down commands without source search. Expensive
development proof state belongs in a named background quality lane with a JSON
verdict, not in an untracked terminal scrollback.

For "what happened?" questions, start with `zclassic23 timeline sync 50` or
`zcl_timeline(category="sync", count=50, since_secs=3600)` and switch category
as needed (`peer`, `message`, `chain`, `validation`, `condition`, `oracle`,
`mirror`, `boot`, `db`, `wallet`, `disk`, `mcp`, `net`). Use object/MCP filters
for `since_secs`, `since_us`, `peer`, `height`, `reducer_stage`, `condition`,
`deploy`, and `lane`; the node scans a bounded retained window server-side
instead of making agents pipe raw events through `jq`. The response is
`zcl.timeline.v1`, includes `head_seq`, and returns `events[].seq` so agents can
tie a timeline slice to later drill-downs. The same payload includes
`semantic_summary`, `type_counts`, `peer_counts`, `log_references`,
`safe_next_action`, and `recommended_drilldowns` so common root-cause triage
stays server-side.

For peer churn, reconnect, or duplicate-entry reports, start with
`zclassic23 peerincidents` / `zcl_peer_incidents`; use
`zclassic23 dumpstate peer_lifecycle incidents` /
`zcl_state(subsystem="peer_lifecycle", key="incidents")` only as the generic
fallback. If a running target predates the direct `peerincidents` RPC but still
supports `dumpstate`, the native CLI and MCP tool automatically normalize
`dumpstate peer_lifecycle incidents` back into the same
`zcl.peer_incidents.v1` contract and add
`compatibility_fallback=true` plus `compatibility_source` /
`compatibility_reason` fields. The first-class command returns bounded
`zcl.peer_incidents.v1` JSON
with `primary_host_issue`, `top_host_incidents`, flat `primary_issue_host` /
`primary_issue_class` / `primary_issue_next_action` fields, `top_incidents`,
`duplicate_host_groups`, reconnect counts, last reasons,
direction, handshake age, advertised height, service summaries, bootstrap
readiness/usefulness, fast-sync readiness/usefulness, advertised-height trust,
current handshaked service/height/ZClassic23 counts, trusted/untrusted
advertised-height host counts, host `direction` / `mixed_direction`,
current open/handshaked direction summaries, reconnect cadence (`last_reconnect_interval_secs` and
host min/max/latest reconnect intervals), current open/handshaked connection
counts, top-level bootstrap/fast-sync blocker verdicts, and separate
duplicate-host counts for historical entries versus live open/handshaked
duplicates. `primary_host_issue` and `top_host_incidents` are
the no-jq path for reconnect storms where many peer rows share one host; they
collapse the storm to one host-level `issue_class`, `incident_score`,
`next_action`, direction summary, readiness reason, and reconnect cadence.
`host_incident_count` is the total scored host count; `host_count_returned` is
the bounded number included in `top_host_incidents`.
Use the full
`dumpstate peer_lifecycle` only after the compact incident view identifies the
host or peer worth drilling into.

## Operator Lane

`zclassic23 agent`, REST `GET /api/v1/agent`, and MCP
`zcl_operator_summary` include `operator_lane`
(`zcl.operator_lane.v1`). The lane is normally declared by the node's own boot
context (`-operator-lane=canonical|soak|dev|test|copy`, or
`ZCL_OPERATOR_LANE`) and reports the lane name, runtime profile, datadir, ports,
and machine-readable restart policy. If a systemd override drops the explicit
lane flag, the agent API can still classify a first-class lane by an exact match
against the C-owned topology registry (`datadir + rpcport + p2p_port`). That
fallback reports `lane_source="inferred_exact_topology"`,
`lane_declared=false`, and `lane_inferred=true`; explicit declarations report
`lane_source="declared_boot_context"`.

Use it to distinguish the long-running canonical node from the pinned soak lane
and the restartable development lane. Tooling should branch on the booleans
`canonical`, `soak_evidence`, `development`, and `ephemeral`, not on systemd
unit names or comments.

The lane object also includes `deployment_safety`
(`zcl.operator_deployment_safety.v1`). Automation must read this nested contract
before any restart or binary deployment:

- `automation_restart_ok` / `automation_deploy_ok` — whether an agent loop may
  do the action without an operator confirmation.
- `requires_operator_confirmation` and `guard_env` — the explicit stop sign for
  canonical and soak lanes.
- `protects_public_endpoint`, `counts_for_soak_hours`, and
  `isolated_from_canonical_datadir` — why the lane exists.
- `preferred_deploy_target` and `safe_default_action` — where fresh code should
  go when the current lane must not be disturbed.

Canonical defaults to observe-only, soak defaults to preserving the evidence
window, and dev defaults to `deploy_dev_lane`.

For fast deploy loops and MCP summaries, the most important lane-safety values
are also duplicated as compact top-level fields on the public agent packet:
`operator_lane_name`, `automation_restart_ok`, `automation_deploy_ok`,
`requires_operator_confirmation`, `preferred_deploy_target`, and
`safe_default_action`. The lane source fields are duplicated there too:
`operator_lane_source`, `operator_lane_declared`, and
`operator_lane_inferred`. They are emitted by the same C helper as the nested
lane object. A canonical packet should therefore say
`operator_lane_name="canonical"`, `automation_restart_ok=false`,
`automation_deploy_ok=false`, and
`safe_default_action="observe_only_or_use_dev_lane"`.

`zclassic23 agentlanes` / `zcl_agent_lanes` returns the native
`zcl.agent_lanes.v1` topology contract for all first-class operator lanes:
canonical (`zclassic23`, `~/.zclassic-c23`, RPC 18232 / P2P 8033), soak
(`zclassic23-soak`, `~/.zclassic-c23-soak`, RPC 18242 / P2P 8043), and dev
(`zcl23-dev`, `~/.zclassic-c23-dev`, RPC 18252 / P2P 8053). It also embeds
`current_runtime_lane`, the same `zcl.operator_lane.v1` object used by
`zclassic23 agent`, plus `current_runtime_services`
(`zcl.agent_runtime_services.v1`). The lane object's port fields are the
configured boot intent; `current_runtime_services` separates those configured
ports from observed in-process listeners (`rpc_running`, `https_running`,
`https_bound_port`, `fs_running`, `fs_bound_port`). Use this C-native topology
before deciding where to deploy or restart; use `make lane-health` /
`tools/scripts/lane_health.sh --json` only as the external systemd/RPC
readiness probe that verifies the declared lanes are actually running.

Each lane also embeds `recovery_state`
(`zcl.operator_lane_recovery.v1`), a cheap C-native view over boot-owned
recovery sentinels that affect deploy safety. Today it reports the lane
datadir, `auto_reindex_request` marker path, marker presence, well-formed
status, anchor/count, `auto_reindex_pending`, `auto_reindex_terminal`,
`auto_reindex_malformed`, `deploy_blocker`, `deploy_blocker_reason`,
`explicit_recovery_env`, and `safe_next_action`. A pending marker is a deploy
blocker because a routine restart would consume it and enter a long pre-RPC
`-reindex-chainstate` rebuild. A terminal marker is reported as
`status="terminal_auto_reindex"` but is not pending; it means the bounded
budget already paged the operator and the marker should only be cleared after
repair is proven.

The same public status contract includes `resources`
(`zcl.node_resources.v1`) with cheap process-level RSS, RSS warning threshold,
Linux cgroup memory usage/limits when available, memory pressure (`ok`,
`watch`, `warn`, or `unknown`), `memory_pressure_detail`, pressure basis,
uptime, and source. Cgroup/systemd pressure is preferred over raw RSS because
canonical can have a large steady RSS while still running comfortably below its
service memory guardrails. When cgroup v2 `memory.stat` is available, the node
also reports anon/file/kernel buckets, reclaimable bytes, working set, and
working-set percentages against `memory.high` / `memory.max`. With a cgroup
`memory.high` limit, `watch` starts at 85% of total cgroup usage. `warn` starts
at 95% only when the unreclaimable working set is also near the limit; a high
total caused mostly by reclaimable file cache remains `watch` with
`memory_pressure_detail="cgroup_reclaimable_cache_high"`. Without
`memory.high`, the max limit gives an 80% watch and 90% warn fallback. This is
the first-call place to notice a lane that is still serving but approaching
memory pressure, before reaching for shell-only systemd probes.

The same contract also includes `restart_watchdog`
(`zcl.restart_watchdog.v1`). It is the cheap first-call view over the chain tip
watchdog's bounded restart memory: whether the watchdog is registered, whether
an autonomous no-progress recycle happened in the current episode, the stuck
height anchoring that episode, restart count, restart budget remaining, and the
deep drill-down command (`zclassic23 dumpstate chain_tip_watchdog`). A recent
controlled liveness recycle appears as
`last_restart_autonomous=true`,
`last_restart_reason="no_progress_tip_stall"`, and
`status="restart_budget_burning"`, which lets agents distinguish a deliberate
watchdog remedy from a crash or manual soak rebaseline without scraping
`node.log`.

The top-level status always reports exact `gap`, `index_gap`, and
`target_height`, but it treats normal live-tip churn of up to 10 blocks as
serving-health-compatible. Larger gaps still become `chain_gap` /
`download_queue_idle` and can set `operator_needed`.

The same packet includes `height_contract` (`zcl.height_contract.v1`) so agents
do not confuse height surfaces. Top-level `height`, `served_height`,
`getblockcount`, `getblockchaininfo.blocks`, and P2P `start_height` are the
served/provable reducer frontier H*. `active_tip_height` is the internal
sync-window lookahead tip and may be one block above H* while `tip_finalize`
waits for a canonical successor. In that case `height_contract.status` is
`normal_lookahead`, not a peer-connectivity failure. `lagging` means the served
gap is material and should be diagnosed with `getsyncdiag` / `dumpstate
reducer_frontier`.

The same response includes `readiness` (`zcl.agent_readiness.v1`) so agents do
not have to infer operational safety from the top-level status string alone.
`chain_serving_ready=true` means the chain surface is serving with peers, no
operator-needed latch, no material tip gap, and no material reducer log-head
gap. `index_projection_ready=false` means explorer/projection reads may be
stale even though the chain is still serving. In that case the top-level status
can remain `degraded` with `primary_blocker="projection_lag"`, while
`readiness.status="serving_projection_deferred"` and
`agent_work_ready=true` tell automation that development and diagnostics can
continue without treating the node as down.

The same readiness booleans are also duplicated as compact top-level fields:
`readiness_status`, `chain_serving_ready`, `index_projection_ready`,
`agent_work_ready`, `operator_action_required`, and
`readiness_next_action`. They are computed by the same C helper that builds the
nested readiness object, so shell deploy guards and MCP callers can read one
flat key without re-parsing nested JSON.
The dev-lane deploy probe declares `AGENT READY` from `agent_work_ready=true`,
so projection lag stays visible without blocking unrelated development work.

`make deploy` is guarded by `tools/deploy_guard.sh canonical-deploy`. The guard
calls the running node's C-native `agentdeployguard` RPC
(`zcl.agent_deploy_guard.v1`) when available and falls back to the systemd
`-operator-lane=` flag only for older running binaries. An active canonical
lane is refused by default; set `ZCL_DEPLOY_ALLOW_CANONICAL=1` only for a
deliberate canonical restart window. Development builds should normally use
`make deploy-dev`. The deploy-guard response also carries the same compact
lane-safety fields (`operator_lane_name`, `automation_restart_ok`,
`automation_deploy_ok`, `requires_operator_confirmation`,
`preferred_deploy_target`, and `safe_default_action`) so a refusal can be
handled without scraping nested JSON. The native no-RPC form is intentionally
safe by default: `zclassic23 agentdeployguard deploy` refuses until a lane is
declared. Generic `deploy` and `restart` evaluate the current runtime lane.
Explicit lane actions evaluate their named target from the same C topology
registry used by `agentlanes`: `canonical-deploy` / `canonical-restart`
always evaluate `target_lane_name="canonical"` and refuse without an operator
window, while `deploy-dev` / `restart-dev` evaluate `target_lane_name="dev"`
even when the inspected service is canonical. Use
`zclassic23 agentdeployguard deploy-dev` when checking the documented dev-lane
deploy path from automation. The native command prints the same JSON every
time and sets its process exit status from the JSON `exit_code`: `0` means the
guard allowed the action; nonzero means refuse. Scripts therefore do not need
`jq` just to decide whether to continue. `make check-agent-cli` runs the
hermetic executable regression for that contract: it creates isolated HOME
trees, proves a clean `deploy-dev` returns exit `0`, then plants a dev-lane
`auto_reindex_request` and proves the same command returns exit `1`. If the dev
lane has a pending
`auto_reindex_request`, the guard refuses with
`reason="pending_auto_reindex_requires_explicit_recovery_boot"`,
`recovery_deploy_blocker=true`, `recovery_status="pending_auto_reindex"`, and
`explicit_recovery_env="ZCL_DEV_ALLOW_AUTO_REINDEX_DEPLOY"`. Set that
environment variable only for a deliberate recovery boot, or prove the marker
stale before clearing it. `ZCL_OPERATOR_LANE=dev zclassic23 agentdeployguard
deploy` and `zclassic23 agentdeployguard -operator-lane=dev deploy` remain
supported for checking a process already declared as dev.

`make lane-health` is the read-only redundancy check for the canonical, soak,
and dev lanes. `make lane-recover LANE=dev` or `LANE=soak` plans a bounded
noncanonical repair as `zcl.lane_recovery_plan.v1`; add
`ZCL_LANE_RECOVERY_APPLY=1` only when it is acceptable to restart that
noncanonical unit. The recovery tool refuses `live`, `canonical`, and `main`,
never writes the canonical datadir, and uses the optional
`ZCL_LANE_SNAPSHOT_LOADER_FLAG` systemd hook in the dev/soak units instead of
rewriting long command lines. If the snapshot is ahead of the lane's served
height, the plan imports headers first with the documented
`--importblockindex $HOME/.zclassic` step; `--import-headers` /
`ZCL_LANE_RECOVERY_IMPORT_HEADERS=1` forces that step for a loader-equipped
noncanonical lane that is still pre-RPC. Forced import is skipped when the
selected snapshot is not newer than the lane height, unless
`ZCL_LANE_RECOVERY_ALLOW_STALE_HEADER_IMPORT=1` is set. The import is bounded by
`ZCL_LANE_RECOVERY_IMPORT_TIMEOUT` (default 1200 seconds).

When lane RPC is reachable, `make lane-health --json` also consumes the native
`agent` contract and exposes `agent_rpc_state`, `agent_build_commit`,
`agent_contract_trusted`, `agent_status`, `agent_operator_needed`,
`agent_primary_blocker`, `agent_next`, `agent_validation_pack_ok`, and
`agent_validation_pack_detail`. Agent calls use
`ZCL_LANE_AGENT_TIMEOUT` (default 10 seconds), separate from the cheap generic
RPC timeout. `agent_rpc_state` is `ok`, `timeout`, `error`, `empty`, or
`not_called`; a timeout becomes `status=warn` with `reason=agent_timeout`
unless a stronger condition-engine operator page is already active. A current
native `agent` contract
(`agent_contract_trusted=true`, which requires `build_commit`) with `blocked`
or `operator_needed=true` makes the lane `status=fail` and clears role
readiness, even if basic peer/height/listener checks still look fine. Older
compact agent responses are still printed but do not override lane status by
themselves; lane health falls back to `condition_engine` operator-needed pages
for those runtimes. This keeps the shell lane summary subordinate to the
C-native API without letting stale wrapper-era responses hide or invent a
validation-pack hold.

`make deploy-dev` is a transaction over an immutable content-addressed binary
generation, not an overwrite of the running executable. It stages and
preflights the candidate while the old process still serves, then flips the
atomic `current` link and verifies the exact `/proc` executable, build identity,
RPC, native `agent`, operator snapshot, MCP catalog, and MCP self-test. Failure
quarantines the candidate, restores `last-good`, restarts once, and verifies the
recovery. Every attempt saves `~/.zclassic-c23-dev/agent-deploy.json`
(`zcl.agent_dev_deploy.v1`) with candidate/current/running/last-good identities,
activation-lock state, rollback status, rejected generations, build and probe
status, failure capsule, and any pending auto-reindex marker. The activator and
unit are hard-bound to the dev service, datadir, and ports; canonical and soak
targets are rejected.
`make agent-dev-status` / `zclassic23 agentdevstatus` /
`zcl_agent_dev_status` expose the same restart hazard before deploy as
`deploy_blocker`, `deploy_blocker_reason`, `explicit_recovery_env`, and
`auto_reindex_stale_candidate`. The same response starts with the explicit
`worker_lane` contract (`role=worker`, `mutation_policy=noncanonical_dev_only`,
and `canonical_guard=never_touches_live_or_soak`), so a healthy dev RPC cannot
hide an `auto_reindex_request` or blur the dev lane into canonical/soak.
For a stale candidate, `make agent-clear-stale-dev-reindex` archives the marker
only after the dev RPC is serving at or above the marker anchor and the dev
agent contract is not blocked; it does not restart or mutate canonical/soak.

## Bootstrap Service Status

Use REST `GET /api/v1/bootstrap`, MCP `zcl_bootstrapstatus`, or raw RPC
`bootstrapstatus` before claiming a zclassic23 node is helping fresh peers
bootstrap. Compatibility alias: `GET /api/v1/bootstrapstatus`. The response is
versioned as `zcl.bootstrap_status.v1` and separates two surfaces:

- `legacy_p2p_bootstrap`: ordinary full-node serving over `version`,
  `getheaders`, `getdata`, `getaddr`, and related P2P messages. This is the
  path used by zclassicd when its beta6 snapshot bootstrap is disabled with
  `-bootstrap=0`, or after its snapshot stage has completed.
- `beta6_snapshot_bootstrap`: the zclassicd v2.1.2-beta6 fast-bootstrap
  snapshot protocol. A compatible server must advertise `NODE_BOOTSTRAP`
  (`1 << 24`) and answer `getbsman/bsman`, `getbschk/bschk`,
  `getbspman/bspman`, and `getbspchk/bspchk`. zclassic23 must not advertise
  that bit until the matching C service is implemented.

The key booleans are `serving_p2p_bootstrap`,
`serving_addr_bootstrap`, `serving_snapshot_bootstrap`,
`zclassic23_fast_sync_compatible`, `zclassicd_beta6_p2p_compatible`, and
`zclassicd_beta6_fast_bootstrap_compatible`. `blockers[]` names missing
requirements such as `not_listening`, `provable_tip_not_published`, or
`beta6_NODE_BOOTSTRAP_not_advertised`.

For a fresh zclassic23 node, consume `readiness`,
`fresh_node_next_action`, and `zclassic23_bootstrap`
(`zcl.bootstrap.zclassic23.v1`) before using peer gossip or ZNAM endpoint
records. That object names whether this node is preferred for fresh zclassic23
bootstrap, the `NODE_ZCL23` fast-sync service bit, the direct-P2P-first route
preference, the ZNAM service-record schema to use for onion fallback, and the
ordered `fresh_node_flow`. The intended UX is: connect to the direct P2P
endpoint when `serving=true`; if direct reachability fails, resolve a
`zcl.names.show.v1` service directory, pick an onion endpoint from
`zcl.names.service_record.v1`, then validate all downloaded data against
normal ZClassic L1 consensus.

The same response also includes `snapshot_loader` (`zcl.snapshot_loader.v1`),
the binary-owned recovery contract for the node's own fast-start bundle:
datadir, highest `utxo-seed-<h>.snapshot`, seed height, matching
`block_index.bin`, failed marker, active `-load-snapshot-at-own-height` path,
and `recovery_hint` (`loader_active`,
`restart_with_load_snapshot_at_own_height`, `install_tip_seed_snapshot`, etc.).
Its nested `authority` object (`zcl.snapshot_loader_authority.v1`) reports the
durable progress-store side of the proof: whether `coins_kv` is a proven local
authority, the `coins_applied_height`, the current reducer H*, whether the coin
set covers H*, whether the fast-rebuild authority surface is ready, and whether
the self-folded sovereignty marker is present. A node can have a bootable
snapshot bundle but still report `fast_rebuild_authority_ready=false` until the
loader/reindex epilogue has seeded `coins_kv`, cursors, and `utxo_sha3`.
Operational scripts should consume this versioned C API instead of scraping
systemd command lines whenever the node RPC is reachable.

## Build loop

This is a C23 project, so the edit loop should compile only what changed.

- `make dev-watch` is the public save-driven loop. It batches a quiet save,
  captures the same `zcl.agent_fast_plan.v1` impact routing used by
  `agentimpact`, performs focused verification, and writes exactly one durable
  `zcl.dev_cycle.v1` verdict. `MODE=auto` chooses the smallest proven-safe path;
  `MODE=hotswap` requires exact manifest eligibility plus the running dev
  node's authenticated `dev_hotswap` bridge. Its default
  `tools/dev/hotswap-running-dev.sh` transport reserves exit 69 for bridge
  unavailability, which permits auto fallback to reload; generation validation
  or probe failure remains a rejected cycle. `MODE=reload` activates an
  immutable binary generation;
  `MODE=stage` builds and preflights without restart; and `MODE=check` never
  activates. Mixed changes, headers, stateful code, missing transport, and any
  unproven dependency/quiescence contract are `reload_required`. Use
  `make dev-watch-once` for an explicit changed-file batch and
  `make dev-watch-selftest` for the hermetic watcher contract.
- Every watch attempt atomically refreshes
  `~/.local/state/zclassic23-dev/latest-cycle.json` and stores its immutable
  cycle record below `cycles/`. The record names changed files, impact-rule
  hits and mapped tests, selected path and reason, phase timings,
  candidate/running/last-good generations, probes, rollback result, failure
  capsule, and one executable `agent_next_action`. A heartbeat lets
  `zcl.agent_dev_status.v1` distinguish an idle watcher from a dead one.
- Process activation is serialized by a nonblocking lock under
  `~/.local/lib/zclassic23-dev/`. Candidate preflight cannot disturb the running
  process. A failed warm activation restores and verifies `last-good`; a known
  bad generation is quarantined instead of being fed to an unbounded restart
  loop. `ZCL_DEV_DEPLOY_BUILD=strict make deploy-dev` exercises the same
  isolated transaction with a production-flag candidate; it does not replace
  strict compile, consensus, full-suite, reproducibility, or real-chain gates.
- `make agent-loop` is the manual one-shot AI/operator edit loop. It runs the
  cache-aware `make fast-ci` checks; set `ZCL_AGENT_LOOP_BIN=1` to also link
  `build/bin/zclassic23-dev`, `ZCL_AGENT_LOOP_DEPLOY=stage` to stage the
  dev-lane binary for the next restart without stopping the service, or
  `ZCL_AGENT_LOOP_DEPLOY=dev` to run the fast transactional dev-lane reload.
- `make agent-plan` is the read-only fast-lane decision packet
  (`zcl.agent_fast_plan.v1`). It reports changed files, selected focused tests,
  unmapped code changes, the changed-compile plan, green-input cache hit/miss,
  dev-lane stage/deploy commands, and the MCP one-shot shortcuts.
- `make immutable-history-canaries` runs the fast real-chain consensus KATs:
  the h=478544 125,811-byte canonical transaction fixture
  (`domain_consensus_tx_structural`) plus `consensus_parity`. Use it whenever a
  bounded consensus predicate changes before paying the heavier
  `make replay-canary-anchor` / `make replay-canary-genesis` gates.
- `make build-only` compiles all node objects without linking. It uses
  `build/obj` plus header depfiles (`-MMD -MP` and included `.d` files), so
  unchanged translation units keep their existing `.o` files and changed
  headers recompile their dependents.
- `make fast-changed-compile` is the cheapest guarded edit check. It compiles
  changed node `.c` files directly into `build/dev-obj/`, and after the dev
  object graph is warmed it compiles direct depfile dependents for narrow
  `.h`/`.def` edits. It falls back to `make fast-compile` for templates,
  Makefile changes, removed files, unwarmed depfiles, or broad edits.
- `make fast-compile` is the cheapest no-link edit-loop compile check. It uses
  the non-LTO dev object tree (`build/dev-obj`) and skips the final executable
  link, so it is the right first command for "did this C change compile?".
- `make fast-rebuild` builds the local non-LTO node binary and is the preferred
  edit-loop rebuild target. It is an alias for `make dev-bin`, with a clearer
  name for agents and operators.
- `make dev-bin` builds `build/bin/zclassic23-dev` from cached objects under
  `build/dev-obj`. It links without LTO, keeps symbols, defaults most code to
  `ZCL_DEV_OPT=-Og`, and keeps hot consensus/crypto/script/validation buckets
  at `ZCL_DEV_HOT_OPT=-O2`. `ZCL_DEV_LINKER` auto-selects `mold` or `ld.lld`
  when present and can be set empty to force the platform linker. This binary
  is for local agent/API iteration, not deploy or release.
- `make agent-dev-status` is the no-build dev-lane status command. It reports
  the lane's explicit `worker_lane` contract (`role=worker`,
  `mutation_policy=noncanonical_dev_only`, safe status/deploy/stage/recover
  commands, and the guard that it never touches live or soak),
  the source and installed dev binaries, `zcl23-dev` linger service and RPC
  state, current/running/last-good/staged generations, exact-running-identity
  match, activation lock, rejected generations, rollback availability, saved
  deploy state, current `zcl.dev_cycle.v1`, watcher heartbeat, latency-SLO and
  background-quality freshness, auto-reindex state, deploy blocker/reason, and
  the next safe action. Use `make agent-dev-status ARGS=--json` for
  `zcl.agent_dev_status.v1`;
  use `zclassic23 agentdevstatus` or MCP `zcl_agent_dev_status` for the
  first-class native/MCP contract.
- `make agent-clear-stale-dev-reindex` archives a proven-stale dev-lane
  `auto_reindex_request` after the dev RPC serves at or above the marker anchor.
  It does not restart the lane and never touches canonical or soak.
- `make agent-doctor` is the no-build combined development check. It reports
  build binary identity, dev-lane status, the embedded `zcl.agent_fast_plan.v1`
  fast-lane decision, recent focused-test failure hints, dirty-file count, MCP
  shortcuts, and a single next safe command. Use `ARGS=--json` for
  `zcl.agent_doctor.v1`.
- `make agent-stage-dev` builds and preflights an immutable candidate, then
  moves only the `staged` generation link. It does not stop the current service
  or change `current`/`last-good`.
- `make agent-index` atomically generates root `compile_commands.json` from a
  dry-run of the actual `DEV_OBJS` recipes. It keeps the real C23 flags,
  generated headers, compiler/cache wrapper, output object, and target-specific
  normal `-Og` versus hot consensus/crypto/script/validation `-O2` profile.
  Hash/freshness metadata lives under `.cache/zcl-agent-index/`. clangd is an
  optional consumer; generation and freshness reporting work without it.
- `make dev-loop-bench` writes `zcl.dev_loop_bench.v1` with configuration,
  source/host identity, per-case raw millisecond samples, failures, p50/p95,
  and separate hot-swap/reload SLO verdicts. Its default cases cannot activate
  a service: hot-swap and reload remain `not_measured` until the operator opts
  in explicitly. A missed p95 stays a `miss` in the artifact.
- `zcl.hotswap_manifest.v2` currently admits only stateless MCP route sets. A
  load validates schema/host ABI, capabilities, build/source identities, exact
  input hash, mapped tests/probes, stateless state schema, and quiescence before
  generation code runs. It stages all route replacements, runs the generation
  self-test, and publishes one immutable resident router snapshot; any failure
  publishes zero replacements. REST, diagnostics, services, models, storage,
  events, conditions, supervisors, wallet/network/crypto state, reducers,
  consensus, and process/bootstrap ownership remain `reload_required`.
  Successful generations stay mapped so in-flight calls finish against their
  original code. Inspect provenance and rejection detail through
  `zcl_state(subsystem="hotswap")`, schema `zcl.hotswap_generation.v2`.
- A hot-swap is process-local and ephemeral. It disappears on restart. After a
  successful watch-mode hot-swap, an asynchronous `fast-rebuild` converges the
  source-tree binary and preflights an immutable `staged` generation without
  changing `current`; durable activation still requires the next transactional
  process reload. In a dev build on the exact `~/.zclassic-c23-dev` lane, the
  authenticated JSON-RPC methods `dev_hotswap` and `dev_mcp_call` dispatch
  through the running process's resident MCP router. `dev_mcp_call` refuses
  destructive tools; `dev_hotswap` is the dedicated narrow mutation bridge.
  Release, canonical, and soak nodes do not register either method. The normal
  persistent operation is `make hotswap FILES=tools/mcp/controllers/app_controller.c PROBE=zcl_name_list`;
  it requires an already-running isolated dev node and
  never starts or restarts one. The native dev-loop equivalent is
  `zclassic23-dev dev change apply --input='{"files":[...]}'`; a direct legacy
  `mcpcall zcl_agent_hotswap` still affects only that short-lived helper
  process. Watch mode uses
  `tools/dev/hotswap-running-dev.sh` by default. Only its exit 69 means the
  persistent RPC transport is unavailable and allows `MODE=auto` to reload;
  ABI, capability, source/build/hash, self-test, commit, and probe failures stay
  visible and do not silently reload. Use `make hotswap-sim` for the focused
  deterministic simulated-network proof; `make sim-fast` remains the broader
  checked-in scenario and seeded replay suite.
- `make agent-mcp-call TOOL=<tool>` is the legacy fresh source-tree MCP smoke
  path (removed in zero-MCP W3; prefer native commands like `zclassic23 status`
  / `zclassic23 dumpstate <subsystem>`). It refreshes `build/bin/zclassic23-dev`
  before dispatch. Use `make agent-mcp-call-hot TOOL=<tool>` when the existing
  source-tree dev binary is good enough, and `make agent-mcp-call-dev
  TOOL=<tool>` for the installed `zcl23-dev` linger lane. Set
  `ZCL_AGENT_MCP_BUILD=0`,
  `ZCL_AGENT_BIN=...`, or `ZCL_AGENT_MCP_ARGS='-datadir=... -rpcport=...'`
  for custom no-build terminal probes.
- `make t-fast ONLY=<group>` uses `build/test-obj` and
  `build/bin/test_parallel_fast`, a cached non-LTO test harness for hot-path
  focused tests.
- `make fast-ci` runs `git diff --check`, shell syntax checks, `lint-fast`,
  the changed compile gate, focused tests inferred from changed files, and a
  native linger-service probe when the service is available. Repeated identical
  green inputs hit `.cache/zcl-agent-fast-ci/` and skip repeated
  lint/build/focused tests while still refreshing the live probe. The live probe trusts the
  native `zcl.public_status.v1` health contract instead of duplicating height
  gap policy in shell, and prints compact status JSON when it fails.
- Focused test routing is DRY: both native `zclassic23 agentimpact` and
  `tools/agent_fast_ci.sh` read
  `app/controllers/include/controllers/agent_impact_rules.def`. Add a rule
  there first, then verify `agentimpact` reports `shared_rule_hits > 0`.
- `make fast-ci` auto-selects `sccache cc`, then `ccache cc`, then `cc`.
  Override with `ZCL_FAST_CC='ccache cc'`. Use `ZCL_FAST_JOBS=N`,
  `ZCL_FAST_COMPILE=dev` to force full `fast-compile`,
  `ZCL_FAST_COMPILE=strict` to replace the changed compile gate with strict
  `build-only`, `ZCL_FAST_CHANGED_COMPILE_LIMIT=N`,
  `ZCL_FAST_CHANGED_FILES_ONLY=1` when an explicit changed-file list is exact,
  `ZCL_FAST_TESTS=group[,group]`,
  `ZCL_FAST_STRICT_TESTS=1`, and `ZCL_FAST_LIVE=0` as needed.
  Use `ZCL_FAST_CACHE=0` to force a rerun,
  `ZCL_FAST_CACHE_RESET=1` to clear the green-input cache, or
  `ZCL_FAST_CACHE_DIR=...` to move it.
- Normal Makefile compile/link recipes also auto-wrap `CC` with `sccache` when
  installed, otherwise `ccache`. Set `ZCL_USE_CCACHE=0` to force a direct
  compiler call.

Before pushing `main`, the tracked pre-push hook computes the exact
`origin/main..HEAD` changed-file set, rejects non-`main` remote refs, and runs
`make pre-push-ci`. That command uses cached `make t-fast ONLY=<group>` tests
selected by `tools/agent_fast_ci.sh`, plus `ZCL_FAST_COMPILE=strict` so the
compile gate is `make build-only` for compiler and `-Werror` coverage; it does
not rerun the full suite when the changed files only require narrower coverage.
It also sets `ZCL_FAST_LIVE=0`, so an already-running
node condition is visible through telemetry but does not block a code push. Set
`ZCL_FAST_STRICT_TESTS=1` when a change needs strict whole-harness focused
tests. Full-suite, fuzz, and coverage evidence belongs to the background quality lanes: install them with
`make install-quality-linger` and inspect them with `make quality-linger-status`.
Status JSON is written under `~/.local/state/zclassic23-quality`. The native
`zclassic23 agentbuild` / `zcl_agent_build` response also embeds
`recommended_loop` (`zcl.agent_build_loop.v1`) with the cheapest command for
each intent (`agent-plan`, `agent-loop`, `fast-changed-compile`, `fast-compile`,
`fast-rebuild`, `agent-index`, `dev-loop-bench`,
`immutable-history-canaries`, focused `t-fast`, and `pre-push-ci`),
`dev_node_binary` (`make dev-bin`, `build/bin/zclassic23-dev`, hot-path
optimization buckets, and release/deploy boundary), `indexing`
(`zcl.agent_index_runtime.v1`, compilation-database presence/hash/freshness and
optional clangd status), `dev_loop_benchmark` (`zcl.dev_loop_bench.v1`, latest
artifact/SLO status), plus
`immutable_history_canaries` (`zcl.immutable_history_canaries.v1`, the pinned
h=478544 fixture, fast command, and replay gate commands) plus
`background_quality_status` (`zcl.background_quality_runtime.v1`), a C-native
reader for those status files. It reports the resolved state/status directory,
one entry each for `fuzz`, `coverage`, and `tests`, whether each lane verdict
file exists, whether it parsed as JSON, and the latest parsed
`zcl.background_quality_lane.v1` payload when present. Each lane also carries
`expected_commit`, `latest_commit`, `commit_matches_expected`, and
`commit_freshness` (`current`, `stale`, `unknown`, or `no_verdict`). Treat
`background_quality_stale` as proof debt: a passed fuzz/test/coverage verdict
from an older commit is useful history, not evidence for the running build.
Agents should read that field first and use `make quality-linger-status` when
they need systemd timer logs or human-formatted service output.

## Remote Node Updates

Use `tools/scripts/remote_node_update.sh <ssh-host>` or
`make remote-node-update ZCL_REMOTE_HOST=<ssh-host>` to keep remote node
worktrees on `main` and their compile caches warm. The contract schema is
`zcl.remote_node_update.v1`; `zclassic23 agentbuild` / `zcl_agent_build`
exposes the same command under `remote_node_update`.

The default is intentionally observe-only:

- `ZCL_REMOTE_DRY_RUN=1` prints the host, branch, current `HEAD`,
  `origin/main`, target systemd unit, and the planned action.
- The remote checkout must be `main`; `origin/main` is the only accepted
  remote ref; updates use `git merge --ff-only origin/main`.
- Tracked local changes on the remote refuse the run unless
  `ZCL_REMOTE_ALLOW_DIRTY=1` is set after review.
- `ZCL_REMOTE_RESTART=0` by default; restarts require explicit opt-in and run
  through `tools/deploy_guard.sh` / `zcl.agent_deploy_guard.v1`.
- No Python or `jq` is required. Pass `--json`, set `ZCL_REMOTE_JSON=1`, or
  run `make remote-node-update-json` for one JSON object per host; operational
  logs move to stderr.
- Non-dry-run builds preflight cold-build prerequisites before fast-forwarding.
  LevelDB accepts either `cmake` or the direct C++11 fallback, so warm remote
  nodes do not need package-manager access just to rebuild that archive. The
  remote preflight also requires a C++ driver because the Makefile uses it to
  locate libstdc++ for the final `cc` link.
  Failures return the same `zcl.remote_node_update.v1` JSON shape with
  `status:"error"` and an `error` field.

Common commands:

```bash
tools/scripts/remote_node_update.sh rhett@205.209.104.118
tools/scripts/remote_node_update.sh --json rhett@205.209.104.118
make remote-node-update-json ZCL_REMOTE_HOST=rhett@205.209.104.118
ZCL_REMOTE_DRY_RUN=0 ZCL_REMOTE_BUILD=fast-rebuild tools/scripts/remote_node_update.sh rhett@205.209.104.118
ZCL_REMOTE_DRY_RUN=0 ZCL_REMOTE_BUILD=release ZCL_REMOTE_INSTALL_BIN=$HOME/bin/zclassic23 tools/scripts/remote_node_update.sh rhett@205.209.104.118
ZCL_REMOTE_DRY_RUN=0 ZCL_REMOTE_BUILD=release ZCL_REMOTE_INSTALL_BIN=$HOME/bin/zclassic23 ZCL_REMOTE_RESTART=1 ZCL_REMOTE_UNIT=zclassic23-test.service tools/scripts/remote_node_update.sh rhett@205.209.104.118
```

For a node to keep itself warm without giving it restart authority, run
`make install-self-update-linger`. It installs the example timer from
`deploy/examples/zclassic23-self-update.timer`, which runs
`remote_node_update.sh self` daily with `ZCL_REMOTE_BUILD=fast-rebuild`,
`ZCL_REMOTE_DRY_RUN=0`, and `ZCL_REMOTE_RESTART=0`. Check it with
`make self-update-status`. Promote a node from warm-build to install/restart
only with a reviewed systemd drop-in that sets `ZCL_REMOTE_BUILD=release`,
`ZCL_REMOTE_INSTALL_BIN=...`, and `ZCL_REMOTE_RESTART=1`.

For a long-running remote zclassic23 test node, run
`make install-remote-test-node-linger`. It installs
`deploy/examples/zclassic23-remote-test-node.service` as
`zclassic23-test.service`, creates `~/.zclassic23-test`, and creates
`~/.config/zclassic23/remote-test.env` only if it does not already exist.
The service uses the remote test ports (`18033` P2P, `18233` RPC), appends
logs to `~/.zclassic23-test/node.log`, and carries the soak resource budget
(`MemoryHigh=24G`, `CPUWeight=30`, `IOWeight=30`) so a long bootstrap is not
throttled at the 12G plateau. Its env example sets
`ZCL_LANE_SNAPSHOT_LOADER_FLAG=-load-snapshot-at-own-height`, matching the
soak-node fast-bootstrap hook so remote nodes do not spend days replaying from
genesis when a consensus-bound loader is available. Dedicated remote hosts can
add a reviewed `MemoryMax=32G` drop-in; the committed template leaves the hard
cap out so the repo-wide systemd memory-budget lint does not double-count
mutually exclusive example nodes. Check it with `make remote-test-node-status`;
edit the env file before starting or restarting the service.

## Background proof lanes

The background lanes keep expensive proof work running without blocking every
push or AI edit loop.

- `zclassic23-fuzz.timer` runs `tools/scripts/background_quality_lane.sh fuzz`
  hourly. Default duration is 900 seconds per fuzzer; override with
  `ZCL_FUZZ_DURATION`.
- `zclassic23-coverage.timer` runs
  `tools/scripts/background_quality_lane.sh coverage` weekly.
- `zclassic23-test-suite.timer` runs
  `tools/scripts/background_quality_lane.sh tests` hourly.
- `make quality-linger-status` prints timer status plus the latest
  `zcl.background_quality_status.v1` JSON verdict.
- `zclassic23 agentbuild` exposes the same lane verdict files through
  `background_quality_status` (`zcl.background_quality_runtime.v1`) without
  invoking shell wrappers or Python.

## Reproducible build proof

Use `make ci-reproducible` for byte identity. It runs
`tools/scripts/check_reproducible_build.sh`, which builds twice in isolated
`BUILD_DIR`s using `tools/scripts/repro_build_vars.sh`, then compares the
binaries with `cmp`.

The reproducible profile pins `SOURCE_DATE_EPOCH` to the HEAD commit time
unless overridden, forces portable `-march=x86-64-v3`, and disables the linker
build id with `-Wl,--build-id=none`.

## Rule

Do not add new operator logic to `tools/z`. Add native JSON once, expose it via
MCP or REST if needed, document the schema, and cover it with focused tests.
Do not require Python for the preferred operator API path.
