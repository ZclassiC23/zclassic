# Security and Integrity Model

ZClassic23 is security-sensitive infrastructure: a cryptocurrency full node,
wallet, P2P server, onion-hosted explorer, and local MCP operator surface.
Those pieces can look suspicious to generic scanners because they include
network listeners, Tor, wallet/key code, fuzzers, crash harnesses, and agent
tooling. This document states the actual boundary and the checks that make it
auditable.

## Current status

- ZClassic23 is pre-v1 and in active stabilization. It is not production-ready
  and has no supported release line yet.
- The v1 bar is [`MVP.md`](./MVP.md). The project does not claim v1 until all
  eight operator acceptance criteria pass at the documented bar.
- Known gaps are documented instead of hidden. Examples include the current
  live forward-progress blocker, off-chain ZMSG plaintext transport, and
  incomplete marketplace/swap settlement paths.
- The June 2026 third-party security-audit disposition is
  [`work/security-audit-response-2026-06-09.md`](./work/security-audit-response-2026-06-09.md):
  it records fixed findings, refuted findings with citations, deferred items,
  and deployment gates.

## Authorized scope

This repository is for authorized operation and development of a ZClassic node
and its local operator surfaces:

- validating and serving the ZClassic P2P protocol;
- running an operator-owned wallet, block explorer, onion service, and RPC/MCP
  interface;
- testing this implementation with local unit tests, fuzzers, simulators,
  isolated regtest nodes, and consenting peers.

This repository is not offensive-security tooling. Project code and workflows
must not be used or extended for:

- unauthorized access to systems or accounts;
- credential harvesting, secret exfiltration, or covert collection;
- malware persistence, evasion, exploit delivery, botnet control, or DDoS;
- scanning, attacking, or stressing third-party services without explicit
  authorization;
- mining or running network services on systems without the owner's consent.

If a test needs a live process, peer, datadir, network, or wallet, the operator
must control it or have explicit permission to use it.

## Why high-signal components exist

| Component | Legitimate purpose | Safety boundary |
|-----------|--------------------|-----------------|
| Embedded Tor | Publish the operator's own explorer/API as a hidden service | `-tor` is explicit; test harnesses disable Tor |
| P2P networking and peer scoring | Implement the public ZClassic node protocol | Peer policy is consensus/network defense, not attack tooling |
| Wallet and key code | Local transparent/Sapling wallet operation | Diagnostics must not return private key material |
| MCP tools | Local typed operator API for AI-assisted node operation | Destructive tools are explicit and middleware-gated |
| `zcl_sql` | Incident-response inspection of local `node.db` | SELECT-only, semicolon-rejected, limited, and rate-gated |
| Fuzzers, chaos, kill-9 harnesses | Find crashes and recovery bugs in this codebase | Isolated datadirs and ports; no live-node mutation |
| Atomic swap and market code | Application protocol scaffolding | Settlement gaps are documented; scaffolding is not claimed complete |

## Concrete safeguards

- **Defensive-coding gates:** `make lint` checks raw SQLite writes, raw
  allocation use, silent error paths, MCP error bodies, supervisor liveness,
  app-shape boundaries, one-write-path rules, and no-silent-ready rules. The
  detailed contract is [`DEFENSIVE_CODING.md`](./DEFENSIVE_CODING.md).
- **Local integration gate:** `make ci` runs lint before tests, then the test
  harness, benchmark regression, hermetic MVP slice gates, crash tests, and
  fuzz smoke tests where the toolchain is available. This checkout does not
  contain a GitHub Actions workflow file; do not treat hosted push/PR automation
  as present unless a workflow is added.
- **MCP middleware:** tool routes carry destructive flags, destructive tools
  are rate-limited separately, tool calls emit events, and `ZCL_MCP_BEARER_TOKEN`
  can require bearer auth where a transport supports it. The default `-mcp`
  mode is a local stdio operator interface.
- **Operator-private HTTP routes:** wallet, message, and swap API routes are
  blocked on the clearnet listener as recorded in the June 2026 audit response.
- **Data-integrity discipline:** application writes go through the ActiveRecord
  lifecycle or explicit storage-layer APIs; chain progress is represented as
  durable stage cursors; recovery and self-heal paths must be observable.
- **Live-data discipline:** consensus-adjacent fixes are proven on a datadir
  copy before deployment. The isolated node harness refuses live datadirs and
  live ports and runs on throwaway `/tmp/zcl23-*` state.
- **Release integrity:** `tools/release.sh` builds with deterministic release
  flags, writes `BUILDINFO`, emits a SHA3-256 attestation, and requires GPG
  signing unless the operator explicitly passes `--unsigned`.
- **Dependency provenance:** vendored and ported third-party code is tracked in
  [`ATTRIBUTIONS.md`](./ATTRIBUTIONS.md), [`../NOTICE`](../NOTICE), and the
  repository tree. Packaging of several static libraries is still a known
  pre-v1 build gap and is documented in the README.

## Reviewer checklist

High-signal local checks:

```bash
git status --short --branch
make lint
make ci
```

Evidence files worth reading first:

- [`../README.md`](../README.md) - public status and feature scope.
- [`../.github/SECURITY.md`](../.github/SECURITY.md) - vulnerability reporting.
- [`MVP.md`](./MVP.md) - v1 acceptance criteria and readiness score.
- [`work/security-audit-response-2026-06-09.md`](./work/security-audit-response-2026-06-09.md) - audit disposition.
- [`RUNBOOK.md`](./RUNBOOK.md) - operational safety rails.
- [`../tools/mcp/middleware.c`](../tools/mcp/middleware.c) - MCP destructive-tool policy.
- [`../tools/scripts/isolated_node_env.sh`](../tools/scripts/isolated_node_env.sh) - isolated process/datadir guardrails.
- [`../tools/release.sh`](../tools/release.sh) - reproducible release and signing logic.

## Reporting

Report vulnerabilities privately through GitHub security advisories as described
in [`../.github/SECURITY.md`](../.github/SECURITY.md). Do not bury a valid
finding in reassuring language. If a claim is not currently proven, document the
gap and the proof needed to close it.
