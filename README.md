# ZClassic23

[![license](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![language](https://img.shields.io/badge/language-C23-00599C.svg)](#)
[![status](https://img.shields.io/badge/status-pre--v1-orange.svg)](docs/MVP.md)
[![CI](https://img.shields.io/badge/CI-local%20make%20ci%20(40%2B%20gates)-success.svg)](docs/DEFENSIVE_CODING.md)

One self-contained ~15 MB pure-C23 binary: a full ZClassic node (Equihash 200,9
PoW, Sapling shielded transactions), an embedded Tor onion service, a block
explorer, a shielded wallet, and a built-in MCP server that lets an AI agent
operate the node through ~100 typed tools.

**One binary, one onion, one stack.**

## Status

**Pre-v1 — not yet production-ready.** Of the eight v1 acceptance criteria in
[`docs/MVP.md`](docs/MVP.md), four pass their local operator proof (MRS 4/8);
none are yet end-to-end CI-verified across a live soak. Don't rely on it as your
only mainnet node yet.

It runs on ZClassic mainnet on the `zclassicd` consensus floor and **reaches the
network tip** via a **borrowed-but-consensus-bound stopgap**: it seeds from a
UTXO snapshot whose anchor hash is bound to the in-binary PoW header, rather than
folding that set from its own checkpoint. The **sovereign cold-start cure** —
fold real block bodies forward from the verified checkpoint, then delete the
borrowed-seed machinery — is in flight; its design is in
[`docs/work/never-stuck-plan.md`](docs/work/never-stuck-plan.md). For current live
state, ask the running node (`zcl_status`) or read
[`docs/HANDOFF.md`](docs/HANDOFF.md) — this file does not track it. The other known
soft spot: **off-chain ZMSG is plaintext on the wire**.

It is operator-owned full-node infrastructure: embedded Tor publishes *your* onion
service, wallet state stays in your datadir, MCP is a typed *local* operator
interface. Safety boundary and integrity checks:
[`docs/SECURITY_AND_INTEGRITY.md`](docs/SECURITY_AND_INTEGRITY.md).

## What's on board

A complete rewrite of zclassicd in pure C23. One binary is at once a full node
(Equihash PoW, ECDSA scripts, Sprout/Sapling zk-SNARKs, history validates
identically to zclassicd), a fast-sync server (FlyClient MMB + SHA3 UTXO
snapshot — *the ~1-minute cold sync this enables is a design target, not the
proven path today; see [Bootstrapping to tip](#bootstrapping-to-tip)*), an
in-process Tor hidden service (`-tor`), a block explorer (`/api`), a shielded
wallet (transparent + Sapling), and an MCP server. Full subsystem catalog in
[`CLAUDE.md`](CLAUDE.md).

Honestly labeled: **ZNAM** name registry (working); **ZMSG** messaging (on-chain
shielded; off-chain P2P is plaintext on the wire); **ZCL Market** + **ZSWP**
atomic swaps (scaffolding — no settlement yet); **P2P games** (ping +
TicTacToe).

## Quick start

**Prerequisites:** gcc 14+ (or clang with `-std=c23`), GNU make, plus
`cmake`, `autoconf`, `curl`/`wget`, and `unzip` for the one-time vendored-library
build.

```bash
git clone https://github.com/ZclassiC23/zclassic.git && cd zclassic
make zclassic23     # main binary -> build/bin/zclassic23
make test           # full suite (~430 parallel groups)
make lint           # defensive-coding gates
```

The first `make zclassic23` auto-runs **`make vendor`**, which builds the static
third-party archives in `vendor/lib/` from source (OpenSSL, libevent ×3, LevelDB,
SQLite, zlib) plus the in-tree Tor stub — sources are pulled from pinned URLs and
verified against pinned SHA256 hashes, then compiled locally. Only
`libsecp256k1.a` (a custom Bitcoin Core fork build) ships committed. `make vendor`
is idempotent: once the archives exist it is a no-op (`make vendor-force`
rebuilds). Per-library sources, versions, and hashes are in
[`docs/BUILD.md`](docs/BUILD.md).

```bash
build/bin/zclassic23          # start a node (fresh datadir → long initial sync)
build/bin/zclassic23 -tor     # + .onion (opt-in build — see note below)
```

A fresh node starts honestly empty (`getblockcount` → `0`) and begins syncing
from peers. To reach the chain tip quickly today, see
[Bootstrapping to tip](#bootstrapping-to-tip) — a plain start on an empty datadir
has a **long** initial sync.

> **The onion service is an opt-in build.** The default binary links a Tor
> *stub*, so `-tor` runs the node normally **without** an onion and logs that Tor
> is disabled. To enable the real in-process hidden service, build the bundled
> Tor fork — `git submodule update --init vendor/tor`, then build it per
> [`docs/BUILD.md`](docs/BUILD.md) — and the Makefile auto-links it
> (`build/bin/zclassic23 -tor` then publishes a `.onion`, visible in
> `zcl_status`).

Datadir `~/.zclassic-c23/` (`-datadir=DIR`). Default ports: P2P `8033`, RPC
`18232`. To run alongside a local `zclassicd` (which holds `8033`), use
`-port=8023` — the shipped service does. The authoritative lane/port table is in
[`docs/HANDOFF.md`](docs/HANDOFF.md).

## First boot — what a fresh node looks like

A brand-new datadir is honestly empty. It does **not** report a fake height:

- `getblockcount` returns **`0`** until blocks are actually folded — no phantom
  tip. `getblockchaininfo` returns `blocks: 0, headers: 0,
  initialblockdownload: true` (best-block resolves to genesis).
- **Peer discovery has no DNS seeders** — the historical ZCL DNS names no longer
  resolve. The node bootstraps from hardcoded MagicBean IP seeds (10 addresses)
  and a Tor `.onion` directory seed, harvesting clearnet peers from each onion's
  `/directory.json`. You can add your own onion seeds (one `.onion` per line,
  `#` comments allowed) in `~/.config/zclassic23/onion-seeds`.
- **Without a bootstrap bundle the initial sync is long** (full P2P from
  genesis is ~hours; see [Bootstrapping to tip](#bootstrapping-to-tip) for the
  fast paths). This is expected on a fresh node.

### Is it healthy?

One call answers it — over MCP `zcl_status`, or over RPC the equivalent
`zcl-rpc` calls (`getblockcount`, `getpeerinfo`, `syncstate`, `healthcheck`).
The key fields:

| Field | Fresh / syncing | Synced (at tip) |
|---|---|---|
| `height` | `0`, then rising | network tip height |
| `peers` | `0`, then climbing | several connected |
| `sync.state` | `finding_peers` → `headers_download` → `blocks_download` | `at_tip` |
| `header_gap` / `sync_behind` | large / `true` | `0` / `false` |
| `health.healthy` | `false` while catching up | `true` |
| `health.checks.has_peers` | `false` | `true` |
| `health.checks.onion_address` | absent until Tor onion is up | present (`…onion`) |
| `blockers.active_count` / `dominant_blocker` | `0` / `null` on a healthy boot | `0` / `null` |

Illustrative `zcl_status` output (**example values, not a live capture** —
large diagnostic subtrees trimmed):

```jsonc
// Fresh node, still finding peers:
{
  "height": 0, "header_gap": 0, "sync_behind": false, "peers": 0,
  "connections": {"total":0,"inbound":0,"outbound":0,"zcl23":0,"magicbean":0},
  "sync":   {"state":"finding_peers","state_id":1},
  "health": {"healthy":false,
             "checks":{"synced":false,"has_peers":false,"tor_ready":false,
                       "peer_count":0,"tip_lag":0}},
  "blockers": {"active_count":0,"dominant":null}, "dominant_blocker": null
}

// Synced node at tip, onion up:
{
  "height": 3160247, "header_gap": 0, "sync_behind": false, "peers": 8,
  "connections": {"total":8,"inbound":2,"outbound":6,"zcl23":3,"magicbean":5},
  "sync":   {"state":"at_tip","state_id":5},
  "health": {"healthy":true,
             "checks":{"synced":true,"has_peers":true,"tor_ready":true,
                       "peer_count":8,"tip_lag":0,
                       "onion_address":"…24qd.onion"}},
  "blockers": {"active_count":0,"dominant":null}, "dominant_blocker": null
}
```

A healthy node is **`sync.state: at_tip`, `health.healthy: true`,
`peers > 0`, `blockers.active_count: 0`**. A stall is never silent: it surfaces
as a growing `header_gap` or a named entry in `blockers` / `dominant_blocker`.

## Bootstrapping to tip

Be precise about what is proven versus designed:

- **Designed (not the proven path today):** native P2P fast sync — a fresh node
  pulls a SHA3-verified UTXO snapshot from another zclassic23 peer (FlyClient/MMB
  proof bound to the PoW chain), targeting an operational tip in ~a minute. This
  is the intended default *once the native peer network is established*; it is
  built but not the everyday cold-start proof. Details: [`docs/SYNC.md`](docs/SYNC.md)
  "Method 1". A self-contained bootstrap **bundle** ("tip in a sitting") is in
  flight.
- **Proven (development bootstrap, ~25 min):** the two-step import from a synced
  local `zclassicd` — **headers first, then a normal boot.** Order matters:
  skipping step 1 leaves a ~3.1M-header hole and the node pins.

  ```bash
  # 1. Headers FIRST — ~3.1M headers in ~60-74 s from a legacy zclassicd datadir.
  build/bin/zclassic23 --importblockindex ~/.zclassic
  # 2. Then a NORMAL boot — legacy import is on by default; it auto-reads/links
  #    ~/.zclassic and reaches tip. Opt out with -nolegacyimport.
  build/bin/zclassic23
  ```

  This path exists only while the native peer network is small — it reads data
  from the old C++ `zclassicd` (leave it running). Full recipe + the canonical
  authority model: [`docs/SYNC.md`](docs/SYNC.md) "Method 3".

For the current live bootstrap posture and the in-flight sovereign cold-start
cure (fold real bodies forward from a self-minted checkpoint, then delete the
borrowed seed), see [`docs/HANDOFF.md`](docs/HANDOFF.md).

## Claude integration (MCP)

The differentiator: a built-in [MCP](https://modelcontextprotocol.io) server, so
Claude Code queries and operates the node through typed tools — no curl, no log
spelunking.

```bash
claude mcp add zcl23 -- build/bin/zclassic23 -mcp
```

Restart Claude Code and the tools appear. Start with `zcl_status` (height,
peers, sync, onion, health in one call); `zcl_tools_list` enumerates the full
~100-tool catalog. The daily-driver reference is in [`CLAUDE.md`](CLAUDE.md).
MCP is an operator interface (stdio, local client) — don't expose RPC/MCP to
untrusted clients.

## Architecture

Canonical doc: [`docs/FRAMEWORK.md`](docs/FRAMEWORK.md) — the Prime Directive,
the Ten Laws, and the eight lint-enforced code shapes. Diagrams:
[`docs/ARCHITECTURE_DIAGRAMS.md`](docs/ARCHITECTURE_DIAGRAMS.md).

The short version: **an event log is the source of truth, state is rebuilt
through pure projections, and chain progress is a stage cursor on disk** — so
silent halts are unreachable by construction.

```
zclassic23 (~15 MB, static)
├── Full node      P2P 8033, RPC 18232, Equihash 200,9, Sapling
├── Tor            in-process .onion (no SOCKS)
├── MVC            Models (SQLite) · Controllers (C23) · Views (HTML/JSON)
├── Fast sync      FlyClient + SHA3 UTXO snapshot
├── Wallet         transparent + Sapling
└── MCP            ~100 typed tools on stdio (-mcp)
```

## Repository layout

| Dir | Contents |
|-----|----------|
| `src/` | Binary entry points (node, CLI) |
| `app/` | App code in the eight shape folders (models, views, controllers, services, jobs, conditions, events, supervisors) |
| `lib/` | Subsystem libraries (consensus, net, sync, storage, crypto, sapling, script, rpc, util, test harness) |
| `domain/` | Pure domain logic (consensus rules, encodings, wallet primitives — no I/O) |
| `config/` | Composition root: boot sequence + wiring |
| `ports/` · `adapters/` | Hexagonal port interfaces + outbound adapters |
| `tools/` | MCP server, lint gates, fuzzers, simulators, release scripts |
| `docs/` | All documentation |
| `deploy/` | systemd user service + host setup |
| `vendor/` | Vendored deps + Tor submodule |

## Engineering posture

- **Defensive coding is mandatory** and lint-enforced
  ([`docs/DEFENSIVE_CODING.md`](docs/DEFENSIVE_CODING.md)): every write through the
  ActiveRecord lifecycle, every error logs context, every alloc checked, every
  long loop on a supervisor liveness tree.
- **Tests:** `make test` (~423 parallel groups); bugs become 64-bit seeds in a
  deterministic simulator ([`docs/CHAOS_HARNESS.md`](docs/CHAOS_HARNESS.md)).
- **Gates are local:** `make lint` + `make ci` (not GitHub Actions).
- **Deploy builds fresh:** `make deploy` rebuilds the binary and verifies the
  running `build_commit` — never ships stale code.
- **Reproducible builds**, optional GPG signing (`tools/release.sh`).

## Operating

```bash
sudo deploy/setup.sh                 # one-time host setup
make deploy                          # build fresh + install + restart + verify
systemctl --user status zclassic23
tail -f ~/.zclassic-c23/node.log
```

Operator flags (`-externalip`, `-addnode`) go in `~/.config/zclassic23/env` (copy
`deploy/zclassic23.env.example`), not the tracked unit. `zcl-rpc` honors
`ZCL_RPCPORT` (default 18232) and `ZCL_DATADIR` (for the `.cookie`).

**Peer discovery / bootstrap** (no DNS seeders, hardcoded IP + onion seeds,
custom `~/.config/zclassic23/onion-seeds`) is covered in
[First boot](#first-boot--what-a-fresh-node-looks-like); getting to tip is
covered in [Bootstrapping to tip](#bootstrapping-to-tip).

## Troubleshooting

Break-glass checklist. Over MCP each step is a typed tool; over the shell it is a
`zcl-rpc` call. Full operator runbook: [`docs/RUNBOOK.md`](docs/RUNBOOK.md).

**No peers (`peers: 0` stays at 0).**
```bash
zcl-rpc getpeerinfo | jq 'length'      # count peers (MCP: zcl_peers)
zcl-rpc getnetworkinfo | jq .connections
zcl-rpc addnode "IP:PORT" "onetry"     # add a known peer (MCP: zcl_addnode)
ss -tlnp | grep 8033                   # P2P port reachable?
```
Add onion seeds in `~/.config/zclassic23/onion-seeds` (one `.onion` per line) so
the node can harvest peers without DNS or fixed IPs.

**Stuck height (height frozen, not at tip).** A stall is never silent — it is
either a growing tip gap or a named blocker:
```bash
# MCP: zcl_status → check sync.state, header_gap, health.checks, blockers
# MCP: zcl_syncstate (sync phase), zcl_blockers / zcl_state subsystem=supervisor
zcl-rpc syncstate                      # sync FSM state
zcl-rpc healthcheck | jq .checks       # synced / has_peers / tip_stale / tip_lag
```
Look at `blockers` / `dominant_blocker` in `zcl_status` for the named reason. A
transient `sync.state: failed` often clears on `systemctl --user restart
zclassic23`.

**Reading the log.** `node.log` lives in the datadir: `~/.zclassic-c23/node.log`.
```bash
tail -f ~/.zclassic-c23/node.log       # follow live
# MCP: zcl_node_log(pattern="...", since_secs=300, level="warn")
#      server-side reverse scan — no full download, level + regex filter
```

**Boot failure (node won't start).** Look for `EV_BOOT_VALIDATION_FAILED` or a
specific error in `node.log`; the boot stage that refused is named. Recovery
paths and the kill-9 / OOM cases are in [`docs/RUNBOOK.md`](docs/RUNBOOK.md).

## Documentation

- [`CLAUDE.md`](CLAUDE.md) — MCP reference, build/test/deploy, recovery
- [`docs/FRAMEWORK.md`](docs/FRAMEWORK.md) — canonical architecture
- [`docs/MVP.md`](docs/MVP.md) — v1 criteria + honest readiness
- [`docs/SYNC.md`](docs/SYNC.md) · [`docs/RUNBOOK.md`](docs/RUNBOOK.md) — sync + troubleshooting
- [`docs/SECURITY_AND_INTEGRITY.md`](docs/SECURITY_AND_INTEGRITY.md) · [`.github/SECURITY.md`](.github/SECURITY.md) — security
- [`.github/CONTRIBUTING.md`](.github/CONTRIBUTING.md) — build prereqs + contribution contract
- [`docs/BUILD.md`](docs/BUILD.md) — vendored-library sources, versions, build steps

**Issues & changes:** file bugs and features via GitHub Issues (templates
provided); security reports follow [`.github/SECURITY.md`](.github/SECURITY.md).
Consensus changes are declined on principle — see
[`docs/CONSENSUS_PARITY_DOCTRINE.md`](docs/CONSENSUS_PARITY_DOCTRINE.md).

## License

Copyright 2026 Rhett Creighton. Apache License 2.0 — see [`LICENSE`](LICENSE).
Upstream notices (Bitcoin Core, Zcash, zclassicd, Tor, SQLite, secp256k1,
LevelDB, dcrdex) are in [`NOTICE`](NOTICE); concept attributions in
[`docs/ATTRIBUTIONS.md`](docs/ATTRIBUTIONS.md).
