# Getting Started With ZClassic23

This is the public first-run path for a fresh machine. Maintainer-only live
state lives in `docs/HANDOFF.md`; use that file only when you are operating the
project's own host.

## What You Get

`zclassic23` builds one C23 binary plus small CLI clients:

- A ZClassic full node on the zclassicd consensus floor.
- RPC and native CLI status commands for local operation.
- A built-in MCP server for typed local AI/operator tools.
- Optional Tor onion service support when the bundled Tor fork is built.

The project is still pre-v1. Do not rely on it as your only mainnet node yet.

## Prerequisites

Install:

- gcc 14+ or clang with working `-std=c23` support.
- GNU make.
- `cmake`, `autoconf`, `curl` or `wget`, `unzip`, and `sha256sum` for the
  one-time vendored-library build.

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
make build-only              # compile check, no final link
make t-fast ONLY=<group>     # one test group
make fast-ci                 # cache-aware lint/build/focused-test loop
```

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
build/bin/zclassic23 -datadir="$HOME/.zclassic-c23" agentops
build/bin/zclassic-cli -datadir="$HOME/.zclassic-c23" getblockcount
build/bin/zclassic-cli -datadir="$HOME/.zclassic-c23" getnetworkinfo
```

Healthy synced status means:

- `status=healthy`
- `serving=true`
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

## Optional MCP

Claude Code or another MCP client can operate the node through typed local
tools:

```bash
claude mcp add zcl23 -- build/bin/zclassic23 -mcp -datadir="$HOME/.zclassic-c23"
```

Start with:

- `zcl_status`
- `zcl_agent_ops`
- `zcl_state`
- `zcl_node_log`
- `zcl_tools_list`

MCP is a local operator interface. Do not expose RPC or MCP to untrusted
clients.

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
