# Session handoff — Detective Wave 3: "Always strongly syncing to tip" (2026-07-10)

**Context:** the live node (build `3b0de63b0`) sat **wedged 216 blocks behind headers for
its entire ~4.6h uptime**. This wave (a) root-caused and cured that stall class with an
auto-terminating remedy, (b) shipped the watchtower that pages the class within minutes,
(c) hardened the peer layer against restart-amnesty and onion/IPv6 sybils, (d) refreshed +
wired the golden immutable-history evidence, and (e) closed part of the AI self-dev loop.

## The live P0 (diagnosed + cured this session)

Root cause chain (verified in code, live via MCP, and by the Opus planner):
1. The snapshot-seed path ran `anchor_kv_reset_in_tx` (activation_cursor=3,056,758)
   **without inserting an initial Sapling anchor frontier row**.
2. First shielded-output block after seed (h=3,176,326) → empty `sapling_anchors` table →
   `anchor_kv_latest_tree` = `HISTORY_INCOMPLETE` (`lib/storage/src/anchor_kv.c`) →
   `fold_sapling` FAILS CLOSED (`app/jobs/src/utxo_apply_anchors.c`) → utxo_apply cursor
   held forever; blocker `utxo_apply.apply_failed` (`shielded_anchor_history_gap`) fired
   9,104× with an EMPTY escape action. Consensus-correct, liveness-fatal, no self-cure —
   a stickiness-invariant violation.
3. Red herring: `sapling_tree_ckpt.dat` "missing_or_corrupt" at boot is a separate,
   already-cured class (`e009646bb` persists the output tree; it does NOT cure this).

**The cure (merged):** condition `sapling_anchor_frontier_unavailable`
(`app/conditions/src/sapling_anchor_frontier_unavailable.c`):
- **Tier 1** seeds ONE header-verified frontier row from the flat sapling checkpoint —
  only when its root == `hashFinalSaplingRoot` at the checkpoint height AND the height is
  in the safe window `[activation, stall)` (no sapling outputs in between, guaranteed by
  the empty table). `anchor_kv_seed_frontier_row` re-verifies and writes nothing on
  mismatch.
- **Tier 2** arms the bounded refold-from-anchor + supervised respawn via
  `sticky_escalator_note_stall` (reuses the TERMINAL budget machinery).
- **Tier 3** leaves the honest owner-gated permanent blocker. Never fake-resolves.
- Boot now pre-seeds the verified frontier after both `anchor_kv_reset_in_tx` call sites
  (`config/src/boot.c`), so fresh seeds never enter the state. SPROUT is not tier-1
  curable (no header-committed root) — falls to tier 2/3 by design.
- Parity guardrails held: `sapling_frontier_mismatch` untouched, unknown anchors still
  rejected, `activation_cursor` never zeroed; `check-consensus-parity` + parity groups green.

**⚠ COPY-PROVE VERDICT (run this session, `sapling-anchor-cure2`, `--full
-paramsdir=~/.zcash-params`):** exactly the predicted honest failure — tier 1 REFUSED
(the flat checkpoint was rewritten at h=3,176,326 == stall height, outside the safe
window `[activation, stall)`), tier 2 found no `utxo-anchor.snapshot`, tier 3 PAGED
(`operator_needed` within ~140s of boot, re-paging on backoff). **The silent-stall class
is dead** (named refusal + page instead of 4.6h of nothing), **but the node does not yet
self-cure this instance → DO NOT deploy expecting an unwedge.**

**Tier-1b findings (verified this session, build on these):**
- The uss seed snapshot (`utxo-seed-3155842.snapshot`) carries NO sapling tree — no
  verified frontier exists anywhere on the live datadir. A frontier cannot be derived
  from a root; rebuilding from bodies lacks a verified base below the seed.
- zclassicd has NO `z_gettreestate` RPC (probed, -32601). The viable **stopgap**: read
  the sapling anchor trees from zclassicd's chainstate LevelDB directly (the legacy
  import machinery already reads that datadir; zcashd-lineage chainstate stores every
  historical sapling anchor tree). Borrow the tree at a safe-window height, VERIFY its
  root == the PoW-committed `hashFinalSaplingRoot` header field (fail-closed, same trust
  model as the legacy bootstrap), then `anchor_kv_seed_frontier_row`. Copy-prove gated
  on H\* CLIMB past 3,176,326.
- The **terminal cure is the sovereign anchor artifact** (the long-running
  `utxo-anchor.snapshot` mint, see the anchor-mint sections of this file's parent
  HANDOFF): the artifact carries the anchor frontier
  (`boot_anchor_seed_from_snapshot`), and the merged tier 2 then fires AUTOMATICALLY
  (arm → respawn → refold). Finishing the mint cures this P0 with zero new code — the
  P0's terminal cure and the sovereign-cure critical path are the same work.

## What landed on main this session (all gated: lint clean + full test_parallel 0/540)

- `c76e40ec3` **SHA3 golden windows extended to h=3,175,999** (3,176 rows, +65) and the
  generator hardened: found + fixed a REAL latent bug — the old trailing row was a
  partial-window mint that would false-fire the tripwire on any refold crossing
  h=3,110,999. `gen_sha3_windows` now emits full windows only, has `--extend` (keeps
  locked rows, proves continuity against the source, refuses on divergence) +
  `--confirm-depth`. Standalone tool link repaired (safe_alloc/cleanse deps).
- `068a34c40` **4 new watchtower alert rules** (9 total): `header_gap_growing`
  (H\* vs best header >144 blocks for >900s — would have paged this incident),
  `peer_count_collapsed`, `sync_state_stuck`, `consensus_reject_spike`; new gauges incl.
  `zcl_header_gap_blocks`. Uptime-clocked hysteresis (deterministic tests, no sleeps).
- `b67e2fcdf`+`57d661f9f` **golden UTXO-ladder tripwire wired into production**
  (observe-only, at 100-block boundaries, kill-switch `ZCL_DISABLE_UTXO_LADDER_TRIPWIRE`),
  **golden staleness canary** (pins compiled coverage: sha3_windows == 3176 exact),
  nightly gains byzantine detective-scenario sweep + `ZCL_UTXO_LADDER_HEAVY` step +
  golden-freshness check (fails at >70k blocks tip lag).
- `caeaf81bb`+`d290bff62`+`2d3a7152b` **the P0 cure** (above) + hermetic group
  `sapling_anchor_frontier_condition`.
- `abdebf7c5`..`a32f44fb1` **peer hardening**: banlist.dat persistence (SHA3-sidecar
  atomic format; restart no longer amnesties attackers), `PEER_OFFENCE_UNREQUESTED` wired
  false-positive-safe (withheld for `DL_STALL_TIMEOUT_SECS` after forced settles),
  per-peer addr rate limit (3000/60s → FLOOD), outbound diversity caps for IPv6 (/32, max
  2) and onion (flat max 2; onion-seed eclipse recovery verified unaffected).
- `491d310f1` light copy-prove now includes `sapling_tree_ckpt.dat`.
- Lint-gate improvements: E12 honest-witness regex learned
  `reducer_frontier_provable_tip_cached` (the canonical cached H\* read).

Also verified this session (no code): all 63 PoW checkpoints in `chainparams.c`
corroborated against a fresh dual-source mint (zclassicd == zclassic23, hashes in the
session scratchpad; table was NOT placeholders — the static `{{0}}` initializer is
populated at runtime via `uint256_set_hex`).

## In flight / next steps (ordered)

1. **Read the copy-prove verdict** (`~/.zclassic-c23-COPY-*-sapling-anchor-cure2`,
   summary JSON at end of the run log). PASS → deploy is owner-approved: `make deploy`,
   then verify H\* climbs to network tip, `zcl_state subsystem=blocker` clears the anchor
   blockers, `header_gap_growing` alert armed. FAIL/tier-3 → build tier-1b (see above),
   copy-prove again. **Never deploy on a red or unread copy-prove.**
2. **Merge Lane 5** if not already merged: worktree branch has `db3839a84` (**zcl_agent_test**
   MCP-triggerable allow-listed test/scenario runner, destructive-tier, clones the
   agentcopyprove async pattern) + `79307fcd8` (**`_health` seeded in 22 more dumpers**).
   Reconcile: test-group count (main is at 544), `mcp_router_count` / EXPECTED_TOTAL in
   `test_mcp_controllers.c`/`test_mcp_e2e`, condition/doc counts. Full gates after.
3. **Enable the nightly timer** (owner-gated, one command):
   `systemctl --user enable --now zclassic23-simnet-nightly.timer` (units in `deploy/`).
4. **Lane 2 (approved, not started):** at-tip invariant hardening — audit every
   fail-closed hold site in utxo_apply/script_validate/coin_backfill for a missing
   auto-remedy; meta-detector condition "typed blocker with empty escape_action + H\* not
   advancing > T" arming the escalator. Design in the approved plan
   (`~/.claude/plans/analyze-this-project-i-validated-brook.md`).
5. Small follow-ups: `getblock` verbosity-0 (raw hex) RPC parity gap (blocks second-source
   golden mints; zclassicd returns hex, we return verbose JSON + `size:0`); `zcl-rpc` CLI
   string-param quoting bug (-32700 on any string arg); extend PoW checkpoint table
   3.1M→3.15M (hash already dual-source-minted); ladder checkpoint rung h=3,056,758 is
   not a %100 boundary so the fold can never verify it (ties to the deferred
   boundary-root backfill, which stays BEHIND the sovereign cure).

## Traps this session hit (verify-don't-trust wins)

- **`pkill -f` self-matches**: killing a copy-prove by pattern killed the RELAUNCH command
  that contained the same string. Kill by PID (the memory already warned; it still bit).
- The copy-prove harness logs to `repro_node.log` in the copy dir — `node.log` there is
  the COPIED live history (its stale `path=` lines look alarming; they're not).
- Copy-prove of anything shielded needs `-paramsdir=$HOME/.zcash-params` (isolated HOME
  hides the params → node parks at `crypto_params_missing`).
- A leftover copy node from a prior session (`COPY-20260710-final-deploy`, p2p port
  18935) collided with the harness's default HTTPS port. Killed by PID. ~20 old
  `~/.zclassic-c23-COPY-*` dirs remain — cleanup candidates (KEEP
  `COPY-20260701-113424-stall-3166384`: it's the documented anchor-mint recreate source).
- Explorer claims need spot-checks: "checkpoint table is all placeholder zeros" was a
  misread (runtime-populated); "golden ladder verifier has zero callers" was REAL.
- Merged-main lint ratchets (silent-return, honest-witness, doc-counts) fire on merges
  even when each lane was green in isolation — reconcile at merge, don't re-litigate.
