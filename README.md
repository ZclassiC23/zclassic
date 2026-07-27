# ZClassic23

[![license](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![language](https://img.shields.io/badge/language-C23-00599C.svg)](#)
[![status](https://img.shields.io/badge/status-pre--v1-orange.svg)](docs/MVP.md)
[![CI](https://img.shields.io/badge/CI-local%20make%20lint-success.svg)](docs/DEFENSIVE_CODING.md)

**Run your own money, your own name, your own website, and your own private
network service — from one binary you compiled yourself, on hardware you
control, with nothing in the trust path but proof-of-work.**

No hosting provider. No domain registrar. No certificate authority. No DNS. No
API keys. No accounts. `make`, then run it.

```bash
git clone https://github.com/ZclassiC23/zclassic.git && cd zclassic
make
build/bin/zclassic23
```

---

## What you can do with it

Each of these is a thing you own outright once the node is running.

### Hold and move money nobody can freeze

A full ZClassic node with a built-in wallet — transparent addresses and
shielded Sapling addresses in the same binary. You are not asking a service for
your balance; you validated every block yourself.

```bash
build/bin/zclassic-cli getnewaddress          # transparent
build/bin/zclassic-cli z_getnewaddress        # shielded
build/bin/zclassic-cli z_getbalance <addr>
```

Receiving shielded funds, and verifying and relaying everyone else's shielded
transactions, works in the default build. *Sending* shielded value needs the
Sapling prover: rebuild with `make ZCL_WITH_RUST=1`. Without it `z_sendmany`
refuses with a typed error naming that flag — it never fails silently.

### Publish a website that has no address to seize

Build the bundled Tor and your node becomes a hidden service in-process — no
SOCKS proxy, no second daemon, no port forwarding, no certificate. Your node
serves its own block explorer and REST API on your own `.onion`.

```bash
git submodule update --init vendor/tor      # then build per docs/BUILD.md
build/bin/zclassic23 -tor
build/bin/zclassic23 status                 # your .onion address
```

The default build links a Tor *stub*, so `-tor` runs the node normally without
an onion and says so in the log. The onion is opt-in because it is a large
dependency, not because it is unfinished.

Prefer clearnet? Drop a certificate at `<datadir>/ssl/fullchain.pem` and
`privkey.pem` and the HTTPS explorer starts on port `8443` once the node is near
tip. With no cert and no Tor, there is deliberately **no** public endpoint — the
node is private by default.

### Claim a name that is yours on-chain

ZNAM is a working on-chain registry. Names are 1–63 characters, first-come,
and one name can carry addresses for ZCL, BTC, LTC and DOGE at once, plus
free-form text records like an email or a URL.

```bash
build/bin/zclassic-cli name_register "yourname"
build/bin/zclassic-cli name_resolve  "yourname"
```

### Browse the whole chain from your own machine

The node serves a block explorer at `/explorer` with a JSON API under `/api`,
over your onion or over HTTPS. Start API discovery at `/api/v1`:
`/api/v1/service-catalog` lists what your node can host, advertise, verify or
construct. It is not on the RPC port — a plain `GET` to `18232` returns
`405`, by design.

### Send messages with no server in the middle

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

### Hand the whole thing to an AI agent

This is the part that does not exist anywhere else. The binary contains a typed
native command registry — over a hundred commands with declared input and output
schemas, byte budgets, auth levels and risk classes — so an agent operates your
node directly. No curl, no log scraping, no separate server, no vendor SDK, no
model lock-in.

```bash
build/bin/zclassic23 status                 # height, peers, sync, health, one call
build/bin/zclassic23 discover help          # the live command catalog
build/bin/zclassic23 code map               # navigate the source tree
build/bin/zclassic23 ops logs --pattern='error|warn'
```

Every reply is self-describing and size-bounded, and no failure reply lacks a
`next` action to run. A stall is never silent: either the tip gap grows or a
blocker is named. Full contract:
[`docs/NATIVE_COMMAND_INTERFACE.md`](docs/NATIVE_COMMAND_INTERFACE.md).

### Play with it

A P2P game framework rides the same network layer — peer latency in
microseconds, and a working TicTacToe as the reference implementation.

```bash
build/bin/zclassic23 core network peers latency
```

---

## Getting there

`make doctor` probes your machine and prints the exact install line for
anything missing. What you need: **gcc 14+** (or clang with working
`-std=c23`), GNU make, git, and a C++ compiler for LevelDB. Rust is optional
and only for sending shielded value.

```bash
make                # node + CLI + RPC tool -> build/bin/
make dev-bin        # a fast non-LTO build for iterating -> build/bin/zclassic23-dev
make test           # all registered parallel groups
make lint           # the defensive-coding gates
```

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

The node is built to tell you, so ask it rather than reading logs. Four
commands cover almost everything:

```bash
build/bin/zclassic23 status                    # height, peers, sync, blocker, health
build/bin/zclassic23 core sync diagnose        # why sync is where it is
build/bin/zclassic23 ops logs --pattern='error|warn'
build/bin/zclassic23 ops health                # synced / has_peers / tip_stale / tip_lag
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
build/bin/zclassic23 --importblockindex ~/.zclassic   # headers FIRST
build/bin/zclassic23                                  # then a normal boot
```

Order matters — skipping the import leaves a ~3.1M-header hole and the node
pins. Every path, including the prebuilt starter pack and its honest limits, is
in [`docs/BOOTSTRAPPING.md`](docs/BOOTSTRAPPING.md) and
[`docs/SYNC.md`](docs/SYNC.md).

---

## Why you can trust it

- **Two things, and no third.** Every trust claim reduces to the binary you
  compiled and the proof-of-work-heaviest header chain. No operator vouching,
  no certificate authorities, no trusted registries.
- **The consensus core is sealed.** `core/` — checkpoints, chain params,
  consensus math — is pinned to a SHA3-256 manifest. Any byte change fails a
  hard gate unless it goes through a documented owner ritual
  ([`core/UNSEAL.md`](core/UNSEAL.md)). Your node cannot quietly disagree with
  the network, and neither can an AI agent working on it.
- **Consensus compatibility is inviolable.** ZClassic23 stays bit-for-bit
  compatible with `zclassicd`; consensus-changing contributions are declined on
  principle ([`docs/CONSENSUS_PARITY_DOCTRINE.md`](docs/CONSENSUS_PARITY_DOCTRINE.md)).
- **It locks itself down as it boots.** `-sandbox=steady` applies
  `no_new_privs`, Landlock datadir grants and a seccomp deny-list across
  *every* running thread, as the last boot stage. Witness it with
  `dumpstate sandbox`.
- **No shell-outs anywhere.** Zero `system()`/`popen()` in shipped code,
  lint-enforced.
- **Your wallet is encrypted at rest.** AES-256-GCM for new wallets. An
  existing plaintext wallet still loads, with a warning.
- **Crash recovery is demonstrated, not asserted.**
  `make test-crash-bootstrap` kill-9s a node mid-write and proves it folds back
  to its tip with no manual repair.
- **Gates run on your machine**, not in someone's cloud: `make lint`,
  `make ci`.

Details: [`docs/SECURITY_AND_INTEGRITY.md`](docs/SECURITY_AND_INTEGRITY.md).
The gates that specifically stop an AI agent from claiming a victory it cannot
cite: [`docs/AI_SAFETY_GATES.md`](docs/AI_SAFETY_GATES.md).

---

## Where it is today

**Pre-v1.** Don't make it your only mainnet node yet. The bar for v1 is the
eight acceptance criteria in [`docs/MVP.md`](docs/MVP.md).

Working now: full node and validation, wallet, mining, explorer and REST API,
onion service, ZNAM names, on-chain shielded messaging, the native command
registry, P2P games.

Not finished yet, plainly: off-chain messaging is plaintext on the wire;
cross-chain atomic swaps have contract and settlement plumbing but are not an
end-to-end trade; the file marketplace serves and gates chunks but payment
settlement is not wired through; the ~1-minute cold sync the fast-sync stack is
designed for is a target, not today's proven path.

Ask the running node rather than trusting this page: `zclassic23 status`.

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
- [`CLAUDE.md`](CLAUDE.md) — the agent's own daily-driver reference

**Something wrong?** `zclassic23 status` first, then
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
