# Live-tip wedge — the complete 5-blocker stack (root-caused 2026-06-06)

Owner directive: zclassicd (RPC 127.0.0.1:8232, height ~3,137,700) is TRUSTED;
sync zclassic23 to it, live, on this machine (no extra computers; use
non-conflicting ports — zclassicd owns P2P 8033 / RPC 8232, zclassic23 owns
8023 / 18232).

The tip holds at **3,134,303**. It is NOT one bug — it is a stack of five
interlocking blockers. The new reducer pipeline (header_admit → validate_headers
→ body_fetch → body_persist → script_validate → proof_validate → utxo_apply →
tip_finalize) is what's wedged; the legacy engine got coins to 3,134,303.

---

## ⚠️ CORRECTED UNDERSTANDING — 2026-06-06 instrumented diagnostic repro

A clean reproduce-and-trace run on a full copy (binary at `095dec9cb`, the wedge
fix `345114d06` carried forward) **overturned the central Blocker-8 theory**.
Read this before acting on anything below it.

**1. The reducer wedge fix WORKS.** Fed `backfill_header_solutions` +
`rebuild_recent` rounds from the trusted zclassicd, the copy climbed
**3,134,303 → 3,135,248 (+945 blocks)**, crossing every transparent-spend block
(CODE1 resolver), tip_finalize correctly *holding* the lookahead frontier
(CODE2: `precondition_failed … not_script_valid` → JOB_IDLE, no oscillation),
no consensus error. The fix is sound; the only thing gating forward speed is
**header-solution supply + peer-starvation**, not consensus.

**2. "Node destabilizes/exits after ~900 blocks" was a TEST ARTIFACT — not a
node bug.** The deaths in prior runs were the node being SIGTERM'd because it had
been launched as a *child of a transient background task*; when that task was
reaped, the node's process group got the signal (silent: no crash_log, no OOM,
not in the crash handler's `{SIGABRT,SIGSEGV,SIGBUS,SIGFPE}` set). Relaunched
fully detached (`setsid`, own session), the **idle node survives indefinitely at
0 peers**. There is no ~900-block self-termination.

**3. The "coins_best lag trips chain_integrity_failed" theory is WRONG** (exactly
as the design-workflow's adversarial panel found). During the live climb
`chain_integrity_failed` never fired; coins_best lag is **benign at runtime** —
cec logs `coins_best_block != active_tip … deferring/recoverable` (WARN) and the
node keeps working. `chain_integrity_check_post_restore` does not even read
coins_best.

**4. The REAL Blocker 8 = RESTART durability (Case-4 reset).** On restart, with
coins_best (node.db, 3,134,303) lagging the reducer tip (3,135,248),
`chain_state_validator` hits Case 4 and **resets the public tip back down to the
stale coins tip**:
```
Resetting chain to disk-backed coins tip — will replay blocks.
csr: tip committed from=3135248 to=3134303 reason=chain_coins_mismatch_reset
post-restore integrity RECONCILABLE: tip_window_holes=945 … DEGRADED_SERVING; Not fatal.
```
The node recovers (DEGRADED_SERVING, not fatal) but **loses the climb on every
restart**. cec correctly *refuses to rewrite the persisted high tip down*
(high-water protection). This is the genuine durability gap.

**The fix** is therefore a **tear-safe coins_best-follow** (the design workflow
`wj65w0x4n` returned BLOCKED_NO_GO on the first cut and gave the exact
constraints): advance coins_best toward the reducer tip **only AFTER the
tip_finalize cursor is durably committed** (so coins_best can only LAG, never
LEAD — a lead would hit `BOOT_RECOVER_WIPE_WAIT` on a torn restart, "TEAR A"),
keyed on **(height, hash)** so a same-height reorg re-anchors (not a no-op), using
`new_tip->nHeight` (not the off-by-one), routed through the already-validated
`promote_tip` single-writer. This is consensus-critical + deploy-gated; prove on
a copy to full convergence + `gettxoutsetinfo == zclassicd` before any deploy.

**Side findings this run:**
- **FIXED + committed `095dec9cb`:** a NULL-unsafe JSON accessor class bug — the
  `dumpstate` RPC with no arg SIGSEGV'd the whole node (json_get_str(json_at(
  params,0))→NULL deref). Any handler reading an absent param could crash the
  node. Root-fixed in lib/json + regression test.
- **Follow-up (not yet fixed):** `chain_evidence_controller_reconcile_startup`
  runs on EVERY ephemeral `chain_evidence_controller_init` (per-event,
  per-tip-hook, per-health-check) — observed 773 identical WARNs in one catch-up.
  It is a once-per-boot operation. Benign (node works) but wastes CPU + floods
  logs. Correct fix = explicit one-time boot reconcile (or a global-ndb-only
  guard); a naive process-static guard breaks the 9 per-fixture cec unit tests.
- The forward-progress drive that WORKS for a live catch-up to zclassicd:
  repeated `backfill_header_solutions(tip+1)` + `rebuild_recent(tip+1)` rounds
  (see `tools/blocker8_drive_*.sh`). Packaging that as a self-driving "follow
  zclassicd" service is the operational path to a live sync, once coins_best
  follow makes it restart-durable.

---

## Why a surgical coins_best-follow is DEAD — two adversarial NO_GOs (2026-06-06)

Two independent design+adversarial-review workflows (`wj65w0x4n`, `wf_813bb6be`)
both returned **BLOCKED_NO_GO** on every "advance coins_best to follow the reducer
tip" design. The verdicts converge on one structural truth, verified against
source:

- **There are TWO separate UTXO engines.** The reducer fills ONLY
  `utxo_projection` (utxo.db); **no `app/jobs/*` stage ever writes the coins.db
  `utxos` table** (verified by grep). The legacy coins.db (`utxos` rows +
  `best_block`) is written ONLY by `coins_view_sqlite_batch_write_ex` on the
  connect_block/process_block path.
- **coins_best is meaningless without its matching `utxos` materialization.**
  `coins_view_sqlite_check_tip_consistency` (lib/storage/src/coins_view_sqlite.c:448,
  gated at :706) **FATALs (DB_ERR_TIP_MISMATCH)** at coins-DB-open if
  `coins_best` names a height above `MAX(utxos.height)` — and it runs BEFORE the
  chain_state_validator. Advancing coins_best ahead of the (reducer-unfilled)
  coins.db utxos table hard-halts the node.
- **Multi-surface coins_best.** node.db `coins_best_block` (csr) /
  `cec.coins_best_block_height` (cec) / coins.db `best_block` row / in-memory
  `g_coins_tip.hash_block` are FOUR surfaces with different writers + lifetimes.
  The boot validator + restore read the coins_view_cache/coins.db surface, not
  the surface a bridge would write.
- **Cross-DB tear is structural for any reducer stage.** `stage_run_once` wraps
  every stage `step()` in a progress.kv `BEGIN IMMEDIATE … COMMIT`
  (lib/util/src/stage.c:316–347). A coins_best write inside a step lands in that
  open txn and commits non-atomically with the cursor — the exact "TEAR A" the
  trailing-stage design tried to avoid, merely relocated.

**Conclusion — the correct fix is the SINGLE-ENGINE CUTOVER, not a bridge.** Make
the reducer's `utxo_projection` the authoritative coins view end-to-end: give the
projection a real `get_best_block` keyed off the durable tip_finalize finalized
tip (today it hard-returns NULL — coins_view_projection.c:37-45), and flip the
boot validator / restore / tip-consistency gate to read the projection's tip
instead of the coins.db `utxos` surface (the projection is ALREADY the runtime
consensus read model — bound at config/src/boot.c:1795). This retires the coins.db
`utxos` table as a second authority rather than keeping two engines in lockstep.
It is the long-planned single-engine work (`docs/work/single-engine-newcode-plan.md`,
steps 6-10) — large, consensus-critical, deploy-gated, proven on a copy
(restart × N kill-seeds → no WIPE; `gettxoutsetinfo == zclassicd`) before any
deploy. Do NOT attempt another surgical coins_best bridge; the adversarial record
above shows why every cut forks or FATALs.

### Cutover obstacle map — verified constraints for the migration (round-3 review `wf_9805dc09`)

A third design+adversarial workflow tried the cleanest possible cutover ("one
edit to `cvp_get_best_block_impl` flips all three boot read-sites via cache
delegation"). Verdict: **BLOCKED_NO_GO**, but it pinned the EXACT obstacles any
cutover must clear. Use these as the migration's spec, not a blank page:

1. **VERIFIED SAFE — the parity surface is already projection-sourced.**
   `rpc_gettxoutsetinfo` (blockchain_controller_chain.c:334) and
   `rpc_getutxocommitment`/utxo_sha3 (:383) read `utxo_projection_get_global()`
   EXCLUSIVELY — no coins.db read. So the byte-exact-vs-zclassicd gate already
   reflects the authoritative set; the cutover only has to make the projection
   *name a tip*, not change what the commitment reads. Lock this with a
   regression test so a refactor can't re-route it to the stale coins.db utxos.

2. **Site A (`coins_view_sqlite_check_tip_consistency`, coins_view_sqlite.c:448-573,
   gated boot.c:1719) reads coins.db DIRECTLY** (`SELECT MAX(height) FROM utxos`
   :455; `coins_best_block` from node_state :469) — it never touches the
   projection vtable. A `get_best_block` change does NOT flip it; the FATAL gate
   must be authority-flipped in its own edit (allow coins.db stale-behind when a
   `tip_finalize_log` finalized tip outranks it; keep FATAL only for coins.db
   utxos max_height ABOVE the finalized tip = genuine corruption).

3. **The shared `coins_view_cache` MEMOIZES** the first `get_best_block` into
   `c->hash_block` (coins_view.c:202-204), and it is shared across the boot gates,
   the projection backing, AND legacy `connect_block`. Two consequences:
   (a) Site C (`utxo_recovery_restore`, boot.c:2571) runs BEFORE Site B
   (`validate_coins_chain_agreement`, boot.c:2734) and calls `csr_commit_tip →
   coins_view_cache_set_best_block`, stamping the coins.db hash and **bypassing
   delegation** for Site B; (b) `connect_block.c:151-162` REJECT_FATALs
   ("connect_block-view-mismatch") if the cached best != the connecting block's
   `hashPrevBlock` — so a cache returning the finalized tip while the coins.db
   view is materialized only to a stale height **halts the climb**. The boot
   gates need a read of the finalized tip that does NOT poison the shared cache
   that `connect_block` depends on.

4. **`connect_block` must be retired as an engine, not just outranked.** Obstacle
   3(b) is the original "coins_best is meaningless without its matching UTXO
   materialization" re-emerging through the cache: as long as the legacy
   connect_block path runs with the coins.db view, advertising the reducer's tip
   to it FATALs. The migration must make the reducer the ONLY block-connect
   engine (single-engine purge), not run both.

5. **Default-boot path caveat:** Case-3 AGREE cannot assume the active tip was
   seeded from the tip_finalize cursor — `rebuild_seed_tip`
   (block_index_loader_rebuild.c:130-170) only runs on `-rebuildfromlog`; a normal
   restart uses `load_block_index_flat/sqlite`. And a finalized tip present in
   `tip_finalize_log` but not yet folded into the in-memory block map at the gate
   moment can make `block_map_find(coins_best)` NULL → a reachable WIPE path.
   Ordering + map-presence must be proven on a copy.

**Conclusion:** the cutover is the FULL single-engine migration (steps 6-10 of
`single-engine-newcode-plan.md`), executed as sequenced, individually-copy-proven
steps — give the projection a durable tip source, authority-flip Site A, route the
boot gates to the finalized tip WITHOUT poisoning the connect_block cache, and
retire the legacy connect_block engine — each proven on a datadir copy
(restart × N kill-seeds → no WIPE; `gettxoutsetinfo == zclassicd`) before the
owner-gated deploy. It is a dedicated effort, not a one-file edit.

---

## The five blockers (all verified against source + a full datadir copy)

1. **Header-solution gap.** ~676K node.db rows have empty Equihash solutions
   (header-only fast-import legacy); the in-RAM block_index also drops nSolution.
   validate_headers can't PoW-verify them and fails
   `no-header-solution-backfill-required`
   (`app/jobs/src/validate_headers_validator.c:128,143`). For heights above the
   persisted node.db tip the ONLY solution source is the progress.kv
   `header_solution_repair` side-table (source #1 `header_from_repair_table`).
   **FIX-1 — LANDED `1bc2b1a62`:** `reducer_ingest_service.c` reducer_ingest_block
   saves the check_block-verified solution to the repair table on every
   full-block ingest. PROVEN on copy: validate_headers caught up 3134303→3137724.
   Consensus-safe (3-lens review GO): hash-bound save + independent re-verify on load.

2. **Latched validate_headers failures.** The stage cursor is forward-only and
   sat at 3,137,669 (past the gap) with ok=0 rows, so a freshly-supplied solution
   is never re-checked. The self-heal Condition `stale_validate_headers_repair`
   exhausted its 5 attempts (its supply step `header_probe_pull_range` doesn't
   populate gap solutions) and latched at operator_needed.
   **Cleared by** `stage_repair_header_solution_poison_rewind`
   (`app/jobs/src/stage_repair_rewind.c:188`) — frontier-only (height==active_tip+1),
   deletes validate_headers_log + downstream logs ≥ height, rewinds those
   PERSISTED cursors, REFUSES if any finalized ok=1 row sits at/above the
   frontier (Tier-2 public-tip floor). FIX-2 (rebuild_recent rewind-retry,
   uncommitted) drives it; the cleaner design is a single frontier rewind.

3. **tip_finalize lookahead cursor-advance latch.** To finalize H, tip_finalize
   requires the LOOKAHEAD block H+1 to have BLOCK_HAVE_DATA AND BLOCK_VALID_SCRIPTS
   (`tip_finalize_stage.c:151-160`). On `precondition_failed` it writes an ok=0
   row and **advances its cursor anyway** (`tip_finalize_stage.c:344-345`:
   `c->cursor_out = c->cursor_in + 1; return JOB_ADVANCED`), stranding H.
   `anchor_cursor_to_authority` is MONOTONIC (`tip_finalize_stage.c:109`) so even
   a restart can't pull it back; the boot re-floor
   `stage_reconcile_clamp_tip_finalize_to_floor` is wired at `config/src/boot.c:3337`
   (restart-only). This is the live **3134304→3134302 oscillation**
   (reorg_detected_total≈124422).
   **CODE2 (consensus-critical):** on a LOOKAHEAD precondition_failed return
   JOB_IDLE/JOB_BLOCKED instead of JOB_ADVANCED so the frontier retries once the
   successor lands.

4. **UTXO-set hole (dispositive).** node.db `utxos` MAX(height)=**3,132,687**
   while tip=3,134,303 (1,616-block hole; verified on copy: count=1,344,623,
   maxh=3132687). created_outputs forward index is empty for the gap. Re-wedges at
   the first transparent-spend block (~3,134,341) because the prevout it spends
   was created inside the hole and isn't in the UTXO set. Filling it requires
   re-applying 3,132,688..3,134,303 (bodies ARE on disk in a full datadir) — i.e.
   rewinding utxo_apply below the hole and re-draining, gated against zclassicd
   gettxoutsetinfo. DEEPEST / highest-risk piece.

5. **utxo_apply g_lookup==NULL in production.** `utxo_apply_stage.c:52`
   `g_lookup=NULL`, only set by `utxo_apply_stage_set_lookup` (test-only callers);
   reset to NULL at `:382`. With it NULL, `utxo_apply_delta.c:178-195` rejects
   EVERY transparent spend as `spend_unknown_utxo`. script_validate already
   self-defaults (`script_validate_stage.c:562-563`
   `if (!g_prevout) g_prevout = created_index_prevout`); utxo_apply does not.
   Never hit before because the pipeline never reached utxo_apply with a real
   spend (wedged upstream).
   **CODE1:** init-time default g_lookup mirroring script_validate's default.

## Cleanest design (workflow synthesis, two_fix_sufficient=NO)

- **CODE3** (additive/safe): bulk-fill `header_solution_repair` for
  [persisted_tip+1 .. header_tip] from zclassicd's `~/.zclassic/blocks/index`
  LevelDB — `blocks_index_legacy_reader.c` already deserializes nSolution
  (`block_index_db.c:157-164`); extend it to call
  `stage_repair_header_solution_save` (hash-bound, idempotent). Then a restart
  lets `validate_headers` recheck_failed_rows flip all 634 latched ok=0 → ok=1
  with no per-block rewind.
- **CODE1** (consensus): default utxo_apply g_lookup.
- **CODE2** (consensus): tip_finalize lookahead precondition_failed → IDLE/BLOCKED,
  not ADVANCE.
- **CODE4**: single frontier poison_rewind (replace fix-2 per-block crawl) — see
  Trace-3 two-phase rebuild (prefill solutions, then one rewind + drain).
- Bodies: rebuild_recent (getblock verbose=0 from zclassicd) — REBUILD_RECENT_MAX_RANGE
  =5000, the ~3,417-block forward gap fits one window. P2P body source is gated
  out (anti-eclipse floor min 3, only 2 healthy; mirror rpc-unreachable) — so
  rebuild_recent from zclassicd is the only body source. NEVER weaken the peer
  floor or the poison_rewind frontier gate.
- UTXO hole (blocker 4): needs utxo_apply rewound below 3,132,688 and re-drained;
  gate vs zclassicd gettxoutsetinfo.

## Implemented (2026-06-06, union-gate GREEN: build + lint 35 + test_parallel 0/372)

The UTXO-hole (blocker 4) was REJECTED as a red herring — the reducer read model
utxo_projection.db has NO hole (verified on copy: MAX(height)=3,134,301, 1,507
in-gap coins); only the off-path legacy node.db.utxos table is holed. So NO
re-author. The real fix is six changes (CODE1-4 + anchor-skip), all landed in the
working tree, adversarially reviewed:

- **CODE1** (`utxo_apply_stage.c`): default `g_lookup` to `projection_live_lookup`
  — resolves a prevout to its LIVE (delete-on-spend) coin from utxo_projection
  (NOT the created index, which would accept double-spends). PLUS a per-step
  `utxo_projection_catch_up` in `utxo_apply_stage_step_once` after JOB_ADVANCED so
  a coin created earlier in the same drain is visible to a later block's spend.
  PROVEN: utxo_apply crossed block 3,134,341 (the first transparent-spend block)
  up to 3,137,761 on a copy — the dispositive blocker is solved.
- **CODE2** (`tip_finalize_stage.c`): lookahead precondition_failed (transient
  have_data_missing/not_script_valid) → JOB_IDLE (hold, no cursor move), instead
  of writing ok=0 + advancing (which stranded the height — the 3134304↔3134302
  oscillation). chainwork_not_greater kept as ADVANCE (terminal). New
  `successor_pending_total` counter.
- **CODE3** (`repair_controller_rebuild.c`): new `backfill_header_solutions(from)`
  RPC — bulk-fills header_solution_repair from zclassicd getblock; additive,
  hash-bound, idempotent, span-capped.
- **CODE4** (`repair_controller_rebuild.c`): single FRONTIER poison_rewind in
  rebuild_recent (fires once, height==active_tip+1) + continue-on-backfill so the
  loop keeps supplying bodies while validate_headers re-drains async.
- **anchor-skip (6th blocker)** (`tip_finalize_log_store.{c,h}` + `tip_finalize_stage.c`):
  `finalized_row_active_match` now skips status="anchor" rows. Anchor rows store
  hash(H) (the tip seed's own hash) while finalized rows store hash(H+1) (the
  lookahead convention); comparing an anchor's hash(H) to active_chain_at(H+1)
  ALWAYS mismatched → false reorg → rewound the cursor onto the seed forever.

## PROVEN ON COPY — the tip crosses the wedge and climbs (2026-06-06)

Procedure on a full datadir copy (NON-isolated so it finds peers and the no-peer
self-heal does not exit it): boot → `backfill_header_solutions` → `rebuild_recent
(3134304)`. RESULT: the public tip climbed **3,134,303 → 3,135,048+** and kept
going, node alive; `utxo_apply` crossed block 3,134,341 (first transparent spend),
`tip_finalize` advanced past the seed (3134304 → 3134663 → …) in bursts. All six
fixes carry forward end-to-end.

- The earlier "blocker 7" (tip_finalize have_data_missing) was a MISREAD — it is
  transient: tip_finalize correctly HOLDS (CODE2 JOB_IDLE) until each lookahead
  body lands, then advances. The anchor-skip fix was the real final piece (the
  false reorg had been rewinding the cursor onto the seed forever).
- The "node-exit" was purely a TEST ARTIFACT of `-connect=<dead>` isolation: with
  0 peers the boot self-heal exits the node ("Connections 0", supervisor stall).
  Non-isolated, the node stays up and the pipeline drains.

## REMAINING before live deploy (owner-gated)

- **Blocker 8 (the real stability gap) = SINGLE-ENGINE coins_best follow (task #2).**
  The reducer pipeline advances its authoritative tip (tip_finalize → 3,135,202 in
  the proof) and authors utxo_projection forward, but the LEGACY `coins_best` /
  node.db.utxos stays at 3,132,687 — it never follows. The growing divergence trips
  `chain_integrity_failed` (app/conditions/src/chain_integrity_failed.c) and the
  node destabilizes/exits after climbing ~900 blocks. So the wedge fix advances the
  tip CORRECTLY, but the node can't STAY up until coins_best tracks the reducer tip.
  This is the deploy-gated single-engine cutover (make the reducer tip authoritative
  over coins.db, or have utxo_apply/tip_finalize advance coins_best). MUST land with
  the wedge fix for a stable live recovery.
- **Full convergence + §3 byte-exact gate**: once coins_best follows, run the copy
  to ~3,137,787 (zclassicd's tip); gettxoutsetinfo / utxo_sha3 must match zclassicd
  (diverge by one coin → STOP).
- **Performance**: the per-step `utxo_projection_catch_up` (CODE1 freshness) folds
  the event log every advancing block — O(events) per step — the likely cause of
  the ~1-3 blk/s climb. Consider a bounded/incremental catch_up or a periodic fold
  once correctness is locked. Functional first, then this.

## Status
- FIX-1 `1bc2b1a62`; CODE1-4 + anchor-skip `345114d06` — union-gate green, now
  PROVEN to advance the tip across the wedge on a copy. Deploy still owner-gated:
  run to full convergence + the byte-exact gate on the copy, THEN `make deploy`.
- Design + adversarial review: workflows `wokek1cfg` (trace) + `w75jh8zx4` (design).
- Repro: `repro_on_copy --full` → 3,134,303; `backfill_header_solutions`; then
  `rebuild_recent(3134304)`. Do NOT P2P-isolate (no-peer self-heal exits).
