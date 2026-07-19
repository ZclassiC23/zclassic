export const meta = {
  name: 'cure-seams-implement',
  description: 'Implement the two cure-path seams (proof_validate re-arm + cold-import trust anchor) so a cured node reaches tip',
  phases: [
    { title: 'Implement', detail: '4 isolated worktree lanes: fix-A proof_validate re-arm, fix-B trust-anchor, consistent-copy tool, integration gate' },
  ],
}

const LANE_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['branch', 'verdict', 'files_changed', 'summary', 'gates'],
  properties: {
    branch: { type: 'string' }, verdict: { type: 'string', enum: ['MERGE_READY','NEEDS_WORK','BLOCKED','PASS'] },
    files_changed: { type: 'array', items: { type: 'string' } },
    summary: { type: 'string' }, gates: { type: 'string' }, caveats: { type: 'string' },
  },
}

const COMMON = `zclassic23 is one C23 binary, consensus-parity with zclassicd. Durable append-only log (progress.kv) folded by 8 reducer stages (header_admit -> validate_headers -> body_fetch -> body_persist -> script_validate -> proof_validate -> utxo_apply -> tip_finalize); each advances its cursor or names a typed blocker; H* = MIN over contiguous ok=1 prefix.

CONTEXT (2026-07-16, main d27d6e603/3130e15e9): the -import-complete-shielded importer is now fast + PROVEN to clear the shielded anchor_backfill_gap wedge on a copy of the live canonical node (boundary=3,176,325 flipped, coins advance to 3,176,326). But two REAL cure-path seams (diagnosed live; they affect the LIVE node too) block the cured node from folding the ~7,000-block tail to network tip (zclassicd oracle = 3,183,592). This workflow implements the two fixes as CONTAINED, owner-gated, copy-prove-gated changes. Do NOT weaken any consensus guard.

HARD RULES: consensus parity inviolable; recovery-apply paths stay CONTAINED — respect check-no-new-borrowed-seed / check-no-new-repair-rung (a new rung must be contained/verify-or-owner-gated, never an autonomous live mutation); every malloc via zcl_malloc, every error return LOG_*, every write via AR lifecycle; files < 800 lines (app/+config ENFORCED); every thread supervised; NEVER touch ~/.zclassic-c23 (live canonical), ~/.zclassic-c23-mint (producer), or stop zclassicd (read-only OK). If your worktree cannot link, run make worktree-init first. You are in your OWN isolated git worktree — commit to THIS branch only; do NOT push/merge/touch others. Self-gate: make build-only + focused test group (make t-fast ONLY=<group>) + make lint (+ agent_impact_rules.def per changed .c; reconcile DOC-COUNTS if you add a dumper). Final message = structured data, technical; report the branch name.`

phase('Implement')

const lanes = await parallel([
  () => agent(`${COMMON}

LANE fix-A: proof_validate NULL-block_hash re-arm. DIAGNOSIS (verified): the node's proof_validate_log rows for h≈3,155,843–3,176,488 were written 2026-07-10, BEFORE commit 7fb9f5650 (2026-07-13) added bi->phashBlock stamping to proof_validate_log_insert, so they have block_hash=NULL. utxo_apply step_apply (app/jobs/src/utxo_apply_stage.c:347) CORRECTLY refuses a hashless proof verdict (the 'label_splice' / 'proof_validate_log block_hash MISSING' blocker; intentional, tested at lib/test/src/test_utxo_apply_stage.c:780 — DO NOT weaken this guard). proof_validate's cursor (≈3,176,489) already passed those heights so it never re-stamps → post-cure wedge at 3,176,326. No existing repair rung covers ok=1 + NULL-block_hash (hash_split needs length=32; proof_internal_error needs ok=0).
IMPLEMENT a CONTAINED re-arm: when a node has just cured the shielded wedge (or on boot detecting the ok=1/NULL-block_hash condition in proof_validate_log below the pv cursor), rewind proof_validate's cursor to the import boundary (or the lowest NULL-block_hash pv height) and delete the NULL-block_hash proof_validate_log suffix so the CURRENT binary re-derives + re-stamps block_hash on the next fold, letting utxo_apply advance. Keep it contained (owner-gated / copy-prove-gated like other recovery-apply; must not autonomously mutate a public/dev node; respect check-no-new-repair-rung — if you add a rung it is a contained re-derive-in-place, not a borrowed-seed install). Reuse stage_rederive_range / the reducer-frontier rewind machinery if it fits (be careful: that subsystem caused a 2026-07-02 stall — the LCC invariant must hold). Files: app/jobs/src/proof_validate*.c + the rewind/rederive mechanism it owns + a focused test (fixture: ok=1/NULL-block_hash pv rows below cursor + a body present -> re-arm rewinds, re-derives, stamps block_hash, utxo_apply advances; a normal ok=1/valid-hash row -> untouched). Do NOT touch utxo_apply_stage.c's guard or the shielded_history_import_service.c integration (lane fix-B owns import-time hooks).`,
    { isolation: 'worktree', model: 'opus', label: 'fix-A:pv-rearm', phase: 'Implement', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE fix-B: cold-import trust anchor for the cured tip. DIAGNOSIS (verified): after the import cures the wedge, the cured coins tip h=3,176,325 is a "detached island" — app/services/src/utxo_recovery_frontier_gate.c:140/267 Invariant-A CORRECTLY refuses to install it because (a) there is a pprev height-tear between the compiled SHA3 anchor and 3,176,325 and (b) the cold-import trust anchor was never registered ("cold-import seed anchor not held at H=3176325, found=3182783"). With install refused, the in-memory active chain stays at genesis: lib/validation/src/chainstate.c:860 active_chain_height() reads 3,176,325 from tip_finalize_log (projection) while chainstate.c:278 active_chain_tip() returns NULL (h >= genesis-sized capacity). So lib/net/src/msg_headers.c process_headers builds a genesis-only getheaders locator -> every peer replies headers 1..160 -> "header tip=160" stall forever, tail bodies never requested.
IMPLEMENT: make the shielded import register the cured tip as a cold-import TRUST anchor (via utxo_recovery_set_cold_import_trust_anchor or the equivalent) so Invariant-A accepts installing 3,176,325 into the in-memory active chain, OR make the imported block-index ancestry contiguous from the SHA3 anchor to the cured tip so the pprev height-tear is resolved. The tip must become trust-rooted so active_chain_tip() returns it and the getheaders locator is built from the real tip. Keep it CONTAINED (the trust anchor is the import's own verified header-committed tip, not a borrowed peer value; respect check-no-new-borrowed-seed). Do NOT patch msg_headers.c to paper over the correct Invariant-A refusal. Files: app/services/src/utxo_recovery_frontier_gate.c + the trust-anchor setter + app/services/src/shielded_history_import_service.c (the import-time registration hook) + a focused test (imported cured tip -> trust anchor registered -> Invariant-A installs -> active_chain_tip() returns the tip, not NULL). Do NOT touch proof_validate*.c (lane fix-A owns it).`,
    { isolation: 'worktree', model: 'opus', label: 'fix-B:trust-anchor', phase: 'Implement', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE consistent-copy tool. The copy-prove was compromised by cp -a of a LIVE running node -> torn block_index.bin ("Corrupt flat file — reloading from SQLite") + inconsistent WAL. Build a tool/script that makes a CONSISTENT point-in-time copy of a node datadir for copy-prove: use SQLite's Online Backup API (or a WAL-checkpoint-then-copy) for node.db and every *.db, and a safe copy of the leveldb/flat block-index + progress.kv, so the copy opens clean (quick_check ok, block_index flat tip resolves, no torn projections). It must work WITHOUT stopping the source node. Prefer a small C tool (tools/) wired via a make target, or a hardened shell in tools/scripts consistent with the existing harnesses (no external deps beyond libc + the vendored sqlite). Verify: make a consistent copy of a running fixture node, boot the copy, confirm NO "Corrupt flat file" / "torn" warnings and the block_index tip resolves to the source height. Files: the copy tool + its make target/harness + a test/selftest. No consensus code.`,
    { isolation: 'worktree', model: 'sonnet', label: 'tool:consistent-copy', phase: 'Implement', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE integration-gate (VERIFY-ONLY on current integrated main). Build (make -j) and run the FULL suite (make test-parallel) on the CURRENT integrated main (HEAD 3130e15e9: the fast importer + reader _bulk + service inlined-insert/tip-verify + 8 merged workflow lanes incl main.c setvbuf, reader/boot/gap-condition tests, tail-fold dumper field, harnesses, docs). Confirm ALL TESTS PASSED — 0/N via tools/scripts/gate-and-report.sh (grep -q "ALL TESTS PASSED" AND no "SOME TESTS FAILED"). Verify every merged test file is registered (test_parallel.c + agent_impact_rules.def) and DOC-COUNTS pins are reconciled (the tail-fold dumper field in app/jobs/src/reducer_frontier_dump.c must not have broken a count pin). If a merge left an unregistered test / stale pin / lint gap, FIX exactly that (registration + count reconcile ONLY — no feature code). Report PASS with the exact token + group count, or NEEDS_WORK with the failing groups + first failure. This gate is the pre-push floor for the whole integration.`,
    { isolation: 'worktree', model: 'sonnet', label: 'gate:integration', phase: 'Implement', schema: LANE_SCHEMA }),
])

return { lanes: lanes.filter(Boolean) }
