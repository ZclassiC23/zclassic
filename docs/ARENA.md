# ZCODE Arena

The Arena is the smallest complete example of what Z23 is for: someone
writes exact C23, any node builds and runs it under bounded authority, and
every other node can reproduce the result byte-for-byte instead of taking
anyone's word for it.

It is a headless aerial dogfight. Two **pilot** programs fly a team each. The
match core advances 60 ticks per second of match time with integer arithmetic
only, so the same seed and the same controls produce the same bytes on any
machine and any compiler. The match writes a **replay** — the exact control
stream plus the final state — and two cryptographic roots over it.

![ZCODE Arena](assets/zcode-arena.svg)

## Run it

```bash
make arena-demo
```

No blockchain sync, no Tor, no wallet, no browser, no JavaScript, no Python, no
network, and no running node. It compiles the arena core, two pilots and the
runner, plays a match, re-simulates the replay, checks the roots against the
pinned reference, and proves that a single altered byte is refused.

Expected output:

```text
ZCODE ARENA
Red Ace defeated Blue Drone 10-6
11,941 deterministic ticks

Replay verification:       MATCH
Result vs pinned roots:    MATCH
Altered control byte:      REFUSED (match-incomplete)

Seed:                      7 (3v3)
Red pilot:                 Red Ace — zdogace 0.1.1
Blue pilot:                Blue Drone — zdogdrone 0.1.0
Replay root:               05ed352dbb2213aad289cdf403d424d18d9ae075db57252a52c4e745a25e8396
Final-state root:          e4b37a9b94547cead91a7d4ae2a63b0385b29a99bb603bd0ac3519cebd270ebd
State-root chain:          657cbc598e8cfff4e3a67e0b11de17a6b576be686ae924149614eca3e156f87b
```

Those three roots are the acceptance. If your machine prints them, your machine
ran the same match — not a similar one.

Related targets:

| Command | What it does |
|---|---|
| `make arena-demo` | play, verify, check the pinned roots, refuse a tampered replay |
| `make arena-svg` | regenerate `docs/assets/zcode-arena.svg` from a freshly verified match |
| `make arena-svg-check` | fail if the committed artwork is stale |
| `make arena-demo-opt-parity` | rebuild the pilots at `-O0` and `-O2` and require identical roots |
| `make tools/arena-selftest` | run the arena core's and both pilots' own test suites |

## The pieces

| Package | Role |
|---|---|
| [`packages/zdogfight`](../packages/zdogfight) | the match core: world, flight model, guns, kills, scoring, canonical state encoding, pilot ABI |
| [`packages/zdogace`](../packages/zdogace) | a pursuit pilot — flies red |
| [`packages/zdogdrone`](../packages/zdogdrone) | a simple patrol pilot — flies blue |
| [`packages/zprng`](../packages/zprng) | the only entropy source, drawn solely by respawns |
| [`tools/arena_runner.c`](../tools/arena_runner.c) | plays a match between two confined pilot processes; also verifies a replay |
| [`tools/arena_svg.c`](../tools/arena_svg.c) | renders a verified replay to a deterministic SVG |

A pilot is an ordinary program. Per tick, for each of its living planes, it
reads one 82-byte observation frame on stdin and writes one 7-byte control
frame on stdout. That is the whole interface.

Each pilot runs **confined**: a Landlock domain whose only filesystem grant is
read+execute on the pilot binary itself, the session seccomp deny-list with
W^X, a scrubbed environment, and a CPU-seconds budget. A pilot that crashes,
stalls past its budget, or sends a short frame is marked dead, and from that
tick onward its whole team receives neutral controls — recorded in the replay
like any other control. A misbehaving pilot cannot abort a match or make it
nondeterministic; it can only lose.

Confinement is a property of your kernel, not of the match. `arena_runner`
refuses to run unconfined by default and exits 3 with a named reason when
Landlock or seccomp is unavailable — inside some containers, on older kernels,
under some VMs. `make arena-demo` catches that one exit code, re-runs with
`--no-sandbox`, and prints which mode it used:

```text
Pilot confinement:         Landlock + seccomp
```

The roots are identical either way, because confinement bounds what a pilot
*process* may do and is not an input to the simulation. Nothing else about the
demo changes: every root, the re-simulation and the tamper refusal are checked
at full strength in both modes. If you are running a pilot you did not write,
use a kernel that can confine it.

## Write your own pilot

Copy the smaller pilot and change its decision function:

```bash
cp -r packages/zdogdrone packages/mypilot
```

Then edit `packages/mypilot/src/`. The decision function receives a
`zdog_obs` — your plane's position, attitude, speed, health, the score, ticks
left, and the nearest living enemy's relative position, distance, velocity and
health — and fills a `zdog_ctl` with roll, pitch, throttle and a fire flag.

Rules your pilot must respect to stay deterministic: integer arithmetic only
(the core has no floating point anywhere), no clock, no `rand()`, no
filesystem, no network, no allocation. `zdog_sin16`/`zdog_cos16` are exported
so your bearing maths uses the exact table the simulation uses.

Build and fly it against the shipped pilots:

```bash
cc -std=c23 -O1 -static -D_POSIX_C_SOURCE=200809L \
   -Ipackages/mypilot/include -Ipackages/zdogfight/include \
   -Ipackages/zprng/include \
   packages/mypilot/app/main.c packages/mypilot/src/*.c \
   packages/zdogfight/src/zdogfight.c packages/zdogfight/src/zdogfix.c \
   packages/zprng/src/zprng.c -o /tmp/mypilot -lm

build/bin/arena_runner --seed 7 --planes-per-team 3 \
    --pilot-red /tmp/mypilot --pilot-blue build/bin/pilot_zdogdrone \
    --replay-out /tmp/my.replay
```

`-static` is required: the sandbox's W^X denial refuses the executable mapping
a dynamic loader needs.

## Verify a replay

```bash
build/bin/arena_runner --verify-replay /tmp/my.replay
```

Verification re-applies the recorded control frames with **no pilots at all**
and requires three things: the match reaches its end phase at exactly the
recorded tick count, the re-encoded final state is byte-identical to the block
stored in the file, and the recomputed roots match. Anything else exits 1 with
a named mismatch — `header-magic`, `size`, `ctl-frame`, `match-incomplete`,
`tick-count`, or `final-state`.

That is why the demo's tamper leg matters. It flips exactly one byte of the
recorded control stream and the verifier refuses by name. "Verified" here is a
predicate somebody else can run, not an adjective this project prints about
itself.

The three roots:

- **replay root** — SHA3-256 over the whole replay file. Identifies the exact
  match, controls included.
- **final-state root** — SHA3-256 over the 2,163-byte canonical final-state
  encoding. Identifies the outcome independently of how it was reached.
- **state-root chain** — a SHA3-256 chain folded every 600 completed ticks, so
  two nodes that disagree can find the first ten-second window where they
  diverged instead of only learning that they did.

## The two-node proof

`make arena-demo` proves reproduction on one machine. The cross-node proof is
[`tools/dev/arena_acceptance.sh`](../tools/dev/arena_acceptance.sh), and it is
the reason the pinned roots above are worth anything.

It stands up two isolated nodes on one host. Node A serves the four published
arena packages. Node B starts empty, fetches them over the package swarm,
installs each through the confined build-and-test worker, and builds its own
pilot binaries from its own store. Then both nodes play the same match
independently. The proof asserts that B's pilot binaries and installed
archives are byte-identical to A's, that both replays are byte-identical, and
that all three roots and the winner and tick count agree. It also flips a byte
in a copy of B's replay and requires a named refusal, and SIGKILLs a build
worker mid-build to show the identical retry reproduces identical archives.

Run it deliberately — it spawns two real node processes:

```bash
bash tools/dev/arena_acceptance.sh
```

## Honest gaps

These are named because they are real, not because they are about to be fixed.

- **The Arena is not on-chain and not consensus.** Nothing here touches
  ZClassic consensus, block validation, wallet custody, or the ledger. There is
  no live ZC23 token economics; scoring is a simulation.
- **The pinned demo pilots are repo-source, not the published packages.** The
  published `zdogace` 0.1.0 carries a steering-sign quirk; the sign-fixed 0.1.1
  in `packages/` is what the demo builds. Both are exact and deterministic —
  they simply play different matches. The published-package leg of the
  acceptance script pins its own separate roots.
- **The cross-node proof runs two nodes on one host.** It proves independent
  fetch, install, build and replay across two disjoint datadirs and stores; it
  does not prove it across two CPU microarchitectures. It is no longer the only
  cross-machine evidence: the `arena (make arena-demo)` job in
  [`.github/workflows/build.yml`](../.github/workflows/build.yml) re-derives
  these exact roots on a hosted runner — a different machine, a fresh userland,
  every push — from a clean checkout with no cache. That covers the *match*
  reproducing elsewhere. It does not cover the *package swarm* reproducing
  elsewhere, which is what the two-node script is for and what still runs on one
  host.
- **Match definitions are carried out of band.** The acceptance script writes
  the seed, plane count and package roots to a file both nodes read. There is
  no signed on-network challenge/accept wire for a match yet; the transport
  under test there is the package swarm, not the match definition.
- **A fresh peer can only learn four package roots.** The announce quota
  refuses the fifth-plus announce to a new peer within its window, so a store
  serving more than four complete packages cannot introduce the excess to a
  fresh node. The acceptance script prunes its scratch copy to exactly the four
  arena packages to work inside that limit; the limit itself is a real product
  gap.
- **Release-envelope import is DHT-gated.** The raw swarm delivers exact
  package content, but persisting the signed release envelope needs the
  authenticated DHT provider route, so the acceptance script hands those
  envelopes over the same out-of-band channel.
- **The artwork is a contact sheet, not an animation.** Six deterministic
  overhead snapshots, chosen by fixed rules over the re-simulation. There is no
  renderer, no browser and no 3D view in this repository, by design.

## Where this sits in the project

Z23 is first a public ZClassic full node. The Arena is an application of
the second thing it does: the decentralized C23 Commons, where ordinary nodes
publish, discover, fetch, verify, build, independently reproduce and serve
exact C23 packages without GitHub or a central registry. Package activity never
takes priority over consensus, relay, sync, peer health, wallet custody or
deployment — see [`AGENTS.md`](../AGENTS.md).

The Arena is the version of that story you can check in under a minute.
