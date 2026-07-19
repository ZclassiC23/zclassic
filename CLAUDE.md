# ZClassic23 — AI-Integrated Personal Sovereignty Stack

## Vision — Personal Sovereignty Stack

ZClassic23 is one self-contained C23 binary that runs a full ZClassic node (Equihash 200,9 PoW, Sapling shielded txs), an embedded Tor onion service, a block explorer, a shielded wallet, a P2P file marketplace, ZNAM name registry, P2P messaging (plaintext P2P channel; on-chain Sapling-memo channel implemented — requires Sapling params + a passing prover self-test to send), cross-chain atomic swaps (BTC/LTC/DOGE; redeem/refund/settlement: in progress), a P2P game framework, and a native command registry. **Claude is a first-class operator via 100+ typed native commands** — not just an observer. Cold sync to tip in ~60 seconds via FlyClient + SHA3 UTXO snapshots (design target — see `docs/HANDOFF.md`; today's proven recovery is the two-step header-import + boot, ~25 min). Silent halts are unreachable by construction — a stall is always a named blocker or a growing tip gap, never a quiet stop (chain progress is a stage cursor on disk); the node can still halt, it just cannot do so without saying so. Bugs become 64-bit seeds in a deterministic simulator. Deterministic build flags and a legacy GPG-capable packaging script exist, but byte identity is not yet proven by a two-builder gate; unsigned output is local-development-only and stable publication remains contained. **One binary, one onion, one stack — your sovereign personal computing surface.**

See [`docs/HOW_THE_NODE_WORKS.md`](./docs/HOW_THE_NODE_WORKS.md) for the plain-language mental model (the node as a state machine), [`docs/FRAMEWORK.md`](./docs/FRAMEWORK.md) for the canonical architecture (the Prime Directive, the Ten Laws of Beauty, and the eight shapes), [`docs/AGENT_ARCHITECTURE.md`](./docs/AGENT_ARCHITECTURE.md) for the concrete future-agent feature slice (REST resources, ActiveRecord, validations, relationships, schema, services, native surfaces), [`docs/ARCHITECTURE_DIAGRAMS.md`](./docs/ARCHITECTURE_DIAGRAMS.md) for current subsystem/boot topology, and [`docs/adr/0001-personal-sovereignty-stack.md`](./docs/adr/0001-personal-sovereignty-stack.md) for the 2026-05-22 pivot rationale.

**The developer operating manual is the `zclassic23-dev` skill** ([`.claude/skills/zclassic23-dev/SKILL.md`](./.claude/skills/zclassic23-dev/SKILL.md)) — navigator-first code lookup, the fast dev loop / hot-swap tiers, typed-commands-over-bash, push traps, and build/test/deploy live there, not in this file.

## Security model for AI agents

ZClassic23 is operator-owned full-node infrastructure. Tor, wallet/key
handling, P2P networking, native operator commands, fuzzers, and crash harnesses
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
Plain meaning: the **sovereign cure** is the self-verified UTXO rebuild that
starts from the in-binary SHA3/PoW checkpoint, folds real block bodies forward,
then deletes the borrowed `zclassicd`-minted seed path.

**Current canonical state is wedged, not synced.** The public daily-driver is
held below tip by incomplete historical shielded anchors and nullifiers; verify
the live H\* via `zclassic23 status` / `zclassic23 dumpstate reducer_frontier` before acting
(`docs/HANDOFF.md` holds current state). The consolidated loader did
previously seed transparent `coins_kv` from a `zclassicd`-minted snapshot and
reach tip, but matching that artifact's anchor hash to a validated local header
binds only its height/hash location. ZClassic headers do **not** commit the UTXO,
Sapling/Sprout frontier, or nullifier contents, so the borrowed state is not
PoW- or consensus-bound content. The cure must install complete independently
validated transparent and shielded state atomically and pass copy proof; the
current v1-oriented refold reset must not discard v3 shielded sections.

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
> canonical node is currently wedged below tip (live H\* via `zclassic23 status`) on the permanent
> `utxo_apply.anchor_backfill_gap` / nullifier-history dependency. A prior
> transparent borrowed seed reaching tip did not prove complete shielded state.
> No canonical soak time is clean evidence while this gap exists; cure and
> copy-prove the complete state first, then start a fresh exact-parity soak
> window. Current truth: [`docs/HANDOFF.md`](./docs/HANDOFF.md). Cure design:
> [`docs/work/self-verified-tip-plan.md`](./docs/work/self-verified-tip-plan.md).

**The active #1 track is the sovereign shielded-state cure and copy proof.**
The typed native CLI (`zclassic23 <command>`) is the only agent interface.
The source-code navigator lives under `lib/codeindex/` and is exposed through
the native `code` command branch.

**The v1 bar is [`docs/MVP.md`](./docs/MVP.md)** — 8 operator acceptance criteria; v1 = MRS 8/8.
**THE plan is [`docs/work/FORWARD_PLAN.md`](./docs/work/FORWARD_PLAN.md)** — MVP-anchored, covering the autonomous / owner-gated / operational critical path. Current live state is in [`docs/HANDOFF.md`](./docs/HANDOFF.md). **#1 priority: complete and copy-prove the sovereign shielded-state cure; only then start a fresh exact-candidate soak.**

**The framework/architecture refactor is ~90% done and OFF the v1 path — do not jump the queue.** [`docs/FRAMEWORK.md`](./docs/FRAMEWORK.md) is the canonical architecture (the Prime Directive, Ten Laws, eight shapes) and §9 is the architecture debt board. It is reference, not the mission. Every `.c` under `app/` still lives in exactly one of eight shape folders, lint-enforced.

**Parallel-worktree workflow:** main repo is the orchestrator; `~/github/zclassic23-2` (wt2) and `~/github/zclassic23-3` (wt3) are workers. See [`docs/work/README.md`](./docs/work/README.md) and [`docs/work/agent-protocol.md`](./docs/work/agent-protocol.md). Worker identity = pwd suffix.

### On a fresh session

Type **`continue zclassic23 development`**. The agent will:
1. Run `pwd` to detect worktree ID (`main`, `wt2`, `wt3`, ...).
2. For the one-page mental model, skim **[`docs/HOW_THE_NODE_WORKS.md`](./docs/HOW_THE_NODE_WORKS.md)** (append-only fact log in `consensus.db` → eight reducer stages, each advance-cursor-or-name-a-blocker → projections → health = `network_tip − log_head`). **[`docs/CODEBASE_MAP.md`](./docs/CODEBASE_MAP.md)** is where-things-live + how-to-do-each-thing; **[`docs/AGENT_ARCHITECTURE.md`](./docs/AGENT_ARCHITECTURE.md)** is the required feature-slice recipe for REST resources, ActiveRecord models, validations, relationships, database schema, services, and native command surfaces; **[`docs/AGENT_TRAPS.md`](./docs/AGENT_TRAPS.md)** lists things that look broken but are intentional or already-done — read it before "fixing" or re-proposing anything.
3. `cat docs/HANDOFF.md` FIRST (the current entry point / live state), then `docs/MVP.md` (the v1 contract) and `docs/work/FORWARD_PLAN.md` (THE plan; the sovereign-cure spine is `docs/work/self-verified-tip-plan.md`). `docs/FRAMEWORK.md` (§9 is the debt board) is architecture reference, off the v1 path.
4. Check the live node before trusting any doc: `zclassic23 status`, then
   `zclassic23 dumpstate reducer_frontier`. A doc can be stale; the node cannot.
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
- **Every native command handler must set an error body** — never `return -1;` without explaining why
- **Before/after save hooks** — wire them for wallet keys, UTXOs, blocks

`make lint` checks for violations. `make ci` runs lint before tests.

## Agent interface — native commands

The interface is the native command registry: `zclassic23 <command>` under
`core.*`/`app.*`/`ops.*`/`dev.*`/`discover.*`. Start with `zclassic23 status`;
enumerate with `discover help` / `discover search <q>`; three diagnostic
primitives (`ops state --subsystem=<name>`, `ops logs`, and
`core storage query` for SELECT-only SQL)
answer most one-off questions. Full doc:
[`docs/NATIVE_COMMAND_INTERFACE.md`](./docs/NATIVE_COMMAND_INTERFACE.md);
daily usage patterns are in the `zclassic23-dev` skill.

### Adding state introspection

The native registry has three **diagnostic primitives** (`ops state`,
`ops logs`, and `core storage query`) that cover most diagnostic questions
without needing a new bespoke command per question. When adding a new subsystem that has interesting
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

4. No edit to the state command handler is needed; its subsystem catalog is
   populated at runtime from the diagnostics registry. Update the native
   diagnostics registry test if it asserts the list.

That's it — no new RPC handler, command route, or schema.
Every future subsystem becomes introspectable via `zclassic23 ops state` with
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
use `zclassic23 core storage query --sql='SELECT ...'`: SELECT-only,
semicolon-rejected, auto-LIMIT, 2 s wall-clock budget, and a 100-row hard cap.
Arbitrary scans can still be expensive, so keep queries bounded.

For tailing node.log without downloading the whole file, use
`zclassic23 ops logs --pattern='<regex>'` with the optional time, line-count,
and level arguments — a server-side reverse scan in 64 KB chunks.

---

## Node Architecture

A single self-contained C23 binary (Equihash 200,9 PoW, Sapling zk-SNARKs). The full
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
-txindex              Enable full transaction index
-addnode=IP:PORT      Connect to specific peer
-pin-reducer          Pin the P2P block-ingest ("reducer") thread to the
                      large-L3 CCD when hw_profile detects an asymmetric
                      multi-CCD host (e.g. a 7950X3D); default OFF, advisory
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

The .onion address is visible via `zclassic23 core status` →
`health.checks.onion_address`.

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

1. **FlyClient** — sampled header/PoW evidence. The auxiliary MMB and any
   `utxo_root` it carries are not committed by ZClassic headers and cannot
   authenticate peer-provided state.

2. **SHA3 UTXO Snapshot** — a canonical-order UTXO payload transferred with a
   SHA3-256 byte-integrity commitment. A peer-provided root proves consistency
   with that peer's manifest, not consensus provenance.

3. **Delta sync** — Headers + blocks from snapshot height to tip via standard P2P.

The ~60-second goal is assisted operational readiness, not sovereignty. Mining,
wallet spending, snapshot re-serving, and canonical publication remain disabled
until complete-state validation and local full-history promotion succeed.

Native checks: `zclassic23 core consensus mmb`,
`zclassic23 core consensus utxo commitment`, `zclassic23 core sync status`,
and `zclassic23 core sync validation`.

### P2P Game Service

Built-in P2P game framework for latency measurement and gameplay:

**Ping (Type 0)** — Measures round-trip latency in microseconds. Used by:
- `zclassic23 core network peers latency` — round-trip latency for every peer

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
- **On-chain**: structured data in the Sapling 512-byte encrypted memo field (shielded) — **implemented**: `msg_send_onchain()` (`messaging_controller.c`) composes `z_sendmany` with the 38-byte memo codec (`lib/net/src/zmsg.c`), receive-side ingestion is wired at tip-finalize; sending requires Sapling params loaded + a passing prover self-test
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

Progress via: `zclassic23 core sync validation`

---

## Development

Build/test/deploy commands, the fast inner loop, and the push traps are in
the **`zclassic23-dev` skill** — the short version: `make build-only`
(compile-check), `make -j$(nproc)` (full build), `make test` /
`make test-parallel` (the canonical runner — never `test_zcl` directly),
`make lint`, and `make deploy` (owner-gated canonical only). Public
`make deploy-dev*`, `make agent-deploy-fast`, and recovery apply entry points
are contained and refuse; use build, verify, plan, and probe surfaces during
Phase 0.

Note on `-j`: each binary is ONE whole-program `cc` over ~660–1400 `.c` files
(LTO), so `-j$(nproc)` only overlaps the 2–3 separate binaries + the LTO link,
NOT the per-binary front-end compile. For a fast compile-check inner loop use
`make build-only` (664 `cc -c` genuinely parallel under `-j`); header edits are
now tracked via depfiles, so it no longer false-greens on a header change.
Default build targets `-march=x86-64-v3` (portable: AVX2/FMA/BMI2); pass
`ZCL_NATIVE=1` to build for the host CPU only.

### Services
```bash
systemctl --user status zclassic23        # Main node
systemctl --user status zclassicd         # C++ legacy reference/oracle
systemctl --user status zclassic23-test   # Test instance
```
