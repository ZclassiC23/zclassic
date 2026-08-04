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
