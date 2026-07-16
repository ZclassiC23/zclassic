export const meta = {
  name: 'cure-prove-and-support',
  description: 'Prove-harness, observability, anchor-completeness merge, and cutover docs for the cure-to-tip path',
  phases: [
    { title: 'Support', detail: '4 isolated worktree lanes: end-to-end prove harness, post-cure observability, anchor-completeness merge, cutover runbook' },
  ],
}

const LANE_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['branch', 'verdict', 'files_changed', 'summary', 'gates'],
  properties: {
    branch: { type: 'string' }, verdict: { type: 'string', enum: ['MERGE_READY','NEEDS_WORK','BLOCKED','PASS','DIAGNOSIS_ONLY'] },
    files_changed: { type: 'array', items: { type: 'string' } },
    summary: { type: 'string' }, gates: { type: 'string' }, caveats: { type: 'string' },
  },
}

const COMMON = `zclassic23 is one C23 binary, consensus-parity with zclassicd. The -import-complete-shielded importer (fast, proven) clears the shielded anchor_backfill_gap wedge. Two cure-path seams are being fixed in a sibling workflow: (A) re-arm proof_validate over pre-2026-07-13 NULL-block_hash rows so utxo_apply advances; (B) register the cured tip as a cold-import trust anchor so Invariant-A installs it and header-sync builds a real getheaders locator (else "header tip=160" stall). The full cure-to-tip path is: consistent-copy source -> header-refresh (--importblockindex) -> import (-import-complete-shielded) -> [A re-arm] + [B trust-anchor] -> boot with a peer for tail bodies -> H* climbs past the wedge to network tip. Main is at 3130e15e9.

HARD RULES: consensus parity inviolable; recovery/cutover paths CONTAINED + owner-gated (respect check-no-new-borrowed-seed / check-no-new-repair-rung); every malloc via zcl_malloc, every error return LOG_*, files < 800 lines; NEVER touch ~/.zclassic-c23, ~/.zclassic-c23-mint, or stop zclassicd. If your worktree cannot link, FIRST: cp -a /home/rhett/github/zclassic23/vendor/lib vendor/. OWN isolated worktree — commit to THIS branch only, do NOT push/merge/touch others. Self-gate per lane. Final message = structured data, technical; report the branch name.`

phase('Support')

const lanes = await parallel([
  () => agent(`${COMMON}

LANE prove-harness-full. Extend tools/scripts/canonical-cure-prove.sh (already the turnkey canonical-copy->tip prove) to the COMPLETE cure-to-tip sequence: (1) consistent point-in-time copy of the source datadir (use the new consistent-copy tool the sibling workflow adds if present — detect it; else fall back to cp -a with a loud WARNING that a torn block_index may result); (2) header-refresh via --importblockindex; (3) import via -import-complete-shielded (require 'IMPORT COMPLETE (committed=' + parse boundary); (4) the proof_validate re-arm step + the trust-anchor registration step (invoke whatever contained CLI/flag the sibling fixes expose; if they are boot-automatic, note that and skip explicit invocation); (5) clear any stale auto_reindex_request; (6) isolated boot with -addnode=<zclassicd P2P> for tail bodies; (7) GATE on H* CLIMB PAST the wedge boundary toward network tip (poll dumpstate reducer_frontier hstar / coins_applied over a bounded window; PASS when hstar exceeds boundary+K, FAIL with the first blocker if it stalls). Always clean up the copy. Keep the marker guard + never touch a live datadir + zclassicd read-only. Update the hermetic selftest. Files: tools/scripts/canonical-cure-prove*.sh ONLY. bash -n + selftest exit 0.`,
    { isolation: 'worktree', model: 'sonnet', label: 'harness:full-prove', phase: 'Support', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE post-cure-observability. Make the cure->tip transition fully answerable via typed dumpstate so an operator/agent sees exactly where a post-import node is: extend an EXISTING dumper (sovereignty or reducer_frontier or rom_compile — do NOT add a new file if one covers it) to report: trust_mode (release_assisted), the cold-import trust-anchor status (registered? at what height? does active_chain_tip() resolve vs NULL — the seam-B signal), the proof_validate re-arm status (are there ok=1/NULL-block_hash pv rows below the cursor? — the seam-A signal), the header-sync locator health (is the getheaders locator genesis-only vs real-tip? — the "tip=160" signal), and H* vs network tip (tail gap). These are exactly the fields that would have told us in one call why the cured node stalled. Reconcile EXPECTED_DIAGNOSTICS_DUMPERS + DOC-COUNTS if a dumper field count is pinned. Files: at most one existing dumper .c + its test + the count-pin files. Focused test.`,
    { isolation: 'worktree', model: 'sonnet', label: 'obs:post-cure', phase: 'Support', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE anchor-completeness-merge. A prior lane built tools/verify_anchor_completeness.c (+ a 'make verify_anchor_completeness' target) that opens a zclassicd chainstate LevelDB directly and counts 'Z'/'A'/'S'/'s' keys to ground-truth the import's completeness (it PROVED the import is complete; the apparent deficit was temporal skew — zclassicd advanced ~32k Sapling-active blocks since the import snapshot). That tool lives on branch worktree-wf_849bc50e-d3e-3. Re-create/verify it cleanly on YOUR worktree (it is a diagnostic tool, no consensus code): tools/verify_anchor_completeness.c + the Makefile target, building green with 'make verify_anchor_completeness', and add a short docs note (docs/work/) explaining it is the import-completeness ground-truth check and how temporal skew vs a fresh chainstate copy is expected. Confirm 'make verify_anchor_completeness' builds. Files: tools/verify_anchor_completeness.c + Makefile + a docs note. Keep it self-contained (vendored leveldb C API only).`,
    { isolation: 'worktree', model: 'sonnet', label: 'tool:anchor-completeness', phase: 'Support', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE cutover-runbook-update. Update docs/HANDOFF.md + docs/work/canonical-cutover-runbook-2026-07-16.md to reflect the FULL, accurate cure-to-tip picture as of 2026-07-16: (1) the import fix is proven + landed (fast, clears the shielded wedge on real canonical state); (2) the TWO remaining cure-path seams and their fixes — A: proof_validate NULL-block_hash re-arm (pre-2026-07-13 rows); B: cold-import trust anchor so the cured tip installs into the active chain (else the "header tip=160" genesis-locator stall); (3) the copy-prove MUST use a consistent (non-torn) source copy; (4) the exact owner-gated cutover sequence once a copy-prove reaches network tip, WITH a concrete revert path (preserve the pre-cutover datadir; the node is a linger service restorable via systemctl --user). Be precise + technical (exact commands, exact H*-climb gate, exact rollback trigger). Make clear it is OWNER-GATED, not auto-executed. Correct any stale claims in HANDOFF that predate today. Files: docs/HANDOFF.md + docs/work/canonical-cutover-runbook-2026-07-16.md ONLY.`,
    { isolation: 'worktree', model: 'sonnet', label: 'docs:cutover-update', phase: 'Support', schema: LANE_SCHEMA }),
])

return { lanes: lanes.filter(Boolean) }
