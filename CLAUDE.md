# ZClassic23 — AI-Integrated Personal Sovereignty Stack

<!-- FIRST-FIVE-BEGIN -->
<!-- These five steps were followed cold on 2026-07-28 and every observation
     below is from that transcript. The load-bearing facts are bound to
     check-doc-claims predicates at the end of the block; the root and
     subsystem counts are bound to check-doc-counts. Re-follow, do not
     re-remember. -->
## First five commands in a fresh clone

1. `make setup` — one-time: fetch and build the vendored dependencies, and arm this clone's local git hooks. It ends by printing `Next: make doctor`; that is an optional environment check, not step 2.
2. `make -j"$(nproc)"` — build `build/bin/zclassic23`. One whole-program LTO `cc` per binary, so this is minutes even warm.
3. `build/bin/zclassic23 discover help` — the top of the command tree, not a flat list: it prints the 8 command roots plus the bare `status` leaf. Descend with `discover help <path>`, narrow with `discover search <query>` (query is **positional**), and get exact input keys with `discover schema <leaf>`. The whole catalog is `docs/API_REFERENCE.md`, generated from `config/commands/*.def`.
4. `build/bin/zclassic23 code map` — the source tree's floor plan, from the navigator built into the binary. First call builds the index (~1.4 s, and it reports `budget_exceeded`); later calls are ~12 ms.
5. `make t-fast ONLY=<substring>` — run the test groups whose names contain `<substring>`, e.g. `make t-fast ONLY=boot_phase`. `ONLY=` is mandatory and unvalidated: run it with the literal placeholder and you get a full test-binary compile followed by `sh: 1: Syntax error: end of file unexpected`. All 785 test groups by name, no build: `git grep -hoE 'X\([a-z_0-9]+\)' lib/test/src/test_parallel.c | tr -d 'X()'` (drop `-h` and you get the filename glued to every name). `make test-parallel` runs the whole suite.

<!-- claim: symbol-present t-fast Makefile # step 5's target -->
<!-- claim: symbol-present test-parallel Makefile # step 5's whole-suite target -->
<!-- claim: symbol-present discover.schema config/commands/root.def # step 3's leaf -->
<!-- claim: symbol-present code.map config/commands/code.def # step 4's leaf -->
<!-- claim: file-present docs/API_REFERENCE.md # step 3's generated catalog -->

Scope every text search with `git grep` (or `git ls-files | xargs grep`) — never `grep -r` or `find .` from the repository root. The root also contains untracked full checkouts under `.claude/worktrees/` and per-run scratch under `test-tmp/`, so an unscoped recursive search reads duplicate copies of the source and test debris instead of the tracked tree. The root `.ignore` file makes most search tools skip those by default; `git grep` is exact regardless.
<!-- FIRST-FIVE-END -->

## Vision — Personal Sovereignty Stack

ZClassic23 is one self-contained C23 binary that runs a full ZClassic node (Equihash 200,9 PoW, Sapling shielded txs), an embedded Tor onion service, a block explorer, a shielded wallet, a P2P file marketplace, ZNAM name registry, P2P messaging (plaintext P2P channel; on-chain Sapling-memo channel implemented — requires Sapling params + a passing prover self-test to send), cross-chain atomic swaps (BTC/LTC/DOGE; redeem/refund/settlement: in progress), a P2P game framework, and a native command registry. **Claude is a first-class operator via 100+ typed native commands** — not just an observer. Cold sync to tip via FlyClient + SHA3 UTXO snapshots is a design target (`docs/HANDOFF.md`); the proven recovery path today is the two-step header-import + boot. Silent halts are unreachable by construction — a stall is always a named blocker or a growing tip gap, never a quiet stop (chain progress is a stage cursor on disk); the node can still halt, it just cannot do so without saying so. Bugs become 64-bit seeds in a deterministic simulator. Deterministic build flags and a legacy GPG-capable packaging script exist; unsigned output is local-development-only and stable publication remains contained pending two-builder byte-identity proof. **One binary, one onion, one stack — your sovereign personal computing surface.**

See [`docs/HOW_THE_NODE_WORKS.md`](./docs/HOW_THE_NODE_WORKS.md) for the plain-language mental model (the node as a state machine), [`docs/FRAMEWORK.md`](./docs/FRAMEWORK.md) for the canonical architecture (the Prime Directive, the Ten Laws of Beauty, and the eight shapes), [`docs/AGENT_ARCHITECTURE.md`](./docs/AGENT_ARCHITECTURE.md) for the concrete future-agent feature slice (REST resources, ActiveRecord, validations, relationships, schema, services, native surfaces), [`docs/ARCHITECTURE_DIAGRAMS.md`](./docs/ARCHITECTURE_DIAGRAMS.md) for current subsystem/boot topology, and [`docs/adr/0001-personal-sovereignty-stack.md`](./docs/adr/0001-personal-sovereignty-stack.md) for the personal-sovereignty-stack pivot rationale.

**The developer operating manual is [`docs/DEVELOPING.md`](./docs/DEVELOPING.md)** (also loaded as the `zclassic23-dev` skill, which is a stub that imports it) — navigator-first code lookup, the fast dev loop / hot-swap tiers, typed-commands-over-bash, push traps, and build/test/deploy live there, not in this file.

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

**Every consensus predicate lives under `core/` and `core/` is byte-sealed.**
`core/{consensus,chainparams,params,math}` is pinned by `core/MANIFEST.sha3`;
the `check-core-seal` gate fails `make lint` on any drift — *after* you have
already written the edit. Unlock with `make core-unseal REASON="…"` (owner
ritual: appends to [`core/UNSEAL.md`](./core/UNSEAL.md), mints a one-commit
token), then re-freeze with `make core-seal`. Rationale:
[`docs/adr/0002-sealed-consensus-core.md`](./docs/adr/0002-sealed-consensus-core.md).

## Tenacity & recovery (operator invariants)

Full model: [`docs/TENACITY.md`](./docs/TENACITY.md) + the live-diagnosis
fast path [`docs/work/fast-path.md`](./docs/work/fast-path.md). Current bootstrap
posture + the sovereign-cure path: [`docs/HANDOFF.md`](./docs/HANDOFF.md).
Plain meaning: the **sovereign cure** is the self-verified UTXO rebuild that
starts from the in-binary SHA3/PoW checkpoint, folds real block bodies forward,
then deletes the borrowed `zclassicd`-minted seed path.

**Current live state lives exclusively in [`docs/HANDOFF.md`](./docs/HANDOFF.md)**
— this file carries no live height/sync-status claims; verify with
`zclassic23 status` / `zclassic23 dumpstate reducer_frontier` before acting.
The standing architectural fact behind every cure (past or future): ZClassic
headers do **not** commit the UTXO, Sapling/Sprout frontier, or nullifier
contents, so a state artifact whose height/hash merely matches a validated
header is not thereby PoW- or consensus-bound content — installing borrowed
state requires independently validating its transparent and shielded contents
atomically and passing copy proof, matching a header alone is not enough.
Watch the v1-oriented refold-reset path (`config/src/boot_shielded_seed.c`)
on any future cure: it must not discard a captured v3 shielded section.

**The legacy TWO-step recipe works** (hash-identical tip vs `zclassicd` at
multiple heights, warm-reboot-proven; this is the legacy `zclassicd`-datadir
bootstrap, not the cure). zclassicd stays RUNNING:

```bash
# 1. Headers FIRST — ~3.1M headers in ~60-74s from the legacy zclassicd datadir
build/bin/zclassic23 --importblockindex $HOME/.zclassic
# 2. Then a NORMAL boot — it auto-reads/links $HOME/.zclassic (legacy import
#    is on by default; opt out with -nolegacyimport)
build/bin/zclassic23
```

(The old `-cold-import=`/`-fastimport=` flags no longer exist. The argv loop
does **not** accept an unrecognized `-flag` silently — it prints
`Warning: unrecognized flag '<f>' (ignored) — …` to stderr on every boot
(`config/src/args.c`). Advisory, never fatal, so after any flag change grep
stderr for `unrecognized flag`.) Skipping step 1 is a
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

> **Check the live node before treating any status claim below as current.**
> This file states the durable mission shape only; it never carries a live
> height, wedge, or soak-progress claim — that all lives in
> [`docs/HANDOFF.md`](./docs/HANDOFF.md) §0-LATEST, re-derived from
> `zclassic23 status` / `zclassic23 dumpstate reducer_frontier` before acting.
> A doc can be stale; the node cannot.

The mission is one node that reaches and holds the network tip on
**self-verified state** — a UTXO/anchor/nullifier set the node re-derived from
block bodies and its own baked checkpoint, never one borrowed from an external
snapshot — and then earns a clean soak window on that foundation. The typed
native CLI (`zclassic23 <command>`) is the only agent interface. The
source-code navigator lives under `lib/codeindex/` and is exposed through the
native `code` command branch.

**The v1 bar is [`docs/MVP.md`](./docs/MVP.md)** — 8 operator acceptance criteria; v1 = MRS 8/8.
**THE plan is [`docs/work/FORWARD_PLAN.md`](./docs/work/FORWARD_PLAN.md)** — MVP-anchored, covering the autonomous / owner-gated / operational critical path; it carries the current ordered priority, not this file. Current live state is in [`docs/HANDOFF.md`](./docs/HANDOFF.md).

**Executor entry point — play the game:** run **`make arch-score`** (0-100
mechanical completion of the sync architecture). It names the highest-value
unfinished quest and points to **[`docs/ARCH_QUEST_BOARD.md`](./docs/ARCH_QUEST_BOARD.md)**
(exact move + un-cheatable win-proof per quest) over
**[`docs/ARCHITECTURE_NORTH_STAR.md`](./docs/ARCHITECTURE_NORTH_STAR.md)** (the
theory: one canonical ledger per domain, single writer per frontier). The loop
is: `make arch-score` → open the top ✗ quest → make the move in a worktree →
copy-prove → confirm the score rose → `make lint && make test-parallel` →
commit → repeat to 100. **Never edit the scorer to win.**

**The framework/architecture refactor is OFF the v1 path — do not jump the queue.** [`docs/FRAMEWORK.md`](./docs/FRAMEWORK.md) is the canonical architecture (the Prime Directive, Ten Laws, eight shapes) and §9 is the architecture debt board. It is reference, not the mission. Every `.c` under `app/` lives in exactly one of eight shape folders, lint-enforced.

**Parallel-worktree workflow:** main repo is the orchestrator; `~/github/zclassic23-2` (wt2) and `~/github/zclassic23-3` (wt3) are workers. See [`docs/work/README.md`](./docs/work/README.md) and [`docs/work/agent-protocol.md`](./docs/work/agent-protocol.md). Worker identity = pwd suffix.

### On a fresh session

Type **`continue zclassic23 development`**. The agent will:
1. Run `pwd` to detect worktree ID (`main`, `wt2`, `wt3`, ...).
2. For the one-page mental model, skim **[`docs/HOW_THE_NODE_WORKS.md`](./docs/HOW_THE_NODE_WORKS.md)** (append-only fact log in `consensus.db` → eight reducer stages, each advance-cursor-or-name-a-blocker → projections → health = `network_tip − log_head`). **[`docs/CODEBASE_MAP.md`](./docs/CODEBASE_MAP.md)** is where-things-live + how-to-do-each-thing; **[`docs/AGENT_ARCHITECTURE.md`](./docs/AGENT_ARCHITECTURE.md)** is the required feature-slice recipe for REST resources, ActiveRecord models, validations, relationships, database schema, services, and native command surfaces; **[`docs/AGENT_TRAPS.md`](./docs/AGENT_TRAPS.md)** lists things that look broken but are intentional or already-done — read it before "fixing" or re-proposing anything; **[`docs/EXTENSION_POINTS.md`](./docs/EXTENSION_POINTS.md)** is the one page for the surfaces under active construction (vault ownership, big integers, the declarative service manifest), and every claim on it is gate-bound.
3. Read exactly two more: `docs/HANDOFF.md` (live state) and `docs/work/FORWARD_PLAN.md` (the ordered priority). `docs/MVP.md` (the v1 contract), `docs/ARCH_QUEST_BOARD.md`, `docs/ARCHITECTURE_NORTH_STAR.md`, and `docs/FRAMEWORK.md` are **reference — open on demand, not on arrival**. The sovereign-cure spine is `docs/work/self-verified-tip-plan.md`.
4. Check the live node before trusting any doc: `zclassic23 status`, then
   `zclassic23 dumpstate reducer_frontier`. A doc can be stale; the node cannot.
5. If worker → read `docs/work/wt<N>-*.md` and follow `docs/work/agent-protocol.md`. If orchestrator → review in-flight work in the status board, merge pushed branches, dispatch next assignments. Before dispatching from a plan under `~/.claude/plans/`, run `make check-plan-claims`: those files are outside the repository, so `make lint` never sees them, and a plan that lists finished work as PENDING has already cost this project three agents.

**[`docs/README.md`](./docs/README.md) is the curated documentation map** — use
it to find anything not listed here. It splits public-contributor docs from
maintainer/live-node docs; `docs/HANDOFF.md` is on the maintainer side, which is
why step 3 above applies to this host and not to a fresh clone.

## Defensive Coding Standards (MANDATORY)

**The five rules that matter are listed below — they are the whole contract.**
[`DEFENSIVE_CODING.md`](./docs/DEFENSIVE_CODING.md) is the per-gate reference
(one section per `check-*` gate); open the section `make lint` names, not the
whole file.

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
8 command roots — `core.*`, `app.*`, `dev.*`, `ops.*`, `discover.*`, `code.*`,
`vault.*`, `zcode.*` — plus the bare `status` leaf. Never work from a
remembered root list; `discover help` prints the live one.
Start with `zclassic23 status`;
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

3. Add one descriptor row to
   `app/controllers/include/controllers/diagnostics_dumpers.def`. `DIAG_ENTRY`
   is the long form (~12 fields); nine row macros exist and most rows use a
   short one — read the `#define DIAG_*` block in
   `app/controllers/src/diagnostics_registry.c` and pick from it.
   **Do not edit that file's table** — it builds `g_dumpers[]` by
   `#include`-ing the `.def` and has no editable table.

4. No edit to the state command handler is needed; its subsystem catalog is
   populated at runtime from the diagnostics registry. Update the native
   diagnostics registry test if it asserts the list.

That's it — no new RPC handler, command route, or schema.
**Enumerate the live catalog with `zclassic23 statecatalog`** (name, owner
file, accepted key forms, cost, owning test path) rather than trusting a
hand-list here. `zclassic23 ops state` with no `--subsystem` is not an
enumeration — it fails with `MISSING_SUBSYSTEM`.

The `supervisor` subsystem is the *root* of the liveness
tree: it lists every registered child (`sync.watchdog`,
`net.outbound_floor`, `chain.coord_escalation`), along
with each child's last_tick_age_us, progress_marker, deadline,
ticks_run, and stall_fires. Use it to confirm any time-driven thing
is actually running. See `lib/util/include/util/supervisor.h` for the
contract API and `DEFENSIVE_CODING.md` Gate #15 for the lint gate that
ratchets adoption.

**Running is not the same as achieving anything, and the dump separates
the two.** `ticks_run` is activity; `progress_marker` is results, and
`idle_ticks` is "ran, legitimately had nothing to do". Whether anything
*checks* the results is `progress_policy` per child — `armed` (a frozen
marker with no idle report raises `no_progress`), `exempt` (off on
purpose, with `progress_exempt_reason` stating why), or `undeclared`
(nobody chose; off, and counted in the root's
`progress_undeclared_count`). A child must declare one, floor-gated
shrink-only by `check-supervisor-progress-declared`. When arming a
service, report `supervisor_progress_idle()` **only** where it has
positively established there is no work — never on an error or
not-wired path, which are the states the detector exists to catch;
`app/services/src/op_return_backfill_service.c` is the worked example.
The root also publishes `child_headroom`; at 0 the next subsystem to
register runs unsupervised.

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

A fresh node is *designed* to sync 3M+ blocks in ~60 seconds (design target — the stack below is built but not yet the proven cold-start; the proven cold-sync path today is the two-step `--importblockindex` + boot, see the Tenacity section):

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

- **Off-chain**: P2P messages (`zmsg`/`zmsgack`) between connected nodes — Noise-encrypted v2 transport is **implemented** below the message layer (`lib/net/src/v2_transport.c` + `lib/session/src/noise_handshake.c`, armed as INITIATOR in `lib/net/src/net.c` and decrypted/torn down in `lib/net/src/connman.c`) but **default OFF** pending rollout; until a peer negotiates v2, messages ride plaintext on the wire
- **On-chain**: structured data in the Sapling 512-byte encrypted memo field (shielded) — **implemented**: `msg_send_onchain()` (`messaging_controller.c`) composes `z_sendmany` with the 38-byte memo codec (`lib/net/src/zmsg.c`), receive-side ingestion is wired at tip-finalize; sending requires Sapling params loaded + a passing prover self-test
- Messages stored in SQLite, delivery acknowledgment
- RPC: `msg_send`, `msg_inbox`, `msg_read`

### ZCL Market — Crypto-Incentivized File Sharing

File marketplace: offer gossip with price metadata, proof-of-possession challenges, and a working file service that streams chunk bytes (`file_service.c` → `fs_send_chunk_fast`) with chunk unlock gated on a mempool-verified payment txid (`handle_zfilepay`, `msgprocessor.c`). On-chain payment settlement and the buy/offer RPC-to-transfer glue are not yet wired end-to-end.

- P2P gossip of file offers with price per MB
- Chunk challenges for sybil resistance (prove you have the data)
- RPC: `zmarket_list`, `zmarket_offer`, `zmarket_buy`, `zmarket_status`

### Atomic Swaps (ZSWP) — Cross-Chain HTLC Trading

HTLC contract scaffolding: swap initiation and participation with redeem script generation. Redemption, refund, and status/secret-extraction are wired end-to-end (`app/controllers/src/swap_controller.c`): `rpc_swap_redeem`/`rpc_swap_refund` build the settlement tx (`swap_settlement_build_redeem`), sign it, broadcast it on-chain (`swap_broadcast()`), and persist `SWAP_REDEEMED`/`SWAP_REFUNDED` state.

- **Chains**: ZCL, BTC, LTC, DOGE (same 97-byte contract as dcrdex)
- Script: OP_IF/OP_SHA256/OP_CLTV with shared OP_CHECKSIG
- Secret extraction / redeem + refund scriptSig builders exist as library primitives (`lib/script/{src/htlc.c,include/script/htlc.h}`, tested) and are wired to the node-broadcast/settlement path via the RPCs below
- RPC: `swap_chains`, `swap_initiate`, `swap_participate`, `swap_list`, `swap_redeem`, `swap_refund`, `swap_status`, `swap_extractsecret`
- Reference: dcrdex HTLC script format (Blue Oak License 1.0.0), reimplemented in `lib/script/{src/htlc.c,include/script/htlc.h}`

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
