export const meta = {
  name: 'wallet-w1-tests',
  description: 'Prove the existing wallet engine end-to-end on simnet: history, import/backup, reorg-restatement, shielded',
  phases: [
    { title: 'Test', detail: '4 isolated worktree lanes: history E2E, key import/export+backup E2E, reorg->wallet-balance, shielded E2E via librustzcash' },
  ],
}

const LANE_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['branch', 'verdict', 'files_changed', 'summary', 'gates'],
  properties: {
    branch: { type: 'string' }, verdict: { type: 'string', enum: ['MERGE_READY','NEEDS_WORK','BLOCKED'] },
    files_changed: { type: 'array', items: { type: 'string' } },
    summary: { type: 'string' }, gates: { type: 'string' },
    found_defect: { type: 'string', description: 'any real wallet bug the E2E test surfaced (or NONE)' },
    caveats: { type: 'string' },
  },
}

const COMMON = `zclassic23 is one C23 binary, a full ZClassic node + wallet (Equihash PoW, Sapling shielded + transparent txs), consensus-parity with zclassicd. The wallet is ALREADY fully implemented over JSON-RPC (transparent + shielded); this workflow adds COMPREHENSIVE END-TO-END TESTS on the deterministic offline simnet — no mainnet, no synced ledger needed.

TEST INFRA (reuse, do not reinvent): lib/sim/include/sim/simnet.h + lib/sim/include/sim/simnet_wallet.h give simnet_init, simnet_activate_sapling_at, simnet_mint_coinbase/_to/_to_height, simnet_spend, and the high-level wallet toolkit simnet_wallet_create/address/script/balance/fund/send/send_to_wallet/send_many/op_return; determinism via simnet_use_seed_tape. Model your new test on the existing ones: lib/test/src/test_simnet_txkit.c (fund/mature/balance/send/send_many), test_simnet_chained_tx.c (deterministic txid), test_simnet_cluster_reorg.c (competing branches / reorg convergence), test_simnet_sapling_shielded_send.c (real Groth16 t->z / z->z via the production librustzcash prover, params-gated). The real wallet RPC handlers to drive live in app/controllers/src/wallet_controller.c (register_wallet_rpc_commands, :598) and wallet_shielded_controller.c (:472).

Register a new test group in lib/test/src/test_parallel.c (TEST_LIST(X) macro + extern int test_<name>(void)) and add its agent_impact_rules.def mapping; reconcile DOC-COUNTS (tools/scripts/check_doc_counts.sh). Prefer a NEW test file (disjoint from other lanes). Your job is to TEST the existing engine and REPORT any real defect (found_defect) — fix a wallet .c ONLY if the E2E surfaces a genuine bug, and keep that fix minimal.

HARD RULES: consensus parity inviolable — do NOT change tx validity or the C23-native Groth16 prover (out of scope; the wallet ships on librustzcash). NEVER touch a live datadir (~/.zclassic-c23), the mint producer (~/.zclassic-c23-mint), or stop zclassicd. every malloc via zcl_malloc, every error return LOG_*, files < 800 lines. If your worktree cannot link, run make worktree-init first. You are in your OWN isolated git worktree — commit to THIS branch only; do NOT push/merge/touch others. Self-gate: make build-only + make t-fast ONLY=<your-group> + make lint. Final message = structured data, technical; report the branch name + found_defect.`

phase('Test')

const lanes = await parallel([
  () => agent(`${COMMON}

LANE W1-a transaction-history E2E. New file lib/test/src/test_simnet_wallet_history.c (group e.g. simnet_wallet_history). On simnet: create a wallet, fund it (mine coinbase to a wallet address, mature it), send to another wallet address, mine confirmations. Then drive the REAL history RPCs and assert: listtransactions returns the send + receive rows with correct amount/category(send|receive)/confirmations; gettransaction returns the tx detail (amount, fee, confirmations, hex); z_listreceivedbyaddress after a shielded receive (if params present — else skip that sub-case cleanly with a SKIP marker). Cover edge cases: empty wallet history, a self-transfer, an unconfirmed (0-conf) tx in the list. This closes the "no transaction-history test" gap. Files: the new test + test_parallel.c + agent_impact_rules.def + DOC-COUNTS. Only touch app/controllers/src/wallet_controller_history.c if a real bug is found.`,
    { isolation: 'worktree', model: 'sonnet', label: 'W1-a:history', phase: 'Test', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE W1-b key import/export + backup/restore E2E. New file lib/test/src/test_simnet_wallet_import_backup.c. Part 1 (import/export): on simnet, fund address A in wallet1, dumpprivkey(A), create a FRESH wallet2, importprivkey into wallet2, rescan, assert wallet2 now sees A's balance and can SPEND the imported coin (build+broadcast a real tx, mine it, assert spent). Assert dumpprivkey round-trips (WIF decodes back to the same key). Part 2 (backup/restore): use the wallet_backup_service (app/services/src/wallet_backup_service.c) to back up a funded wallet (incl. an ENCRYPTED backup via wallet_backup_crypto.c ChaCha20-Poly1305), then restore into a fresh datadir and assert keys + balance survive; assert a wrong passphrase fails closed. Files: the new test + registration. Touch wallet_controller_keys.c / wallet_backup_service.c only on a real defect.`,
    { isolation: 'worktree', model: 'sonnet', label: 'W1-b:import-backup', phase: 'Test', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE W1-c reorg -> wallet-balance restatement (HARD). New file lib/test/src/test_simnet_wallet_reorg.c on the test_simnet_cluster_reorg.c pattern. Build competing branches on simnet and assert the WALLET BALANCE (not just the coins-view) restates correctly across a reorg: (1) transparent — a coin received on the losing branch disappears from wallet balance after the reorg; a coin spent on the losing branch becomes unspent again; the winning branch's coins appear. (2) shielded (params-gated; SKIP cleanly without ~/.zcash-params) — a Sapling note received on the losing branch is removed, its nullifier un-marked, and witnesses/anchors roll back then forward so the wallet's z-balance is correct after reconverging. Assert no double-count and no negative balance at any step. This closes the "reorg->wallet-balance only covered at coins-view level" gap. If the wallet scan/witness rollback has a REAL restatement bug, fix it minimally in the ONE owning file (likely lib/wallet/src/wallet.c wallet_scan/advance_wallet_witnesses or app/services/src/wallet_scan_service.c) and note it in found_defect. Files: the new test + registration (+ at most one wallet file if a real bug).`,
    { isolation: 'worktree', model: 'opus', label: 'W1-c:reorg', phase: 'Test', schema: LANE_SCHEMA }),

  () => agent(`${COMMON}

LANE W1-d full shielded E2E via the production prover (params-gated) (HARD). Extend lib/test/src/test_simnet_sapling_shielded_send.c (or add lib/test/src/test_simnet_shielded_wallet_e2e.c in the same params-gated group) so that, WHEN ~/.zcash-params is present and the librustzcash prover self-test passes (zclassic_sapling_prover_is_ready), the test asserts an ACCEPTED valid Sapling spend end-to-end through the real C23 consensus verifier — i.e. drive z_sendmany for t->z, z->z, and z->t, mine the resulting tx, and assert contextual_check_transaction / the verifier ACCEPTS it (not merely verifier==probe agreement). Add full-wallet API assertions: z_getbalance / z_gettotalbalance / z_listunspent reflect the shielded receive and the change note; note-selection picks the right notes; memo round-trips via z_getmemo. Keep the existing verifier==probe self-consistency assertion for the C23-native prover as a SEPARATE, clearly-labeled sub-case (do NOT change consensus crypto or the native Groth16 prover). SKIP cleanly (SKIP marker) when params are absent so the default fast run stays green. Files: the shielded test file(s) + registration. Do NOT touch lib/sapling/*.c consensus/prover code.`,
    { isolation: 'worktree', model: 'opus', label: 'W1-d:shielded', phase: 'Test', schema: LANE_SCHEMA }),
])

return { lanes: lanes.filter(Boolean) }
