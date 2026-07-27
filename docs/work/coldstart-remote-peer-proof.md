# Cold start to tip against a REMOTE peer

The C3 wipe-to-tip stopwatch (`tools/scripts/cold_start_to_tip_stopwatch.sh`)
has only ever been run against a peer on the same machine. Loopback is
structurally privileged on **both** sides of the wire, so a loopback pass
cannot stand in for a remote one:

- **client side** — `is_trusted_peer()` in `lib/net/src/net.c` exempts
  `127.0.0.0/8` and `-whitelist` peers from `peer_misbehaving()`, so a loopback
  client never rides the score-to-ban path;
- **server side** — the per-IP inbound sybil cap in `lib/net/src/net.c`
  ("too many inbound connections from same IP", max 3) is only ever contended
  when several clients share one source IP, which is the remote case and never
  the one-node-per-loopback case.

`make mvp-coldstart-to-tip-remote` pins the remote invocation.

## Run

```
bash tools/scripts/cold_start_to_tip_stopwatch.sh \
    --peer=205.209.104.118:8033 --budget=600 --sample=15
```

Fresh `/tmp` datadir, isolated `$HOME`, ports 39170-39173, `-listen=0`,
`-nolegacyimport`, `-nobgvalidation`, no bundle / snapshot / import flags.
Read-only P2P client: the remote's datadir and services are never touched.

## Result

**Exit 4 — STALLED-NAMED.** `WALL_CLOCK_SECONDS=607`, one boot, budget 600 s.
H\* and the provable sample never left zero across all 41 samples;
`network_tip` was never readable (`-1` in every row, `network_tip_read_ok`
never true). One active blocker the whole run:
`bootstrap.no_state_source` (owner `bootstrap`, class `dependency`).

This is a genuine stall with a named cause, not a silent stall and not a SKIP.
It is also not a SEAM: nothing climbed, so there is no forward progress to
report and no wall-clock number worth publishing.

## Named cause: the P2P handshake never completes

`dumpstate peer_lifecycle` during the run:

```
attempted 8, connected 8, version_sent 2, version_received 0,
verack_received 0, handshake_complete 0, pre_handshake_disconnects 8
```

Every attempt reached TCP and none reached the handshake; the captured
`node.log` carries 18 `protocol failure before handshake` lines over the run.

`dumpstate connman` addnode ledger for `205.209.104.118:8033`:
`tcp_failures 0`, `protocol_failures 1+`, `backoff_sec 60`. Node log:

```
Addnode 205.209.104.118:8033: protocol failure before handshake
    (remote-close, state=connecting)
```

TCP connects succeed; the peer closes the socket before any version/verack
exchange completes. A bare probe confirms it independently — connect, send
nothing, read: EOF arrives immediately, zero bytes.

Because no peer ever reaches `PEER_HANDSHAKE_COMPLETE`, no peer height is ever
advertised, `network_tip` stays unreadable, and the PASS predicate
(authoritative H\* ≥ `network_tip`) is unreachable by construction for the
whole budget — independent of any reducer-side defect.

## The peer-ban theory is refuted for this run

The client-side ban path (`lib/net/src/peer_scoring.c` weights `invalid_block`
/ `protocol_violation` at 100 against a threshold of 100) was never entered: no
`banlist.dat` is written in the wiped datadir (`banlist_present: false`), no
`peer_banned` line appears in the node log, and `peer_misbehaving()` requires a
message-layer offence that a connection dying pre-version can never produce.
The exclusion happens on the **serving** side, at `accept()`, before scoring
exists.

The fitting server-side rule is the per-IP inbound cap in `lib/net/src/net.c`:
`same_ip_count >= 3` closes the socket immediately with
`too many inbound connections from same IP`. `ss -tn state established 'dst
205.209.104.118:8033'` shows this host already holding exactly three
established sockets to that peer — the cap value — opened by the canonical node
that shares this machine's public IP. The harness node is the fourth connection
from the same source IP and is refused at accept. This is the sybil defence
working as written; the consequence is that a second node behind one IP (or any
NAT) cannot cold-start off that peer.

## Harness changes landed with this record

1. **Peer precheck** (`peer_precheck` / `classify_peer_precheck`). The old
   check treated "TCP connect succeeded" as "serving peer present" — true on
   loopback, false here. The probe now connects without sending a byte and
   classifies `unreachable` / `held_open` / `accept_close`. `accept_close`
   prints a loud warning naming the per-IP cap and the `ss` command to confirm
   it, and is recorded as `peer_precheck` in `proof.json`. It is deliberately
   **advisory**: it never converts a verdict, and an accept-closing peer is
   never laundered into a SKIP. The pure classifier is covered by `--selftest`.
2. **Network capture in the failure bundle.** Non-pass runs now write
   `net-connman.json`, `net-peer_lifecycle.json`, `net-network.json`, and
   `banlist.dat` when one exists (`banlist_present` in `proof.json`). The first
   question a remote non-pass verdict raises — did we ever handshake, and did
   we ban our only peer — was previously unanswerable from the artifact alone,
   because a loopback peer always handshakes.

## What a next remote run needs

Either a peer whose per-IP inbound allowance for this host is not already
consumed (free a slot, use a different source IP, or `-whitelist` the client on
the serving node), or a different serving peer. Until a run reaches
`peer_precheck=held_open` and a completed handshake, no remote run can measure
the reducer at all.
