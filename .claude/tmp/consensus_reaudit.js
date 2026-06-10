export const meta = {
  name: 'consensus-fix-reaudit',
  description: 'Adversarial re-audit of the two consensus-critical fixes before merge/deploy',
  phases: [
    { title: 'Attack', detail: 'three adversaries try to break the fixes' },
    { title: 'Adjudicate', detail: 'synthesize GO / fix-more verdict' },
  ],
}

const REPO = '/home/rhett/github/zclassic23'
const BASE = 'The repo is ' + REPO + ', branch finish/self-healing-service, HEAD commit 811320cd7 which fixed audit blockers. Read code, do not modify. Cite file:line and quote exact code for every claim. Be a skeptic trying to BREAK the fix; if you cannot, say so explicitly. Inspect with: git -C ' + REPO + ' show 811320cd7 --stat ; and per-file: git -C ' + REPO + ' diff fbfec53a0~1..811320cd7 -- PATH.'

const ATTACK_SCHEMA = {
  type: 'object',
  properties: {
    target: { type: 'string' },
    holds: { type: 'boolean', description: 'true if the fix is sound and you could NOT break it' },
    findings: {
      type: 'array',
      items: {
        type: 'object',
        properties: {
          severity: { type: 'string', enum: ['consensus-safety', 'correctness', 'dos-imprecision', 'minor', 'none'] },
          title: { type: 'string' },
          evidence: { type: 'string', description: 'exact code path + why, with file:line' },
          is_regression_from_this_commit: { type: 'boolean', description: 'true if 811320cd7 INTRODUCED it; false if it pre-existed on the branch' },
          suggested_fix: { type: 'string' },
        },
        required: ['severity', 'title', 'evidence', 'is_regression_from_this_commit', 'suggested_fix'],
      },
    },
  },
  required: ['target', 'holds', 'findings'],
}

phase('Attack')
const attacks = await parallel([
  () => agent(BASE + '\n' +
    'TARGET: the consensus acceptance gate in app/services/src/chain_activation_controller.c reducer_pending_body_is_accepted, which now requires utxo_apply_stage_succeeded_at(bi->nHeight) (implemented in app/jobs/src/utxo_apply_stage.c).\n' +
    'Try to find a path where reducer_ingest_block returns TRUE (MODE_VALID) for a block that is NOT fully/correctly validated. Probe specifically:\n' +
    '(a) HEIGHT-ONLY MATCH: utxo_apply_stage_succeeded_at(height) queries utxo_apply_log by height only, with NO block-hash match. Can a competing/fork block whose body is persisted (HAVE_DATA) at a height H where the ACTIVE chain already applied a DIFFERENT block (ok=1 at H) be accepted as MODE_VALID? Trace reducer_ingest_block end to end: is the ingested block_index guaranteed to be the active-chain block at its height when the pending path runs, or can it be a non-active fork block? What are the REAL consequences — does the fork block become the tip / mutate the UTXO set, or is it only a wrong return value to submitblock/relay with no chain-state effect?\n' +
    '(b) Does utxo_apply ok=1 at H truly imply script_validate AND proof_validate passed at H? Read app/jobs/src/utxo_apply_stage.c upstream gating + the stage advance-or-block contract. Can utxo_apply record ok=1 without script/proof ok=1 (a skip, a stubbed reader, reorg residue)?\n' +
    '(c) RACE: reducer_pending_body_is_accepted runs AFTER zcl_mutex_unlock. Can utxo_apply_log or the block_index change under it between unlock and the query?\n' +
    'For each: is it a consensus-safety hole (invalid block becomes tip / corrupts UTXO), a correctness/dos imprecision, or none — and did THIS commit introduce it vs pre-exist on the branch (compare to fbfec53a0~1).',
    { label: 'attack:accept-gate', phase: 'Attack', schema: ATTACK_SCHEMA, agentType: 'Explore' }),

  () => agent(BASE + '\n' +
    'TARGET: app/jobs/src/stage_repair.c stage_repair_header_solution_poison_rewind no longer deletes tip_finalize_log rows (removed from downstream_logs[]) but STILL rewinds the tip_finalize cursor (downstream_stages[] keeps tip_finalize) and still deletes the other 6 downstream logs.\n' +
    'Try to break it:\n' +
    '(a) NEW WEDGE: after the repair the tip_finalize cursor is rewound to the frontier height and the upstream utxo_apply_log rows at/above that height are deleted, but the tip_finalize_log ok=0 rows at/above it SURVIVE. When tip_finalize re-runs from the rewound cursor, do those surviving ok=0 rows get cleanly overwritten (log_insert INSERT OR REPLACE) on re-finalize, or can a leftover row block/poison/short-circuit the re-drive (e.g. a read that treats a stale ok=0 row as terminal)? Read app/jobs/src/tip_finalize_stage.c re-finalize path + app/jobs/src/tip_finalize_log_store.c log_insert.\n' +
    '(b) Does the success_checked_logs ok=1 guard (which retains tip_finalize_log) still fully protect the Tier-2 public tip = MAX(height) WHERE ok=1? Can the repair ever lower the public tip now?\n' +
    '(c) Could keeping tip_finalize_log rows while deleting the upstream rows leave an inconsistent state worse than before (a tip_finalize_log row that references a utxo_apply row that no longer exists)?\n' +
    'Classify each finding severity and whether 811320cd7 introduced it.',
    { label: 'attack:stage-repair', phase: 'Attack', schema: ATTACK_SCHEMA, agentType: 'Explore' }),

  () => agent(BASE + '\n' +
    'TARGET: regressions in callers of reducer_ingest_block from the tightened acceptance gate. Callers: app/controllers/src/mining_controller.c (submitblock + internal mine, ~48 and ~199-220), config/src/boot_services.c (P2P/intake, ~893,905,917), app/controllers/src/repair_controller_rebuild.c (~178-185).\n' +
    'Try to find a case where the NEW requirement (utxo_apply ok=1 at height) makes reducer_ingest_block return FALSE for a block that SHOULD be accepted — a valid block rejected now that was accepted before. Probe:\n' +
    '(a) CATCH-UP / SYNC: during P2P catch-up the node ingests block H then H+1. When ingesting H, is utxo_apply guaranteed ok=1 at H after reducer_drain_to_convergence within the same call? If proof_validate is real/slow, could utxo_apply NOT yet have ok=1 at H, so a valid block is rejected and forward progress stalls? Read reducer_drain_to_convergence + the stage drain order in app/services/src/chain_activation_controller.c.\n' +
    '(b) submitblock of a freshly-mined valid tip: confirm utxo_apply ok=1 is present after the drain so it is still accepted (no miner regression).\n' +
    '(c) Any caller that treats a false return as fatal/ban-worthy and would mishandle a transiently-not-yet-applied block.\n' +
    'Also confirm the e2e accept keystone in lib/test/src/test_reducer_ingest_e2e.c still reflects correct behavior. Classify severity + whether 811320cd7 introduced it.',
    { label: 'attack:caller-regression', phase: 'Attack', schema: ATTACK_SCHEMA, agentType: 'Explore' }),
])

const results = attacks.filter(Boolean)
log('Attack phase done: ' + results.length + '/3 adversaries reported.')

phase('Adjudicate')
const SYNTH = {
  type: 'object',
  properties: {
    verdict: { type: 'string', enum: ['GO', 'FIX_MORE', 'NO_GO'] },
    rationale: { type: 'string' },
    must_fix_before_merge: { type: 'array', items: { type: 'string' } },
    note_but_dont_block: { type: 'array', items: { type: 'string' } },
  },
  required: ['verdict', 'rationale', 'must_fix_before_merge', 'note_but_dont_block'],
}
const synthesis = await agent('You are the merge/deploy gate. Commit 811320cd7 addressed a prior NO_GO audit; below are 3 adversarial re-attacks on the two consensus-critical fixes (acceptance gate + stage_repair no-delete).\n' +
  'Decide:\n' +
  '- NO_GO/FIX_MORE only if a CONSENSUS-SAFETY hole (an invalid block can become the tip or corrupt the UTXO / public tip) was found AND introduced or left open by this commit.\n' +
  '- A pre-existing dos-imprecision, or a wrong-return-value for a fork block that does NOT mutate chain state, is a note_but_dont_block item — record it but it does not block merging a strict improvement over the prior state.\n' +
  '- GO if the fixes are sound and only non-safety items remain.\n' +
  'Be decisive. Quote the strongest evidence.\n\n' +
  'ATTACK RESULTS (JSON):\n' + JSON.stringify(results, null, 2),
  { label: 'adjudicate', phase: 'Adjudicate', schema: SYNTH })

return { synthesis, attacks: results }
