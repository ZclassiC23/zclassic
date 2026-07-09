# 10 — Wire adversaries: partition and heal a P2P link

## What this demonstrates

`examples/10_wire_adversaries.c` drives `simnet_wire` (the deterministic
in-memory P2P harness in `lib/sim/src/simnet_wire.c`) with two peers against
one node-under-test: an honest peer that completes the handshake, and a
`FLOOD` adversary that pushes unbounded bytes at the same node. It then closes
(`simnet_wire_partition_peer`) the honest peer's link mid-run — modeling "the
node's only real connection drops while an attacker connection stays up" —
confirms no `PERMANENT` blocker fires during the outage (no silent halt, just
a visibly closed link), reopens the link, and proves recovery with a fresh
ping/pong round trip. Everything is tick-keyed off a fixed seed, so the run
(including the final FNV fingerprint) is byte-for-byte reproducible.

## Build and run

```bash
make -C examples && ./examples/bin/10_wire_adversaries
```

The file compiles against the same public `lib/sim` + `lib/net` + `lib/util`
headers the real test binary uses (`-Ilib/*/include`, `-DZCL_TESTING`); see
`lib/test/src/test_simnet_wire.c` for the equivalent test-harness coverage
(`test_simnet_wire_partition_recovery`).

## Expected output (sketch)

```
[1/6] building a 2-peer scenario: peer 0 honest, peer 1 FLOOD (seed=0x10adee5a0000a11e)...
[2/6] baseline: draining handshake + attacker traffic, then round-tripping a ping on the honest link...
    OK: honest peer handshook and got its pong back despite concurrent flood traffic.
[3/6] partitioning peer 0's link (closed=true) — the node's only real connection just went dark...
[4/6] running 256 ticks with the link down (attacker still flooding on slot 1)...
    [mid-outage] peers_open=1 handshake=1 pong=1 no_perm_blocker=1 monitor_failed=0 fp=0x...
    OK: no permanent blocker fired; the outage is visible as peers_open==1, not a silent halt.
[5/6] healing the link (closed=false)...
[6/6] proving recovery with a fresh ping/pong round trip...
    [post-heal] peers_open=2 handshake=1 pong=1 no_perm_blocker=1 monitor_failed=0 fp=0x...
OK: partition -> no silent halt -> heal -> recovery, all deterministic (fingerprint=0x..., ticks=..., backpressure_rejects=...)
```

Exit code 0 on success; nonzero with a `FAIL: ...` line on `stderr` if any
step or invariant does not hold.

## Key APIs used

- `lib/sim/include/sim/simnet_wire.h` / `lib/sim/src/simnet_wire_peer.c` —
  `simnet_wire_create_scenario`, `simnet_wire_start_peer_kind` (via the
  scenario's `peers[]`), `simnet_wire_run`.
- `lib/sim/src/simnet_wire.c` — `simnet_wire_partition_peer` (per-link
  `WIRE_EVENT_OPEN`/`WIRE_EVENT_CLOSE`, egress pinned to peer slot 0),
  `simnet_wire_peer_send_ping`, `simnet_wire_peer_pong_received`,
  `simnet_wire_get_stats` (`peers_open`, `no_unexpected_permanent_blocker`,
  `monitor_failed`, `fingerprint`).
- `lib/util/include/util/blocker.h` / `lib/util/src/blocker.c` —
  `blocker_reset_for_testing` (clean baseline before creating a wire; not
  `ZCL_TESTING`-gated).
- `lib/net/include/net/net.h` — `MAX_RECV_MESSAGES` (the receive-queue bound
  the flood peer is checked against).

## Production counterpart

- Real handshake/framing/backpressure over actual sockets:
  `lib/net/src/msgprocessor.c` (dispatch table) and `lib/net/src/net.c`
  (`p2p_node` receive/send, `MAX_RECV_MESSAGES` enforcement) — the exact code
  `simnet_wire` drives directly, minus the socket layer.
- Real no-silent-halt / partitioned-peer detection on a live node:
  `app/services/src/sync_monitor.c` and the supervisor liveness tree
  (`lib/util/src/supervisor.c`; see `CLAUDE.md` "Adding state introspection"
  and `zcl_state subsystem=supervisor`), surfaced via `zcl_state
  subsystem=blocker` instead of this example's direct
  `simnet_wire_get_stats()` polling.

## Notes

`make -C examples` is not yet a wired build target in the repo `Makefile` at
the time this file was written — the compile command above is the intended
integration point; verify against the current `Makefile`/`examples/`
directory before relying on it.
