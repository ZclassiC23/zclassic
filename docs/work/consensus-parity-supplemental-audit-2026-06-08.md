# Consensus parity backlog (c23 vs zclassicd) — supplemental audit (2026-06-08)

Source of truth: zclassicd reference at `/home/rhett/zclassic-cpp` (origin
`ZclassicCommunity/zclassic`). c23 audited at the post-fix state of branch
`consensus/zclassicd-parity-2026-06-08` (FR-1 expiry, FR-2 finality, FR-3
tx-size, miner cap already landed). Method: 8 consensus surfaces read against
both trees, every candidate divergence adversarially refuted by 2 skeptics
(refute-by-default), then synthesized. 6 confirmed, 4 refuted, 111 sub-rules
verified decision-identical.

## 1. Executive summary

**HISTORY-FORKING ALARM: NONE proven.** No divergence rewrites the verdict on
any already-accepted block. All 6 are practically **forward-forking**: each
diverges only at/near the live tip or on future/crafted blocks. Every
historical block already passed at-tip validation on the live network, and a
strictly-more-permissive c23 cannot disagree with a zclassicd accept. (The
inverse — these fixes *tightening* c23 — is also history-safe: zclassicd already
enforces every one of these rules, so the immutable chain contains no block that
violates them. Adding the rule to c23 cannot newly-reject history, exactly the
FR-3 argument.)

**Root pattern:** c23 ports the per-tx / per-block *contextual* rules faithfully
but **never wires them into the production block-connect path**.
`contextual_check_block()` has ZERO production callers; the reducer connect path
runs only context-free structural checks plus the staged proof gate. Four of six
confirmed items share this single root cause; wiring that one call (with an IBD
early-return gate) closes #2, #3, #4 together. #1 and #5 are localized adds.

| # | Surface | Class | Reachability |
|---|---------|-------|--------------|
| 1 | CHECKDATASIG sigop undercount | history-applicable rule / forward-reachable | adversarial/future block crossing 20000 sigops |
| 2 | ContextualCheckBlock per-tx rules unwired (expiry / version-gating / finality / bad-cb-height) | forward | live/future tip |
| 3 | JoinSplit Ed25519 sig not on connect path | forward | live/future tip |
| 4 | Height-gated Sapling/Overwinter structural rules unwired | forward | live/future tip |
| 5 | Coinbase-must-be-shielded rule absent (`bad-txns-coinbase-spend-has-transparent-outputs`) | forward | live tip & forever forward |

## 2. CONFIRMED divergences

### #1 — CHECKDATASIG/CHECKDATASIGVERIFY counted as 0 sigops (block sigop undercount)
- **c23:** `domain/consensus/src/check_block.c:168-169` (CheckBlock passes
  `SCRIPT_VERIFY_NONE`); `lib/validation/src/connect_block.c:286` (ConnectBlock
  `flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY`), feeding the
  legacy tally at `:358` and the P2SH tally at `:429`. Counter itself is correct:
  `lib/script/src/script.c:109-113` gates the `+1` on bit 11.
- **zclassicd:** `src/main.cpp:4006` (CheckBlock via `STANDARD_SCRIPT_VERIFY_FLAGS`,
  which includes the bit per `src/script/standard.h:62`); `src/main.cpp:2567`
  (ConnectBlock `... | SCRIPT_VERIFY_CHECKDATASIG_SIGOPS`) used at `:2615`/`:2634`.
- **Decision that differs:** c23 omits `SCRIPT_VERIFY_CHECKDATASIG_SIGOPS` (bit 11)
  at both block-sigop count sites, so every top-level `OP_CHECKDATASIG` (0xba) /
  `OP_CHECKDATASIGVERIFY` (0xbb) contributes **0** sigops in c23 vs **+1** in
  zclassicd. A block whose true sigop total crosses `MAX_BLOCK_SIGOPS=20000` only
  via these opcodes is ACCEPTED by c23 / REJECTED by zclassicd (`bad-blk-sigops`).
  Bit 11 affects sigop counting only, never EvalScript execution.
- **Mainnet reachability:** Rule is unconditional (active since genesis). Crossing
  20000 needs ~20000 *top-level* CHECKDATASIG opcodes in one block — exotic; no
  known historical block crosses it (most 0xBA/0xBB bytes are push-data, skipped by
  the GetOp walker in both trees). History-safe (zclassicd counted these and
  rejected any crosser, so the chain has none).
- **Fix:** Add bit 11 at both count sites — (a) `connect_block.c:286` OR-in the bit
  (covers `:358` and `:429`); (b) `check_block.c:169` replace `SCRIPT_VERIFY_NONE`
  with a flags value including bit 11. Define one shared `CONSENSUS_SIGOP_COUNT_FLAGS`
  so the two can't drift. verify_script sites need no change (bit 11 is a no-op for
  execution). Regression test: a tx with >20000 CHECKDATASIG ops must be
  `bad-blk-sigops` in both CheckBlock and ConnectBlock.

### #2 — ContextualCheckBlock per-tx height-gated rules NOT enforced on connect (`contextual_check_block` has zero production callers) — ROOT
- **c23:** `lib/validation/src/check_block.c:390-414` — `contextual_check_block` is
  orphaned (callers are only tests `test_bip113_bip65.c`, `test_chain.c`). It would
  call `contextual_check_transaction` (`lib/validation/src/contextual_check_tx.c:32`;
  expiry branch `:67-74`) and `is_final_tx` (`check_block.c:412`). The live path
  `connect_block.c:125` → `check_block()` runs only context-free `check_transaction`
  (`domain/consensus/src/tx_structural.c`). `bad-cb-height` (`check_block.c:417`) is
  likewise stranded inside the orphan.
- **zclassicd:** `src/main.cpp:4070` `ContextualCheckBlock` → `:4079`
  `ContextualCheckTransaction(tx,state,nHeight,100)` (expiry `:1012-1015`, version
  gating `:949-1000`) and `:4086` `IsFinalTx` → `bad-txns-nonfinal`; invoked
  unconditionally from AcceptBlock (`:4203`) and TestBlockValidity (`:4311`).
  `ContextualCheckTransaction` short-circuits `return true` only while
  `IsInitialBlockDownload()` is true, which latches false permanently at tip
  (`:1814-1837`).
- **Decision that differs:** On connect, c23 never runs any height-gated per-tx
  rule — Overwinter expiry (`tx-overwinter-expired`), network-upgrade version gating,
  per-tx finality (`bad-txns-nonfinal`), BIP34 `bad-cb-height`. A tip block
  containing an expired, non-final, or height-inappropriate-version tx is ACCEPTED
  by c23 / REJECTED by at-tip zclassicd → permanent fork. (Shielded *proofs* are
  separately enforced via the staged proof reducer — not part of this.)
- **Fix:** Invoke `contextual_check_block(block, state, params, pindex->pprev)` on
  the production connect path — inside `connect_block.c` near the `check_block()`
  gate (`:125`) and/or the reducer ingest gate
  (`app/services/src/reducer_ingest_service.c:271`). **Critically, gate it with the
  same IBD semantics zclassicd uses** (thread an `isInitBlockDownload` predicate
  into `contextual_check_transaction` so it is a no-op during IBD) — otherwise c23
  becomes *stricter* than zclassicd during sync and could reject historical blocks
  (an opposite fork). The finality cutoff already correctly uses
  `block.GetBlockTime()` (`check_block.c:411` == `main.cpp:4083-4086`, our FR-2).

### #3 — JoinSplit Ed25519 signature NOT verified on the block-connection consensus path
- **c23:** `lib/validation/src/contextual_check_tx.c:99-103` (the `ed25519_verify`
  exists) but its only non-test caller is `accept_to_mempool.c:125` (mempool) + the
  dead `contextual_check_block`. The authoritative connect gate — reducer
  `app/services/src/reducer_ingest_service.c:271` → `check_block()` (structural only)
  and the proof gate `app/jobs/src/proof_validate_stage.c:101-220`
  (`default_verify_tx`) — verifies Sapling spend/output/binding + Sprout zk-SNARK but
  **OMITS** the JoinSplit Ed25519 sig. The only ed25519 check on a live-block path is
  background `app/services/src/bg_validation_proofs.c:54-61`; on failure
  `bg_validation_service.c:~519` only sets `BG_VALIDATION_FAILED` — no invalidate, no
  reorg.
- **zclassicd:** `src/main.cpp:1044-1058` (`crypto_sign_verify_detached` over
  `dataToBeSigned`, reason `bad-txns-invalid-joinsplit-signature`) inside
  `ContextualCheckTransaction`, run per-tx by `ContextualCheckBlock`.
- **Decision that differs:** A block with a Sprout-joinsplit tx carrying a valid
  zk-proof but a **forged** `joinSplitSig` is accepted and finalized by c23 (proof
  gate never checks the Ed25519 sig) / rejected DoS(100) by zclassicd. The zk-SNARK
  binds `joinSplitPubKey` into `h_sig` but does NOT substitute for the Ed25519
  signature over the tx sighash.
- **Fix:** In `proof_validate_stage.c:default_verify_tx`, after computing `sighash`
  (`:137`), when `tx->num_joinsplit > 0` call `ed25519_verify(tx->joinsplit_sig,
  sighash.data, 32, tx->joinsplit_pubkey.data)` before the per-joinsplit zk-SNARK
  loop; on failure set `out->ok=false`, `first_failure_proof_type="joinsplit_sig"`.
  Propagates `ok=0` → `utxo_apply` `upstream_failed` → blocks `tip_finalize`.
  Respect the existing `g_deferred_proof_validation_below_height` defer semantics.
  **(Codex-adjacent: this file is on Codex's forward-sync proof/script path —
  coordinate.)**

### #4 — Height-gated Sapling/Overwinter structural tx rules NOT enforced on connect
- **c23:** `domain/consensus/src/sapling_structural.c:36-161` (overwinter-not-active,
  tx-overwintered-flag-not-set, bad-sapling-tx-version-group-id,
  bad-tx-sapling-version-too-low/high, tx-overwinter-active, pre-Sapling
  bad-txns-oversize) is reached only via `contextual_check_tx.c:46-56`, whose only
  non-test callers are `accept_to_mempool.c:125` (mempool) and the dead
  `contextual_check_block`. The connect gate enforces only the height-**independent**
  version rules.
- **zclassicd:** `src/main.cpp:935-1025` per-tx via `ContextualCheckBlock`;
  Overwinter+Sapling both activate at `476969`.
- **Decision that differs:** At height ≥ 476969, a block whose tx is non-overwintered
  (Sprout-format), carries the wrong version-group-id for the height, or an
  out-of-range Sapling version, passes c23's connect gates and is finalized, while
  zclassicd rejects it. Note the OVERWINTER group-id is accepted by `tx_structural`
  at a Sapling height, so the Sapling-group-id rule genuinely escapes the
  context-free path.
- **Fix:** Same surface as #2 — call the already-correct `contextual_check_block`
  from the connect path, **gated with the IBD early-return**. Regression test: a
  v4-non-overwintered tx and a Sprout-format tx at height ≥ 476969 are rejected at
  connect, not just at mempool.

### #5 — Missing rule: coinbase outputs must be spent only to shielded outputs
- **c23:** `lib/validation/src/connect_block.c:366-385` — the coinbase-input branch
  has the maturity check (`:373-374`) but **no** coinbase-protection check. Config
  flags exist but are never read: `lib/chain/src/chainparams.c:107`
  (`fCoinbaseMustBeProtected=true` mainnet; `:577` false regtest) and
  `lib/validation/include/validation/main_state.h:41,73`
  (`fCoinbaseEnforcedProtectionEnabled=true`). Both write-only. Mempool and the
  reducer value path (`utxo_apply_delta.c:155-316`) also lack it.
- **zclassicd:** `src/main.cpp:2062-2070` inside `Consensus::CheckTxInputs`; enabled
  by `src/main.cpp:76` + `src/chainparams.cpp:85`.
- **Decision that differs:** zclassicd REJECTS (`bad-txns-coinbase-spend-has-transparent-outputs`)
  any tx that spends a coinbase output and has a non-empty transparent `vout`; the
  rule is absent in c23, which ACCEPTS such a tx. Not height-gated (active from
  genesis on mainnet; disabled only on regtest).
- **Fix:** In the production value/coinbase path (reducer `utxo_apply_delta.c`
  non-coinbase branch, where spent-input `is_coinbase` is already known; and/or
  `connect_block.c` after the maturity check), when `prev_coins.is_coinbase`, flags
  enabled, and `tx->num_vout != 0`: reject with
  `bad-txns-coinbase-spend-has-transparent-outputs`. Thread the
  `fCoinbaseEnforcedProtectionEnabled` flag into the path (not currently in
  `struct consensus_params`). Consume existing flags (no new config); the regtest
  carve-out is automatic. Also add to mempool relay.

## 3. Refuted candidates (do not re-investigate)

- **pow-diffadj operator-precedence in `scaleDifficultyAtUpgradeFork`/BUTTERCUP
  window** (`c23 pow.c:50-55` vs `zclassicd pow.cpp:45-49`): mainnet sets
  `scale=true`, both parses identical at every mainnet height incl. BUTTERCUP. Only
  testnet/regtest observable.
- **BIP34 height encoding for heights 1-16** (`check_block.c:357-385` vs
  `main.cpp:4096-4103`): real chain mined heights 1-16 via `OP_N`; no future block
  can have height ≤ 16. Unreachable on mainnet.
- **Missing Sprout value-pool checkpoint seed (ZIP209 turnstile @440329):** flips a
  decision only if a future block drives the Sprout pool negative, which the verified
  joinsplit zk-SNARKs already prevent. Dormant defense-in-depth.
- **BIP30 same-height self-write tolerance** (`connect_block.c:261-276` vs
  `main.cpp:2560-2565`): post-BIP34 coinbase txids are height-unique; the tolerance
  fires only on a kill-9 partial-apply residue of the same block. Recovery-path only,
  decision-identical on the normal path.

## 4. Clean surfaces verified decision-identical (111 sub-rules)

Spot-confirmed identical between trees:
- **Context-free structural tx checks** (`tx_structural.c` vs
  `CheckTransactionWithoutProofVerification`): version floors, version-group-id
  accept set, vin/vout-empty, `MAX_TX_SIZE_AFTER_SAPLING=102000`, value/MoneyRange,
  valueBalance sign, joinsplit vpub bounds, duplicate inputs/joinsplit/sapling
  nullifiers, coinbase shape, scriptSig length [2,100], null-prevout — DoS scores +
  reject strings byte-identical. Wired on the connect path. All consensus constants
  match.
- **`transaction_is_coinbase` / `outpoint_is_null` / `is_expired_tx` (strict `>`)** —
  identical.
- **Shielded proof verification** (Sapling spend/output/binding Groth16 + spendAuthSig,
  Sprout Groth16/PHGR13) IS enforced via `proof_validate_stage.c` reducer gating
  `utxo_apply` — same decision, different mechanism. (Only JoinSplit Ed25519 sig
  missing → #3.)
- **Script-verify:** branch IDs (incl. intentional Bubbly==Buttercup `0x930b540d`),
  activation heights, CurrentEpoch/BranchId selection, Sapling/Overwinter + Sprout
  SignatureHash, SIGHASH type handling, strict-DER (always on), LOW_S/STRICTENC
  (flag-gated, OFF in both consensus paths), all script-flag bit values, P2SH
  (genesis-active), CLTV, CSV absent (OP_NOP3 plain NOP) in both, NULLDUMMY/NULLFAIL/
  SIGPUSHONLY/CLEANSTACK/MINIMALDATA (OFF in both), OP_CHECKDATASIG **execution**
  identical, legacy sigop walker mechanics, `MAX_BLOCK_SIGOPS=20000`, sig-version —
  identical.
- **Block-contextual:** merkle root + CVE-2012-2459 mutation detection,
  first/only-coinbase, txn-count/size bounds, BIP34 height for heights >16, equihash
  solution size + (N,K) per-epoch table (200,9 pre-585318 → 192,7 from BUBBLES),
  CheckBlockHeader 4 checks (`MIN_BLOCK_VERSION=4`), MTP time-too-old (`<=`), version
  floor, `bad-diffbits`, per-tx finality cutoff (header time, our FR-2) — identical.
- **pow-diffadj:** all constants, activation heights/protocol versions,
  `GetNextWorkRequired` control flow + min-difficulty ramp, `CalculateNextWorkRequired`
  retarget math (div-before-mul truncation order preserved), `IncreaseDifficultyBy`,
  `CheckProofOfWork`, `CheckEquihashSolution`, `arith_uint256` SetCompact/GetCompact,
  `GetBlockProof`, `GetMedianTimePast` (11-block), upgrade-state gate (PoWTargetSpacing
  150→75 at BUTTERCUP 707000), `nPowAllowMinDifficulty` disabled on mainnet —
  identical.
- **subsidy-founders:** base reward `12.5*COIN`, slow-start ramp, halving (pre/post-
  BUTTERCUP triple-halving), intervals (840000/1680000), `halvings>=64` guard, all 48
  founders addresses byte-identical and in order, `GetLastFoundersRewardBlockHeight=
  840000`. Founders reward is NOT consensus-enforced in either tree (vestigial Zcash
  vector) — decision-identical. Coinbase value cap `bad-cb-amount` identical.
- **upgrade-heights:** activation heights, protocol versions, branch IDs,
  `enum upgrade_index` ordinals, Equihash N/K table, ALWAYS_ACTIVE/NO_ACTIVATION
  sentinels, no stock-Zcash upgrades active. Two cosmetic non-fork bugs (off accept
  path): `consensus_is_branch_id`/`consensus_is_activation_height_any` unconditionally
  return true (no caller); `chain_inspect_controller.c:148-150` `epoch_names[]`
  mislabels a display JSON string.
- **coinbase-maturity-bip30:** `COINBASE_MATURITY=100`, maturity comparison (`<100`),
  BIP30 core predicate (no BIP34 carve-out — Zclassic never had the duplicate-coinbase
  blocks), deferred-validation skip below snapshot height, BIP34 height-uniqueness —
  identical.
- **shielded-consensus (clean portion):** in-tx duplicate nullifier detection (Sprout +
  Sapling), valueBalance sources/sinks + overflow, shielded value-pool transparent
  accounting, ZIP209 turnstile on a from-genesis chain, Sapling/Sprout proof gating in
  `proof_validate_stage.c`, shielded sighash, activation heights. (Known FR-4
  `bad-sapling-root-in-block` and FR-5 `HaveShieldedRequirements` stub owned by Codex.)

## Cross-cutting fix note

#2, #3, #4 share one root cause — the connect/reducer path does not invoke
`contextual_check_transaction`/`contextual_check_block`. Wiring that single call
(with the IBD early-return gate to avoid the opposite over-strict fork) closes #2,
#3, and #4 together; #1 and #5 are independent localized adds. Implement on a datadir
COPY-proven branch; do not weaken the must-never-fork gates.
