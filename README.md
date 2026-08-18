# Z23

> **ZClassic23 is now Z23.** Same node, same protocol, same git history — new
> name. The repository moved to <https://github.com/z23c/z23> (the old
> `ZclassiC23/zclassic` URL redirects here). The binaries are now `z23` /
> `z23-dev`; the old `zclassic23` names still work as aliases while scripts
> migrate. Consensus, chain IDs, network parameters, `zcl.*` protocol domains,
> package roots, and the `~/.zclassic-c23` datadir are unchanged.

[![license](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![language](https://img.shields.io/badge/language-C23-00599C.svg)](#)
[![status](https://img.shields.io/badge/status-pre--v1-orange.svg)](docs/MVP.md)
[![CI](https://img.shields.io/badge/CI-local%20make%20lint-success.svg)](docs/DEFENSIVE_CODING.md)

**Write exact C23. Any node builds it, runs it under bounded authority, and
reproduces the result byte-for-byte — with no central software registry.**

Z23 exists to make useful software abundant without taking control away from
the user. **Software made for you, not imposed on you.** Describe what you
want your device to do; AI workers reuse existing C23 parts first, create only
what is missing, build the result fast, show its real behavior, and reproduce
the exact result on another machine — you accept and use that exact version,
and it stays usable even when the original agent, vendor, registry, or company
disappears.

Z23 is a public ZClassic blockchain full node in one self-contained C23
binary, and that same node is a decentralized C23 software commons. People and
AI agents propose exact source; nodes fetch it, build it in confinement, run
it, and check each other's results. Nothing is accepted because of who produced
it.

Here is the whole idea in one command — two pilot programs fly a deterministic
dogfight, and your machine re-derives the exact same match:

```bash
git clone https://github.com/z23c/z23.git
cd z23
make arena-demo
```

![ZCODE Arena](docs/assets/zcode-arena.svg)

```text
ZCODE ARENA
Red Ace defeated Blue Drone 10-6
11,941 deterministic ticks

Replay verification:       MATCH
Result vs pinned roots:    MATCH
Altered control byte:      REFUSED (match-incomplete)

Replay root:               05ed352dbb2213aad289cdf403d424d18d9ae075db57252a52c4e745a25e8396
Final-state root:          e4b37a9b94547cead91a7d4ae2a63b0385b29a99bb603bd0ac3519cebd270ebd
```

No blockchain sync, no Tor, no wallet, no browser, no JavaScript, no Python, no
network, no running node — just a C compiler. Those roots are the point: a
second machine that builds the same sources prints the same digests, and a
single altered byte is refused by name. Write your own pilot, verify a replay,
and read the two-node proof and the honest gaps in
[`docs/ARENA.md`](docs/ARENA.md).

---

> **One binary, one onion, one stack.** Z23 is first a public ZClassic
> blockchain full node: consensus-compatible validation, wallet custody, peer
> networking, synchronization, and optional in-process Tor in one C23 binary.

The same full-node network can optionally act as a decentralized C23 software
commons. Ordinary nodes can publish, discover, fetch, verify, build,
independently reproduce, and serve exact C23 packages without GitHub or a
central package registry. Downloaded C is inert by default: fetching and
storing do not authorize building, testing, installing, linking, executing, or
deployment.

**VERIFY, DON’T TRUST.** Any agent may propose code and any node may perform
computation, but each receiving node verifies exact objects, signatures, roots,
and evidence under its own local policy. No result is accepted because of who
produced it.

Z23 implements the ZClassic (ZCL) consensus rules—Equihash 200,9
proof-of-work plus transparent and Sapling shielded transactions—and must
remain bit-for-bit consensus-compatible with `zclassicd`.

The same process — no extra daemons, no sidecars, no reverse proxy — also runs:

| | |
|---|---|
| **Tor onion service** | in-process, so the node's HTTP surface is reachable with no domain name, no static IP, and no TLS certificate |
| **Block explorer + REST API** | `/explorer` and `/api/v1`, served over the onion or over HTTPS |
| **Wallet** | transparent and Sapling shielded keys — see the encryption caveat below before storing value |
| **P2P marketplace** | yardsale: signed, expiring sale ads gossiped between nodes, settled bilaterally over Tor — buyer and seller IPs stay hidden |
| **Decentralized C23 Commons** | publish, discover, fetch, verify, build, independently reproduce, and serve content-addressed C23 packages; metaverse and creation tools are optional application layers |
| **Name registry (ZNAM)** | on-chain names resolving to ZCL/BTC/LTC/DOGE addresses plus text records |
| **Messaging** | Sapling-memo on-chain messages, and direct node-to-node messages |
| **Command registry** | typed commands with declared input/output schemas, byte budgets and risk classes — the interface an AI agent uses to operate the node (`discover help` lists the live set) |

It links only stock `libc` plus static archives built from pinned,
SHA-256-verified sources. There is no runtime dependency to install, no
configuration server to reach, no account, and no API key.

## Verify, don’t trust

- Content roots identify exact bytes.
- Signatures identify the key that made a statement.
- Build, test, and reproduction receipts record exact observations under bound
  inputs; independent reproduction checks an exact build claim.
- Each node verifies evidence locally and decides acceptance under its own
  policy.
- Evidence proves only its declared claim. It does not by itself prove that
  arbitrary code is safe, correct, secure, useful, or worthy of acceptance.

No central service or project developer is an authority a node must rely on.
Workers and coding agents are untrusted producers of independently verifiable
proposals and evidence.

## What Z23 does not do

- It has no central coordinator or central package registry.
- It never executes downloaded C merely because it was fetched or stored.
- It does not claim that tests, signatures, or reproduction prove general
  safety.
- The C23 Commons does not change ZClassic consensus.
- It does not require one AI vendor; AI workers are
  replaceable proposal engines.
- It has no live ZC23 token economics today; those surfaces remain
  simulation-only.

```bash
git clone https://github.com/z23c/z23.git && cd z23
make
build/bin/z23
```

---

## One journey: intent to working software

Tell the node what you want C23 software on this device to do. It reuses
existing C23 parts first, creates only what is missing, builds and tests the
result in confinement, shows you the real behavior, lets another node
reproduce it, and only then asks you to accept the exact version. Ask the node
for the journey rather than trusting this page:

```bash
build/bin/z23 zcode guide
```

| Step | One command |
| --- | --- |
| Describe the behavior you want | `z23 zcode guide` |
| Reuse existing C23 first, create only what is missing | `z23 zcode work start --datadir=/tmp/z23-work --input='{"workspace":".","goal":"<desired behavior>"}'` |
| Build and test it, contained | `z23 zcode work run --datadir=/tmp/z23-work --input='{"work":"latest","adapter":"manual"}'` |
| See the real consequence | `z23 zcode work show --input='{"work":"latest"}'` |
| Reproduce it on another node | `z23 zcode package source reproduce --datadir=/tmp/z23-commons` |
| Accept the exact version | `z23 zcode work accept --datadir=/tmp/z23-work --input='{"work":"latest"}'` |
| Use it in a real application | `z23 zcode use --datadir=/tmp/z23-commons` |

Every step returns the next safe command, so the journey never depends on
remembering this table. The scratch datadirs above keep an experiment off your
live node; drop them once you mean to act on the real one. Fetching never authorizes building; building never
authorizes installing, linking, executing, or deploying; acceptance is a human
decision about one exact version, made on your node under your policy. The
full walkthrough with inputs and failure paths is
[`docs/C23_COMMONS_QUICKSTART.md`](docs/C23_COMMONS_QUICKSTART.md).

The result stays usable when the original agent, vendor, registry, or company
disappears: the source is content-addressed, the recipe is declarative, the
evidence is local, and any node can rebuild the exact bytes.

---

## What you can do with it

Every command below runs against your own node, on your own machine.

### Hold and spend ZCL from your own node

A full ZClassic node with a built-in wallet — transparent addresses and
shielded Sapling addresses in the same binary. You are not asking a service for
your balance; you validated every block yourself.

```bash
build/bin/zclassic-cli getnewaddress          # transparent
build/bin/zclassic-cli z_getnewaddress        # shielded
build/bin/zclassic-cli z_getbalance <addr>
```

Receiving and sending shielded funds, plus verifying and relaying shielded
transactions, use the in-tree native C23 Sapling implementation. No Rust
toolchain, library, or runtime is part of Z23.

### Serve a site with no domain name and no certificate

Build the bundled Tor and your node becomes a hidden service in-process — no
SOCKS proxy, no second daemon, no port forwarding, no certificate. Your node
serves its own block explorer and REST API on your own `.onion`.

```bash
git submodule update --init vendor/tor      # then build per docs/BUILD.md
build/bin/z23 -tor
build/bin/z23 status                 # your .onion address
```

The default build links a Tor *stub*, so `-tor` runs the node normally without
an onion and says so in the log. The onion is opt-in because it is a large
dependency, not because it is unfinished.

Prefer clearnet? Drop a certificate at `<datadir>/ssl/fullchain.pem` and
`privkey.pem` and the HTTPS explorer starts on port `8443` once the node is near
tip. With no cert and no Tor, there is deliberately **no** public endpoint — the
node is private by default.

### Buy and sell without exposing your IP

Yardsale is the built-in P2P marketplace. Sellers pin up signed, expiring sale
ads; nodes gossip them byte-identically; a buyer settles directly with the
seller through a short bilateral ceremony. There is no matching engine and no
middleman. Everything rides the onion surface — the ad board at `/yardsale`,
and the mutating `accept` step is served **only** on the onion, never on
clearnet HTTPS — so neither side needs to expose an IP address, a domain name,
or an account.

```bash
build/bin/z23 -tor                    # your .onion hosts /yardsale
build/bin/z23 discover help yardsale  # the typed ad/ceremony commands
```

Any node can host its own MVC-style web app on its onion the same way the
explorer and yardsale do — the recipe is
[`docs/cookbook/13_host_your_own_mvc_onion_app.md`](docs/cookbook/13_host_your_own_mvc_onion_app.md).

### Register an on-chain name

ZNAM is a working on-chain registry. Names are 1–63 characters, first-come,
and one name can carry addresses for ZCL, BTC, LTC and DOGE at once, plus
free-form text records like an email or a URL.

```bash
build/bin/zclassic-cli name_register "yourname"
build/bin/zclassic-cli name_resolve  "yourname"
```

### Browse the chain locally

The node serves a block explorer at `/explorer` with a JSON API under `/api`,
over your onion or over HTTPS. Start API discovery at `/api/v1`:
`/api/v1/service-catalog` lists what your node can host, advertise, verify or
construct. It is not on the RPC port — a plain `GET` to `18232` returns
`405`, by design.

### Send messages peer-to-peer

On-chain messages ride inside the 512-byte encrypted Sapling memo field, so
they are shielded and permanent. Off-chain messages go directly between
connected nodes.

```bash
build/bin/zclassic-cli msg_send  <addr> "text"
build/bin/zclassic-cli msg_inbox
```

Be aware: **off-chain messages are plaintext on the wire today.** The encrypted
transport is written but not switched on by default. On-chain messages are
shielded; off-chain ones should be treated as postcards.

### Mine

Equihash 200,9, in the same binary, validating what it mines against the same
rules as the rest of the network.

### Let an AI agent operate the node

The binary contains a typed native command registry with declared input and
output schemas, byte budgets, auth levels, and risk classes, so an agent can
operate the node directly. The live catalog is discoverable; no vendor SDK or
model lock-in is required.

```bash
build/bin/z23 status                 # height, peers, sync, health, one call
build/bin/z23 discover help          # the live command catalog
build/bin/z23 discover search <query>     # find a command by keyword
build/bin/z23 discover schema <leaf>      # exact input keys before you call
build/bin/z23 code map               # navigate the source tree
build/bin/z23 ops logs --pattern='error|warn'
```

Every reply is self-describing and size-bounded, and no failure reply lacks a
`next` action to run. A stall is never silent: either the tip gap grows or a
blocker is named. Full contract:
[`docs/NATIVE_COMMAND_INTERFACE.md`](docs/NATIVE_COMMAND_INTERFACE.md).

**Working on this repository with an agent?** Start with
[`AGENTS.md`](AGENTS.md) — the mission, the safety boundary, the first five
commands in a fresh clone, and the lint gates the agent must respect. The
daily-driver manuals are [`docs/DEVELOPING.md`](docs/DEVELOPING.md) and
[`docs/CODEBASE_MAP.md`](docs/CODEBASE_MAP.md), and
[`docs/AI_SAFETY_GATES.md`](docs/AI_SAFETY_GATES.md) lists the gates that stop
an agent from claiming a result it cannot cite.

### Measure peers, and play P2P games

A P2P game framework rides the same network layer — peer latency in
microseconds, and a working TicTacToe as the reference implementation.

```bash
build/bin/z23 core network peers latency
```

### Build on the C23 Commons

The optional C23 Commons can publish and verify exact packages and support
application layers such as signed spaces and the ZC23 Living Commons
projection. These applications sit below the public full node and package
reproduction network in the product priority order.

```bash
build/bin/z23 zcode guide                    # the creator's map: find, inspect, fetch, create, improve
build/bin/z23 discover search metaverse      # orient in the command tree
build/bin/z23 metaverse property list --input='{"datadir":"/tmp/zcl23-tour"}'
build/bin/z23 zcode commons status --input='{"workspace":"/tmp/zcl23-tour-commons"}'
```

Honest scope: ZC23 patronage today is **simulation-only** — no live token, and
every live-money path fails closed by design. The tour, the architecture, and
the truth-about-status page: [`docs/METAVERSE.md`](docs/METAVERSE.md).

---

## Getting there

`make doctor` probes your machine and prints the exact install line for
anything missing. What you need: **gcc 14+** (or clang with working
`-std=c23`), GNU make, git, and a C++ compiler for the LevelDB test oracle.

```bash
make -j"$(nproc)"   # node + CLI + RPC tool -> build/bin/
make -j"$(nproc)" dev-bin  # a fast non-LTO build for iterating -> build/bin/z23-dev
make -j"$(nproc)" test     # all registered parallel groups
make lint           # the defensive-coding gates
```

The `make dev-bin` target is the non-LTO developer build; invoke it with the
parallel form above so its per-translation-unit work uses the available CPUs.

The first `make` runs `make vendor` once, which downloads pinned third-party
sources, checks each against a pinned SHA-256, and compiles them locally. After
that every build is offline. Clone to passing test suite is about ten minutes
and 1.6 GB of disk; `make first-build-timing` measures it on your own hardware.

The shipped binary links only stock `libc` plus static archives in
`vendor/lib/`, so there is nothing to install at runtime and nothing to keep
updated.

Datadir is `~/.zclassic-c23/` (`-datadir=DIR`). Default ports: P2P `8033`,
RPC `18232`.

**Public start here:** [`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md) is the
fresh-machine walkthrough — build, run a node and explorer, and set up an
isolated development instance.

### When something looks wrong

The node is built to report its own state, so begin with its typed diagnostics:

```bash
build/bin/z23 status                    # height, peers, sync, blocker, health
build/bin/z23 core sync diagnose        # why sync is where it is
build/bin/z23 ops logs --pattern='error|warn'
build/bin/z23 ops health                # synced / has_peers / tip_stale / tip_lag
```

If the height is frozen, `status` names the cause in `primary_blocker` — a stall
is never a silent stop. If peers stay at `0`, add onion seeds to
`~/.config/zclassic23/onion-seeds` so the node can find peers without DNS.
Deeper paths, including kill-9 and out-of-memory recovery, are in
[`docs/RUNBOOK.md`](docs/RUNBOOK.md).

### Reaching the chain tip

A fresh node is honestly empty — `getblockcount` returns `0` until blocks are
actually folded, never a phantom tip — and then syncs from peers. There are no
DNS seeders; the node bootstraps from verified IP seeds baked into the sealed
consensus core plus a Tor onion directory, harvesting peers from each onion's
`/directory.json`. Add your own seeds in `~/.config/zclassic23/onion-seeds`.

A plain start from an empty datadir takes hours. If you already run the C++
`zclassicd`, importing its headers first is the fast path:

```bash
build/bin/z23 --importblockindex ~/.zclassic   # headers FIRST
build/bin/z23                                  # then a normal boot
```

Order matters — skipping the import leaves a ~3.1M-header hole and the node
pins. Every path, including the prebuilt starter pack and its honest limits, is
in [`docs/BOOTSTRAPPING.md`](docs/BOOTSTRAPPING.md) and
[`docs/SYNC.md`](docs/SYNC.md).

---

## Properties, and how to check them yourself

Don't take these on assertion — each one names the mechanism and the command
that shows it.

**Inputs to a validity decision: two.** The binary you compiled, and the
proof-of-work-heaviest header chain. There is no operator attestation, no
certificate authority, and no registry lookup anywhere in a consensus path.

**`core/` is byte-sealed.** Checkpoints, chain params and consensus math are
pinned by `core/MANIFEST.sha3`. Any byte change fails the `check-core-seal`
gate; changing it requires a recorded unseal ([`core/UNSEAL.md`](core/UNSEAL.md)).
This binds an AI agent working on the code exactly as much as it binds you.

```bash
make lint                   # check-core-seal is one of the gates it runs
```

**Consensus stays bit-compatible with `zclassicd`.** Enforced by the
`check-consensus-parity` gate plus a golden-value test group; consensus-changing
contributions are declined
([`docs/CONSENSUS_PARITY_DOCTRINE.md`](docs/CONSENSUS_PARITY_DOCTRINE.md)).

```bash
make -j"$(nproc)" t-fast ONLY=consensus_parity
```

**The process restricts itself before it reports ready.** `-sandbox=steady`
applies `no_new_privs`, `PR_SET_DUMPABLE(0)`, Landlock datadir grants and a
seccomp deny-list, installed with seccomp `TSYNC` so already-running P2P and
validation threads are covered rather than only new ones.

```bash
build/bin/z23 dumpstate sandbox      # per-thread coverage
```

**No subprocess execution.** Zero `system()` and `popen()` in shipped
app/lib/config code, enforced by a lint gate rather than convention.
This describes the full-node runtime boundary. An explicitly admitted C23
Commons build or test action may invoke its bound toolchain only inside the
separate bounded worker lifecycle; fetching source never invokes it.

**Wallet key encryption at rest is single-writer.** The keystore file path
wraps keys in AES-256-GCM (PBKDF2-HMAC-SHA512, 200k iterations), and the
`wallet_keys` / `wallet_sapling_keys` / `wallet_seed` secret columns in
`node.db` have exactly one writer — the encryption-aware `wallet_sqlite`
layer (`lib/wallet/src/wallet_sqlite.c`). The old plaintext mirror
(`node_db_sync_wallet_keys` → `db_wallet_key_save`) is deleted, and a lint
gate ratchets that it never returns. With no passphrase set, keys are stored
as raw 32-byte blobs (Bitcoin-Core unlocked-wallet semantics — the datadir
must be protected). A boot-time scrub
(`wallet_sqlite_scrub_plaintext_r`, run from `config/src/boot_services.c`)
wraps any legacy plaintext secret rows into WKS1 envelopes in place
whenever a passphrase is configured; it never deletes rows. The full
picture is in
[`docs/CUSTODY_MODEL.md`](docs/CUSTODY_MODEL.md).

**Crash recovery is executed, not claimed.** `make test-crash-bootstrap` kill-9s
a node mid-write on an isolated datadir and requires it to fold back to its tip
with no manual repair.

**Read-only queries are constrained by construction.**
`z23 core storage query` is SELECT-only, rejects semicolons, auto-LIMITs,
carries a wall-clock budget, and denies wallet-secret tables by name.

**The gates run on your machine**, without reliance on a hosted CI service:
`make lint`, `make ci`.

Full boundary: [`docs/SECURITY_AND_INTEGRITY.md`](docs/SECURITY_AND_INTEGRITY.md).
The gates that specifically stop an AI agent from claiming a result it cannot
cite: [`docs/AI_SAFETY_GATES.md`](docs/AI_SAFETY_GATES.md).

---

## Where it is today

**Pre-v1.** Don't make it your only mainnet node yet. The bar for v1 is the
eight acceptance criteria in [`docs/MVP.md`](docs/MVP.md).

Working now: full node and validation, wallet, mining, explorer and REST API,
onion service, the yardsale P2P marketplace (signed ad gossip + bilateral
settlement), the ZCODE package commons and metaverse surfaces
(simulation-complete, no live ZC23), ZNAM names, on-chain shielded messaging,
the native command registry, P2P games.

Not finished yet, plainly: off-chain messaging is plaintext on the wire;
cross-chain atomic swaps have contract and settlement plumbing but are not an
end-to-end trade; the older chunk-serving file market (distinct from yardsale)
gates chunks but payment settlement is not wired through; ZC23 issuance and
custody are pre-genesis and simulation-only; the ~1-minute cold sync the
fast-sync stack is designed for is a target, not today's proven path.

Ask the running node rather than treating this page as live state:
`z23 status`.

---

## Going further

- [`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md) — first run on a fresh machine
- [`docs/NATIVE_COMMAND_INTERFACE.md`](docs/NATIVE_COMMAND_INTERFACE.md) — the agent interface
- [`docs/HOW_THE_NODE_WORKS.md`](docs/HOW_THE_NODE_WORKS.md) — the node as a state machine
- [`docs/FRAMEWORK.md`](docs/FRAMEWORK.md) — architecture: the laws and the eight code shapes
- [`docs/CODEBASE_MAP.md`](docs/CODEBASE_MAP.md) — where everything lives
- [`docs/DEVELOPING.md`](docs/DEVELOPING.md) — the developer operating manual
- [`docs/BUILD.md`](docs/BUILD.md) — vendored sources, versions, build steps
- [`docs/RUNBOOK.md`](docs/RUNBOOK.md) — operating and troubleshooting
- [`docs/MVP.md`](docs/MVP.md) — v1 criteria and honest readiness
- [`AGENTS.md`](AGENTS.md) — model-neutral coding-agent entry point

**Something wrong?** `z23 status` first, then
[`docs/RUNBOOK.md`](docs/RUNBOOK.md). File bugs through GitHub Issues — the
templates in [`.github/ISSUE_TEMPLATE/`](.github/ISSUE_TEMPLATE/) ask for the
two things that make a report actionable. Security reports:
[`.github/SECURITY.md`](.github/SECURITY.md). Contributing:
[`.github/CONTRIBUTING.md`](.github/CONTRIBUTING.md).

## License

Copyright 2026 Rhett Creighton. Apache License 2.0 — see [`LICENSE`](LICENSE).
Upstream notices (Bitcoin Core, Zcash, zclassicd, Tor, SQLite, secp256k1,
LevelDB, dcrdex) are in [`NOTICE`](NOTICE); concept attributions in
[`docs/ATTRIBUTIONS.md`](docs/ATTRIBUTIONS.md).
