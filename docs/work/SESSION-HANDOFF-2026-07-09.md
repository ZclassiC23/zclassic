# Session handoff — 2026-07-09

Complete resume guide for the next developer. Read this, then verify live
state with `zcl_status` / `zcl_agent` before trusting any claim below.

## TL;DR

`main` is clean and pushed (`0b8cb4ec8`); canonical is deployed to it and
healthy/serving. A big multi-agent session shipped ~15 fixes plus the
**in-memory simulation-network foundation** (`lib/sim/simnet`). Four WIP
efforts are preserved on `handoff/*` branches, each one small step from done.

## ⛔ #1 LIVE P0 — wallet key persistence is BROKEN on canonical (not fixed)

`getnewaddress` fails ("New address NOT saved"); `getwalletinfo.persistence`
shows `healthy:false, mismatch:true` (~201 persisted vs ~304 in keystore). A
wallet BEGIN-IMMEDIATE+retry fix WAS shipped and deployed, but **it did not
resolve the symptom** — it only handles transient contention. The real cause,
verified live: `utxo_mirror_sync` is stuck ~1300 blocks behind
(`mirror_height` << `frontier`) and `mirror_rebuild_from_coins_kv`
(`utxo_mirror_sync_service.c:329`) retries a FULL ~1.3M-row rebuild every ~5s,
each `rebuild: aborted after 0 rows` + `node_db_exec: database is locked`
(node-WIDE). A persistent writer holds the node.db WAL write lock
near-continuously; the wallet flush and the mirror both starve. **This is the
S2 utxo_mirror defect (`handoff/s2-utxo-mirror-wip`, roadmap §2 S2) with
live consequences beyond performance.** Real fix = S2 delta-apply mirror (stop
the huge repeated write txns) AND identify/resolve the persistent lock holder
(the key un-answered question — who holds it? candidates: a coins_view/reducer
flush, a stuck txn). The node otherwise serves fine. **DO NOT send real ZCL
until this is fixed** — receiving also writes to the starved DB.

## What shipped to `main` this session (origin is current)

Correctness / robustness / security:
- `download_queue_starved` no longer pages the operator forever at tip
  (settle-accounting fix + `accounting_drift` observable).
- `zcl_replay_exec` can no longer launder destructive MCP tools past the auth
  tier; the tool is itself destructive-flagged.
- `snapshot_negotiation_stalled` re-arms instead of latching forever (cooldown).
- Wallet `wallet_sqlite_flush_r` now uses `BEGIN IMMEDIATE` + bounded retry
  (correct for TRANSIENT node.db WAL contention; unit-tested). **NOTE: this did
  NOT fix the live wallet P0 — see the #1 LIVE P0 section above. The real cause
  is the S2 mirror thrash + a persistent lock holder, not transient
  contention.** Fix stays (it's correct as far as it goes) but is insufficient
  alone.

Performance / strength:
- `addrman` duplicate lookup is now O(1) (hash index), was O(n) per gossiped
  address — a remote-triggerable CPU lever.
- `block_index_projection` caches its per-event exists-check statement.
- S5/S6: window-extend fast/slow counters (via `zcl_state
  subsystem=reducer_frontier`) + onion-seed last-resort bootstrap test.

MCP / DRY / infra:
- Per-route MCP dispatch timeout (long-running tools no longer killed at 5s) +
  fixed a detached-worker scratch-buffer race; done with a name-keyed table so
  no global warning was disabled.
- download-stats + version-identity JSON consolidated across controllers.
- explorer snprintf warnings silenced.
- `agent_impact_rules.def` mappings for all new files (the pre-push gate
  requires every changed source file map to a focused test group — remember
  this when adding files).

## The in-memory simulation network (NEW — the strategic thrust)

**Why:** exercise ZSLP tokens, ZName names, escrow, spends etc. against the
REAL consensus code, deterministically, in RAM, BEFORE any real ZCL moves.

**Foundation (shipped, `lib/sim/simnet.{c,h}`, test group `simnet`):** mints
blocks through the real `connect_block(..., expensive_checks=false)` — PoW and
scripts skipped via a synthetic checkpoint, so no Equihash is solved and no
consensus code is touched. API: `simnet_init/free`, `simnet_mint_coinbase`,
`simnet_spend`, `simnet_tip_height`, `simnet_coin_exists`, `simnet_coin_value`.
State is a RAM-only `coins_view_cache`.

**Next slices (recipe in `docs/work/sovereign-service-roadmap.md`):**
1. Add public `simnet_mint_txs(s, txs, ntx)` (thin pass-through to the private
   `sim_mint_block`) so callers assemble arbitrary transparent+OP_RETURN blocks.
2. **ZSLP slice:** build genesis/send txs with the real `slp_build_genesis` /
   `slp_build_send` (`lib/zslp`), mint through simnet, assert the token
   projection. OP_RETURN is unspendable so consensus accepts it.
3. **ZName slice:** same shape with the `znam` register/resolve path.
4. **Multi-node:** wrap N `struct simnet` and copy accepted blocks between them
   via `tools/sim/sim_peer`.

## Preserved WIP branches (LOCAL only — origin holds only `main`)

- `handoff/zname-correctness-wip` — ZName (ZNAM) owner-check security fix
  (non-owners must not set records for a name) + RENEW expiry. **Diagnosed
  remaining work:** the migration reaches version 25 but `NODE_DB_MAX_SCHEMA`
  is 24 → reopen refuses; bump the max-schema constant to 25. Also a v20
  seeded test fails ("no such table: znam_names") because the synthetic DB
  seeds only wallet tables and skips the v16 migration that creates
  `znam_names` — fix the test fixture to create the table or run the real
  migration chain. Then build+test+lint and merge.
- `handoff/dry-mcp-consolidation-wip` — `status_peer_survey()` helper (unifies
  3 duplicate peer loops) + `mcp_res_set_oom()` helper (~45 OOM sites). **All
  three test groups passed; only `make lint` remained** — set
  `core.hooksPath tools/githooks`, run lint, commit, merge.
- `handoff/s2-utxo-mirror-wip` — S2 delta-apply mirror; barely started, likely
  redo fresh from roadmap §2 S2.
- `handoff/fold-timing-s1-precursor` — `ZCL_FOLD_TIMING` instrumentation + a
  refold-only H* gate. Keep the instrumentation; roadmap S1 supersedes the
  gate with a full incremental `reducer_frontier_compute_hstar` cache.

## Known gotchas (will bite you if you don't know them)

- **Adding a new source file → add an `AGENT_IMPACT_RULE` line** in
  `app/controllers/include/controllers/agent_impact_rules.def` mapping it to a
  test group, or the pre-push gate blocks the push ("no focused test mapping").
- **Creating a git worktree clobbers `core.hooksPath`** to an absolute path →
  `make lint`'s `check-git-hooks-installed` fails. Fix: `git config
  core.hooksPath tools/githooks`.
- **Dev lane keeps raising `auto_reindex` markers** (hit anchor 3174556,
  3175019, 3175019 this session). Deploy with `ZCL_DEV_ALLOW_AUTO_REINDEX_DEPLOY=1
  make deploy-dev`. Root cause (why dev keeps requesting reindex) is unsolved —
  worth investigating. After a recovery boot the dev RPC returns empty for a
  while (rebuilding chainstate) — expected, not a regression.
- **Session/API limits** were hit repeatedly; large parallel swarms burn budget
  fast. Pace at ~3 concurrent agents and prefer verified single slices.

## Live node state

Canonical is deployed to `main` (`0b8cb4ec8`) and healthy/serving at tip —
`primary_blocker: none`. BUT the wallet-persistence + utxo_mirror degradation
(the #1 LIVE P0 above) is live and node-wide. Verify with
`build/bin/zclassic23 mcpcall zcl_agent` and
`... zcl_node_log '{"pattern":"utxo_mirror|database is locked","since_secs":120,"max_lines":20}'`.

## Recommended next steps (priority order)

1. **Fix the #1 LIVE P0**: S2 delta-apply the utxo_mirror (stop the full ~1.3M
   row rebuild every 5s) AND find/resolve the persistent node.db write-lock
   holder. This unblocks wallet persistence + receiving funds. Start from
   `handoff/s2-utxo-mirror-wip` + roadmap §2 S2. Copy-prove before redeploying
   canonical.
2. Finish `handoff/zname-correctness-wip` (diagnosed: bump `NODE_DB_MAX_SCHEMA`
   24→25 + fix the seeded-test fixture) — security fix.
3. Finish `handoff/dry-mcp-consolidation-wip` (just needs lint) — quick win.
4. Chain the ZSLP + ZName sim slices onto `simnet` (recipe above).
5. Investigate the recurring dev auto-reindex.
6. Roadmap strength item S1 (incremental H*).
