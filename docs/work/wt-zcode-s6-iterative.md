# S6 iterative ZCODE lookup lane

**STATUS: IN PROGRESS**

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

