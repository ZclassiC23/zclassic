# 100-transaction micro lab

This is the owner-visible plan for demonstrating **100 newly mined ZClassic
transactions**, each with a primary recipient value of **0.00001000 ZCL**
(1,000 zatoshi), while collecting enough evidence to make future AI-operated
transactions predictable.

The human interface is conversational. The agent uses the typed native
commands, checks custody, presents each exact redacted plan for approval,
waits for confirmation, and records the public result. The owner is not
expected to translate this runbook into CLI input.

```text
100 confirmed transactions  [--------------------]   0/100
14 exact-value shapes        [--------------------]   0/14
execution gate               [--------------------]   EXTERNAL CHECK REQUIRED
```

No transaction in this campaign has been broadcast according to its ledger.
This page intentionally carries no live lane or balance claim: re-read
`docs/HANDOFF.md`, current typed status, and `make custody-status` before every
session. A remembered 0.30000000 ZCL observation is not spend authority.

## What “smallest possible fee” means

The current node has two different fee numbers:

- Its local minimum relay policy is 100 zatoshi per transaction.
- Every supported wallet, shielded, vault-intent, swap, market, and overlay
  builder currently uses the wallet default of 10,000 zatoshi.

There is no connected live fee estimator or safe per-plan fee override. The
smallest fee the typed custody workflow can currently construct and bind is
therefore **10,000 zatoshi (0.00010000 ZCL)**. Sending at the theoretical
100-zatoshi relay floor would bypass the supported workflow and would not prove
that peers or miners accept it. This campaign does not do that.

Every plan must expose its exact fee. The checked campaign currently requires
exactly 10,000 zatoshi. If a future implementation safely lowers the wallet default,
the manifest and its checks must be deliberately revised and the lower fee
validated on an isolated chain before mainnet. “Lowest” is an evidence-backed
policy choice, never a hard-coded guess about miner behavior.

## Exact budget

```text
100 recipients x 1,000 zat       100,000 zat   0.00100000 ZCL
100 fees x <=10,000 zat        1,000,000 zat   0.01000000 ZCL
bounded setup envelope           900,000 zat   0.00900000 ZCL
campaign maximum               2,000,000 zat   0.02000000 ZCL
```

The 0.02000000 ZCL maximum is below the lifetime 0.05000000 ZCL transaction-lab
allocation. If development custody is freshly proven as 0.30000000 ZCL and no
other lab allocation has been consumed, the maximum would leave 0.28000000 ZCL,
above the 0.25000000 ZCL reserve. Those are conditional calculations; the
identity-bound live snapshot and existing reservations remain authoritative.

The setup envelope covers owner-visible, separately approved prerequisites:

- one Sapling seed note up to 600,000 zatoshi plus its fee;
- confirmed P2SH multisig funding outputs;
- four confirmed HTLC funding outputs for two claim and two timeout spends;
- the minimum ZSLP inventory and two-party commerce fixtures;
- their fees and protocol-defined dust outputs.

Setup transactions are not secretly counted among the 100. They are separately
identified public transactions, included in the same lifetime cap, and never
created automatically. The original four-transaction shield/unshield proposal
in `LIVE_TRANSACTION_DEMONSTRATIONS.md` is superseded by this campaign's setup
when this campaign is selected; do not fund both.

## The 100 numbered slots

The stable machine-readable allocation is
`tools/dev/transaction_micro_lab_catalog.def`. It covers 14 transaction shapes:

| Slots | Count | Shape | What varies |
|---:|---:|---|---|
| 001–016 | 16 | transparent T-to-T | ordinary standard payments |
| 017–026 | 10 | raw custom | separated create/sign/broadcast path |
| 027–028 | 2 | P2SH multisig spend | threshold-signed spends |
| 029–043 | 15 | Sapling T-to-Z | shielding micro-payments |
| 044–058 | 15 | Sapling Z-to-Z | private micro-payments |
| 059–073 | 15 | Sapling Z-to-T | unshielding micro-payments |
| 074–078 | 5 | mixed Sapling | transparent plus shielded recipients totaling 1,000 zat |
| 079–086 | 8 | on-chain memo | encrypted Sapling memo payments |
| 087–090 | 4 | ZPAY | typed payment-envelope memo payments |
| 091–092 | 2 | HTLC redeem | secret-claim settlements |
| 093–094 | 2 | HTLC refund | matured timeout settlements |
| 095–096 | 2 | store payment | one-time transparent order payments |
| 097–098 | 2 | yardsale purchase | atomic ZCL-for-token purchases |
| 099–100 | 2 | market purchase | paid, verified file delivery |

“Primary recipient value” excludes ordinary change, OP_RETURN metadata, and a
protocol-required token dust output. Those are still included in exact plan
review and fee/accounting receipts. The catalog deliberately excludes the 25
other semantic types from the 1,000-zatoshi claim: coinbase and Sprout are
process references; metadata-only operations do not have a payment amount;
some lifecycle operations require more than a 1,000-zatoshi spendable output;
Blog is contained; and shielded store payment is isolated-only. The separate
39-type runbook still demonstrates those honestly instead of disguising them
as micro-payments.

## Safe execution order

The campaign is serial by default:

1. Re-run the global gate in `LIVE_TRANSACTION_DEMONSTRATIONS.md`. Continue only
   at custody 5/5 with both wallet identities `CURRENT`, a current tip and money
   snapshot, no conflicting reservations, and explicit `dev` scope.
2. Create and confirm the separately approved setup transactions. Refresh the
   identity-bound snapshot after each one.
3. For the next numbered slot, ask
   `app transaction-types guide --type=<case_id>` for current schemas. Create a
   non-broadcasting exact plan with a unique idempotency key.
4. Show the owner the redacted wallet identity, route, exact outputs, recipient
   total, maximum fee, reserve after, tip, snapshot root, and expiry. Generic
   approval of this campaign does not approve unknown outputs or a changed fee.
5. Commit the exact approved plan once. A timeout is not permission to resend.
   Record `broadcast`, then query and reconcile the same plan until mined,
   conflicted, expired, or reorged.
6. Record confirmation, refresh custody, and only then advance to the next slot.

Small bounded parallel batches may be considered later only if independent
notes/UTXOs and atomic reservations are proven. The first live run remains
one-at-a-time so the confirmation latency and fee evidence are interpretable
and the campaign cannot become a burst of low-value traffic.

## Notebook and statistics

The redacted append-only ledger is
`docs/work/transaction-micro-lab-events.jsonl`. It permits only public txids,
block identity, amounts, fees, and Unix times. Addresses, endpoints, paths,
grant tokens, plan IDs, memos, secrets, keys, and recovery words are rejected.

Immediately after a successful broadcast, the agent records:

```bash
tools/dev/transaction-micro-lab.sh record \
  --slot=1 --state=broadcast --txid=<64-lowercase-hex> \
  --fee-zat=<actual-fee> --broadcast-unix=<unix-seconds>
```

After it is mined:

```bash
tools/dev/transaction-micro-lab.sh record \
  --slot=1 --state=confirmed --txid=<same-64-lowercase-hex> \
  --fee-zat=<same-actual-fee> --broadcast-unix=<same-unix-seconds> \
  --confirmed-unix=<unix-seconds> --block-height=<height> \
  --block-hash=<64-lowercase-hex>
```

`conflicted`, `expired`, and `reorged` are append-only corrective states. A
reconfirmed transaction appends a new `confirmed` event with the same txid and
new block identity. The checker rejects slot/case drift, changed accounting,
duplicate broadcasts, txid reuse, impossible transitions, missing block
identity, over-ceiling fees, and sensitive field names.

Developer/operator summary:

```bash
make transaction-micro-lab-check
make transaction-micro-lab-status
```

The status reports confirmed transaction and type coverage, in-flight and
terminal counts, recipient/fee totals, minimum/maximum/average fee, and average
broadcast-to-confirmation time. It is evidence-only: neither command reads a
private wallet, signs, authorizes, or broadcasts.
