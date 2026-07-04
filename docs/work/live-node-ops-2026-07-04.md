# Live-node ops incident and runbook - 2026-07-04

Purpose: preserve the 2026-07-04 live-node incident facts and the safe
operator path. This is a docs-only runbook: do not edit code, systemd units, or
live datadirs from here.

## Incident snapshot

Captured with read-only commands at `2026-07-04 00:15 UTC`.

- `zclassic23` service: active/running since `2026-07-04 00:03:44 UTC`.
  `ExecStart` uses `-datadir=/home/rhett/.zclassic-c23`, `-port=8033`,
  `-rpcport=18232`, `-listen`, `-txindex`, `-tor`, `-nobgvalidation`,
  `-nolegacyimport`, and
  `-load-snapshot-at-own-height=/home/rhett/.zclassic-c23/utxo-seed-3155842.snapshot`.
- `zclassicd` service: active/running since `2026-07-03 22:11:34 UTC`.
  `ExecStart` uses `-datadir=/home/rhett/.zclassic`, `-port=8034`,
  `-externalip=74.50.74.102:8034`, and RPC is read through `8232`.
- Public identity split is intentional: `zclassic23` owns canonical ZClassic23
  P2P port `8033`; legacy `zclassicd` is the local oracle/peer on noncanonical
  P2P port `8034`. Do not move `zclassicd` to `8033`.
- `zclassic23 getnetworkinfo` advertised `74.50.74.102:8033` and showed
  `127.0.0.1:8034` as a connected addnode. That local addnode is the legacy
  advisory peer, not the public c23 identity.
- Live c23 state was not healthy/serving: `blocks=3162825`,
  `headers=3169223`, `sync_state=blocks_download`, `tip_lag=7329`,
  `H*=3162825`, and `coins_applied_height=3162826`.
- Named blockers/evidence: `contradiction_frozen`,
  `chain_integrity_failed`, `download_queue_starved`,
  `tip_stall_oracle_rebuild`, and
  `active_tip_ancestry_unlinkable (h=3161894)`.
- Legacy oracle `zclassicd` was at `blocks=headers=3169223`. At c23 `H*`
  `3162825`, both nodes returned block hash
  `0000087c8efbd35e51453654e39f0377306e629c6ff2c31efd13cf780f67d6f8`.

## Post-fix state

Captured after the clean-identity deploy at `2026-07-04 01:10 UTC`.

- Commit `28e97e217` was pushed to `origin/main` after `make lint`,
  `make build-only`, focused identity/finality tests, and the pre-push
  `make ci` hook passed.
- The CI-built `build/bin/zclassic23` was installed as
  `$HOME/.local/bin/zclassic23-live`, then only `zclassic23` was restarted.
  `zclassicd` was left running.
- `zclassic23 getnetworkinfo` advertised exact BIP14 user-agent
  `/ZClassic23:0.1.0/`, listened publicly on `74.50.74.102:8033`, and kept
  `127.0.0.1:8034` as the local legacy addnode.
- `./tools/z mirror --json` reported `state=healthy`, `mirror_running=true`,
  `zclassic23_height=zclassicd_height=3169281`, matching hash
  `0000069bfa190d563e8c098e2a4112955b80f636770b1b827fa0ebeaae57f4dc`,
  `lag=0`, `consensus_authority=local_consensus_validation`, and
  `overrides_total=0`.

## Rules

- Never stop, restart, reconfigure, or kill `zclassicd` during c23 recovery.
  It is the parity oracle and the local noncanonical peer on `8034`.
- Never perform live surgery first. Copy the datadir, reproduce on the copy,
  prove the repair fires on the copy, then make any live move owner-gated.
- Do not advertise or document `8034` as the canonical c23 network port.
  `8033` is c23; `8034` is legacy oracle isolation.
- A boot without FATAL is not proof. The gate is `H*` climbing, then holding tip,
  with block-hash parity against `zclassicd`.

## Identity note

- The legacy C++ source uses `CLIENT_NAME("MagicBean")` only to format its own
  BIP14 user-agent (`/MagicBean:<version>/`) and to expose that value through
  RPC/logs. Its peer receive path stores a remote peer's subversion for
  sanitized logging/RPC; it does not require remote peers to advertise
  `/MagicBean`.
- The actual ZClassic P2P message-start bytes are separate consensus/network
  framing (`24 e9 27 64`). A clean c23 user-agent such as
  `/ZClassic23:0.1.0/` is therefore an identity string, not a consensus or
  network-magic change.

## Safe verification commands

Topology and services, read-only:

```bash
systemctl --user show zclassic23 -p ActiveState -p SubState -p ExecStart -p NRestarts -p ActiveEnterTimestamp
systemctl --user show zclassicd -p ActiveState -p SubState -p ExecStart -p NRestarts -p ActiveEnterTimestamp
ss -ltnp | rg ':(8033|8034|18232|8232)\b'
```

Live c23 state, read-only:

```bash
tools/z rpc getnetworkinfo
tools/z rpc getblockchaininfo
tools/z rpc healthcheck
tools/z state reducer_frontier
tools/z state chain_evidence
tools/z state condition_engine
tools/z log 'contradiction|operator_needed|FATAL|blocker' 3600 80 WARN
```

Legacy oracle, read-only:

```bash
/home/rhett/bin/zclassic-cli -datadir=$HOME/.zclassic -rpcport=8232 getblockchaininfo
H=$(tools/z state reducer_frontier | jq -r '.state.hstar')
tools/z rpc getblockhash "$H"
/home/rhett/bin/zclassic-cli -datadir=$HOME/.zclassic -rpcport=8232 getblockhash "$H"
```

MCP equivalents, when available: start with `zcl_status`, then
`zcl_state subsystem=reducer_frontier`, `zcl_state subsystem=chain_evidence`,
and `zcl_state subsystem=condition_engine`.

## Snapshot fallback vs full-history repair

- Snapshot fallback datadir: the live service currently uses
  `$HOME/.zclassic-c23` plus
  `$HOME/.zclassic-c23/utxo-seed-3155842.snapshot`. This is the borrowed
  stopgap/availability lane. Preserve it as evidence when unhealthy; do not let
  it hide a full-history contradiction.
- Full-history repair lane: use `$HOME/.zclassic-c23-fullhist` or a fresh copy
  of the failing live datadir as the repair specimen. Run repair only on an
  isolated copy with non-live ports, then gate on `H*` climb,
  `coins_applied_height == hstar + 1`, hash parity with `zclassicd` at sampled
  heights, and a warm reboot.
- Sovereign cure lane: the self-minted anchor snapshot still gates the final
  removal of the borrowed seed. Follow
  `docs/work/sovereign-cutover-runbook.md`; do not stage a cutover without the
  verified artifact and copy-prove pass.

## Next hardening tasks

- Add one read-only live-topology probe that reports c23 P2P/RPC/datadir,
  legacy P2P/RPC/datadir, active snapshot path, `H*`, header tip, and zclassicd
  parity height in one JSON object.
- Add a guard/test that c23 public service config stays on `8033` and
  `zclassicd` stays on `8034`, including `127.0.0.1:8034` classification as a
  legacy advisory peer.
- Add a copy-prove profile for this incident class: contradiction-frozen plus
  tip-finalize log hole, isolated ports, `H*` climb gate, and mandatory zclassicd
  hash comparison.
- Make the health output name the fallback snapshot file and whether the current
  datadir is fallback, fullhist, soak, or scratch.
- Keep the sovereign anchor mint/cutover moving so the borrowed snapshot loader
  can be deleted after the copy-proven live cutover.
