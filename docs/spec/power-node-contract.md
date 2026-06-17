# Power-Node Architecture Contract

The stable architecture and observability contract for a ZClassic23 power
node: one C23 process serving chain state, application services, onion-hosted
surfaces, ZClassicDNS name resolution, and MCP tools. A contract, not an
implementation plan.

## Scope

Authoritative references:

- `CLAUDE.md` — current power-node feature set (full chain node, embedded Tor
  hidden service, fast sync, MVC web framework, ZSLP, Sapling, games, store,
  MCP server).
- `app/services/README.md` — service-layer boundary.
- `lib/event/include/event/event.h` — event taxonomy.
- `tools/mcp/controllers/app_controller.c` — app MCP surface (names,
  messaging, market, swaps).
- `tools/mcp/controllers/{ops,chain,net,wallet}_controller.c` — remaining MCP
  domains.

Consensus, P2P wire parsing, rendering, and application orchestration stay
separated. Services may coordinate work and expose status, but must not define
consensus rules, parse raw P2P messages, render HTML/JSON, or dispatch routes.

## node_state_api

`node_state_api` is the process-wide persisted state contract backed by the
SQLite `node_state(key, value)` table: durable node cursors and small binary
checkpoints — schema version, best-block pointers, wallet scan cursors, UTXO
commitments, MMR/MMB state, snapshot metadata, Sapling tree state.

Invariants:

- Keys are named, versioned contracts. New keys must document owner,
  byte format, writer, reader, and rollback behavior before production use.
- `node_state` stores state snapshots and cursors only. It must not become an
  append-only event log, cache table, or opaque dumping ground for large
  domain records.
- Writes that advance chain, wallet, or snapshot state must be atomic with the
  data they point at. A persisted cursor must never reference unavailable block,
  UTXO, wallet, or Sapling data.
- Readers must treat missing keys as a recoverable cold-start condition unless
  the owning subsystem explicitly marks the key mandatory for that phase.
- MCP and controller reads may report `node_state` values, but mutation remains
  behind the owning subsystem APIs.

Concrete files: `db/schema.sql`, `app/models/src/database.c`,
`lib/wallet/src/wallet_sqlite.c`, `lib/coins/src/utxo_commitment.c`,
`lib/storage/src/coins_view_sqlite.c`, `config/src/boot.c`.

## service_registry

`service_registry` is the boot-time ownership contract for services under
`app/services`. Services are long-lived orchestration units: sync workflows,
snapshot lifecycle, wallet indexing/rescan, health/status aggregation,
explorer query aggregation, peer policy.

Invariants:

- Services own orchestration and status snapshots, not consensus validity,
  raw network message formats, or view rendering.
- Service startup must be idempotent. Registering or initializing the same
  service twice must leave one coherent owner, not duplicate workers.
- Services that run background work must expose a bounded status snapshot for
  `zcl_status`, `zcl_kpi`, `zcl_health`, or domain-specific MCP tools.
- Service failures must be observable through events and/or health status.
  Silent loops are contract violations.
- Service APIs must state thread ownership for mutable state and must avoid
  returning borrowed pointers across worker-thread boundaries.

Concrete files: `app/services/README.md`, `config/include/config/runtime.h`,
`config/src/boot_services.c`, `app/services/src/node_health_service.c`,
`app/services/src/snapshot_sync_service.c`,
`app/conditions/src/sync_state_stuck.c`.

## Onion Gateway

The onion gateway is the Tor-hidden-service ingress for the same surfaces
served locally. With `-tor`, the embedded Tor runtime publishes an ephemeral
`.onion` service and routes requests through dynhost into the normal
controller stack.

Invariants:

- Onion ingress must call the same controller/business logic as local HTTP or
  internal routes. It must not fork a second app implementation.
- Onion status must be visible through health/status surfaces, especially
  `zcl_status` and node UI status views.
- Onion request handling must preserve normal authentication, permission, and
  destructive-action rules.
- Directory publishing must expose only intended discovery metadata: onion
  address, optional clearnet endpoint, height, and version.
- The gateway must not bypass consensus, wallet, database, or MCP middleware.

Concrete files: `lib/net/src/onion_service.c`,
`lib/net/src/tor_integration.c`, `lib/net/src/https_server.c`,
`app/controllers/src/network_controller.c`, `CLAUDE.md`.

## ZClassicDNS

`ZClassicDNS` is the human-readable naming contract built on ZCL Names (ZNAM)
— see `CLAUDE.md` ZNAM for the feature surface. Names are on-chain records
resolving to `.onion`, shielded, transparent, or text/content targets. The
DNS-like layer is resolution/routing convenience; the authoritative registry
is chain data.

Invariants:

- Name registration and update commands must be represented as on-chain ZNAM
  operations. Off-chain caches may accelerate reads but cannot become
  authoritative.
- Names must retain the existing syntax contract: 1-63 lowercase
  alphanumeric-or-hyphen characters, first-come-first-served ownership.
- Resolution APIs must distinguish absent names, malformed names, and records
  that exist but lack the requested target type.
- Messaging and app routing that use names must resolve through the ZNAM
  controller/library path, not ad hoc string maps.
- Multi-coin and text/content records must remain typed. A `.onion` target must
  not be silently treated as a payment address, and payment addresses must not
  be treated as routable hosts.

Concrete files: `lib/znam/include/znam/znam.h`,
`lib/znam/src/znam.c`, `app/controllers/src/name_controller.c`,
`tools/mcp/controllers/app_controller.c`, `app/models/src/database.c`.

## MCP Surface

The MCP surface is the typed AI-agent API, divided by domain (see Scope) and
registered through controller route tables.

Invariants:

- Every MCP tool must have a stable `zcl_` name, domain label, description,
  handler, and parameter schema when it accepts input.
- Handlers must validate parameters before dispatching to node RPCs or internal
  services. On failure they must set an MCP error body and log context.
- Destructive tools must be explicit and gated by middleware policy. Read-only
  diagnostics must remain safe to call during incident response.
- Tool calls must emit `EV_MCP_REQUEST` with tool name, result code, and
  latency so `zcl_events` and MCP metrics can explain agent activity.
- The `zcl_rpc` escape hatch is not the contract for new features. New stable
  functionality should get a typed tool.

Concrete files: `tools/mcp/router.c`, `tools/mcp/middleware.c`,
`tools/mcp/metrics.c`, `tools/mcp/controllers/app_controller.c`,
`tools/mcp/controllers/ops_controller.c`,
`tools/mcp/controllers/chain_controller.c`,
`tools/mcp/controllers/net_controller.c`,
`tools/mcp/controllers/wallet_controller.c`,
`lib/test/src/test_mcp_controllers.c`.

## Permissions

`permissions` are enforced at endpoint, controller, middleware, and filesystem
boundaries. The power node exposes local, onion, RPC, and MCP surfaces, so
checks must sit close to each ingress and repeat before destructive state
changes.

Invariants:

- Read-only status, health, peer, and event surfaces may be broadly available
  unless they expose secrets or wallet-private data.
- Wallet mutation, file-market purchase/offer creation, swaps, name
  registration, peer mutation, rescans, and admin operations require explicit
  authorization or local-only policy.
- Cookie, backup, wallet, Sapling, and private-key material must use strict
  filesystem permissions and must never be returned by diagnostics.
- Onion-hosted views must not weaken local authentication or turn local-only
  actions into remote actions.
- Middleware must classify destructive MCP tools independently from tool
  descriptions so wording changes cannot alter permission policy.

Concrete files: `tools/mcp/middleware.c`,
`lib/test/src/test_mcp_middleware.c`,
`lib/test/src/test_rpc_auth_hardening.c`,
`app/controllers/include/controllers/file_controller.h`,
`app/controllers/src/wallet_controller.c`.

## Event Expectations

`event expectations` are the observability contract. The event log is the
shared explanation surface for networking, sync, validation, chain, boot,
database, model lifecycle, recovery, MCP, wallet backup, disk, mempool, and
integrity behavior. The event/projection model is canonical in
`docs/FRAMEWORK.md`.

Invariants:

- State-machine transitions must emit typed events with old state, new state,
  and reason where applicable.
- Rejections and recoverable failures must emit events with enough context to
  debug from `zcl_events` and `zcl_logtail` without attaching a debugger.
- Long-running services must emit progress or status events at bounded
  intervals and completion/failure events at terminal states.
- MCP dispatch must emit `EV_MCP_REQUEST`; crash handlers must retain recent
  events; health/KPI surfaces should derive from event-backed counters where
  practical.
- Event payloads are bounded by `EVENT_PAYLOAD_SIZE`; emit compact structured
  text rather than unbounded JSON blobs.

Concrete files: `lib/event/include/event/event.h`,
`lib/event/src/event.c`, `tools/mcp/router.c`, `tools/mcp/metrics.c`,
`tools/mcp/controllers/ops_controller.c`.

## Change Control

Changing serialized block/transaction formats, consensus constants, P2P wire
formats, or the meaning of existing `node_state_api` keys is outside this spec
row and requires coordinator review. New code should update this contract when
it adds a stable power-node surface, persistent state key, service, permission
class, MCP tool family, or event family.
