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

## 2026-08-04 exact ZID identity-lifecycle chain proof

The three public identity mutations now form one exact isolated lifecycle.
`core identity anchor` emits the first key's production OP_RETURN; the shared
owner-aware fixture funds it from a complete P2PKH output and admits the exact
bytes through `connect_block` at height 500. The retained transaction is folded
through the production explorer registry, where it must create an active ZID
row with the same transaction ID, height, and derived owner.

The resulting file-backed projection is then the real pre-flight authority for
`core identity rotate`. Its exact old-key/new-key command bytes are mined at
height 700 from the same owner and projected into a rotated predecessor plus an
active successor. Finally, `core identity revoke` reads that successor, emits
its exact owner-bound bytes, and the transaction is mined at height 900; the
projection must retire the successor without changing its original anchor
transaction or height.

The catalog keeps both evidence axes explicit: `test_identity_command` owns the
public command-to-chain lifecycle, while supplemental `test_zid_identity`
continues to prove malformed records, non-owner refusals, claimed-target
protection, idempotent replay, ZNAM-derived identities, and model persistence.

These are deterministic isolated chains. No live node, wallet, endpoint, key,
or ZCL is contacted, and no address or raw transaction is written to the public
notebook.

The current result is **34/34 PASS, 0 BLOCKED**, with **29 simulated-chain
confirmations** across **20 exact proof groups**, **0 mainnet confirmations**,
and **0 ZCL** live recipient value or fees. No live wallet was contacted and no
funds moved.

## 2026-08-04 exact Yardsale atomic-purchase chain proof

The Yardsale app now carries its byte-exact controller ceremony across the
broadcast boundary and into an isolated block. A fresh simnet creates three
real P2PKH funding outputs: the seller's ZSLP-bearing dust input and two buyer
ZCL inputs. Those actual transaction IDs, output indexes, scripts, and values
are placed in the signed Yardsale ad/accept ceremony; no synthetic outpoints are
installed after assembly.

The normal controller path sends the buyer accept, has the seller validate the
signed terms and sign only the token input, then has the buyer verify that
signature and sign both ZCL inputs. The test captures the exact fully signed
transaction at the production broadcast port and transfers that same object to
`simnet_mint_txs`, which admits it through `connect_block` with Sapling active.

After mining, all three funding outputs must be consumed. The chain UTXO view
must contain exactly 546 zat of token dust, 1.25000000 ZCL paid to the seller,
the seller's exact dust change, and the buyer's exact change after a 0.00010000
ZCL fee. The existing Stage-3 golden-vector leg remains alongside this dynamic
chain proof, so future changes must preserve both stable protocol bytes and
real-outpoint settlement behavior.

This is an isolated deterministic transaction. No live wallet, peer, endpoint,
or mainnet ZCL is contacted, and its keys, addresses, and raw bytes are not
written to the public notebook.

The current result is **34/34 PASS, 0 BLOCKED**, with **30 simulated-chain
confirmations** across **20 exact proof groups**, **0 mainnet confirmations**,
and **0 ZCL** live recipient value or fees. No live wallet was contacted and no
funds moved.

## 2026-08-04 exact transparent-store chain proof

Transparent store settlement now starts before the old projection fixture. A
fresh isolated chain creates and matures a payer funding outpoint. The real
transparent wallet sender selects that exact coin, produces its ECDSA
signature, and admits the transaction through the production mempool gate.
The test retains those exact bytes and mines them through `connect_block`.

The payment has one input, one output paying the order's one-time P2PKH
address, and an exact 0.00010000 ZCL fee. After the block consumes the payer
outpoint, the production wallet projection scans the retained confirmed
transaction with the merchant key. Only that derived `wallet_utxos` row is
then visible to `store_confirmed_payment`; after three confirmations,
`store_process_payments` fulfills the address-bound order. Existing negatives
still prove that another order's payment, shallow value, coinbase value,
underpayment, and a second reconciliation cannot unlock or double-credit it.

This is a deterministic isolated payment. Its temporary trust declaration,
keys, addresses, transaction bytes, and databases remain under `test-tmp` and
are destroyed after the proof. No live wallet, peer, endpoint, or mainnet ZCL
is contacted.

The current result is **34/34 PASS, 0 BLOCKED**, with **31 simulated-chain
confirmations** across **20 exact proof groups**, **0 mainnet confirmations**,
and **0 ZCL** live recipient value or fees. No live wallet was contacted and no
funds moved.

## 2026-08-04 exact shielded-store chain proof

Shielded store settlement now begins with a real isolated funding outpoint.
The test matures that transparent coin, uses the production Sapling output
builder to encrypt the exact store-order memo to the merchant's newly derived
address, and creates the binding signature over the funded transaction. The
merchant wallet decrypts the exact transaction and recovers its value, memo,
commitment, nullifier, and recipient material before the same transaction is
mined through `connect_block`.

The block must consume the funding output and append exactly one Sapling
commitment to the chain tree. Only the recovered confirmed note is written to
the wallet projection. At three confirmations, the memo-bound finder credits
the intended order; a second real encrypted note to the same address but naming
another order proves that the legacy address-only query over-counts while the
order-aware query remains exact.

This fixture uses the Sapling simnet harness's documented deferred contextual
proof-verification boundary. It directly proves encryption, wallet decryption,
binding-signature construction, block connection, commitment-tree application,
and application reconciliation; separate parameter-enabled tests own full
prover/verifier coverage. It is not evidence of a mainnet transaction, and the
public store command remains deliberately contained to isolated networks.

The current result is **34/34 PASS, 0 BLOCKED**, with **32 simulated-chain
confirmations** across **20 exact proof groups**, **0 mainnet confirmations**,
and **0 ZCL** live recipient value or fees. No live wallet, peer, endpoint, key,
or funds were contacted.

## 2026-08-04 exact file-market purchase chain proof

The complete file-market purchase proof no longer substitutes a fixed txid at
the wallet-send port. Its isolated chain matures a funding output worth the
signed offer range plus the exact 0.00010000 ZCL maximum fee. Commit passes the
seller address, exact integer amount, and authenticated 512-byte purchase memo
through the normal service port; the fixture builds the corresponding Sapling
output, creates its binding signature, and mines the transaction through
`connect_block`.

The mined block must consume the funding outpoint and append exactly one
Sapling commitment before the returned transaction ID can be sealed into the
durable vault intent and signed buyer claim. Replay cannot mine or notify a
second payment. The existing second half then proves pending delivery, restart,
session-bound encrypted retrieval, per-chunk and full-manifest verification,
atomic destination publication, and idempotent replay.

As with the other isolated Sapling transport fixtures, contextual proof
verification is deferred inside this simnet leg; separate parameter-enabled
prover/verifier tests own that cryptographic boundary. No live wallet, seller,
peer, address, key, or ZCL was used, and no private transaction material enters
the public notebook.

The current result is **34/34 PASS, 0 BLOCKED**, with **33 simulated-chain
confirmations** across **20 exact proof groups**, **0 mainnet confirmations**,
and **0 ZCL** live recipient value or fees. The only remaining below-chain case
is receive-only legacy Sprout processing, for which no supported builder exists.

## 2026-08-04 canonical Sprout receive/validation proof

The final process-only row now points to a complete real transaction rather
than a generic transaction-structure group. The immutable 2,022-byte fixture is
mainnet block 241 transaction
`55c6c3a289d295954936076b697cc1e2a713c99dd268934f7ab6518f825148fd`,
retrieved from the local chain through the typed, read-only
`core.chain.transaction.get` command.

The focused test deserializes the canonical wire, recomputes and pins its txid,
matches the JoinSplit anchor, public values, nullifiers, and commitments to the
independent proof KAT, and passes context-free structural consensus. It then
forces proof verification on at height 241 and drives the production contextual
validator: the Ed25519 JoinSplit signature, derived `hSig`, and real PHGR13
proof all verify. Flipped proof and nullifier bytes remain hard negative cases.

This remains `consensus_verified`, not `simnet_confirmed`: the transaction's
historical anchor membership and transparent input script depend on preceding
mainnet state, and substituting an empty or invented Sprout tree would weaken
the evidence. The node intentionally exposes receive/validate/project only;
there is no supported builder for creating new deprecated Sprout spends.

The same work makes large raw reads usable by agents. With `verbose=false`,
`core.chain.transaction.get` now returns bounded `zcl.raw_transaction.v1`
pages using `raw_offset` and `raw_bytes` (maximum 1,024 bytes), including total
size, completion, and next offset. Valid raw hex is no longer truncated into a
false error.

The current result is **34/34 PASS, 0 BLOCKED**, with **33 simulated-chain
confirmations** across **20 exact proof groups**, **0 mainnet confirmations**,
and **0 ZCL** live recipient value or fees. All 34 semantic transaction types
now have type-specific reproducible evidence; Sprout alone is intentionally a
historical process-only proof rather than a newly constructed isolated spend.

## 2026-08-04 structural transaction coverage audit

The semantic 34-row inventory is now complemented by a source-derived wire
catalog rather than being overstated as every byte shape consensus could ever
carry. `app transaction-types wire` enumerates the four finite transaction wire
families and six output-script classifier buckets used by this codebase, while
preserving explicit generic buckets for consensus-valid nonstandard scripts,
unknown/future OP_RETURN tags, and opaque Sapling memos.

This audit found one application-level gap and records it instead of inflating
the semantic count: the ZPAY Sapling-memo codec exists, but there is no typed
chain workflow or type-specific mined proof yet. It therefore appears in the
wire catalog as `codec_only_no_typed_chain_workflow`, not as a demonstrated
semantic transaction row. A positive post-Sapling Sprout Groth16 transaction
fixture would also strengthen proof-era coverage; the existing canonical
Sprout fixture proves the legacy v2 PHGR13 path.

The lab statistics remain **34/34 PASS**, **33 simulated-chain confirmations**,
**0 mainnet confirmations**, and **0 ZCL** live recipient value or fees. The
structural catalog changes discovery and audit completeness only; it neither
constructs nor broadcasts a transaction and creates no new lab evidence event.

## 2026-08-04 ZPAY typed workflow and mined Sapling proof

Source commit `d6ca69d0` closes the codec-only gap named by the structural
audit. Two public deterministic commands now own the application boundary:
`app payments zpay compose` encodes an anonymous canonical 512-byte invoice,
payment, or receipt memo from explicit fields, while
`app payments zpay inspect` strictly decodes it, verifies any embedded ZID
document, and checks an explicit expected network and caller-supplied clock.
Neither command reads a wallet, accepts an identity seed, or moves funds. The
existing owner-only `core wallet shielded send` remains the sole plan/commit
step and accepts the composer's exact `memo_hex`.

The params-backed `test_simnet_zmsg_onchain` proof now constructs a second
transparent-funded Sapling transaction carrying a canonical ZPAY payment. It
uses the production Sapling output prover and binding signature, decrypts the
recipient note with authenticated encryption, recovers the exact request and
asset fields, proves anonymous authentication remains explicit, accepts the
bound regtest network/time window, refuses mainnet for the same bytes, mines
the transaction through `connect_block`, and observes the second note in the
Sapling tree. `test_zpay` independently drives the exact native compose and
inspect commands and covers signed-envelope authentication and strict negative
codec cases.

The current result is **35/35 PASS, 0 BLOCKED**, with **34 simulated-chain
confirmations** across **21 exact proof groups**, **0 mainnet confirmations**,
and **0 ZCL** live recipient value or fees. No live wallet, address, peer,
endpoint, private key, identity seed, or mainnet ZCL participated.

## 2026-08-04 post-Sapling Sprout Groth16 mainnet proof

Source commit `83a5b2e8` closes the proof-era gap named by the structural
audit. The local read-only `zclassicd` oracle identified the first transaction
after Sapling activation carrying a Sprout JoinSplit: mainnet height 476970,
block `000000002ef6ebe979c451adfa9508121d216ac861ee12576015b5cae8d3733c`,
txid `6eb069da34331871a55314ec3b92fcf50d8fabe914d16c46d686be853c8a3047`.
It is a version-4 / `0x892f2085` transaction with fourteen transparent inputs,
one Groth16 Sprout JoinSplit, no transparent outputs, and no Sapling spends or
outputs.

`test_sprout_groth16_kat` embeds the exact 3,890 public transaction bytes and
only the 1,828-byte public verification-key prefix needed by the Sprout
Groth16 verifier. It requires no live node, external params file, wallet,
proving material, or private key. The test pins the txid and complete wire,
round-trips byte-identically through the production serializer, passes
structural consensus, verifies the JoinSplit Ed25519 signature and Groth16
proof through the production contextual validator at height 476970, verifies
the proof directly, and rejects both a flipped proof byte and a changed public
input. The primary `test_sprout_phgr13_kat` remains required, so the semantic
Sprout row now proves both pre-Sapling PHGR13 and post-Sapling Groth16 eras.

The notebook records this as `network=mainnet` with
`proof=consensus_verified`, not `live_confirmed`: it is a historical public
chain transaction that predates this lab, not a newly authorized spend. It
therefore does not increase the live-mainnet bar or invent recipient/fee
amounts for this project. Current totals remain **35/35 PASS**, **34/35
simulated/live confirmations**, and **0/35 live-mainnet confirmations**, now
across **22 exact proof groups**, with **0 ZCL** lab recipient value or fees.

## 2026-08-04 mainnet wire-era and script-class evidence

Source commit `c1c1f01e` makes the structural catalog distinguish wire support
from actual mainnet reachability. ZClassic mainnet activates Overwinter and
Sapling at the same height, 476969. The new
`test_transaction_wire_evidence` proves that a v3 transaction is premature at
height 476968 and has the wrong version-group ID at height 476969; there is no
mainnet v3-only height and therefore no honest canonical v3 transaction to
pin. The API now reports v1/v2 as `historical_only`, v3 as `never_active`, and
v4 as `current`, with nullable height bounds and exact evidence groups.

The same test embeds complete public bytes for five canonical mainnet
transactions and requires exact txid, byte-identical serialization,
structural/contextual acceptance, and the production script solver's class:

| Height | Txid | Pinned output class |
|---:|---|---|
| 1 | `13e63618e0f7dd61ecbb3ee0607489ead19a10317c2311e50a72585643256f56` | `pubkey` |
| 122001 | `c6b58ab4533eafd151b998c8b232d3910417ead11e916d04f7a633afc171e1cc` | `nonstandard` |
| 255001 | `b18c3f28d2d4867920a126d09f90e619f3e64e41cd31a7c9f9653b9adce60c83` | `scripthash` |
| 3139216 | `1765e9c9b0dbcbd9c9a968ea4f3c9c4b60d447d86c2583aa186e9a107c2e7c91` | `pubkeyhash` |
| 3139216 | `34ed27f1291a95c0f829c089522227bc30e4c215ac62b4e20a434179e36bd754` | `nulldata` |

Bare `multisig` remains supported and positively covered by deterministic
builder/solver/interpreter vectors, but no canonical example is claimed. A
read-only sparse audit sampled every 100th mainnet block through height
3205386 and did not observe one; that sample is evidence for keeping the API
at `mainnet_example_status=not_pinned`, not proof that none exists anywhere in
history.

This structural proof is supplemental evidence for `raw_custom_transaction`,
so `make transaction-lab-proof` now runs **23 exact proof groups**. It is not a
new lab broadcast and adds no event to the monetary ledger: totals remain
**35/35 PASS**, **34/35 simulated/live confirmations**, **0/35 live-mainnet
confirmations**, and **0 ZCL** recipient value or fees.

## 2026-08-04 native-command reverse coverage audit

The semantic catalog now answers both directions. `app transaction-types
guide --type=<id>` still starts from an intention; the new read-only `app
transaction-types command <path>` starts from a native leaf and returns every
type and role it serves. Canonical builder/commit/component/inspection roles
are derived from `transaction_types.def`. Nine alternate vault route bindings
are declared separately so a pass-through never looks like an independent
transaction implementation.

The audit also records 18 reviewed negative dispositions for wallet-security,
key-management, backup/recovery, scan, receive-request, agent-grant, and ZCODE
P2P-endpoint mutations. An omitted command is not called off-chain: the API
returns `unclassified` and tells the agent to stop. `test_api` scans every ready
wallet-risk mutation and every ready mutation whose registry contract contains
a chain signal, then fails unless the command has a positive semantic mapping
or an explicit negative reason. It also proves every alias and negative row
names a live leaf and that no negative row is simultaneously mapped.

This is discovery and completeness evidence, not a transaction experiment. It
does not change the 35 semantic cases or add a monetary event. Totals remain
**35/35 PASS**, **34/35 simulated/live confirmations**, **0/35 live-mainnet
confirmations**, **23 exact proof groups**, and **0 ZCL** recipient value or
fees.

## 2026-08-04 generic ZANC capability completeness audit

The independent source audit found one real semantic omission after the 35-row
catalog had become internally green: the production `anchor_publish` RPC and
ZANC codec can anchor an arbitrary file or digest, while the catalog named only
the specialized epoch and ZCODE-root meanings. A notebook agreeing with its
own catalog could not detect that omission.

Commit `aeda6af4` adds deterministic `core anchor compose` and strict `core
anchor inspect` native commands. The composer accepts only a public SHA2-256 or
SHA3-256 digest and optional label; it returns the exact canonical OP_RETURN and
the `op_return_hex` fragment consumed by the raw owner workflow. Raw create now
appends that exact zero-value script without manual byte surgery. Funding,
signing, the non-mutating broadcast plan, and the explicit confirmed commit
remain separate owner-only commands.

`test_native_api_contract` drives those production commands end to end: compose,
strict inspect, reject trailing bytes, create with an explicit matured funding
outpoint and change output, sign with the resident isolated key, prove the first
broadcast call does not mutate, commit the identical bytes, and mine them
through `connect_block`. The decoded mined transaction contains the exact ZANC
digest and label. Supplemental `test_zanc` retains the codec/RPC hostile cases.

The current result is **36/36 PASS**, **35/36 simulated/live confirmations**,
**0/36 live-mainnet confirmations**, **24 exact proof groups**, and **0 ZCL**
recipient value or fees. No live wallet, node mutation, address, key, endpoint,
or mainnet funds participated.

## 2026-08-04 identity-bound multi-recipient payment audit

An independent source-to-catalog audit found that the production wallet and
deterministic simulator both support one transparent transaction paying
multiple recipients, but the semantic catalog exposed only a single-recipient
payment. The production implementation was already safer than a convenience
`sendmany` wrapper: `vault intent plan` accepts 1..50 exact decimal-string ZCL
effects, encrypts the recipient payload at rest, atomically reserves value plus
the maximum fee, and binds selected inputs, wallet instance, network genesis,
tip hash, current money snapshot root and expiry. `vault intent commit`
revalidates every binding, persists signed bytes before relay, deduplicates
commit, and `vault intent status` retains the reservation through mempool,
confirmation, failure, conflict and reorg reconciliation.

The audit did find one real security defect in the compatibility surface:
direct `sendmany` RPC calls did not carry the sovereignty guard already present
on `sendtoaddress` and `z_sendmany`. Commit `d46abb69` closes that bypass and
`test_sovereignty_guard` proves borrowed `release_assisted` state cannot spend
through it. The public developer guide deliberately points to the vault-intent
API instead of creating a second transaction lifecycle.

`test_simnet_txkit` independently builds, signs, admits and mines the exact
multi-recipient transparent fan-out through `connect_block`; the transaction
has two recipient outputs plus change with deterministic fee accounting.
`test_transaction_intent` covers durable encrypted reservations, wallet
identity persistence, restart reconstruction, reserve-floor/lab-cap
enforcement and state transitions. Together these are isolated technical
proof, not a live-wallet experiment.

The current result is **37/37 PASS**, **36/37 simulated/live confirmations**,
**0/37 live-mainnet confirmations**, **26 exact proof groups**, and **0 ZCL**
recipient value or fees. No live wallet, address, endpoint, private key or
mainnet funds participated.

## 2026-08-05 durable mixed-pool transaction proof

Implementation commit `27b927b3` extends the canonical vault-intent lifecycle
across all Sapling pool directions and mixed recipient fan-out. A plan requires
an explicit `dev|prod` scope, source address, route, 1..50 exact effects, and
idempotency key. It encrypts the normalized recipients and effective memos,
reserves recipient value plus maximum fee, and binds wallet instance, genesis,
tip hash, current custody snapshot, expiry, and request digest. Commit rebuilds
the exact effects, persists signed bytes before relay, and uses a guarded note
reservation that accepts the same txid on restart but rejects a conflicting
txid.

The params-backed `test_shielded_payment_gate` now drives the public durable
API rather than calling the compatibility send RPC. In an isolated wallet with
a real encrypted backup it plans a `0.04000000 ZCL` mixed transaction, proves
that planning leaves mempool empty, then commits `0.03000000 ZCL` to Sapling
and `0.01000000 ZCL` transparently. The production native Groth16 prover signs
the transaction; the full mempool boundary independently accepts it; a flipped
proof byte is rejected; and the wallet decrypts exactly `0.03000000 ZCL`.

This is `consensus_verified`, not a simulated block or mainnet spend. The
current result is **38/38 PASS**, **36/38 simulated/live confirmations**,
**0/38 live-mainnet confirmations**, and **0 ZCL** live recipient value or
fees. No live wallet, address, endpoint, key, or mainnet funds participated.

## 2026-08-05 typed P2SH multisig workflow and interpreter proof

Implementation commit `2196407a` promotes transparent P2SH multisig from a
generic raw-transaction possibility to a discoverable typed workflow.
`core wallet transaction multisig compose` accepts an explicit signature
threshold and 1..16 public keys, then returns the standard P2SH address, exact
CHECKMULTISIG redeem script, and the canonical fund/create/sign/broadcast
command paths. It stores nothing, moves no funds, and accepts no private keys.

The workflow funds that address through the ordinary transparent payment
path, then uses the raw owner path to create and sign its spend. Signing is
available only when the resident owner wallet already holds the threshold
keys; the command neither exports keys nor claims to merge partial signatures
from separate wallets. The catalog makes that boundary part of the network
policy instead of hiding it in prose.

`test_simnet_txkit` funds a real 2-of-2 P2SH output, builds its spend with
production ECDSA signatures, passes it through the production consensus
P2SH/CHECKMULTISIG interpreter, and proves a signature-byte mutation is
rejected. The exact signed transaction then enters the isolated mempool and
is mined; its funding output is consumed and its recipient output is present.
The deterministic cost table pins that spend at **322 serialized bytes**, an
explicit **0.00005000 ZCL simulated fee**, and one simulated confirmation
block (2.5 virtual minutes).
Independent native-contract coverage pins public-key-only composition, typed
result fields, and local threshold rejection before RPC. `test_multisig` and
the consensus branch suite retain the direct signature/key-count and
NULLDUMMY/NULLFAIL matrices.

This is `simnet_confirmed`, not a mainnet spend. The current result is
**39/39 PASS**, **37/39 simulated/live confirmations**,
**0/39 live-mainnet confirmations**, and **0 ZCL** live recipient value or
fees. No live wallet, address, endpoint, key, or mainnet funds participated.
