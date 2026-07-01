# C5 — real shielded-purchase proof (store sell) — design of record

**Status (2026-07-01): IMPLEMENTED as a hermetic C5 slice, not a full C5 ✅.**
The original 2026-06-17 design below is now mostly historical context. Current
code has the additive `store_e2e_shielded` selector, the memo-bound model finder,
the live memo-bound store reconcile, and the fail-loud Sapling-address path that
refuses synthetic/unrecoverable `zs1_pay_<time>` payment addresses. Focused proof:
`ZCL_STRESS_TESTS=1 ZCL_TEST_ONLY=store_e2e_shielded build/bin/test_zcl` passes
with `ivk-decrypted, memo-bound`.

Remaining C5 work is the full operator proof: a real buyer pays shielded and
receives the file through the live store/onion/file-transfer path.

## The original gap

MVP C5 ([`docs/MVP.md`](../MVP.md) row 5) is "operator lists a product → buyer
pays **shielded** → buyer receives the file." The old gate
(`lib/test/src/test_store_e2e_gate.c`, `make ci-mvp-gates`) proves the
persistence + reconcile + token-gated binary-safe download plumbing, but the
"payment" is **fabricated**: `p11_5_seed_confirmed_note()` hand-seeds a Sapling
note with a placeholder `ivk = memset(0x63)` and `address = order.payment_addr`
(a **transparent** `t1…` string), and `store_process_payments` matches it to
the order by **address-string equality**. No incoming-viewing-key decryption of
a real Sapling output ever happens, so "pays shielded" is not exercised.

## The shipped upgrade

The shipped `store_e2e_shielded` selector replaces the fabricated-note proof
with a **genuine ivk-decrypt +
memo-bound** purchase, reusing the **params-free** mechanism already proven by
`lib/test/src/test_shielded_receive_slice.c` (real Sapling output encrypted to
a wallet ivk; `wallet_try_sapling_decrypt` recovers value+memo without
`~/.zcash-params` — the receive path never inspects the Groth16 proof).

**Non-gameable assertion (the teeth):** an order reaches `STORE_ORDER_SENT` and
credits ZSLP **iff** `wallet_try_sapling_decrypt` with the **merchant's** ivk
recovered a note from the paying tx whose recovered **memo == order_id** and
whose **value ≥ amount** at **≥3 confirmations**. A placeholder-ivk note
(`memset(0x63)`) decrypts to 0 notes; a foreign-ivk note is rejected — so
address-string equality alone can never satisfy it.

### Original adversarial verification and current status

| Claim | Verified at | Result |
|---|---|---|
| A · placeholder fallback existed | `app/services/src/zslp_service.c` | **FIXED:** payment address generation now requires a seeded Sapling keystore and fails loudly instead of minting a synthetic/unrecoverable address |
| B · reconcile was address+amount only | `app/controllers/src/store_controller.c` | **FIXED:** live reconcile calls `db_store_received_payment_for_memo` |
| C · `memo` column exists + written | `app/models/src/database_schema.c:164` `memo BLOB`; writer `node_db_sync_sapling_note` (`app/controllers/src/sync_controller_writers.c:358`) forwards `memo/memo_len` | confirmed — the persisted memo-bound reconcile needs **no** schema change |
| D · params-free reuse mechanism | `lib/test/src/test_shielded_receive_slice.c` header "WHY THIS IS PARAMS-FREE … wallet_try_sapling_decrypt never inspects the proof" | confirmed reusable (`srs_build_received_output_tx`) |

## Implementation split

**Slice 1 — SAFE, additive, hermetic (no live behavior change): DONE**
- `app/models/src/store.c` (+`store.h`): add `db_store_received_payment_for_memo(ndb, pay_addr, order_id, max_height)` — the same query as `db_store_received_payment` plus a memo→order_id predicate (decode in C, not SQL, per risk note 1). **Additive**: does not change `db_store_received_payment`.
- `lib/test/src/test_store_e2e_gate.c`: a **new** selector `store_e2e_shielded` that (a) derives a merchant wallet sapling key + z-address, (b) builds a **real** paying Sapling output via `srs_build_received_output_tx` with `memo = "ZCL23ORDER:" || order_id`, (c) `wallet_try_sapling_decrypt` → assert value+memo, persist via `node_db_sync_sapling_note`, (d) reconcile via the new memo-bound finder, (e) keep ALL existing downstream assertions (order→SENT, ZSLP==10 once, dedupe, token-gated blob bytes incl. NUL + re-hash). The **old** `store_e2e` selector stays **unchanged** (never weaken it).
- `Makefile`: wire `store_e2e_shielded` into `ci-mvp-gates` (params-free → stays hermetic) with the false-green guard.

Slice 1 strengthens the C5 gate from "fake note + address match" to "real
ivk-decrypt note + memo bind" with **zero live behavior change** and is
teeth-verified by `make test_zcl`.

**Slice 2 — live behavior hardening: DONE**
- `app/controllers/src/store_controller.c:546`: switch the live
  `store_process_payments` reconcile from `db_store_received_payment` to the
  memo-bound finder (closes the "an unrelated same-amount payment could match"
  hole in prod).
- `app/services/src/zslp_service.c:359`: remove the `zs1_pay_<time>` placeholder
  fallback — fail the order loudly (503) when the wallet has no sapling key
  instead of minting an undecryptable address (the prod gameability hole).
- Both were app-layer only (no consensus path touched).

## Why it stays in bounds

- **Consensus:** touches no consensus rule, activation height, or block/tx
  validity — purely store reconcile + a test. E13 / `test_consensus_parity`
  unaffected.
- **Params:** the genuine path is params-free (claim D), so Slice 1 stays a
  hermetic `make ci` gate (no `~/.zcash-params`, self-skips only on
  `!ZCL_STRESS_TESTS`).
- **MRS:** Slice 1 raises the C5 floor (a stronger ◐); it does not by itself
  reach ✅ (the full operator claim is a real shielded purchase + file transfer
  between live nodes). Tracked in [`FORWARD_PLAN.md`](./FORWARD_PLAN.md) §A/§B.

## Proof command (Slice 1)

```
make test_zcl && ZCL_STRESS_TESTS=1 ZCL_TEST_ONLY=store_e2e_shielded build/bin/test_zcl
# PASS: "OK (order=.. payment=zs1.. balance=10, ivk-decrypted, memo-bound)"
```
