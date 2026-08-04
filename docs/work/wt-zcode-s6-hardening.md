# S6 production hardening and scale validation lane

**STATUS: DONE (wf_zcode-s6-hardening; not deployed)**

**Worker:** `wf_zcode-s6-hardening`
**Branch:** `lane/zcode-s6-hardening`
**Base:** `origin/main` at `07acc9ecc8f3294eb66b9e0998b836ea8bb68d59`

## Goal

Complete the final production-hardening and scale-validation pass for the S6
read-only ZCODE DHT. Preserve the existing authenticated Noise/ZID/ZENDP trust
boundary while correcting lookup state, replacement probing, replay/session
identity, cold bootstrap, public asynchronous lookup admission, lock boundaries,
and deterministic scale/memory-safety coverage.

## Allowed paths

- `lib/vcs/{include/vcs,src}/zcode_dht*`
- `config/{include/config,src}/boot_zcode_dht*`
- the smallest required reachability/session adapters under `lib/net/`
- the bounded RPC timeout/dispatch and native ZCODE network command surfaces
- `config/commands/zcode*.def`
- focused tests under `lib/test/`, test registration/impact metadata, and the
  smallest Makefile/sanitizer runner changes needed to expose required gates
- ZCODE DHT hermetic/daemon acceptance helpers under `tools/`
- `docs/HANDOFF.md`, `docs/work/ZCODE_SCIENTIFIC_METAVERSE.md`, and this file

## Required slices

1. A 64-entry deterministic candidate pool with closest-k=16 active frontier,
   in-flight preservation, full-ID deduplication, bookkeeping-preserving
   frontier churn, and last-arriving-target proof.
2. Explicit replacement-probe WAITING/IN_FLIGHT/RESPONDED/FAILED/EXPIRED
   states; transmitted-probe-only incumbent failure; fresh validation on every
   promotion/removal path; saturated lookup starvation proof.
3. Independent FIND_NODE-request/NODES-response replay namespaces; one retained
   service session per node ID; local monotonic connection serial; bounded
   unauthenticated handshake-slot expiry.
4. Autonomous cold bootstrap from persisted COLD/UNVERIFIED IDs through
   accepted chain-bound ZENDP plus connman/addrman, with dial dedupe/backoff and
   a zero-peer restart daemon proof.
5. Bounded `zcode.network.find.begin|poll|cancel` lifecycle with opaque,
   owner-bound expiring IDs; the existing `find` becomes a wrapper over it;
   eight genuinely simultaneous external callers occupy eight service slots.
6. Snapshot/unlock/external-work/relock generation validation around all DHT
   lock boundaries; deterministic fixed-size node-ID reachability index,
   invalidation and operation counters; documented lock order and stress proof.
7. Deterministic 32-node sparse model with at least 10,000 transitions and
   continuous cap/authentication/incumbent invariants; focused ASan+UBSan gate
   covering codecs, routing, service and the model with zero suppressions.
8. Preserve and pass every named focused, daemon, science, persistence, full,
   reproducibility, yardsale, store/stress and pre-push gate; update exact docs.

## Forbidden

- S7 provider/root records, STORE acknowledgements or replication
- consensus/core changes
- deployment, live datadir/service mutation, or movement of funds
- a second transport, direct routing-table/SQL shortcuts, shared acceptance
  identities, gate weakening, sanitizer suppression, or timing-only claims

## Completion evidence

All eight required slices are implemented in these independently reviewable
commits, rebased onto `origin/main` commit `22007cbd6`:

- `9f3c256d1` — claim this lane and freeze its scope.
- `a278aad7c` — retain the bounded 64-candidate pool and closest-16 frontier.
- `25ebe1730` — make replacement-probe state and deadlines explicit.
- `3694e5dcb` — isolate replay namespaces and connection/session identities.
- `545e6b2b9` — cold bootstrap, asynchronous lifecycle, lock/cache hardening,
  scale model, sanitizers, daemon proof, commands, generated docs, and the S7
  design boundary.

Exact final gates on 2026-08-04:

- `make build-only` and `make -j"$(nproc)"`: PASS, including full-program LTO.
- `make lint`: PASS, 132/132 gates.
- `make t-fast ONLY=zcode_dht`: PASS, 6 groups.
- `make zcode-dht-asan`: PASS under ASan+UBSan at `-O2`, zero suppressions;
  codecs, delegation, messaging, routing/service/lookup, and the deterministic
  32-node/12,000-transition model all pass.
- `make t-fast ONLY=ed25519`: PASS, 5,419 differential comparisons.
- `make t-fast ONLY=noise`, `ONLY=v2_transport`, `ONLY=connman`,
  `ONLY=cli_argv_strict`, `ONLY=peer_lifecycle`, `ONLY=rpc_timeout`, and
  `ONLY=rpc`: PASS (the RPC selection contains 8 groups).
- `make t-fast ONLY=persistence`: PASS, wallet and net-ban persistence.
- `make t-fast ONLY=yardsale`: PASS, 3 groups; `ONLY=zcode_store`: PASS.
- `ZCL_STRESS_TESTS=1 make t-fast ONLY=store_e2e`: PASS,
  `test_store_e2e_gate` and `test_store_e2e_shielded`.
- `make test-zcode-dht-acceptance`: PASS — seven-node sparse lookup, broken
  nearest-path recovery, true eight-caller asynchronous admission, canonical
  persistence, and autonomous cold bootstrap from no peer database.
- `make test-science-acceptance`: PASS — explicit carrier, no automatic
  carrier/execution, cold restart, and byte-identical CAS projection rebuild.
- `make -j"$(nproc)" test-parallel TEST_PARALLEL_ARGS=--no-cache`: PASS — 898
  registered, 889 run, 0 cached, 9 parameter-heavy groups gated by policy, 0
  failed, and 19 explicit self-skips (87.9 s, 32 workers).
- `make ci-reproducible`: PASS — two 21,781,128-byte binaries, identical
  SHA3-256 `e808a8cef470c3bd32b67f9d430bc6dc908ea3280584937a8316c8aca4aea3be`.
- `make repro-verify`: PASS across different-length builder paths — two
  21,781,208-byte binaries, identical SHA3-256
  `e05529ca82b1ad86a949bbc24e8e94e7c57abea2856f612afe33a2721ddb8d0f`.
- The repository pre-push hook remains mandatory; its final result is recorded
  by the policy-enforced push of this lane to `origin/main`.

Honest limits: S6 discovers and authenticates node IDs, not content providers
or service roots. Candidate dials require locally accepted chain-bound ZENDP
evidence and remain subject to local policy and connman limits. A lookup can
expire or terminate without a target; it does not manufacture reachability.
The 9 parameter-heavy full-suite groups remain opt-in, while both store stress
groups named by the acceptance contract were run explicitly and passed. No
deployment, live datadir mutation, service action, consensus/core edit, wallet
operation, or movement of funds occurred.

S7 is explicitly untouched. No provider/root record, STORE acknowledgement,
replication path, space manifest, doorbell, board, mailbox, agent mission, or
user-defined service protocol was implemented. The future design note requires
all of them to reuse the same Noise/`zpkgswm`/CAS/DHT foundation and preserves
per-node policy sovereignty; unknown packages still require explicit local
policy and the confined ZCODE executor.
