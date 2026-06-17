# ZClassic23

[![license](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![language](https://img.shields.io/badge/language-C23-00599C.svg)](#)
[![status](https://img.shields.io/badge/status-pre--v1-orange.svg)](docs/MVP.md)
[![CI](https://img.shields.io/badge/CI-local%20make%20ci%20(37%20gates)-success.svg)](docs/DEFENSIVE_CODING.md)

One self-contained ~15 MB pure-C23 binary: a full ZClassic node (Equihash 200,9
PoW, Sapling shielded transactions), an embedded Tor onion service, a block
explorer, a shielded wallet, and a built-in MCP server that lets an AI agent
operate the node through ~100 typed tools.

**One binary, one onion, one stack.**

## Status

**Pre-v1 — not yet production-ready.** Zero of the eight v1 acceptance criteria
in [`docs/MVP.md`](docs/MVP.md) are end-to-end CI-verified (MRS 0/8), so don't
rely on it as your only mainnet node yet.

It runs on ZClassic mainnet and holds the tip hash-identical to a local
`zclassicd` reference, publishing blocks as they arrive. It was recovered on
2026-06-16 from a multi-day cold-import outage and is now accumulating fresh
soak time. The known soft spots are stated plainly here and in
[`docs/MVP.md`](docs/MVP.md): **cold-start bootstrap is slow + fragile**, and
**off-chain ZMSG is plaintext on the wire**.

It is operator-owned full-node infrastructure: embedded Tor publishes *your* onion
service, wallet state stays in your datadir, MCP is a typed *local* operator
interface. Safety boundary and integrity checks:
[`docs/SECURITY_AND_INTEGRITY.md`](docs/SECURITY_AND_INTEGRITY.md).

## What's on board

A complete rewrite of zclassicd in pure C23. One binary is at once a full node
(Equihash PoW, ECDSA scripts, Sprout/Sapling zk-SNARKs, history validates
identically to zclassicd), a fast-sync server (FlyClient MMB + SHA3 UTXO
snapshot, operational tip in ~a minute), an in-process Tor hidden service
(`-tor`), a block explorer (`/api`), a shielded wallet (transparent + Sapling),
and an MCP server. Full subsystem catalog in [`CLAUDE.md`](CLAUDE.md).

Honestly labeled: **ZNAM** name registry (working); **ZMSG** messaging (on-chain
shielded; off-chain P2P is plaintext on the wire); **ZCL Market** + **ZSWP**
atomic swaps (scaffolding — no settlement yet); **P2P games** (ping +
TicTacToe).

## Quick start

**Prerequisites:** gcc 14+ (or clang with `-std=c23`) and GNU make.

> **Build note (known gap):** `vendor/lib/` tracks only `libsecp256k1.a`. The
> other 10 static archives (OpenSSL, libevent ×3, leveldb, SQLite, zlib, the Tor
> stub) are not yet in the repo, so a fresh clone **will not link**
> `make zclassic23` until you supply them. [`docs/BUILD.md`](docs/BUILD.md) lists
> each one's source, version, and build command; `make vendor` automation is on
> the roadmap.

```bash
git clone https://github.com/ZclassiC23/zclassic.git && cd zclassic
# see docs/BUILD.md first — vendored libs are required to link
make zclassic23     # main binary -> build/bin/zclassic23
make test           # full suite (~423 parallel groups)
make lint           # defensive-coding gates
```

```bash
build/bin/zclassic23                                  # start
build/bin/zclassic23 -tor                             # + .onion
build/bin/zclassic23 --importblockindex ~/.zclassic   # fast header import from
                                                      # legacy data, then a normal
                                                      # boot auto-links ~/.zclassic
                                                      # (opt out: -nolegacyimport)
```

Datadir `~/.zclassic-c23/` (`-datadir=DIR`). Default ports: P2P `8033`, RPC
`18232`. To run alongside a local `zclassicd` (which holds `8033`), use
`-port=8023` — the shipped service does. The authoritative lane/port table is in
[`docs/HANDOFF.md`](docs/HANDOFF.md).

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

**Bootstrap:** DNS seeds `dnsseed.zclnet.net`, `dnsseed.zslp.org`,
`mainnet.zclassic.org` (prefer DNS — hardcoded IP seeds rot).

## Documentation

- [`CLAUDE.md`](CLAUDE.md) — MCP reference, build/test/deploy, recovery
- [`docs/FRAMEWORK.md`](docs/FRAMEWORK.md) — canonical architecture
- [`docs/MVP.md`](docs/MVP.md) — v1 criteria + honest readiness
- [`docs/SYNC.md`](docs/SYNC.md) · [`docs/RUNBOOK.md`](docs/RUNBOOK.md) — sync + troubleshooting
- [`docs/SECURITY_AND_INTEGRITY.md`](docs/SECURITY_AND_INTEGRITY.md) · [`.github/SECURITY.md`](.github/SECURITY.md) — security
- [`.github/CONTRIBUTING.md`](.github/CONTRIBUTING.md) — build prereqs + contribution contract
- [`docs/BUILD.md`](docs/BUILD.md) — vendored-library sources, versions, build steps
- [`CHANGELOG.md`](CHANGELOG.md) — notable changes (pre-v1)

**Issues & changes:** file bugs and features via GitHub Issues (templates
provided); security reports follow [`.github/SECURITY.md`](.github/SECURITY.md).
Consensus changes are declined on principle — see
[`docs/CONSENSUS_PARITY_DOCTRINE.md`](docs/CONSENSUS_PARITY_DOCTRINE.md).

## License

Copyright 2026 Rhett Creighton. Apache License 2.0 — see [`LICENSE`](LICENSE).
Upstream notices (Bitcoin Core, Zcash, zclassicd, Tor, SQLite, secp256k1,
LevelDB, dcrdex) are in [`NOTICE`](NOTICE); concept attributions in
[`docs/ATTRIBUTIONS.md`](docs/ATTRIBUTIONS.md).
