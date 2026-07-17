export const meta = {
  name: 'hardening-followups',
  description: 'Reorg wallet auto-reconciliation, error-discipline ratchet re-run, labels/address-book, stage-latency telemetry',
  phases: [ { title: 'Harden', detail: '4 isolated worktree lanes: reorg auto-reconcile, error ratchet, labels, stage telemetry' } ],
}
const LANE_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['branch','verdict','files_changed','summary','gates'],
  properties: {
    branch:{type:'string'}, verdict:{type:'string', enum:['MERGE_READY','NEEDS_WORK','BLOCKED']},
    files_changed:{type:'array', items:{type:'string'}}, summary:{type:'string'}, gates:{type:'string'}, caveats:{type:'string'},
  },
}
const COMMON = `zclassic23 is one C23 binary (full ZClassic node + wallet), consensus-parity with zclassicd. Offline simnet (lib/sim) mines blocks + builds real txs deterministically (no mainnet needed). HARD RULES: consensus parity inviolable; every malloc via zcl_malloc, every error return LOG_*, every write AR lifecycle; files < 800 lines (app/+config ENFORCED); every thread supervised; NEVER touch a live datadir / the mint producer / stop zclassicd. If your worktree cannot link, FIRST: cp -a /home/rhett/github/zclassic23/vendor/lib vendor/. OWN isolated worktree — commit to THIS branch only, do NOT push/merge/touch others. Self-gate: make build-only + focused test group + make lint (+ agent_impact_rules.def per changed .c; reconcile DOC-COUNTS / EXPECTED_DIAGNOSTICS_DUMPERS if a dumper). Final message = structured data, technical; report the branch name.`

phase('Harden')
const lanes = await parallel([
  () => agent(`${COMMON}

LANE reorg-auto-reconcile (funds-safety, HARD). A verified gap: across a real reorg the wallet balance restates only via an EXTERNAL rescan, not automatically. app/jobs/src/tip_finalize_post_step.c notifies the wallet only on CONNECT (wallet_sync_transaction) and does NO wallet reconciliation on a DISCONNECT; lib/wallet/src/wallet.c wallet_verify_utxos is invoked only from a diagnostic-repair controller (never the reorg path) and is ONE-DIRECTIONAL (prunes coins that vanished from the tip but never un-marks a coin that becomes unspent again after a reorg). Wire wallet reconciliation into the live reorg path: on a block DISCONNECT, call wallet_rollback_transaction for the disconnected block's wallet txs (un-mark transparent outpoints + Sapling nullifiers, remove losing-branch notes), and make the connect/disconnect path keep wallet spendable balance correct across a reorg WITHOUT a full rescan. CAREFUL: a naive symmetric un-mark is unsafe against unconfirmed mempool spends (double-spend risk) — only un-mark outpoints/nullifiers whose spending tx is no longer in the active chain AND not a still-valid mempool tx. Add a focused simnet test: build competing branches, assert transparent + shielded wallet balance restates automatically (no rescan) with no double-count / negative balance. Files: app/jobs/src/tip_finalize_post_step.c + lib/wallet/src/wallet.c (the reconciliation primitives) + a test. This is funds-safety-sensitive — keep the change tight and fully tested.`,
    { isolation:'worktree', model:'opus', label:'fix:reorg-reconcile', phase:'Harden', schema:LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE error-ratchet (re-run). tools/lint/silent_bool_errors_baseline.txt carries ~55 grandfathered sites that return a bool/-1 error WITHOUT logging context (check_silent_bool_errors.sh is RATCHET). Reduce the baseline: pick a batch (PRIORITIZE files NOT under lib/net/, lib/session/, lib/wallet/, app/jobs/proof_validate*, app/services/utxo_recovery* — other in-flight work owns those), add the proper LOG_FAIL/LOG_ERR/LOG_NULL/LOG_RETURN context to each offending return, remove those entries from the baseline, and confirm the gate passes with the smaller baseline. Pure behavior-preserving refactor — only add the missing log-and-return context, never change control flow. Files: the touched .c files + silent_bool_errors_baseline.txt. Gate: make lint (check_silent_bool_errors) + build-only + the focused test group per touched file's owner. Report how many sites you cleared (55 -> N).`,
    { isolation:'worktree', model:'sonnet', label:'os:error-ratchet', phase:'Harden', schema:LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE labels / address-book. The wallet has no Bitcoin-style label/address-book RPC. Add setlabel / getaddressesbylabel / listlabels RPC handlers + core.wallet.address.label / core.wallet.address.by-label native surface, backed by a small labels table (an AR model app/models/src/wallet_label.c + schema in database_schema.c or a feature migration). Label is a per-address string, unique-per-(address); getaddressesbylabel returns all addresses with a label. Add a focused test: set a label, read it back, list labels, re-label, and that an unknown address/label is handled cleanly. Files: the new model + schema + a controller (app/controllers/src/wallet_label_controller.c) + core.def + native bridge + test. Reconcile DOC-COUNTS / persistence-adapter counts if the schema adds a table.`,
    { isolation:'worktree', model:'sonnet', label:'W2-c:labels', phase:'Harden', schema:LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE stage-latency telemetry (OS observability). Per-reducer-stage p50/p99 latency is not exposed over Prometheus (the histogram machinery lib/metrics/src/prometheus_metrics.c currently serves MCP-request latencies only). Add a per-reducer-stage latency surface: register a stage_metrics Prometheus histogram (or gauge set) fed from the EXISTING per-stage step_us EWMA / step counters (grep <stage>_stage_step_us_ewma, lib/metrics/src/stage_metrics.c), exposing fold/sync p50-p99 over time via /metrics, complementing the rom_compile dumper. If a dumper field is added, reconcile EXPECTED_DIAGNOSTICS_DUMPERS + DOC-COUNTS. Add a focused test asserting the metric is populated from synthetic stage step timings. Files: lib/metrics/src/*.c + at most one wiring point + a test. Measured/real timings only — no fabricated numbers.`,
    { isolation:'worktree', model:'sonnet', label:'os:stage-metrics', phase:'Harden', schema:LANE_SCHEMA }),
])
return { lanes: lanes.filter(Boolean) }
