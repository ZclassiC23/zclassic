# Session handoff — 2026-07-09 (evening)

Resume guide for the next developer. Verify live state with the node's own
tools before trusting any claim here (`zclassic23 agent`, or `build/bin/zclassic23
mcpcall zcl_agent`).

## TL;DR

`main` is clean and pushed at **`b119b339e`** (verify: `git log --oneline -1
origin/main`). A large multi-agent session shipped the wallet P0 fix and a full
**simulator phase-2 stack**, all merged + CI-green + deployed to the **dev** lane.
Two security/DRY branches are verified-and-mergeable. **The Codex executor tier was
removed** this session — the next Claude uses native subagents (opus/sonnet/haiku),
not the Codex plugin.

## What shipped to origin/main this session

Two integration rounds, both through the pre-push `fast-ci` gate (lint + build +
focused test groups), then deployed to the dev lane (`make deploy-dev`, verified
healthy at tip):

**Round 1 (@41b9f159d):**
- **Wallet P0 fix** (`p0/nodedb-write-lane`) — routes the node.db catch-up job's
  writes through the serialized `db_service` write lane, ending the cross-connection
  SQLITE_BUSY that starved the wallet flush. Copy-proven on a live-datadir copy:
  mirror advances, "database is locked" gone from steady state, `getnewaddress`
  persists (row_count 201→302, healthy:true). **This was the #1 live P0.**
- **Dev auto-reindex cure** (`fix/one-shot-worker-stall-class`) — one-shot boot
  workers now `supervisor_child_complete()` on exit so a finished worker can't trip
  the 1800s time-deadline false stall; `worker_on_stall` no longer arms the sticky
  escalator (only chain-tip causes do). Root-caused from dev logs.
- **ZNAM security** (`handoff/zname-correctness-wip`) — owner-auth on
  SET_RECORD/SET_TEXT, real RENEW expiry, node.db schema v25.
- **DRY MCP** (`handoff/dry-mcp-consolidation-wip`) — `mcp_res_set_oom()` +
  `status_peer_survey()` helpers, −90 lines.
- **Simulator phase-1 + cluster** (`sim/zslp-zname-slices`, `sim/cluster-reorg`,
  `sim/action-matrix`) — `simnet` mints through the real `connect_block`; multi-node
  cluster with real `disconnect_block` reorgs; ZSLP/ZNAM/action-coverage tests;
  `docs/SIMULATOR.md`.

**Round 2 (@b119b339e):**
- **P2P parser fuzz** (`fuzz/p2p-parsers`) — coverage 3/46 → 46/46 inbound parsers
  via the real dispatcher path; fixed the `fuzz-ci` false-green (missing clang now
  exits 2, not a silent PASS).
- **Byzantine peers** (`sim/byzantine-peers`) — 8 tier-1 rejects through real
  `connect_block` + 3 tier-2 header-gate rejects, asserting typed-blocker + no
  silent halt; mutation self-check.
- **Transaction toolkit** (`sim/tx-toolkit`) — `simnet_mempool` + `simnet_wallet`
  high-level API, all tx kinds (P2PKH/consolidation/fan-out/OP_RETURN/HTLC
  fund-redeem-refund/chained), a **measured** cost table (ZCL fee + size + time-to-
  usable), `docs/SIMULATOR_TXNS.md` with worked examples for future AI development.

Also confirmed a stale audit item as already-fixed: coins_ram UAF/O(cap)
(`6d998f6be`, `e2db5ca87`) — no-op, memory corrected.

## Verified-and-mergeable (local branches, NOT yet merged)

- **`security/nullifier-backfill` @ee1cf50e2** — MERGE-READY, **OWNER-GATED**. Fixes
  the live `nullifier_backfill_gap`: a populate-only walker re-derives Sprout/Sapling
  nullifiers below the activation cursor (3155843) via the REAL
  `utxo_apply_check_and_insert_nullifiers` writer (consensus check path UNTOUCHED —
  verified). Gated behind `-backfill-nullifiers` / `ZCL_NULLIFIER_BACKFILL=1`
  (one-shot, before P2P/RPC — never runs on a normal canonical boot). Idempotent +
  resumable. Test proves a pre-activation double-spend is accepted before backfill /
  rejected after. **Copy-prove before any live use; never auto-deploy to canonical.**
- **`dry/coins-record-codec`** — unifies the 4-5 hand-decoded coin-record decoders
  into one `lib/storage/src/coins_record_codec.{c,h}`, behavior-preserving. The 3
  known silent-truncation tightenings (coins_db 4096-vout cap, utxo_import short-
  value→height=0, node_db_import 65535B) are DEFERRED as replay-gated follow-ups per
  CONSENSUS_PARITY_DOCTRINE (tightening a bounded coin-decode predicate needs a
  full-history replay first).

## Designs ready to build (docs/work/)

- **`docs/work/io-harness-design.md`** — the owner-directed network-IO adversarial
  harness (`simnet_wire`): an in-memory transport substituting at
  `p2p_node_receive_bytes` (ingress) + draining `node->send_head` (egress) so the
  node's framing/checksum/handshake/rate-limit/ban machinery runs byte-identical to a
  real socket; deterministic (seed_tape), reuses the cluster scheduler + byzantine
  artifacts + fuzz seam; monitors the prime invariants; CI grep-gate forbids socket
  syscalls in the harness. Build order: Slice A (transport engine) → B (malformed/
  flood peers + monitors) → C (byzantine wire injection). Start here for sim phase-3.
- **`docs/work/next-wave-plan.md`** — ranked DRY/security/test lanes. Remaining after
  this session: #3 peer-gossip blob memcpy hardening (AR_READ_BLOB), printf→LOG_*
  MCP-stdio, explorer boilerplate unify, RPC oracle boilerplate. (3 older review
  findings re-verified as already-fixed — skip.)
- **`docs/work/sim-phase2-plan.md`** — the phase-2 roadmap (mostly shipped this
  session; the seed-fuzzing Item 3 and contract-overlay Item 4 remain).

## Dev workflow change: Codex removed

The Codex executor plugin (`openai-codex`) was **removed** this session. It caused
significant friction (bwrap sandbox EPERM on this host requiring a patch; forwarder
agents repeatedly ending their turns mid-push). The next Claude should orchestrate
with **native subagents**: Fable coordinates/plans/reviews, Opus for hard
implementation, Sonnet for scoped implementation + verification, Haiku for mechanical
work. Set `model:` on every Agent call. The parallel-worktree pattern still applies
(main orchestrates; create worktrees for isolated lanes; `cp -a vendor/lib` from main
so fresh worktrees link).

## Live state

- Canonical (`~/.zclassic-c23`, rpc 18232): healthy, at tip, serving. **Still runs
  the pre-fix binary until you deploy — the wallet P0 fix is on main+dev but NOT on
  canonical (owner-gated).** See `docs/work/canonical-deploy-brief-p0.md` (scratchpad
  copy) for the decision brief: copy-prove evidence + the reversible `make deploy`
  command. Deploy to canonical is YOUR call.
- Dev (`~/.zclassic-c23-dev`, rpc 18252): running `b119b339e`, healthy at tip.

## Gotchas re-learned

- `make deploy-dev` verify-sync-health is a long wait loop; NEVER run two deploys
  concurrently (they fight over the dev service — cost us a restart this session).
  One deploy at a time.
- A `git push` runs the pre-push `fast-ci` hook (~2-3 min for a many-commit push);
  don't kill it mid-build. If a driver ends its turn, the push can be orphaned — run
  push+deploy from one persistent script, not an agent that may stop.
- New source file → add an `AGENT_IMPACT_RULE` in `agent_impact_rules.def` or the
  pre-push gate blocks the push.
