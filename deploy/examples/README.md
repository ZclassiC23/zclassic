# deploy/examples — operator-specific unit files

Reference units from the original operator's machine. Unlike the portable
units in `deploy/` (which use `%h`), these hardcode operator paths or name
operator-specific peers. Copy and adapt before installing:

- `zclassicd-rhett.service` — legacy C++ `zclassicd` dev peer used as a
  drift/consensus oracle (see CLAUDE.md "Services" and docs/SYNC.md).
- `zclassic23-soak.service` / `.timer` — 10-minute three-guarantees soak
  probe driving `tools/scripts/soak_assert.sh` (hardcoded repo path).
- `zclassic23-replay-canary-nightly.{service,timer}` — nightly from-anchor
  replay canary driving `tools/scripts/replay_canary.sh --from=anchor`
  (~45 min). Replays bodies anchor(3,056,758)->tip through the HEAD reducer
  in an ISOLATED /tmp scratch datadir on 3905x ports and asserts zero
  consensus rejects, the anchor checkpoint passed without an integrity
  FATAL, and coarse UTXO stats == co-located zclassicd `gettxoutsetinfo`
  (RPC 8232, READ-ONLY). Verdict is the sentinel FILE
  `~/.local/state/zclassic23-canary/replay_canary_anchor.json`, written
  atomically only after every assertion passes — never exit-0-as-proof.
- `zclassic23-replay-canary-weekly.{service,timer}` — weekly from-genesis
  replay (genesis->tip, ~6 h, bg-validation ON). Same asserts plus full
  script coverage. This run dials the co-located zclassicd P2P
  (127.0.0.1:8033) via `-addnode` for bodies — the one real peer, read-only.
- `zclassic23-replay-canary-onfailure@.service` — templated `OnFailure=`
  arm; logs `CANARY FAILED` to the journal (`journalctl -t replay-canary`).

## Replay-canary operational notes

- **Disk**: each canary run mints one scratch datadir on /tmp holding the
  chainstate + a copy of the LevelDB block index — budget ~50-80 GB
  transient. The harness runs a `df /tmp` preflight and REFUSES loudly if
  /tmp has < 80 GiB free. If /tmp is a small tmpfs, point the harness at a
  real-disk scratch root by sourcing it with a /tmp that has headroom, or
  provision tmpfs/disk accordingly. The cleanup trap removes the datadir on
  exit (including on SIGKILL of the group).
- **Port reservation**: the nightly uses 39050 base, the weekly 39060. The
  future nightly crash-soak (tenacity-roadmap item 7) should run at a
  DISJOINT calendar slot (~04:30) and on the RESERVED 39070 base. Distinct
  `ISO_KIND` mints distinct `/tmp/zcl23-replay-*` datadirs, so even an
  overlapping run is datadir-safe; the `ss(8)` LISTEN preflight in
  `isolated_mainnet_env.sh` is the backstop against port collision.
- **Read-only zclassicd**: the canary only ever READS zclassicd via
  `--importblockindex` (copies-then-strips the COPIED LOCK; never touches
  the source LOCK) and `gettxoutsetinfo` on RPC 8232. It never issues
  `stop`/`generate`.
- **Opt-in targets**: `make replay-canary-anchor` / `make replay-canary-genesis`
  run the same harness by hand. The hermetic verdict-logic gate
  (`test_replay_canary_verdict`) runs in `make ci` and red-fails on a
  seeded known-bad reducer before any green night can count.
