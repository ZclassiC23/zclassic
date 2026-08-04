# Transaction laboratory notebook

This notebook answers two different questions without blending them:

1. Can the current code build, sign, prove, validate, and settle each supported
   transaction family in an isolated deterministic environment?
2. Has that transaction family actually confirmed on mainnet under the bounded
   dev custody grant?

The first answer can be green while the second remains zero. Run
`make transaction-lab-status` for both progress bars and cumulative value/fee
statistics. Run `make transaction-lab-proof` to reproduce the isolated proof
matrix; it uses production transaction builders, ECDSA signing, Sapling proving
and verification, the consensus script interpreter, and the isolated settlement
projections named in `tools/dev/transaction_lab_catalog.def`.

For the complete machine-readable inventory—including receive-only, contained,
planned, ZID/ZDIR/ZANC, and ZCODE shapes—start with
`zclassic23 app transaction-types list`; field meanings and the AI workflow are
documented in the [transaction API guide](../TRANSACTION_API.md).

## Safety boundary

- Mainnet uses only the explicitly bound `dev` wallet scope.
- Total recipient value plus fees may not exceed `0.05000000 ZCL`; the
  `0.25000000 ZCL` development reserve remains untouched.
- A live transaction requires a current identity-bound money snapshot and the
  owner-visible vault plan/commit path. Raw observed balance is not authority.
- Two fresh isolated recipient wallets are required before live funding.
- No automatic transfer or rebalance is permitted.
- The notebook never stores addresses, endpoints, datadir paths, grant tokens,
  private keys, recovery words, memos, or swap secrets. A mainnet txid is public
  and may be recorded after broadcast.
- `docs/HANDOFF.md` currently says not to start the mainnet transaction lab.
  That operational gate and all custody checks must be clear first.

## Evidence vocabulary

| Proof | Meaning |
|---|---|
| `builder_verified` | Production builder emitted the expected signed or unsigned transaction shape. |
| `interpreter_verified` | The consensus script interpreter accepted the intended spend branch and rejected invalid branches. |
| `projection_verified` | An isolated confirmed-payment fixture was reconciled by the application projection. |
| `consensus_verified` | The production consensus transaction verifier accepted the complete transaction. |
| `simnet_confirmed` | The transaction was admitted or mined on the deterministic simulated chain. |
| `live_confirmed` | A public mainnet txid reached the required confirmation state. |
| `not_demonstrated` | No end-to-end transaction path exists; the case must remain `BLOCKED`, never PASS. |

Only `live_confirmed` increments the live-mainnet bar or cumulative live value
and fee totals.

## Append-only event ledger

The canonical ledger is `docs/work/transaction-lab-events.jsonl`, schema
`zcl.transaction_lab_event.v1`. Existing lines are evidence and must not be
edited or reordered; corrections are later events for the same `case_id`.
Statistics use the latest event per case.

Validate before and after recording:

```bash
make transaction-lab-check
tools/dev/transaction-lab.sh record \
  --case=transparent_t_to_t --network=mainnet --proof=live_confirmed \
  --result=PASS --source=owner_visible_receipt --txid=<64-lowercase-hex> \
  --recipient-zat=<integer> --fee-zat=<integer>
make transaction-lab-status
```

The recorder accepts no address, path, endpoint, memo, token, or secret field.
Recording is bookkeeping only; it cannot build, sign, broadcast, or authorize a
transaction.

## 2026-08-03 isolated run

Source commit `07091e0f`:

- 10 exact groups ran across the base ZCL, Sapling memo, ZSLP, ZNAM, HTLC, and
  store-payment families.
- Transparent `t→t` was signed, admitted to the mempool, mined, and observed at
  the recipient in simnet.
- Sapling `t→z` and `z→z` were built with the native C23 Groth16 prover,
  consensus-verified, mined, decrypted, and checked for nullifier replay.
- Sapling `z→t` was built with the production spend prover and accepted by the
  consensus verifier.
- A shielded ZMSG memo transaction was proved, decrypted, ingested, and mined.
- ZSLP genesis/mint/send/burn and all six ZNAM mutation encodings passed their
  production builder/validation suites.
- HTLC funding assembly produced deterministic fully signed bytes; redeem and
  refund passed the consensus P2SH+CLTV interpreter, including negative cases.
- Transparent and shielded store settlement projections credited only the
  correctly bound confirmed payment.
- Live mainnet transactions: **0**. Live recipient value: **0 ZCL**. Live fees:
  **0 ZCL**. No live wallet or service was mutated.

## 2026-08-04 catalog-complete run

Source commit `cb8ab59d` plus the append-only notebook expansion in this change:

- The lab catalog now has one case for every one of the 33 semantic types in
  `app transaction-types list`; the Make target derives its exact test groups
  from that catalog, so a future case cannot be silently omitted from the
  proof command.
- Nine additional exact groups passed: coinbase/simnet, raw native transaction
  adapters, Sprout transaction processing, ZID identity transitions, ZDIR
  write paths, epoch/ZANC, ZCODE release anchoring, yardsale final transaction,
  and file-market protocol behavior.
- The catalog-derived `make transaction-lab-proof` target passed all 18 unique
  exact groups (0 failed, 0 skipped), including the ten groups retained from
  the earlier notebook run.
- ZID ANCHOR/ROTATE/REVOKE and ZDIR REGISTER/DEREGISTER bytes were parsed and
  folded into their isolated projections. The ZCODE release batch produced a
  deterministic root and a ZANC OP_RETURN that round-tripped. The yardsale
  ceremony produced its golden fully signed final transaction and delivered it
  to the broadcast port.
- `market_purchase` is the one `BLOCKED` case: file-market payment-to-unlock
  glue has no end-to-end broadcast path. The seller-authenticated,
  network-bound offer ingress contract is implemented and focused-test proven,
  but it is P2P setup—not a payment proof. The follow-up exact-payment slice
  rejects the old mempool-only notification, binds a signed buyer claim to the
  offer/network/range/amount and canonical 512-byte Sapling memo, and proves
  confirmation, restart reconstruction, reorg revocation, and reconfirmation
  against wallet-note + chain authorities. At this run, buyer wallet
  plan/commit and the authenticated encrypted file request were absent, so the
  proof stayed `not_demonstrated` and was deliberately not counted as PASS.
- Current notebook result: **32/33 PASS**, **1 BLOCKED**, **5 simulated chain
  confirmations**, **0 mainnet confirmations**, and **0 ZCL** live recipient
  value or fees. No live wallet or service was mutated.

## 2026-08-04 durable buyer-payment slice

The next file-market slice closed the chain-payment subflow without relabeling
the unfinished composite purchase:

- `app market purchase plan` now loads the exact authenticated offer and range,
  creates a buyer credential and canonical 512-byte Sapling memo behind wallet
  metadata encryption, and atomically reserves recipient value plus maximum
  fee through the existing vault-intent authority.
- The plan is bound to explicit `dev|prod` scope, random persistent wallet
  instance, network genesis, current tip hash, complete money snapshot root,
  exact offer/range/amount/fee, expiry, and caller idempotency key. A stale,
  incomplete, wrong-wallet, over-cap, or reserve-floor state refuses.
- `app market purchase commit` revalidates every binding and source ownership,
  claims the durable intent once, sends the exact seller output and memo, then
  persists the txid and reconstructible signed claim. Replay can requeue the
  claim but cannot send a second payment.
- Restart reconstructs the public claim without exposing the buyer seed. A
  persisted `PROVING` row without txid reports `COMMIT_UNCERTAIN` and never
  retries a possibly broadcast spend. Changed tip-bound state conflicts before
  the wallet send callback.
- Focused proof covers the service, application-bound intent uniqueness,
  migration idempotency, typed plan/commit/status surface, and absence of
  source/seller/memo/path fields in native output.

The ledger result deliberately remains **32/33 PASS, 1 BLOCKED**. The
`market_purchase` catalog row represents payment *plus* completed file
delivery; the buyer still lacks targeted seller notification, encrypted chunk
retrieval, full-manifest verification, restart-safe assembly, and atomic
destination publication. Live mainnet confirmations, recipient value, and fees
remain **zero**; this slice used deterministic callbacks and moved no ZCL.

## 2026-08-04 complete file-market purchase slice

Source commit `c95264fe` completes the previously blocked composite without a
live-wallet mutation:

- The buyer connects to the exact IP/port authenticated by the signed offer,
  performs the real encrypted file-service handshake, signs a request bound to
  that session and paid offer, and accepts bytes only after the typed seller
  authorization reply.
- A schema-v59 ActiveRecord resource stores only owner-private destination and
  staging paths plus immutable plan/offer/manifest terms and sequential chunk
  progress. Public command output remains path-, endpoint-, address-, memo-,
  and key-free.
- Restart reopens the same download, truncates an uncommitted tail, and rehashes
  every fsynced chunk against its durable child record before fetching more.
- Every reply hash is checked, the ordered chunk-hash manifest must equal the
  signed offer root, and publication is an atomic same-directory no-overwrite
  operation. Replay neither downloads nor publishes twice.
- The focused group exercises a confirmed-payment projection and complete
  purchase service, including real encrypted loopback delivery, restart,
  manifest verification, publication, and replay safety. This supports the
  catalog's `projection_verified` proof; it is not a simnet or mainnet
  confirmation claim.

The latest notebook result is **33/33 PASS, 0 BLOCKED**, with **5 simulated
chain confirmations**, **0 mainnet confirmations**, and **0 ZCL** live
recipient value or fees. No live wallet or service was mutated.

## 2026-08-04 completeness audit: ZBLG gap made visible

The 33/33 result above was accurate for every semantic type declared at that
time, but a second inventory audit found one implemented chain format that the
catalog had omitted: the strict `ZBLG` v1 OP_RETURN commitment used by the Blog
projection. Hiding an incomplete path by omitting it would make the aggregate
misleading, so `blog_anchor` is now the 34th catalog and notebook case.

The production codec builds and parses the ZNAM-bound signed-event commitment,
and the projection observes it. Those facts do not demonstrate an end-to-end
transaction: there is intentionally no public event-signing constructor and no
broadcast command. The case is therefore `planned`, `not_demonstrated`, and
`BLOCKED` until a bounded wallet plan/commit path and isolated confirmation
proof exist.

The corrected current result is **33/34 PASS, 1 BLOCKED**, with **5 simulated
chain confirmations** across **19 exact proof groups**, **0 mainnet
confirmations**, and **0 ZCL** live recipient value or fees. This audit moved
no funds and contacted no live wallet.

## 2026-08-04 mined overlay proof promotion

Source commit `f8052a89` already contained stronger evidence than nine catalog
rows claimed. The `test_simnet` group uses the production ZSLP and ZNAM codecs,
spends real simulated UTXOs, admits each transaction through block connection,
and then folds the mined bytes into the token or name projection. The notebook
now records that evidence instead of leaving it mislabeled as builder-only.

- ZSLP GENESIS, SEND, and MINT are mined and their token/transfer balances are
  read back from the chain-derived projection. ZSLP burn remains
  `builder_verified`; the simulator does not yet demonstrate its implicit-burn
  accounting.
- All six ZNAM mutations are mined. REGISTER resolves the name, UPDATE changes
  the target, SET_RECORD and SET_TEXT populate their child records, RENEW is
  observed as the current projection no-op, and TRANSFER changes the owner.
  The same run also mines non-owner and malformed variants and proves they do
  not mutate authoritative name state.
- `raw_custom_transaction` remains `builder_verified`: the simulator mines an
  arbitrary OP_RETURN spend, but it does not invoke the public raw
  create/sign/broadcast command path end-to-end, so that semantic API proof is
  not promoted by association.

The current result is **33/34 PASS, 1 BLOCKED**, with **14 simulated-chain
confirmations** across **18 exact proof groups**, **0 mainnet confirmations**,
and **0 ZCL** live recipient value or fees. No live wallet was contacted and no
funds moved.

## 2026-08-04 strict ZSLP burn and baton-lineage proof

The burn follow-up strengthened the whole simulated ZSLP sequence instead of
adding a superficial output check. `test_simnet` now feeds every mined ZSLP
transaction through both the explorer projection and the strict per-outpoint
validity ledger.

That stronger authority exposed an invalid assumption in the earlier fixture:
its MINT spent an ordinary token output while the explorer-only projection
still displayed the declared quantity. The corrected sequence creates a real
GENESIS mint baton at vout 2, preserves it through MINT, and verifies that the
strict ledger accepts the lineage. This changes no ZClassic consensus rule;
ZSLP remains a chain-derived application overlay.

The new burn spends a valid 250-unit SEND output, declares only 100 units of
token change, and is mined through `connect_block`. The strict ledger records
the 150-unit difference as burned and reports exactly 1,125 minted, 150 burned,
and 975 circulating units while the replacement mint baton remains active.
The transaction retains separate ordinary ZCL change so the next fixture does
not accidentally destroy the remaining token output.

The current result is **33/34 PASS, 1 BLOCKED**, with **15 simulated-chain
confirmations** across **17 exact proof groups**, **0 mainnet confirmations**,
and **0 ZCL** live recipient value or fees. No live wallet was contacted and no
funds moved.

## 2026-08-04 public raw-transaction pipeline proof

The raw custom transaction case now crosses the public typed boundary and the
production RPC implementation before it reaches simulated consensus. The lab
creates a real P2PKH funding key and matured simnet coin, invokes
`core.wallet.transaction.raw.create` with its exact outpoint and a valid ZCL
destination, invokes `core.wallet.transaction.raw.sign` with the resident lab
key, and checks that signing is complete.

The first `core.wallet.transaction.raw.broadcast` call remains a non-mutating
plan: it does not call `sendrawtransaction` and the funding coin stays live.
The identical confirmed call crosses the real `sendrawtransaction` actor; the
test bridge decodes those exact signed bytes and mines them through
`connect_block`. The mined UTXO view proves the input is consumed, output zero
contains exactly 800,000 zatoshi, and the public returned txid is the mined
transaction hash. Private key bytes and addresses are fixture-local and never
enter the notebook event.

The current result is **33/34 PASS, 1 BLOCKED**, with **16 simulated-chain
confirmations** across **17 exact proof groups**, **0 mainnet confirmations**,
and **0 ZCL** live recipient value or fees. No live wallet was contacted and no
funds moved.

## 2026-08-04 mined Sapling unshielding proof

The production-prover shielded E2E already constructed a real Z-to-T Sapling
spend, selected the second simulated note by its exact witness position,
created its Groth16 spend proof and signatures, and passed
`contextual_check_transaction`. It stopped before block connection, so the
catalog correctly remained at `consensus_verified`.

The fixture now mines those same verified bytes through `connect_block`. It
proves the shielded note becomes an exact transparent output worth 99,990,000
zatoshi and that the Sapling note tree remains at two leaves because
unshielding creates no new shielded output. This completes mined coverage for
all three Sapling directions: T-to-Z, Z-to-Z, and Z-to-T.

The current result is **33/34 PASS, 1 BLOCKED**, with **17 simulated-chain
confirmations** across **17 exact proof groups**, **0 mainnet confirmations**,
and **0 ZCL** live recipient value or fees. No live wallet was contacted and no
funds moved.

## 2026-08-04 mined HTLC lifecycle and multi-source evidence

The atomic-swap rows previously separated two useful but incomplete facts:
the initiate/participate workflow builders were exercised by
`test_zswap_ceremony`, while redeem/refund script semantics were exercised by
`test_swap_settlement`. Neither primary group alone proved that the four chain
shapes entered a block and changed the UTXO set.

`test_simnet_contract` now distinguishes both funding roles. An initiator and
a separate participant wallet independently create and mine exact P2SH HTLC
outputs worth 200,000 and 180,000 zatoshi. It then mines the secret-bearing
redeem, proves ownership moves to the recipient and the contract coin is
spent, rejects an early refund as non-final, advances to the lock height, and
mines the refund to its refunder. The direct interpreter still proves a wrong
preimage fails `OP_EQUALVERIFY`; the notebook does not pretend simnet's
checkpoint-covered block connection executes scripts.

Because the strongest statement is the intersection of independent evidence,
the catalog now exposes `supplemental_test_groups`. HTLC rows retain their
original workflow/interpreter test as the primary group and add
`test_simnet_contract` as the mined-chain source. The proof target consumes
both, preventing a future promotion from hiding one axis of evidence.

The current result is **33/34 PASS, 1 BLOCKED**, with **21 simulated-chain
confirmations** across **18 exact proof groups**, **0 mainnet confirmations**,
and **0 ZCL** live recipient value or fees. No live wallet was contacted and no
funds moved.

## 2026-08-04 ZBLG plan/commit and mined anchor proof

The last explicit gap, `blog_anchor`, now has a public typed transaction
boundary. `app blog anchor` accepts an explicit `dev`/`prod` wallet scope, a
canonical ZNAM name, the 32-byte ID of an already stored wallet-signed Blog
event, and an idempotency key. Its first call prepares one exact signed
transaction and atomically reserves its maximum fee in `vault_intents`; it
does not broadcast. It returns a durable `plan_id`. The `confirm:true` call
accepts only that plan ID and scope, then rechecks wallet instance, network
genesis, operator lane, tip hash, money snapshot root, exact prepared bytes,
event signature, and current ZNAM owner before relaying. A node without a
current identity-bound wallet snapshot refuses the plan instead of presenting
missing custody as zero.

The proof intentionally has three independent sources. `test_blog` proves
wallet-bound event signatures, ZNAM ownership, strict/minimal codec parsing,
maximum-fee reservation, application idempotency, wrong-scope refusal,
commit idempotency, transaction/canonical-block relationships, and reorg
status. `test_transaction_intent` proves fee-only application reservations and
prepared raw bytes are inserted atomically. `test_native_api_contract` proves
the typed plan reaches the node once to create the durable reservation and the
confirmed plan ID reaches it once to commit. `test_simnet` places the exact
public codec bytes in a funded transaction, mines them through `connect_block`,
consumes the input, and retains the ZBLG OP_RETURN at the mined height in the
explorer projection.

The catalog remains honest about the composite boundary: the custody-bound
anchor command is implemented, while creation of a new signed Blog event
remains contained behind the unfinished runtime App grant broker. No raw key,
event signature, address, endpoint, datadir path, or prepared transaction bytes
enter the public command result, receipt, or notebook.

The current result is **34/34 PASS, 0 BLOCKED**, with **22 simulated-chain
confirmations** across **19 exact proof groups**, **0 mainnet confirmations**,
and **0 ZCL** live recipient value or fees. No live wallet was contacted and no
funds moved.

## 2026-08-04 exact ZCODE release-anchor chain proof

The ZCODE release case now crosses its public typed builder before entering the
chain fixture. `zcode release sign` creates a real Ed25519-signed release
document; `zcode release anchor` loads that stored document, byte-sorts its
record digest into the domain tree, stores the reproducible leaf set, and emits
the exact ZANC SHA3 OP_RETURN for the resulting root and `zcode@<tip>` label.

A reusable transaction-lab fixture funds those exact command-produced bytes,
mines the transaction through `connect_block`, proves its input is consumed
and transparent change enters the UTXO view, then folds the retained exact
transaction through the production explorer overlay registry. The ZANC
projection must recover the same root, label, txid, and mined height. The RPC
bridge remains stubbed throughout: no live node, wallet, endpoint, or funds are
contacted.

The helper lives in `test/transaction_lab_simnet.h` so ZID, ZDIR, epoch-ZANC,
and future OP_RETURN transaction families can reuse the same two-axis
block-connection-plus-projection receipt instead of inventing weaker per-test
fixtures.

The current result is **34/34 PASS, 0 BLOCKED**, with **23 simulated-chain
confirmations** across **19 exact proof groups**, **0 mainnet confirmations**,
and **0 ZCL** live recipient value or fees. No live wallet was contacted and no
funds moved.

## 2026-08-04 exact epoch-ZANC chain proof

`core epoch anchor` now has the same two-axis chain evidence as the ZCODE
release anchor. The existing test first builds a real OP_RETURN catalog cursor
whose digest is explicitly bound to its declared base and head range. With the
node RPC forcibly stubbed, the public command emits the exact ZANC SHA3 script
and `zepoch@<catalog-height>` label that an operator wallet would publish.

The shared transaction-lab simnet fixture funds those exact command bytes,
admits them through `connect_block`, and proves the funding input is consumed
and transparent change enters the UTXO view. The retained transaction is then
folded through the production explorer overlay registry; the resulting ZANC
row must contain the same catalog digest, label, transaction ID, and mined
height. Existing epoch tests continue to prove range declaration, paging,
cross-operator agreement/disagreement, and incomparable partial histories.

The current result is **34/34 PASS, 0 BLOCKED**, with **24 simulated-chain
confirmations** across **19 exact proof groups**, **0 mainnet confirmations**,
and **0 ZCL** live recipient value or fees. No live wallet was contacted and no
funds moved.

## 2026-08-04 exact ZDIR register/deregister chain proof

The public `core zdir register` and `core zdir deregister` commands now cross
the same two-axis chain fixture as the ZANC anchors. With node RPC stubbed, the
test takes each command's exact `op_return_hex`, funds a complete P2PKH output
for the declared owner, and admits the spending transaction through
`connect_block` at deterministic heights 500 and 700.

The retained transactions are then folded through the production explorer
overlay registry. Before each fold, the reusable receipt seeds the exact
funding previous output into the rebuildable projection, so ZDIR derives the
same owner hash that funded the block-connected transaction. The register must
be active with the expected owner, hostname, optional master key, tx height,
and dial eligibility; the later deregister must come from that same owner,
preserve original seniority, set the update height, and remove dial eligibility.
The existing independent wallet test still proves that the real builder chooses
the recorded owner's coin instead of a richer decoy in the same wallet.

These are isolated deterministic chains, not mainnet transactions. The live
node, wallets, endpoints, and keys are never contacted, and the notebook keeps
no address or raw transaction material.

The current result is **34/34 PASS, 0 BLOCKED**, with **26 simulated-chain
confirmations** across **19 exact proof groups**, **0 mainnet confirmations**,
and **0 ZCL** live recipient value or fees. No live wallet was contacted and no
funds moved.
