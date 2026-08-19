<p align="center">
  <img src="docs/assets/z23-banner.svg" alt="Z23 — software made for you, not imposed on you" width="100%">
</p>

<p align="center">
  <a href="LICENSE"><img src="docs/assets/badges/license.svg" alt="license: Apache-2.0"></a>
  <img src="docs/assets/badges/language.svg" alt="language: C23">
  <a href="docs/MVP.md"><img src="docs/assets/badges/status.svg" alt="status: pre-v1"></a>
  <a href="#play"><img src="docs/assets/badges/start.svg" alt="start: make arena-demo"></a>
</p>

# A peer-to-peer foundry for C23 applications

**Not a package manager.** A package manager hands you someone else's finished
thing and asks you to trust it. A foundry is a place with parts and machines in
it, where you make the thing you actually wanted — and walk out owning it.

Z23 is that foundry, running on your own machine and joined to a peer-to-peer
commons of C23 source. You play with what is already there, describe the change
you want, keep one exact version, and share it back. No store, no account, no
registry, no permanent AI vendor. What you keep still runs when every one of
those is gone.

![Play, change, keep, share — the whole of Z23](docs/assets/z23-journey.svg)

---

## PLAY

**Run a real application right now.** You need a C compiler. That is the whole
list — no node, no chain, no network, no account, no sign-up.

```bash
make arena-demo
```

Two pilot programs fly a dogfight. Your machine then replays the recorded match
with no pilots at all, re-derives the roots, and refuses a copy with a single
byte changed.

```text
ZCODE ARENA
Red Ace defeated Blue Drone 10-6

Replay verification:       MATCH
Result vs pinned roots:    MATCH
Altered control byte:      REFUSED (match-incomplete)
Pilot confinement:         Landlock + seccomp
```

![ZCODE Arena](docs/assets/zcode-arena.svg)

Another machine that builds the same sources prints the same digests, and names
the mismatch when it cannot. Write your own pilot in
[`docs/ARENA.md`](docs/ARENA.md).

Once you are running a node, the same verb reaches the commons: `z23 zcode use`
takes a peer's package onto this machine, on purpose, as one deliberate act.

---

## CHANGE

**Say what you want to be different.** Not a package name — a sentence.

```bash
build/bin/z23 zcode work start --datadir=/tmp/z23-workshop \
  --input='{"workspace":".","goal":"count the words in a file and report its longest line"}'
```

Your node answers with a plan, and the first thing in that plan is what it can
**reuse**. Writing new code is the last resort, not the first move.

![z23 zcode guide — the one obvious next action](docs/assets/z23-term-guide.svg)

From there it is one command at a time — `zcode work run` builds and tests only
what was missing, `zcode work show` tells you what actually happened in your own
words. Every reply, including every refusal, names the next safe command, so
there is nothing to memorise.

Four rules hold the whole way through:

- **Reuse before creation.** Existing C23 is searched first; only the gap is written.
- **No false reuse.** Code you do not hold is never reported as reused.
- **Fetched code is inert.** Arriving on your machine builds nothing and runs nothing.
- **Nothing escalates on its own.** Fetching does not authorise building, and
  building does not authorise installing or running.

---

## KEEP

**One exact version, accepted by hand.**

```bash
build/bin/z23 zcode work accept --input='{"work":"latest"}'
```

That is the human decision, and nothing else in the system can make it for you.
Not the agent that proposed the code, not the peer that served it, not a policy
file. After it, the result is yours: exact bytes you hold, that keep working
when the agent, the vendor, the registry or the company is gone.

---

## SHARE

**Publish it, and a stranger re-derives your bytes.**

```bash
build/bin/z23 zcode publish
build/bin/z23 zcode network publish
```

A package is named by *what it contains*, so any machine holding those bytes can
answer for them, and a machine that answers with different bytes is caught by
the name itself. There is no server in this picture.

![how the bytes travel between two nodes](docs/assets/z23-term-commons-topology.svg)

Publish what you accepted, announce that you hold it, let a peer discover who
has it, fetch the exact bytes, reproduce them independently, then serve them
onward. A provider can vanish without taking the package with it, because the
name was never a URL and never an account.

> [!IMPORTANT]
> **What goes public, and what does not.**
>
> Your node hosts nothing by default. Serving package bytes to peers is opt-in
> with `-packagehost=1`, and announcing yourself as a provider on the overlay is
> a further, separate step.
>
> A published release is **author-signed and permissively licensed**. Publishing
> is refused unless the release carries a secp256k1 signature whose pre-image
> includes the package root — binding the author's key to those exact bytes —
> *and* an SPDX identifier from a frozen allowlist: `0BSD`, `MIT`, `Apache-2.0`,
> `BSD-2-Clause`, `BSD-3-Clause`, `ISC`, `Zlib`, with the matching `LICENSE` text
> present in the package. Anything else is not a release.
>
> Read that scope precisely: it is an admission rule for *becoming* a public
> release, not a filter applied to every byte on the wire. Chunk serving is
> authenticated by content hash, so a node re-serving bytes it fetched does not
> re-run the licence check. Anyone may publish — namespaces are first-come, bound
> to a key, with no central approver.
> Details: [`docs/P2P_SOURCE_HOSTING.md`](docs/P2P_SOURCE_HOSTING.md).

---

## All four, in one command

```bash
make commons-demo
```

Two fresh nodes start on your machine with empty datadirs. One person asks for
behavior; the other node is a stranger that has never seen any of it. Nothing
outside your machine is contacted.

![make commons-demo — the whole journey, end to end](docs/assets/z23-term-commons-demo.svg)

Exit 0 means all of it held. Every step asserts its own promise and stops at the
first broken one, so no version of this demo prints a happy summary over a
failure:

1. A person asks for behavior, and a package that already exists in the commons
   arrives from the other node — peer to peer, executing nothing.
2. Reuse is offered, but not taken until the person admits it *on this machine*.
3. Only the missing behavior is written, built, tested and shown in plain words.
4. The person accepts one exact version, by hand.
5. The second node discovers it, fetches it and re-derives the identical source.
6. Altered source, an unknown dependency, a stale acceptance and altered stored
   bytes are each **refused by name** — never by silence.
7. Both nodes build the application and produce byte-identical programs.

It is ordinary product commands in order, with the assertions written next to
them:
[`tools/dev/commons_journey_acceptance.sh`](tools/dev/commons_journey_acceptance.sh).

### What that run measured

A recording of one real run, written by the demo itself and drawn onto this
page. Run it and you get your own.

![what the demo measured](docs/assets/z23-term-commons-proof.svg)

Read the conditions with the numbers: two isolated nodes on one machine, a
regtest chain, one peer over the node's own authenticated overlay — not a swarm,
not the open internet. It shows the journey completing with zero central
services and byte-identical results on independent nodes; both nodes share a
physical host, so this is node independence, not hardware independence. For the
same journey across separate machines with the publisher then taken offline:
`make commons-multihost-acceptance`.

---

## Where this goes

The arena is a toy that happens to be honest. The real target is applications
people actually want to use, made the same way — and the flagship is a 3D
fly-over: an open world you fly through.

![the fly-over, running standalone today](docs/assets/z23-flyover.gif)

**It is not in the commons yet.** Today it is a standalone raylib project at
[`full-node-firewall-fly-over`](https://github.com/RhettCreighton/full-node-firewall-fly-over)
— same author, Apache-2.0 — and getting it in means porting it to C23 with no
vendor runtime, splitting it into packages, adding the `LICENSE` text the publish
rule above demands even of your own code, and taking it through the same
PLAY → CHANGE → KEEP → SHARE path as everything else. That work is the point: when it lands, changing the world
you are flying through is a sentence you type, and the version you like is one
you keep and hand to someone else.

---

<!-- Everything below is machinery. Read it when you want it, not before. -->

## Why a blockchain belongs here

A software commons needs three things a company usually supplies: a clock
everyone agrees on, money that crosses borders without permission, and names
that cannot be repossessed. Z23 gets them from a public chain instead — the same
binary is a full ZClassic (ZCL) node.

- **Proof of work** orders history without a coordinator. No validator set, no
  foundation, no signer list.
- **Payments** let you pay the peer who served you bytes, or the machine that
  built something for you, with no account at anyone's company. Transparent and
  shielded (Sapling) addresses live in the same binary.
- **Optional anchors and names.** Register an on-chain name, or anchor a release
  so its ordering is publicly checkable. Both are opt-in.
- **Optional Tor.** A node can serve its own onion, so publishing needs no
  domain, certificate or public IP.

And the part that matters most: **your software bytes and your builds stay off
the chain.** The chain never stores your source, never runs your tests and never
decides whether a build is correct — reproduction is machines re-deriving the
same bytes from the same inputs. The C23 Commons changes ZClassic consensus in
no way at all.

## What else your node does

Every command below runs against your machine. You are not asking a service.

| You can | Start here |
| --- | --- |
| **Hold and spend ZCL from a node you validated yourself** — transparent and shielded Sapling addresses in one binary | `build/bin/zclassic-cli z_getnewaddress` |
| **Serve a site with no domain and no certificate** — in-process Tor onion, no SOCKS proxy, no second daemon | `build/bin/z23 -tor` |
| **Buy and sell without exposing an IP** — signed, expiring ads gossip byte-identically; settlement is bilateral over the onion | `build/bin/z23 discover help yardsale` |
| **Register an on-chain name** — carrying ZCL/BTC/LTC/DOGE addresses plus free-form records | `build/bin/zclassic-cli name_register "yourname"` |
| **Browse your own chain** — block explorer at `/explorer`, JSON API from `/api/v1` | `build/bin/z23 status` |
| **Send shielded messages** — on-chain messages ride inside the encrypted Sapling memo | `build/bin/zclassic-cli msg_send <addr> "text"` |
| **Mine** — Equihash in the same binary, validating what it mines against everyone else's rules | `build/bin/z23 -gen` |
| **Measure peers and play P2P games** — peer latency in microseconds, with a working TicTacToe as the reference | `build/bin/z23 core network peers latency` |
| **Let an AI agent operate the node** — typed commands with schemas, byte budgets and risk classes | `build/bin/z23 discover help` |

Receiving, sending, verifying and relaying shielded funds all use the in-tree
native C23 Sapling implementation. No Rust toolchain, library or runtime is part
of Z23.

### You never have to memorise this repository

The binary carries a native command registry, so the catalog is discoverable
from the binary itself — summary, risk class, latency and availability per
command, and `discover describe` for the exact contract before you call it.

![z23 discover help — the live command surface](docs/assets/z23-term-command-surface.svg)

Every reply is self-describing and size-bounded (the `…` is a bounded reply, not
a broken one), and every reply — including every failure — names the next safe
command. This is the same surface an AI agent drives: no vendor SDK, no model
lock-in, no separate agent API to keep in sync. Full contract:
[`docs/NATIVE_COMMAND_INTERFACE.md`](docs/NATIVE_COMMAND_INTERFACE.md).

<details>
<summary>Three things worth knowing before you go looking for them</summary>

The default build links a Tor *stub*, so `-tor` runs the node normally without
an onion and says so in the log; the real onion is opt-in because Tor is a large
dependency, not because it is unfinished (`git submodule update --init
vendor/tor`, then [`docs/BUILD.md`](docs/BUILD.md)).

For clearnet, drop a certificate at `<datadir>/ssl/fullchain.pem` and
`privkey.pem` and the HTTPS explorer starts on port `8443` once the node is near
tip. With no certificate and no Tor there is deliberately **no** public
endpoint — the node is private by default.

Any node can host its own MVC-style web app on its onion exactly the way the
explorer does; the recipe is
[`docs/cookbook/13_host_your_own_mvc_onion_app.md`](docs/cookbook/13_host_your_own_mvc_onion_app.md).

</details>

## Verify, don't trust

Any agent may propose code and any node may perform computation, but each
receiving node verifies exact objects, signatures, roots and evidence under its
own local policy. Nothing is accepted because of who produced it, and no central
service or project developer is an authority a node must rely on. Evidence
proves only its declared claim: it does not establish that arbitrary code is
safe, correct, secure, useful, or worth accepting.

Every property above, paired with the command that shows it, is in
[`docs/SECURITY_AND_INTEGRITY.md`](docs/SECURITY_AND_INTEGRITY.md#check-it-yourself);
the gates that stop an AI agent citing a result it cannot back are in
[`docs/AI_SAFETY_GATES.md`](docs/AI_SAFETY_GATES.md).

## Build it

`make doctor` probes your machine and prints the exact install line for anything
missing. You need **gcc 14+** (or clang with working `-std=c23`), GNU make, git,
and a C++ compiler for the LevelDB test oracle.

```bash
make -j"$(nproc)"          # node + CLI + RPC tool -> build/bin/
make -j"$(nproc)" dev-bin  # fast non-LTO build for iterating -> build/bin/z23-dev
make -j"$(nproc)" test     # all registered parallel groups
make lint                  # the defensive-coding gates
make commons-demo          # the whole product journey, two real nodes
```

The first `make` runs `make vendor` once: it downloads pinned third-party
sources, checks each against a pinned SHA-256, and compiles them locally. Every
build after that is offline. The shipped binary links only stock `libc` plus
static archives in `vendor/lib/`, so there is nothing to install at runtime.

Day to day you want `make dev-bin`: it skips LTO, rebuilds only what changed,
and the compile cache that ships in this repository serves the rest.

Datadir is `~/.zclassic-c23/` (`-datadir=DIR`). Default ports: P2P `8033`, RPC
`18232`. The fresh-machine walkthrough, including syncing to the chain tip, is
[`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md).

## If it looks stuck

The node reports its own state, so start with its typed diagnostics rather than
with a log file:

```bash
build/bin/z23 status                    # height, peers, sync, blocker, health
build/bin/z23 core sync diagnose        # why sync is where it is
build/bin/z23 ops logs --pattern='error|warn'
build/bin/z23 ops health                # synced / has_peers / tip_stale / tip_lag
```

A stall is never a silent stop: if the height is frozen, `status` names the cause
in `primary_blocker`. If peers stay at `0`, add onion seeds to
`~/.config/zclassic23/onion-seeds` so the node can find peers without DNS.
Deeper paths, including kill-9 and out-of-memory recovery, are in
[`docs/RUNBOOK.md`](docs/RUNBOOK.md).

## Where it is today

**Pre-v1. Do not make it your only mainnet node yet.** The bar for v1 is the
acceptance criteria in [`docs/MVP.md`](docs/MVP.md).

Working now: full node and validation, wallet, mining, block explorer and REST
API, in-process onion service, the C23 software commons proved by the demo
above, a peer-to-peer marketplace, on-chain names, shielded on-chain messaging,
the typed command registry, and P2P games.

Open, plainly:

- **Off-chain messages are plaintext on the wire.** The encrypted transport is
  written but not on by default. On-chain messages are shielded; off-chain ones
  are postcards.
- **Cold start to chain tip** has not met its acceptance bar on a stopwatch.
- **A live sale over the real store path** — onion, file transfer, memo-bound
  credit — is proved in-process, not yet end to end between machines.
- **The soak window** has not completed cleanly, and the consensus-parity
  from-genesis canary still needs an accumulated pass.
- **Cross-chain atomic swaps** have contract and settlement plumbing but are not
  an end-to-end trade.
- **ZC23 issuance and custody** are pre-genesis and simulation-only; every
  live-money path fails closed by design
  ([`docs/METAVERSE.md`](docs/METAVERSE.md)).

Demonstrated evidence and architectural capability are different things. That
the design has no central registry, no coordinator and no privileged node is a
property of the code you can read; that it works at internet scale is not
something this repository has proved. Ask the running node rather than this
page: `z23 status`.

## Where to go next

| If you want to | Read |
| --- | --- |
| **Run it** | [`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md) — **Public start here**, first run on a fresh machine |
| **Walk the whole journey** | [`docs/C23_COMMONS_QUICKSTART.md`](docs/C23_COMMONS_QUICKSTART.md) |
| **Operate and troubleshoot** | [`docs/RUNBOOK.md`](docs/RUNBOOK.md), [`docs/BOOTSTRAPPING.md`](docs/BOOTSTRAPPING.md), [`docs/SYNC.md`](docs/SYNC.md) |
| **Check the claims** | [`docs/SECURITY_AND_INTEGRITY.md`](docs/SECURITY_AND_INTEGRITY.md), [`docs/HOW_THE_NODE_WORKS.md`](docs/HOW_THE_NODE_WORKS.md), [`docs/MVP.md`](docs/MVP.md) |
| **Build on it** | [`docs/BUILD.md`](docs/BUILD.md), [`docs/FRAMEWORK.md`](docs/FRAMEWORK.md), [`docs/CODEBASE_MAP.md`](docs/CODEBASE_MAP.md), [`docs/DEVELOPING.md`](docs/DEVELOPING.md) |
| **Drive it as an AI agent** | [`docs/NATIVE_COMMAND_INTERFACE.md`](docs/NATIVE_COMMAND_INTERFACE.md), and [`AGENTS.md`](AGENTS.md) before changing this repository |
| **Report something** | Bugs: [`.github/ISSUE_TEMPLATE/`](.github/ISSUE_TEMPLATE/) · Security: [`.github/SECURITY.md`](.github/SECURITY.md) · Contributing: [`.github/CONTRIBUTING.md`](.github/CONTRIBUTING.md) |

## The name, and the license

**ZClassic23 is now Z23.** Same node, same protocol, same git history. The
repository moved to <https://github.com/z23c/z23>, and the old
`ZclassiC23/zclassic` URL redirects here. The binaries are `z23` / `z23-dev`,
with the old `zclassic23` names kept as aliases while scripts migrate.
Consensus, chain IDs, network parameters, `zcl.*` protocol domains, package
roots and the `~/.zclassic-c23` datadir are unchanged.

Copyright 2026 Rhett Creighton. Apache License 2.0 — see [`LICENSE`](LICENSE).
Upstream notices (Bitcoin Core, Zcash, zclassicd, Tor, SQLite, secp256k1,
LevelDB, dcrdex) are in [`NOTICE`](NOTICE); concept attributions in
[`docs/ATTRIBUTIONS.md`](docs/ATTRIBUTIONS.md).
