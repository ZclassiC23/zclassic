# ZClassic23

[![license](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![language](https://img.shields.io/badge/language-C23-00599C.svg)](#)
[![status](https://img.shields.io/badge/status-pre--v1-orange.svg)](docs/MVP.md)
[![CI](https://img.shields.io/badge/CI-local%20make%20lint%20(83%20gates)-success.svg)](docs/DEFENSIVE_CODING.md)

One self-contained pure-C23 binary: a full ZClassic node (Equihash 200,9
PoW, Sapling shielded transactions), an embedded Tor onion service, a block
explorer, a shielded wallet, a P2P file marketplace, a ZNAM on-chain name
registry, ZCL messaging, cross-chain atomic-swap scaffolding, and a native
command registry that lets an AI agent operate the node through ~130 typed
commands (`zclassic23 <command>`) — a **personal sovereignty stack**: a
secure personal-computing OS whose only trust foundation is the ZClassic
proof-of-work network and the compiled binary itself, with no DNS, CAs, or
registries anywhere in the path.

**One binary, one onion, one stack — your sovereign personal computing surface.**

## Status

**Pre-v1 — not yet production-ready.** Of the eight v1 acceptance criteria in
[`docs/MVP.md`](docs/MVP.md), four pass their local operator proof (MRS 4/8);
none are yet end-to-end CI-verified across a live soak. Don't rely on it as your
only mainnet node yet.

It runs on ZClassic mainnet on the `zclassicd` consensus floor, but the public
canonical node is currently **wedged below tip** (verify the live H\* via
`zcl_status` / `dumpstate reducer_frontier`; [`docs/HANDOFF.md`](docs/HANDOFF.md)
holds current state) by incomplete historical
shielded anchors/nullifiers. A borrowed snapshot previously brought its
transparent state to tip. Its anchor hash is checked against a validated local
header, but ZClassic headers do not commit the snapshot's UTXO or shielded-state
contents; do not call those contents consensus- or PoW-bound. The **sovereign
cold-start cure** folds real block bodies forward from the in-binary SHA3/PoW
checkpoint (`core/chainparams/src/checkpoints.c`, h=3,056,758) to independently
derive complete transparent, Sapling, Sprout, and nullifier state, then installs
it atomically and passes copy proof before canonical deployment. A full-history
fold producer is running toward that checkpoint now; the cure is **in flight,
not complete** — do not treat any current soak time as clean evidence. Its
design is in
[`docs/work/self-verified-tip-plan.md`](docs/work/self-verified-tip-plan.md). For
current live state, ask the running node (`zclassic23 status`) or read
[`docs/HANDOFF.md`](docs/HANDOFF.md) — this file does not track it. The other known
soft spot: **off-chain ZMSG is plaintext on the wire**.

It is operator-owned full-node infrastructure: embedded Tor publishes *your* onion
service, wallet state stays in your datadir, and native commands are a typed
local operator interface. Safety boundary and integrity checks:
[`docs/SECURITY_AND_INTEGRITY.md`](docs/SECURITY_AND_INTEGRITY.md).

## What's on board

A complete rewrite of zclassicd in pure C23. One binary is at once a full node
(Equihash PoW, ECDSA scripts, Sprout/Sapling zk-SNARKs, history validates
identically to zclassicd), a fast-sync server (FlyClient MMB + SHA3 UTXO
snapshot — *the ~1-minute cold sync this enables is a design target, not the
proven path today; see [Bootstrapping to tip](#bootstrapping-to-tip)*), an
in-process Tor hidden service (`-tor`), a block explorer (`/explorer` + `/api`,
served over the onion or HTTPS — see [Block explorer](#block-explorer)), a
shielded wallet (transparent + Sapling), and a native command registry. Full
subsystem catalog in [`CLAUDE.md`](CLAUDE.md).

Honestly labeled: **ZNAM** name registry (working); **ZMSG** messaging (on-chain
shielded; off-chain P2P is plaintext on the wire); **ZCL Market** + **ZSWP**
atomic swaps (scaffolding — no settlement yet); **P2P games** (ping +
TicTacToe).

### The OS model: a layered immutable machine

zclassic23 is organized as a small stack of storage regions with a strict
trust ladder — every arrow below is a SHA3 verify; a mismatch is a named
blocker, never a silent failure:

```
 mutable    TIP RING      mempool / peers / wallet journal — small, delta-replayable
            DELTA         anchor→tip full-validation fold — the only re-done work
 ─────────────────────── finalized frontier ───────────────────────────────────────
 immutable  SEALED STATE  base bundle @ anchor + independent replay receipt
                          (coins, Sprout/Sapling anchors, nullifiers) — re-derived
                          from the datadir's own tables, read via a capability fd
            SEALED        chain_segment store: write-once 0444 segment files,
            HISTORY       SHA3-committed, with a manifest root
 ROM        TRUST ROOT    in-binary SHA3/PoW checkpoint (h=3,056,758) + the binary
                          itself, sealed in `core/` (`core/MANIFEST.sha3`)
```

Every trust claim reduces to two things: the compiled binary and the
PoW-heaviest header chain — no DNS, CAs, or registries in the path. The
**sovereign cure** (see [Status](#status)) is the work item that makes the
sealed-state layer complete and independently derived rather than borrowed
from an external `zclassicd` snapshot.

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
make test           # full suite (631 parallel groups)
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
> `zclassic23 status`).

Datadir `~/.zclassic-c23/` (`-datadir=DIR`). Default ports: P2P `8033`, RPC
`18232`. On the operator host, `zclassic23` owns the canonical public P2P port
`8033`; the co-located legacy `zclassicd` oracle is isolated on P2P `8034` and
RPC `8232`. The authoritative lane/port table is in [`docs/HANDOFF.md`](docs/HANDOFF.md).
The 2026-07-04 live incident runbook was removed from the tree — recover it
with `git log --follow -- docs/work/archive/live-node-ops-2026-07-04.md`.

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

One native call answers it: `build/bin/zclassic23 status`. It returns one line;
`--format=json` returns the typed `zcl.result.v1` envelope with a compact
`zcl.core_status_brief.v1` body. The key fields are:

| Field | Fresh / syncing | Synced (at tip) |
|---|---|---|
| `hstar` | `0`, then rising | validated served frontier |
| `header_height` / `gap` | validated target and distance remaining | equal / `0` |
| `sync_state` | `finding_peers` → `headers_download` → `blocks_download` | `at_tip` |
| `peer_count` / `peer_best` | peer availability and advisory height | several / near tip |
| `healthy` / `serving` | health is false while blocked; serving may remain true at the proven frontier | true |
| `primary_blocker` | the causal named blocker | `none` |

A stall is never silent: `gap` grows or `primary_blocker` names the cause.
Use `zclassic23 core status --format=json` only when the larger diagnostic tree
is actually needed.

## Bootstrapping to tip

**Legacy assisted starter pack (isolated/copy lanes only).** A historical
prebuilt block index plus digest-verified UTXO snapshot exists (the
[`starterpack-3155842`](https://github.com/ZclassiC23/zclassic/releases/tag/starterpack-3155842)
release). At boot the node recomputes its SHA3 body hash and checks the claimed
anchor height/hash against the validated local header chain. That detects
changed bytes and the wrong chain location; it does not prove UTXO or shielded
contents. Stable starter-pack publication is currently disabled.

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

The borrowed seed may initialize the held frontier near **3,155,842**, but the
current fail-closed shielded-history gate intentionally stops at the first
spend whose anchor/nullifier prefix is unproven. Do not expect this v2 artifact
to reach tip under the current safety posture. See
[`docs/BOOTSTRAPPING.md`](docs/BOOTSTRAPPING.md).

**Other paths:**

- **Plain start, no starter pack** — the from-genesis P2P path is the sovereignty
  target, but its current end-to-end time/completeness claim is not proven. The
  proven recovery floor remains header import plus normal boot with a local
  `zclassicd` archive.
- **Native P2P fast sync (designed, not yet the everyday proof):** pull the
  digest-verified snapshot directly from another zclassic23 peer. FlyClient/MMB
  authenticates advertised header work, not peer UTXO contents; this remains an
  assisted-readiness design, not a sovereign minute-sync claim. Details:
  [`docs/SYNC.md`](docs/SYNC.md) "Method 1".
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

The differentiator: a native command registry built into the binary — the
**primary** agent surface — so an AI agent queries and operates the node
through typed commands, no curl, no log spelunking, no separate server
process. It is the ABI: **~130 typed command leaves** (178 catalog entries
counting branches) across 7 registry files
(`config/commands/{root,core,app,dev,ops,accounts,code}.def`) under
`core.*`/`app.*`/`ops.*`/`dev.*`/`discover.*`/`code.*`, each a typed
`zcl_command_spec` (input/output schema, a one-line output **semantics**
contract, a per-leaf response **byte budget**, auth, risk, latency, cost)
validated fail-closed at every startup — so responses are self-describing and
bounded, and no error reply lacks a `next` action to run.

```bash
build/bin/zclassic23 status
build/bin/zclassic23 dumpstate supervisor
build/bin/zclassic23 discover help
build/bin/zclassic23 code map          # source-code navigator
```

Start with `status` (height, peers, sync, blocker, health in one call);
`discover help` / `discover search <q>` enumerates the full command
catalog. Full doc: [`docs/NATIVE_COMMAND_INTERFACE.md`](docs/NATIVE_COMMAND_INTERFACE.md);
daily-driver reference in [`CLAUDE.md`](CLAUDE.md).

The typed native command registry is the sole AI/operator surface. The legacy
MCP stdio server has been removed.

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
  node's `.onion` (visible via `zclassic23 status`). No certificate needed.
- **Over HTTPS on clearnet** — drop a TLS certificate and key at
  `<datadir>/ssl/fullchain.pem` and `<datadir>/ssl/privkey.pem`. The HTTPS
  explorer then starts once the node is near tip (default port `8443`). Without a
  cert the node logs `HTTPS: no cert … block explorer not on clearnet` and skips
  it — this is expected on a default build.

A default build (Tor stub, no cert) intentionally has **no public explorer
endpoint**. Use the native command registry or `zcl-rpc` for node data in that
configuration.

## Architecture

Canonical doc: [`docs/FRAMEWORK.md`](docs/FRAMEWORK.md) — the Prime Directive,
the Ten Laws, and the eight lint-enforced code shapes. Diagrams:
[`docs/ARCHITECTURE_DIAGRAMS.md`](docs/ARCHITECTURE_DIAGRAMS.md).

The short version: **an event log is the source of truth, state is rebuilt
through pure projections, and chain progress is a stage cursor on disk** — so
silent halts are unreachable by construction.

```
zclassic23 (single static binary)
├── Full node      P2P 8033, RPC 18232, Equihash 200,9, Sapling
├── Tor            in-process .onion (no SOCKS)
├── MVC            Models (SQLite) · Controllers (C23) · Views (HTML/JSON)
├── Fast sync      FlyClient + SHA3 UTXO snapshot
├── Wallet         transparent + Sapling
└── Native cmds    ~130 typed command leaves (`zclassic23 <cmd>`)
```

## Security posture

- **Sealed consensus core:** `core/` (checkpoints, chain params, consensus
  math) is pinned to a SHA3-256 manifest (`core/MANIFEST.sha3`); any byte
  change fails the HARD lint gate `check-core-seal` unless it goes through the
  documented owner unseal ritual (`core/UNSEAL.md`).
- **Steady-state sandbox:** `-sandbox=steady` applies `no_new_privs`,
  `PR_SET_DUMPABLE(0)`, Landlock datadir grants, and a seccomp deny-list
  installed atomically across **every running thread** (seccomp `TSYNC`, so the
  already-spawned P2P/RPC/validation threads are covered, not just new ones),
  entered as the last boot stage before the node reports ready; thread coverage
  witnessed via `dumpstate sandbox`.
- **No shell-outs:** zero `system()`/`popen()` in shipped app/lib/config code
  (lint-enforced).
- **Wallet keystore:** AES-256-GCM at-rest encryption for new wallets; an
  existing plaintext wallet still loads with a warning (encryption isn't yet
  the enforced default for pre-existing datadirs).
- **Capability-fd discipline:** privileged reads (the replay receipt, the
  consensus-bundle exporter) go through capability file descriptors, not bare
  pathnames.
- **`zcl_sql`** is SELECT-only, semicolon-rejected, auto-LIMIT, and denies a
  set of wallet-secret tables/columns by name.
- **83 lint gates** (`make lint`) enforce these and the defensive-coding
  rules below on every change. Full list:
  [`docs/DEFENSIVE_CODING.md`](docs/DEFENSIVE_CODING.md); safety boundary:
  [`docs/SECURITY_AND_INTEGRITY.md`](docs/SECURITY_AND_INTEGRITY.md).

Known gaps: off-chain P2P messaging is plaintext on the wire (Noise-based
transport encryption is designed, not yet wired); the wallet-encryption
default doesn't yet apply retroactively to existing plaintext wallets.

## Repository layout

| Dir | Contents |
|-----|----------|
| `src/` | Binary entry points (node, CLI) |
| `core/` | Sealed consensus core (checkpoints, chain params, consensus math) — SHA3-manifest pinned, HARD lint gate |
| `app/` | App code in the eight shape folders (models, views, controllers, services, jobs, conditions, events, supervisors) |
| `lib/` | Subsystem libraries (consensus, net, sync, storage, crypto, sapling, script, rpc, util, test harness) |
| `domain/` | Pure domain logic (consensus rules, encodings, wallet primitives — no I/O) |
| `config/` | Composition root: boot sequence + wiring; `config/commands/*.def` is the native command registry |
| `ports/` · `adapters/` | Hexagonal port interfaces + outbound adapters |
| `tools/` | Developer tools, lint gates, fuzzers, simulators, release scripts |
| `docs/` | All documentation |
| `deploy/` | systemd user service + host setup |
| `vendor/` | Vendored deps + Tor submodule |

## Engineering posture

- **Defensive coding is mandatory** and lint-enforced
  ([`docs/DEFENSIVE_CODING.md`](docs/DEFENSIVE_CODING.md)): every write through the
  ActiveRecord lifecycle, every error logs context, every alloc checked, every
  long loop on a supervisor liveness tree.
- **Tests:** `make test` (631 registered parallel groups); bugs become 64-bit
  seeds in a
  deterministic simulator ([`docs/CHAOS_HARNESS.md`](docs/CHAOS_HARNESS.md)).
- **Crash recovery is demonstrable:** `make test-crash-bootstrap` runs a
  hermetic kill-9 / restart harness (`tools/crash_recovery_test.c`, isolated
  self-seeded datadir) that proves the node folds back to its tip after being
  killed mid-write — no manual repair.
- **Gates are local:** `make lint` + `make ci` (not GitHub Actions).
- **Deploy builds fresh:** `make deploy` rebuilds the binary and verifies the
  running `build_commit` — never ships stale code.
- **Release work is contained:** deterministic flags and legacy GPG-capable
  packaging exist, but stable publication waits for exact-candidate evidence,
  two-builder byte identity, and required offline signatures. Unsigned output is
  local-development-only.

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

Break-glass checklist. Prefer the typed native commands; use direct RPC for
protocol-level detail. Full operator runbook: [`docs/RUNBOOK.md`](docs/RUNBOOK.md).

**No peers (`peers: 0` stays at 0).**
```bash
build/bin/zclassic23 status
build/bin/zclassic23 core network peers list
build/bin/zclassic-cli getnetworkinfo       # connection summary
build/bin/zclassic-cli addnode "IP:PORT" "onetry"
ss -tlnp | grep 8033                        # P2P port reachable?
```
Add onion seeds in `~/.config/zclassic23/onion-seeds` (one `.onion` per line) so
the node can harvest peers without DNS or fixed IPs.

**Stuck height (height frozen, not at tip).** A stall is never silent — it is
either a growing tip gap or a named blocker:
```bash
build/bin/zclassic23 status
build/bin/zclassic23 core sync diagnose
build/bin/zclassic23 dumpstate reducer_frontier
build/bin/zclassic23 dumpstate supervisor
build/bin/zclassic23 core sync status  # sync FSM state
build/bin/zclassic23 ops health        # synced / has_peers / tip_stale / tip_lag
```
Look at `blockers` / `dominant_blocker` in `zclassic23 status` for the named reason. A
transient `sync.state: failed` often clears on `systemctl --user restart
zclassic23`.

**Reading the log.** `node.log` lives in the datadir: `~/.zclassic-c23/node.log`.
```bash
tail -f ~/.zclassic-c23/node.log       # follow live
build/bin/zclassic23 ops logs --pattern='error|warn'
```

**Boot failure (node won't start).** Look for `EV_BOOT_VALIDATION_FAILED` or a
specific error in `node.log`; the boot stage that refused is named. Recovery
paths and the kill-9 / OOM cases are in [`docs/RUNBOOK.md`](docs/RUNBOOK.md).

## Documentation

- [`CLAUDE.md`](CLAUDE.md) — agent commands, build/test/deploy, recovery
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
