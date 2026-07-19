export const meta = {
  name: 'integration-cutover-ready',
  description: 'Gate the integrated main, make the canonical copy-prove turnkey, and prepare the owner-gated cutover',
  phases: [
    { title: 'Verify+Prepare', detail: '4 isolated worktree lanes: full-suite gate, turnkey prove harness, cutover runbook, worktree cleanup + trust dumper check' },
  ],
}

const LANE_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['branch', 'verdict', 'files_changed', 'summary', 'gates'],
  properties: {
    branch: { type: 'string' },
    verdict: { type: 'string', enum: ['MERGE_READY', 'NEEDS_WORK', 'BLOCKED', 'PASS', 'DIAGNOSIS_ONLY'] },
    files_changed: { type: 'array', items: { type: 'string' } },
    summary: { type: 'string' },
    gates: { type: 'string' },
    caveats: { type: 'string' },
  },
}

const COMMON = `zclassic23 is one C23 binary, consensus-parity with zclassicd. Today (2026-07-16) the -import-complete-shielded importer was made FAST (7-9s, was intractable: O(anchors×Pedersen) -> O(anchors)+O(1)) and proven to CLEAR the anchor_backfill_gap wedge on a copy of the live canonical node (boundary=3,176,325 flipped, coins advanced to 3,176,326). Main is at d27d6e603 (importer fix + 6 merged workflow lanes: reader _bulk, service inlined-insert+progress, main.c setvbuf, reader negative tests, boot O(delta) test, gap-condition healer). NOT yet pushed.

HARD RULES: consensus parity inviolable; every malloc via zcl_malloc, every error return LOG_*, files < 800 lines; recovery-apply / cutover paths stay CONTAINED and owner-gated (respect check-no-new-borrowed-seed / check-no-new-repair-rung); NEVER touch ~/.zclassic-c23 (live canonical), ~/.zclassic-c23-mint (producer), or stop zclassicd. If your worktree cannot link, run make worktree-init first. You are in your OWN isolated git worktree — commit to THIS branch only; do NOT push/merge/touch others. Self-gate per lane. Final message = structured data, technical; report the branch name.`

phase('Verify+Prepare')

const lanes = await parallel([
  () => agent(`${COMMON}

LANE full-suite-gate (VERIFY-ONLY). Build the integrated main (make -j) and run the FULL suite (make test-parallel). The 6 merged lanes added test files (test_chainstate_legacy_reader, test_boot_refold_window_extend, test_sapling_anchor_frontier_condition, updated test_shielded_history_import) — confirm they are registered (test_parallel.c + agent_impact_rules.def) and GREEN, and that DOC-COUNTS pins are reconciled. Report the exact "ALL TESTS PASSED — 0/N" (or the failing groups). If a merge left an unregistered test / stale count pin / lint gap, FIX exactly that (registration + count reconcile only — do NOT change feature code). Gate on the PASS token per tools/scripts/gate-and-report.sh (grep -q "ALL TESTS PASSED" AND no "SOME TESTS FAILED"). Report verdict PASS with the token, or NEEDS_WORK with the failing group names + first failure.`,
    { isolation: 'worktree', model: 'sonnet', label: 'gate:full-suite', phase: 'Verify+Prepare', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE turnkey-canonical-prove. Extend tools/scripts/import-copy-prove.sh (already the import prove harness) OR add a sibling tools/scripts/canonical-cure-prove.sh that runs the FULL verified canonical-copy→tip flow as ONE turnkey command: (1) cp -a a source datadir to a /.zclassic-c23-COPY-*-canonical marked dir (KEEP the marker guard; refuse a live datadir); (2) header-refresh: \$NODE_BIN --importblockindex \$ZD_DATADIR \$COPY/node.db (argv[1] header-only form — REQUIRED so the copy's headers cover zclassicd's chainstate best block AND blocks.sapling_root is populated for the bind); (3) import: \$NODE_BIN -datadir=\$COPY -import-complete-shielded=\$ZD_DATADIR — FAIL unless 'IMPORT COMPLETE (committed=' with boundary= the copy's wedge height; (4) clear any stale auto_reindex_request; (5) boot the copy isolated (unique ports) with -addnode=\$ZD_P2P for tail bodies; (6) GATE on H* CLIMB past the pre-import wedge height (dumpstate reducer_frontier hstar / coins_applied_height must exceed it). Every step FAILs loud; always clean up the copy + its snapshot on exit; zclassicd only read, never stopped. Add a hermetic selftest (mock node prints the banners) and prove it exits 0. Files: the harness script(s) ONLY (no .c). bash -n + selftest.`,
    { isolation: 'worktree', model: 'sonnet', label: 'harness:canonical-prove', phase: 'Verify+Prepare', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE cutover-runbook (DOCS). Write the owner-gated live-canonical cutover + revert plan, given a green canonical copy-prove. Update docs/HANDOFF.md (current live state: canonical wedged at H*=3,176,325 on anchor_backfill_gap; the import fix + wedge-cure are proven on a copy; the tail-fold-to-tip is the remaining item tracked by the cured-node-to-tip workflow) and write docs/work/canonical-cutover-runbook-2026-07-16.md: the exact steps to cut the live canonical over once the copy reaches tip — (a) copy-prove green + H* past wedge, (b) revert path exists (the pre-cutover datadir is preserved / the node is a linger service that can be restored), (c) stop service -> apply the proven header-refresh+import on the live datadir (or swap in the proven copy) -> restart -> verify H* climbs + explorer/onion return, (d) rollback if H* does not climb within N minutes. Be precise and technical (exact commands, exact gates). Make clear this is OWNER-GATED and must not be auto-executed. Files: docs/HANDOFF.md + docs/work/canonical-cutover-runbook-2026-07-16.md ONLY.`,
    { isolation: 'worktree', model: 'sonnet', label: 'docs:cutover-runbook', phase: 'Verify+Prepare', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE trust-mode-and-cleanup. Two small things. (1) Confirm the sovereignty/trust dumpers correctly report an import-cured node as release_assisted (NOT sovereign, self_folded=false) with the shielded_import provenance stamp and the tail-fold progress (H* vs network tip) visible via dumpstate — boot your own copy of the cured canonical copy and capture 'dumpstate sovereignty' + 'dumpstate reducer_frontier'; if a field that would answer "is it cured and now syncing the tail?" is missing, add it to the existing dumper (extend, don't add a new file). (2) There are 65 registered git worktrees (leftover from prior workflows) — produce (do NOT execute) a safe cleanup list: which .claude/worktrees/* and wf_* branches are stale/merged and safe to 'git worktree remove' + 'git branch -D', excluding any with unmerged commits. Write it to docs/work/worktree-cleanup-2026-07-16.md. Files: at most one existing dumper .c (if a field is genuinely missing) + the cleanup doc. If the dumpers already cover it, verdict DIAGNOSIS_ONLY for part 1 and just deliver the cleanup list.`,
    { isolation: 'worktree', model: 'sonnet', label: 'verify:trust-cleanup', phase: 'Verify+Prepare', schema: LANE_SCHEMA }),
])

return { lanes: lanes.filter(Boolean) }
