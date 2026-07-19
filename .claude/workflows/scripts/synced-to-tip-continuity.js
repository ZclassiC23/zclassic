export const meta = {
  name: 'synced-to-tip-continuity',
  description: 'Prove boot-after-import resumes folding to tip, and make the import->resume transition observable',
  phases: [
    { title: 'Implement', detail: '4 isolated worktree lanes: fold-continuity test, boot O(delta) resume, resume observability dumper, gap condition healer' },
  ],
}

const LANE_SCHEMA = {
  type: 'object',
  additionalProperties: false,
  required: ['branch', 'verdict', 'files_changed', 'summary', 'gates'],
  properties: {
    branch: { type: 'string', description: 'the git branch this lane committed to, or NONE' },
    verdict: { type: 'string', enum: ['MERGE_READY', 'NEEDS_WORK', 'BLOCKED'] },
    files_changed: { type: 'array', items: { type: 'string' } },
    summary: { type: 'string' },
    gates: { type: 'string' },
    caveats: { type: 'string' },
  },
}

const COMMON = `zclassic23 is one C23 binary, consensus-parity with zclassicd. A durable append-only log (progress.kv) is folded by 8 reducer stages (header_admit -> validate_headers -> body_fetch -> body_persist -> script_validate -> proof_validate -> utxo_apply -> tip_finalize); each stage advances its cursor by one OR names a typed blocker; projections rebuild from the log; H* = MIN over the contiguous ok=1 prefix; getblockcount serves H*. The shielded-history importer (-import-complete-shielded, app/services/src/shielded_history_import_service.c) clears utxo_apply.{anchor,nullifier}_backfill_gap by importing zclassicd's complete anchor+nullifier set atomically and flipping both activation cursors (anchor_kv/nullifier_kv activation cursors) to 0. The gap blocker is owned by the utxo_apply stage (app/jobs/src/utxo_apply_anchors.c + utxo_apply_nullifiers.c) and healed as a condition (app/conditions/src/sapling_anchor_frontier_unavailable.c). The tipbind fix + the importer itself ALREADY LANDED on main (c1014e000) — do NOT re-implement them.

GOAL OF THIS WORKFLOW: prove and harden the "synced to tip" tail — after the import flips the cursors and COMMITs, the very next boot/reducer tick must clear the gap blocker and RESUME folding forward from the wedge height to tip WITHOUT re-wedging, skipping, duplicating, or doing an O(chain) rescan; and an operator must be able to SEE that recovery happening via typed state, not log-greps.

HARD RULES: consensus parity inviolable; copy-prove before any live effect; recovery-apply paths stay CONTAINED (verify-only, refuse public/dev lanes, NEVER auto-run a borrowed-seed install — respect the check-no-new-borrowed-seed / check-no-new-repair-rung lint gates; any rung you add points the operator at the import + copy-prove, it does not autonomously mutate); every malloc via zcl_malloc, every error return LOG_*, every write via AR lifecycle; files under 800 lines; every thread supervised. If your worktree cannot link (missing vendor/lib/*.a), run make worktree-init first. You are in your OWN isolated git worktree — commit to THIS worktree's branch only; do NOT push/merge/touch other worktrees. NEVER touch a live datadir, stop zclassicd, or disturb the mint producer. Self-gate: make build-only + focused test group (make t-fast ONLY=<group>) + make lint (+ agent_impact_rules.def mapping per changed .c; reconcile DOC-COUNTS / EXPECTED_DIAGNOSTICS_DUMPERS if you add a dumper). Final message = structured data, precise + technical; report the branch name.`

phase('Implement')

const lanes = await parallel([
  () => agent(`${COMMON}

LANE fold-continuity (THE proof). Files: a NEW test lib/test/src/test_import_fold_continuity.c (wire it into lib/test/src/test_parallel.c + agent_impact_rules.def) + AT MOST the one stage file that owns the gap-blocker refresh (app/jobs/src/utxo_apply_anchors.c and/or utxo_apply_nullifiers.c) if you find a real continuity bug. Build a datadir fixture in the exact post-import wedge-cleared state: positive-then-flipped-to-0 activation cursors, the imported complete anchor+nullifier set present, gap blocker previously raised. Tick utxo_apply / tip_finalize and ASSERT: (a) the anchor/nullifier gap blocker is re-checked after the cursor flip and CLEARS (not stale-latched); (b) coins_applied_height advances to hstar+1 — CONTINUITY, not a skip over the gap; (c) no re-wedge over the next N heights. If the blocker refresh does NOT re-check after a cursor flip (a real latch bug), fix it MINIMALLY in the owning stage file. Keep the fix tiny and additive.`,
    { isolation: 'worktree', model: 'opus', label: 'impl:fold-continuity', phase: 'Implement', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE boot-resume-Odelta. Files: config/src/boot_refold_staged.c (+ its focused test) ONLY — disjoint from the utxo_apply stage files another lane owns. Verify that booting a node whose cursors were just flipped by the import does an O(delta) resume (fold only wedge_height..tip), NOT an O(chain) re-scan from genesis or a full re-derive. Instrument/measure the work done on the first post-import boot on a fixture (heights folded, rows re-read) and assert it is bounded by the delta, not the chain length. If you find an O(chain) rescan triggered by the import's cursor state (e.g. a repair pass that fires because it misreads the flipped cursor as "needs full backfill"), fix it minimally so the resume is delta-only while staying correct. Add/extend a focused test asserting the bound. Do NOT touch utxo_apply_*.c (another lane owns it).`,
    { isolation: 'worktree', model: 'opus', label: 'impl:boot-resume', phase: 'Implement', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE resume-observability. Make the import->resume transition introspectable via a typed dumpstate view (closes the 'is it actually recovering?' question). Per CLAUDE.md 'Adding state introspection': add a dump function (prefer a NEW file app/controllers/src/shielded_import_status_view.c, or extend an existing sovereignty/shielded dumper if one already covers this) that reports for a node in the import->resume window: trust_mode (release_assisted vs sovereign), self_folded (false right after import), both activation cursors (anchor + nullifier, value + flipped?), gap-blocker present?, imported per-pool anchor/nullifier counts, and the resume height (H*) climbing. If the importer-core lane exposes shielded_history_import_progress_snapshot(), also surface live import progress (anchors/nullifiers done, phase) when an import is mid-flight — graceful-absent otherwise. Register the dumper in app/controllers/src/diagnostics_registry.c; reconcile EXPECTED_DIAGNOSTICS_DUMPERS in lib/test/src/test_rpc.c and DOC-COUNTS via tools/scripts/check_doc_counts.sh. Add/extend a dumper test. Own ONLY: the new view file + diagnostics_registry.c + the count-pin files + your test.`,
    { isolation: 'worktree', model: 'sonnet', label: 'impl:resume-observability', phase: 'Implement', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE gap-condition-healer. Files: app/conditions/src/sapling_anchor_frontier_unavailable.c (+ its focused test) ONLY. When the anchor/nullifier gap blocker is present, make the condition emit a NAMED, actionable remedy (the exact 'header-refresh then -import-complete-shielded then boot' operator recipe + the copy-prove step) via the typed blocker/condition surface — instead of only a passive blocker — and a WITNESS that fires when a subsequent tick observes the gap cleared (blocker gone + H* advanced). CONTAINMENT IS INVIOLABLE: the remedy is verify-only guidance, owner-gated + copy-prove-gated; it must REFUSE on public/dev lanes and must NOT auto-execute a live import (respect check-no-new-repair-rung / check-no-new-borrowed-seed — if these gates flag your change, you have over-reached; back off to pure guidance). Reconcile any condition-cooldown / typed-blocker / supervisor-registration lint gate you touch. Focused test: gap present -> named remedy surfaced + refuses live auto-exec; gap cleared -> witness fires; gap absent -> no remedy.`,
    { isolation: 'worktree', model: 'sonnet', label: 'impl:gap-healer', phase: 'Implement', schema: LANE_SCHEMA }),
])

return { lanes: lanes.filter(Boolean) }
