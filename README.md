# ZClassic23

[![license](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

One self-contained ~15 MB pure-C23 binary: a full ZClassic node (Equihash
200,9 proof-of-work, Sapling shielded transactions), an embedded Tor onion
service, a block explorer, a shielded wallet, and a built-in MCP server that
lets an AI agent operate the node through ~100 typed tools.

One binary, one onion, one stack.

## Status — active stabilization

ZClassic23 is a work in progress and is **not production-ready**. The node
is in active stabilization. Live sync stays at tip (hash-identical to a
local zclassicd reference at every height checked), blocks publish as they
arrive, and restarts keep the connected chain — but the v1 acceptance
criteria in [`docs/MVP.md`](docs/MVP.md) are not yet CI-enforced and no
multi-day soak has been completed. Do not rely on this build as a mainnet
node until they are.

ZClassic23 is operator-owned full-node infrastructure. Its security model
emphasizes local control: embedded Tor publishes the operator's own onion
service, wallet state stays in the operator datadir, MCP provides a typed local
operator interface, and fuzz/chaos harnesses run against isolated fixtures to
harden recovery. The safety boundary and concrete integrity checks are
documented in
[`docs/SECURITY_AND_INTEGRITY.md`](docs/SECURITY_AND_INTEGRITY.md).

## What is this?

A complete rewrite of zclassicd in pure C23. Every node is simultaneously:

- **A full ZClassic node** — syncs headers, validates blocks (Equihash PoW,
  ECDSA scripts, Sprout/Sapling zk-SNARK proofs), relays transactions, and
  serves peers on the ZClassic P2P network (port 8033). Historical chain
  validation matches legacy zclassicd; an ongoing consensus-parity audit
  tracks the forward edge.
- **A fast-sync server** — FlyClient MMB proofs plus SHA3-committed UTXO
  snapshots let a fresh node reach an operational tip in about a minute,
  with optional background re-validation of every historical proof and
  signature.
- **A Tor hidden service** — Tor is compiled into the binary. With `-tor`,
  the node creates a `.onion` address and serves the explorer and REST API
  over Tor circuits via in-process calls (no SOCKS, no exposed ports).
- **A block explorer** — charts, HODL-wave analysis, ZSLP token scanner,
  and a JSON REST API at `/explorer` and `/api`.
- **A shielded wallet** — transparent + Sapling shielded addresses, served
  over RPC and MCP.
- **An MCP server** — Claude (or any MCP client) operates the node as a
  first-class operator. See [Claude integration](#claude-integration-mcp).

Also on board, in varying states of completeness (labels match the
internal docs — scaffolding is scaffolding):

- **ZNAM** — on-chain name registry (OP_RETURN, multi-coin resolution,
  text records). Working.
- **ZMSG** — messaging: on-chain via the Sapling encrypted memo field
  (shielded); off-chain P2P messages are currently **plaintext on the
  wire** (transport encryption not yet implemented).
- **ZCL Market** — file-marketplace **scaffolding**: offer gossip and
  proof-of-possession challenges exist; file transfer and payment
  settlement are not yet implemented.
- **ZSWP atomic swaps** — HTLC contract **scaffolding** for BTC/LTC/DOGE:
  initiation and participation with redeem-script generation; redemption,
  refund, and settlement are not yet wired to broadcast.
- **P2P games** — latency ping + TicTacToe over a binary wire protocol,
  as an extensible framework.

## Quick start

### Prerequisites

- **gcc 14+** (or a clang with working `-std=c23` support) and GNU make.
- Vendored static libraries under `vendor/lib/` (LevelDB, SQLite, libevent,
  OpenSSL, zlib, Tor). Only `libsecp256k1.a` is tracked in-tree today; the
  remaining archives must currently be built locally and dropped into
  `vendor/lib/`. Packaging them for fresh clones is a known gap — expect
  friction building outside the maintainer's environment for now.

### Build and test

```bash
git clone https://github.com/ZclassiC23/zclassic.git
cd zclassic
make zclassic23     # main binary at build/bin/zclassic23
make test           # 860+ test cases across 350+ test files
make lint           # defensive-coding gates (see docs/DEFENSIVE_CODING.md)
```

Other targets: `make zcl-rpc` (CLI RPC client), `make zcl-nodectl` (node
lifecycle tool), `make deploy` (install the systemd user service),
`make ci` (local full gate: lint + tests + MVP slices + fuzz where available).

### Run

```bash
build/bin/zclassic23                              # start node
build/bin/zclassic23 -tor                         # with .onion hidden service
build/bin/zclassic23 --importblockindex ~/.zclassic   # fast header import from legacy zclassicd data
                                                  # (then a normal boot auto-links ~/.zclassic;
                                                  #  opt out with -nolegacyimport)
build/bin/zclassic23 -addnode=74.50.74.102        # connect to a seed node
```

Data directory: `~/.zclassic-c23/` (override with `-datadir=DIR`). Default
ports: P2P `8033`, RPC `18232`.

## Claude integration (MCP)

The differentiator: zclassic23 ships a built-in
[Model Context Protocol](https://modelcontextprotocol.io) server, so Claude
Code can query and operate the node directly through typed tools — no curl,
no log spelunking.

```bash
claude mcp add zcl23 -- build/bin/zclassic23 -mcp
```

Restart Claude Code and the tools appear automatically. The daily drivers:

| Tool | What it does |
|------|--------------|
| `zcl_status` | Height, peers, sync, onion address, health — one call |
| `zcl_state` | Generic state dump of any subsystem (supervisor, watchdog, boot, …) |
| `zcl_node_log` | Server-side regex tail of `node.log` |
| `zcl_sql` | SELECT-only SQL over the node database (rate-gated) |
| `zcl_rpc` | Escape hatch into 85+ raw JSON-RPC methods |
| `zcl_tools_list` | Enumerate the full live tool catalog (~100 tools) |

Wallet, mining, ZNAM, ZMSG, swaps, metrics, and admin operations are typed
tools too — see the MCP section of [`CLAUDE.md`](CLAUDE.md) for the full
reference.

The MCP server is an operator interface. In the default `-mcp` mode it runs on
stdio for a local client; destructive tools are explicit in the route table and
middleware-gated, and diagnostic SQL is SELECT-only and rate-limited. Do not
expose RPC or MCP transports to untrusted clients.

## Architecture

The canonical architecture document is
[`docs/FRAMEWORK.md`](docs/FRAMEWORK.md) — the Prime Directive, the Ten
Laws of Beauty, and the eight code shapes that every source file under
`app/` must live in (lint-enforced). Current subsystem and boot topology
diagrams are in
[`docs/ARCHITECTURE_DIAGRAMS.md`](docs/ARCHITECTURE_DIAGRAMS.md); the
rationale for the personal-sovereignty-stack pivot is in
[`docs/adr/0001-personal-sovereignty-stack.md`](docs/adr/0001-personal-sovereignty-stack.md).

The short version: an event log is the source of truth, state is rebuilt
through pure projections, and chain progress is a stage cursor on disk —
so silent halts are unreachable by construction.

```
zclassic23 binary (~15 MB, statically linked)
├── Full node         P2P 8033, RPC 18232, Equihash 200,9, Sapling
├── Tor (embedded)    .onion hosting via dynhost (in-process, no SOCKS)
├── MVC framework     Models (SQLite), Controllers (C23), Views (HTML/JSON)
├── Block explorer    /explorer routes + /api REST endpoints
├── Fast sync         FlyClient + SHA3 UTXO snapshot transfer
├── Wallet            transparent + Sapling shielded
└── MCP server        ~100 typed tools on stdio (-mcp)
```

## Repository layout

| Directory | What lives there |
|-----------|------------------|
| `src/` | Binary entry points: `main.c` (the node) and `cli.c` (the CLI client) |
| `app/` | Application code in the eight lint-enforced shape folders: `models/`, `views/`, `controllers/`, `services/`, `jobs/`, `conditions/`, `events/`, `supervisors/` |
| `lib/` | Subsystem libraries: consensus, net, sync, storage, crypto, sapling, script, rpc, kernel, util, the test harness, and more |
| `domain/` | Pure domain logic — consensus rules, encodings, wallet primitives (no I/O) |
| `application/` | Hexagonal application-layer scaffold (currently empty, reserved) |
| `ports/` | Port interfaces (storage, clock, event-emitter, …) for the hexagonal seam |
| `adapters/` | Outbound adapters implementing those ports (SQLite, block-log files) |
| `config/` | Composition root: the boot sequence and subsystem wiring (`boot.c` and friends) |
| `tools/` | Developer + MCP tooling: the MCP server, lint gates (`tools/lint`, `tools/scripts`), fuzzers, simulators, soak/release scripts |
| `docs/` | All documentation, including the contributor docs (`DEFENSIVE_CODING.md`, `BOOT_INVARIANTS.md`, `LEGACY_LIFECYCLE.md`, `ATTRIBUTIONS.md`) |
| `deploy/` | systemd user service, host setup script, environment example |
| `tests/` | Test fixtures (block fixtures); the test code itself lives in `lib/test/` |
| `db/` | Canonical SQLite schema (`schema.sql`) |
| `vendor/` | Vendored dependency headers and static libraries; Tor as a submodule |

## Engineering posture

- **Defensive coding is mandatory** —
  [`docs/DEFENSIVE_CODING.md`](docs/DEFENSIVE_CODING.md) is enforced by `make lint`:
  every write goes through the ActiveRecord lifecycle, every error return
  logs context, every allocation is checked, every long-running loop is
  registered with a supervisor liveness tree.
- **Tests** — 860+ test cases across 350+ test files (`make test`), run as
  ~415 parallel suites by `test_parallel`. Bugs become 64-bit seeds in a
  deterministic simulator (`docs/CHAOS_HARNESS.md`).
- **Local gates** — `make lint` and `make ci` are the required integrity gates
  for this checkout. The contributor policy documents maintainer-run CI outside
  GitHub Actions; do not treat hosted push/PR automation as present unless a
  workflow file is added.
- **Reproducible builds** — byte-identical, with optional GPG signing
  (`tools/release.sh`).

Boot-ordering invariants are documented in
[`docs/BOOT_INVARIANTS.md`](docs/BOOT_INVARIANTS.md); historical performance numbers
live in [`docs/BENCHMARKS_LOG.md`](docs/BENCHMARKS_LOG.md) (treat them as
measurements from a healthy sync, not current guarantees).

## Security

Start with [`docs/SECURITY_AND_INTEGRITY.md`](docs/SECURITY_AND_INTEGRITY.md)
for the project's operator safety boundary, local gates, MCP controls,
release-integrity checks, and reviewer checklist.

A third-party security audit was received and triaged in June 2026; the
point-by-point response — what was fixed, what was refuted, and why — is in
[`docs/work/security-audit-response-2026-06-09.md`](docs/work/security-audit-response-2026-06-09.md).
Known soft spots are stated plainly there and in the status notice above
(e.g. off-chain ZMSG is plaintext on the wire).

To report a vulnerability, please open a GitHub security advisory or
contact the maintainer privately rather than filing a public issue — see
[`.github/SECURITY.md`](.github/SECURITY.md).

## Operating the node

```bash
# One-time host setup (port capabilities, user-service linger)
sudo deploy/setup.sh

# Build, install, and watch the systemd user service
make zclassic23 && make deploy
systemctl --user status zclassic23
tail -f ~/.zclassic-c23/node.log
```

Operator-specific flags (`-externalip` and a seeded `-addnode` list) live
in `~/.config/zclassic23/env`, not in the tracked systemd unit. Copy
`deploy/zclassic23.env.example` there and edit; a fresh install without
the env file starts cleanly against DNS seeds.

Useful RPC calls via `build/bin/zcl-rpc`: `getblockchaininfo`,
`getpeerinfo`, `syncstate`, `healthcheck`, `eventlog 100`,
`z_gettotalbalance`. The REST API mirrors the explorer: `GET /api/blocks`,
`/api/block/:id`, `/api/tx/:txid`, `/api/address/:addr`, `/api/stats`,
`/api/hodl`, `/api/health`.

Handy environment variables: `ZCL_RPCPORT` (RPC port for `zcl-rpc` /
`zclassic-cli`, default `18232`) and `ZCL_DATADIR` (where `zcl-rpc` finds
the `.cookie` file, default `~/.zclassic-c23`). Other `ZCL_*` knobs exist
for niche tuning; see the source.

**Seeds**: `74.50.74.102`, `205.209.104.118`, `140.174.189.3` ·
**DNS**: `dnsseed.zclnet.net`, `dnsseed.zslp.org`, `mainnet.zclassic.org`

## Documentation

- [`CLAUDE.md`](CLAUDE.md) — MCP tool reference, architecture overview, build/test/deploy
- [`docs/FRAMEWORK.md`](docs/FRAMEWORK.md) — canonical architecture
- [`docs/ARCHITECTURE_DIAGRAMS.md`](docs/ARCHITECTURE_DIAGRAMS.md) — boot, services, P2P, wallet diagrams
- [`docs/MVP.md`](docs/MVP.md) — v1 acceptance criteria and honest readiness score
- [`docs/SYNC.md`](docs/SYNC.md) — sync methods, verification layers, self-healing
- [`docs/RUNBOOK.md`](docs/RUNBOOK.md) — symptom-driven troubleshooting
- [`docs/DEFENSIVE_CODING.md`](docs/DEFENSIVE_CODING.md) — mandatory coding standards (lint-enforced)
- [`.github/CONTRIBUTING.md`](.github/CONTRIBUTING.md) — build prerequisites, test workflow, contribution contract
- [`.github/SECURITY.md`](.github/SECURITY.md) — supported status and vulnerability reporting
- [`docs/spec/power-node-contract.md`](docs/spec/power-node-contract.md) — power-node contract surface

## License

Copyright 2026 Rhett Creighton

Licensed under the Apache License, Version 2.0 — see [`LICENSE`](LICENSE)
for the full text. Upstream copyright notices from inherited code
(Bitcoin Core, Zcash, zclassicd) and vendored dependencies (Tor, SQLite,
secp256k1, LevelDB, dcrdex) are preserved in [`NOTICE`](NOTICE).
Architectural concept attributions (Erigon, mcp-language-server, etc.)
are tracked in [`docs/ATTRIBUTIONS.md`](docs/ATTRIBUTIONS.md).
