# Commitment-audit wedge — decision doc + cure (2026-06-29)

**Status:** FINAL — implement directly from this.
**Wedge:** live node `~/.zclassic-c23-fullhist` (build `6b9fa108c`) frozen ~10h
at `H*=3164075`, header tip `3164549` (474 behind), `operator_needed=true`.
**Class:** projection-audit HOLD falsely freezing the PoW-proven tip — a REMEDY
defect, not a detector defect. Zero consensus-code change.
**Scope:** 3 files — `app/services/src/invariant_sentinel.c`,
`app/conditions/src/state_window_inconsistent.c`,
`lib/coins/src/utxo_commitment.c` (shared helper).

---

## 1. CONFIRMED ROOT CAUSE

A 0.0007% UTXO-count drift against a frozen out-of-band checkpoint is latching a
chain-linkage HOLD that refuses the PoW-proven tip move, and nothing can clear
it. The D-chain, with decoded live numbers (checkpoint count **1,344,871** vs
live `utxos` rows **1,344,880** = **+9 growth**):

- **D1 — wrong oracle.** The commitment audit
  (`invariant_sentinel.c:512` `invariant_sentinel_commitment_audit_once`)
  recomputes an O(n) XOR digest over the **`utxos` table**
  (`utxo_commitment.c:134` `utxo_commitment_compute_db` → `SELECT txid,vout,value,height
  FROM utxos`) and compares its **count** to a stored `node_state('utxo_commitment')`
  checkpoint (`invariant_sentinel.c:528,553`). On this build `utxos` is a
  **rebuildable projection** of the `coins_kv` authority, wholesale-rebuilt by
  `utxo_mirror_sync_service.c` (`DELETE FROM utxos` + reinsert) **without** ever
  co-committing a checkpoint — so the audit's load-bearing comment
  (`invariant_sentinel.c:531-535`, "every utxos flush co-commits a fresh checkpoint
  in the same txn") is **FALSE on the live writer path**. The checkpoint is written
  only out-of-band at boot/recovery (`reindex_epilogue.c:124`,
  `utxo_recovery_service.c:478`). Result: while `coins_kv` tracks the tip the
  checkpoint stays frozen and `computed > saved` is the **expected benign state**.

- **D2 — the HOLD raise.** When the classifier
  `utxo_recovery_xor_mismatch_is_corruption_candidate`
  (`utxo_recovery_service.c:207-211` = `return computed_count <= saved_count`)
  returns true, the audit raises a diagnostic blocker AND a chain-linkage HOLD
  (`invariant_sentinel.c:584-587` `chain_linkage_hold_set("commitment_audit", tip+1,
  ...)`). That HOLD is enforced at `chain_linkage_check.c:264-281` →
  `active_chain_move_window_tip` returns false (`chainstate.c:406`) →
  `tip_finalize_stage.c:728-739` returns **JOB_FATAL** → the framework rolls back
  the txn → the just-inserted `finalized` row for `3164076` is discarded → **H*
  never advances** (error_count ~15088 and climbing; live log
  "[validation_pack] HOLD refused tip move h=3164077 ... repeats=15108").

- **D3 — the clear asymmetry (why it never self-releases).** The ONLY production
  clear of this hold is gated on **exact** `saved==computed`
  (`invariant_sentinel.c:553-562`). The growth/stale branch
  (`invariant_sentinel.c:565-570`) does a bare `return true` **without clearing**.
  The classifier fires on `computed<=saved` but the clear needs `computed==saved`
  — so a HOLD latched during the earlier negative-drift transient (6 mismatches
  fired when drift flipped negative — consistent with the audit's O(n) scan racing a
  mirror `DELETE`+reinsert window the torn-scan guard at `:547-551` misses) is now
  **stranded permanently** by the +9 growth.

- **D4 — no auto-owner.** The owning condition
  `state_window_inconsistent` detects the `coins.commitment_spot_check` blocker
  (`state_window_inconsistent.c:27-28`) but its remedy is a **stub**
  (`state_window_inconsistent.c:31-36` `return COND_REMEDY_FAILED` — the never-wired
  WINDOW_REBUILD seam) → `operator_needed` forever. Meanwhile the consensus
  authority is healthy: `utxo_apply` ok=1 through frontier `3164371`,
  `coins_applied_height=3164372`, `block_index` verdict "ok".

---

## 2. VERDICT — false positive? decoupling consensus-safe?

**YES, false positive. YES, decoupling is consensus-safe** — with two
plan-shaping constraints from the refute phase.

- **It is a benign skew, not table-vs-chain corruption — UPHELD [high].** Every
  path confirms projection-vs-frozen-stamp staleness: `utxos` is a rebuildable
  mirror of `coins_kv` (`coins_view_sqlite.c:470-507`, `utxo_projection.c`); the
  checkpoint is written only out-of-band; and the codebase itself labels growth
  as staleness, not corruption (`utxo_recovery_service.c:204-211` +
  `:469-485` "A mismatch here is a stale checkpoint, not corruption — refresh
  it."). The audit never compares the table to the chain or to `coins_kv` — only
  to a stored count — so it **structurally cannot evidence** table-vs-chain
  corruption.

- **Decoupling from `chain_linkage` is consensus-safe — risk-note #1.** The HOLD
  is explicitly E13-neutral: it gates only OUR local pipeline tip advance, never
  block/tx validity (`tip_finalize_post_step.c:88-90`, `chain_linkage_check.c:5-7`).
  Decoupling touches no Equihash param, activation height, or validity rule, and
  does not alter the `utxo_apply` ok-gate or the keystone. `check-consensus-parity`
  (E13) and `test_consensus_parity` goldens are unaffected.

- **PLAN-SHAPING CLAIM A — "utxo_apply ok=1 is the real global guard" → REFUTED
  [high].** `utxo_apply` ok=1 is a **per-block delta** verdict
  (`utxo_apply_stage.c:507`, `:249-293`), not a global coin-set check, and the
  self-minted keystone (`tip_finalize_post_step.c:216-241`) computes its root FROM
  the node's own coins — self-referential, never compared to an independent value.
  The whole-table XOR audit is the **only** live cross-check of the global coin set
  against an independent number. **Consequence for the plan:** do **not** delete
  the detector or demote it to advisory. Keep it as a non-fatal diagnostic and give
  it a real auto-terminating repair owner.

- **PLAN-SHAPING CLAIM B — "blind resync from the table is safe" → REFUTED
  [high].** Adopting `compute_db(utxos)` unconditionally would, on a **shrink /
  equal-count-different-hash** mismatch, silently bake a coin-loss into the
  checkpoint with no downstream catcher until some future block spends the missing
  coin. **Consequence for the plan:** resync the checkpoint **only in the growth
  direction** (`computed > saved`, the deterministic-mirror-ahead case). The shrink
  / different-hash direction must NOT blind-adopt — it routes to the owner whose
  remedy first re-derives the projection from the `coins_kv` **authority**
  (rebuild) and only then resyncs.

- **Once latched it never auto-clears — UPHELD [high].** Confirms D3: the only
  runtime clear is the exact-equal branch; the one resync precedent
  (`utxo_recovery_service.c:478`) lives in a **boot-only** path that a running
  wedged process never reaches; restart is not an auto-terminating remedy for a
  non-crashing wedged node.

**Net:** keep the detector, sever its power to freeze the PoW-proven tip, make the
benign-growth case self-terminate, and give the corruption-candidate case a
real authority-bound repair owner.

---

## 3. THE CURE — exact ordered edits

Three files. No consensus predicate changes. Helper-first to satisfy DRY.

### EDIT 0 (DRY helper) — `lib/coins/src/utxo_commitment.c`
Add `bool utxo_commitment_resync_from_db(sqlite3 *db, struct utxo_commitment *out_optional)`:
recompute via `utxo_commitment_compute_db`, then `utxo_commitment_save_checkpoint`,
`LOG_INFO` count on success / `LOG_WARN` on save failure, return the save result; copy
the digest to `*out_optional` if non-NULL. Declare it in the `utxo_commitment.h`
public header. Then refactor the two existing call sites
(`utxo_recovery_service.c:476-485`, `reindex_epilogue.c:120-127`) to call it, and use
it from EDIT 1 and EDIT 3. (Save/load/compute signatures confirmed:
`utxo_commitment.c:134,180,203`.)

### EDIT 1 (resync + D3 latch fix) — `app/services/src/invariant_sentinel.c:565-570`
Replace the bare growth/stale `return true` with a handler that BEFORE returning:
1. `utxo_commitment_resync_from_db(ndb->db, NULL)` — adopt the recomputed digest
   (same refresh the boot path performs; safe because `utxos` is a deterministic
   rebuild of the `coins_kv` authority). LOG per helper.
2. UNCONDITIONALLY clear any latched hold + blocker (idempotent clear-if-held):
   ```c
   if (atomic_load(&g_audit_blocker_active)) {
       atomic_store(&g_audit_blocker_active, false);
       blocker_clear("coins.commitment_spot_check");
   }
   chain_linkage_hold_clear("commitment_audit");
   ```
3. `return true;`

This alone clears the LIVE +9-growth wedge: the latched hold releases and the next
audit hits exact-equal (`:553-562`).

### EDIT 2 (DECOUPLE + keep diagnostic) — `app/services/src/invariant_sentinel.c:581-588`
DELETE the `chain_linkage_hold_set` block (`:584-587`):
```c
int64_t tip = tip_finalize_stage_last_height();
if (tip >= 0)
    chain_linkage_hold_set("commitment_audit", (int)tip + 1,
                           "utxo commitment corruption candidate");
```
KEEP `:572-580` (count, `g_audit_blocker_active=true`, `audit_log_range_counts`,
`sentinel_raise_blocker("coins.commitment_spot_check", ...)`) as the NON-FATAL
diagnostic. A persistent shrink/different-hash mismatch now raises only a blocker
routed to the owner — it never refuses a tip move. The exact-equal clear at
`:553-562` stays (drop the now-orphaned `chain_linkage_hold_clear` there only if
EDIT 1 already guarantees the clear runs on every non-equal pass — safer to leave
it; it is idempotent).

*Optional robustness:* gate the raise behind 2 consecutive corruption-candidate
ticks to swallow the mirror-rebuild torn-scan race that produced the earlier 6
false fires (the guard at `:536-551` keys on tip cursor + checkpoint and misses the
`DELETE`+reinsert window).

### EDIT 3 (auto-terminating owner) — `app/conditions/src/state_window_inconsistent.c:31-36`
Replace `return COND_REMEDY_FAILED;` in `remedy_state_window_inconsistent` with:
```c
if (blocker_exists("coins.commitment_spot_check")) {
    /* Re-derive the projection from the coins_kv AUTHORITY, then resync the
     * checkpoint to the authority-faithful digest. Never blind-adopt the
     * suspect table. */
    if (g_utxo_mirror_sync) {
        int64_t rows = utxo_mirror_sync_run_once(g_utxo_mirror_sync);
        if (rows < 0) return COND_REMEDY_FAILED;   /* logged; surfaces */
    }
    if (g_utxo_mirror_sync && g_utxo_mirror_sync->ndb &&
        g_utxo_mirror_sync->ndb->db) {
        if (!utxo_commitment_resync_from_db(g_utxo_mirror_sync->ndb->db, NULL))
            return COND_REMEDY_FAILED;
    }
    return COND_REMEDY_OK;   /* witness must confirm the blocker self-cleared */
}
/* window.consistency: keep self-clearing-sweep semantics. */
return COND_REMEDY_SKIP;
```
The witness (`:38-48`, `!detect`) confirms the blocker actually cleared; if it
persists the engine downgrades to `COND_REMEDY_UNWITNESSED` (`condition.h:21-32`) —
no false green. Update the file header (`:14-17`) to delete the "operator owns the
repair" line. Set `cooldown_secs=300` and `cooldown_max_rearms` > 0 on
`c_state_window_inconsistent` (`:50-60`, fields at `condition.h:84-85`) so a
slow-converging repair re-arms instead of latching `operator_needed` permanently
(sticky-node plan #7).

**Concurrency note (EDIT 3):** the remedy runs on the condition-engine thread;
`utxo_mirror_sync_run_once` and the 5s background mirror thread both write `utxos`
under `ndb`. Do NOT issue a second concurrent `node_db_begin` on the same `ndb` —
`utxo_mirror_sync_run_once` is already self-serializing/self-logging and idempotent
(REPLACE), so call it as the single rebuild entry; do not add a parallel begin.

### KEEP UNCHANGED (structural holds stay enforcing)
`chain_linkage_check.c:236` (`linkage` pprev-splice), `tip_finalize_post_step.c:110`
(`coinbase_label`), `invariant_sentinel.c:439` (`window_sweep`),
`mirror_divergence_locator.c:272` (`mirror_divergence`). They share the enforcement
point `chain_linkage_check_advance` — **do not** touch `chainstate.c:406`; the
decouple is at the RAISE site only. (Removing `commitment_audit` as a hold caller
also fits the 5 check_ids into `CHAIN_HOLD_SLOTS=4`, `chain_linkage_check.h:49`.)

---

## 4. TESTS to add/change

1. **GROWTH+LATCH (pins EDIT 1):** seed checkpoint count `N`, populate `utxos` with
   `N+9` rows, pre-latch `chain_linkage_hold_set("commitment_audit", tip+1, ...)` +
   `g_audit_blocker_active` + raise `coins.commitment_spot_check`. Run
   `invariant_sentinel_commitment_audit_once()`. Assert: returns true;
   `chain_linkage_hold_active()==false`; `blocker_exists("coins.commitment_spot_check")
   ==false`; stored checkpoint count now `N+9`.
2. **DECOUPLE (pins EDIT 2):** force shrink / different-hash mismatch. Assert
   `chain_linkage_hold_active()==false` ALWAYS for the commitment audit, tip moves
   not refused, at most a non-fatal diagnostic blocker raised.
3. **STRUCTURAL-HOLD REGRESSION:** a label-splice (`chain_linkage_check_advance`
   with `bi->pprev->nHeight != bi->nHeight-1`) STILL returns false and raises
   `chain.linkage_violation` — proves only the commitment audit was decoupled.
4. **OWNER (pins EDIT 3):** raise `coins.commitment_spot_check`, drive
   `remedy_state_window_inconsistent()` with a stub mirror that resyncs the
   checkpoint; assert `COND_REMEDY_OK` then witness `!detect`, no
   `operator_needed_emitted`. Negative: if resync does NOT clear, assert engine
   records `COND_REMEDY_UNWITNESSED`, never a silent ok.
5. **tip_finalize integration:** with a commitment mismatch present, assert
   `tip_finalize_stage_step_once` advances past `next_h` and durably inserts the
   finalized row (no JOB_FATAL from a commitment hold).
6. Update `test_invariant_sentinel.c:282-360` "check 5" only where it asserts the
   growth branch leaves a hold latched (it asserts `blocker_exists` only today, so
   EDIT 2 keeps it green; add the new growth-clears-hold assertion). `test_mcp_controllers`
   enum_csv assertion is unaffected (no condition/blocker list change).
   `test_mirror_divergence_locator.c`, `test_chain_linkage_check.c` unaffected
   (structural ids — regression floor).

---

## 5. LINT / PARITY notes

- **Consensus parity (E13) — unaffected.** No consensus predicate changes; the XOR
  audit is a zcl23-only node-local heuristic with no `zclassicd` analogue.
  `utxo_apply` ok-gate and the keystone are untouched.
- **AR lifecycle:** the resync writes through `utxo_commitment_save_checkpoint`
  (already `AR_STEP_WRITE`, `utxo_commitment.c:195`) — no raw `sqlite3_step`
  introduced. EDIT 3 reuses `utxo_mirror_sync_run_once`'s existing begin/commit +
  annotation; adds none.
- **LOG_FAIL/LOG_ERR (every error return logs context):** the EDIT 0 helper
  LOG_INFOs success / LOG_WARNs failure. EDIT 3 must `return COND_REMEDY_FAILED`
  (not OK) when the rebuild or resync fails, so a real failure still surfaces.
- **Blocker lifecycle:** `blocker_clear` / `chain_linkage_hold_clear` are idempotent
  clear-if-held; keep `g_audit_blocker_active` consistent with the blocker so the
  PAGE-once gate in `sentinel_raise_blocker` is not re-armed spuriously. The
  `k_pack_blockers` health list (`invariant_sentinel.c:654-662`) is unchanged —
  `coins.commitment_spot_check` stays tracked but now non-fatal/self-clearing.
- **Honest-witness (condition.h:21-32):** the owner remedy is credited ok only when
  the symptom actually clears; keep `honest-witness-ok` annotations
  (`state_window_inconsistent.c:41`, `tip_label_divergence.c:45`,
  `chain_linkage_check.h:31,52`) intact.
- **Lint annotation:** keep `// one-write-path-ok:reducer-tip-authority`
  (`tip_finalize_stage.c:729`) — the move_window_tip call/return handling is
  unchanged.
- **Supervisor Gate #15:** the audit/sweep children
  (`invariant_sentinel.c:635-649`) are unchanged.
- **DRY (Law 8):** EDIT 0 removes the existing `compute_db → save_checkpoint`
  duplication across `utxo_recovery_service.c:476-485` and
  `reindex_epilogue.c:120-127`.

---

## 6. COPY-PROVE GATE + DEPLOY

**Copy-prove (never live surgery):**
```
cp -a ~/.zclassic-c23-fullhist <isolated-copy-on-free-disk>
build/bin/zclassic23 -datadir=<copy> -port=8123 -rpcport=18332 \
    -load-snapshot-at-own-height
```
(Never two copy-proves on the same datadir/ports — exit 144. No `-nolegacyimport`
contamination needed; the copy already has bodies.)

**PASS (gate on H* CLIMB, not "booted without FATAL"):**
- `reducer_frontier` H* (via `zcl_state subsystem=reducer_frontier` + `getblockcount`)
  CLIMBS strictly **past 3164077** and continues toward header tip `3164549`; AND
- within the first audit cycle `chain_linkage_hold_active()==false` and blocker
  `coins.commitment_spot_check` ABSENT (`zcl_blockers`); AND
- `coins_applied_height` never regresses below its start (stays ahead of H*); AND
- no `JOB_FATAL "[tip_finalize] chain_active set_tip failed"` recurs in node.log;
  AND the `[validation_pack] HOLD refused tip move` warnings STOP; AND
  `error_count` stops climbing. Hold the climb for a sustained window.

**FAIL:** H* stays pinned at 3164075/3164076, OR the commitment hold re-latches, OR
any FATAL/`operator_needed` re-arms, OR error_count keeps climbing. `test_parallel`
green is only a regression floor, not a liveness proof.

**Deploy (only after the copy proves H* climbs past 3164077 and holds):**
```
systemctl --user stop zclassic23           # stop the live node
cp build/bin/zclassic23 ~/.local/bin/zclassic23-live
systemctl --user start zclassic23
```
Save rollback assets first. `getblockcount=0` for tens of seconds after a fast
restart is expected. node.log is `~/.zclassic-c23/node.log`.
