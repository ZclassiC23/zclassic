# S7.1 distributed record discovery and possession replication lane

**STATUS: COMPLETE — gate-proven 2026-08-05; not deployed**

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

## Completion receipt

Implementation commits, in reviewable order:

```text
c26665d63 zcode: derive record keys and advance lookup frontier
50380d52d zcode: add iterative signed record discovery
fa0719661 zcode: expose asynchronous record discovery lifecycle
a52d36344 zcode: paginate records with provider anti-crowding
45b3f9fc3 zcode: publish records and prove storage possession
fbe0cd5d7 zcode: route science fetches through authenticated providers
7bf78e82a test: prove sparse replication and renewal boundaries
59cb40eae zcode: bind science publication and provider routing roots
```

Exact evidence on `59cb40eae` plus this documentation-only receipt:

- `make test-zcode-dht-acceptance`: PASS — seven-daemon sparse iterative
  lookup, broken-nearest recovery, bounded asynchronous admission, persistence
  and autonomous cache/peer-DB-empty cold bootstrap.
- `make test-science-acceptance`: PASS — the DHT acceptance above followed by
  a two-daemon semantic-root-only POINTER/PROVIDER lookup, authenticated fetch,
  byte rederivation, cold restart and byte-identical rebuild after SQL wipe;
  stable CAS counts A=20/B=21.
- focused 12-node hermetic proof: PASS — no direct publisher or peer database,
  closest-key publication reaches an initially disconnected responsible node,
  traffic stays at or below 256, policy freezes and later resumes renewal,
  physical pinned-CAS-byte loss defeats possession and prevents ACK renewal,
  and local root blocking remains local.
- `make zcode-dht-asan`: PASS with ASan+UBSan and zero sanitizer failures or
  suppressions.
- `t-fast ONLY=adversarial`: 8/8 groups PASS; `zcode_store`: 1/1 PASS;
  `file_market`: 1/1 PASS; `yardsale`: 3/3 PASS.
- `make lint`: all 132 gates PASS. Full-program `make -j"$(nproc)"`: PASS.
- cold uncached suite: 902 registered, 893 run, 0 cached, 9 parameter-heavy
  groups gated, 0 failed, 19 explicit self-skip markers, 85.7 s on 32 workers.
- `make ci-reproducible`: two 22,174,440-byte binaries, byte-identical at
  SHA3-256 `2c5ba2b0fdf6f258031739662f199737e912b6d28ec6442c86b36a1819b5b7e4`.
- `make repro-verify`: two different-length builder paths, each 22,174,520
  bytes, byte-identical at SHA3-256
  `c238324d4c12a1605cc3dbc4ff3c596e58ac9d6640f8c490571d03a504eab0a0`.
- `make pre-push-ci`: PASS on the final committed lane before integration.

Integration receipt: while the lane gates ran, `origin/main` advanced by six
wallet/transaction commits. They merged without source conflict; the generated
API reference was regenerated from the combined 509-entry catalog. Integration
head `09770961f` then passed strict `build-only`, all 132 lint gates, and the
exact `make test-science-acceptance` target (including its seven-daemon DHT
prerequisite). Fresh integrated reproducibility receipts are:

- `make ci-reproducible`: two 25,602,728-byte binaries, byte-identical at
  SHA3-256 `2f0f08773db50719178eb16d68da81a3583fba4d919a18207c47cdbe3b425a70`.
- `make repro-verify`: two different-length builder paths, each 25,602,808
  bytes, byte-identical at SHA3-256
  `f8b599b5aa190e35dd80159fee9e728d0c9e27998eb780dd12a077665ab730fc`.

The nine parameter-heavy groups were unavailable in the cold default because
they require the opt-in parameter fixture. The 19 reported self-skips are the
suite's explicit stress/live-fixture boundaries, not hidden failures.

Honest boundary: the topology evidence is a 12-node hermetic sparse proof, a
seven-daemon DHT acceptance and a two-daemon science acceptance. It is not a
claim that one 12-daemon deployment was run. Records remain expiring evidence,
not content truth or operator-independence proof. No private key is persisted;
unknown C23 is never auto-executed. No space manifest, board, mailbox,
doorbell, agent mission, arbitrary service execution, second network stack,
consensus change, wallet spend, deployment, service restart or live-datadir
mutation occurred. Future sovereign spaces reuse this generic typed-record,
object and authenticated transport foundation.
