# OS-A6 — the adaptive client puzzle

Shipped. The primitive is `lib/net/src/puzzle.c` +
`lib/net/include/net/puzzle.h`; `lib/test/src/test_puzzle.c` is the
behavior model (test group `puzzle`).

`struct puzzle_gate` is the one admission primitive in the tree. It owns a
server-issued challenge seed that rotates on an epoch (with a one-epoch
grace window so a solve in flight is never invalidated), a single-use ring
so one accepted solution admits exactly one request, and a difficulty that
rises with the accepted-request rate and with concurrent large serves and
falls back to an idle floor. The rate term is an EWMA, so there is no
window boundary a flood can start just after and read as idle. A
`struct puzzle_policy` lets each surface pick its own difficulty band,
seed epoch, and load thresholds without a second implementation.

The puzzle is `SHA3-256(challenge_seed || peer_token || ts || nonce)` with
D leading zero bits. Verifying costs one keccak; the requester pays
O(2^D). None of it is persisted and none of it is a consensus predicate —
a fresh process starts clean.

Surfaces on it today:

| Surface | Instance | Binding |
|---------|----------|---------|
| file service bulk stream | `g_fs_pow_gate` (`lib/net/src/file_service.c`) | handshake nonce |
| onion expensive routes | `g_onion_puzzle_gate` (`lib/net/src/onion_ratelimit.c`) | route class |

`puzzle_gate_admit_external()` lends the single-use ring and the load EWMA
to a surface that verifies its own proof, for a wire format that cannot
carry a seed round-trip. It has no caller yet.

Two surfaces are still on the legacy fixed-difficulty
`fast_sync_verify_pow()`:

- **Snapshot serve** (`snapsync_validate_serve_request`,
  `app/services/src/snapshot_sync_service.c`). Adding single-use here
  needs a protocol revision first, not just a gate call: the `zsnapreq`
  proof is `(peer_id, ts, nonce)` where `peer_id = SHA3(peer_ip)` and the
  solver walks nonces from zero, so two honest requests from one address
  inside the same second produce a byte-identical proof and the second
  would be refused as a replay. Carry a server-issued rotating seed in the
  snapshot offer and this moves onto `puzzle_gate_verify` outright.
  (`test_snapshot_serve_loopback` catches exactly this collision.)
- **Store order mint** (`app/controllers/src/store_controller_pow.c`),
  fixed at 20 bits behind its own replay ring, so it has no load response.
  It already binds a per-product token, so this one is a direct swap.
