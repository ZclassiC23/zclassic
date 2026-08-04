# S7.1 distributed record discovery and possession replication lane

**STATUS: IN PROGRESS**

**Worker:** `wf_zcode-s7-1-replication`  
**Branch:** `lane/zcode-s7-1-replication`  
**Base:** `origin/main` at `1c60480511c6fd13012189693da26865ae58a4c2`  
**Owner promotion:** the attached S7.1 sprint explicitly promotes this work
without authorizing deployment, live-datadir mutation, wallet spending, or a
consensus change.

## Goal

Turn S7's signed-record cache into iterative, responsible-node DHT discovery
and make replication durability depend on locally revalidated possession.
Science fetch must start from only its semantic root, discover and preserve
pointer conflicts, authenticate candidate providers through the existing
ZENDP/connman/Noise path, and admit only bytes that rederive the requested
root.

## Required slices

1. Derive a domain-separated DHT key from network, record kind, namespace and
   selected root. Use the existing `k=16`, `alpha=3`, 64-candidate lookup to
   reach the closest responsible nodes, then query signed records in bounded
   rounds until the closest set is stable, the deadline expires, or enough
   verified results have arrived. `records.v1` remains only a cache.
2. Add bounded record-discovery begin/poll/cancel capabilities. Synchronous
   provider and science wrappers must consume that same lifecycle rather than
   reading the local cache as network discovery.
3. Publish to closest eligible DHT nodes. Persist renewal intent without
   private keys, resume with a fresh online delegation after restart, honor
   expiry/backoff, and stop forwarding immediately when local policy changes.
   Failed or unreachable members of the closest 16 must admit candidates
   17–64 into the active frontier.
4. Add deterministic pagination over at most 64 records per selector. Prioritize
   distinct provider IDs and return same-sequence conflicts in a separate
   collection so a conflict flood cannot hide an honest provider.
5. Make science fetch provider-directed. Deterministically choose among
   preserved pointer conflicts; re-check sovereignty over semantic root,
   transport root, publisher ZID and service type; discover provider records;
   resolve only accepted ZENDP evidence; dial via connman; require fresh
   Noise/delegation authentication; and fall back after absence, timeout,
   corruption or lies.
6. Forbid generic caller-authored `STORAGE_ACK`. ACK creation requires the
   package store to parse and root-bind the manifest, read and hash every
   chunk, confirm complete transport-root possession, and require a local pin
   unless a future explicit policy says otherwise. `STORE_RESULT` remains a
   transport admission response, not possession evidence. Unpin, missing
   bytes, corruption or failed revalidation prevents renewal, so durability
   falls as old ACKs expire.
7. Prove the complete path with 12–16 sparse nodes. A cache-empty late joiner
   with no peer database, no publisher connection and only a science root must
   find the pointer over multiple hops, reject malicious providers, fetch and
   rederive bytes, restart and rebuild identically. The proof also covers
   pagination, renewal, ACK loss after data deletion, concurrent lookups,
   traffic caps and one-node root blocking while another succeeds.

## Allowed paths

- `lib/vcs/{include/vcs,src}/` for record keys, pagination, iterative record
  operations, publication renewal, possession verification adapters and
  replication accounting.
- the smallest existing S6 composition-root/session changes under
  `config/{include/config,src}/` and existing `lib/net/` adapters required to
  reuse ZENDP, connman and authenticated Noise sessions.
- `app/services/` science carrier code and package-store primitives that own
  complete local possession.
- `config/commands/zcode*.def`, native ZCODE command handlers and generated
  API documentation.
- focused tests, the DHT/science acceptance harnesses, impact metadata, this
  assignment, the work index and final HANDOFF evidence.

## Forbidden

- spaces, boards, doorbells, mailboxes, agents, arbitrary service execution,
  automatic execution of unknown packages, a second network stack or a
  science-specific discovery protocol
- direct routing-table/SQL shortcuts, private-key persistence or disclosure,
  global bans, weakened caps/locks/replay separation/gates
- `core/` or other consensus changes, wallet spending, deployment, canonical
  service restart, or live datadir access

## Exact verification contract

Focused born-red and regression groups must cover record-key KATs, frontier
advancement, iterative record rounds, capability ownership/cancel/expiry,
pagination/conflict separation, closest-node publication and renewal,
possession ACK revalidation, provider authentication/fallback and science
root rederivation. Final proof requires:

```text
make test-zcode-dht-acceptance
make test-science-acceptance
make zcode-dht-asan
store/market/yardsale focused regressions
make lint
make -j"$(nproc)" test-parallel TEST_PARALLEL_ARGS=--no-cache
make -j"$(nproc)"
make ci-reproducible
make repro-verify
make pre-push-ci
```

Every implementation slice is committed only after its focused tests pass.
The completion section records exact heads, commands, verdicts, binary sizes
and hashes, honest limits, and any unavailable parameter-heavy coverage.
