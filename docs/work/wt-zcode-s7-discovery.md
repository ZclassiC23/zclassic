# S7 generic provider and sovereignty discovery lane

**STATUS: IN PROGRESS (wf_zcode-s7-discovery)**

**Worker:** `wf_zcode-s7-discovery`
**Branch:** `lane/zcode-s7-discovery`
**Base:** `origin/main` at `5ec65c0b5b99e577af26cb989b3ed13ac6f545ff`
**Owner promotion:** the 2026-08-04 staged S7 sprint explicitly promotes this
work despite the older v1 queue; deployment and every live lane remain out of
scope.

## Goal

Make generic ZCODE objects discoverable and retrievable by semantic or
transport root across the authenticated S6 DHT, while keeping all admission,
storage, serving, forwarding, indexing, fetching, and execution decisions
local to each sovereign node.

## Required slices

1. Canonical signed `PROVIDER`, `POINTER`, and `STORAGE_ACK` records binding
   network genesis, namespace, roots, provider node ID, sequence, validity
   times, and delegated signer. Preserve conflicting valid records as
   equivocation evidence; never treat a provider claim as possession proof or
   emit an acknowledgement before manifest/chunk verification.
2. Bounded record storage and lookup traffic over the existing Noise/
   `zpkgswm` transport, reusing S6 sessions, replay namespaces, rate limits,
   deadlines, routing, and snapshot/unlock/relock generation rules. Reject
   malformed, oversized, cross-network, stale, replayed, or unauthorized
   records and cap them per root, peer, and node.
3. Canonical persistence with rebuildable projections. Restart reproduces
   active records and conflicts byte-for-byte; records never affect consensus.
4. One local sovereignty policy engine consulted before `DISCOVER`, `FETCH`,
   `STORE`, `INDEX`, `SERVE`, `FORWARD`, or `EXECUTE`. Support exact-root,
   publisher-ZID, and service-type rules. Unknown content is never fetched,
   served, or executed automatically; shared blocklists remain advisory and
   opt-in.
5. Typed provider/publish/pin/unpin and policy surfaces with exact schemas and
   plan/commit mutation lifecycles. Expose no keys or private policy data.
6. Close the science gap: publish the transport object, pointer, and provider;
   a fresh node given only a science root discovers, fetches through the
   existing verifier, re-derives the science root, admits it, and rebuilds its
   projection.
7. Bounded replication targeting up to eight providers. `durable` requires
   five valid acknowledgements across three declared owner groups; label this
   declared diversity, never proof of separate operators. Preserve partial
   success, expiry, restart, and dishonest-provider behavior.
8. A sparse multi-node proof covers multi-hop discovery, one lying provider,
   corrupt data, honest fallback, cold restart with identical result, and an
   exact-root local ban that blocks store/serve/forward on one node while
   another node continues successfully.

## Allowed paths

- `lib/vcs/{include/vcs,src}/` for generic record codecs, record store,
  policy-free DHT services, provider discovery, replication, and science
  carrier adapters.
- the smallest required authenticated transport/session adapters under
  `lib/net/` and S6 boot wiring under `config/{include/config,src}/`.
- `app/models/`, `app/services/`, and persistence adapters/ports needed for
  ActiveRecord policy/plan state and rebuildable projections.
- `config/commands/zcode*.def`, native ZCODE command handlers, and generated
  command documentation.
- focused tests under `lib/test/`, hermetic/daemon acceptance tools, test
  registration and impact metadata, and the smallest Makefile changes needed
  to expose exact gates.
- `docs/HANDOFF.md`, `docs/CODEBASE_MAP.md`,
  `docs/work/ZCODE_SCIENTIFIC_METAVERSE.md`, this assignment, and the work-doc
  index.

## Forbidden

- space manifests, doorbells, boards, mailboxes, agent missions, or arbitrary
  user-defined service execution
- automatic execution of unknown C23 packages or any bypass of the confined
  ZCODE executor
- a second socket stack, science-only discovery silo, direct routing-table or
  SQL shortcuts, key exposure, private-policy disclosure, or global bans
- consensus/core edits, wallet spending, deployment, live datadir/service
  mutation, or lowering/weaking any existing gate

## Execution order

1. Inspect the exact S6 service, session, persistence, science carrier, swarm,
   command, and schema seams; freeze record/policy contracts in born-red tests.
2. Land codecs plus validation and adversarial KATs.
3. Land canonical record persistence/conflict reconstruction.
4. Land bounded authenticated record traffic and generic lookup.
5. Land the single policy engine and typed plan/commit commands.
6. Land science-root-only publish/discover/fetch/admit.
7. Land replication/ack/durability accounting.
8. Land the hostile sparse daemon proof, then the full gate stack and exact
   handoff.

Each implementation unit is committed separately after its owning focused
tests pass. The worker never pushes; the orchestrator reviews, fast-forwards
`main`, runs the mandatory pre-push gate, and pushes only `origin/main`.

## Completion evidence

Pending. Completion requires exact commits, files, command schemas, caps,
adversarial verdicts, sanitizer output, S6/science/market/store/yardsale
regressions, cold uncached suite, LTO, both reproducibility proofs, mandatory
pre-push CI, explicit S8/non-goals, and a clean `origin/main` handoff.
