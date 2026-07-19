export const meta = {
  name: 'importer-realdata',
  description: 'Make the shielded-history importer provably correct on real wedged data',
  phases: [
    { title: 'Implement', detail: '4 isolated worktree lanes: tip-bind fix, reader correctness, turnkey harness, provenance' },
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

const COMMON = `zclassic23 is one C23 binary, bit-for-bit consensus-parity with zclassicd. The shielded-history importer (app/services/src/shielded_history_import_service.c, flag -import-complete-shielded=<zclassicd-datadir> in src/main.c) scans zclassicd's finished anchor+nullifier sets (Z/A/S/s chainstate keyspaces via lib/storage/src/chainstate_legacy_reader.c) into anchor_kv+nullifier_kv in ONE atomic transaction, binds the tip Sapling frontier to the header-committed hashFinalSaplingRoot, then flips both activation cursors to 0 to clear utxo_apply.{anchor,nullifier}_backfill_gap. Read docs/work/fast-sync-to-tip-plan-2026-07-16.md §4 for the spec and tools/scripts/import-copy-prove.sh for the acceptance contract.

EMPIRICAL FINDING (2026-07-16, a real diagnostic run of the merged importer against a wedged clone + live zclassicd): the import REFUSED at the tip-bind with:
  "tip bind FAILED: best Sapling anchor != expected header hashFinalSaplingRoot at h=3151581 — refusing"
  "Tip bind: height=3151581 hashFinalSaplingRoot=0000...0000"
i.e. the target block WAS found, but the target's stored blocks.sapling_root at that height is ALL-ZERO, so it can't match zclassicd's real anchor root. The fix is NOT about block connection status; it is that the tip-bind must obtain the header-committed hashFinalSaplingRoot from a source that is actually populated for that height (the block header itself / block_index), not a possibly-null projection column.

HARD RULES: consensus parity inviolable (no new opcode; validate against the real chain, not zclassicd source text); all-or-nothing atomicity (any anomaly rolls back leaving cursors POSITIVE + blockers raised — a safe halt, never a partial flip); every malloc via zcl_malloc, every error return via LOG_*, every write via AR lifecycle; files under 800 lines (app/+config ENFORCED, lib WARN); NEVER touch a live datadir or stop zclassicd (you MAY read $HOME/.zclassic READ-ONLY). If your worktree cannot link (undefined vendor symbols / missing vendor/lib/*.a — the static libs are gitignored and absent in a fresh worktree), run make worktree-init first. You are in your OWN isolated git worktree — work only here. Self-gate: make build-only, the focused test group via make t-fast ONLY=<group>, and make lint (add agent_impact_rules.def mapping for every changed .c). Commit green work to THIS worktree's branch; do NOT push, do NOT merge to main, do NOT touch any other worktree. Your final message is structured data for the orchestrator — precise and technical; report the branch name you committed to.`

phase('Implement')

const lanes = await parallel([
  () => agent(`${COMMON}

LANE tipbind (CONSENSUS-CRITICAL). Fix the tip-bind so a real wedged, header-imported target binds correctly. Diagnose why blocks.sapling_root is zero at the chainstate best-block height (does --importblockindex populate sapling_root? is it only set on full connection? grep block_index_loader.c and the blocks schema). Then make the importer obtain the header-committed hashFinalSaplingRoot for the bind height from a POPULATED source — the block header / block_index entry (entry.sapling_root at block_index_loader.c:170 is the header's hashFinalSaplingRoot) — rather than a possibly-null blocks.sapling_root column, WITHOUT weakening fork-safety: the imported tip frontier root must still be verified to equal the header-committed root, and a WRONG/incomplete frontier must still be refused with a full rollback. If the real defect is that the target's header import does not populate sapling_root at all, the correct fix may be in the header-import/block_index path so the column is populated from the header (preferred if that is the true root cause). Add a realistic integration test: chainstate tip ABOVE the target connected height, target header present with a real hashFinalSaplingRoot -> import binds + clears the wedge; a frontier root that does not match the header -> full rollback (empty tables, cursors positive). Files: app/services/src/shielded_history_import_service.c/.h, src/main.c (import mode), possibly the block_index/header-import path, lib/test/src/test_shielded_history_import.c. Group: shielded_history_import.`,
    { isolation: 'worktree', model: 'opus', label: 'impl:tipbind', phase: 'Implement', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE reader. Verify the 6 chainstate_legacy_reader iterators (chainstate_legacy_iter_{sapling,sprout}_{anchors,nullifiers}, chainstate_legacy_get_best_{sapling,sprout}_anchor) against the ACTUAL zclassicd chainstate structure. You MAY read $HOME/.zclassic/chainstate READ-ONLY to confirm the real Z/A/S/s key prefixes, value serialization, and obfuscation XOR — but only open a LevelDB handle on a COPY, never the live handle, and never write. Confirm: (1) seek prefixes/key formats match what zclassicd writes; (2) the anchor value is wire-compatible with incremental_tree_deserialize and the fail-closed root re-hash aborts a torn record; (3) large-scan bounds — no unbounded alloc, iterator freed on every path. Harden anything assumption-based. Extend the reader unit test on a synthetic LevelDB fixture: empty pool, single row, corrupt (non-re-hashing) anchor. Files: lib/storage/src/chainstate_legacy_reader.c/.h and its test group (grep test_parallel.c).`,
    { isolation: 'worktree', model: 'opus', label: 'impl:reader', phase: 'Implement', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE harness. Make tools/scripts/import-copy-prove.sh a TURNKEY end-to-end prove on a real wedged clone. Add, BEFORE phase 1: an optional header-refresh step that runs '\$NODE_BIN --importblockindex \$ZD_DATADIR \$COPY_DIR/node.db' (the proven read-only two-step recipe, step 1) against the COPY's node.db, so the clone's headers cover zclassicd's current chainstate tip AND its blocks.sapling_root is populated for the bind height (this is directly related to the tipbind finding above — the copy's headers being stale is one cause of the zero-root/absent-bind). Make it skippable with --skip-header-refresh; on failure the script FAILs (never silently proceeds with stale headers). Keep every safety invariant (the /.zclassic-c23-COPY- marker guard, never a live datadir, zclassicd only read, never stopped). Keep the importer contract EXACTLY as merged: -import-complete-shielded=\$ZD_DATADIR, banner 'IMPORT COMPLETE (committed=' / 'IMPORT REFUSED'. Update the hermetic selftest tools/scripts/import-copy-prove-selftest.sh so its mock node accepts --importblockindex and the -import-complete-shielded flag; prove 'bash tools/scripts/import-copy-prove-selftest.sh' exits 0. Files: the two harness scripts ONLY (no .c). Run bash -n + the selftest.`,
    { isolation: 'worktree', model: 'sonnet', label: 'impl:harness', phase: 'Implement', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE provenance. Make the operational-vs-sovereign trust state introspectable + docs accurate. (1) Ensure a dumpstate subsystem surfaces, for an imported node: trust_mode (release_assisted vs sovereign), self_folded (false after import), per-pool anchor/nullifier counts, and the shielded_import.provenance row. If the merged 'sovereignty' dumper already exists, extend it; else add one per CLAUDE.md 'Adding state introspection' (register in app/controllers/src/diagnostics_registry.c; reconcile DOC-COUNTS via tools/scripts/check_doc_counts.sh). (2) Reconcile any remaining prose in docs/work/fast-sync-to-tip-plan-2026-07-16.md that still names the old -import-shielded-history flag / IMPORTED banner to the merged -import-complete-shielded / 'IMPORT COMPLETE' contract. Files: diagnostics + docs + count-pin files. Group: test_rpc + any dumper test.`,
    { isolation: 'worktree', model: 'sonnet', label: 'impl:provenance', phase: 'Implement', schema: LANE_SCHEMA }),
])

return { lanes: lanes.filter(Boolean) }
