<!-- GENERATED FILE — DO NOT EDIT BY HAND.
     Source of truth: config/commands/*.def
     Template (editorial prose): docs/API_REFERENCE.md.in
     Generator: tools/gen_api_reference.c
     Regenerate: make docs-api-reference
     Gate: tools/lint/check_api_reference_generated.sh (check-api-reference-generated) -->
# API_REFERENCE.md — the native command tree, leaf by leaf

This is a **reference table**, not a spec. For the grammar, envelope shapes,
budgets, and migration status, read
[`docs/NATIVE_COMMAND_INTERFACE.md`](./NATIVE_COMMAND_INTERFACE.md) first —
that document is the frozen contract; this one enumerates every leaf the
contract currently declares. For additional native aliases and operator
contracts (`agentops`, `agentdiagnose`, `servicecatalog`, and the ZNAM/ZMSG/
Market/ZSWP methods), see [`docs/AGENT_API.md`](./AGENT_API.md).

## Source of truth

**This page is generated. Do not edit it by hand.**

Every table below is emitted by
[`tools/gen_api_reference.c`](../tools/gen_api_reference.c) directly from the
declarative `.def` files under [`config/commands/`](../config/commands/) — the
same files [`config/src/command_catalog.c`](../config/src/command_catalog.c)
expands into the immutable `g_catalog_commands[]` table
([`lib/kernel/src/command_registry.c`](../lib/kernel/src/command_registry.c)).
The generator is a second consumer of the identical X-macro grammar, so the C
preprocessor — not a hand-rolled parser — reads the catalog. A leaf added,
promoted, or re-scheduled in a `.def` file shows up here the moment the page is
regenerated, and never before or after.

Editorial prose (this section, the legend, the envelope summary, and the proof
table at the end) lives in the template
[`docs/API_REFERENCE.md.in`](./API_REFERENCE.md.in) and is copied through
verbatim; everything derived from the catalog replaces a
`<!-- ZCL-GEN:… -->` marker.

```bash
make docs-api-reference      # regenerate this page from config/commands/*.def
make lint                    # check-api-reference-generated fails on any drift
```

The drift gate is
[`tools/lint/check_api_reference_generated.sh`](../tools/lint/check_api_reference_generated.sh):
it regenerates into a temporary file and fails if the checked-in page differs.
Editing this page instead of the `.def` file it came from is therefore a lint
failure, not a silent lie.

To confirm any row against a running binary:

```bash
zclassic23 discover help [path]        # branch menu — immediate children only
zclassic23 discover describe <path>    # one leaf's full spec
zclassic23 discover search <text>      # ≤5 ranked matches
zclassic23 discover schema <path> --side=input|output
```

## What the catalog currently declares

| Catalog fact | Count |
|---|---|
| Registry entries (branches + leaves) | 330 |
| Top-level roots | 9 |
| Branches | 76 |
| Leaves (dispatchable command paths) | 254 |
| … `ready` (live handler in this build) | 212 |
| … `compat` (metadata only, names a fallback) | 17 |
| … `planned` (fail-closed BLOCKED, exit 3) | 25 |
| … dev-gated 🔧 (`ready` only in `zclassic23-dev`) | 16 |
| Leaves with `effect=mutate` | 68 |
| Leaves with `effect=destructive` | 4 |
| Leaves requiring **owner** authority | 56 |

Per source file:

| `.def` file | Entries | Branches | Leaves |
|---|---|---|---|
| `config/commands/root.def` | 10 | 5 | 5 |
| `config/commands/core.def` | 97 | 24 | 73 |
| `config/commands/apps.def` | 9 | 2 | 7 |
| `config/commands/app_features.def` | 26 | 5 | 21 |
| `config/commands/ops.def` | 43 | 8 | 35 |
| `config/commands/dev.def` | 45 | 11 | 34 |
| `config/commands/code.def` | 16 | 2 | 14 |
| `config/commands/accounts.def` | 11 | 2 | 9 |
| `config/commands/vault.def` | 14 | 3 | 11 |
| `config/commands/zcode.def` | 59 | 14 | 45 |


## Column legend

| Column | Meaning |
|---|---|
| **Command** | the leaf's dotted path written as CLI words (`core chain block get`), plus any declared aliases |
| **Avail** | `ready` (dispatches now) · `compat` (metadata only; NULL handler, names a `→` fallback) · `planned` (fail-closed BLOCKED, exit 3, no handler) · 🔧 = dev-gated |
| **Policy** | `effect / risk / authority`, then a non-`sync` mode and any confirmation ritual, then `· latency/cost` — the `zcl_command_*` enums, see `docs/NATIVE_COMMAND_INTERFACE.md` §13 |
| **Input keys** | the leaf's allowed input keys; **bold** = `positional_keys`, the key(s) the handler requires |
| **Output schema** | the leaf's `output_schema` id |
| **Example** | the invocation the `.def` entry declares as its canonical example |
| **Summary** | the leaf's one-line `summary`; for a non-`ready` leaf, its `availability_reason` follows in italics |

A `ready` leaf always has a live handler in this build; a `planned` leaf always
fails closed with `COMMAND_PLANNED` (exit 3) and no handler — never a silent
stub. Both invariants are proven for the *whole* catalog by
`test_command_registry_catalog.c` (`test_catalog_wellformed`,
`test_ready_leaves_bound`, `test_planned_fail_closed`), not asserted here.

**Dev-gated leaves** (🔧) are declared via `ZCL_COMMAND_DEV_READ` /
`ZCL_COMMAND_DEV_COMMAND`. In a `ZCL_DEV_BUILD` binary (`zclassic23-dev`) they
are `ready` with a real handler; in a release build they are honest `compat`
stubs whose `availability_reason` and `compat_target` tell you to run the same
command against `zclassic23-dev` instead. This page always renders the
**release** view, so it describes the binary an operator actually ships.
[`tools/lint/check_release_no_dev_symbols.sh`](../tools/lint/check_release_no_dev_symbols.sh)
proves via `nm` that the release binary links none of the dev executors, so the
distinction is structural, not a convention.

**Never RPC/REST-bound**: everything under `dev.*` is checkout-local by design
(see [`config/commands/README.md`](../config/commands/README.md): "No `lib/`
source may include App, controller, service, or development handler headers").
Almost every `ready` leaf under `core.*` and `ops.*` dispatches through
`zcl_native_bridge_command` — a direct call into either a native handler body
(`app/controllers/src/*_native_handlers.c`) or the backing JSON-RPC method,
through the command bridge.

## Roots

The root order below is a wire contract, not a presentation choice.

| Root | CLI | Kind | Avail | Summary |
|---|---|---|---|---|
| `status` | `status` | leaf | ready | Compact node status and next action |
| `core` | `core` | branch | ready | Consensus-bound node capabilities |
| `app` | `app` | branch | ready | Capability-scoped sovereign applications |
| `dev` | `dev` | branch | ready | Native edit, proof, and publication loop |
| `ops` | `ops` | branch | ready | Node diagnostics |
| `discover` | `discover` | branch | ready | Search and describe the command registry |
| `code` | `code` | branch | ready | Hierarchical source-code navigator |
| `vault` | `vault` | branch | ready | What this node owns, and what may act on it |
| `zcode` | `zcode` | branch | ready | ZCODE source-package hosting: publish, search, host |


## The tree, leaf by leaf

Sections follow catalog declaration order. A branch appears only when it owns
at least one direct leaf; a branch that exists purely to nest other branches is
represented by its children's sections.

### `status` — Compact node status and next action

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `status` | ready | read / read / public · fast/low | none | `zcl.core_status_brief.v1` | `zclassic23 status` | Compact node status and next action |

### `core` — Consensus-bound node capabilities

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core status` | ready | read / read / public · fast/low | none | `zcl.core_status.v2` | `zclassic23 core status` | Consensus node status: height, sync, health |
| `core status brief` | ready | read / read / public · fast/low | none | `zcl.core_status_brief.v1` | `zclassic23 core status brief` | Flat lean status: hstar, gap, blocker, conditions, peers, rss (full field list: docs/NATIVE_COMMAND_INTERFACE.md CLI UX contract) |

#### `core.chain` — Blocks, transactions, and mempool

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core chain tip` | ready | read / read / public · fast/low | none | `zcl.chain_tip.v1` | `zclassic23 core chain tip` | Active chain tip in one call |

#### `core.chain.block` — Block by height or hash

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core chain block get` | ready | read / read / public · fast/low | **`height`**, `hash`, `verbosity` | `zcl.block.v1` | `zclassic23 core chain block get --height=478544` | Get one block by height or hash |

#### `core.chain.transaction` — Transaction by id

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core chain transaction get` | ready | read / read / public · fast/low | **`txid`**, `verbose` | `zcl.transaction.v1` | `zclassic23 core chain transaction get --txid=<hex>` | Get one transaction by id |

#### `core.chain.mempool` — Mempool state

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core chain mempool status` | ready | read / read / public · fast/low | none | `zcl.mempool_status.v1` | `zclassic23 core chain mempool status` | Mempool size, bytes, and fee summary |
| `core chain mempool list` | ready | read / read / public · fast/low | none | `zcl.mempool_list.v1` | `zclassic23 core chain mempool list` | List mempool transaction ids |

#### `core.chain.wait` — Block until a chain condition holds

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core chain wait height` | planned | read / read / operator · persistent/low | **`height`**, `timeout_ms` | `zcl.wait_result.v1` | `zclassic23 core chain wait height --height=3200000` | Wait until the tip reaches a target height — *blocking wait proxy is deferred to the Wave 2.2 job protocol* |
| `core chain wait blocker` | planned | read / read / operator · persistent/low | **`blocker`**, `timeout_ms` | `zcl.wait_result.v1` | `zclassic23 core chain wait blocker` | Wait until a named blocker is raised or cleared — *blocking wait proxy is deferred to the Wave 2.2 job protocol* |
| `core chain wait halt` | planned | read / read / operator · persistent/low | `timeout_ms` | `zcl.wait_result.v1` | `zclassic23 core chain wait halt` | Wait until the node halts on a named blocker — *blocking wait proxy is deferred to the Wave 2.2 job protocol* |

#### `core.sync` — Sync phase and validation progress

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core sync status` | ready | read / read / public · fast/low | none | `zcl.sync_status.v1` | `zclassic23 core sync status` | Sync phase and header/block gap |
| `core sync validation` | ready | read / read / public · fast/low | none | `zcl.validation_status.v1` | `zclassic23 core sync validation` | Background validation progress |
| `core sync blockers` | ready | read / read / public · fast/low | none | `zcl.blockers.v1` | `zclassic23 core sync blockers` | Active named sync blockers |
| `core sync diagnose` | ready | read / read / operator · fast/moderate | none | `zcl.syncdiag.v1` | `zclassic23 core sync diagnose` | Diagnose why sync is not advancing |
| `core sync frontier offline` | ready | read / read / operator · fast/low | `datadir` | `zcl.core_sync_frontier_offline.v1` | `zclassic23 core sync frontier offline --input='{"datadir":"/home/you/.zclassic-c23"}'` | H* (reducer frontier) of a STOPPED/COPIED datadir |

#### `core.epoch` — Epoch anchors: commit the overlay catalog digest on-chain

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core epoch status` | ready | read / read / operator · fast/low | `datadir` | `zcl.core_epoch_status.v1` | `zclassic23 core epoch status` | Catalog digest, epoch position, and anchor presence |
| `core epoch anchor` | ready | mutate / wallet / operator · foreground/moderate | `datadir` | `zcl.core_epoch_anchor.v1` | `zclassic23 core epoch anchor` | Anchor the current catalog digest on-chain (spends a fee) |
| `core epoch verify` | ready | read / read / operator · fast/low | `height`, `datadir` | `zcl.core_epoch_verify.v1` | `zclassic23 core epoch verify` | Check the current epoch's anchor against the live digest |

#### `core.consensus` — Consensus reports, integrity, and mutation

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core consensus report` | ready | read / read / public · fast/low | none | `zcl.consensus_report.v1` | `zclassic23 core consensus report` | Consensus parity and rule report |
| `core consensus integrity` | ready | read / read / public · foreground/moderate | none | `zcl.data_integrity.v1` | `zclassic23 core consensus integrity` | SHA3 over consensus tables |
| `core consensus mmb` | ready | read / read / public · fast/low | none | `zcl.mmb.v1` | `zclassic23 core consensus mmb` | Merkle Mountain Belt commitment state |

#### `core.consensus.utxo` — UTXO set commitment and audit

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core consensus utxo commitment` | ready | read / read / public · foreground/moderate | none | `zcl.utxo_commitment.v1` | `zclassic23 core consensus utxo commitment` | SHA3 commitment over the UTXO set |
| `core consensus utxo audit` | ready | read / read / operator · foreground/moderate | none | `zcl.utxo_audit.v1` | `zclassic23 core consensus utxo audit` | Audit the UTXO set for drift |

#### `core.consensus.block` — Invalidate or reconsider a block

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core consensus block invalidate` | planned | destructive / core-recovery / **owner**, plan-commit · foreground/moderate | **`hash`** | `zcl.block_mutation.v1` | `zclassic23 core consensus block invalidate --hash=<hash>` | Mark a block invalid and reorg away from it — *chain-mutation confirmation handshake is a Wave 2.2 deliverable* |
| `core consensus block reconsider` | planned | mutate / core-recovery / **owner**, plan-commit · foreground/moderate | **`hash`** | `zcl.block_mutation.v1` | `zclassic23 core consensus block reconsider --hash=<hash>` | Clear an invalid mark and reconsider a block — *chain-mutation confirmation handshake is a Wave 2.2 deliverable* |

#### `core.network` — Peers and onion transport

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core network status` | ready | read / read / public · fast/low | none | `zcl.network_status.v1` | `zclassic23 core network status` | Network info and connections |
| `core network chain_view` | ready | read / read / public · fast/low | none | `zcl.network_chain_view.v1` | `zclassic23 core network chain_view` | Reachable-network chain view: modal tip, max height, our delta, forks |
| `core network census` | ready | read / read / public · fast/low | `ua-contains`, `min-height`, `seen-within`, `page`, `limit` | `zcl.network_census.v1` | `zclassic23 core network census --ua-contains=MagicBean --limit=25` | Paginated list of every node the crawler has seen |
| `core network node` | ready | read / read / public · fast/low | **`target`** | `zcl.network_node.v1` | `zclassic23 core network node 1.2.3.4:8033` | Everything known about one node (census row, history, edges) |
| `core network versions` | ready | read / read / public · fast/low | none | `zcl.network_versions.v1` | `zclassic23 core network versions` | User-agent / version distribution across the census |
| `core network graph` | ready | read / read / public · fast/low | none | `zcl.network_graph.v1` | `zclassic23 core network graph` | Topology stats: node/edge counts and top-advertised endpoints |

#### `core.network.peers` — Connected peers

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core network peers list` | ready | read / read / public · fast/low | none | `zcl.peers.v1` | `zclassic23 core network peers list` | List connected peers |
| `core network peers incidents` | ready | read / read / operator · fast/low | none | `zcl.peer_incidents.v2` | `zclassic23 core network peers incidents` | Recent peer misbehavior incidents |
| `core network peers latency` | ready | read / read / public · fast/low | none | `zcl.peer_latency.v1` | `zclassic23 core network peers latency` | Round-trip latency for every peer |
| `core network peers add` | planned | mutate / core-recovery / operator · fast/low | **`address`** | `zcl.peer_add.v1` | `zclassic23 core network peers add --address=<ip:port>` | Add an outbound peer connection — *peer-mutation binding is a Wave 2.2 deliverable* |

#### `core.network.onion` — Embedded onion service

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core network onion status` | ready | read / read / public · fast/low | none | `zcl.onion_status.v1` | `zclassic23 core network onion status` | Onion address and bootstrap status |
| `core network onion health` | ready | read / read / operator · fast/low | none | `zcl.onion_health.v1` | `zclassic23 core network onion health` | Onion reachability health |

#### `core.wallet` — Keys, balance, and transactions

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core wallet status` | ready | read / read / operator · fast/low | none | `zcl.wallet_status.v1` | `zclassic23 core wallet status` | Wallet summary and key counts |
| `core wallet balance` | ready | read / read / operator · fast/low | none | `zcl.wallet_balance.v1` | `zclassic23 core wallet balance` | Confirmed and total balance |
| `core wallet audit` | ready | read / read / operator · foreground/moderate | none | `zcl.wallet_audit.v1` | `zclassic23 core wallet audit` | Audit wallet key/UTXO consistency |
| `core wallet rescan` | ready | mutate / wallet / **owner** · background/high | `start_height` | `zcl.wallet_rescan.v2` | `zclassic23 core wallet rescan` | Rescan the chain for wallet transactions |
| `core wallet replay` | planned | mutate / wallet / **owner**, job · background/high | none | `zcl.wallet_replay.v1` | `zclassic23 core wallet replay` | Replay wallet state from chain — *wallet replay job binding is a Wave 2.2 deliverable* |

#### `core.wallet.address` — Transparent addresses

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core wallet address new` | ready | mutate / wallet / **owner** · fast/low | none | `zcl.wallet_address.v1` | `zclassic23 core wallet address new` | Derive and persist a new transparent address |
| `core wallet address list` | ready | read / read / operator · fast/low | none | `zcl.wallet_addresses.v1` | `zclassic23 core wallet address list` | List transparent addresses |
| `core wallet address import` | ready | mutate / wallet / **owner** · fast/low | **`address`** | `zcl.wallet_address.v1` | `zclassic23 core wallet address import --address=<addr>` | Import a watch-only address |
| `core wallet address export-key` | ready | read / read / **owner**, plan-commit · fast/low | **`address`**, `confirm` | `zcl.wallet_privkey.v1` | `zclassic23 core wallet address export-key --address=<addr>` | Export the private key for an address |
| `core wallet address label` | ready | mutate / app-write / operator · fast/low | **`address`**, `label` | `zcl.wallet_label.v1` | `zclassic23 core wallet address label --input='{"address":"t1...","label":"friends"}'` | Set or clear the label on an address |
| `core wallet address by-label` | ready | read / read / operator · fast/low | **`label`** | `zcl.wallet_by_label.v1` | `zclassic23 core wallet address by-label friends` | List addresses carrying a given label |

#### `core.wallet.utxo` — Spendable outputs

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core wallet utxo list` | ready | read / read / operator · fast/low | none | `zcl.wallet_utxos.v1` | `zclassic23 core wallet utxo list` | List spendable UTXOs |

#### `core.wallet.transaction` — Wallet transactions

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core wallet transaction list` | ready | read / read / operator · fast/low | none | `zcl.wallet_tx_list.v1` | `zclassic23 core wallet transaction list` | List recent wallet transactions |
| `core wallet transaction get` | ready | read / read / operator · fast/low | **`txid`** | `zcl.wallet_tx.v1` | `zclassic23 core wallet transaction get --txid=<hex>` | Get one wallet transaction by id |
| `core wallet transaction send` | ready | mutate / wallet / **owner**, plan-commit · foreground/moderate | `address`, `amount`, `idempotency_key`, `confirm` | `zcl.wallet_send.v1` | `zclassic23 core wallet transaction send --input='<obj>'` | Build, sign, and broadcast a payment |

#### `core.wallet.shielded` — Sapling shielded addresses and notes

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core wallet shielded address` | ready | mutate / wallet / **owner** · fast/low | none | `zcl.shielded_address.v1` | `zclassic23 core wallet shielded address` | Derive a new shielded address |
| `core wallet shielded balance` | ready | read / read / operator · fast/low | **`address`** | `zcl.shielded_balance.v1` | `zclassic23 core wallet shielded balance --address=<zaddr>` | Shielded balance for one address |
| `core wallet shielded notes` | ready | read / read / operator · fast/low | none | `zcl.shielded_notes.v1` | `zclassic23 core wallet shielded notes` | List spendable shielded notes |
| `core wallet shielded send` | ready | mutate / wallet / **owner**, job, plan-commit · background/high | `from`, `to`, `amount`, `idempotency_key`, `confirm` | `zcl.shielded_send.v1` | `zclassic23 core wallet shielded send --input='<obj>'` | Send a shielded payment |

#### `core.wallet.backup` — Wallet backup

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core wallet backup status` | ready | read / read / operator · fast/low | none | `zcl.wallet_backup_status.v1` | `zclassic23 core wallet backup status` | Wallet backup freshness |
| `core wallet backup now` | ready | mutate / wallet / **owner** · fast/low | none | `zcl.wallet_backup.v1` | `zclassic23 core wallet backup now` | Take a wallet backup now |

#### `core.storage` — Raw node storage

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core storage stats` | ready | read / read / operator · fast/low | none | `zcl.db_stats.v1` | `zclassic23 core storage stats` | Database size and table stats |
| `core storage integrity` | planned | read / read / operator · foreground/moderate | none | `zcl.storage_integrity.v1` | `zclassic23 core storage integrity` | Verify raw storage integrity — *a distinct storage-integrity handler is a Wave 2.2 deliverable* |
| `core storage query` | ready | read / read / operator · fast/moderate | **`sql`**, `limit` | `zcl.storage_query.v1` | `zclassic23 core storage query --sql='SELECT ...'` | Run one SELECT-only query over node.db |
| `core storage query offline` | ready | read / read / operator · fast/moderate | `datadir`, `sql`, `limit` | `zcl.storage_query.v1` | `zclassic23 core storage query offline --input='{"datadir":"/home/you/.zclassic-c23","sql":"SELECT ..."}'` | Run one SELECT-only query over a STOPPED/COPIED datadir's node.db |

#### `core.mining` — Mining info and benchmarks

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core mining status` | ready | read / read / public · fast/low | none | `zcl.mining_status.v1` | `zclassic23 core mining status` | Mining info and difficulty |
| `core mining benchmark` | ready | read / read / operator · foreground/moderate | none | `zcl.mining_benchmark.v1` | `zclassic23 core mining benchmark` | Run an Equihash solver benchmark |

#### `core.node` — Local node lifecycle

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core node bootstatus` | ready | read / read / operator · fast/low | `datadir` | `zcl.core_bootstatus.v1` | `zclassic23 core node bootstatus -datadir=/home/you/.zclassic-c23` | Pre-RPC boot status |
| `core node bootwait` | ready | read / read / operator · foreground/low | `datadir`, `timeout_ms`, `heartbeat_ms` | `zcl.core_bootstatus.v1` | `zclassic23 core node bootwait -datadir=/home/you/.zclassic-c23 --timeout_ms=120000` | Wait for boot to serve |

#### `core.identity` — Sovereign identities: resolve and anchor master keys

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core identity resolve` | ready | read / read / public · fast/low | **`pubkey`**, `name`, `datadir` | `zcl.core_identity_resolve.v1` | `zclassic23 core identity resolve --pubkey=<64hex>` | Resolve one master key by pubkey or ZNAM name |
| `core identity anchor` | ready | mutate / wallet / operator · foreground/moderate | **`pubkey`**, `datadir` | `zcl.core_identity_anchor.v1` | `zclassic23 core identity anchor --pubkey=<64hex>` | Anchor a master key on-chain (spends a fee) |
| `core identity rotate` | ready | mutate / wallet / operator · foreground/moderate | `pubkey`, `new_pubkey`, `datadir` | `zcl.core_identity_anchor.v1` | `zclassic23 core identity rotate --input='{"pubkey":"<64hex>","new_pubkey":"<64hex>"}'` | Rotate an anchored master key to a successor (spends a fee) |
| `core identity revoke` | ready | mutate / wallet / operator · foreground/moderate | **`pubkey`**, `datadir` | `zcl.core_identity_anchor.v1` | `zclassic23 core identity revoke --pubkey=<64hex>` | Retire an anchored master key with no successor (spends a fee) |
| `core identity list` | ready | read / read / public · fast/low | `limit`, `offset`, `datadir` | `zcl.core_identity_index.v1` | `zclassic23 core identity list --limit=25` | Page the anchored identities, newest anchor first |

#### `core.zdir` — On-chain node directory: announce and retire onion hostnames

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `core zdir register` | ready | mutate / wallet / operator · foreground/moderate | **`hostname`**, `pubkey`, `datadir` | `zcl.core_zdir_register.v1` | `zclassic23 core zdir register --hostname=<56 base32>.onion` | Announce a v3 onion hostname on-chain as a node (spends a fee) |
| `core zdir deregister` | ready | mutate / wallet / operator · foreground/moderate | **`hostname`**, `datadir` | `zcl.core_zdir_register.v1` | `zclassic23 core zdir deregister --hostname=<56 base32>.onion` | Retire an onion hostname from the on-chain directory (spends a fee) |

### `app` — Capability-scoped sovereign applications

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `app list` | ready | read / read / public · fast/low | none | `zcl.app_index.v1` | `zclassic23 app list` | List installed App manifests |
| `app inspect` | ready | read / read / public · fast/low | **`app_id`** | `zcl.app_manifest_summary.v1` | `zclassic23 app inspect social` | Inspect one App manifest and bindings |
| `app protocols` (aliases: `appprotocols`) | compat → `zclassic23 appprotocols` | read / read / public · fast/low | none | `zcl.app_protocols.v1` | `zclassic23 app protocols` | List App protocol contracts — *native adapter is not executable yet; use the compatibility target* |

#### `app.service` — Token-gated services declared in the service catalog

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `app service list` | ready | read / read / public · instant/tiny | none | `zcl.service_binding_index.v1` | `zclassic23 app service list` | List declared services and their catalog identity |
| `app service inspect` | ready | read / read / public · instant/tiny | **`service`** | `zcl.service_binding.v1` | `zclassic23 app service inspect reference` | Inspect one service binding: namespaces, token gate, isolation |
| `app service access` | ready | read / read / public · fast/low | **`service`**, `address`, `datadir`, `tip_height` | `zcl.service_access_verdict.v1` | `zclassic23 app service access reference` | Evaluate one service's token gate and explain the verdict |
| `app service status` | ready | read / read / public · instant/tiny | **`service`** | `zcl.service_lifecycle.v1` | `zclassic23 app service status` | Show each declared service's runtime lifecycle state |

#### `app.names` — Names

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `app names resolve` | ready | read / read / public · fast/low | **`name`** | `zcl.app_name_record.v1` | `zclassic23 app names resolve alice` | Resolve a ZCL Name to its target |
| `app names list` | ready | read / read / public · fast/low | none | `zcl.app_name_index.v1` | `zclassic23 app names list` | List registered ZCL Names |
| `app names register` | ready | mutate / app-write / **owner**, plan-commit · foreground/moderate | **`name`**, `type`, `value`, `confirm` | `zcl.app_name_txresult.v1` | `zclassic23 app names register --input='{"name":"alice","type":"zaddr","value":"zs1..","confirm":true}'` | Register a ZCL Name on-chain |
| `app names update` | ready | mutate / app-write / **owner**, plan-commit · foreground/moderate | **`name`**, `type`, `value`, `confirm` | `zcl.app_name_txresult.v1` | `zclassic23 app names update --input='{"name":"alice","type":"zaddr","value":"zs1..","confirm":true}'` | Replace a ZCL Name's primary target |
| `app names transfer` | ready | mutate / app-write / **owner**, plan-commit · foreground/moderate | **`name`**, `new_owner`, `confirm` | `zcl.app_name_txresult.v1` | `zclassic23 app names transfer --input='{"name":"alice","new_owner":"t1..","confirm":true}'` | Transfer ZCL Name ownership |
| `app names renew` | ready | mutate / app-write / **owner**, plan-commit · foreground/moderate | **`name`**, `confirm` | `zcl.app_name_txresult.v1` | `zclassic23 app names renew --input='{"name":"alice","confirm":true}'` | Renew a ZCL Name registration term |
| `app names set-record` | ready | mutate / app-write / **owner**, plan-commit · foreground/moderate | **`name`**, `type`, `value`, `confirm` | `zcl.app_name_txresult.v1` | `zclassic23 app names set-record --input='{"name":"alice","type":"btc","value":"bc1..","confirm":true}'` | Set a multi-coin address record |
| `app names set-text` | ready | mutate / app-write / **owner**, plan-commit · foreground/moderate | **`name`**, `key`, `value`, `confirm` | `zcl.app_name_txresult.v1` | `zclassic23 app names set-text --input='{"name":"alice","key":"url","value":"https://..","confirm":true}'` | Set a text record on a ZCL Name |

#### `app.tokens` — Tokens

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `app tokens list` | ready | read / read / public · fast/low | none | `zcl.app_token_index.v1` | `zclassic23 app tokens list` | List ZSLP tokens on the network |

#### `app.messaging` — Messaging

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `app messaging inbox` | ready | read / read / **owner** · fast/low | none | `zcl.app_message_index.v1` | `zclassic23 app messaging inbox` | List inbox messages |
| `app messaging send` | ready | mutate / app-write / **owner**, plan-commit · foreground/moderate | `message`, `channel`, `peer_id`, `to`, `from_address`, `reply_to`, `confirm` | `zcl.app_message_send_result.v1` | `zclassic23 app messaging send --input='{"channel":"p2p","peer_id":1,"message":"hi","confirm":true}'` | Send a message |
| `app messaging send-named` | planned | mutate / app-write / **owner**, plan-commit · foreground/moderate | **`name`**, `message`, `confirm` | `zcl.app_message_send_result.v1` | `zclassic23 app messaging send-named --input='{"name":"alice","message":"hi"}'` | Send a message to a ZCL Name — *needs an outbound delivery path before it can be exposed natively: rpc_msg_send_named (messaging_controller.c) resolves the name, calls zmsg_store_add + db_zmsg_save, and answers status=queued, but nothing in the tree ever drains that store onto a peer socket — no writer sends MSG_ZMSG for a stored message, so the queue has no consumer and the message is never delivered. Use app messaging send with an explicit peer_id, whose write to the peer socket is real* |
| `app messaging read` | ready | mutate / app-write / **owner** · fast/low | **`msg_id`** | `zcl.app_message_read_result.v1` | `zclassic23 app messaging read --input='{"msg_id":"<64hex>"}'` | Mark a message read |

#### `app.market` — Market

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `app market list` | ready | read / read / public · fast/low | none | `zcl.app_market_index.v1` | `zclassic23 app market list` | List files on the ZCL Market |
| `app market status` | ready | read / read / operator · fast/low | none | `zcl.app_market_status.v1` | `zclassic23 app market status` | ZCL Market status |
| `app market offer` | planned | mutate / app-write / **owner**, plan-commit · foreground/moderate | `filepath`, `price_per_mb_zat`, `confirm` | `zcl.app_market_offer_result.v1` | `zclassic23 app market offer --input='{"filepath":"/data/f","price_per_mb_zat":1000}'` | Announce a file for sale — *needs an origin-announce path before it can be exposed natively: rpc_zmarket_offer (file_market_controller.c) stats the file and calls file_market_add_offer + db_file_offer_save, then answers status=announced, but the only MSG_FILE_LIST writer in the tree is the re-gossip branch of handle_zfilelist (msgprocessor.c) — nothing ever announces a LOCALLY created offer, so no peer learns of it. Its root_hash is also SHA3(filepath:size), not a hash of the file contents* |
| `app market buy` | planned | mutate / wallet / **owner**, plan-commit · foreground/moderate | **`root_hash`**, `confirm` | `zcl.app_market_buy_result.v1` | `zclassic23 app market buy --input='{"root_hash":"<64hex>"}'` | Buy and download a market file — *needs the payment leg wired before a spend leaf can be exposed natively: rpc_zmarket_buy (file_market_controller.c) only calls file_market_start_download, which allocates an in-memory session in state FDL_CHALLENGING. No code path sends MSG_FILE_CHAL, and nothing builds or broadcasts the payment transaction whose mempool-verified txid handle_zfilepay (msgprocessor.c) requires to unlock chunks, so the session never advances and no funds move* |

#### `app.swap` — Swaps

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `app swap chains` | ready | read / read / operator · fast/low | none | `zcl.app_swap_chains.v1` | `zclassic23 app swap chains` | List supported atomic-swap chains |
| `app swap list` | ready | read / read / operator · fast/low | **`state`** | `zcl.app_swap_index.v1` | `zclassic23 app swap list --input='{"state":"pending"}'` | List atomic-swap contracts |
| `app swap initiate` | ready | mutate / wallet / **owner**, plan-commit · foreground/moderate | `my_address`, `counter_address`, `amount`, `locktime_blocks`, `chain`, `confirm` | `zcl.app_swap_contract.v1` | `zclassic23 app swap initiate --input='{"my_address":"t1..","counter_address":"t1..","amount":1,"locktime_blocks":20,"confirm":true}'` | Initiate an atomic swap |
| `app swap participate` | ready | mutate / wallet / **owner**, plan-commit · foreground/moderate | `my_address`, `counter_address`, `amount`, `locktime_blocks`, `secret_hash`, `chain`, `confirm` | `zcl.app_swap_contract.v1` | `zclassic23 app swap participate --input='{"my_address":"t1..","counter_address":"t1..","amount":1,"locktime_blocks":10,"secret_hash":"<64hex>","confirm":true}'` | Participate in an atomic swap |

#### `app.auth` — Public-key challenge/response login

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `app auth challenge` | ready | mutate / app-write / public · fast/low | `server`, **`address`** | `zcl.auth_challenge.v1` | `zclassic23 app auth challenge --input='{"address":"t1..."}'` | Issue a single-use login challenge to sign |
| `app auth verify` | ready | mutate / app-write / public · fast/low | `server`, `address`, `nonce`, `signature`, `pubkey` | `zcl.auth_session.v1` | `zclassic23 app auth verify --input='{"address":"t1...","nonce":"..","signature":".."}'` | Verify a signed challenge and mint a session |

#### `app.account` — Principal (multi-user) administration

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `app account list` | ready | read / read / operator · fast/low | none | `zcl.account_index.v1` | `zclassic23 app account list` | List principals (public projection) |
| `app account show` | ready | read / read / operator · fast/low | **`address`** | `zcl.account.v1` | `zclassic23 app account show t1...` | Show one principal by address |
| `app account whoami` | ready | read / read / public · fast/low | **`address`** | `zcl.account.v1` | `zclassic23 app account whoami t1...` | Resolve one address to its role/caps |
| `app account add` | ready | mutate / app-write / **owner** · fast/low | **`address`**, `pubkey`, `role`, `key_kind` | `zcl.account.v1` | `zclassic23 app account add --input='{"address":"t1...","pubkey":"..","role":"operator"}'` | Register or update a principal with a role |
| `app account role` | ready | mutate / app-write / **owner** · fast/low | **`address`**, `role` | `zcl.account.v1` | `zclassic23 app account role --input='{"address":"t1...","role":"owner"}'` | Set a principal's role |
| `app account suspend` | ready | mutate / app-write / **owner** · fast/low | **`address`** | `zcl.account.v1` | `zclassic23 app account suspend t1...` | Suspend a principal |
| `app account unsuspend` | ready | mutate / app-write / **owner** · fast/low | **`address`** | `zcl.account.v1` | `zclassic23 app account unsuspend t1...` | Reactivate a suspended principal |

### `dev` — Native edit, proof, and publication loop

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `dev status` | ready | read / read / operator · instant/low | none | `zcl.dev_cycle.v1` | `zclassic23 dev status` | Read the latest native cycle verdict |
| `dev ff` | ready | read / read / operator · instant/low | none | `zcl.dev_ff.v1` | `zclassic23 dev ff` | Fail-fast ladder: compile, test, lint |
| `dev verify-change` | compat 🔧 → `make dev-bin, then zclassic23-dev dev verify-change` | read / read / **owner** · background/high | none | `zcl.dev_verify_change.v1` | `zclassic23-dev dev verify-change` | Compile affected code and run mapped focused proofs with compact output — *changed-scope verification requires the dev-only process executor* |

#### `dev.core` — Core boundary and proof lanes

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `dev core boundary` | ready | read / read / public · instant/tiny | none | `zcl.core_app_boundary.v1` | `zclassic23 dev core boundary` | Show the enforced Core/App ownership law |
| `dev core proof` | planned | read / read / **owner** · background/high | `files` | `zcl.dev_core_proof.v1` | `zclassic23 dev core proof` | Run mandatory Core parity proof lanes — *native proof job extraction is not complete* |

#### `dev.app` — Build capability-scoped C Apps

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `dev app list` | ready | read / read / operator · fast/low | none | `zcl.dev_app_index.v1` | `zclassic23 dev app list` | List checkout App manifests |
| `dev app describe` | ready | read / read / operator · fast/low | **`app_id`** | `zcl.dev_app.v1` | `zclassic23 dev app describe social` | Describe an App manifest and its proofs |
| `dev app plan` | ready | read / read / operator · instant/tiny | **`app_id`**, **`resource`** | `zcl.dev_app_plan.v1` | `zclassic23 dev app plan social posts` | Plan one conventional App resource slice |
| `dev app scaffold` | planned | mutate / dev-mutation / **owner** · foreground/moderate | **`app_id`**, **`resource`** | `zcl.dev_app_scaffold.v1` | `zclassic23 dev app scaffold social posts` | Materialize a conventional App resource slice — *native bounded file materializer is not implemented* |
| `dev app simulate` | ready | read / read / operator · fast/moderate | **`app_id`**, `scenario`, `seed` | `zcl.dev_app_sim.v1` | `zclassic23 dev app simulate social --seed=0x534f4349414c0001` | Run deterministic App network scenarios |
| `dev app inspect` | planned | read / read / operator · fast/low | **`app_id`** | `zcl.dev_app_inspect.v1` | `zclassic23 dev app inspect social` | Inspect a resident App generation — *public App ABI is not connected to resident generations yet* |
| `dev app publish` | planned | mutate / dev-mutation / **owner**, job, idempotency · foreground/high | **`app_id`**, `idempotency_key` | `zcl.dev_app_publish.v1` | `zclassic23 dev app publish social --idempotency-key=<key>` | Atomically publish a proven App generation — *App ABI generation publication is not wired yet* |

#### `dev.change` — Classify and apply changes

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `dev change plan` | ready | read / read / operator · instant/tiny | `files` | `zcl.dev_plan.v1` | `zclassic23 dev change plan --input='{"files":["apps/social/app.def"]}'` | Classify files and select the smallest proof |
| `dev change apply` (aliases: `dev.change.cycle`) | compat 🔧 → `zclassic23-dev dev change cycle` | mutate / dev-mutation / **owner**, job · foreground/high | `files` | `zcl.dev_cycle.v1` | `zclassic23 dev change apply --input='{"files":["apps/social/app.def"]}'` | Contained publication entrypoint: returns RUNTIME_PUBLICATION_CONTAINED — *change application requires the dev-only process/activation executor* |

#### `dev.loop` — Persistent save-to-verdict loop

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `dev loop ensure` (aliases: `dev.loop.watch`) | compat 🔧 → `zclassic23-dev dev loop ensure --input='{"mode":"verify"}'` | mutate / dev-mutation / **owner** · fast/low | `root`, `mode` | `zcl.dev_loop_status.v1` | `zclassic23 dev loop ensure --input='{"root":".","mode":"verify"}'` | Ensure one verify-only watcher; publication modes are contained — *watcher ownership requires the dev-only executor* |
| `dev loop status` (aliases: `dev.loop.heartbeat`) | compat 🔧 → `zclassic23-dev dev loop heartbeat` | read / read / operator · instant/low | none | `zcl.dev_loop_status.v1` | `zclassic23 dev loop status` | Read watcher identity, epoch, and latest verdict — *watcher state is available through the dev binary* |
| `dev loop wait` | compat 🔧 → `zclassic23-dev dev loop wait` | read / read / operator · persistent/low | `after_epoch`, `timeout_ms` | `zcl.dev_cycle.v1` | `zclassic23 dev loop wait --input='{"after_epoch":41}'` | Wait for one verdict after a cycle epoch — *bounded verdict waiting is available through the dev binary* |
| `dev loop events` | planned | read / read / operator · persistent/stream | `after`, `heartbeat_ms` | `zcl.dev_loop_event.v1` | `zclassic23 dev loop events --format=jsonl` | Stream resumable source and cycle events — *resumable NDJSON event journal is not implemented* |
| `dev loop stop` | compat 🔧 → `zclassic23-dev dev loop stop` | mutate / dev-mutation / **owner** · fast/low | **`watcher_id`** | `zcl.dev_loop_status.v1` | `zclassic23 dev loop stop <watcher-id>` | Stop one identified native watcher — *watcher shutdown requires the dev-only executor* |

#### `dev.test` — Focused proof selection

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `dev test plan` | ready | read / read / operator · instant/tiny | `files` | `zcl.dev_test_plan.v1` | `zclassic23 dev test plan --input='{"files":[]}'` | Map changed files to mandatory proof groups |
| `dev test run` (aliases: `dev.test.focused`) | compat 🔧 → `zclassic23-dev dev test focused` | read / read / **owner** · background/high | **`group`** | `zcl.dev_focused_test.v1` | `zclassic23 dev test run hotswap_simnet` | Run one exact prebuilt focused test group — *focused tests require the dev-only process executor* |
| `dev test sim` | compat 🔧 → `zclassic23-dev dev test sim` | read / read / **owner** · fast/moderate | `app_id` | `zcl.dev_sim.v1` | `zclassic23 dev test sim` | Run the generic hot-swap network proof — *the simulation runner requires the dev-only process executor* |
| `dev test replay` | planned | read / read / **owner** · foreground/moderate | **`seed`**, `scenario` | `zcl.dev_test_replay.v1` | `zclassic23 dev test replay 1234` | Replay one deterministic failure seed — *generic seed replay registry is not implemented* |

#### `dev.generation` — Generation provenance

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `dev generation current` | compat 🔧 → `zclassic23-dev dev generation current` | read / read / operator · instant/low | none | `zcl.dev_generation_status.v1` | `zclassic23 dev generation current` | Show current and last-good generations — *generation state is available through the dev binary* |
| `dev generation history` | compat 🔧 → `zclassic23-dev dev generation history` | read / read / operator · fast/low | `cursor`, `max_items` | `zcl.dev_generation_history.v1` | `zclassic23 dev generation history` | Page accepted and rejected generations — *generation history is available through the dev binary* |
| `dev generation rollback` | planned | destructive / dev-mutation / **owner**, job, plan-commit · foreground/high | `intent_id`, `effect_digest` | `zcl.dev_generation_rollback.v1` | `zclassic23 dev generation rollback --input='<intent>'` | Restore verified last-good in the dev lane — *native activation engine is not implemented* |
| `dev generation compact` | planned | destructive / dev-mutation / **owner**, job, plan-commit · foreground/moderate | `intent_id`, `effect_digest` | `zcl.dev_generation_compact.v1` | `zclassic23 dev generation compact --input='<intent>'` | Compact unleased old generations — *native lease-aware compaction is not implemented* |

#### `dev.diagnose` — Failure capsule lookup

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `dev diagnose latest` | compat 🔧 → `zclassic23-dev dev diagnose latest` | read / read / operator · instant/low | none | `zcl.dev_failure_latest_result.v1` | `zclassic23 dev diagnose latest` | Read the latest compiler failure — *failure capsules are available through the dev binary* |
| `dev diagnose show` | compat 🔧 → `zclassic23-dev dev diagnose show <failure-id>` | read / read / operator · fast/low | **`failure_id`** | `zcl.dev_failure_show.v1` | `zclassic23-dev dev diagnose show <failure-id>` | Show one compiler failure record — *durable failure artifacts are available through the dev binary* |

#### `dev.vcs` — One-command source+binary revert

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `dev vcs revert` | compat 🔧 → `zclassic23-dev dev vcs revert <to> [--relink-generation]` | mutate / dev-mutation / **owner** · foreground/high | **`to`**, `relink_generation` | `zcl.dev_vcs_revert.v1` | `zclassic23 dev vcs revert --input='{"to":"<64-hex commit id>","relink_generation":true}'` | Restore the checkout to a prior ZVCS commit; generation relinking is currently contained — *one-command source+binary revert requires a dev build* |

#### `dev.vcs.seal` — Owner-run ZVCS unseal-token ritual

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `dev vcs seal grant` | compat 🔧 → `zclassic23-dev dev vcs seal grant --input='{"reason":"...","confirm":true}'` | mutate / dev-mutation / **owner** · fast/low | **`reason`**, `confirm` | `zcl.dev_vcs_seal_grant.v1` | `zclassic23 dev vcs seal grant --input='{"reason":"post-baseline app/jobs edits reviewed","confirm":true}'` | Mint a one-shot ZVCS unseal token authorizing the CURRENT worktree's sealed content for the next green-cycle anchor — *granting a ZVCS unseal token requires a dev build* |

#### `dev.hotswap` — Tier-1 command-leaf hot-swap

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `dev hotswap apply` | compat 🔧 → `zclassic23-dev dev hotswap apply --input='{"so_path":"...","probe_leaf":"..."}'` | mutate / dev-mutation / **owner** · fast/moderate | **`so_path`**, `probe_leaf` | `zcl.dev_hotswap.v1` | `zclassic23-dev dev hotswap apply --input='{"so_path":"/tmp/gen.so","probe_leaf":"core.status"}'` | Live module hot-swap: forwards to the resident dev node; verify-only unless -hotswap-activate + ZCL_HOTSWAP_ACTIVATE=1 on the dev datadir (canonical refused) — *in-process hot-swap requires a dev build* |
| `dev hotswap probe` | compat 🔧 → `zclassic23-dev dev hotswap probe --input='{"so_path":"...","probe_leaf":"..."}'` | read / read / **owner** · fast/low | **`so_path`**, `probe_leaf` | `zcl.dev_hotswap.v1` | `zclassic23-dev dev hotswap probe --input='{"so_path":"/tmp/gen.so","probe_leaf":"core.status"}'` | Verify-only in-process: dlopen + ABI-validate + self_test of a module .so; never commits — *resident hot-swap probing requires a dev build* |

#### `dev.test.background` — Background proof freshness

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `dev test background status` | planned | read / read / operator · instant/low | none | `zcl.dev_background_quality.v1` | `zclassic23 dev test background status` | Read lint, sanitizer, replay, and reproducibility freshness — *native background-quality projection is not implemented* |

### `ops` — Node diagnostics

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `ops health` | ready | read / read / public · fast/low | none | `zcl.health.v1` | `zclassic23 ops health` | Health rollup |
| `ops diagnose` | ready | read / read / operator · fast/moderate | none | `zcl.ops_diagnose.v1` | `zclassic23 ops diagnose` | Operator diagnosis |
| `ops lanes` | ready | read / read / operator · fast/low | none | `zcl.ops_lanes.v1` | `zclassic23 ops lanes` | Lanes |
| `ops logs` | ready | read / read / operator · fast/low | **`pattern`**, `since_secs`, `max_lines`, `level` | `zcl.ops_logs.v1` | `zclassic23 ops logs --pattern='blocker'` | Log regex tail |
| `ops timeline` | ready | read / read / operator · fast/low | none | `zcl.ops_timeline.v1` | `zclassic23 ops timeline` | Events |
| `ops metrics` | ready | read / read / operator · fast/low | none | `zcl.ops_metrics.v1` | `zclassic23 ops metrics` | Metrics |
| `ops state` | ready | read / read / operator · fast/low | **`subsystem`**, `key`, `explain` | `zcl.ops_state.v1` | `zclassic23 ops state --subsystem=reducer_frontier` | Subsystem state |
| `ops selftest` | ready | read / read / operator · fast/low | none | `zcl.ops_selftest.v1` | `zclassic23 ops selftest` | Self-test |

#### `ops.jobs` — Job lifecycle

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `ops jobs list` | planned | read / read / operator · fast/low | none | `zcl.jobs.v1` | `zclassic23 ops jobs list` | List asynchronous jobs — *the native job registry is a Wave 2.2 deliverable* |

#### `ops.debug` — Diagnostics

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `ops debug explain` (aliases: `explain`, `ops.explain`) | ready | read / read / operator · fast/low | **`topic`** | `zcl.ops_explain.v1` | `zclassic23 explain sync` | Explain a subsystem in plain prose |
| `ops debug meaning` (aliases: `meaning`, `ops.meaning`) | ready | read / read / operator · fast/low | `subsystem`, `name`, `question` | `zcl.ops_meaning.v1` | `zclassic23 meaning --name=pre_handshake_disconnects` | What a telemetry field means, and which report answers a question |
| `ops debug profile` (aliases: `profile`, `ops.profile`) | ready | read / read / operator · foreground/moderate | **`seconds`**, `top_n` | `zcl.ops_profile.v1` | `zclassic23 profile 3` | Sample thread CPU + stage rates |
| `ops debug producer` (aliases: `ops.producer.status`) | ready | read / read / operator · fast/low | **`datadir`** | `zcl.ops_producer_status.v1` | `zclassic23 ops producer status -datadir=/home/you/.zclassic-c23-mint` | Read a producer datadir's fold progress + receipt |
| `ops debug rom` (aliases: `ops.rom`) | ready | read / read / operator · fast/low | none | `zcl.rom_compile.v1` | `zclassic23 ops rom` | ROM compilation fold progress |
| `ops debug backtrace` | ready | read / read / operator · fast/low | none | `zcl.ops_debug_backtrace.v1` | `zclassic23 ops debug backtrace` | Dump every thread's backtrace |
| `ops debug bundle` | ready | read / read / operator · fast/low | none | `zcl.ops_debug_bundle.v1` | `zclassic23 ops debug bundle` | Write one-shot debug bundle JSON |

#### `ops.debug.dash` — Operator rollup dashboards

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `ops debug dash kpi` (aliases: `kpi`, `ops.kpi`) | ready | read / read / operator · fast/moderate | none | `zcl.ops_kpi.v1` | `zclassic23 kpi` | One-shot subsystem KPI dashboard |
| `ops debug dash snapshot` (aliases: `ops.snapshot`) | ready | read / read / operator · fast/low | none | `zcl.operator_snapshot.v3` | `zclassic23 ops snapshot` | Operator snapshot payload |
| `ops debug dash summary` (aliases: `ops.summary`) | ready | read / read / operator · fast/low | none | `zcl.operator_summary.v1` | `zclassic23 ops summary` | Fail-closed operator summary |
| `ops debug dash milestone` (aliases: `milestone`, `ops.milestone`) | ready | read / read / operator · fast/low | none | `zcl.milestone.v1` | `zclassic23 milestone` | Version milestone progress |
| `ops debug dash mirror` (aliases: `ops.mirror`) | ready | read / read / operator · fast/low | none | `zcl.mirror_status.v2` | `zclassic23 ops mirror` | zclassicd mirror lockstep |
| `ops debug dash selfheal` (aliases: `selfheal`, `ops.selfheal`) | ready | read / read / operator · fast/low | none | `zcl.self_heal_stats.v1` | `zclassic23 ops selfheal` | Self-heal recovery counters |

#### `ops.debug.rom_seed` — ROM-seed policy

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `ops debug rom_seed status` | ready | read / read / public · fast/low | none | `zcl.rom_seed_status.v1` | `zclassic23 ops debug rom_seed status` | ROM-seed policy + live counters |
| `ops debug rom_seed enable` | ready | mutate / app-write / **owner** · fast/low | none | `zcl.rom_seed_status.v1` | `zclassic23 ops debug rom_seed enable` | Enable ROM-seed serving |
| `ops debug rom_seed disable` | ready | mutate / app-write / **owner** · fast/low | none | `zcl.rom_seed_status.v1` | `zclassic23 ops debug rom_seed disable` | Disable ROM-seed serving |
| `ops debug rom_seed artifacts` | ready | read / read / public · fast/low | none | `zcl.rom_seed_artifacts.v1` | `zclassic23 ops debug rom_seed artifacts` | List served ROM artifacts + seed stats |
| `ops debug rom_seed publish` | ready | mutate / app-write / **owner** · fast/low | none | `zcl.rom_seed_publish.v1` | `zclassic23 ops debug rom_seed publish` | Publish this node's starter artifacts to the swarm |

#### `ops.debug.rom_fetch` — ROM-fetch engine

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `ops debug rom_fetch status` | ready | read / read / public · fast/low | none | `zcl.rom_fetch_status.v1` | `zclassic23 ops debug rom_fetch status` | ROM-fetch engine status |
| `ops debug rom_fetch bundle` | ready | mutate / app-write / **owner** · background/moderate | `peer`, `port`, `root`, `whole_sha3`, `size`, `filename`, `out_dir` | `zcl.rom_fetch_bundle.result.v1` | `zclassic23 ops debug rom_fetch bundle --input='{"peer":"203.0.113.7","root":"<64hex>","whole_sha3":"<64hex>","size":"538507264"}'` | Fetch + verify a ROM artifact from a peer |

#### `ops.postmortem` — Postmortems

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `ops postmortem list` | ready | read / read / operator · fast/low | none | `zcl.postmortem_list.v1` | `zclassic23 ops postmortem list` | List captured postmortems |
| `ops postmortem replay` | planned | read / read / operator · foreground/moderate | **`id`** | `zcl.postmortem_replay.v1` | `zclassic23 ops postmortem replay <id>` | Replay one captured postmortem — *postmortem replay argument mapping is a Wave 2.2 deliverable* |

#### `ops.config` — Runtime config

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `ops config show` | planned | read / read / operator · fast/low | none | `zcl.ops_config.v1` | `zclassic23 ops config show` | Show effective runtime configuration — *an effective-config reader is a Wave 2.2 deliverable* |
| `ops config reload` | planned | mutate / core-recovery / operator · fast/low | none | `zcl.ops_config_reload.v1` | `zclassic23 ops config reload` | Reload runtime configuration — *config-reload binding is a Wave 2.2 deliverable* |

#### `ops.recovery` — Recovery ops

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `ops recovery status` | ready | read / read / operator · fast/low | none | `zcl.recovery_status.v1` | `zclassic23 ops recovery status` | Refold and recovery progress |
| `ops recovery rebuild` | planned | destructive / core-recovery / **owner**, job, plan-commit · background/high | `depth` | `zcl.recovery_rebuild.v1` | `zclassic23 ops recovery rebuild --depth=100` | Rebuild recent chain state — *recovery rebuild plan/commit handshake is a Wave 2.2 deliverable* |

### `discover` — Search and describe the command registry

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `discover help` (aliases: `help`, `dev.help`) | ready | read / read / public · instant/tiny | **`path`** | `zcl.discovery_document.v1` | `zclassic23 discover help dev.app` | Show one branch or leaf without loading the whole tree |
| `discover search` (aliases: `search`, `dev.search`, `dev.diagnose.search`) | ready | read / read / public · instant/tiny | **`query`** | `zcl.command_search.v1` | `zclassic23 discover search 'ABI mismatch'` | Rank at most five commands by local deterministic intent search |
| `discover describe` | ready | read / read / public · instant/tiny | **`path`** | `zcl.command_spec.v1` | `zclassic23 discover describe dev.app.simulate` | Describe one leaf's schemas, safety, authority, and availability |
| `discover schema` | ready | read / read / public · instant/tiny | **`path`**, `side` | `zcl.command_schema.v1` | `zclassic23 discover schema dev.app.simulate --side=input` | Return the compact input or output schema contract for one leaf |

### `code` — Hierarchical source-code navigator

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `code group` | ready | read / read / public · fast/tiny | **`group`** | `zcl.code_group.v1` | `zclassic23 code group app/services` | Top source groups, or one group's subgroups and files |
| `code map` | ready | read / read / public · fast/tiny | none | `zcl.code_map.v1` | `zclassic23 code map` | Map the tree: root groups and app shapes with file counts |
| `code tests` | ready | read / read / public · fast/tiny | **`path`** | `zcl.code_tests.v1` | `zclassic23 code tests lib/net/src/download.c` | Which focused test group a change to one file routes to |
| `code room` | ready | read / read / public · fast/tiny | **`path`** | `zcl.code_room.v1` | `zclassic23 code room app/jobs/src/utxo_apply_stage.c` | Compose shape, purpose, neighbors and test route for one path |
| `code file` | ready | read / read / public · fast/tiny | **`path`** | `zcl.code_file.v1` | `zclassic23 code file lib/vcs/src/vcs_index.c` | One file's symbol table and in-tree include dependencies |
| `code sym` | ready | read / read / public · fast/tiny | **`name`** | `zcl.code_symbol.v1` | `zclassic23 code sym codeindex_open` | One symbol's card: kind, signature, def/decl, doc, guard |
| `code capsule` | ready | read / read / public · fast/tiny | **`name`** | `zcl.code_capsule.v1` | `zclassic23 code capsule sovereignty_guard_allow` | Compose one symbol's identity, call graph, includes and commands |
| `code change-plan` | ready | read / read / public · fast/tiny | **`name`**, `symbol`, `intent`, `patch` | `zcl.code_change_plan.v1` | `zclassic23 code change-plan codeindex_open` | Turn a symbol, intent, or patch into an edit and proof plan |
| `code refs` | ready | read / read / public · fast/tiny | **`name`**, `limit` | `zcl.code_refs.v1` | `zclassic23 code refs zcl_malloc` | List call sites and references to one symbol |
| `code impact` | ready | read / read / public · fast/tiny | **`path`** | `zcl.code_impact.v1` | `zclassic23 code impact lib/util/include/util/safe_alloc.h` | The reverse-dependency blast radius of one changed file |
| `code find` | ready | read / read / public · fast/tiny | **`text`**, `limit` | `zcl.code_find.v1` | `zclassic23 code find hotswap` | Rank N symbols by name, with a one-line context per hit |

#### `code.provenance` — Attribute output back to the code that produced it

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `code provenance merkle` | ready | read / read / public · fast/tiny | **`path`** | `zcl.code_merkle.v1` | `zclassic23 code provenance merkle lib/net` | Give the SHA3 Merkle root of the tree or a subtree |
| `code provenance facts` | ready | read / read / public · background/moderate | **`key`**, `store` | `zcl.code_facts.v1` | `zclassic23 code provenance facts coins_applied_height` | Census the durable named slots and name the ones with several writers |
| `code provenance emitter` | ready | read / read / public · foreground/moderate | **`text`** | `zcl.code_emitter.v1` | `zclassic23 code provenance emitter 'address_index.below_snapshot_seed'` | Resolve emitted text to its emitting code |

### `vault` — What this node owns, and what may act on it

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `vault list` | ready | read / read / operator · fast/low | **`class`** | `zcl.vault_list.v1` | `zclassic23 vault list` | One row per asset class: everything this node owns |
| `vault show` | ready | read / read / operator · fast/low | **`class`**, `limit` | `zcl.vault_show.v1` | `zclassic23 vault show transparent` | Itemize the holdings inside one asset class |
| `vault encumbered` | ready | read / read / operator · fast/low | **`class`**, `limit` | `zcl.vault_encumbered.v1` | `zclassic23 vault encumbered` | What is owned but not free to move, and what would release it |
| `vault routes` | ready | read / read / public · instant/tiny | **`class`** | `zcl.vault_routes.v1` | `zclassic23 vault routes` | Which existing path owns the spend for each asset class |
| `vault send` | ready | mutate / wallet / **owner**, plan-commit · foreground/moderate | `address`, `amount`, `idempotency_key`, `confirm` | `zcl.wallet_send.v1` | `zclassic23 vault send --input='{"address":"t1..","amount":1.5}'` | Spend transparent ZCL by dispatching to the wallet's own send |
| `vault send-shielded` | ready | mutate / wallet / **owner**, job, plan-commit · background/high | `from`, `to`, `amount`, `idempotency_key`, `confirm` | `zcl.shielded_send.v1` | `zclassic23 vault send-shielded --input='{"from":"zs1..","to":"zs1..","amount":1}'` | Spend shielded ZCL by dispatching to the wallet's own shielded send |

#### `vault.session` — Scoped, revocable spend-authority grants for agents

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `vault session create` | ready | mutate / wallet / **owner**, plan-commit · foreground/moderate | `account`, `max_per_tx`, `max_per_window`, `window_seconds`, `allowlist`, `expires_in`, `confirm` | `zcl.vault_session_create.v1` | `zclassic23 vault session create --input='{"account":"t1..","max_per_tx":"1.5","max_per_window":"10","window_seconds":"86400"}'` | Mint a scoped agent spend session; returns the token once |
| `vault session list` | ready | read / read / operator · fast/low | `account` | `zcl.vault_session_list.v1` | `zclassic23 vault session list --input='{"account":"t1.."}'` | List agent spend sessions; the token is always redacted |
| `vault session revoke` | ready | mutate / wallet / **owner**, plan-commit · foreground/moderate | `session_id`, `confirm` | `zcl.vault_session_revoke.v1` | `zclassic23 vault session revoke --input='{"session_id":"<32hex>","confirm":true}'` | Revoke an agent spend session by its full token |

#### `vault.swap` — Release funds locked in an atomic-swap contract

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `vault swap redeem` | ready | mutate / wallet / **owner**, plan-commit · foreground/moderate | `swap_id`, `secret`, `funding_txid`, `vout`, `confirm` | `zcl.vault_swap_settle.v1` | `zclassic23 vault swap redeem --input='{"swap_id":"..","secret":"<64hex>"}'` | Claim a funded swap HTLC by dispatching the node's swap_redeem |
| `vault swap refund` | ready | mutate / wallet / **owner**, plan-commit · foreground/moderate | `swap_id`, `funding_txid`, `vout`, `confirm` | `zcl.vault_swap_settle.v1` | `zclassic23 vault swap refund --input='{"swap_id":".."}'` | Reclaim an expired swap HTLC by dispatching the node's swap_refund |

### `zcode` — ZCODE source-package hosting: publish, search, host

#### `zcode.package` — Published packages: releases, manifests, chunks

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `zcode package search` | ready | read / read / operator · fast/low | `publisher`, `name_prefix`, `license`, `keyword`, `limit`, `datadir` | `zcl.zcode_package_search.v1` | `zclassic23 zcode package search --input='{"keyword":"ring"}'` | Search locally published packages |
| `zcode package show` | ready | read / read / operator · fast/low | **`root`**, `datadir` | `zcl.zcode_package_show.v1` | `zclassic23 zcode package show --input='{"root":"<64hex>"}'` | Full release record and manifest summary for one package root |
| `zcode package recipe` | ready | read / read / operator · fast/low | **`root`**, `datadir` | `zcl.zcode_package_recipe.v1` | `zclassic23 zcode package recipe --input='{"root":"<64hex>"}'` | Decoded declarative build recipe for one package root |
| `zcode package verify` | ready | read / read / operator · fast/low | **`root`**, `datadir` | `zcl.zcode_package_verify.v1` | `zclassic23 zcode package verify --input='{"root":"<64hex>"}'` | External-verifier attestation quorum for one package root |
| `zcode package resolve` | ready | read / read / operator · fast/low | **`name`**, `datadir` | `zcl.zcode_package_resolve.v1` | `zclassic23 zcode package resolve --input='{"name":"ringbuffer"}'` | Resolve a ZNAM package name to its release |
| `zcode package fetch` | ready | mutate / app-write / operator · foreground/moderate | **`root`**, `day`, `datadir` | `zcl.zcode_package_fetch.v1` | `zclassic23 zcode package fetch --input='{"root":"<64hex>"}'` | Fetch a package from the authenticated swarm (resumable) |
| `zcode package peers` | ready | read / read / operator · fast/low | **`root`**, `datadir` | `zcl.zcode_package_peers.v1` | `zclassic23 zcode package peers --input='{"root":"<64hex>"}'` | Live swarm peers advertising one package root |
| `zcode package pin` | ready | mutate / app-write / operator · foreground/moderate | **`root`**, `datadir` | `zcl.zcode_package_pin.v1` | `zclassic23 zcode package pin --input='{"root":"<64hex>"}'` | Operator-pin a tracked package (PINS pool, never evicted) |
| `zcode package unpin` | ready | mutate / app-write / operator · foreground/moderate | **`root`**, `datadir` | `zcl.zcode_package_unpin.v1` | `zclassic23 zcode package unpin --input='{"root":"<64hex>"}'` | Release an operator pin |

#### `zcode.package.publish` — Publish a signed release into the local store (plan, then commit)

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `zcode package publish plan` | ready | read / read / operator · foreground/moderate | `release_hex`, `manifest_hex`, `dir`, `datadir` | `zcl.zcode_publish_plan.v1` | `zclassic23 zcode package publish plan --input='{"release_hex":"..","manifest_hex":"..","dir":"/tmp/pkg"}'` | Validate a candidate release without persisting anything |
| `zcode package publish commit` | ready | mutate / app-write / operator · foreground/moderate | `release_hex`, `manifest_hex`, `dir`, `day`, `datadir` | `zcl.zcode_publish_commit.v1` | `zclassic23 zcode package publish commit --input='{"release_hex":"..","manifest_hex":"..","dir":"/tmp/pkg"}'` | Re-validate and persist a candidate release into the local store |

#### `zcode.contributor` — Contributors: secp256k1 publisher-key identities

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `zcode contributor show` | ready | read / read / operator · fast/low | **`pubkey`**, `day`, `datadir` | `zcl.zcode_contributor_show.v1` | `zclassic23 zcode contributor show --input='{"pubkey":"<66hex>"}'` | Contributor profile for one publisher pubkey |
| `zcode contributor packages` | ready | read / read / operator · fast/low | **`pubkey`**, `datadir` | `zcl.zcode_contributor_packages.v1` | `zclassic23 zcode contributor packages --input='{"pubkey":"<66hex>"}'` | Published releases of one contributor key |
| `zcode contributor badges` | ready | read / read / operator · fast/low | **`pubkey`**, `limit`, `offset`, `datadir` | `zcl.zcode_contributor_badges.v1` | `zclassic23 zcode contributor badges --input='{"pubkey":"<66hex>"}'` | Earned ZCODE Badges of one contributor (permanent evidence) |

#### `zcode.reward` — Contribution scoring and SIMULATED settlement (placeholder token only)

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `zcode reward score` | ready | read / read / operator · foreground/moderate | **`root`**, `datadir` | `zcl.zcode_reward_score.v1` | `zclassic23 zcode reward score --input='{"root":"<64hex>"}'` | Deterministic contribution score breakdown for one release root |
| `zcode reward eligible` | ready | read / read / operator · foreground/moderate | **`root`**, `datadir` | `zcl.zcode_reward_eligible.v1` | `zclassic23 zcode reward eligible --input='{"root":"<64hex>"}'` | Reward eligibility gate list for one release root |
| `zcode reward queue` | ready | read / read / operator · fast/low | `state`, `limit`, `offset`, `datadir` | `zcl.zcode_reward_queue.v1` | `zclassic23 zcode reward queue --input='{"state":"queued"}'` | Inspect the daily reward settlement queue |
| `zcode reward plan` | ready | mutate / app-write / operator · foreground/moderate | **`day`**, `datadir` | `zcl.zcode_reward_plan.v1` | `zclassic23 zcode reward plan --input='{"day":20500}'` | Assemble one settlement window batch (SIMULATED) |
| `zcode reward commit` | ready | mutate / app-write / operator · foreground/moderate | **`plan_id`**, `datadir` | `zcl.zcode_reward_commit.v1` | `zclassic23 zcode reward commit --input='{"plan_id":"<64hex>"}'` | Settle a planned batch (SIMULATED, idempotent) |
| `zcode reward receipt` | ready | read / read / operator · fast/low | **`plan_id`**, `datadir` | `zcl.zcode_reward_receipt.v1` | `zclassic23 zcode reward receipt --input='{"plan_id":"<64hex>"}'` | Durable receipt for a settled batch (SIMULATED) |

#### `zcode.leaderboard` — ZCODE Rankings: earned score, never token balances

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `zcode leaderboard daily` | ready | read / read / operator · fast/low | `category`, `day`, `limit`, `offset`, `breakdown`, `datadir` | `zcl.zcode_leaderboard.v1` | `zclassic23 zcode leaderboard daily --input='{"day":20500,"category":"security-fixes"}'` | Daily ZCODE Ranking (earned score, never balances) |
| `zcode leaderboard weekly` | ready | read / read / operator · fast/low | `category`, `day`, `limit`, `offset`, `breakdown`, `datadir` | `zcl.zcode_leaderboard.v1` | `zclassic23 zcode leaderboard weekly --input='{"day":20500}'` | Weekly ZCODE Ranking (ISO-8601 week, earned score) |
| `zcode leaderboard monthly` | ready | read / read / operator · fast/low | `category`, `day`, `limit`, `offset`, `breakdown`, `datadir` | `zcl.zcode_leaderboard.v1` | `zclassic23 zcode leaderboard monthly --input='{"day":20500}'` | Monthly ZCODE Ranking (calendar month, earned score) |
| `zcode leaderboard all` | ready | read / read / operator · fast/low | `category`, `limit`, `offset`, `breakdown`, `datadir` | `zcl.zcode_leaderboard.v1` | `zclassic23 zcode leaderboard all --input='{}'` | All-time ZCODE Ranking (earned score) |

#### `zcode.badge` — ZCODE Badges: achievement evidence (SIMULATED assets)

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `zcode badge eligible` | ready | read / read / operator · fast/low | **`pubkey`**, `day`, `datadir` | `zcl.zcode_badge_eligible.v1` | `zclassic23 zcode badge eligible --input='{"pubkey":"<66hex>","day":20500}'` | Which ZCODE Badges a contributor qualifies for right now |
| `zcode badge plan` | ready | mutate / app-write / operator · foreground/moderate | **`pubkey`**, `day`, `datadir` | `zcl.zcode_badge_plan.v1` | `zclassic23 zcode badge plan --input='{"pubkey":"<66hex>","day":20500}'` | Assemble one dedup-checked badge issuance batch (SIMULATED) |
| `zcode badge issue` | ready | mutate / app-write / operator · foreground/moderate | **`plan_id`**, **`issuer_secret`**, `datadir` | `zcl.zcode_badge_issue.v1` | `zclassic23 zcode badge issue --input='{"plan_id":"<64hex>","issuer_secret":"<64hex>"}'` | Issue a planned badge batch (SIMULATED, idempotent) |

#### `zcode.seed` — Local seeding facts: verified-bytes ratio and tiers

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `zcode seed status` | ready | read / read / operator · fast/low | `pubkey`, `day`, `datadir` | `zcl.zcode_seed_status.v1` | `zclassic23 zcode seed status --input='{"pubkey":"<66hex>"}'` | Local serving facts, tier, and allowances per contributor key |
| `zcode seed ratio` | ready | read / read / operator · fast/low | `pubkey`, `datadir` | `zcl.zcode_seed_ratio.v1` | `zclassic23 zcode seed ratio --input='{"pubkey":"<66hex>"}'` | The local verified-bytes ratio and exactly how it is computed |

#### `zcode.storage` — The content-addressed store: quota pools and policy

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `zcode storage status` | ready | read / read / operator · fast/low | `datadir` | `zcl.zcode_storage_status.v1` | `zclassic23 zcode storage status --input='{}'` | Store quota pools plus the pin-allowance policy view |

#### `zcode.release` — Release records: sign/verify against a master key

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `zcode release sign` | ready | mutate / app-write / operator · fast/low | `name`, `version`, `root`, `seed_file`, `seq`, `expiry`, `datadir` | `zcl.zcode_release_sign.v1` | `zclassic23 zcode release sign --input='{"name":"demo","version":"0.1","root":"<64hex>","seed_file":"/path/seed.hex"}'` | Sign a release record with a master seed |
| `zcode release verify` | ready | read / read / public · fast/low | `doc`, `file`, `proof`, `root`, `anchored`, `datadir` | `zcl.zcode_release_verify.v1` | `zclassic23 zcode release verify --input='{"doc":"<hex>"}'` | Verify a signed release record (optionally its batch inclusion) |
| `zcode release anchor` | ready | mutate / wallet / operator · foreground/moderate | `tip`, `domain`, `datadir` | `zcl.zcode_release_anchor.v1` | `zclassic23 zcode release anchor --input='{}'` | Anchor the release batch's domain root on-chain |
| `zcode release prove` | ready | read / read / operator · fast/low | **`name`**, **`version`**, `domain`, `datadir` | `zcl.zcode_release_prove.v1` | `zclassic23 zcode release prove --input='{"name":"demo","version":"0.1"}'` | Emit the domain-batch inclusion proof for one release |

#### `zcode.domain` — Anchor domains: leaf sets behind batch proofs

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `zcode domain list` | ready | read / read / operator · fast/low | `datadir` | `zcl.zcode_domain_list.v1` | `zclassic23 zcode domain list --input='{}'` | List the anchor domains stored in this datadir |
| `zcode domain status` | ready | read / read / operator · fast/low | `domain`, `datadir` | `zcl.zcode_domain_status.v1` | `zclassic23 zcode domain status --input='{"domain":"zcode"}'` | Show one anchor domain's stored root, leaves, and anchor |

#### `zcode.proof` — Light-client proof-chain verification

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `zcode proof walk` | ready | read / read / public · foreground/low | `doc`, `doc_file`, `proof`, `root`, `tx`, `header`, `headers`, `merkle_branch`, `merkle_index`, `now` | `zcl.zcode_proof_walk.v1` | `zclassic23 zcode proof walk --input='{"doc":"<hex>","proof":"<hex>","root":"<64hex>","tx":"<hex>","header":"<hex>","merkle_index":1,"merkle_branch":"<64hex>"}'` | Walk a record's proof chain down to proof-of-work, rung by rung |

#### `zcode.desc` — Onion descriptors: signed service records

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `zcode desc publish` | ready | mutate / app-write / operator · fast/low | **`onion`**, `intros`, `seed_file`, `seq`, `not_before`, `expiry`, `now`, `datadir` | `zcl.zcode_desc_publish.v1` | `zclassic23 zcode desc publish --input='{"onion":"<56base32>.onion","seed_file":"/path/seed.hex","intros":"<56base32>.onion:<64hex>","seq":"1"}'` | Publish a signed onion-service descriptor |
| `zcode desc verify` | ready | read / read / public · fast/low | `doc`, `file`, **`pubkey`**, `now` | `zcl.zcode_desc_verify.v1` | `zclassic23 zcode desc verify --input='{"doc":"<hex>","pubkey":"<64hex>"}'` | Check a descriptor's signature against a master key you supply |
| `zcode desc resolve` | ready | read / read / public · fast/low | **`pubkey`**, `now`, `datadir` | `zcl.zcode_desc_resolve.v1` | `zclassic23 zcode desc resolve --input='{"pubkey":"<64hex>"}'` | Look up an identity's current descriptor by its blinded record key |

#### `zcode.endpoint` — Signed node addresses, chain-anchored

| Command | Avail | Policy | Input keys (**required**) | Output schema | Example | Summary |
|---|---|---|---|---|---|---|
| `zcode endpoint publish` | ready | mutate / app-write / operator · fast/low | `onion`, `onion_port`, `ipv4`, `ipv4_port`, `ipv6`, `ipv6_port`, `services`, `height`, **`seed_file`**, `seq`, `not_before`, `expiry`, `now`, `datadir` | `zcl.zcode_endpoint_publish.v1` | `zclassic23 zcode endpoint publish --input='{"onion":"<56base32>.onion","onion_port":"8033","seed_file":"/path/seed.hex","seq":"1","height":3196556}'` | Publish this node's signed endpoint record |
| `zcode endpoint accept` | ready | mutate / app-write / operator · fast/low | **`doc`**, `file`, `now`, `datadir` | `zcl.zcode_endpoint_accept.v1` | `zclassic23 zcode endpoint accept --input='{"doc":"<hex>"}'` | Verify a peer's endpoint record against the chain and file it |
| `zcode endpoint verify` | ready | read / read / public · fast/low | **`doc`**, `file`, `now`, `datadir` | `zcl.zcode_endpoint_verify.v1` | `zclassic23 zcode endpoint verify --input='{"doc":"<hex>"}'` | Check an endpoint record against the chain without storing it |
| `zcode endpoint resolve` | ready | read / read / public · fast/low | **`pubkey`**, `now`, `datadir` | `zcl.zcode_endpoint_resolve.v1` | `zclassic23 zcode endpoint resolve --input='{"pubkey":"<64hex>"}'` | Look up a filed endpoint record by its blinded record key |
| `zcode endpoint list` | ready | read / read / public · fast/low | `now`, `datadir` | `zcl.zcode_endpoint_list.v1` | `zclassic23 zcode endpoint list` | Show every filed endpoint record and whether the node will use it |


## Aliases

Every alias resolves through the same grammar as its canonical path
(`test_native_api_contract.c::test_root_and_discover_aliases_resolve`).

| Alias | Resolves to |
|---|---|
| `help` | `discover.help` |
| `dev.help` | `discover.help` |
| `search` | `discover.search` |
| `dev.search` | `discover.search` |
| `dev.diagnose.search` | `discover.search` |
| `appprotocols` | `app.protocols` |
| `explain` | `ops.debug.explain` |
| `ops.explain` | `ops.debug.explain` |
| `meaning` | `ops.debug.meaning` |
| `ops.meaning` | `ops.debug.meaning` |
| `profile` | `ops.debug.profile` |
| `ops.profile` | `ops.debug.profile` |
| `ops.producer.status` | `ops.debug.producer` |
| `ops.rom` | `ops.debug.rom` |
| `kpi` | `ops.debug.dash.kpi` |
| `ops.kpi` | `ops.debug.dash.kpi` |
| `ops.snapshot` | `ops.debug.dash.snapshot` |
| `ops.summary` | `ops.debug.dash.summary` |
| `milestone` | `ops.debug.dash.milestone` |
| `ops.milestone` | `ops.debug.dash.milestone` |
| `ops.mirror` | `ops.debug.dash.mirror` |
| `selfheal` | `ops.debug.dash.selfheal` |
| `ops.selfheal` | `ops.debug.dash.selfheal` |
| `dev.change.cycle` | `dev.change.apply` |
| `dev.loop.watch` | `dev.loop.ensure` |
| `dev.loop.heartbeat` | `dev.loop.status` |
| `dev.test.focused` | `dev.test.run` |


## Shared output schemas

Output schema ids carried by more than one leaf — the places where two commands
promise the same document shape.

| Output schema | Leaves |
|---|---|
| `zcl.core_status_brief.v1` | `status`, `core.status.brief` |
| `zcl.wait_result.v1` | `core.chain.wait.height`, `core.chain.wait.blocker`, `core.chain.wait.halt` |
| `zcl.block_mutation.v1` | `core.consensus.block.invalidate`, `core.consensus.block.reconsider` |
| `zcl.wallet_address.v1` | `core.wallet.address.new`, `core.wallet.address.import` |
| `zcl.wallet_send.v1` | `core.wallet.transaction.send`, `vault.send` |
| `zcl.shielded_send.v1` | `core.wallet.shielded.send`, `vault.send-shielded` |
| `zcl.storage_query.v1` | `core.storage.query`, `core.storage.query.offline` |
| `zcl.core_bootstatus.v1` | `core.node.bootstatus`, `core.node.bootwait` |
| `zcl.core_identity_anchor.v1` | `core.identity.anchor`, `core.identity.rotate`, `core.identity.revoke` |
| `zcl.core_zdir_register.v1` | `core.zdir.register`, `core.zdir.deregister` |
| `zcl.app_name_txresult.v1` | `app.names.register`, `app.names.update`, `app.names.transfer`, `app.names.renew`, `app.names.set-record`, `app.names.set-text` |
| `zcl.app_message_send_result.v1` | `app.messaging.send`, `app.messaging.send-named` |
| `zcl.app_swap_contract.v1` | `app.swap.initiate`, `app.swap.participate` |
| `zcl.rom_seed_status.v1` | `ops.debug.rom_seed.status`, `ops.debug.rom_seed.enable`, `ops.debug.rom_seed.disable` |
| `zcl.dev_cycle.v1` | `dev.status`, `dev.change.apply`, `dev.loop.wait` |
| `zcl.dev_hotswap.v1` | `dev.hotswap.apply`, `dev.hotswap.probe` |
| `zcl.dev_loop_status.v1` | `dev.loop.ensure`, `dev.loop.status`, `dev.loop.stop` |
| `zcl.account.v1` | `app.account.show`, `app.account.whoami`, `app.account.add`, `app.account.role`, `app.account.suspend`, `app.account.unsuspend` |
| `zcl.vault_swap_settle.v1` | `vault.swap.redeem`, `vault.swap.refund` |
| `zcl.zcode_leaderboard.v1` | `zcode.leaderboard.daily`, `zcode.leaderboard.weekly`, `zcode.leaderboard.monthly`, `zcode.leaderboard.all` |


## Envelope shapes (quick reference)

Full spec: `docs/NATIVE_COMMAND_INTERFACE.md` §8–§9. Summary only:

| Schema | When | Key fields |
|---|---|---|
| `zcl.command_menu.v1` | `discover help <branch>` / invoking a branch | `path`, `summary`, `registry_digest`, `children[]` (each: `path`,`summary`,`risk`,`latency`,`availability` — nothing else) |
| `zcl.command_spec.v1` | `discover describe <leaf>` | `availability`(+`availability_reason` if non-ready), `input_schema{id,allowed_keys,positional_keys}`, `output_schema`, `policy{layer,effect,risk,scope,authority,mode,latency,cost,confirmation,...}`, `example` |
| `zcl.command_search.v1` | `discover search <text>` | `matches[]` (≤5: `path`,`reason`,`risk`,`latency`,`availability`), `total_matches`, `truncated` |
| `zcl.result.v1` | executing any leaf | `ok`, `status` (`passed`\|`accepted`\|`blocked`\|`failed`), `data_schema`+`data` on success, `error{code,message,phase,retryable,mutated,blockers}` on failure, `next[]` |

Exit codes: `0` passed/accepted · `1` failed · `2` invalid input/unknown
command · `3` blocked by a named precondition (includes every `planned`
leaf) · `4` auth/capability denied · `5` transiently unavailable · `6`
internal contract failure.

## Where this is proven, not just documented

| Invariant | Test |
|---|---|
| This page matches `config/commands/*.def` byte for byte | `tools/lint/check_api_reference_generated.sh` (lint gate `check-api-reference-generated`) |
| Catalog well-formed, every leaf has schemas/example, ready⇒handler, planned⇒no handler | `test_command_registry_catalog.c::test_catalog_wellformed`, `test_ready_leaves_bound`, `test_planned_fail_closed` |
| Root menu stays in budget; branch menus stay shallow | `test_command_registry_catalog.c::test_root_menu_budget`, `test_branch_menus_shallow` |
| **Every** branch menu lists only its own immediate children, fixed 5-field shape | `test_native_api_contract.c::test_every_branch_menu_lists_only_own_children` |
| **Every** leaf's dotted path resolves 1:1 through its space-separated CLI words | `test_native_api_contract.c::test_every_leaf_dot_path_resolves_from_cli_words` |
| Declared aliases resolve through the same grammar | `test_native_api_contract.c::test_root_and_discover_aliases_resolve` |
| Search returns ≤5 ranked matches | `test_command_registry_catalog.c::test_search_bounded` |
| Missing required input fails closed with a structured `zcl.result.v1` error, not a silent pass | `test_command_registry_catalog.c::test_ops_state_requires_subsystem`; `test_native_api_contract.c::test_missing_required_input_fails_closed_structured` |
| `dev.*` leaves are release-`compat`, never falsely `ready` | `test_command_registry_catalog.c::test_dev_branch_leaves`, `test_dev_vcs_revert_release_stub`, `test_dev_vcs_seal_grant_release_stub` |
| Every bridged `ready` leaf has exactly one dispatch binding (body fn XOR direct RPC) | `test_command_registry_catalog.c` bridge-binding coverage |
| Release binary links no dev-mutation executor symbols | `tools/lint/check_release_no_dev_symbols.sh` |
