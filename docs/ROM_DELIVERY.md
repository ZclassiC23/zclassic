# ROM delivery — free-tier P2P distribution of the sync artifacts

zclassic23 is a P2P file delivery system, and the files it delivers fastest
and most generously are its own bootstrap ("ROM") artifacts: the consensus-
state bundle (`zcl.consensus_state_bundle.v1`, see
[`docs/work/CONSENSUS-STATE-BUNDLE.md`](./work/CONSENSUS-STATE-BUNDLE.md)) and
header-chain seed data. A fresh node should be able to pull these from any
peer — free, unmetered, no payment — instead of folding block-by-block from
genesis. This page covers the trust model, the policy knobs that keep
generosity from becoming a bandwidth liability, the operator commands, and
the fetch recipe.

## Trust model — delivery is untrusted, content is verified

Nothing about who serves an artifact, how fast, or how many peers vouch for
it makes that artifact more trustworthy. A malicious or careless seeder can
waste a fetcher's bandwidth; it cannot poison the fetcher's state, because
every artifact is verified by content after download, never by transport:

1. **Whole-file digest.** The downloaded bundle is hashed (SHA3-256) and
   compared against the digest the fetcher already committed to before
   requesting chunks — a mismatch discards the whole file, no partial trust.
2. **Checkpoint re-derivation.** The installer re-derives the compiled
   checkpoint SHA3 from the bundle's contents and Pedersen-roots the Sapling
   frontier against the PoW header chain it has independently validated. See
   [`docs/work/CONSENSUS-STATE-BUNDLE.md`](./work/CONSENSUS-STATE-BUNDLE.md)
   for the full component list a bundle must independently prove.
3. **PoW header roots remain the anchor.** `hashFinalSaplingRoot` and the
   rest of the header commitment are validated against the header chain the
   node builds itself; a bundle's own claims about height/hash only matter
   once they tie back to that independently-validated chain.

The practical effect: seeding capability ships **enabled by default**. There
is no safety reason to gate it behind an opt-in, because a seeder cannot
corrupt what it serves — it can only be slow, rude, or absent, all of which
the policy layer below bounds.

## Policy layer

The policy surface lives in `lib/net/include/net/rom_seed_policy.h` /
`lib/net/src/rom_seed_policy.c`. It is deliberately narrow: config-backed
knobs, pure admission/boost decision helpers, and live counters. It never
opens a socket, reads a chunk off disk, or speaks the wire protocol — the
seed engine (the artifact registry + chunk-serving path, `dumpstate
rom_seed`) is the thing that calls into this header on every admission
decision and every completed/refused upload.

| Knob | Default | Purpose |
|---|---|---|
| `enabled` | on | Master switch. Off refuses every new upload regardless of load. |
| `global_up_bytes_per_sec` | 50 MB/s | Node-wide upload cap across all artifact serving. |
| `per_peer_up_bytes_per_sec` | 2 MB/s | Per-peer upload cap, so one requester cannot claim the whole global seat. |
| `max_concurrent_uploads` | 24 | Concurrency cap independent of bandwidth. |
| `generosity_boost_days` | 7 | A freshly-registered artifact gets a per-peer cap multiplier for its first N days — the network needs new ROM spread fast, before it has many seeders. |

Policy is persisted at `<datadir>/rom_seed_policy.json`, written tmp+rename
(crash-safe, never a torn read) and falling back to the compiled defaults on
a missing or corrupt file — the policy module never refuses to boot and
never crashes on a bad file. Owner-mutating changes go through
`rom_seed_policy_apply()`, which validates against fixed bounds
(`ROM_SEED_POLICY_MIN/MAX_*` in the header) and leaves the live policy
untouched on any validation failure — never a partial apply.

### Consensus always preempts artifact traffic

This is a hard rule, not a knob. Whatever the reducer/sync supervisor judges
necessary for consensus progress (headers, bodies, tx relay) outranks
artifact seeding. The sync supervisor calls
`rom_seed_policy_set_consensus_active(true)` whenever that pressure is live;
`rom_seed_policy_admit_upload()` then refuses new uploads until the signal
clears. A caller that never touches the signal gets
`rom_seed_policy_consensus_active() == false` forever — safe by default, no
silent seeding disablement from an unwired caller.

### The serve-path contract

A chunk-serving path is expected to call, in order:

1. `rom_seed_policy_admit_upload(current_active_uploads)` before accepting a
   new upload — false means refuse (disabled, consensus-preempted, or at the
   concurrency cap).
2. `rom_seed_policy_note_upload_started()` on acceptance.
3. `rom_seed_policy_effective_per_peer_cap(boosted)` to throttle bytes/sec
   for that peer, where `boosted` comes from
   `rom_seed_policy_is_boosted(artifact_first_seen_unix, now)`.
4. `rom_seed_policy_note_upload_finished(bytes_sent)` or
   `rom_seed_policy_note_upload_refused()` on completion/refusal.

If a sibling engine's serve path is shaped a little differently, wrap these
calls in a thin adapter — the surface is intentionally small so the wiring
stays trivial regardless of naming differences on either side.

## Serve-log ledger

`lib/net/include/net/rom_seed_ledger.h` /
`lib/net/src/rom_seed_ledger.c` is an append-only, retention-capped record
of completed (or aborted) serve sessions — which peer, which artifact, how
many chunks/bytes, when — stored in `<datadir>/rom_seed_ledger.db`
(`artifact_serve_log` table), following the same append-then-prune shape as
the `peer_sessions` ledger in `lib/storage/src/peers_projection.c`. It caps
at `ROM_SEED_LEDGER_RETENTION_CAP` rows (delete-oldest past the cap) so
telemetry never grows unbounded. This is observability, not consensus state
or a rebuildable projection — there is no upstream event log this table
folds from; the ledger IS the append point.

`artifact_id` is an opaque 32-byte digest (the SHA3 root the caller already
has), so the ledger never needs to know the seed engine's own artifact-
metadata shape.

## Operator commands

All three live under `ops.debug.rom_seed.*` (native command registry — see
[`docs/NATIVE_COMMAND_INTERFACE.md`](./NATIVE_COMMAND_INTERFACE.md)). They
nest under `ops.debug` rather than a new top-level `ops.*` branch because
the top-level `ops` menu is already at its listing-budget ceiling
(`ZCL_COMMAND_BRANCH_BUDGET`) — see the precedent comment above
`ops.debug.dash` in `config/commands/ops.def`:

```bash
zclassic23 ops debug rom_seed status      # policy + live counters + ledger availability
zclassic23 ops debug rom_seed enable      # owner-mutating: turn seeding on
zclassic23 ops debug rom_seed disable     # owner-mutating: turn seeding off
zclassic23 ops debug rom_seed artifacts   # every artifact served + per-artifact seed stats
```

`status` and `artifacts` are public reads; `enable`/`disable` are
owner-authenticated mutating leaves (idempotent — enabling an
already-enabled policy, or disabling an already-disabled one, returns the
same settled state). `dumpstate rom_seed_policy` and `dumpstate
rom_seed_ledger` expose the same data through the generic diagnostics
primitive, per the "Adding state introspection" convention in the repo's
`CLAUDE.md`.

`ops.debug.rom_seed.artifacts` reports `seed_stats` folded from the local
serve log today. The seed engine's own artifact registry (names, digests, the
free-tier artifact catalog) is a sibling subsystem (`dumpstate rom_seed`);
until it is wired into a given build, the `registry` section of the reply
honestly reports `available: false` rather than guessing at that lane's
shape.

## Fresh-node fetch recipe

A fresh node fetches the ROM artifacts from any peer that has them, verifies
by content, and installs — no operator setup required beyond a normal boot:

1. Boot with networking enabled (default). The node discovers peers via
   normal P2P bootstrap or, with `-tor`, the onion directory
   (`/directory.json`) served by any Tor-enabled peer.
2. The fetch engine (`dumpstate rom_fetch`, sibling subsystem) requests the
   consensus-state bundle and header-chain seed data from multiple seeders
   in parallel, verifying each downloaded chunk's position/length against the
   manifest before accepting it.
3. On whole-file digest match, the installer re-derives the checkpoint SHA3
   and Pedersen-roots the Sapling frontier against the independently
   validated PoW header chain (see Trust model above). Only a bundle that
   passes this re-derivation is installed.
4. The node then folds forward from the installed checkpoint via normal
   block-body sync to reach the live tip.

A node that never finds a peer serving these artifacts falls back to the
legacy two-step `--importblockindex` + boot recovery path (see the
Tenacity section of the repo's `CLAUDE.md`) — ROM delivery is an
acceleration, not a hard dependency.
