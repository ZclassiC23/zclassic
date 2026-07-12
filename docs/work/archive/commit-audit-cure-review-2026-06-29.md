# Commitment-Audit Wedge Cure — Deploy Decision (2026-06-29)

Subject: the commitment-audit decouple + auto-terminating-owner cure
(EDIT 1 growth-branch checkpoint resync, EDIT 2 chain_linkage HOLD removal,
EDIT 3 clear-on-growth latch fix + 2-candidate streak gate + thread-safe
owner hook + new `test_invariant_sentinel` check 5).

Inputs: 3 review lenses (correctness / concurrency / consensus-safety) +
the class-wide halt audit. Code spot-checked at
`app/services/src/invariant_sentinel.c:540-641` and
`app/services/include/services/invariant_sentinel.h:31-43`.

---

## 1. Decision: **GO**

Deploy to the live node.

**Single most important reason:** the change strictly converts a
consensus-safe-but-liveness-fatal misfire into a non-fatal, auto-terminating
diagnostic. The old behavior let an hourly XOR audit of a *rebuildable
projection* (`utxos`) against a *forgeable self-minted XOR checkpoint*
raise a `chain_linkage` HOLD, which made `tip_finalize` JOB_FATAL-roll-back
an already-inserted, PoW-proven finalized row — pinning H\* with no
auto-remedy (the live 3164076 wedge). The cure stops touching
`chain_linkage` and leaves only a non-fatal blocker owned by an
auto-terminating condition. The **real consensus gates are untouched** by
this diff: the per-block fold ok-verdict (`utxo_apply` → `tip_finalize
upstream.ok`), the SHA3 full-set commitment, every E13 predicate, the four
structural `chain_linkage` holds (linkage / coinbase_label / window_sweep /
mirror_divergence), and JOB_FATAL enforcement. All three lenses
independently conclude safe-to-deploy; **there is no critical or high
finding** — the strongest concern in any lens is medium, and every medium is
either doc-accuracy or a cheap, self-healing concurrency nicety. A projection
audit that only ever compared a rebuildable table to a self-minted cache
could never evidence a genuinely divergent/forked tip, so removing its teeth
cannot let a bad tip finalize.

This is a net liveness improvement (unwedges 3164076) with zero consensus
downside.

---

## 2. MUST-FIX (bundle into the same build before live)

Neither item is wedge- or consensus-blocking on its own — the node is safe
without them — but you are rebuilding to deploy anyway, both fixes are a few
lines, and both directly protect the *generalization* (this function is the
designated template for `linkage` / `coinbase_label`, so a lying contract
and a sloppy new race get copied).

### MF-1 — Serialize the new growth-branch checkpoint write under `progress_store_tx_lock`
`invariant_sentinel.c:596` (`utxo_commitment_resync_from_db` →
`utxo_commitment_save_checkpoint`).

This is the one **genuinely new race** the diff introduces and the only
finding flagged by all three lenses. The growth branch now WRITES `node.db`
from the audit/supervisor-drive thread; that connection is shared
(`SQLITE_OPEN_FULLMUTEX`) and is also written by the 5s `utxo_mirror_sync`
rebuild (`BEGIN; DELETE FROM utxos; reinsert ~1.3M; COMMIT`), which the
project's own invariant says is serialized under `progress_store_tx_lock`.
An unlocked autocommit `INSERT OR REPLACE` silently joins whatever foreign
transaction is open, so the "XOR checkpoint resynced" INFO can be a lie when
the write is rolled back with the mirror's batch, and a dirty mid-rebuild
read can manufacture spurious shrink candidates.

Reconcile the two lenses' fixes — they only *appear* to conflict:
- **DO** wrap in the mutex: `progress_store_tx_lock(); (void)utxo_commitment_resync_from_db(ndb->db, NULL); progress_store_tx_unlock();`
- **DO NOT** wrap in `node_db_begin` — the concurrency lens is correct that a
  nested `BEGIN` raises "cannot start a transaction within a transaction".
  Keep the SQL bare; serialize with the lock only.

This restores the documented serialization invariant the sweep's
atomic-snapshot reasoning depends on, kills the lost-write/lying-log, and
removes the dirty read. Net impact: benign → correct.

> Skeptic's note: the concurrency lens rates this LOW and self-healing, and
> it is right that the node cannot crash/corrupt/wedge without the fix. If
> the build is already cut and the node is wedged *right now*, deploying to
> unwedge takes priority and MF-1 follows in the immediate next build. It is
> "must-fix-in-this-PR," not "must-fix-or-the-node-breaks."

### MF-2 — Fix the contradicting / stale contract comments
`invariant_sentinel.h:31-43` and `:139`; `invariant_sentinel.c:554-558`.

The header still documents the *removed* behavior — "co-committed with every
coins flush" (line 34) and "blocker + HOLD + PAGE" (line 43) — and the
torn-scan guard comment at 554-558 ("every utxos flush co-commits a fresh
checkpoint in the same txn") **directly contradicts** the new growth-branch
comment at 584-587 ("never co-committed by the live coins_kv mirror
writer"). Two comments in the same file giving opposite answers to "is the
checkpoint co-committed?" is a correctness-of-intent hazard: a future agent
who trusts the header could re-couple the audit to `chain_linkage`
(reintroducing the 3164076 wedge) or strip the streak gate (reintroducing
the false fires).

Resolution (both comments are individually true of *different* writers):
`coins_view_sqlite` co-commits the checkpoint only on the **boot/recovery**
flush; the **steady-state `utxo_mirror_sync` rebuild does NOT** rewrite the
checkpoint — which is exactly why the checkpoint-changed torn-scan witness
cannot catch a mirror rebuild and the **2-consecutive streak gate** is the
only protection against that race. Rewrite the header check-5 block to:
non-fatal diagnostic blocker only, DECOUPLED from chain_linkage/finalize,
2-consecutive-candidate streak gate, growth resyncs the checkpoint; update
`:139` to "(reads node.db; resyncs the derived XOR checkpoint on a growth
pass — a write)"; scope the 554-558 guard comment to the boot/recovery
co-committer and cross-reference the streak gate.

---

## 3. SAFE-TO-DEFER (track, not deploy gates)

- **Re-add the dropped equal-count/different-hash test sub-case**
  (`test_invariant_sentinel.c` check 5). The classifier still flags
  equal-count as a candidate (`utxo_recovery_service.c:210`) but the diff
  *removed* the only test exercising it. Cheap, high-value; do it in this PR
  if possible. Not a runtime gate.
- **The audit is now a silent self-certifying check** — it overwrites its
  baseline with the current (possibly corrupt) set on every growth and
  auto-clears the blocker every episode, so it never reaches `max_attempts`
  and never pages. This is an accepted defense-in-depth reduction of an
  already-weak detector (XOR root is forgeable; baseline self-resyncs);
  integrity rests on the untouched fold ok-verdict + SHA3 commitment. If a
  durable detector is later wanted, audit the projection against the
  **coins_kv authority count/root** (true ground truth), not a self-resynced
  out-of-band checkpoint, and page after N episodes. **Do not blindly copy
  the "self-resync your own baseline + clear forever" half of this cure to
  linkage/coinbase_label** (see §4) — it works here only because `utxos` is a
  rebuildable projection whose integrity lives elsewhere.
- **Remedy clears the signal but does not itself kick a projection rebuild**
  (`state_window_inconsistent.c:48-58`). A genuine *persistent* projection
  corruption would flap (re-raise/re-clear) with no page; in practice
  `utxo_mirror_sync` / `mirror_divergence_locator` rebuild `utxos` from
  coins_kv independently. Optional: pair the clear with a mirror resync kick,
  or expose a throttled `g_commitment_autoclears_total` WARN so a sustained
  flap is observable.
- **Streak resets non-strictly** — skip/discard early returns
  (`invariant_sentinel.c:545,551,570-574`) don't reset the streak, so "2
  consecutive" isn't strictly consecutive. Minor on a non-fatal auto-cleared
  fire; either reset on discard or amend the comment.
- **Flag-coupled clear** (`audit_clear_latched_blocker` early-returns on
  `!g_audit_blocker_active`) — currently sound (line 612 is the sole raiser
  and sets the flag first) but fragile and opens a sub-µs desync window.
  Hardening: make the clear unconditional (all sub-calls are idempotent
  no-ops), keep the flag as a mirror only.
- **Nits**: null-guard logging at `utxo_commitment.c:228`; the dead
  `chain_linkage_hold_clear("commitment_audit")` belt-and-suspenders call
  (nothing raises that id post-EDIT 2 — drop it or comment it as
  pre-upgrade-residual only).

---

## 4. The generalization — next halt sources for the auto-owner treatment

The exact defect class — **a detective heuristic that latches a
`chain_linkage` HOLD refusing PoW-proven forward tip moves, with no
auto-terminating owner** — has four more members. Prioritized:

### P0 — same dead owner, freezes a PoW-proven tip, NO clear path exists
1. **`linkage`** — `lib/validation/src/chain_linkage_check.c:236`. Internal
   pprev->nHeight label-consistency heuristic (explicitly E13-neutral).
   HOLD + permanent blocker + one page. Owned by the **dead**
   `tip_label_divergence` condition (`COND_REMEDY_FAILED`, no cooldown →
   latches operator_needed forever). No `chain_linkage_hold_clear("linkage")`
   exists outside test reset.
2. **`coinbase_label`** — `app/jobs/src/tip_finalize_post_step.c:110`. BIP34
   embedded-height vs our nHeight label, E13-neutral, fires *after* the tip
   already finalized — same "freeze a PoW-proven tip on an internal label
   mismatch" shape as the commitment audit. Same dead
   `tip_label_divergence` owner. No clear outside tests.

   **Treatment (verbatim commitment-audit template):** decouple from
   `chain_linkage` — an internal label mismatch must NOT refuse a PoW-proven
   tip — OR give `tip_label_divergence` a real auto-terminating remedy:
   `window_rebuild(refuse_from)` that **re-derives the index/labels from the
   canonical bodies**, calls `chain_linkage_hold_clear` on witnessed success,
   and re-arms on cooldown instead of latching `COND_REMEDY_FAILED`.
   **Caveat (do not over-copy):** unlike the commitment case, the cure here
   must be an *actual repair* (bodies re-derive), NOT a "overwrite the
   baseline with current state + clear forever" self-resync — a label
   self-resync would mask a genuine block-index label corruption. Copy the
   decouple + `COND_REMEDY_OK`-on-witnessed-repair shape; pair it with a real
   bodies-re-derive, not a baseline overwrite. Model to mirror:
   `block_failed_mask_at_tip` (remedy runs `process_block_revalidate`,
   returns OK on recovery).

### P1 — same class, softer (a seam exists but the repair owner is a stub / contingent)
3. **`window_sweep`** — `app/services/src/invariant_sentinel.c:446`. Has a
   self-clear seam (later CLEAN sweep, lines 464-480) and cooldown re-arm so
   it won't page forever, BUT the `window.consistency` arm's remedy is still
   `COND_REMEDY_FAILED` ("no automated repair seam yet"). Nothing actively
   re-derives the I4.x state, so the HOLD can stay frozen until some other job
   happens to fix it. Build the I4.x repair seam.
4. **`mirror_divergence`** — `app/services/src/mirror_divergence_locator.c:272`.
   Has a service-side auto-clear (`mirror_divergence_note_agreement`) but it
   is **contingent on an external zclassicd re-agreeing**; on a
   judge/standalone node, or a genuinely persistent divergence, the HOLD
   never clears and the condition pages forever. Give it a non-oracle
   auto-clear (re-derive vs own canonical bodies).

### Separate sub-classes — NOT detective-heuristic-on-valid-state (note, lower / different cure)
- **`reducer_frontier.script_undetermined`**
  (`stage_repair_reducer_frontier_coin.c:898`) — borderline: an
  "irreducible" verdict that becomes a misfire if the body later becomes
  fetchable from peers. Give it a **re-arm/refetch owner**, not a permanent
  page. Worth doing soon.
- **`authority.pair_self_check`** (`invariant_sentinel.c:189`) — permanent
  blocker with no owner, but does NOT HOLD the fold. Give it an owner; lower
  priority.
- **`utxo_apply.fatal_store`** (`utxo_apply_stage.c:365`) and
  **`seed.linkage_gate`** / **`seed.torn_import`** — genuine store
  corruption / boot refuse-to-start, NOT heuristic misfires. Different cure
  = the sovereign **bodies-only refold** (so a torn borrowed seed is never
  trusted), already in flight; do not give these the decouple treatment.
- **`contradiction_frozen`**, **`replay_canary_failed`**,
  **`utxo_drift_detected (parity_bh)`** — permanent operator pages that do
  NOT freeze the coins fold. Observability latches; lowest priority for this
  generalization.

**Order of work:** linkage + coinbase_label (P0, identical fix, shared dead
owner) → window_sweep + mirror_divergence repair seams (P1) →
reducer_frontier refetch owner → the rest.
