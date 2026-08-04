# S6 iterative ZCODE lookup lane

**STATUS: ✅ DONE — ready for merge (branch lane/zcode-s6-iterative, head 0408e5d58)**

**Worker:** `wf_zcode-s6-iterative`
**Branch:** `lane/zcode-s6-iterative`
**Base:** `origin/main` at `2094e05e8`

## Goal

Replace the one-round authenticated FIND_NODE exchange with a deterministic,
fair, bounded iterative Kademlia lookup and prove it on a sparse 6–8 node
network. Harden replay/time/rotation/ancestry edges and expose privacy-bounded
lookup diagnostics. S7 provider/STORE work and deployment are out of scope.

## Allowed paths

- `lib/vcs/{include/vcs,src}/zcode_dht*`
- `config/{include/config,src}/boot_zcode_dht*`
- the smallest existing-network reachability adapter required under `lib/net/`
- ZCODE DHT focused tests and acceptance helpers under `lib/test/` and `tools/`
- test registration/impact rules required by those changes
- `docs/HANDOFF.md` and the S6 scientific acceptance row
- this assignment file

## Proof required

- deterministic k=16 / alpha=3 iteration with fair eight-lookup scheduling
- authenticated-only promotion/results and master-authorized monotonic rotation
- replay saturation, monotonic deadline, stale-candidate, and deep-ancestry tests
- sparse independent-identity multi-node acceptance with at least three rounds,
  progress, bounded traffic, alternate-path recovery, persistence/restart, and
  reauthentication
- focused gates, lint, uncached full suite, LTO, reproducibility, science,
  yardsale, store, and store-stress gates

## Forbidden

- consensus/core changes
- S7 provider records or STORE acknowledgements
- live datadir/service mutation, deploy, or live funds
- direct routing-table/SQL shortcuts in acceptance

## Completion evidence

- `make test-zcode-dht-acceptance`: PASS across seven independent sparse
  daemons, including broken-path recovery, eight concurrent callers,
  persistence, cold restart, and reauthentication
- focused DHT, Noise, v2 transport, argv, connman, peer-lifecycle, RPC,
  yardsale, store, and both opt-in store stress groups: PASS
- `make lint`: 132/132 gates PASS
- uncached `test-parallel`: 887/887 eligible groups PASS, zero cached
- full whole-program LTO build: PASS
- science acceptance lifecycle: PASS, including package/blob carrier and
  byte-identical rebuild after direct projection wipe
- `make ci-reproducible`: byte-identical SHA3-256
  `3a28a407c6c5c56277833e739580ab6dcc192239e6567b0ba6fa9f3ace5b1e2a`
- `make repro-verify`: different-path builders byte-identical at SHA3-256
  `25d7943eca11d89afe8f0e3bd611eadc339c0db55f2aae1d6cd94c6d2f11b180`

S7 provider/root records, STORE acknowledgements, replication, live funds,
deployment, and live datadir mutation remain out of scope and untouched.

## Completion (wf_zcode-s6-iterative, 2026-08-04)

### Summary

S6 now performs a genuine bounded iterative Kademlia lookup: deterministic
`k=16` shortlists, `alpha=3` probes, explicit candidate states, fair scheduling
across eight lookups, stable/target/timeout/no-result completion, and
privacy-bounded diagnostics. Only Noise-authenticated, delegation-authorized,
chain-bound peers can enter results; address-free NODES responses can request a
bounded reachability attempt without becoming authenticated routing state.

The hardening includes monotonic lookup/replay/rate/reachability deadlines,
master-authorized monotonic online-key rotation, candidate revalidation, deep
ancestry lookup, replay saturation beyond 16 entries, zero parser output on
rejection, service-specific dial deduplication, v2 reconnect hint retention,
and bounded RPC queueing for eight concurrent native callers.

### User benchmark

No primary value in `docs/USER_BENCHMARKS.md` moved: this hermetic DHT work does
not measure cold/warm start, MTBF, RSS, or kill-9 recovery. It unblocks the S7
provider-root discovery path and scientific-metaverse package discovery without
introducing an all-to-all topology or shared identities. Live chain-advance
verification is not applicable because this lane does not touch sync,
validation, header/block admission, or a cutover, and live service/datadir
mutation was explicitly forbidden.

### Commits and files

- `14273aad5` — claim the iterative ZCODE DHT lane.
- `ac369f955` — implement and test iterative S6 lookup.
- `71c2d185d` — record S6 proof in the handoff and scientific acceptance row.
- Added `lib/vcs/src/zcode_dht_service_lookup.c` and
  `lib/net/src/connman_dht_hint.c`; modified the scoped ZCODE DHT service,
  message codec, boot integration, connman/v2 reachability adapter, bounded RPC
  timeout, native diagnostics, focused tests, seven-node acceptance harness,
  `docs/HANDOFF.md`, and `docs/work/ZCODE_SCIENTIFIC_METAVERSE.md`.

### Acceptance verification

- Final `gate-and-report.sh`: lint 132/132, full whole-program link, and cold
  uncached suite 887/887 eligible groups passed (`groups_failed=0`, 19 declared
  self-skips, 9 gated groups).
- After upstream `main` advanced to `cb8ab59d2`, merge `0408e5d58` integrated
  it conflict-free and the final owning gate passed again: cold 887/887,
  `groups_cached=0`, `groups_failed=0`, toolkey `d67458f5f18a`.
- Seven-node sparse DHT acceptance passed with at least three lookup rounds,
  XOR progress, bounded traffic, alternate-path recovery after killing the
  nearest path, eight concurrent callers, replay saturation, persistence, cold
  restart, and reauthentication.
- Focused DHT, Noise NK/XX, v2 transport parity, strict argv, connman,
  peer-lifecycle, RPC timeout, yardsale, store, and both opt-in store stress
  groups passed.
- Full LTO build and science package/blob lifecycle passed, including dual cold
  boot and byte-identical CAS/projection rebuild after projection wipe.
- `make ci-reproducible` produced identical SHA3-256
  `3a28a407c6c5c56277833e739580ab6dcc192239e6567b0ba6fa9f3ace5b1e2a`;
  `make repro-verify` produced identical different-path SHA3-256
  `25d7943eca11d89afe8f0e3bd611eadc339c0db55f2aae1d6cd94c6d2f11b180`.

### Surprises and follow-ups

Loopback-wide dial deduplication collapsed independent acceptance identities,
the generic addrman cadence delayed learned hints, v2 one-shot upgrade state
could consume a learned non-addnode target, four RPC workers constrained eight
concurrent callers, and the cold-restart harness retained a stale PID array.
The lane fixes each within scope. S7 provider records, STORE acknowledgements,
replication, and deployment remain separate follow-up work.
