# S7 generic provider and sovereignty discovery lane

**STATUS: ✅ DONE — ready for orchestrator review and merge
(lane/zcode-s7-discovery; not deployed)**

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

## Completion (wf_zcode-s7-discovery, 2026-08-04)

All eight slices landed as independently reviewable commits on
`lane/zcode-s7-discovery`, based on `origin/main` `5ec65c0b5`:

- `7acb45612` freezes the S7 lane and future sovereign-space boundary.
- `12a9ef670`, `fc253808b` and `d4ccf7cda` add the signed record codec,
  canonical/conflict-preserving `records.v1` store and authenticated messages.
- `d9dccfb2f` routes record service traffic through S6; `e3e311635` adds the
  seven-action local sovereignty engine; `974a3b0cd` adds redacted policy,
  provider, publication, replication and plan/commit pin surfaces; and
  `055a30bda` replicates owned records over authenticated sessions.
- `341df2294` closes duplicate reachability requests and `32913db44` begins
  the signed, chain-anchored science acceptance identity.
- `981c40e1e` through `faf172375` harden the science/acceptance proof,
  provision real chain-anchored identities, isolate exact RPC/P2P ports and
  preserve reconnect/authentication truth. `756baafed` binds the one-day
  science pointer inside the default three-day delegation.
- `32e14befb` closes the final read-path defect found by the cold suite:
  loading an absent sovereignty policy no longer creates directories, the
  datadir non-mutation matrix owns `policy.list`, and pin tests exercise the
  exact plan/commit lifecycle.
- `3ef2c80b5` closes the independent review finding that record admission,
  indexing, forwarding and science fetch/execute were not all consulting the
  same engine. It gates all seven actions, fixes typed policy mutation input,
  binds short-lived command checks to the signed network delegation, and
  proves the explicit per-node `science` opt-in in the named acceptance.

The implementation is generic rather than science-specific. One exact
551-byte record covers PROVIDER, POINTER and STORAGE_ACK; it binds network,
namespace, roots, provider identity, sequence/window and delegated signer.
Canonical caps are 4,096 records/node, 64/root, 256/provider, eight conflicts,
eight records/frame, eight record operations and 256 records/peer. PROVIDER is
bounded to two hours; POINTER and ACK to seven days. ACK creation remains an
explicit operator action after verified possession.

The one 1,024-rule local engine decides DISCOVER/FETCH/STORE/INDEX/SERVE/
FORWARD/EXECUTE using full root, package, publisher ZID, service type or local
classification. Unknown content defaults to discovery evidence only. Advisory
rules are opt-in and local blocks never become network-wide bans. Replication
targets eight and requires five live ACKs across three declared owner groups;
the UI calls this declared diversity and denies any operator-independence
claim.

`make test-science-acceptance` passes after its prerequisite seven-node DHT
proof. B begins with only A's science root, resolves a signed pointer/provider,
fetches through the existing verifier, re-derives the root from bytes and
rebuilds identical projections after cold restart and a six-table SQL wipe
(CAS counts stable at A=20/B=21). Focused hostile tests cover malformed,
cross-network, unauthorized, replayed and expired records; equivocation;
multi-hop/broken-nearest lookup; absent/corrupt/lying candidates with honest
fallback; restart identity; partial/expired replication; and an exact-root ban
that blocks one node while an alternate succeeds.

Exact final gate receipts:

- `make test-science-acceptance`: PASS, including its prerequisite seven-node
  DHT acceptance and root-only science transfer/rebuild proof.
- Exact focused rerun of `test_simnet_perf`, `test_zcode_fetch`,
  `test_read_leaf_no_datadir_write` and `test_zcode_sovereignty_policy`: PASS;
  the detector measured clean growth 1,148 permille and injected-regression
  growth 3,526 permille.
- `make lint`: PASS, 132/132 gates (27.3 s wall).
- Full LTO `make -j"$(nproc)"`: PASS.
- Uncached `make -j"$(nproc)" test-parallel TEST_PARALLEL_ARGS=--no-cache`:
  PASS—900 registered, 891 run, 0 cached, 9 parameter-heavy groups gated,
  0 failed, 19 explicit self-skips (86.2 s, 32 workers).
- `make zcode-dht-asan`: PASS under ASan+UBSan at `-O2`, zero suppressions;
  all seven DHT groups plus standalone messages/service/lookup/model pass.
- `make ci-reproducible`: PASS—two 21,875,336-byte binaries, identical
  SHA3-256 `c8fc3c317053687e6d7b375d8c0c64afe2e0e99b5c599058439e05d51f46de94`.
- `make repro-verify`: PASS across deliberately different-length builder
  paths—two 21,875,416-byte binaries, identical SHA3-256
  `208b3218398353d382d13f9aff548ed0ad6b8bf8bff65393a5d5b024f6f27560`.
- `make pre-push-ci`: PASS on the clean committed lane—strict build-only,
  17-gate fast lint and the source-wide suite (900 registered, 891 run,
  0 cached, 9 gated, 0 failed, 19 explicit self-skips; 87.3 s).

This moves the scientific-metaverse root-discovery benchmark from an
out-of-band transport root to a positive root-only, node-to-node proof. It
unblocks S8 evidence checkpoints without changing consensus or deployment.

Honest limits and non-goals: records are expiring hints, not possession,
content truth, scientific acceptance, availability or separate-operator
proof. No space manifest, doorbell, board, mailbox, agent mission, arbitrary
service execution, REST protocol silo, consensus/core edit, wallet spend,
deployment, live datadir/service mutation or second network stack was added.
Unknown C23 still requires explicit local policy and the confined executor.
