# ZCL file-market protocol

This page is the developer and agent map for paid file offers and purchases.
The seller-authenticated offer ingress and exact confirmed-payment authority
are implemented. The encrypted `zfileget.v1` request and authorize-before-read
gate are implemented. Owner-private paid-content registration and verified
chunk loading are implemented. Local paid-offer signing/announcement and buyer
wallet planning remain deliberately unavailable.

## Table of contents

1. [Current boundary](#current-boundary)
2. [Lifecycle](#lifecycle)
3. [Signed offer wire](#signed-offer-wire)
4. [Signed payment claim](#signed-payment-claim)
5. [Canonical Sapling memo](#canonical-sapling-memo)
6. [Payment reconciliation](#payment-reconciliation)
7. [Buyer-authenticated delivery request](#buyer-authenticated-delivery-request)
8. [Owner-private seller content](#owner-private-seller-content)
9. [Validation invariants](#validation-invariants)
10. [API posture](#api-posture)
11. [Where code belongs](#where-code-belongs)
12. [AI and developer workflow](#ai-and-developer-workflow)
13. [Proof checklist](#proof-checklist)

## Current boundary

```text
seller-signed offer -> peer decode -> network/time/signature validation
                    -> dedup/rate limit -> cache + ActiveRecord persistence
                    -> exact-wire forwarding

local manifest/sign/announce       PLANNED
buyer plan/reserve/pay             PLANNED
signed payment-claim ingress       IMPLEMENTED
seller exact-output verification   IMPLEMENTED
restart/reorg reconciliation       IMPLEMENTED
per-chunk authorization service    IMPLEMENTED
encrypted paid file request        IMPLEMENTED
authorize-before-content-read      IMPLEMENTED
owner content registration/reader  IMPLEMENTED
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
  |<-- signed claim + payment txid ----+
  | verify buyer signature             |
  | verify exact canonical Sapling     |
  | output/address/amount/memo          |
  | retain/recheck across restart/reorg |
  +--- buyer-authenticated chunks ----->|
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

## Signed payment claim

The legacy 72-byte `root_hash + txid + range` notification is rejected. It
could not identify the signed offer or network, carried no amount, and let a
mempool sighting masquerade as payment.

`zfilepay.v1` is exactly 218 bytes. Integers are little-endian.

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 8 | `ZFMPAY\r\n` magic |
| 8 | 2 | version (`1`) |
| 10 | 32 | ZClassic network genesis hash |
| 42 | 32 | exact signed `offer_id` |
| 74 | 32 | claimed shielded-payment txid |
| 106 | 4 | first paid chunk index |
| 110 | 4 | number of contiguous paid chunks |
| 114 | 8 | exact range price in zatoshis |
| 122 | 32 | ephemeral buyer Ed25519 public key |
| 154 | 64 | buyer Ed25519 signature |

The signature covers a domain-separated SHA3-256 root of bytes `0..153`.
`claim_id` is a separate domain-separated SHA3-256 commitment to all 218
bytes. The claim is public evidence and a lookup key—not proof of payment.

Range pricing calls `file_market_offer_range_zat`:

```text
range_bytes = real bytes covered by [chunk_start, chunk_start + count)
amount_zat  = ceil(range_bytes * price_per_mb_zat / 1,048,576)
```

The final partial chunk is charged only for its real bytes. Zero-length,
overflowing, out-of-bounds, altered, or over-`MAX_MONEY` ranges fail closed.

## Canonical Sapling memo

The paying Sapling output carries exactly 512 bytes:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 8 | `ZFMPAYM1` magic |
| 8 | 32 | network genesis hash |
| 40 | 32 | signed `offer_id` |
| 72 | 4 | first paid chunk |
| 76 | 4 | chunks paid |
| 80 | 8 | exact amount in zatoshis |
| 88 | 32 | buyer public key |
| 120 | 392 | zero padding |

Every byte must match. Prefix matching is forbidden. Binding the buyer public
key into the encrypted memo prevents another peer from copying a public txid
and claiming the paid bytes; the future file request must prove possession of
the corresponding ephemeral private key. That private key remains in the
buyer-owned signing boundary and never enters P2P structs, database rows,
logs, receipts, command output, or agent context.

## Payment reconciliation

`zfilepay` ingress now follows this fail-closed pipeline:

```text
fixed claim decode + buyer signature
        |
        v
persisted seller-signed offer lookup
        |
        v
active tip == canonical SQLite tip
wallet projection height == active tip
        |
        v
exact local Sapling note match:
  txid + seller address + amount + all 512 memo bytes
        |
        v
transaction belongs to a canonical block
        |
        v
minimum confirmations + payment block inside offer window
        |
        v
CONFIRMED per-chunk authorization
```

The durable `market_payment_claims` row retains the exact signed offer and
claim wires so restart does not depend on an expiring gossip cache. Its status
is only a rebuildable projection. `market_payment_authorize_chunk` re-runs the
wallet/chain checks on every access; a reorg revokes a formerly confirmed
grant, and later reconfirmation can restore it. Spending the seller's received
note later does not erase the historical purchase.

State meanings are strict:

| Status | Meaning |
|---|---|
| `PENDING` | Exact transaction has not reached the required canonical confirmation. |
| `CONFIRMED` | Exact output and memo are in the current canonical chain. |
| `UNKNOWN` | Chain tip, wallet projection, or required reader is not current/complete. |
| `CONFLICTED` | Canonical evidence contradicts the claim, including reorg removal. |
| `REJECTED` | Claim contract, signature, offer, network, range, or amount is invalid. |

## Buyer-authenticated delivery request

`zfileget.v1` rides an encrypted `FS_REQUEST` frame and is exactly 206 bytes.
Integers are little-endian.

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 8 | `ZFGETV1\n` magic |
| 8 | 2 | version (`1`) |
| 10 | 32 | ZClassic network genesis hash |
| 42 | 32 | exact signed `offer_id` |
| 74 | 4 | requested chunk index |
| 78 | 32 | buyer Ed25519 public key from the payment memo |
| 110 | 32 | session ID |
| 142 | 64 | buyer Ed25519 signature |

The session ID is domain-separated SHA3-256 over network genesis, the
initiator handshake nonce, and the responder handshake nonce. The signature
covers a domain-separated root of bytes `0..141`. A captured request therefore
cannot be moved onto another file-service session, and changing the offer,
chunk, buyer, network, or session invalidates the signature.

Every request receives one encrypted 84-byte `zfileget.reply.v1` before any
raw chunk bytes. Its fixed fields are reply magic, version, typed status,
offer ID, chunk index, byte size, and SHA3-256. Only `READY` is followed by the
raw authenticated chunk. Other statuses are `MALFORMED`, `UNAUTHENTICATED`,
`PENDING`, `UNKNOWN`, `CONFLICTED`, `REJECTED`, `CONTENT_UNAVAILABLE`, and
`RESOURCE_LIMIT`.

The server call order is load-bearing:

```text
fixed decode -> session/signature verification
             -> synchronous current payment authorization
             -> owner-registered content loader
             -> content SHA3 verification
             -> typed READY reply
             -> paid bytes
```

`PENDING`, `UNKNOWN`, `CONFLICTED`, `REJECTED`, and unauthenticated requests
never invoke the content loader. The production boot adapter already connects
the authorization step to `market_payment_authorize_chunk` and the loader to
the owner-created `market_contents` resource. A confirmed payment still gets
`CONTENT_UNAVAILABLE` when the exact offer is unregistered, its file vanished,
or its current chunk digest differs; none of those cases permits a path guess
or accidental free serve. The existing `ROM` path remains a separate
price-zero recovery service and cannot be used as a paid-content registry.

## Owner-private seller content

`app market content register` is an owner-only, no-funds mutation. It accepts
an exact signed `offer_id` and a local content path, then performs this bounded
workflow:

```text
authenticated current paid offer
        |
        v
open O_NOFOLLOW + require non-empty regular file
        |
        v
canonicalize and reopen the same device/inode
        |
        v
derive every 50 MiB chunk SHA3 and manifest root
        |
        v
require exact signed size/chunk count/root
        |
        v
atomically upsert one market_contents row
```

The row keeps the canonical private path and complete bounded chunk manifest
inside the operator's `node.db`. Native and private REST reads project only
`offer_id`, `root_hash`, `size_bytes`, `num_chunks`, and `registered_at`.
Paths and manifests are absent from replies, logs, P2P messages, and public
market listings. Registration is capped at 4,096 chunks (200 GiB at the fixed
50 MiB chunk size), so a signed but operationally unreasonable offer cannot
force an unbounded SQLite blob or hashing allocation.

Every paid read reopens the registered path with `O_NOFOLLOW`, requires the
same size and regular-file shape, reads exactly one chunk, and compares its
current SHA3 with the persisted manifest. Restart reconstructs the reader from
SQLite; replacement, truncation, or byte mutation revokes delivery without
altering payment evidence. The delivery layer independently hashes the returned
bytes again before emitting `READY`.

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
| `app market content list` | ready/owner-read | Lists path-free local seller bindings. |
| `app market content register` | ready/owner-write | Verifies and atomically binds exact private bytes to one signed offer; moves no funds and announces nothing. |
| `app market offer` | planned/fail-closed | Awaits local manifest construction, owner signing, and origin announcement. |
| `app market buy` | planned/fail-closed | Seller registration and authenticated delivery exist; buyer wallet plan/commit remains. |
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
| payment claim/memo codec | `lib/net/src/file_market_payment.c` |
| durable payment locator | `app/models/src/market_payment_claim.c` |
| exact chain/note reconciliation | `app/services/src/file_market_payment_service.c` |
| signed delivery codec and no-read gate | `lib/net/src/file_market_delivery.c` |
| private content rows and path-free projection | `app/models/src/market_content.c` |
| registration and verified chunk reader | `app/services/src/file_market_content_service.c` |
| boot authorization + content adapter | `config/src/boot_file_market_delivery.c` |
| future buyer plan/commit | a dedicated wallet-touching `app/services/` service |
| thin native/REST adapters | `app/controllers/` plus `config/commands/` |
| semantic readiness/proof | `transaction_types.def` and transaction lab |

The future purchase service must reuse wallet, UTXO/note, mempool, vault
intent, and chain-confirmation authorities. It must not maintain an independent
balance or confirmation counter.

## AI and developer workflow

Do not build a payment by copying structs or invoking an RPC from memory:

1. A seller first runs `app market content register` with the exact signed
   `offer_id`; then checks `app market content list`. Do not copy the private
   path into notes, receipts, logs, or peer messages.
2. Read `app transaction-types show --type=market_purchase` and refuse while
   availability is `planned`.
3. Discover the exact `app market buy` schema from the native registry once
   that operation becomes ready.
4. Select an exact current `offer_id`; never infer an offer from a filename or
   default peer.
5. Read identity-bound custody and create an owner-visible wallet plan that
   commits the offer, range, output, memo, maximum fee, expiry, tip, and
   idempotency key.
6. Keep the buyer private key and wallet keys behind their signing boundaries.
7. Treat a txid or `PENDING` claim as locked. Only the seller's synchronous
   `CONFIRMED` authorization permits bytes to leave the paid file service.
8. Sign each `zfileget.v1` request for the current encrypted session. Do not
   reuse a request body across reconnects or expose the ephemeral buyer seed.
9. Accept bytes only after the typed `READY` reply and verify the announced
   chunk SHA3. `CONTENT_UNAVAILABLE` means the seller has not registered the
   content locally; it is not permission to try a filesystem path.
10. On `UNKNOWN` or `CONFLICTED`, stop and report the reason; never substitute
   zero, retry a spend blindly, or bypass reconciliation.

## Proof checklist

For offer-contract changes, run at least:

```bash
make -j"$(nproc)" build-only
make -j"$(nproc)" t-fast-exact ONLY=test_file_market
make -j"$(nproc)" t-fast-exact ONLY=test_db_migration_idempotent
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
<!-- claim: symbol-present file_payment_auth_verify_for_offer lib/net/src/file_market_payment.c -->
<!-- claim: symbol-present market_payment_authorize_chunk app/services/src/file_market_payment_service.c -->
<!-- claim: symbol-present file_market_delivery_prepare lib/net/src/file_market_delivery.c -->
<!-- claim: symbol-present file_market_content_register app/services/src/file_market_content_service.c -->
<!-- claim: symbol-present db_market_content_save app/models/src/market_content.c -->
<!-- claim: symbol-present market_purchase app/controllers/include/controllers/transaction_types.def -->
