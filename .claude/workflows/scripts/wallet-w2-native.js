export const meta = {
  name: 'wallet-w2-native',
  description: 'Wire the native agent wallet write-interface + interactive lock/unlock + re-run the 529-failed W1 tests',
  phases: [ { title: 'Build', detail: '4 isolated worktree lanes: native mutating commands, lock/unlock, history E2E, import/backup E2E' } ],
}
const LANE_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['branch','verdict','files_changed','summary','gates'],
  properties: {
    branch: {type:'string'}, verdict: {type:'string', enum:['MERGE_READY','NEEDS_WORK','BLOCKED']},
    files_changed: {type:'array', items:{type:'string'}}, summary:{type:'string'}, gates:{type:'string'},
    found_defect:{type:'string'}, caveats:{type:'string'},
  },
}
const COMMON = `zclassic23 is one C23 binary (full ZClassic node + wallet), consensus-parity with zclassicd. The wallet is ALREADY fully implemented over JSON-RPC (transparent + shielded; real librustzcash prover). The NATIVE/agent command taxonomy (config/commands/core.def core.wallet.*) has READ paths wired (ZCL_COMMAND_READY_READ, bridged to RPC via tools/command/native_command.c) but every MUTATING command is ZCL_COMMAND_PLANNED_COMMAND — unbound, fails closed exit 3 ("Wave 2.2 deliverable"). The deterministic offline simnet (lib/sim/include/sim/simnet.h + simnet_wallet.h) mines blocks + builds real txs with NO mainnet needed.

HARD RULES: consensus parity inviolable (no tx-validity / prover change); every malloc via zcl_malloc, every error return LOG_*, every write AR lifecycle; files < 800 lines (app/+config ENFORCED); NEVER touch a live datadir (~/.zclassic-c23), the mint producer (~/.zclassic-c23-mint), or stop zclassicd. If your worktree cannot link, run make worktree-init first. You are in your OWN isolated git worktree — commit to THIS branch only; do NOT push/merge/touch others. Self-gate: make build-only + make t-fast ONLY=<group> + make lint (+ agent_impact_rules.def per changed .c; reconcile DOC-COUNTS). Final message = structured data, technical; report the branch name.`

phase('Build')
const lanes = await parallel([
  () => agent(`${COMMON}

LANE W2-a: bind the mutating core.wallet.* native commands to the real RPC engine. Replace the ZCL_COMMAND_PLANNED_COMMAND stubs in config/commands/core.def (~456-668) for: core.wallet.address.new, core.wallet.address.import, core.wallet.address.export-key, core.wallet.transaction.send, core.wallet.shielded.send, core.wallet.rescan, core.wallet.backup.now — with real handlers that call the existing RPC handlers (getnewaddress / importaddress / dumpprivkey / sendtoaddress|sendmany / z_sendmany / rescanblockchain / the backup service). Honor the DECLARED contract: keep AUTH_OWNER + CONFIRM_PLAN_COMMIT for sends and export-key (implement the plan->commit handshake), RISK_WALLET/EFFECT_MUTATE semantics, bounded output. Wire the handler bodies in app/controllers/src/wallet_native_handlers.c (+ the bridge in tools/command/native_command.c following the existing read-path pattern at :111-118,229-232). Add a focused test (native mutating command E2E on simnet: address.new persists an address; transaction.send moves funds through plan->commit). Reconcile DOC-COUNTS + the command-contract lint gate (distinct semantics per leaf).`,
    { isolation:'worktree', model:'opus', label:'W2-a:native-writes', phase:'Build', schema:LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE W2-b: interactive wallet lock/unlock. The wallet has at-rest encryption keyed off env ZCL_WALLET_PASSPHRASE (lib/wallet/src/wallet_keystore.c) but NO Bitcoin-style interactive lock/unlock. Add encryptwallet / walletpassphrase / walletlock RPC handlers (a new app/controllers/src/wallet_lock_controller.c or into wallet_controller.c if it fits under 800 lines) layered on the existing keystore envelope, plus core.wallet.lock / core.wallet.unlock native commands (config/commands/core.def + native bridge). Semantics: unlock decrypts the spending keys into memory for a timeout; lock zeroizes; a spend while locked is refused with a clear error. Add a focused test: encrypt -> spend refused (locked) -> walletpassphrase unlock -> spend allowed -> walletlock -> spend refused again -> auto-relock after timeout. Files: the new controller + core.def + native bridge + test. Do NOT weaken the at-rest crypto.`,
    { isolation:'worktree', model:'sonnet', label:'W2-b:lock-unlock', phase:'Build', schema:LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE W1-a (re-run): transaction-history E2E. New file lib/test/src/test_simnet_wallet_history.c (group simnet_wallet_history). On simnet: create a wallet, fund it (mine coinbase to a wallet address, mature), send to another wallet address, mine confirmations. Assert the REAL history RPCs: listtransactions returns send+receive rows (amount/category/confirmations); gettransaction returns detail (amount, fee, confirmations, hex); z_listreceivedbyaddress after a shielded receive (params-gated — SKIP cleanly without ~/.zcash-params). Edge cases: empty history, self-transfer, an unconfirmed (0-conf) tx. Register in test_parallel.c + agent_impact_rules.def; reconcile DOC-COUNTS. Only touch wallet_controller_history.c if a real bug is found (report found_defect).`,
    { isolation:'worktree', model:'sonnet', label:'W1-a:history', phase:'Build', schema:LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE W1-b (re-run): key import/export + backup/restore E2E. New file lib/test/src/test_simnet_wallet_import_backup.c. Part 1: on simnet fund address A in wallet1, dumpprivkey(A), create FRESH wallet2, importprivkey into wallet2, rescan, assert wallet2 sees A's balance and can SPEND the imported coin (build+broadcast+mine, assert spent); assert dumpprivkey WIF round-trips. Part 2: wallet_backup_service (app/services/src/wallet_backup_service.c) backup of a funded wallet incl. an ENCRYPTED backup (wallet_backup_crypto.c ChaCha20-Poly1305), restore into a fresh datadir, assert keys+balance survive, assert wrong passphrase fails closed. Register + reconcile DOC-COUNTS. Touch wallet .c only on a real defect (report found_defect).`,
    { isolation:'worktree', model:'sonnet', label:'W1-b:import-backup', phase:'Build', schema:LANE_SCHEMA }),
])
return { lanes: lanes.filter(Boolean) }
