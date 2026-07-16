export const meta = {
  name: 'always-sync-hardening',
  description: 'Make anchor-gap self-healing, post-import fold continuous, supervised, and fail-closed',
  phases: [
    { title: 'Implement', detail: '4 isolated worktree lanes: remediation rung, fold continuity, supervise+telemetry, defensive bounds' },
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

const COMMON = `zclassic23 is one C23 binary, consensus-parity with zclassicd. Durable append-only log (progress.kv) folded by 8 reducer stages (each advance-cursor-or-name-a-typed-blocker); projections rebuild from the log; H* = MIN over the contiguous ok=1 prefix; a stall is ALWAYS a named blocker, never silent. The just-landed shielded-history importer (app/services/src/shielded_history_import_service.c, -import-complete-shielded flag) clears utxo_apply.{anchor,nullifier}_backfill_gap by importing zclassicd's complete anchor+nullifier set atomically and flipping both activation cursors to 0. Recovery is a coordinator that picks the cheapest rung: warm restart -> range re-derive -> segment refetch -> named blocker. Supervisors form a liveness tree (dumpstate supervisor); conditions are {detect,remedy,witness} healers (app/conditions/).

HARD RULES: consensus parity inviolable; copy-prove before any live effect (owner-gated; recovery-apply paths are CONTAINED — they refuse on public/dev lanes and NEVER auto-run a borrowed-seed install; the lint gates check-no-new-borrowed-seed / check-no-new-repair-rung EXIST — respect them: any rung you add must be a contained/verify-only surface, not an autonomous live mutation); every malloc via zcl_malloc, every error return LOG_*, every write AR lifecycle; files under 800 lines; every thread supervised. If your worktree cannot link (undefined vendor symbols / missing vendor/lib/*.a), FIRST run: cp -a /home/rhett/github/zclassic23/vendor/lib vendor/ (the static libs are gitignored and absent in a fresh worktree). You are in your OWN isolated git worktree — work only here, commit to THIS worktree's branch, do NOT push/merge/touch other worktrees. Self-gate: make build-only + focused test group (make t-fast ONLY=<group>) + make lint (+ agent_impact_rules.def mapping per changed .c). Final message = structured data, precise + technical; report the branch name.`

phase('Implement')

const lanes = await parallel([
  () => agent(`${COMMON}

LANE remediation-rung (owner-gated, CONTAINED). Make anchor_backfill_gap / nullifier_backfill_gap a NAMED, actionable remedy surfaced through the typed command/blocker surface, integrated with the recovery model — WITHOUT weakening containment. Requirements: (1) owner-gated + copy-prove-gated exactly like other recovery-apply paths; it must REFUSE on public/dev lanes and must NOT auto-execute a live import (no silent borrowed-seed install; respect check-no-new-borrowed-seed / check-no-new-repair-rung — if you add a rung it is contained/verify-only, pointing the operator at the import + copy-prove path, never an autonomous mutation). (2) When the gap blocker is present, emit a NAMED remedy (the exact import command + copy-prove step) instead of only a passive blocker. (3) A dumpstate view showing the gap blocker with its named remedy + containment state. Prefer NEW files (app/conditions/src/*.c or a controller) over editing the monolith. Focused test: gap present -> named remedy surfaced + refuses live; gap absent -> no remedy. Reconcile any lint gate you touch (supervisor-registration, typed-blocker, condition-cooldown, no-new-repair-rung).`,
    { isolation: 'worktree', model: 'opus', label: 'impl:remediation-rung', phase: 'Implement', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE fold-continuity. After the importer flips both activation cursors to 0 and COMMITs, the next reducer tick must clear the gap blocker and RESUME folding forward from the wedge height without re-wedging/skipping/duplicating. Add a focused test: seed a datadir fixture at the wedge state (positive activation cursor + gap blocker raised + the imported complete anchor/nullifier set present + cursors flipped to 0), tick utxo_apply / tip_finalize, assert: (a) the anchor-gap blocker refresh clears the blocker, (b) coins_applied_height advances to hstar+1 (continuity, not skipping), (c) no re-wedge over the next N heights. If you find a real continuity bug (e.g. the blocker refresh does not re-check after a cursor flip), fix it minimally in the owning stage file. Prefer extending an existing test group. Files: a test + at most the one stage file that owns the blocker refresh.`,
    { isolation: 'worktree', model: 'sonnet', label: 'impl:fold-continuity', phase: 'Implement', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE supervise-telemetry. Make the import -> resume transition OBSERVABLE. Extend the ROM/compile status or a dumpstate subsystem so an operator SEES, for a node in the import->resume transition: both activation cursors, gap-blocker presence, imported anchor/nullifier counts, and the resume height climbing — closing the 'is it actually recovering?' question with a typed view, not log-greps. Reuse the existing rom_compile_status / diagnostics dumper machinery; do NOT add a bespoke command if a dumper row suffices. Verify the post-import reducer resume is already on the supervisor liveness tree (staged sync supervisor) and add a dumpstate assertion if a gap exists. Add/extend the focused test. Reconcile DOC-COUNTS / dumper-catalog pins if you add a dumper.`,
    { isolation: 'worktree', model: 'sonnet', label: 'impl:supervise-telemetry', phase: 'Implement', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE defensive-bounds. Audit the import path for the fail-anything standard: every input, bound, and allocation on the -import-complete-shielded path and the chainstate iterators must fail closed. Check: (a) the snapshot path + target datadir args are length/format validated (no overflow of the fixed cs_src/snap_path buffers in src/main.c import_complete_shielded_mode); (b) an absurd chainstate row count cannot OOM the importer (bounded batching or an explicit sane cap with a NAMED refusal, never a silent truncation); (c) the tip-bind SQL uses a bound parameter and cannot be fooled by a duplicate-hash row; (d) a malformed/short anchor or nullifier value is refused, not read out of bounds. Add negative-path tests for any gap. If already sound, say so with the exact checks re-run and add at most a hardening assertion + test. Keep edits SMALL and additive (you own bounds/negative-path only). Files: src/main.c import mode + shielded_history_import_service.c + reader + tests.`,
    { isolation: 'worktree', model: 'sonnet', label: 'impl:defensive-bounds', phase: 'Implement', schema: LANE_SCHEMA }),
])

return { lanes: lanes.filter(Boolean) }
