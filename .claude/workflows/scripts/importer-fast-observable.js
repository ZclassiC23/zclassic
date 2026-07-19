export const meta = {
  name: 'importer-fast-observable',
  description: 'Make the shielded-history import fast, never-silent, and turnkey to prove',
  phases: [
    { title: 'Implement', detail: '4 isolated worktree lanes: importer core perf+progress, main-mode UX, reader defensive tests, turnkey harness' },
  ],
}

const LANE_SCHEMA = {
  type: 'object',
  additionalProperties: false,
  required: ['branch', 'verdict', 'files_changed', 'summary', 'gates'],
  properties: {
    branch: { type: 'string', description: 'the git branch this lane committed to (its worktree branch), or NONE' },
    verdict: { type: 'string', enum: ['MERGE_READY', 'NEEDS_WORK', 'BLOCKED'] },
    files_changed: { type: 'array', items: { type: 'string' } },
    summary: { type: 'string', description: 'what changed and why, technical' },
    gates: { type: 'string', description: 'exact results: build-only, focused test group, lint' },
    caveats: { type: 'string', description: 'anything the integrator must know before merging' },
  },
}

const COMMON = `zclassic23 is one C23 binary, bit-for-bit consensus-parity with zclassicd. The shielded-history importer (app/services/src/shielded_history_import_service.c, flag -import-complete-shielded=<zclassicd-datadir> handled in src/main.c import_complete_shielded_mode ~line 2574) clears utxo_apply.{anchor,nullifier}_backfill_gap: it ldb_snapshot_make's zclassicd's chainstate (HARDLINK of the immutable SSTs — already instant, NOT a bottleneck), opens the snapshot via chainstate_legacy_reader, streams every Sapling/Sprout anchor + nullifier into anchor_kv/nullifier_kv inside ONE BEGIN IMMEDIATE, binds the tip Sapling frontier to the header-committed hashFinalSaplingRoot (the tipbind fix ALREADY LANDED — do NOT redo it), then flips both activation cursors to 0.

LIVE EMPIRICAL FINDING (2026-07-16, a real import of the merged binary against a wedged clone + live zclassicd): the import runs CPU-bound (state=R, 100% one core) for MANY MINUTES with ZERO progress output and NO node.db writes yet (WAL empty at 5 min) — it is silently iterating + re-hashing every anchor's incremental merkle tree (the fail-closed root re-check in the streaming callbacks shi_anchor_cb / shi_nullifier_cb, which only count, never log). This VIOLATES the node's prime invariant ("a stall is ALWAYS a named blocker at a known point, never a silent stop") AND the "node must always sync FAST" invariant. Two separate defects: (1) it is SILENT, (2) it may be SLOW. Both are in scope. Also: src/main.c's import-mode printf() header lines are stdio-buffered so the operator sees nothing until exit.

HARD RULES: consensus parity inviolable; all-or-nothing atomicity (any anomaly rolls back leaving cursors POSITIVE + blockers raised — a safe halt, never a partial flip; do NOT weaken this to go faster); every malloc via zcl_malloc, every error return via LOG_*, every write via AR lifecycle; files under 800 lines (app/+config ENFORCED, lib WARN); NEVER touch a live datadir, NEVER stop zclassicd or the mint producer (you MAY read $HOME/.zclassic READ-ONLY). If your worktree cannot link (undefined vendor symbols / missing vendor/lib/*.a — gitignored, absent in a fresh worktree), run make worktree-init first. You are in your OWN isolated git worktree — work only here; commit green work to THIS worktree's branch; do NOT push, merge to main, or touch any other worktree. Self-gate: make build-only, the focused test group via make t-fast ONLY=<group>, make lint (add agent_impact_rules.def mapping for every changed .c). Final message = structured data for the orchestrator, precise + technical; report the branch name.`

phase('Implement')

const lanes = await parallel([
  () => agent(`${COMMON}

LANE importer-core (perf + progress; THE key lane). Files: app/services/src/shielded_history_import_service.c + .h ONLY. Do TWO things WITHOUT weakening atomicity or the fail-closed anchor root re-check:
(1) NEVER-SILENT: add periodic progress. Every N rows (pick N so it logs a few times/sec at real scale, e.g. 50000) LOG_INFO an "shielded import progress: anchors=A nullifiers=M pool=P elapsed_ms=..." line, AND maintain atomic progress counters (anchors_done, nullifiers_done, phase enum {snapshot,scan_anchors,scan_nullifiers,bind,commit}) inside the service, exposed via a new getter in the .h: 'bool shielded_history_import_progress_snapshot(struct shi_progress *out);' so a concurrent dumpstate can read it (another lane will render it — you only expose the getter + atomics, do NOT register a dumper). Reentrant/atomic-load safe.
(2) FAST: profile where the minutes go (the incremental-tree deserialize + root re-hash per anchor is the prime suspect). Cut redundant work — e.g. batch the anchor_kv/nullifier_kv inserts (one prepared stmt reused, not re-prepared per row), avoid re-deserializing a tree twice, hoist invariant work out of the per-row loop. Measure before/after on a synthetic fixture with many anchors+nullifiers and report the delta. If a correctness-preserving cheaper fail-closed check exists (e.g. the reader already validated the root — don't re-hash a second time in the service), use it, but the imported frontier root MUST still be verified against the header-committed root before commit. Add/extend a test in group shielded_history_import: progress counters advance monotonically; a forced mid-scan anomaly still rolls back fully (empty tables, cursors positive).`,
    { isolation: 'worktree', model: 'opus', label: 'impl:importer-core', phase: 'Implement', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE main-import-ux. Files: src/main.c import_complete_shielded_mode ONLY (~line 2574-2760). Three fixes: (1) OUTPUT VISIBILITY: the printf() header/progress lines are stdio-buffered so nothing shows until exit — make import-mode stdout line-buffered or unbuffered (setvbuf(stdout, NULL, _IOLBF/_IONBF, 0) at mode entry, or fflush(stdout) after each printf) so the operator sees 'Source chainstate', 'Chainstate snapshot', 'Tip bind', and progress LIVE. Keep the banners EXACT ('IMPORT COMPLETE (committed=' / 'IMPORT REFUSED') — the copy-prove harness greps them. (2) SNAPSHOT CLEANUP: the hardlinked snapshot dir (target/shielded_import_cs_snap, ~389MB of hardlinks) is left behind — remove it on BOTH success and every failure/rollback exit path (a helper + goto cleanup, or a small unlink-tree). Never delete anything OUTSIDE target/shielded_import_cs_snap. (3) keep all existing bounds/refusals (cs_src/snap_path/db_path buffer checks, root_populated, block status). Add a focused assertion/test if the mode has a test hook; otherwise verify by 'make build-only' + a bash -n and describe the manual check. Do NOT touch the service .c (another lane owns it).`,
    { isolation: 'worktree', model: 'sonnet', label: 'impl:main-import-ux', phase: 'Implement', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE reader-defensive-tests. Files: lib/storage/src/chainstate_legacy_reader.c/.h + its unit test group ONLY (grep test_parallel.c for the reader test group name; likely test_chainstate_legacy_reader). Re-add the two negative-path tests that were dropped in an earlier conflict resolution and harden any matching bound: (a) an EMPTY anchor/nullifier value → refused, not a crash or out-of-bounds read; (b) a TRUNCATED / short anchor value (fewer bytes than incremental_tree_deserialize needs) → the reader detects the short buffer and fails closed (no over-read past the value length). Build a synthetic LevelDB fixture (empty pool, single valid row, corrupt/short row). Confirm every iterator frees its handle on every path (including the early-abort torn-record path). If the bounds are already sound, say so with the exact checks re-run and add only the two regression tests. Keep edits SMALL and additive.`,
    { isolation: 'worktree', model: 'sonnet', label: 'impl:reader-tests', phase: 'Implement', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE harness-turnkey. Files: tools/scripts/import-copy-prove.sh + tools/scripts/import-copy-prove-selftest.sh + docs/work/fast-sync-to-tip-plan-2026-07-16.md ONLY (no .c). Make import-copy-prove.sh a SINGLE turnkey prove that mirrors the verified live sequence: (1) cp -a a wedged source datadir to a /.zclassic-c23-COPY- marked dir (KEEP the marker guard — refuse any target without it, never a live datadir); (2) header-refresh: \$NODE_BIN --importblockindex \$ZD_DATADIR \$COPY/node.db (argv[1] form = header-only; this populates blocks.sapling_root for the bind height — REQUIRED before import or the tipbind refuses with a zero root); (3) import: \$NODE_BIN -datadir=\$COPY -import-complete-shielded=\$ZD_DATADIR, FAIL unless it prints 'IMPORT COMPLETE (committed=' with a non-zero committed count; (4) boot the COPY isolated on throwaway ports and GATE ON H* CLIMB PAST THE WEDGE (getblockcount / dumpstate reducer_frontier H* must exceed the pre-import wedge height) AND, if reachable, tip-hash parity vs the live zclassicd best hash; (5) always clean up the COPY + its snapshot on exit. Every step FAILs loud (never silently proceeds); zclassicd is only ever read, never stopped. Note in a comment: the ldb snapshot is hardlinked (instant) and the anchor scan is CPU-bound and currently multi-minute. Update the hermetic selftest so its mock node accepts --importblockindex + -import-complete-shielded and prints the exact banners; prove 'bash tools/scripts/import-copy-prove-selftest.sh' exits 0. Reconcile any prose in the plan doc still naming the old -import-shielded-history/IMPORTED contract to -import-complete-shielded/'IMPORT COMPLETE'. Run bash -n on both scripts + the selftest.`,
    { isolation: 'worktree', model: 'sonnet', label: 'impl:harness-turnkey', phase: 'Implement', schema: LANE_SCHEMA }),
])

return { lanes: lanes.filter(Boolean) }
