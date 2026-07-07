# Sovereign Command Center Plan

Goal: build zclassic23 into a ZCL-verified sovereign node UX: names,
services, Tor/P2P, bootstrap, CRUD APIs, and tests.

## Non-negotiable boundary

Zclassic consensus remains the base layer and authority for valid blocks,
transactions, scripts, PoW, and shielded rules. Application protocols may
interpret confirmed chain bytes, build valid transactions, store signed
off-chain objects, and expose projections, but they must not redefine block or
transaction validity.

## User-facing product model

- **Node command center:** synced height, peer health, Tor readiness, direct
  P2P reachability, bootstrap serving state, warnings, blockers, and next
  action.
- **Identity and names:** a user registers a name, attaches public service
  records, rotates keys, publishes onion/clearnet endpoints, and sees exactly
  what is public forever.
- **Private onion host:** the node exposes selected public services over the
  embedded Tor dynhost adapter and reports which services are visible.
- **Direct P2P upgrade:** nodes can rendezvous through Tor or chain-discovered
  endpoints, authenticate signed peer identity, then prefer direct P2P for low
  latency when reachable.
- **Bootstrap provider:** a node can advertise and serve hash-verified
  bootstrap manifests/snapshots so new nodes can sync faster while checking
  commitments against verified ZCL history.
- **Marketplace/content:** users publish signed listings and content hashes,
  never arbitrary files on-chain; hosting and mirroring are explicit opt-in.
- **Script contract workbench:** users construct escrow/swap/refund flows using
  only legacy-valid Zclassic script behavior, with exact script preview and
  state tracking.

## Layer model

1. **ZCL consensus:** full-node validation of legacy-compatible chain history.
2. **Chain anchors:** OP_RETURN records, standard script contracts, payments,
   commitments, revocations, service pointers, and bootstrap roots.
3. **Signed objects:** listings, service announcements, endpoint records,
   content descriptors, buy intents, route hints, and bootstrap manifests.
4. **Projections:** ActiveRecord/read-model state rebuilt from confirmed chain
   bytes plus validated signed objects.
5. **CRUD surfaces:** REST read resources, MCP/native JSON operator tools, and
   private transaction-construction commands.
6. **UX:** command-center screens and AI workflows that explain the node state
   without shell pipelines.

## Shared object contract

Every non-consensus service object should be:

- versioned
- canonically encoded
- hash-addressed
- signed where authorship matters
- size-capped
- expiry-capped where stale data is dangerous
- replay-protected by sequence, height, or revocation
- validated before storage
- stored through the model/ActiveRecord lifecycle when persisted to `node.db`
- rebuildable or explicitly marked local-only

## Protocol registry contract

`zclassic23 appprotocols`, `zcl_app_protocols`, `/api/v1/protocols`, route
contracts, and OpenAPI extensions are the machine-readable source of truth for
the application layer. Each protocol row should declare:

- base layer and consensus boundary
- chain anchor and anchor kind
- CRUD capability
- mutation authority
- object types
- UX surfaces
- projection model
- reorg model
- cryptographic model
- transport model
- privacy model
- diagnostics surface

Do not add a new protocol, REST route family, or MCP workflow without updating
this registry first.

## Architecture rules

- Keep parser/canonical-encoding code pure and testable.
- Keep controllers thin: parse, authorize, call one service, return JSON.
- Keep writes behind ActiveRecord or the documented progress.kv kernel store.
- Keep projections rebuildable and reorg-aware.
- Keep private mutation APIs separate from public read-only REST.
- Keep Tor as a transport adapter, not a business-logic dependency.
- Keep direct P2P authenticated by signed identity or chain-anchored keys.
- Keep mempool observations advisory until confirmed by chain history.

## Test gates

Each protocol needs focused tests for:

- exact byte vectors
- malformed input ignored or rejected with a typed error
- canonical encoder round trips
- signature/hash validation
- cap and expiry behavior
- projection rebuild from genesis or fixture history
- reorg disconnect/replay
- CRUD route schema
- MCP/native schema
- diagnostics state
- consensus parity guard when script or transaction construction is touched

## First implementation sequence

1. Enrich the protocol registry with object, UX, security, projection, and
   reorg fields.
2. Harden ZNAM determinism and service-record tests.
3. Define signed service announcements for onion, clearnet, bootstrap, and
   capabilities.
4. Add Tor/full-stub link mode and service exposure diagnostics.
5. Define signed marketplace/content objects before expanding routing.
6. Add bootstrap manifest commitments and serving diagnostics.
7. Build direct-P2P negotiation on top of authenticated identity and Tor
   fallback.
