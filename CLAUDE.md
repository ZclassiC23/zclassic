# ZClassic23 — AI-Integrated Personal Sovereignty Stack

## Vision — Personal Sovereignty Stack

ZClassic23 is one ~15 MB self-contained C23 binary that runs a full ZClassic node (Equihash 200,9 PoW, Sapling shielded txs), an embedded Tor onion service, a block explorer, a shielded wallet, a P2P file marketplace, ZNAM name registry, P2P messaging (plaintext in-memory and P2P channels, on-chain via Sapling encrypted memo field), cross-chain atomic swaps (BTC/LTC/DOGE), a P2P game framework, and an MCP server. **Claude is a first-class operator via ~100 typed MCP tools** — not just an observer. Cold sync to tip in ~60 seconds via FlyClient + SHA3 UTXO snapshots (design target — see `docs/HANDOFF.md`; today's proven recovery is the two-step header-import + boot, ~25 min). Silent halts are unreachable by construction — a stall is always a named blocker or a growing tip gap, never a quiet stop (chain progress is a stage cursor on disk); the node can still halt, it just cannot do so without saying so. Bugs become 64-bit seeds in a deterministic simulator. Build flags for reproducibility are in place (pinned `-march`, `SOURCE_DATE_EPOCH`, dropped build-id, deterministic tarball) with optional GPG signing (sign optional, can be waived with --unsigned) — byte-identity is not yet proven by a build-twice-and-compare gate. **One binary, one onion, one stack — your sovereign personal computing surface.**

See [`docs/HOW_THE_NODE_WORKS.md`](./docs/HOW_THE_NODE_WORKS.md) for the plain-language mental model (the node as a state machine), [`docs/FRAMEWORK.md`](./docs/FRAMEWORK.md) for the canonical architecture (the Prime Directive, the Ten Laws of Beauty, and the eight shapes), [`docs/ARCHITECTURE_DIAGRAMS.md`](./docs/ARCHITECTURE_DIAGRAMS.md) for current subsystem/boot topology, and [`docs/adr/0001-personal-sovereignty-stack.md`](./docs/adr/0001-personal-sovereignty-stack.md) for the 2026-05-22 pivot rationale.

## Security model for AI agents

ZClassic23 is operator-owned full-node infrastructure. Tor, wallet/key
handling, P2P networking, MCP operator tools, fuzzers, and crash harnesses
exist to run, observe, and harden nodes the operator controls. Keep
development, tests, and diagnostics scoped to local fixtures, isolated
datadirs, and consenting peers.

The project safety boundary, security model, and integrity checks are in
[`docs/SECURITY_AND_INTEGRITY.md`](./docs/SECURITY_AND_INTEGRITY.md).

**Consensus parity is inviolable.** zclassic23 must stay bit-for-bit
consensus-compatible with `zclassicd` — see
[`docs/CONSENSUS_PARITY_DOCTRINE.md`](./docs/CONSENSUS_PARITY_DOCTRINE.md).
A consensus change (Equihash params, activation heights, block/tx validity)
never ships to zclassic23 first — even if framed as opt-in / miner-signaled /
"sidegrade". Enforced by lint gate `check-consensus-parity` (E13, the
mechanism) + the `test_consensus_parity` test group (the golden values).
This is also the bar for reviewing outside PRs (thank + attribute + decline
consensus-breakers, mine the idea, build it better ourselves).

## Tenacity & recovery (operator invariants)

Full model: [`docs/TENACITY.md`](./docs/TENACITY.md) + the live-diagnosis
fast path [`docs/work/fast-path.md`](./docs/work/fast-path.md). Current bootstrap
posture + the sovereign-cure path: [`docs/HANDOFF.md`](./docs/HANDOFF.md).

**Current bootstrap = the consolidated daily-driver loader** — a snapshot whose
anchor hash is consensus-bound to the in-binary PoW header re-seeds `coins_kv`
and raises the reducer trusted base to the seed height, so the node reaches tip
(deployed via `-load-snapshot-at-own-height` + commit `ab512d577`, verified live
2026-06-23; a borrowed-but-consensus-bound stopgap — the sovereign
`-refold-from-anchor` cure that deletes the borrow is in flight, see HANDOFF §3).

**The legacy TWO-step recipe still works** (verified 2026-06-11: hash-identical
tip vs zclassicd at multiple heights, ~25 min total, warm-reboot-proven; this is
the legacy `zclassicd`-datadir bootstrap, not the cure). zclassicd stays RUNNING:

```bash
# 1. Headers FIRST — ~3.1M headers in ~60-74s from the legacy zclassicd datadir
build/bin/zclassic23 --importblockindex $HOME/.zclassic
# 2. Then a NORMAL boot — it auto-reads/links $HOME/.zclassic (legacy import
#    is on by default; opt out with -nolegacyimport)
build/bin/zclassic23
```

(The old `-cold-import=` flag no longer exists — the argv loop ignores
unknown flags, so passing it silently no-ops.) Skipping step 1 is a
footgun: importing UTXOs without the header import leaves a ~3.1M-header
hole (headers=960) and the node pins.

**Consensus rule: validate against the CHAIN, not the reference text.**
zclassicd source is a lossy proxy — the real chain contains a 125,811-byte
tx at h=478544 that the text-copied 102000 cap false-rejects (zclassicd
cannot resync its own chain). Any parity tightening of a bounded predicate
requires a full-history replay against the real chain first.

**Recovery paths get copy-proven on a fixture before live.** Never live
surgery: copy the datadir, repro there, prove the fix FIRES on the copy, then
deploy. Gate on **H\* CLIMB**, not "booted without FATAL." `test_parallel` green
is a regression floor, not a liveness proof.

## Current focus — **Ship v1 (MVP 8/8)**

> **Check the live node before treating MVP/soak as the active mission.** The
> forward-sync wedge class is **FIXED** (commit `ab512d577`, verified live
> 2026-06-23) — the consolidated daily-driver loader reaches the network tip via a
> borrowed-but-consensus-bound stopgap, so soak time can now accrue. The remaining gate is the **sovereign cure**
> (`-refold-from-anchor` cutover that deletes the borrowed seed) **+ accumulated
> soak hours**, not un-wedging. Verify live state with `zcl_status` /
> [`docs/HANDOFF.md`](./docs/HANDOFF.md) — never assume "synced" from a doc.
> Wedge-class cure design: [`docs/work/never-stuck-plan.md`](./docs/work/never-stuck-plan.md).

**The v1 bar is [`docs/MVP.md`](./docs/MVP.md)** — 8 operator acceptance criteria; v1 = MRS 8/8.
**THE plan is [`docs/work/FORWARD_PLAN.md`](./docs/work/FORWARD_PLAN.md)** — MVP-anchored, covering the autonomous / owner-gated / operational critical path. Current live state is in [`docs/HANDOFF.md`](./docs/HANDOFF.md). **#1 priority: CI-enforce the MVP criteria and accumulate soak time.**

**The framework/architecture refactor is ~90% done and OFF the v1 path — do not jump the queue.** [`docs/FRAMEWORK.md`](./docs/FRAMEWORK.md) is the canonical architecture (the Prime Directive, Ten Laws, eight shapes); [`docs/REFACTOR_STATUS.md`](./docs/REFACTOR_STATUS.md) is the architecture debt board. Both are reference, not the mission. Every `.c` under `app/` still lives in exactly one of eight shape folders, lint-enforced.

**Parallel-worktree workflow:** main repo is the orchestrator; `~/github/zclassic23-2` (wt2) and `~/github/zclassic23-3` (wt3) are workers. See [`docs/work/README.md`](./docs/work/README.md) and [`docs/work/agent-protocol.md`](./docs/work/agent-protocol.md). Worker identity = pwd suffix.

### On a fresh session

Type **`continue zclassic23 development`**. The agent will:
1. Run `pwd` to detect worktree ID (`main`, `wt2`, `wt3`, ...).
2. For the one-page mental model, skim **[`docs/HOW_THE_NODE_WORKS.md`](./docs/HOW_THE_NODE_WORKS.md)** (append-only `progress.kv` log → eight reducer stages, each advance-cursor-or-name-a-blocker → projections → health = `network_tip − log_head`). **[`docs/CODEBASE_MAP.md`](./docs/CODEBASE_MAP.md)** is where-things-live + how-to-do-each-thing; **[`docs/AGENT_TRAPS.md`](./docs/AGENT_TRAPS.md)** lists things that look broken but are intentional or already-done — read it before "fixing" or re-proposing anything.
3. `cat docs/HANDOFF.md` FIRST (the current entry point / live state), then `docs/MVP.md` (the v1 contract) and `docs/work/FORWARD_PLAN.md` (THE plan; the sovereign-cure spine is `docs/work/self-verified-tip-plan.md`). `docs/FRAMEWORK.md` + `docs/REFACTOR_STATUS.md` are architecture reference, off the v1 path.
4. Check the live node before trusting any doc: `zcl_status`, then `zcl_state subsystem=reducer_frontier`. A doc can be stale; the node cannot.
5. If worker → read `docs/work/wt<N>-*.md` and follow `docs/work/agent-protocol.md`. If orchestrator → review in-flight work in the status board, merge pushed branches, dispatch next assignments.

## Defensive Coding Standards (MANDATORY)

**Read [`DEFENSIVE_CODING.md`](./docs/DEFENSIVE_CODING.md) before writing any code.**

For modules prefixed `legacy_` (cold-start bootstrap, drift detection
against an external `zclassicd`), see [`LEGACY_LIFECYCLE.md`](./docs/LEGACY_LIFECYCLE.md)
for which paths are active vs deprecated.

For the boot ordering invariants (`enum boot_stage` + the stage
advance state machine in `lib/util/src/boot_phase.c`), see
[`BOOT_INVARIANTS.md`](./docs/BOOT_INVARIANTS.md) — explains what each
stage guarantees and how to wire a new boundary.

Key rules enforced by the compiler and CI:
- **Every write goes through the AR lifecycle** — `AR_BEGIN_SAVE` + `AR_FINISH_SAVE`, or the combined `AR_ADHOC_SAVE` (locally-prepared stmt) / `AR_CACHED_SAVE` (cached stmt). No raw `sqlite3_step()` in app code. See `app/models/include/models/activerecord.h`.
- **Every error return must log context** — use `LOG_FAIL()`, `LOG_ERR()`, `LOG_NULL()` from `util/log_macros.h`
- **Every malloc must be checked** — use `zcl_malloc(size, "label")` from `util/safe_alloc.h`
- **Every MCP handler must set an error body** — never `return -1;` without explaining why
- **Before/after save hooks** — wire them for wallet keys, UTXOs, blocks

`make lint` checks for violations. `make ci` runs lint before tests.

## MCP Server (Model Context Protocol)

ZClassic23 has a built-in MCP server for AI development. Claude Code can query the node directly via typed tools — no curl, no token waste.

### Setup

```bash
claude mcp add zcl23 -- build/bin/zclassic23 -mcp
```

Or with custom datadir/port:
```bash
claude mcp add zcl23 -- build/bin/zclassic23 -mcp -datadir=/path/to/data -rpcport=18232
```

Restart Claude Code after adding. The tools appear automatically.

### Quick Reference

There are ~100 typed tools (call `zcl_tools_list` for the exact live count). **This table lists only the ones you reach for
daily** — it is deliberately not exhaustive. For the full catalog, call
`zcl_tools_list` (live routing table) or read the source of truth:
`tools/mcp/controllers/{app,chain,meta,net,ops,wallet}_controller.c`.

| Tool | When to use |
|------|-------------|
| `zcl_status` | **Start here.** Height, peers, sync, onion, health — one call |
| `zcl_kpi` | KPI dashboard: height, peer_count, sync, validation |
| `zcl_syncstate` / `zcl_validationstatus` | Sync phase + background-validation progress |
| `zcl_getblock` / `zcl_getblockcount` | Block by height/hash; current height |
| `zcl_peers` | Connected peers (addresses, latency, heights) |
| `zcl_dataintegrity` / `zcl_utxocommitment` | SHA3 over consensus tables / UTXO set |
| `zcl_balance` / `zcl_listunspent` | Wallet balance; spendable UTXOs |
| **Primitive** `zcl_state` | Generic state dump: `subsystem=supervisor,watchdog,boot,block_index,…` — prefer this over a bespoke tool |
| **Primitive** `zcl_node_log` | Server-side regex tail of node.log (level filter) |
| **Primitive** `zcl_sql` | SELECT-only SQL over node.db (rate-gated) |
| `zcl_tools_list` / `zcl_self_test` / `zcl_openapi` | Enumerate / smoke-test / schema-dump every tool |
| **Escape hatch** `zcl_rpc` | Call any of 85+ RPC methods directly when no typed tool fits |

Everything else (wallet shielded ops, ZNAM/ZMSG/Market/ZSWP, mining, peer
games, metrics, replay buffer, admin) is a typed tool too — discover it via
`zcl_tools_list` rather than memorizing it here. The three **primitives** plus
the `zcl_rpc` escape hatch answer most one-off questions without a new tool.

### Example: Raw RPC

For commands without a dedicated tool, use `zcl_rpc`:
- `zcl_rpc(method="getmempoolinfo")`
- `zcl_rpc(method="z_listaddresses")`
- `zcl_rpc(method="getblock", params="[\"hash\", 2]")`

### Adding state introspection

The MCP has three **primitives** (`zcl_state`, `zcl_node_log`, `zcl_sql`)
that cover most diagnostic questions without needing a new bespoke
tool per question. When adding a new subsystem that has interesting
runtime state, follow the convention:

1. Add an entry to the subsystem's public header:
   ```c
   /* See CLAUDE.md "Adding state introspection". Reentrant-safe. */
   struct json_value;
   bool <name>_dump_state_json(struct json_value *out, const char *key);
   ```
   `out` is initialized by the caller (`json_set_object(out)` first
   thing). `key` is subsystem-specific or NULL.

2. Implement it in the subsystem's .c file. Use `atomic_load` for any
   fields touched by background threads; brief mutex acquires are OK
   for snapshot consistency. Don't allocate (the caller's JSON value
   owns the buffer).

3. Register the dump function in the dispatcher table at
   `app/controllers/src/diagnostics_registry.c:g_dumpers`. One line.

4. No edit to `diagnostics_controller.c` is needed — `p_state[0].enum_csv`
   is auto-populated at runtime from the diagnostics registry via
   `diagnostics_subsystems_csv()` / `g_state_subsystems_csv`
   (`diagnostics_controller.c:574`). Only update the `enum_csv` assertion
   in `lib/test/src/test_mcp_controllers.c` if it asserts the list.

That's it — no new RPC handler, no new MCP route, no new schema.
Every future subsystem becomes introspectable via `zcl_state` with
~30 lines of changes total. Currently wired: `supervisor`, `watchdog`,
`boot`, `block_index`, plus the existing services that follow the
same convention (`health`, `chain_evidence`, `chain_advance_coordinator`,
`legacy_mirror`, `oracle`, `header_probe`, etc).

The `supervisor` subsystem (Round 5) is the *root* of the liveness
tree: it lists every registered child (`sync.watchdog`,
`net.outbound_floor`, `chain.coord_escalation` after Round 5), along
with each child's last_tick_age_us, progress_marker, deadline,
ticks_run, and stall_fires. Use it to confirm any time-driven thing
is actually running. See `lib/util/include/util/supervisor.h` for the
contract API and `DEFENSIVE_CODING.md` Gate #15 for the lint gate that
ratchets adoption.

For raw SQL inspection of node tables (blocks, utxos, mempool, etc),
use `zcl_sql`: SELECT-only, semicolon-rejected, auto-LIMIT, 2 s
wall-clock budget, 100-row hard cap. Marked destructive in the MCP
middleware (rate-gated at 1 RPS) because arbitrary scans can be
expensive.

For tailing node.log without downloading the whole file, use
`zcl_node_log(pattern, since_secs, max_lines, level)` — server-side
reverse scan in 64 KB chunks.

---

## Node Architecture

A single ~15 MB C23 binary (Equihash 200,9 PoW, Sapling zk-SNARKs). The full
subsystem list is in the Vision section above; the rest of this section covers
how to run and observe it.

### Running

```bash
# Main node (linger service)
systemctl --user start zclassic23

# Flags
-datadir=DIR          Data directory (default: ~/.zclassic-c23)
-port=N               P2P port (default: 8033)
-rpcport=N            RPC port (default: 18232)
-tor                  Enable embedded Tor onion service
-nobgvalidation       Skip background proof verification (saves RAM)
-mcp                  Run as MCP server on stdio (for Claude Code)
-txindex              Enable full transaction index
-addnode=IP:PORT      Connect to specific peer
```

---

## Key Features

### Onion Hidden Service Hosting

**Opt-in build:** the default binary links a Tor *stub* (`vendor/tor_stub.c`), so
`-tor` runs the node without an onion and logs that Tor is disabled. The real
onion requires building the `vendor/tor` submodule (`git submodule update --init
vendor/tor`, then build per `docs/BUILD.md`); the Makefile auto-links
`vendor/tor/libtor.a` when present. The owner's live node runs the real build.

When `-tor` is enabled (with the real Tor built), zclassic23 embeds a modified Tor (RhettCreighton/tor fork with dynhost). The node:

1. Bootstraps Tor as a pthread inside the process
2. Generates an ephemeral .onion address (with optional vanity prefix)
3. Serves the full REST API + block explorer over .onion
4. Handles requests via direct C function calls — no SOCKS, no ports, no HTTP parsing overhead

The .onion address is visible via `zcl_status` → `health.checks.onion_address`.

Architecture: `Client → Tor network → onion_service.c → onion_service_handle_request() → same controllers as HTTPS`

### Peer Discovery via Onion Directory

Each node with Tor enabled serves `/directory.json` on its .onion address, containing:
- Node's .onion address
- Clearnet IP and port (for fast direct connections)
- Block height, version

A fresh node can:
1. Bootstrap Tor (~10 seconds)
2. Fetch `/directory.json` from hardcoded .onion seeds
3. Extract clearnet IPs from the response
4. Connect directly for fast P2P sync

This enables fully decentralized peer discovery even when DNS seeds are unavailable.

### Fast Sync (FlyClient + MMB + SHA3)

A fresh node is *designed* to sync 3M+ blocks in ~60 seconds (design target — the stack below is built but not yet the proven cold-start; today's proven cold-sync is the two-step `--importblockindex` + boot, ~25 min, see the Tenacity section and `docs/HANDOFF.md`):

1. **FlyClient** — 50 random block samples with MMB (Merkle Mountain Belt) inclusion proofs. Each sample verified for PoW. Security: ≥150 bits against any minority adversary.

2. **SHA3 UTXO Snapshot** — Complete UTXO set transferred and verified against SHA3-256 commitment. 1.3M UTXOs in canonical order.

3. **Delta sync** — Headers + blocks from snapshot height to tip via standard P2P.

The node is designed to be fully operational after step 2 (~60s target). Background validation optionally re-verifies every historical signature and proof.

Key MCP tools: `zcl_mmb`, `zcl_utxocommitment`, `zcl_syncstate`, `zcl_validationstatus`

### P2P Game Service

Built-in P2P game framework for latency measurement and gameplay:

**Ping (Type 0)** — Measures round-trip latency in microseconds. Used by:
- `zcl_pingpeer(peer_id=N)` — Ping specific peer
- `zcl_peerlatency` — All peer latencies

**TicTacToe (Type 1)** — Extensible game framework demonstrating P2P messaging:
- Binary wire protocol over `zgame` P2P message
- Move validation, state sync, win detection
- Actions: INVITE, ACCEPT, MOVE, STATE, RESIGN, RESULT

Wire format: `[1 game_type] [1 action] [variable data]`

### ZCL Names (ZNAM) — On-Chain Name Registry

Human-readable names registered on-chain via OP_RETURN. Inspired by ENS (Ethereum Name Service).

- **OP_RETURN protocol** with "ZNAM" lokad ID, same pattern as ZSLP tokens
- Names: 1-63 chars, lowercase alphanumeric + hyphens, first-come-first-served
- **Multi-coin resolution**: a single name can have addresses for ZCL, BTC, LTC, DOGE
- **Text records**: arbitrary key-value metadata (email, url, avatar) — ENS TextResolver pattern
- **Content hash** *(planned)*: link names to file market content — no `SET_CONTENT` opcode exists yet (commands are REGISTER/UPDATE/TRANSFER/RENEW/SET_RECORD/SET_TEXT)
- Commands: REGISTER, UPDATE, TRANSFER, RENEW, SET_RECORD, SET_TEXT
- RPC: `name_register`, `name_resolve`, `name_list`

### ZCL Messaging (ZMSG) — P2P + On-Chain Messages

Two-mode messaging: off-chain (instant, free) and on-chain (permanent, shielded).

- **Off-chain**: P2P messages (`zmsg`/`zmsgack`) between connected nodes — **plaintext on the wire** (transport encryption not yet implemented)
- **On-chain**: structured data in the Sapling 512-byte encrypted memo field (shielded) — **not yet implemented**: no send path sets the on-chain channel (`messaging_controller.c` hardcodes P2P)
- Messages stored in SQLite, delivery acknowledgment
- RPC: `msg_send`, `msg_inbox`, `msg_read`

### ZCL Market — Crypto-Incentivized File Sharing

File marketplace: offer gossip with price metadata, proof-of-possession challenges, and a working file service that streams chunk bytes (`file_service.c` → `fs_send_chunk_fast`) with chunk unlock gated on a mempool-verified payment txid (`handle_zfilepay`, `msgprocessor.c`). On-chain payment settlement and the buy/offer RPC-to-transfer glue are not yet wired end-to-end.

- P2P gossip of file offers with price per MB
- Chunk challenges for sybil resistance (prove you have the data)
- RPC: `zmarket_list`, `zmarket_offer`, `zmarket_buy`, `zmarket_status`

### Atomic Swaps (ZSWP) — Cross-Chain HTLC Trading

HTLC contract scaffolding: swap initiation and participation with redeem script generation. Redemption, refund, and settlement not yet implemented.

- **Chains**: ZCL, BTC, LTC, DOGE (same 97-byte contract as dcrdex)
- Script: OP_IF/OP_SHA256/OP_CLTV with shared OP_CHECKSIG
- Secret extraction / redeem + refund scriptSig builders exist as library primitives (`script/htlc.*`, tested), not yet wired to a node-broadcast/settlement path
- RPC: `swap_chains`, `swap_initiate`, `swap_participate`, `swap_list`
- Reference: dcrdex HTLC script format (Blue Oak License 1.0.0), reimplemented in `script/htlc.*`

### Background Validation

Optional (`-nobgvalidation` to disable). Walks every block from genesis verifying:
- Equihash PoW solutions
- ECDSA script signatures (every input)
- Ed25519 JoinSplit signatures
- Sapling Groth16 spend/output proofs
- Sprout Groth16/PHGR13 proofs
- Merkle root integrity

RAM-aware: auto-detects system memory, caps script batch size on <8GB machines.

Progress via: `zcl_validationstatus`

---

## Development

### Build
```bash
make -j$(nproc)     # Builds build/bin/zclassic23, build/bin/test_zcl, build/bin/zclassic-cli
make test           # Run 1500+ tests
make deploy         # Build + setcap + restart service
```

Note on `-j`: each binary is ONE whole-program `cc` over ~660–1400 `.c` files
(LTO), so `-j$(nproc)` only overlaps the 2–3 separate binaries + the LTO link,
NOT the per-binary front-end compile. For a fast compile-check inner loop use
`make build-only` (664 `cc -c` genuinely parallel under `-j`); header edits are
now tracked via depfiles, so it no longer false-greens on a header change.
Default build targets `-march=x86-64-v3` (portable: AVX2/FMA/BMI2); pass
`ZCL_NATIVE=1` to build for the host CPU only.

### Test
```bash
build/bin/test_zcl  # All tests
```

### RPC (without MCP)
```bash
build/bin/zcl-rpc getblockcount
build/bin/zcl-rpc getpeerinfo
build/bin/zcl-rpc z_gettotalbalance
```

### Services
```bash
systemctl --user status zclassic23        # Main node
systemctl --user status zclassicd         # C++ legacy reference/oracle
systemctl --user status zclassic23-test   # Test instance
```
