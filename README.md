# ZClassic23

[![license](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![language](https://img.shields.io/badge/language-C23-00599C.svg)](#)
[![status](https://img.shields.io/badge/status-pre--v1-orange.svg)](docs/MVP.md)
[![CI](https://img.shields.io/badge/CI-local%20make%20ci%20(40%2B%20gates)-success.svg)](docs/DEFENSIVE_CODING.md)

One self-contained ~15 MB pure-C23 binary: a full ZClassic node (Equihash 200,9
PoW, Sapling shielded transactions), an embedded Tor onion service, a block
explorer, a shielded wallet, and a native command registry that lets an AI
agent operate the node through ~100 typed commands (`zclassic23 <command>`).

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
in-process Tor hidden service (`-tor`), a block explorer (`/explorer` + `/api`,
served over the onion or HTTPS — see [Block explorer](#block-explorer)), a
shielded wallet (transparent + Sapling), and an MCP server. Full subsystem
catalog in [`CLAUDE.md`](CLAUDE.md).

Honestly labeled: **ZNAM** name registry (working); **ZMSG** messaging (on-chain
shielded; off-chain P2P is plaintext on the wire); **ZCL Market** + **ZSWP**
atomic swaps (scaffolding — no settlement yet); **P2P games** (ping +
TicTacToe).

## Quick start

Public start here: [`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md) is the
fresh-machine path for people cloning from GitHub. This README is the overview;
`docs/HANDOFF.md` and `docs/RUNBOOK.md` are maintainer/operator documents for
the project's own hosted lanes.

**Prerequisites:** gcc 14+ (or clang with `-std=c23`), GNU make, plus
`cmake`, `autoconf`, `patch`, `cargo` + `rustc`, `curl`/`wget`, and `unzip` for the one-time vendored-library
build. **The first build needs internet** — it fetches pinned third-party source
tarballs (OpenSSL, libevent, LevelDB, zlib, SQLite, and the canonical Zcash
Sapling prover) and verifies them against
pinned SHA-256s before compiling locally; afterward the archives are cached in
`vendor/lib/` and builds are offline. A clean `make vendor && make` takes ~1–2
minutes on a modern multi-core box.

```bash
git clone https://github.com/ZclassiC23/zclassic.git && cd zclassic
make                # node + CLI + RPC tool -> build/bin/{zclassic23,zclassic-cli,zcl-rpc}
make fast-rebuild   # changed-file dev compile + non-LTO local node link
make dev-bin        # fast non-LTO local node -> build/bin/zclassic23-dev
make test           # full suite (508 parallel groups)
make lint           # defensive-coding gates
```

(`make zclassic23` builds only the node; plain `make` also builds the
`zclassic-cli` / `zcl-rpc` clients used in the examples below.) The first build
auto-runs **`make vendor`**, which builds the static
third-party archives in `vendor/lib/` from source (OpenSSL, libevent ×3, LevelDB,
SQLite, zlib, librustzcash) plus the in-tree Tor stub — sources are pulled from pinned URLs and
verified against pinned SHA256 hashes, then compiled locally. Only
`libsecp256k1.a` (a custom Bitcoin Core fork build) ships committed. `make vendor`
is provenance-idempotent: it skips only archives whose bytes, source pin,
recipe, toolchain, and dependencies match their deterministic stamp
(`make vendor-force` rebuilds all fetched archives). Per-library sources,
versions, and hashes are in
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
`18232`. On the operator host, `zclassic23` owns the canonical public P2P port
`8033`; the co-located legacy `zclassicd` oracle is isolated on P2P `8034` and
RPC `8232`. The authoritative lane/port table is in [`docs/HANDOFF.md`](docs/HANDOFF.md)
and the live incident runbook is in [`docs/work/archive/live-node-ops-2026-07-04.md`](docs/work/archive/live-node-ops-2026-07-04.md).

## First boot — what a fresh node looks like

A brand-new datadir is honestly empty. It does **not** report a fake height:

- `getblockcount` returns **`0`** until blocks are actually folded — no phantom
  tip. `getblockchaininfo` returns `blocks: 0, headers: 0,
  initialblockdownload: true` (best-block resolves to genesis).
- **Peer discovery has no DNS seeders** — the historical ZCL DNS names no longer
  resolve. The node bootstraps from hardcoded legacy ZClassic IP seeds (10 addresses)
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
| `sync_gap` / `sync_behind` | validated-header gap / thresholded boolean | `0` / `false` |
| `header_gap` | peer-advertised header hint (untrusted; `null` without a claim) | normally `0` |
| `health.healthy` | `false` while catching up | `true` |
| `health.checks.has_peers` | `false` | `true` |
| `health.checks.onion_address` | absent until Tor onion is up | present (`…onion`) |
| `blockers.active_count` / `dominant_blocker` | `0` / `null` on a healthy boot | `0` / `null` |

Illustrative `zcl_status` output (**example values, not a live capture** —
large diagnostic subtrees trimmed):

```jsonc
// Fresh node, still finding peers:
{
  "height": 0, "target_height": 0, "sync_gap": 0,
  "sync_behind": false, "header_gap": null, "peers": 0,
  "connections": {"known":true,"total":0,"inbound":0,"outbound":0,"zcl23":0,"magicbean":0},
  "sync":   {"state":"finding_peers","state_id":1},
  "health": {"healthy":false,
             "checks":{"synced":false,"has_peers":false,"tor_ready":false,
                       "peer_count":0,"tip_lag":0}},
  "blockers": {"active_count":0,"dominant":null}, "dominant_blocker": null
}

// Synced node at tip, onion up:
{
  "height": 3160247, "target_height": 3160247, "sync_gap": 0,
  "sync_behind": false, "header_gap": 0, "peers": 8,
  "connections": {"known":true,"total":8,"inbound":2,"outbound":6,"zcl23":3,"magicbean":5},
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
as a growing authoritative `sync_gap` or a named entry in `blockers` /
`dominant_blocker`. A single peer can influence `header_gap`, so treat that
field only as an explicitly untrusted download hint. Missing target evidence is
reported as `null` plus an error, never as a synthetic zero.

## Bootstrapping to tip

**Fast path for a fresh clone — tip in one sitting (~minutes).** Download a
published, prebuilt block index plus a SHA3-self-verified UTXO snapshot (the
[`starterpack-3155842`](https://github.com/ZclassiC23/zclassic/releases/tag/starterpack-3155842)
release), drop both into the datadir, and boot. The snapshot is **not blindly
trusted**: at boot the node recomputes its SHA3 body hash and checks its anchor
block hash against the PoW header compiled into the binary — a tampered or
wrong-chain snapshot is refused (`config/src/boot_refold_staged.c`).

```bash
# 1. Download both assets from the release (block_index.bin 543 MB + snapshot 105 MB)
gh release download starterpack-3155842 -R ZclassiC23/zclassic
#    (or curl the two direct URLs listed in docs/BOOTSTRAPPING.md)

# 2. Verify integrity — must print OK for both
sha256sum -c <<'EOF'
a40b184d0d52f91438762928abdadd151a8011efc0340485c690732988d5d6e0  block_index.bin
46e4f6bd090e51417a4d8b70a1b7c8a218d9c8e3cded1bba812033117f5d9e9f  utxo-seed-3155842.snapshot
EOF

# 3. Drop BOTH into a fresh datadir and boot, pointing the loader at the snapshot
DATADIR="$HOME/.zclassic-c23"
mkdir -p "$DATADIR" && mv block_index.bin utxo-seed-3155842.snapshot "$DATADIR/"
build/bin/zclassic23 -datadir="$DATADIR" \
  -load-snapshot-at-own-height="$DATADIR/utxo-seed-3155842.snapshot"
```

`getblockcount` jumps to ~**3,155,842** within seconds, then climbs to the
network tip in minutes as the node folds forward over P2P block bodies. Full
walkthrough + expected boot log: [`docs/BOOTSTRAPPING.md`](docs/BOOTSTRAPPING.md).

**Other paths:**

- **Plain start, no starter pack** — a fresh node syncs honestly from genesis
  over P2P; fully trustless but **long** (~hours). This is the default if you
  skip the starter pack.
- **Native P2P fast sync (designed, not yet the everyday proof):** pull the
  SHA3-verified snapshot directly from another zclassic23 peer (FlyClient/MMB
  proof bound to the PoW chain), targeting tip in ~a minute once the native peer
  network is established. Details: [`docs/SYNC.md`](docs/SYNC.md) "Method 1".
- **From a local `zclassicd` (~25 min, dev bootstrap):** if you already run the
  C++ node, import headers first, then boot:

  ```bash
  build/bin/zclassic23 --importblockindex ~/.zclassic   # headers FIRST (~60-74 s)
  build/bin/zclassic23                                  # then a normal boot
  ```

  Order matters: skipping step 1 leaves a ~3.1M-header hole and the node pins.
  Leave `zclassicd` running. Full recipe: [`docs/SYNC.md`](docs/SYNC.md) "Method 3".

For the live bootstrap posture and the in-flight sovereign cold-start cure (fold
real bodies forward from a self-minted checkpoint, then delete the borrowed
seed), see [`docs/HANDOFF.md`](docs/HANDOFF.md).

## Claude integration

The differentiator: a native command registry built into the binary, so an AI
agent queries and operates the node through typed commands — no curl, no log
spelunking, no separate server process.

```bash
build/bin/zclassic23 status
build/bin/zclassic23 dumpstate supervisor
build/bin/zclassic23 discover help
```

Start with `status` (height, peers, sync, onion, health in one call);
`discover help` / `discover search <q>` enumerates the full ~100-command
catalog. The daily-driver reference is in [`CLAUDE.md`](CLAUDE.md).

**Legacy MCP server (still runs today, removed in W3):** the owner directive
is zero-MCP — see [`docs/work/MCP-REMOVAL-PLAN.md`](docs/work/MCP-REMOVAL-PLAN.md).
Until then, `build/bin/zclassic23 -mcp` (or `claude mcp add zcl23 --
build/bin/zclassic23 -mcp`) still exposes the same surface as typed MCP
tools. MCP/native are both operator interfaces (stdio, local client) — don't
expose RPC/MCP/native to untrusted clients.

## Block explorer

The node serves a web block explorer (`/explorer`, with a JSON API under `/api`).
It is **not** on the RPC port (`18232`) — a plain `GET` there returns
`405 Method Not Allowed`, by design. The explorer is reachable two ways:

Start API discovery at `/api/v1`. Use `/api/v1/service-catalog` to see what the
node can host, advertise, verify, or construct for users, and
`/api/v1/service-catalog/{service}` for one service contract.
`/api/v1/protocols` lists ZCL application-protocol contracts, and
`/api/v1/bootstrap` checks whether the node is currently useful for fresh-peer
bootstrap.

- **Over the onion service** — build the bundled Tor fork (see the opt-in note in
  [Quick start](#quick-start)) and run `-tor`; the explorer is then served on the
  node's `.onion` (visible via `zcl_status`). No certificate needed.
- **Over HTTPS on clearnet** — drop a TLS certificate and key at
  `<datadir>/ssl/fullchain.pem` and `<datadir>/ssl/privkey.pem`. The HTTPS
  explorer then starts once the node is near tip (default port `8443`). Without a
  cert the node logs `HTTPS: no cert … block explorer not on clearnet` and skips
  it — this is expected on a default build.

A default build (Tor stub, no cert) intentionally has **no public explorer
endpoint**. Use MCP / `zcl-rpc` for node data in that configuration.

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
└── Native cmds    ~100 typed commands (`zclassic23 <cmd>`; legacy MCP on stdio via -mcp, removed W3)
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
- **Tests:** `make test` (488 parallel groups); bugs become 64-bit seeds in a
  deterministic simulator ([`docs/CHAOS_HARNESS.md`](docs/CHAOS_HARNESS.md)).
- **Crash recovery is demonstrable:** `make test-crash-bootstrap` runs a
  hermetic kill-9 / restart harness (`tools/crash_recovery_test.c`, isolated
  self-seeded datadir) that proves the node folds back to its tip after being
  killed mid-write — no manual repair.
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
build/bin/zclassic23 agent                  # compact status; MCP: zcl_agent
build/bin/zclassic-cli getpeerinfo          # connected peers; MCP: zcl_peers
build/bin/zclassic-cli getnetworkinfo       # connection summary
build/bin/zclassic-cli addnode "IP:PORT" "onetry"
ss -tlnp | grep 8033                        # P2P port reachable?
```
Add onion seeds in `~/.config/zclassic23/onion-seeds` (one `.onion` per line) so
the node can harvest peers without DNS or fixed IPs.

**Stuck height (height frozen, not at tip).** A stall is never silent — it is
either a growing tip gap or a named blocker:
```bash
# MCP: zcl_status → check sync.state, sync_gap, health.checks, blockers
# MCP: zcl_syncstate (sync phase), zcl_blockers / zcl_state subsystem=supervisor
build/bin/zclassic23 agent             # compact status and named blocker
build/bin/zclassic23 agentops          # no-jq command center and next work
build/bin/zclassic-cli syncstate       # sync FSM state
build/bin/zclassic-cli healthcheck     # synced / has_peers / tip_stale / tip_lag
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
- [`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md) — public fresh-machine first run
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
