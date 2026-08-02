# Attributions

zclassic23 is licensed under the **Apache License, Version 2.0** (see
[`LICENSE`](../LICENSE)). Upstream copyright notices from inherited code
and vendored dependencies are preserved in [`NOTICE`](../NOTICE) as
required by Apache 2.0 §4(d).

This file documents **concepts and architectural patterns** (not
verbatim source code) that zclassic23 has adopted from other
projects. We credit them here because it is the right thing to do —
all ideas listed below are re-implemented in C23 from scratch and do
not link against the original code. Apache 2.0 is compatible with
the licenses of every project listed here per the Free Software
Foundation's compatibility matrix.

If you are reading this and recognize a pattern we've ported without
naming you here, open a PR or ping the maintainers and we'll add you.

---

## Erigon — Ethereum execution client, LGPL-3.0

**Repository:** https://github.com/erigontech/erigon
**License:** [GNU LGPL-3.0](https://github.com/erigontech/erigon/blob/main/COPYING.LESSER)
**Attribution:** Copyright © The Erigon Authors

Concepts we've adopted:

| zclassic23 feature | Erigon source referenced | Notes |
|---|---|---|
| `struct zcl_stage` + staged sync runner | [`execution/stagedsync/stage.go`](https://github.com/erigontech/erigon/blob/main/execution/stagedsync/stage.go), [`sync.go`](https://github.com/erigontech/erigon/blob/main/execution/stagedsync/sync.go) | Forward/Unwind/Prune triad per stage |
| Stage pipeline ordering | [`default_stages.go`](https://github.com/erigontech/erigon/blob/main/execution/stagedsync/default_stages.go) | Explicit forward vs unwind order |
| Per-stage `Cfg` struct pattern | `HeadersCfg` / `ExecuteBlockCfg` | One config struct per stage |
| ETL (Extract-Transform-Load) for bulk writes | [`db/etl/README.md`](https://github.com/erigontech/erigon/blob/main/db/etl/README.md) | Temp-file sort before bulk load to minimize write amplification |
| Temporal DB interface (hot mutable + cold immutable) | [`db/kv/kv_interface.go`](https://github.com/erigontech/erigon/blob/main/db/kv/kv_interface.go), [`db/agents.md`](https://github.com/erigontech/erigon/blob/main/db/agents.md) | `get_latest(k)` + `get_as_of(k, ts)` |
| Stream vs Cursor split | `db/kv/stream/`, cursor interfaces in `kv_interface.go` | High-level iterator over low-level cursor |
| Per-subsystem `agents.md` files | [`execution/stagedsync/agents.md`](https://github.com/erigontech/erigon/blob/main/execution/stagedsync/agents.md), `cl/agents.md`, `p2p/agents.md`, `db/agents.md` | Localized AI guidance close to code | <!-- doc-path-ok: upstream erigon paths, not this tree -->
| Explicit naming discipline at the top of storage headers | [`kv_interface.go:30-50`](https://github.com/erigontech/erigon/blob/main/db/kv/kv_interface.go) naming block | `tx` vs `txn`, `blockNum` vs `blockID`, etc. |
| Per-stage timing table | `sync.go::timings` | Wall-clock per stage, dumped on cycle end |
| Ruleguard-style antipattern lint | Erigon `CLAUDE.md` ("defer tx.Rollback after error check") | Pattern-level grep gates per recurring issue |
| Consensus spectest harness | [`cl/spectest/`](https://github.com/erigontech/erigon/tree/main/cl/spectest) | Reference corpus → replay → diff state |

These patterns are cited inline in the per-subsystem `agents.md`
files as they land.

---

## Bitcoin Core — MIT

**Repository:** https://github.com/bitcoin/bitcoin
**License:** MIT

The consensus surface of this project descends from Bitcoin Core via
zcashd via zclassicd. Core-inherited algorithms (script interpreter,
BIP-30 / BIP-34 / BIP-65 / BIP-66 semantics, Bloom filter, compact
blocks) are MIT-licensed at their root.

## zcashd — MIT

**Repository:** https://github.com/zcash/zcash
**License:** MIT

Sapling and Sprout zk-SNARK designs, Equihash 200/9 PoW, and the
shielded-pool accounting rules.

## zclassicd (legacy peer) — MIT

**Repository:** https://github.com/ZclassicCommunity/zclassic
**License:** MIT

Chain history, checkpoint schedule, network magic, and the reference
behavior used by the zclassic23 parity-diff service.

## dcrdex — Blue Oak Model License 1.0.0 (concept reference)

**Upstream:** https://github.com/decred/dcrdex
**License:** https://blueoakcouncil.org/license/1.0.0

The cross-chain atomic-swap HTLC script format (P2SH-wrapped, 97-byte contract)
used by the ZCL atomic-swap protocol (ZSWP) was **reimplemented** from dcrdex's
design in `lib/script/` (`lib/script/{src/htlc.c,include/script/htlc.h}`) — no dcrdex source is vendored in this
tree. Credited here for the script-format concept.

## SQLite — Public Domain

**Vendored path:** `vendor/` amalgamation
**License:** https://www.sqlite.org/copyright.html

Embedded database for the canonical UTXO store, wallet keystore, block
index (with CRC), and application state.

## RGFW 1.8.1 — zlib License

**Repository:** https://github.com/ColleagueRiley/RGFW
**Pinned revision:** `d96684e6877d3ef11c731b122b8949942ed071c9`
**Vendored path:** `vendor/rgfw/`
**Attribution:** Copyright © 2022-2025 Riley Mabb (@ColleagueRiley)

The private software-window backend for `lib/presentation`. ZClassic23 exposes
its own bounded bitmap ABI; no RGFW type crosses the public boundary.

## QR Code generator 1.8.0 — MIT

**Repository:** https://github.com/nayuki/QR-Code-generator
**Pinned revision:** `720f62bddb7226106071d4728c292cb1df519ceb`
**Vendored path:** `vendor/qrcodegen/`
**Attribution:** Copyright © Project Nayuki

The dependency-free QR Model 2 encoder behind `lib/encoding/qr`.

## ZClassic logo — CC BY 4.0

**Source:** https://commons.wikimedia.org/wiki/File:ZClassic_Logo.svg
**Attribution:** @jojo, ZClassic Slack/Rocket.Chat, 2016-12-05

`lib/presentation/src/zclassic_icon_mask.inc` is a 64px one-bit rasterization
of the official mark. The shape and canonical `#C87035` color are unchanged.

## AGENTS.md portable-standard (community convention) — no license

**Reference:** https://www.augmentcode.com/guides/how-to-build-agents-md,
https://github.com/0xdevalias/some-notes-on-ai-rule-files

Emerging 2026 convention for AI coding agents: `AGENTS.md` at repo
root is the portable successor to per-tool files (`.cursorrules`,
`CLAUDE.md`, etc.). zclassic23 adopts the convention with a dual-link
to `CLAUDE.md` for Claude Code compatibility.

## Codified Context Infrastructure (arxiv 2602.20478)

**Reference:** https://arxiv.org/abs/2602.20478

Three-tier pattern for AI-native codebases: (1) hot-memory constitution
(conventions + retrieval hooks), (2) specialized domain-expert agents,
(3) cold-memory knowledge base of specification documents. Used for
the cold-memory spec corpus in `docs/spec/`.

## Tor (modified fork with dynhost) — 3-clause BSD

**Vendored path:** `vendor/tor`
**License:** https://gitlab.torproject.org/tpo/core/tor/-/blob/main/LICENSE

Embedded Tor with in-process dynhost API for .onion hidden service
hosting. Fork maintained at https://github.com/RhettCreighton/tor.
