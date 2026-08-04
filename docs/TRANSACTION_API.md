# ZClassic23 transaction API

This is the map from a human intention (pay, shield, register a name, anchor a
release, settle a swap) to the exact typed command that can create the
corresponding ZClassic transaction. The machine-readable catalog is the source
of truth; this page explains how to use it safely.

## Table of contents

1. [Big picture](#big-picture)
2. [First call for an agent](#first-call-for-an-agent)
3. [One-call AI guide](#one-call-ai-guide)
4. [Catalog fields](#catalog-fields)
5. [Transaction families](#transaction-families)
6. [Safe plan/commit workflow](#safe-plancommit-workflow)
7. [What is not a chain transaction](#what-is-not-a-chain-transaction)
8. [Proof and statistics](#proof-and-statistics)
9. [Adding a transaction type](#adding-a-transaction-type)

## Big picture

There are three layers, and none changes legacy ZClassic consensus:

```text
human intent
    |
    v
typed native command ---------> plan / explicit commit
    |                                  |
    v                                  v
ZCL transaction bytes --------> mempool -> block -> confirmation
    |
    +-- ordinary scripts (transparent, P2SH HTLC)
    +-- Sapling spends/outputs and encrypted memos
    +-- OP_RETURN application records (ZSLP, ZNAM, ZID, ZDIR, ZANC/ZCODE)
```

The catalog describes semantic transaction shapes, not aliases. For example,
all `t→z`, `z→z`, and `z→t` payments use one command but appear as three types
because their privacy and chain behavior differ. Conversely, an atomic-swap
funding flow uses two commands—create the HTLC contract, then pay its returned
P2SH address—but appears as one composite type.

Discovery never grants authority. Reading this resource cannot unlock a wallet,
create a vault session, approve a plan, or broadcast a transaction.

## First call for an agent

Use the native interface when operating the node:

```bash
zclassic23 app transaction-types list
zclassic23 app transaction-types show --type=znam_register
zclassic23 app transaction-types guide --type=znam_register
zclassic23 discover describe app.names.register
zclassic23 discover schema app.names.register
```

Public read-only clients may use the REST mirror:

```text
GET /api/v1/transaction-types
GET /api/v1/transaction-types/znam_register
```

The collection schema is `zcl.transaction_types.index.v2` and deliberately
contains compact discovery rows that fit the native response budget; a member
is the full `zcl.transaction_type.v2` contract. The collection also reports
`demonstrated_count`, `blocked_count`, `chain_confirmed_count`,
`mainnet_live_proven_count`, `proof_test_group_count`, and
`fully_demonstrated`, so an agent can assess proof coverage without parsing all
34 rows. `core.wallet.transaction.list` is different: it is
wallet history, not the type catalog. `app.protocols` describes broader
application protocols, not an exhaustive transaction inventory.

A full member keeps its primary `test_group` and also exposes
`supplemental_test_groups`. Supplemental groups are required when one claim
depends on independent evidence—for example, the HTLC rows retain their public
workflow or direct-interpreter group while adding `test_simnet_contract` for
the mined chain lifecycle. `make transaction-lab-proof` derives and
deduplicates both sources, so a future proof cannot silently replace one axis
with another.

An AI should select by `id`, reject `planned`, respect `network_policy`, then
inspect the named command's current input schema. It must not synthesize flags
or infer a wallet scope from examples.

## One-call AI guide

`app transaction-types guide --type=<id>` joins a catalog member to the live
command registry in one bounded read. It returns role-labeled builder, commit,
component, and inspection contracts with each command's exact schemas, allowed
input keys, example, effect, risk, authority, and confirmation mode. It also
returns a fail-closed `agent_decision`, whether a current custody snapshot and
owner authorization are required, the focused proof group, and a short safety
checklist.

The guide grants no authority and executes nothing. A `ready` member may say
`can_execute:true` when every referenced command is currently ready; a
`process_only`, `contained`, or `planned` member still returns its useful
contract but tells the caller to receive only or refuse. For example,
`blog_anchor` is `contained`: `app blog anchor` can plan/commit the on-chain
anchor for an already stored, signed event, but the separate operation that
creates that signed event remains behind the unfinished runtime App grant
broker. An AI must not reinterpret anchor readiness as event-signing authority.

## Catalog fields

| Field | Meaning |
|---|---|
| `availability` | `ready`, `process_only`, `contained`, or `planned`. |
| `transaction_role` | A direct chain transaction, overlay transaction, or multi-command composite. |
| `chain_encoding` | The actual chain shape: standard script, Sapling, OP_RETURN, P2SH HTLC, and so on. |
| `lifecycle` | Whether the operation is plan/commit, build/sign/broadcast, receive-only processing, or a two-party ceremony. |
| `builder_command` | First typed command. Empty means no supported builder exists. |
| `commit_command` | The value-moving/broadcast step. It may be the same command with `confirm:true`. |
| `component_commands` | Every additional command required by a composite flow. |
| `network_policy` | Where the path may run. Values such as `isolated_non_mainnet_only`, `no_broadcast_path`, and `no_public_constructor_or_broadcast_path` are hard boundaries, not suggestions. |
| `proof_level` / `test_group` | Strongest checked-in isolated proof and the exact focused test that reproduces it. |
| `lab_case_id` | Matching append-only notebook case, when one exists. |
| `evidence_status` | `demonstrated` when checked-in evidence exists; otherwise explicit `blocked`. |
| `mainnet_live_proven` | Derived from `proof_level == live_confirmed`; currently false for every type. Monetary mainnet statistics come only from the notebook ledger. |

`process_only` is not zero support: the node can parse, validate, connect, index,
and display the transaction, but agents cannot create a new one. `contained`
means code exists but policy deliberately refuses the named network. `planned`
means no end-to-end broadcast path exists and must never be presented as done.

## Transaction families

The native catalog carries every individual entry. This grouped map is the
human index:

| Family | Semantic type ids | Current posture |
|---|---|---|
| Base ZCL | `coinbase_reward`, `transparent_t_to_t`, `raw_custom_transaction`, `sapling_t_to_z`, `sapling_z_to_z`, `sapling_z_to_t`, `sprout_joinsplit` | Transparent and Sapling builders ready; coinbase and Sprout are process-only. |
| ZSLP tokens | `zslp_genesis`, `zslp_mint`, `zslp_send`, `zslp_burn` | Typed plan/commit builders. |
| ZNAM names | `znam_register`, `znam_update`, `znam_transfer`, `znam_renew`, `znam_set_record`, `znam_set_text` | Typed plan/commit builders with owner checks. |
| Messaging | `sapling_onchain_memo` | On-chain ZMSG uses an encrypted Sapling memo; P2P messaging is off-chain. |
| Identity/directory | `zid_anchor`, `zid_rotate`, `zid_revoke`, `zdir_register`, `zdir_deregister` | Explicit OP_RETURN compose/broadcast paths; all five exact command shapes have isolated owner-funded mined-and-projected proofs. |
| Anchors/ZCODE | `zanc_epoch_anchor`, `zcode_release_anchor` | SHA3 commitment anchors; epoch-ZANC commits the declared catalog range and ZCODE folds signed releases. Both exact command-produced OP_RETURN shapes have isolated mined-and-projected proofs. |
| Blog | `blog_anchor` | `app blog anchor` durably plans/commits the strict ZBLG v1 transaction for an existing verified event. The plan requires explicit custody scope and idempotency; new event signing remains broker-contained. |
| Atomic swaps | `htlc_initiate`, `htlc_participate`, `htlc_redeem`, `htlc_refund` | Contract preparation plus explicit funding; redeem/refund settle the ZCL leg. |
| Commerce | `store_transparent_payment`, `store_shielded_payment`, `yardsale_atomic_purchase`, `market_purchase` | Exact transparent and shielded store payments are isolated-mined and reconciled against their bound one-time order identity; the shielded command remains isolated-only. The exact jointly signed Yardsale controller broadcast is isolated-mined with exact settlement and fee accounting. File-market plan/commit/retrieve mines its exact memo payment before proving authenticated delivery, verified assembly, and atomic publication. |

## Safe plan/commit workflow

For any value-moving operation, an agent follows this sequence:

```text
catalog -> exact command schema -> current bound custody snapshot
        -> confirm:false plan -> owner-authorized confirm:true commit
        -> txid/operation reconciliation -> notebook evidence
```

1. Read the catalog member and stop unless its availability and network policy
   permit the requested environment.
2. Run `discover describe` and `discover schema` for `builder_command` and
   `commit_command`; the catalog is navigation, while the command registry owns
   exact input keys.
3. Read `metaverse agent money --dir=<broker-dir>`. A missing, stale,
   conflicted, incomplete, or wrong-wallet snapshot is a refusal, never a zero
   balance. The wallet scope must be explicit.
4. Create the typed plan and preserve its wallet identity, outputs, maximum
   fee, expiry, snapshot root, and idempotency identity exactly. Some plans are
   pure previews; durable vault and market-purchase plans intentionally mutate
   only reservation state so concurrent commitments cannot oversubscribe the
   wallet. Planning never broadcasts value.
5. Commit only after explicit authorization. A changed tip-bound plan, output,
   fee, wallet, network, reservation, or custody snapshot fails closed.
6. Inspect the returned txid or async operation through the member's
   `inspect_command`, wait for the required confirmation state, then record only
   redacted evidence in the transaction notebook.

Private keys, recovery words, addresses, endpoints, datadir paths, grant tokens,
swap secrets, and private memos never belong in catalog output, agent receipts,
logs, or the notebook. A public mainnet txid may be recorded after broadcast.

## What is not a chain transaction

- `app.messaging.send` with `channel=p2p` writes to a peer socket; only
  `channel=onchain` creates the Sapling-memo transaction in this catalog.
- `app.swap.initiate` and `app.swap.participate` create and persist HTLC
  contracts but broadcast nothing. The composite catalog rows explicitly name
  the later transparent funding command.
- `yardsale.seller.arm` configures the seller. The completed two-party ceremony
  is what produces the atomic ZCL/ZSLP transaction.
- ZCODE reward and badge assets are simulated local objects today. ZCODE
  science, package, DHT, and fetch operations are also off-chain. Only
  `zcode_release_anchor` in this catalog commits a ZCODE-derived root on-chain.
- File-market offers, challenges, proofs, and signed payment claims are P2P or
  local workflow objects. The real Sapling payment leg is exposed as
  `app market purchase plan|commit|status`: it binds the authenticated offer,
  exact range and amount, wallet identity, network, tip, custody snapshot,
  maximum fee, expiry, and idempotency key. Planning atomically reserves value
  plus fee; commit broadcasts at most once and persists the txid and encrypted
  buyer credential across restart.
  `app market purchase retrieve` then requires a confirmed full-file payment,
  targets only the endpoint authenticated by that signed offer, resumes only
  after rehashing durable staged chunks, verifies the full manifest root, and
  atomically publishes without overwriting an existing destination. A payment
  is never presented as a completed download before that final state.
  Paid offer ingress and exact confirmed Sapling-payment reconciliation are
  network-bound, expiry-checked, durable, and reorg-aware. The session-bound
  `zfileget.v1` delivery request verifies the buyer and authorizes before
  invoking the owner-private content reader. `app market content register`
  binds a signed offer to exact local bytes; restart reconstructs that binding
  and file mutation revokes delivery. See
  [`FILE_MARKET_PROTOCOL.md`](./FILE_MARKET_PROTOCOL.md) for the exact contract
  and developer workflow.
- Legacy `zclassicd` wallet funds are operator-owned and outside agent custody.

## Proof and statistics

The catalog's `proof_level` is isolated technical evidence, not a claim that
money moved on mainnet. Reproduce the bounded matrix and print the two separate
bars with:

```bash
make transaction-lab-proof
make transaction-lab-status
make transaction-lab-check
```

The append-only event ledger is
[`work/transaction-lab-events.jsonl`](./work/transaction-lab-events.jsonl), and
the procedure and safety cap are in
[`work/TRANSACTION_LAB.md`](./work/TRANSACTION_LAB.md). Only a `live_confirmed`
mainnet event with a public txid increments live counts, recipient value, or
fees. Simnet confirmation never increments live money statistics.

The current complete inventory is **34/34 isolated cases passing**, with **33
simulated-chain confirmations**, **0 mainnet confirmations**, and **0 ZCL**
live recipient value or fees. The earlier 33/33 result was complete for the
catalog as then declared; the later audit found ZBLG, made the gap explicit,
then added its typed plan/commit and mined proof rather than hiding it.

The safe ZBLG sequence is:

```bash
# Inspect the exact keys before constructing a request.
zclassic23 discover schema app.blog.anchor

# Create a durable plan for an event already stored and ZNAM-owner-verified.
# This prepares exact signed bytes and reserves only the maximum fee; it does
# not broadcast. Keep the returned plan_id.
zclassic23 app blog anchor --input='{"wallet_scope":"dev","name":"alice","event_id":"<64-hex>","idempotency_key":"alice-post-1"}'

# Owner-authorized commit uses only the explicit scope and durable plan ID.
# The event-signing operation is a separate contained capability.
zclassic23 app blog anchor --input='{"wallet_scope":"dev","plan_id":"<64-hex>","confirm":true}'
```

The returned `commit_input` is the exact second-call document. Never rebuild it
from a default wallet flag. The plan is bound to wallet instance ID, network
genesis, operator lane, tip hash, custody snapshot root, exact prepared
transaction bytes, actual and maximum fee, expiry, event ID, current ZNAM
owner, and idempotency key. Planning atomically reserves the maximum fee; a
changed money snapshot or event owner conflicts the plan instead of silently
replanning. A restart reloads the same prepared bytes, so retry can only relay
the same transaction ID.

## Adding a transaction type

Future developers make one coherent feature slice:

1. Add one semantic row to
   `app/controllers/include/controllers/transaction_types.def`. Reuse a type id
   only if the on-chain meaning is unchanged; aliases are component commands,
   not new semantic types.
2. Add or update the typed native builder/reader in `config/commands/*.def`.
   Every non-empty command named by the catalog is test-checked against the
   live command registry and exposed through `app transaction-types guide`.
3. Add the isolated proof to `tools/dev/transaction_lab_catalog.def` and its
   append-only evidence event. Never label builder-only evidence as a chain
   confirmation.
4. Update this grouped index only when a family or safety posture changes; do
   not duplicate the detailed machine catalog here.
5. Run `make t-fast ONLY=test_api`, the referenced transaction test group,
   `make transaction-lab-check`, `make lint`, and the normal build/test gates.

<!-- claim: symbol-present app.transaction-types.list config/commands/apps.def -->
<!-- claim: symbol-present app.transaction-types.show config/commands/apps.def -->
<!-- claim: symbol-present app.transaction-types.guide config/commands/apps.def -->
<!-- claim: file-present app/controllers/include/controllers/transaction_types.def -->
<!-- claim: file-present tools/dev/transaction_lab_catalog.def -->
