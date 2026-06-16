# ZClassic23

[![license](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

One self-contained ~15 MB pure-C23 binary: a full ZClassic node (Equihash 200,9
PoW, Sapling shielded transactions), an embedded Tor onion service, a block
explorer, a shielded wallet, and a built-in MCP server that lets an AI agent
operate the node through ~100 typed tools.

**One binary, one onion, one stack.**

## Status

Live and stable on ZClassic mainnet — holds the tip hash-identical to a local
`zclassicd` reference, publishes blocks as they arrive, and keeps the connected
chain across restarts.

**Not yet production-ready.** The v1 criteria in [`docs/MVP.md`](docs/MVP.md)
aren't CI-enforced and the soak window is still accumulating. The known soft
spots are stated plainly here and in [`docs/MVP.md`](docs/MVP.md) (e.g. cold-start
bootstrap is fragile; off-chain ZMSG is plaintext on the wire). Don't rely on it
as your only mainnet node yet.

It is operator-owned full-node infrastructure: embedded Tor publishes *your* onion
service, wallet state stays in your datadir, MCP is a typed *local* operator
interface. Safety boundary and integrity checks:
[`docs/SECURITY_AND_INTEGRITY.md`](docs/SECURITY_AND_INTEGRITY.md).

## What's on board

A complete rewrite of zclassicd in pure C23. Every node is at once:

- **A full node** — headers, block validation (Equihash PoW, ECDSA scripts,
  Sprout/Sapling zk-SNARKs), tx relay, P2P (port 8033). History validates
  identically to zclassicd; a consensus-parity audit tracks the forward edge.
- **A fast-sync server** — FlyClient MMB proofs + SHA3-committed UTXO snapshots
  reach an operational tip in ~a minute, with optional background re-validation.
- **A Tor hidden service** — Tor is compiled in; `-tor` serves the explorer + API
  over a `.onion` via in-process calls (no SOCKS, no exposed ports).
- **A block explorer** — charts, HODL waves, ZSLP scanner, JSON REST at `/api`.
- **A shielded wallet** — transparent + Sapling, over RPC and MCP.
- **An MCP server** — Claude operates the node as a first-class operator.

Also aboard, honestly labeled: **ZNAM** on-chain name registry (working);
**ZMSG** messaging (on-chain shielded; off-chain P2P is plaintext on the wire);
**ZCL Market** + **ZSWP** atomic swaps (scaffolding — no settlement yet);
**P2P games** (latency ping + TicTacToe).

## Quick start

**Prerequisites:** gcc 14+ (or clang with `-std=c23`) and GNU make. Vendored
static libs live in `vendor/lib/`; only `libsecp256k1.a` is tracked today, so
building outside the maintainer's environment has friction (a known gap).

```bash
git clone https://github.com/ZclassiC23/zclassic.git && cd zclassic
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

Datadir `~/.zclassic-c23/` (`-datadir=DIR`). Ports: P2P `8033`, RPC `18232`.

## Claude integration (MCP)

The differentiator: a built-in [MCP](https://modelcontextprotocol.io) server, so
Claude Code queries and operates the node through typed tools — no curl, no log
spelunking.

```bash
claude mcp add zcl23 -- build/bin/zclassic23 -mcp
```

Restart Claude Code and the tools appear. Daily drivers:

| Tool | What it does |
|------|--------------|
| `zcl_status` | Height, peers, sync, onion, health — one call |
| `zcl_state` | State dump of any subsystem (supervisor, boot, …) |
| `zcl_node_log` | Server-side regex tail of `node.log` |
| `zcl_sql` | SELECT-only SQL over the node DB (rate-gated) |
| `zcl_rpc` | Escape hatch into 85+ raw JSON-RPC methods |
| `zcl_tools_list` | The full live tool catalog (~100 tools) |

Wallet, mining, ZNAM, ZMSG, swaps, and admin are typed tools too — see
[`CLAUDE.md`](CLAUDE.md). MCP is an operator interface (stdio, local client);
don't expose RPC/MCP to untrusted clients.

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

## License

Copyright 2026 Rhett Creighton. Apache License 2.0 — see [`LICENSE`](LICENSE).
Upstream notices (Bitcoin Core, Zcash, zclassicd, Tor, SQLite, secp256k1,
LevelDB, dcrdex) are in [`NOTICE`](NOTICE); concept attributions in
[`docs/ATTRIBUTIONS.md`](docs/ATTRIBUTIONS.md).
