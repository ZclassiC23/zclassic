# S6 production hardening and scale validation lane

**STATUS: IN PROGRESS (wf_zcode-s6-hardening)**

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

Pending. Completion requires exact commands, verdicts, commits, limitations,
and the explicit untouched S7 boundary after all eight phases are proven.
