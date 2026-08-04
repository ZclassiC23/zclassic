# ZCL file-market protocol

This page is the developer and agent map for paid file offers and purchases.
The seller-authenticated offer-ingress foundation is implemented; local paid
offer creation and payment-to-file-unlock remain deliberately unavailable.

## Table of contents

1. [Current boundary](#current-boundary)
2. [Lifecycle](#lifecycle)
3. [Signed offer wire](#signed-offer-wire)
4. [Validation invariants](#validation-invariants)
5. [API posture](#api-posture)
6. [Where code belongs](#where-code-belongs)
7. [Proof checklist](#proof-checklist)

## Current boundary

```text
seller-signed offer -> peer decode -> network/time/signature validation
                    -> dedup/rate limit -> cache + ActiveRecord persistence
                    -> exact-wire forwarding

local manifest/sign/announce       PLANNED
buyer plan/reserve/pay             PLANNED
seller exact-output verification   PLANNED
paid file unlock                   PLANNED
```

`zfileoffer` is an authenticated P2P contract, not a blockchain transaction.
The eventual `market_purchase` is a composite workflow whose payment leg is a
real ZClassic transaction. The transaction catalog therefore keeps
`market_purchase` as `planned` until payment, unlock, confirmation, conflict,
expiry, restart, and reorg behavior are connected and proven end to end.

The old `zfilelist` format is accepted only for price-zero ROM recovery
artifacts. An unsigned paid `zfilelist` entry is discarded. The legacy
`zmarket_offer` and `zmarket_buy` RPCs fail closed instead of reporting a sale
or leaving an in-memory download that cannot progress.

## Lifecycle

```text
SELLER                              BUYER
  |                                  |
  | build canonical content manifest |
  | sign network-bound offer          |
  +---------- zfileoffer ------------>|
  |                                  | verify + choose exact offer_id
  |                                  | create wallet-bound payment plan
  |                                  | owner-authorized commit
  |<--------- payment txid -----------+
  | verify tx output/address/amount    |
  | retain gate across restart/reorg   |
  +---------- authorized chunks ------>|
```

An offer does not grant wallet authority. A buyer must select a current signed
`offer_id`; a future spend plan must bind that id, wallet instance, network,
tip, custody snapshot, exact output, maximum fee, expiry, and idempotency key.

## Signed offer wire

`zfileoffer.v1` is exactly 535 bytes. Integers are little-endian.

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 8 | `ZFMOFF\r\n` magic |
| 8 | 2 | version (`1`) |
| 10 | 32 | ZClassic network genesis hash |
| 42 | 32 | seller Ed25519 public key |
| 74 | 8 | seller nonce |
| 82 | 32 | SHA3 content-manifest root |
| 114 | 8 | file size in bytes |
| 122 | 4 | fixed chunk size |
| 126 | 4 | exact chunk count |
| 130 | 8 | price per MiB in zatoshis |
| 138 | 43 | raw Sapling payment address (`diversifier || pk_d`) |
| 181 | 16 | origin IP bytes |
| 197 | 2 | origin port |
| 199 | 8 | issue time |
| 207 | 8 | expiry time |
| 215 | 1 | filename length |
| 216 | 255 | zero-padded filename |
| 471 | 64 | seller Ed25519 signature |

The signature covers a domain-separated SHA3-256 root of bytes `0..470`.
`offer_id` is a second domain-separated SHA3-256 commitment to all 535 bytes.
Mutable gossip fields (`last_seen`, hop TTL) are outside the contract.

The exact seller amount is integer-only:

```text
ceil(size_bytes * price_per_mb_zat / 1,048,576)
```

It must be in `[1, MAX_MONEY]`. Settlement code must call
`file_market_offer_total_zat`; floating-point display math is never payment
authority.

## Validation invariants

A paid offer enters cache or persistence only when all of these hold:

- version, fixed wire size, magic, genesis, content root, seller key, and nonce
  are valid;
- size and chunk count agree exactly;
- the price and derived total fit ZClassic's money range;
- the Sapling diversifier is valid and `pk_d` decodes to a non-identity Jubjub
  point;
- endpoint and filename are present;
- issue/expiry order is valid and lifetime is at most one hour;
- the expected network matches and the offer is currently live;
- the seller signature verifies;
- an exact duplicate is idempotent, refreshed contracts replace the same
  content root, and both verification attempts and fresh offers are bounded
  per peer.

Private seller seeds exist only at a future owner-controlled signing boundary.
They must never enter offer structs, P2P messages, database rows, command
responses, logs, receipts, fixtures intended as public evidence, or agent
context.

## API posture

| Surface | Status | Meaning |
|---|---|---|
| `app market list` | ready/read-only | Lists cached offers; paid rows expose `authenticated`, `offer_id`, expiry, and exact total. |
| `app market status` | ready/read-only | Reports bounded cache/persistence state. |
| `app market offer` | planned/fail-closed | Awaits local manifest construction, owner signing, and origin announcement. |
| `app market buy` | planned/fail-closed | Awaits wallet-bound planning, exact payment verification, and unlock. |
| `romseed_register` | ready/operator | Registers verified price-zero recovery artifacts; it is not a paid offer. |
| `app transaction-types show --type=market_purchase` | ready/read-only | Canonical machine-readable readiness and proof record. |

Agents must discover the exact schema from the native registry and reject a
`planned` operation. Direct legacy RPC access does not bypass that state: both
write placeholders also refuse without mutation.

## Where code belongs

Follow [`AGENT_ARCHITECTURE.md`](./AGENT_ARCHITECTURE.md) for the remaining
feature slice:

| Responsibility | Authority |
|---|---|
| fixed offer codec/signature/amount | `lib/net/src/file_market_offer.c` |
| bounded peer ingress/cache | `lib/net/src/file_market.c`, `msgprocessor.c` |
| durable offer rows and validation | `app/models/src/file_offer.c` |
| schema migration | `app/models/src/database_migrate_features_v49_up.c` |
| future plan/commit and reconciliation | a dedicated `app/services/` service |
| thin native/REST adapters | `app/controllers/` plus `config/commands/` |
| semantic readiness/proof | `transaction_types.def` and transaction lab |

The future purchase service must reuse wallet, UTXO/note, mempool, vault
intent, and chain-confirmation authorities. It must not maintain an independent
balance or confirmation counter.

## Proof checklist

For offer-contract changes, run at least:

```bash
make -j"$(nproc)" build-only
make -j"$(nproc)" t-fast-exact ONLY=test_file_market
make -j"$(nproc)" t-fast-exact ONLY=test_blob_read_bounds
make transaction-lab-check
make docs-api-reference
make lint
```

Before promoting `market_purchase`, add isolated end-to-end evidence for exact
outputs and fees, idempotent commit, concurrent reservation, insufficient
funds, wrong network/offer, confirmation, conflict, expiry, restart, and reorg.
Only then change its transaction-catalog availability or proof level.

<!-- claim: symbol-present MSG_FILE_OFFER lib/net/include/net/file_market.h -->
<!-- claim: symbol-present file_market_offer_total_zat lib/net/src/file_market_offer.c -->
<!-- claim: symbol-present market_purchase app/controllers/include/controllers/transaction_types.def -->
