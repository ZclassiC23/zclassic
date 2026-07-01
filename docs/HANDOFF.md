## CURRENT STATE (2026-07-01, live verified)

**Restart command:** type **`continue zclassic23 development`**. First checks:
`git status --short --branch`, `./tools/z status`,
`curl -sk https://127.0.0.1:8443/api/status`, and `./tools/z mirror --json`.

**Live node.** The user linger service is running the locally deployed binary
(`make deploy`, installed at `$HOME/.local/bin/zclassic23-live`). The latest
post-commit deploy verifier completed at h=3166104 on build `e0885b027`; use
`./tools/z status` for the exact running `build_commit`. At the latest handoff
check the native node reported `sync_state=at_tip`,
`consensus_authority=local_consensus_validation`, `healthy=true`,
`serving=true`, 4 peers, and Tor/onion ready. The public `/api/status` surface is
now aligned with the health contract for the normal H* race: a one-block
served-frontier gap reports `status=healthy`, `operator_needed=false`, and
`primary_blocker=none`; gaps greater than one remain named as catching-up or
degraded.

```
systems
native node service        [##########] healthy/serving at local tip
REST/MCP public API        [##########] H*-honest; one-block H* race is green
HODL website freshness     [#########-] current view refreshes to served tip
factoids website freshness [########--] capped to served tip; unsafe sections suppressed on projection holes
legacy mirror advisory     [####------] monitor running; zclassicd RPC -28 at height 0
```

**API/data hardening landed.** The REST/factoid/HODL work now serves honest JSON
instead of transient 503s for normal projection races: `/api/hodl` refreshes
synchronously from the transparent UTXO set when cache lags the served tip, and
`/api/factoids` caps to `served_height` while reporting
`chain_height`/`served_height`/`indexed_height`/`index_capped`. The
`hodl_history` table is still a background time-series (`MAX(height)=3162240` at
handoff), but the website appends the latest on-demand HODL point, so the visible
current stats stay tied to the latest served block. If v1 wants the historical
table caught up to tip, make that an explicit follow-up gate.

**Wallet-view cleanup landed.** `wallet_sapling_notes` schema v21 adds validated
`source` (`local` by default, `view` for zclassicd mirror placeholders), a
partial `idx_snote_view_address`, and a fail-closed migration. Empty successful
wallet-view RPC result arrays clear stale mirror rows; Sapling refreshes replace
only `source='view'`; real local notes are preserved; and spend attempts against
view-only placeholder balance return `view-only balance synced from zclassicd`.
Important root cause from the deploy: the partial index must live in the v21
migration, not the baseline v20 create path, because live v20 databases do not
yet have `source`.

**Verification completed before this handoff.** Focused gates covered the new
wallet/data/API paths, `git diff --check`, `make check-test-registration`,
`make check-doc-accuracy`, `tools/scripts/check_raw_sqlite.sh`, `make lint`,
full `make test` (`0/485 groups failed, 14 skipped`), production binary build,
and `make deploy`. Live DB inspection confirmed `wallet_sapling_notes.source`,
schema migration `021`, and `idx_snote_view_address`.

**Final deploy note.** A post-commit restart exposed a startup-only evidence
lag: `tip_finalize_stage_init()` could publish an existing durable served tip
without stamping the chain-evidence pending tip, leaving health red
(`active_tip_hash_mismatch`) until the next normal block finalized. The startup
path now calls `chain_evidence_note_finalized_tip(existing_tip)`, so the first
health collection drains evidence to the recovered served tip immediately.

**Public status false-alarm cleanup.** Build `e0885b027` fixed the compact REST
status contract after the startup-evidence deploy: `/api/status` no longer pages
operator action for the normal one-block gap between `served_height` and
`indexed_height`/`header_height`. Regression coverage lives in `test_api`
(`public status treats one-block served gap as healthy` and
`public status still degrades material served gap`).

**Mirror status.** The zclassicd-authoritative mirror is advisory-only right now:
`mirror_running=true`, transport reachable, but `zclassicd` RPC returns `-28`
(`Activating best chain... height 0 (1%)`). Native authority remains
`local_consensus_validation`; no unsafe overrides are active.

## PREVIOUS STATE (2026-06-29, live verified)

**Live node is healthy and at tip after two 2026-06-29 wedge cures.** The
commitment-audit false positive first unwedged H\*=3164075 → 3164482. The deeper
3164483 coin-hole was then fixed, copy-proven, deployed to `zclassic23.service`,
and live-verified: `coin_backfill` inserted the missing 3164371 outpoint,
`reducer_frontier_reconcile_light` cleared, service state became healthy,
`healthcheck` reported `sync_state=at_tip`, and public RPC reported
`getblockcount=3164694` / H\*=3164694 during the verification sweep. Verify fresh:
`zcl_status`, then `zcl_state subsystem=reducer_frontier` and
`zcl_state subsystem=coin_backfill`.

**Commitment-audit false-positive cure.** A UTXO-commitment audit (a heuristic
over the *rebuildable `utxos` projection*) was raising a permanent
`chain_linkage` HOLD that rolled back a PoW-proven `tip_finalize` (`JOB_FATAL` →
txn rollback → "missing-success-row"), with its owner condition stuck on a
`COND_REMEDY_FAILED` stub. The cure decouples the audit from consensus finalize,
self-heals the checkpoint on the growth path + clears the latch, gates the
diagnostic behind 2 consecutive verdicts (kills the mirror-rebuild race), and
gives it an auto-terminating owner. Full write-up:
[`docs/work/commitment-audit-wedge-cure-2026-06-29.md`](work/commitment-audit-wedge-cure-2026-06-29.md)
+ review [`docs/work/commit-audit-cure-review-2026-06-29.md`](work/commit-audit-cure-review-2026-06-29.md).

**3164483 coin-hole cure.** Block 3164483 spent a coin missing from `coins_kv`
and the old `coin_backfill` scan could persist a terminal refusal before proving
terminal linkage to the hole hash. The fix makes the no-spend scan bind terminal
lineage before `SPENT_FOUND`, treats `node.db` txindex as a hint with a bounded
active-chain fallback, re-proves legacy unversioned refusal markers, and removes
the delta horizon as a refusal boundary. Full write-up:
[`docs/work/coin-hole-3164483-next-2026-06-29.md`](work/coin-hole-3164483-next-2026-06-29.md).

**Reductions in the commitment-audit cure (for the next reviewer):** the audit is
now a non-fatal diagnostic, not a consensus gate — real coin-set integrity rests on
the per-block fold ok-verdict + the SHA3 full-set commitment (untouched). The review
doc flags follow-ups (stale comments to fix, the growth-path checkpoint write shares
the unserialized node.db connection — benign/self-healing, and the audit is now a
transient-stability check rather than a durable-baseline detector).

**Bootstrap is still a borrowed-but-consensus-bound stopgap.** The node seeds
`coins_kv` from a near-tip SHA3 snapshot (`utxo-seed-3155842.snapshot`,
starterpack-3155842) whose anchor hash is bound to the in-binary PoW header, then
folds forward. The snapshot's UTXO *content* is not yet independently re-derived
from genesis — the sovereign self-mint anchor below remains the cure that deletes
the borrowed seed path.

**Remaining in-flight work — the sovereign self-mint anchor.** Goal: self-mint a
from-genesis SHA3 anchor at a compiled checkpoint, cut over via
`-refold-from-anchor`, and DELETE the borrowed near-tip loader. Code lives in
`tools/mint_v2_snapshot.c` (auto-pick seed height + Sapling-frontier reuse so it
can mint from a blocks-pruned source) and `lib/test/src/test_self_folded_anchor.c`
(re-derive == baked checkpoint; a mutated coin is detectable). `mint_v2_snapshot`
is a standalone `BUILD_NODE_TOOL` target (not in default `all`); the anchor
fixture runs under `build/bin/test_zcl`, not `test-parallel`. Plan of record:
[`docs/work/sticky-node-plan.md`](work/sticky-node-plan.md) (sovereign-cure spine:
[`docs/work/self-verified-tip-plan.md`](work/self-verified-tip-plan.md)).

**Do NOT re-chase the cured wedges.** Verify the live node first — a doc can be stale, the
node cannot.

# HANDOFF — read this first

**Restart command:** type **`continue zclassic23 development`**.
Verify HEAD with `git status --short --branch`; detect your worktree with `pwd`
(`main` = orchestrator; `wt2`/`wt3` = workers — `docs/work/README.md`).

## Recent history

`git log --oneline -20` is the authoritative recent record. The most recent
landed work is the **commitment-audit wedge cure** (see **CURRENT STATE** above);
before it, the forward-sync never-wedge cure (`75f35dcaf` + the deadlock/de-latch
commits) and the explorer refresh (`90539edd4`). The dated sections below this line
are historical context — trust `git log` and the live node over any of them.

---

## 0. Meta-discipline (read before you trust ANY claim, including this file)

Trust ONLY what you re-derive from the code THIS minute + the live node. Every
"FIXED/cured/cleared" in older docs is UNPROVEN until you re-read the cited
file:line. Be terse with the owner (3-6 lines). less-is-more. Verify live state
with `zcl_status` — never assume "synced"/"wedged" from a doc.

---

## 1. Where things stand (2026-06-23) — wedge FIXED, node reaches tip

The forward-sync **wedge class is FIXED**. The node runs a **consolidated
daily-driver binary** and **reaches and HOLDS the real network tip** — verified
live (`getblockcount` ~3,156,986, climbing; `verificationprogress=1`; the former
wedge block 3,156,171 is now active). It does so via a
**borrowed-but-consensus-bound STOPGAP**: it trusts the reference node's UTXO set
at a near-tip height (provenance proven, anchor hash bound to the in-binary
PoW header), instead of folding that set from its own checkpoint. It is a
**labeled, time-boxed bridge — not the cure.**

Six proven, consensus-audited fixes make up the consolidated binary:

1. **loader-owns-seed** — `-load-snapshot-at-own-height` re-seeds `coins_kv` from
   a checkpoint-OR-anchor-bound snapshot and raises the reducer trusted base to
   the seed height; a `loader_owns_seed` gate stops the torn-import auto-arm from
   re-seeding off the compiled checkpoint (which had re-pinned H\*).
2. **header-sync forward-only `pindex_best_header`** (`chain_state_service.c`) —
   rejects an equal-work lower-height tip so the best-header pointer can't
   regress; matches zclassicd `pindexBestHeader`, reorg-safe.
3. **stopgap consensus cross-check + Sapling rebuild** — the snapshot's
   `anchor_block_hash` must equal the in-binary PoW header at the seed height
   (FATAL on mismatch; validated live as a match), and the Sapling
   note-commitment tree is rebuilt at the seed.
4. **wallet-persistence ordering** — `node.db` is opened before the wallet block
   so keys/shielded notes persist across restart (was a RAM-only-keys boot bug).
5. **fold-speedup** — a block-parse cache (dedup the repeated per-block parse) +
   `coins_kv` batch apply. Proven **bit-identical**
   (`test_reducer_forward_progress_gate`).
6. **snapshot-above-coins-best window extension** (`ab512d577`) — when the
   snapshot seed height is ABOVE coins-best, the loader widens the active-chain
   window forward to `pindex_best_header` (`active_chain_extend_window`,
   `boot_refold_staged.c:702`) so the consensus anchor-hash cross-check reads the
   real PoW-proven `block_index` at the seed height, instead of FATAL-ing
   "Run --importblockindex". A pprev gap still leaves the slot NULL and the
   downstream FATAL still fires (a forged/missing anchor fails closed). This is
   the fix that actually unwedged the live node: a COMPLETE SHA3-verified seed at
   h=3,156,809 (`utxo-seed-3156809.snapshot`, count 1,344,918, sha3
   `040bde49…692bd125`, block_hash `00000933f7…b039ea074`) sits above the height
   that wedged the older torn 3,151,901 seed, and folds forward without
   re-touching it.

**Resilience posture (red-team verdict 2026-06-22, `docs/work/recovery-selfheal-redteam-2026-06-21.md`):**
the deployed config self-heals UNATTENDED from **3 of 4** corruption classes —
chainstate-corruption (→ `-reindex-chainstate`, confirmed live ~390 blk/s),
kill-9 mid-fold (durable per-block SQLite txn, recovers to tip bit-identical —
but SLOW ~16 min because every boot re-replays the Sapling tree), and
supervisor restart (`Restart=always`, never burns out). A missing/forged anchor
snapshot DECLINES honestly (`operator_needed`, never serves wrong data). cgroup-OOM
is PROVEN survivable (a runaway hits its own cgroup cap, the box survives, the node
restarts). **Two P1 gaps remain:** (a) a corrupted `coins_kv`/`progress.kv`
authority store CRASH-LOOPS under the loader flag (FATALs each boot instead of
dropping+reseeding); (b) recovery is slow (Sapling replay every boot — should
persist + load a cached tree). See backlog P1-6/P1-7.

**Validation posture:** the staged forward-fold `proof_validate` (shielded-proof
verification on the fold) is **NOT** gated by `-nobgvalidation`. `-nobgvalidation`
only skips the from-genesis re-verify of borrowed history (the RAM-heavy
`bg_validation_service`). Enabling full forward-fold proof verification RAM-safely
without the genesis re-verify is open work (see the backlog, P0-1).

---

## 2. The wedge class — the exact mechanism (keep this; it grounds the cure)

`reducer_frontier_compute_hstar` (`app/jobs/src/reducer_frontier.c`) defines the
**provable tip H\*** = MIN over the success-logs' **contiguous `ok=1` prefix from
a trusted anchor** (validate_headers / body_fetch / body_persist /
script_validate / proof_validate / utxo_apply / tip_finalize). A missing or
`ok=0` row **terminates the prefix**, so H\* stops one short of the first hole and
**refuses to advance** — it cannot serve a height it cannot fold-prove. Keep this
core; it is the SELECT-only honesty engine.

The trusted anchor floor starts at the compiled SHA3 checkpoint and only RAISES
to a declared base height if every upstream stage cursor is at/above it.

**Why seeding a high UTXO set wedged:** copying a coin set in at height H (cold
import or snapshot) gives the coins but writes **no per-height `ok=1` log rows**
for `[anchor+1 .. H]`. The contiguous prefix from the compiled anchor hits a hole
at anchor+1 and H\* pins there, even though `coins_applied_height` is far higher
("the borrowed-seed hole"). zclassicd cannot represent this: it has ONE UTXO
writer = ONE cursor = the same object, advanced one block at a time, and refuses
any block whose parent ≠ the coins best-block — a hole below the cursor is
structurally impossible.

Two ways to make H\* climb: **(a)** fold forward from a trusted anchor, writing
real `ok=1` rows for every height (the sovereign cure); **(b)** raise the trusted
base to the seed height so the prefix starts there (the stopgap) — sound **only
if the seed is verified**, which the consensus cross-check (fix #3) now enforces.

---

## 3. The sovereign cure (the North Star) — in flight

The stopgap trades sovereignty for "working today." The real fix makes the node
derive every UTXO by folding from its OWN cryptographic checkpoint, so it never
serves an unproven value:

```
mint genesis→compiled-checkpoint (verified anchor)
  → -refold-from-anchor (proven to climb H* anchor→tip, zero decoupled-authority walls)
  → live cutover
  → retroactively validate the borrowed near-tip snapshot
  → make -refold-from-anchor the DEFAULT + delete the borrowed-seed machinery (~715 LOC)
```

Proven-to-climb evidence: the refold reset hard-asserts commitment+count vs the
checkpoint (FATAL on mismatch), the fold fills all bounding logs contiguously
over on-disk bodies, and H\* = MIN climbs (code-read high-confidence + a regtest
fixture where H\* climbed after a reseed + forward fold).

The bottleneck is the **verified anchor**: the only place a UTXO set can be
cryptographically trusted is the compiled checkpoint, and rolling the near-tip
snapshot back to it is impossible (the reference node's durable block+undo index
ends below the snapshot height). So the **genesis→checkpoint mint** is the only
sound source; the fold-speedup work targets making it tractable.

---

## 4. The backlog (P0/P1/P2) — what to do next

| Pri | Item | State |
|---|---|---|
| **P0-1** | Full validation (drop `-nobgvalidation`) RAM-safely — enable forward-fold shielded-proof verify WITHOUT the genesis re-verify | in flight |
| **P0-2** | The sovereign anchor — genesis→checkpoint mint + fold-speedup | in flight |
| **P0-3** | Sovereign cutover (gated on P0-2): refold-from-anchor on a copy → gate on H\* climb → live cutover → retroactively validate the near-tip snapshot. Re-wedge guard: clamp the resume target to the on-disk body ceiling, not active_chain_height | designed |
| **P1-1** | Sapling caveats: `sapling_tree_rebuild` logs a root MISMATCH but does NOT fail; the resume path accepts an unverified checkpoint when `hashFinalSaplingRoot==0` at a 100k boundary | **LANDED 2026-06-24** (`7ffb3be68` fail-closed rebuilds + `a3fe506a3` checkpoint-binding tests) |
| **P1-2** | Boot-reindex always-terminates gap: a clean/reconcilable mid-replay reindex failure can loop without advancing the budget | **LANDED** (`186eb74e4` persists terminal exhaustion; `test_boot_reindex_terminates`) |
| **P1-3** | OOM durable guard: add a lint/runtime check that the systemd `MemoryMax` cap-sum < physical RAM (a global OOM once killed every node). **Cap re-budget (108G→60G) is APPLIED without root** via user-cgroup drop-ins + linger; only the optional swappiness + negative `oom_score_adj` would need sudo and are not load-bearing. | **LANDED 2026-06-24** (`23a75e868` `check-systemd-memory-budget` in `make lint`) |
| **P1-4** | Persistent .onion + onion-seed `/directory.json` discovery (the .onion regenerates every boot) | in flight |
| **P1-5** | Cold-start <10min (MVP C3): the single-binary snapshot import sets `coins_best_block` but does NOT seed `coins_kv`/`coins_applied_height`, so a snapshot cold-boot stalls (reducer L1 refuses). Banked partial C3: coins_kv seed works but needs header/block-index seeding too | in flight |
| **P1-6** | Corrupted-authority crash-loop: a torn `coins_kv`/`progress.kv` authority store FATALs each boot under the loader flag instead of dropping+reseeding (red-team gap) | open |
| **P1-7** | Slow recovery: every boot re-replays the Sapling note-commitment tree (~16 min). Persist + load a cached Sapling tree so a clean restart doesn't pay the full replay | open |
| **P2-1** | MVP gate (CLAUDE.md #1 priority) — live scorer `tools/mvp_gate.sh`; hermetic `make ci-mvp-gates` / `make mvp-verify` | open |
| **P2-2** | File market end-to-end (MVP C5) — `zmarket_buy` parks; `root_hash` is a `path:size` placeholder; remove the `zs1_pay_<time>` placeholder in `zslp_service.c` | open |
| **P2-3** | The permanent subtraction (~715 LOC): once a verified anchor is guaranteed reachable on every cold-start, make `-refold-from-anchor` the DEFAULT and delete the borrowed-seed machinery. Re-derive the delete-set from the CURRENT tree. KEEP the 5 legit `tip_finalize_stage_seed_anchor` callers. Parity-safe | gated on P0-2/P0-3 |

The wedge-class root-fix design (the fold-forward cure, the per-100-block UTXO
ladder/keystone, the never-stuck hardening map) is
[`docs/work/never-stuck-plan.md`](./work/never-stuck-plan.md); the ordered
subtraction worksheet is
[`docs/work/sync-fix-plan-2026-06-21.md`](./work/sync-fix-plan-2026-06-21.md);
the trustless-fast-sync substrate is
[`docs/work/sync-keystone.md`](./work/sync-keystone.md).

---

## 5. Features: real vs stub (verify before presenting)

**REAL & chain-derived (keep):** wallet, explorer (+projections), ZNAM,
embedded Tor, MCP/RPC, P2P ping/latency.

**STUBS (don't present as working):**
- **Atomic swaps** — builds HTLC script + P2SH + DB row; no funding broadcast,
  no chain monitoring, no settlement.
- **File market** — offer cache + P2P serialize exist; `root_hash` is a SHA3 of
  `path:size` PLACEHOLDER; `zmarket_buy` parks forever; no transfer/payment.
- **On-chain ZMSG** — persists a record labeled ONCHAIN; no Sapling memo
  build/broadcast/scan. (Off-chain P2P ZMSG is real, plaintext on wire.)
- **TicTacToe** — correct logic + tests, but no P2P wiring; not playable.

---

## 6. Live ops state & lanes

| Lane | Datadir | Deploy |
|---|---|---|
| **live** | `$HOME/.zclassic-c23` | `make deploy` (owner-gated) |
| **dev** | `$HOME/.zclassic-c23-dev` | `make deploy-dev` |
| **soak** | `$HOME/.zclassic-c23-soak` | deliberate re-baseline |

The live datadir runs `-load-snapshot-at-own-height` re-seeding 3,156,809 and
folding forward, so each restart is a ~13 min self-healing boot (cold `node.db`
read + Sapling tree rebuild + fold); a plain boot serving the persisted
`coins_kv` is likely faster but is NOT yet proven live.

`zclassicd` (the C++ reference) runs as a co-located service — **never stop it.**
The default ports and the operator runbook specifics live in `docs/SYNC.md` and
the dev notes; re-pull live state yourself (`zcl_status`, `zcl_state`) before
acting.

**Standing method:** `make deploy` rm's the binary first (the binary rule isn't
depfile-tracked; a stale binary was a real multi-day outage; `deploy_verify.sh`
confirms `build_commit`). Copy-prove every recovery path on a COPY before live,
never live surgery; gate on **H\* CLIMB**, not "booted without FATAL." Never
weaken a safety/operator gate. Gate every change: `make` + `make lint` +
`make test_parallel` (read the `N passed, M failed` line, not the pipe exit).
Replay any consensus-predicate tightening against REAL history first (the
h=478544 lesson — `docs/CONSENSUS_PARITY_DOCTRINE.md`).

---

## 7. MVP status

**MRS 4/8** (do NOT bump without proof): **C1** single-binary install, **C2** Tor
onion <60s, **C4** receive shielded, **C7** kill-9 recovery all pass their local
operator proof. **C3** cold-start (<10min), **C5** file market, **C6** 7-day soak,
**C8** exact parity are gated on the **sovereign foundation + accumulated soak
time** — soak hours can now ACCRUE since the node reaches and holds tip, so the
remaining gate is the sovereign cure + soak, not un-wedging.
The v1 contract is [`docs/MVP.md`](./MVP.md); THE plan is
[`docs/work/FORWARD_PLAN.md`](./work/FORWARD_PLAN.md). Live scorer:
`tools/mvp_gate.sh`.

---

## 8. Verify before you trust this file

A map, not the territory. Before building on any claim, re-read the cited
file:line — code moves and old narratives recur. If a claim doesn't match the
code you read THIS minute, trust the code. Architecture reference:
[`docs/FRAMEWORK.md`](./FRAMEWORK.md) + [`docs/REFACTOR_STATUS.md`](./REFACTOR_STATUS.md).
