# Getting Started With ZClassic23

This is the public first-run path for a fresh machine. Maintainer-only live
state lives in `docs/HANDOFF.md`; use that file only when you are operating the
project's own host.

## What You Get

`zclassic23` builds one C23 binary plus small CLI clients:

- A ZClassic full node on the zclassicd consensus floor.
- RPC and native CLI status commands for local operation.
- A built-in native command registry for typed local AI/operator operations.
- Optional Tor onion service support when the bundled Tor fork is built.

The project is still pre-v1. Do not rely on it as your only mainnet node yet.

## Prerequisites

Install:

- gcc 14+ or clang with working `-std=c23` support.
- GNU make.
- `c++` or `g++`, `autoconf`, `curl` or `wget`, `unzip`, and `sha256sum` for
  the one-time vendored-library build. `cmake` is optional; when it is absent,
  LevelDB is built by the repo's direct static-library fallback.

The first build needs internet access. Third-party source tarballs are fetched
from pinned URLs, checked against pinned SHA-256 hashes, and built locally into
`vendor/lib/`. Later builds reuse the local cache.

## Build

```bash
git clone https://github.com/ZclassiC23/zclassic.git
cd zclassic
make vendor
make -j"$(nproc)"
make test
make lint
```

Useful faster loops:

```bash
make zclassic23              # node only
make fast-changed-compile    # cheapest guarded changed .c/.h/.def check
make fast-compile            # fastest dev compile check, no final link
make build-only              # strict release-flag compile check, no final link
make fast-rebuild            # changed-file dev compile + non-LTO local node link
make hot-rebuild             # alias for fast-rebuild during edit loops
make dev-bin                 # fast non-LTO local node binary
make agent-doctor            # combined build/dev-lane/test-failure next action
make agent-dev-status        # no-build dev-lane status + next safe action
make agent-clear-stale-dev-reindex # clear proven-stale dev auto-reindex marker
build/bin/zclassic23-dev agentdevstatus   # native typed dev-lane status
make agent-stage-dev         # contained: refuses; runtime staging/publication is disabled
make t-fast ONLY=<group>     # one test group
make agent-loop              # default AI/operator verification loop; no runtime deploy
make fast-ci                 # cache-aware lint/build/focused-test loop
build/bin/zclassic23 status               # native node status
build/bin/zclassic23-dev status           # dev-lane native status
```

Runtime generation publication is Phase-0 contained. Native apply commands,
auto/apply watcher modes, `make hotswap`, `deploy-dev*`,
`agent-deploy-fast`, direct activation scripts, and generation-relinking revert
all refuse. Use verify/check watch, builds, tests, simulations,
`make hotswap-so` plus build/test verification while the unified immutable
source/proof/CAS/rollback transaction is completed.

Build details, dependency versions, and reproducible-release notes are in
`docs/BUILD.md`.

## First Run

Start a node with an explicit datadir:

```bash
build/bin/zclassic23 -datadir="$HOME/.zclassic-c23"
```

In another terminal, ask the running node for status:

```bash
build/bin/zclassic23 -datadir="$HOME/.zclassic-c23" agent
build/bin/zclassic23 -datadir="$HOME/.zclassic-c23" status
build/bin/zclassic23 -datadir="$HOME/.zclassic-c23" agentops
build/bin/zclassic-cli -datadir="$HOME/.zclassic-c23" getblockcount
build/bin/zclassic-cli -datadir="$HOME/.zclassic-c23" getnetworkinfo
```

Healthy synced status means:

- `status=healthy`
- `serving=true`
- `security_posture.review_required=false` when you need a fully reviewed
  bootstrap/nullifier-history posture, not just a live serving node
- `operator_needed=false`
- `sync_state=at_tip`
- `gap` is small, normally 0 or 1 near the moving network tip.
- `peers` is greater than 0.

A fresh empty datadir starts at height 0 and syncs honestly from peers. That
plain P2P path is slow today. To reach tip quickly, use one of the bootstrap
paths below.

## Bootstrap Choices

1. Starter pack: download the published block index plus SHA3-verified UTXO
   snapshot, place both in a fresh datadir, and boot with the snapshot loader.
   Use `docs/BOOTSTRAPPING.md` for exact asset names, hashes, and commands.
2. Plain P2P: start from an empty datadir and sync from genesis. This is the
   most conservative path but takes much longer.
3. Local zclassicd import: if you already run the legacy C++ node, import
   headers first, then boot normally:

   ```bash
   build/bin/zclassic23 --importblockindex "$HOME/.zclassic"
   build/bin/zclassic23 -datadir="$HOME/.zclassic-c23"
   ```

Always judge success by height climbing toward the network tip, not just by the
process staying up.

## Optional Tor Onion

The default build links a Tor stub, so `-tor` runs the node without publishing an
onion. For the real in-process onion service:

```bash
git submodule update --init vendor/tor
```

Then build Tor as described in `docs/BUILD.md`. When `vendor/tor/libtor.a`
exists, the Makefile links it automatically and `-tor` can publish the onion.

## Agent / Operator Interface

The native command registry is the primary way an AI agent or operator
inspects and drives the node — no separate server process, no client setup:

```bash
build/bin/zclassic23 -datadir="$HOME/.zclassic-c23" status
build/bin/zclassic23 -datadir="$HOME/.zclassic-c23" discover help
build/bin/zclassic23 -datadir="$HOME/.zclassic-c23" dumpstate <subsystem>
```

Start with `status`, then `discover help` / `discover search <q>` to find the
rest of the tree. See `docs/AGENT_API.md` for the full surface.

Keep RPC bound to trusted clients. The native command registry is local to the
operator process and needs no separately configured agent server.

## Troubleshooting

No peers:

```bash
build/bin/zclassic23 -datadir="$HOME/.zclassic-c23" agent
build/bin/zclassic-cli -datadir="$HOME/.zclassic-c23" getpeerinfo
build/bin/zclassic-cli -datadir="$HOME/.zclassic-c23" addnode "IP:PORT" "onetry"
ss -tlnp | grep 8033
```

Stuck height:

```bash
build/bin/zclassic23 -datadir="$HOME/.zclassic-c23" agent
build/bin/zclassic23 -datadir="$HOME/.zclassic-c23" agentops
build/bin/zclassic-cli -datadir="$HOME/.zclassic-c23" syncstate
build/bin/zclassic-cli -datadir="$HOME/.zclassic-c23" healthcheck
```

Logs:

```bash
build/bin/zclassic23 -datadir="$HOME/.zclassic-c23" getnodelog "error|warn"
tail -n 200 "$HOME/.zclassic-c23/node.log"
```

Use the named blocker, gap, peers, and sync state from `agent` before changing
configuration. A stable node should fail loudly with a named reason when it
cannot advance.

## Contributing

Read `.github/CONTRIBUTING.md` and `docs/DEFENSIVE_CODING.md` before changing
code. Consensus parity with zclassicd is inviolable; see
`docs/CONSENSUS_PARITY_DOCTRINE.md`.
