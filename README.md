<p align="center">
  <img src="docs/assets/z23-banner.svg" alt="Z23 — software made for you, not imposed on you" width="100%">
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache--2.0-blue.svg" alt="license"></a>
  <img src="https://img.shields.io/badge/language-C23-00599C.svg" alt="language">
  <a href="docs/MVP.md"><img src="https://img.shields.io/badge/status-pre--v1-orange.svg" alt="status"></a>
  <a href="docs/DEFENSIVE_CODING.md"><img src="https://img.shields.io/badge/CI-local%20make%20lint-success.svg" alt="CI"></a>
  <a href="#the-whole-thing-in-one-command"><img src="https://img.shields.io/badge/demo-make%20commons--demo-6bd18a.svg" alt="demo"></a>
</p>

**Tell your computer what you want it to do. Watch it do it. Keep the result.**

Z23 is a software workshop that runs on your own machine and is joined to a
peer-to-peer commons. You describe the behavior you want. Your node reuses C23
code that peers already published, writes only the part that is missing, and
shows you it working. Another machine re-derives the exact same bytes. Then
*you* decide whether to accept it — and after that it is yours, and it keeps
working when the agent, the vendor, the registry, or the company is gone.

|  |  |
| --- | --- |
| **What is it?** | A native-software workshop on your machine, joined to a peer-to-peer network. |
| **What can I do?** | Ask for behavior, see it work, own the result, hand it to other people. |
| **Why is it different?** | No app store, no cloud account, no central registry, no permanent AI vendor. |
| **What do I run?** | `make commons-demo` — the whole journey, end to end, on your hardware. |

---

## Tell your computer what to do

You do not browse a store, pick a package, read its docs, and hope. You say
what you want to happen:

```bash
build/bin/z23 zcode work start --datadir=/tmp/z23-workshop \
  --input='{"workspace":".","goal":"count the words in a file and report its longest line"}'
```

Your node answers with a plan, and the first thing in that plan is what it can
**reuse**. Writing code is the last resort, not the first move.

![z23 zcode guide — the one obvious next action](docs/assets/z23-term-guide.svg)

From there the whole journey is one command at a time, and every reply — including
every refusal — names the next safe one, so you never have to remember the table
below.

Each row is `build/bin/z23 <command>`, and each takes `--datadir=DIR` — name one
and the workshop is scoped to it instead of to whatever node you already run.

| What you want | What you run |
| --- | --- |
| Say what the software should do | `zcode work start` |
| Take a peer's package onto this machine, on purpose | `zcode use` |
| Build and test only what was missing | `zcode work run` |
| See the real consequence, in your own words | `zcode work show` |
| Let a second machine re-derive the exact bytes | `zcode package source reproduce` |
| Decide — one exact version, by hand | `zcode work accept` |
| Run it, or hand it to the next person | `zcode use` |

Four properties hold at every step, and the demo below proves each one rather
than asserting it here:

- **Reuse before creation.** Existing C23 is searched first; only the gap is written.
- **No false reuse.** Code you do not hold is never reported as reused.
- **Fetched code is inert.** Arriving on your machine builds nothing and runs nothing.
- **Nothing escalates on its own.** Fetching does not authorize building, building
  does not authorize installing or running, and acceptance stays a human decision
  about one exact version.

---

## The whole thing in one command

```bash
make commons-demo
```

Two fresh nodes start on your machine with empty datadirs. One person asks for
behavior; the other node is a stranger that has never seen any of it. Nothing
outside your machine is contacted — no GitHub, no package server, no registry,
no account.

![make commons-demo — the whole journey, end to end](docs/assets/z23-term-commons-demo.svg)

Exit 0 means **all of that held**. Every step asserts its own promise and stops
at the first broken one, so there is no version of this demo that prints a happy
summary over a failure:

1. A person says what they want.
2. A package that already exists in the commons is fetched from the other node,
   peer to peer — and the arriving bytes execute nothing.
3. Reuse is offered but not taken until the person admits it *on this machine*.
4. Only the missing behavior is written, built and tested.
5. The result is shown in plain words, with the roots kept out of the way until
   you ask for them.
6. The person accepts one exact version, by hand.
7. The accepted work is published, discovered and fetched by the second node,
   which re-derives the exact same source and signs for it.
8. Altered source, an unknown dependency, a stale acceptance, and altered bytes
   in the stored application are each **refused by name** — never by silence.
9. The second machine turns the carrier back into the accepted source, builds
   the application and runs it — and the two machines produce byte-identical
   programs.

The demo lives in
[`tools/dev/commons_journey_acceptance.sh`](tools/dev/commons_journey_acceptance.sh);
it is ordinary product commands, in order, with the assertions written next to
them. It is deliberately not part of `make ci` — it spawns real daemons and runs
real builds.

---

## How the bytes travel

There is no server in this picture. A package is named by *what it contains*,
so any machine holding those bytes can answer for them, and a machine that
answers with different bytes is caught by the name itself.

![how the bytes travel between two nodes](docs/assets/z23-term-commons-topology.svg)

That is the whole distribution model: **publish** what you accepted, **announce**
that you hold it, let a peer **discover** who has it, **fetch** the exact bytes,
**reproduce** them independently, and then **serve** them onward. A provider can
vanish without taking the package with it, because the name was never a URL and
never an account.

---

## What the demo measured

These numbers are a **recording of one real run**, written by the demo itself
and rendered straight onto this page — not a claim typed into a README. Run it
and you get your own.

![what the demo measured](docs/assets/z23-term-commons-proof.svg)

Read the conditions with the numbers. This was two isolated nodes on one
machine, on a regtest chain, over the node's own authenticated overlay — one
peer, not a swarm, and not the open internet. What it demonstrates is that the
journey completes with **zero central services** and produces byte-identical
results on independent nodes. That the same design scales to many peers is a
property of the architecture you can read in the code; it is not something this
repository has measured yet, and this page will not pretend otherwise.

---

## Why a blockchain belongs here

A software commons needs three things a company usually supplies: a clock
everyone agrees on, money that crosses borders without permission, and names
that cannot be repossessed. Z23 gets them from a public chain instead — the
same binary is a full ZClassic (ZCL) node.

- **Proof of work** orders history without a coordinator. No validator set, no
  foundation, no signer list; a validity decision has exactly two inputs — the
  binary you compiled and the heaviest header chain.
- **Payments** let you pay the peer who served you bytes, or the machine that
  built something for you, without an account at anyone's company. Transparent
  and shielded (Sapling) addresses live in the same binary.
- **Optional anchors and names.** You can register an on-chain name, or anchor a
  release so its ordering is publicly checkable. Both are opt-in.
- **Optional Tor.** A node can serve its own onion so publishing does not require
  a domain, a certificate, or a public IP.

And the part that matters most: **your software bytes and your builds stay off
the chain.** The chain never stores your source, never runs your tests, and
never decides whether a build is correct. Reproduction is checked by machines
re-deriving the same bytes from the same inputs — consensus about money is a
separate thing from agreement about a build, and mixing them would make both
worse. The C23 Commons changes ZClassic consensus in no way at all.

---

## Where it is today

**Pre-v1. Do not make it your only mainnet node yet.** The bar for v1 is the
eight acceptance criteria in [`docs/MVP.md`](docs/MVP.md).

Working now: full node and validation, wallet, mining, block explorer and REST
API, in-process onion service, the C23 software commons proved by the demo
above, a peer-to-peer marketplace, on-chain names, shielded on-chain messaging,
the typed command registry, and P2P games.

Not finished yet, plainly:

- **Off-chain messages are plaintext on the wire.** The encrypted transport is
  written but not on by default. On-chain messages are shielded; off-chain ones
  should be treated as postcards.
- Cross-chain atomic swaps have contract and settlement plumbing but are not an
  end-to-end trade.
- The older chunk-serving file market gates chunks, but payment settlement is
  not wired through.
- ZC23 issuance and custody are pre-genesis and simulation-only; every live-money
  path fails closed by design ([`docs/METAVERSE.md`](docs/METAVERSE.md)).
- The ~1-minute cold sync the fast-sync stack is designed for is a target, not
  today's proven path.

Demonstrated evidence and architectural capability are different things, and
this page tries hard to keep them apart. What the demo measures, it measures on
two nodes on one machine. That the design has no central registry, no
coordinator and no privileged node is a property of the code you can read; that
it works at internet scale is not something this repository has proved yet.

Ask the running node rather than trusting this page: `z23 status`.

---

## Everything else

### Ask the node what it can do

You never have to memorize this repository. The binary carries a
native command registry, and the catalog is discoverable from the binary
itself: every command declares its summary, risk class, latency and
availability.

![z23 discover help — the live command surface](docs/assets/z23-term-command-surface.svg)

Two rules make that surface navigable without documentation. Every reply is
self-describing and size-bounded — the `…` above is a bounded reply, not a
broken one. And every reply, including every failure, names the next safe
command, so you are never left holding an error with no move.

Before calling anything, you can read its exact contract — inputs, output
schema, risk, cost and time budget:

![z23 discover describe — the typed contract for one command](docs/assets/z23-term-contract.svg)

This is the same surface an AI agent drives. No vendor SDK, no model lock-in,
and no separate agent API to keep in sync. Full contract:
[`docs/NATIVE_COMMAND_INTERFACE.md`](docs/NATIVE_COMMAND_INTERFACE.md).

### Watch determinism prove itself in 30 seconds

All this one needs is a C compiler — no chain, no node, no network:

```bash
make arena-demo
```

Two pilot programs fly a dogfight in confinement. Your machine then replays the
recorded match with no pilots at all, re-derives the roots, and refuses a copy
with a single byte changed:

```text
ZCODE ARENA
Red Ace defeated Blue Drone 10-6
11,941 deterministic ticks

Replay verification:       MATCH
Result vs pinned roots:    MATCH
Altered control byte:      REFUSED (match-incomplete)
```

![ZCODE Arena](docs/assets/zcode-arena.svg)

Another machine that builds the same sources prints the same digests — and
names the mismatch when it cannot. Write your own pilot, and read the two-node
proof plus the honest gaps, in [`docs/ARENA.md`](docs/ARENA.md).

### The rest of what your node does

Every command below runs against your machine. You are not asking a service.

| You can | Start here |
| --- | --- |
| **Hold and spend ZCL from a node you validated yourself** — transparent and shielded Sapling addresses in the same binary | `build/bin/zclassic-cli z_getnewaddress` |
| **Serve a site with no domain name and no certificate** — in-process Tor onion: no SOCKS proxy, no second daemon, no port forwarding | `build/bin/z23 -tor` |
| **Buy and sell without exposing an IP** — signed, expiring ads gossip byte-identically; buyer and seller settle bilaterally over the onion | `build/bin/z23 discover help yardsale` |
| **Register an on-chain name** — 1–63 characters, carrying ZCL/BTC/LTC/DOGE addresses plus free-form text records | `build/bin/zclassic-cli name_register "yourname"` |
| **Browse your own chain** — block explorer at `/explorer`, JSON API from `/api/v1` | `build/bin/z23 status` |
| **Send shielded messages** — on-chain messages ride inside the 512-byte encrypted Sapling memo | `build/bin/zclassic-cli msg_send <addr> "text"` |
| **Mine** — Equihash in the same binary, validating what it mines against the same rules as everyone else | `build/bin/z23 -gen` |
| **Measure peers and play P2P games** — peer latency in microseconds, with a working TicTacToe as the reference implementation | `build/bin/z23 core network peers latency` |
| **Let an AI agent operate the node** — typed commands with schemas, byte budgets and risk classes | `build/bin/z23 discover help` |

Receiving, sending, verifying and relaying shielded funds all use the in-tree
native C23 Sapling implementation. No Rust toolchain, library or runtime is
part of Z23.

Three things worth knowing before you go looking for them. The default build
links a Tor *stub*, so `-tor` runs the node normally without an onion and says
so in the log; the real onion is opt-in because Tor is a large dependency, not
because it is unfinished (`git submodule update --init vendor/tor`, then
[`docs/BUILD.md`](docs/BUILD.md)). For clearnet, drop a certificate at
`<datadir>/ssl/fullchain.pem` and `privkey.pem` and the HTTPS explorer starts
on port `8443` once the node is near tip. With no certificate and no Tor there
is deliberately **no** public endpoint — the node is private by default.

Any node can host its own MVC-style web app on its onion exactly the way the
explorer does; the recipe is
[`docs/cookbook/13_host_your_own_mvc_onion_app.md`](docs/cookbook/13_host_your_own_mvc_onion_app.md).

### How you check, instead of taking this page's word

**Verify, don't trust.** Any agent may propose code and any node may perform
computation, but each receiving node verifies exact objects, signatures, roots
and evidence under its own local policy. Nothing is accepted because of who
produced it, and no central service or project developer is an authority a node
must rely on.

- Content roots identify exact bytes.
- Signatures identify the key that made a statement.
- Build, test and reproduction receipts record exact observations under bound
  inputs; independent reproduction checks an exact build claim.
- Evidence proves only its declared claim. It does not by itself establish that
  arbitrary code is safe, correct, secure, useful, or worth accepting.

Each property below names the mechanism *and* the command that shows it.

| Property | Check it yourself |
| --- | --- |
| **A validity decision has exactly two inputs** — the binary you compiled, and the proof-of-work-heaviest header chain. No operator attestation, no certificate authority, no registry lookup anywhere in a consensus path. | [`docs/HOW_THE_NODE_WORKS.md`](docs/HOW_THE_NODE_WORKS.md) |
| **`core/` is byte-sealed** — checkpoints, chain params and consensus math are pinned by `core/MANIFEST.sha3`. Any byte change fails `check-core-seal` and needs a recorded unseal ([`core/UNSEAL.md`](core/UNSEAL.md)). This binds an AI agent working on the code exactly as much as it binds you. | `make lint` |
| **Consensus stays bit-compatible with `zclassicd`** — enforced by `check-consensus-parity` plus a golden-value test group; consensus-changing contributions are declined ([`docs/CONSENSUS_PARITY_DOCTRINE.md`](docs/CONSENSUS_PARITY_DOCTRINE.md)). | `make -j"$(nproc)" t-fast ONLY=consensus_parity` |
| **The process restricts itself before it reports ready** — `-sandbox=steady` applies `no_new_privs`, `PR_SET_DUMPABLE(0)`, Landlock datadir grants and a seccomp deny-list, installed with seccomp `TSYNC` so already-running threads are covered, not only new ones. | `build/bin/z23 dumpstate sandbox` |
| **No subprocess execution** — zero `system()` and `popen()` in shipped app/lib/config code, enforced by a gate rather than by convention. An explicitly admitted commons build or test action may invoke its bound toolchain only inside the separate bounded worker lifecycle; fetching source never invokes it. | `make lint` |
| **Wallet secrets have exactly one writer** — the encryption-aware `wallet_sqlite` layer. The old plaintext mirror is deleted and a gate ratchets that it never returns. Keys wrap in AES-256-GCM (PBKDF2-HMAC-SHA512, 200k iterations) under a passphrase ([`docs/CUSTODY_MODEL.md`](docs/CUSTODY_MODEL.md)). | `make lint` |
| **Crash recovery is executed, not claimed** — a node is kill-9ed mid-write on an isolated datadir and must fold back to its tip with no manual repair. | `make test-crash-bootstrap` |
| **Read-only queries are constrained by construction** — SELECT-only, semicolons rejected, auto-`LIMIT`, a wall-clock budget, and wallet-secret tables denied by name. | `build/bin/z23 core storage query` |
| **The gates run on your machine**, with no hosted CI service in the loop. | `make lint && make ci` |

And the boundaries, stated plainly. Z23 has no central coordinator and no
central package registry. It never executes downloaded C merely because it was
fetched or stored. It does not claim that tests, signatures or reproduction
prove general safety. The C23 Commons does not change ZClassic consensus. It
does not require one AI vendor — AI workers are replaceable proposal engines.
And it has no live ZC23 token economics today; those surfaces remain
simulation-only.

Full boundary:
[`docs/SECURITY_AND_INTEGRITY.md`](docs/SECURITY_AND_INTEGRITY.md). The gates
that specifically stop an AI agent from claiming a result it cannot cite:
[`docs/AI_SAFETY_GATES.md`](docs/AI_SAFETY_GATES.md).

### Build it

`make doctor` probes your machine and prints the exact install line for
anything missing. You need **gcc 14+** (or clang with working `-std=c23`), GNU
make, git, and a C++ compiler for the LevelDB test oracle.

```bash
make -j"$(nproc)"          # node + CLI + RPC tool -> build/bin/
make -j"$(nproc)" dev-bin  # fast non-LTO build for iterating -> build/bin/z23-dev
make -j"$(nproc)" test     # all registered parallel groups
make lint                  # the defensive-coding gates
make commons-demo          # the whole product journey, two real nodes
```

The first `make` runs `make vendor` once: it downloads pinned third-party
sources, checks each against a pinned SHA-256, and compiles them locally.
Every build after that is offline. Clone to passing test suite is about ten
minutes and 1.6 GB of disk — `make first-build-timing` measures it on your own
hardware. The shipped binary links only stock `libc` plus static archives in
`vendor/lib/`, so there is nothing to install at runtime and nothing to keep
updated.

Day to day you want `make dev-bin`: it skips LTO, rebuilds only what changed,
and the compile cache that ships in this repository serves the rest.

Datadir is `~/.zclassic-c23/` (`-datadir=DIR`). Default ports: P2P `8033`, RPC
`18232`. The fresh-machine walkthrough is
[`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md).

### When something looks wrong

The node reports its own state, so start with its typed diagnostics rather
than with a log file:

```bash
build/bin/z23 status                    # height, peers, sync, blocker, health
build/bin/z23 core sync diagnose        # why sync is where it is
build/bin/z23 ops logs --pattern='error|warn'
build/bin/z23 ops health                # synced / has_peers / tip_stale / tip_lag
```

A stall is never a silent stop: if the height is frozen, `status` names the
cause in `primary_blocker`. If peers stay at `0`, add onion seeds to
`~/.config/zclassic23/onion-seeds` so the node can find peers without DNS.
Deeper paths, including kill-9 and out-of-memory recovery, are in
[`docs/RUNBOOK.md`](docs/RUNBOOK.md).

### Reaching the chain tip

A fresh node is honestly empty — `getblockcount` returns `0` until blocks are
actually folded, never a phantom tip — and then syncs from peers. There are no
DNS seeders; the node bootstraps from verified IP seeds baked into the sealed
consensus core plus a Tor onion directory, harvesting peers from each onion's
`/directory.json`. Add your own in `~/.config/zclassic23/onion-seeds`.

A plain start from an empty datadir takes hours. If you already run the C++
`zclassicd`, importing its headers first is the fast path — and the order
matters, because skipping the import leaves a ~3.1M-header hole and the node
pins:

```bash
build/bin/z23 --importblockindex ~/.zclassic   # headers FIRST
build/bin/z23                                  # then a normal boot
```

Every path, including the prebuilt starter pack and its honest limits, is in
[`docs/BOOTSTRAPPING.md`](docs/BOOTSTRAPPING.md) and
[`docs/SYNC.md`](docs/SYNC.md).

### Going further

- [`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md) — **Public start here**: first run on a fresh machine
- [`docs/C23_COMMONS_QUICKSTART.md`](docs/C23_COMMONS_QUICKSTART.md) — the journey, end to end
- [`docs/NATIVE_COMMAND_INTERFACE.md`](docs/NATIVE_COMMAND_INTERFACE.md) — the agent interface
- [`docs/HOW_THE_NODE_WORKS.md`](docs/HOW_THE_NODE_WORKS.md) — the node as a state machine
- [`docs/FRAMEWORK.md`](docs/FRAMEWORK.md) — architecture: the laws and the eight code shapes
- [`docs/CODEBASE_MAP.md`](docs/CODEBASE_MAP.md) — where everything lives
- [`docs/DEVELOPING.md`](docs/DEVELOPING.md) — the developer operating manual
- [`docs/BUILD.md`](docs/BUILD.md) — vendored sources, versions, build steps
- [`docs/RUNBOOK.md`](docs/RUNBOOK.md) — operating and troubleshooting
- [`docs/MVP.md`](docs/MVP.md) — v1 criteria and honest readiness
- [`AGENTS.md`](AGENTS.md) — model-neutral coding-agent entry point

**Working on this repository with an agent?** Start at [`AGENTS.md`](AGENTS.md)
— mission, safety boundary, the first commands in a fresh clone, and the gates
the agent must respect.

**Something wrong?** `z23 status` first, then
[`docs/RUNBOOK.md`](docs/RUNBOOK.md). File bugs through GitHub Issues — the
templates in [`.github/ISSUE_TEMPLATE/`](.github/ISSUE_TEMPLATE/) ask for the
two things that make a report actionable. Security reports:
[`.github/SECURITY.md`](.github/SECURITY.md). Contributing:
[`.github/CONTRIBUTING.md`](.github/CONTRIBUTING.md).

### The name

**ZClassic23 is now Z23.** Same node, same protocol, same git history. The
repository moved to <https://github.com/z23c/z23>, and the old
`ZclassiC23/zclassic` URL redirects here. The binaries are `z23` / `z23-dev`,
with the old `zclassic23` names kept as aliases while scripts migrate.
Consensus, chain IDs, network parameters, `zcl.*` protocol domains, package
roots and the `~/.zclassic-c23` datadir are unchanged.

### License

Copyright 2026 Rhett Creighton. Apache License 2.0 — see [`LICENSE`](LICENSE).
Upstream notices (Bitcoin Core, Zcash, zclassicd, Tor, SQLite, secp256k1,
LevelDB, dcrdex) are in [`NOTICE`](NOTICE); concept attributions in
[`docs/ATTRIBUTIONS.md`](docs/ATTRIBUTIONS.md).
