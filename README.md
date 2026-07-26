# ZClassic23

[![license](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![language](https://img.shields.io/badge/language-C23-00599C.svg)](#)
[![status](https://img.shields.io/badge/status-pre--v1-orange.svg)](docs/MVP.md)
[![CI](https://img.shields.io/badge/CI-local%20make%20lint-success.svg)](docs/DEFENSIVE_CODING.md)

One self-contained pure-C23 binary: a full ZClassic node (Equihash 200,9
PoW, Sapling shielded transactions), an embedded Tor onion service, a block
explorer, a shielded wallet, a P2P file marketplace, a ZNAM on-chain name
registry, ZCL messaging, cross-chain atomic-swap scaffolding, and a native
command registry that lets an AI agent operate the node through typed
commands (`zclassic23 <command>`) — a **personal sovereignty stack**: a
secure personal-computing OS whose only trust foundation is the ZClassic
proof-of-work network and the compiled binary itself, with no DNS, CAs, or
registries anywhere in the path.

**One binary, one onion, one stack — your sovereign personal computing surface.**

## Status

**Pre-v1 — not yet production-ready.** The v1 bar is the eight acceptance
criteria in [`docs/MVP.md`](docs/MVP.md) (v1 = MRS 8/8). Don't rely on it as
your only mainnet node yet.

It runs on ZClassic mainnet on the `zclassicd` consensus floor. The node's
UTXO/anchor/nullifier state is either self-derived by folding real block
bodies forward from the in-binary SHA3/PoW checkpoint
(`core/chainparams/src/checkpoints.c`) — the **sovereign cure** — or, where a
borrowed snapshot was used to seed transparent state, checked against a
validated local header; ZClassic headers do not commit a snapshot's UTXO or
shielded-state contents, so a header match alone does not make borrowed
contents consensus- or PoW-bound. Design: [`docs/work/self-verified-tip-plan.md`](docs/work/self-verified-tip-plan.md).
The other known soft spot: **off-chain ZMSG is plaintext on the wire**.

Live state: `zclassic23 status` / [`docs/HANDOFF.md`](docs/HANDOFF.md).

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
**sovereign cure** (see [Status](#status)) makes the sealed-state layer
independently derived rather than borrowed from an external `zclassicd`
snapshot.

## Requirements

Run **`make doctor`**: it probes this host against
`tools/scripts/vendor_prereqs.tsv` — the single source of truth — and prints
the one install line for whatever is missing. The list below is what that
table says today.

- **To build and run the node:** a C23 compiler (**gcc 14+**, or clang with a
  working `-std=c23`), **GNU make**, and **git**. The shipped binary links only
  stock `libc` plus the static archives in `vendor/lib/`, so there is no
  runtime package to install.
- **To build those archives once (`make vendor`):** `ar`, `nm`, `sha256sum`,
  `tar`, `unzip`, `patch`, `perl` (OpenSSL's `Configure` is a perl program),
  and `curl` **or** `wget`.
- **Rust is NOT required.** `make` on a host with no `cargo`/`rustc` produces a
  full node: it validates the chain, verifies and relays other people's
  shielded transactions, serves the explorer and REST API, mines, and receives
  shielded funds — all of that is native C23. The single capability that needs
  Rust is *creating* Sapling proofs, i.e. **sending** shielded value; without
  it `z_sendmany` refuses with a typed error naming the flag to rebuild with,
  and never fails silently. Add it with `make ZCL_WITH_RUST=1`, which builds
  and links `librustzcash.a` (the canonical Zcash Sapling prover) and needs
  `cargo` + `rustc`.
- **A C++ compiler** (`c++`/`g++`) builds LevelDB, which is C++11. `cmake` is
  the preferred route and is genuinely optional — a direct C++ compile is used
  when it is absent — but the C++ compiler itself is not optional. "Pure C23"
  describes the node's own source, not every third-party archive it links.
  This one is on its way out: `lib/storage/src/ldb_reader_*.c` is a read-only
  LevelDB reader in plain C23, proven byte-identical to `libleveldb.a` over the
  real on-disk databases on this host. What still has to land before `c++`
  leaves this list is spelled out in [`docs/BUILD.md`](docs/BUILD.md) under
  *Retiring the C++ requirement*.
- **The first `make vendor` needs the internet.** It downloads pinned source
  tarballs (OpenSSL, libevent, LevelDB, zlib, SQLite — plus the Sapling prover
  under `ZCL_WITH_RUST=1`), verifies each against a pinned SHA-256, and
  compiles them locally. Afterwards `vendor/lib/` is cached and every later
  build is offline.

## Quick start

Public start here: [`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md) is the
fresh-machine path for people cloning from GitHub, covering build, running a
production node + block explorer, and setting up an isolated development
instance. This README is the overview; `docs/HANDOFF.md` and
`docs/RUNBOOK.md` are maintainer/operator documents for the project's own
hosted lanes.

### What the first build costs

Measured, not estimated: `make first-build-timing` clones the repository into a
scratch directory and times every stage of a cold build; `make timings` prints
the result back.

| Stage | Command | Wall time | What it needs |
|---|---|---|---|
| Clone | `git clone` | seconds locally; the history is 932 MiB over the network | `git` |
| Vendored archives | `make vendor` | 92 s | network, a C++ compiler (Rust only under `ZCL_WITH_RUST=1`) |
| Arm the clone | `make setup` | 41 s | nothing beyond the above |
| Binaries | `make -j"$(nproc)"` | 205 s | gcc 14+ |
| Full test suite | `make -j"$(nproc)" test-parallel` | 252 s | nothing beyond the above |
| **Clone to passing suite** | | **590 s (under 10 minutes)** | **1.6 GB of disk** |

Measured on a 32-core host (gcc 14.2, rustc 1.95) with the compiler cache
switched off, so it reflects a machine that has never built this project; the
host was running other builds at the time (1-minute load average 29 rising to
49). The 92 s vendor row was measured while the Sapling prover was still part
of the default archive set — it is now opt-in, so a default `make vendor` does
less than that row records and `make ZCL_WITH_RUST=1 vendor` is what it
describes. Your own numbers come from `make first-build-timing`, and `make doctor`
names anything this host is still missing. Fuller breakdown, including which
stage is the long pole and why, is in [`docs/BUILD.md`](docs/BUILD.md).

```bash
git clone https://github.com/ZclassiC23/zclassic.git && cd zclassic
make                # node + CLI + RPC tool -> build/bin/{zclassic23,zclassic-cli,zcl-rpc}
make fast-rebuild   # changed-file dev compile + non-LTO local node link
make dev-bin        # fast non-LTO local node -> build/bin/zclassic23-dev
make test           # full suite (every registered parallel group)
make lint           # defensive-coding gates
```

(`make zclassic23` builds only the node; plain `make` also builds the
`zclassic-cli` / `zcl-rpc` clients used in the examples below.) The first build
auto-runs **`make vendor`**, which builds the static
third-party archives in `vendor/lib/` from source (OpenSSL, libevent ×3, LevelDB,
SQLite, zlib — and librustzcash only under `ZCL_WITH_RUST=1`) plus the in-tree Tor stub — sources are pulled from pinned URLs and
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

## First boot — what a fresh node looks like

A brand-new datadir is honestly empty. It does **not** report a fake height:

- `getblockcount` returns **`0`** until blocks are actually folded — no phantom
  tip. `getblockchaininfo` returns `blocks: 0, headers: 0,
  initialblockdownload: true` (best-block resolves to genesis).
- **Peer discovery has no DNS seeders** — the historical ZCL DNS names no longer
  resolve. The node bootstraps from a small hardcoded set of reachable-verified
  ZClassic IP seeds (baked into the sealed `core/` chain params, and re-verified
  before release rather than pinned in prose — the node prints the live tally as
  `[net] bootstrap sources: … fixed_seeds=N` on every boot)
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

## AI agent integration

The differentiator: a native command registry built into the binary — the
**primary** agent surface — so an AI agent (Claude is the one this project
develops against day to day; the interface is model-agnostic and needs no
vendor SDK) queries and operates the node through typed commands, no curl, no
log spelunking, no separate server process.

It is the ABI. Every leaf lives in a `.def` file under `config/commands/`,
grouped under `core.*`/`app.*`/`ops.*`/`dev.*`/`discover.*`/`code.*`, and each
is a typed `zcl_command_spec` (input/output schema, a one-line output
**semantics** contract, a per-leaf response **byte budget**, auth, risk,
latency, cost) validated fail-closed at every startup — so responses are
self-describing and bounded, and no error reply lacks a `next` action to run.

How many leaves there are is a question for the binary, not this page: run
`zclassic23 discover help` for the live catalog. The code-derived counts the
docs are allowed to quote live in the machine-checked DOC-COUNTS block of
[`docs/CODEBASE_MAP.md`](docs/CODEBASE_MAP.md).

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

The typed native command registry is the sole AI/operator surface.

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
└── Native cmds    typed command leaves (`zclassic23 <cmd>`; `discover help`)
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
- **`zclassic23 dbquery`** is SELECT-only, semicolon-rejected, auto-LIMIT, and denies a
  set of wallet-secret tables/columns by name.
- **Lint gates** (`make lint`) enforce these and the defensive-coding rules
  below on every change. The gate list is the `LINT_GATES` variable in the
  `Makefile`, and the `check-doc-accuracy` gate fails the build if
  [`docs/DEFENSIVE_CODING.md`](docs/DEFENSIVE_CODING.md) ever names a
  different set — so that doc, not this one, is where the gates are listed.
  Safety boundary:
  [`docs/SECURITY_AND_INTEGRITY.md`](docs/SECURITY_AND_INTEGRITY.md); the
  gates that specifically police AI-agent honesty are described in
  [`docs/AI_SAFETY_GATES.md`](docs/AI_SAFETY_GATES.md).

Known gaps: off-chain P2P messaging is plaintext on the wire (Noise-based
transport encryption is designed, not yet wired); the wallet-encryption
default doesn't yet apply retroactively to existing plaintext wallets.

## Repository layout

The root is a curated list. Everything in it is either a source area, a
top-level document, or one of the four generated entries at the bottom of this
section — nothing else belongs there, and `make lint` fails on anything that
shows up (`check-no-stray-root-files`).

**Where the code lives**, from the inside out — the further down the table, the
further from consensus:

| Area | Purpose |
|-----|----------|
| `core/` | The sealed consensus core: checkpoints, chain params, consensus math. SHA3-manifest pinned; changing it needs the owner unseal ritual. |
| `domain/` | Pure domain logic — consensus rules, encodings, wallet primitives. No clock, no RNG, no I/O, so it is testable in isolation. |
| `app/` | Everything the node *does*, filed into the eight shapes (models, views, controllers, services, jobs, conditions, events, supervisors). Lint decides which folder a file belongs in. |
| `lib/` | Subsystem libraries the app builds on: consensus, net, sync, storage, crypto, sapling, script, rpc, util, and the test harness. |
| `ports/` · `adapters/` | The hexagonal write seam — port interfaces on one side, outbound implementations on the other. |
| `config/` | The composition root: what gets wired to what at boot, plus `config/commands/*.def`, the native command registry. |
| `src/` | Binary entry points. Thin — the node and the CLI both assemble what the areas above provide. |

**Everything else at the top level**, one line each:

| Area | Purpose |
|-----|----------|
| `tools/` | Anything you run *at* the code rather than ship: lint gates, fuzzers, simulators, the dev loop, release scripts. |
| `docs/` | Every document. [`docs/DEVELOPING.md`](docs/DEVELOPING.md) is the operating manual; [`docs/CODEBASE_MAP.md`](docs/CODEBASE_MAP.md) is the detailed where-things-live map this table summarizes. |
| `deploy/` | How the node runs on a host: systemd user service and setup. |
| `vendor/` | Third-party source built from pinned inputs, plus the Tor submodule. Nothing here is ours. |
| `examples/` · `apps/` | The outward-facing surface: worked examples you can compile, and the app manifests built on the Core→App header in `lib/framework/include/zclassic23/`. |
| `tests/` | Shared test fixtures — data, not code. |

**Generated, never tracked** — these appear after a build and are the only
untracked entries the root is allowed to have:

| Entry | Where it comes from |
|-----|----------|
| `build/` | All build output, including the binaries in `build/bin/`. |
| `test-tmp/` | The one scratch root for test runs — every test writes its per-run datadir under here, never into the checkout root. Safe to delete at any time. |
| `compile_commands.json` | Written by `make setup` / `make compdb` so clangd and editors know the exact compile flags. Conventional at the root; leave it there. |
| `.cache/` · `.codeindex/` | Tool caches: lint timings, the clangd index, the source navigator's index. |

## Engineering posture

- **Defensive coding is mandatory** and lint-enforced
  ([`docs/DEFENSIVE_CODING.md`](docs/DEFENSIVE_CODING.md)): every write through the
  ActiveRecord lifecycle, every error logs context, every alloc checked, every
  long loop on a supervisor liveness tree.
- **Tests:** `make test` runs all registered parallel groups — how many there
  are is derived from the code by `tools/scripts/check_doc_counts.sh` and
  declared in the DOC-COUNTS block of
  [`docs/CODEBASE_MAP.md`](docs/CODEBASE_MAP.md), which `make lint` fails on
  drift. That same gate re-derives the count for every tracked Markdown file in
  the repo — this README included — so a doc that quotes a group/port/adapter
  count that no longer matches the code fails the build rather than quietly
  rotting. A test that compiles but is registered in no runner is caught by the
  `check-test-registration` gate. Bugs become 64-bit seeds in a deterministic
  simulator ([`docs/CHAOS_HARNESS.md`](docs/CHAOS_HARNESS.md)).
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
- [`docs/AI_SAFETY_GATES.md`](docs/AI_SAFETY_GATES.md) — the gates that stop an AI agent from claiming a victory it cannot cite

**Issues & changes:** file bugs and features via GitHub Issues — the forms in
[`.github/ISSUE_TEMPLATE/`](.github/ISSUE_TEMPLATE/) ask for the two things
that decide whether a report is actionable (the output of `zclassic23 status`,
and whether the change touches consensus). Security reports follow
[`.github/SECURITY.md`](.github/SECURITY.md). Consensus changes are declined on
principle — see
[`docs/CONSENSUS_PARITY_DOCTRINE.md`](docs/CONSENSUS_PARITY_DOCTRINE.md).

## License

Copyright 2026 Rhett Creighton. Apache License 2.0 — see [`LICENSE`](LICENSE).
Upstream notices (Bitcoin Core, Zcash, zclassicd, Tor, SQLite, secp256k1,
LevelDB, dcrdex) are in [`NOTICE`](NOTICE); concept attributions in
[`docs/ATTRIBUTIONS.md`](docs/ATTRIBUTIONS.md).
