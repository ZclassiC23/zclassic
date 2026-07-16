# API_REFERENCE.md — the native command tree, leaf by leaf

This is a **reference table**, not a spec. For the grammar, envelope shapes,
budgets, and migration status, read
[`docs/NATIVE_COMMAND_INTERFACE.md`](./NATIVE_COMMAND_INTERFACE.md) first —
that document is the frozen contract; this one enumerates every leaf the
contract currently declares. For the pre-existing MCP/RPC-era agent surface
(`agentops`, `agentdiagnose`, `zcl_agent_*`, `servicecatalog`, ZNAM/ZMSG/
Market/ZSWP RPCs, etc.) see [`docs/AGENT_API.md`](./AGENT_API.md) — that
surface predates the command registry and is not yet folded into it; the
[ZERO-MCP removal program](./work/MCP-REMOVAL-PLAN.md) is
retargeting agent interaction onto the tree documented here.

## Source of truth

Every row below is transcribed directly from the declarative `.def` files
under [`config/commands/`](../config/commands/) — `root.def`, `core.def`,
`apps.def`, `ops.def`, `dev.def` — compiled by
[`config/src/command_catalog.c`](../config/src/command_catalog.c) into the
immutable `g_catalog_commands[]` table (`lib/kernel/src/command_registry.c`).
Nothing here is from memory or an older doc; every path, schema id, and
example was either read from the `.def` source or confirmed live against a
binary built at commit `88b4e1030` with:

```bash
zclassic23 discover help [path]        # branch menu — immediate children only
zclassic23 discover describe <path>    # one leaf's full spec
zclassic23 discover search <text>      # ≤5 ranked matches
zclassic23 discover schema <path> --side=input|output
```

Regenerate this table by re-reading the `.def` files (or re-running
`discover help`/`describe` against a fresh build) whenever a leaf is added,
promoted, or its schema changes — `lib/test/src/test_command_registry_catalog.c`
and `lib/test/src/test_native_api_contract.c` gate the registry itself, not
this doc's prose.

As of `88b4e1030`: **106 leaves** (57 `ready`, 2 `compat`, 15 `planned` read,
18 `planned` mutating, 8 dev-gated `ready`/`compat`, 6 dev-gated mutating)
across **41 branches**, rooted at 6 top-level choices
(`status`, `core`, `app`, `dev`, `ops`, `discover`).

## Column legend

| Column | Meaning |
|---|---|
| **Path** | machine id, dots (`core.chain.block.get`) |
| **CLI** | same path, spaces, as typed at the shell |
| **Avail** | `ready` (dispatches now) · `compat` (metadata only; NULL handler, `→ compat_target`) · `planned` (fail-closed BLOCKED, exit 3, no handler) |
| **Risk / Auth** | `zcl_command_risk` / `zcl_command_authority` — see `docs/NATIVE_COMMAND_INTERFACE.md` §13 |
| **Input (required)** | allowed input keys; **bold** = `positional_keys` (the key(s) the handler requires) |
| **Output schema** | `output_schema` id |

A `ready` leaf always has a live handler in this build; a `planned` leaf
always fails closed with `COMMAND_PLANNED` (exit 3) and no handler — never a
silent stub. Both invariants are proven for the *whole* catalog by
`test_command_registry_catalog.c` (`test_catalog_wellformed`,
`test_ready_leaves_bound`, `test_planned_fail_closed`), not asserted here.

**Dev-gated leaves** (marked 🔧 below): declared via `ZCL_COMMAND_DEV_READ` /
`ZCL_COMMAND_DEV_COMMAND` in `dev.def`. In a `ZCL_DEV_BUILD` binary
(`zclassic23-dev`) they are `ready` with a real handler. In this release
build they are honest `compat` stubs — `discover describe` reports
`availability_reason` and a `compat_target` telling you to run the same
command against `zclassic23-dev` instead. `tools/lint/check_release_no_dev_symbols.sh`
proves via `nm` that the release binary links none of the dev executors, so
this is a structural guarantee, not a convention.

**Never RPC/REST/MCP-bound**: everything under `dev.*` is checkout-local by
design (`config/commands/README.md`: "No `lib/` source may include App,
controller, service, or development handler headers"). Almost every `ready`
leaf under `core.*` and `ops.*` dispatches through `zcl_native_bridge_command`
— a direct, MCP-router-free call into either a re-homed native handler body
(`app/controllers/src/*_native_handlers.c`) or the backing JSON-RPC method,
per the Zero-MCP W0-A re-home. Two `ops.*` leaves bind their own dedicated
handler instead of the generic bridge: `ops.state`
(`zcl_native_handle_ops_state`, the native `zcl_state` primitive — its input
guard is node-free, but a successful dump still calls the `dumpstate` RPC
against a live node) and `ops.selftest`
(`zcl_native_handle_ops_selftest`, a pure registry sweep with no RPC/MCP tool
behind it at all — it never contacts a node). The MCP tool name
named in `docs/AGENT_API.md` for the same operation is dual-run equivalence
metadata, not a second implementation.

---

## Root — `status`, `core`, `app`, `dev`, `ops`, `discover`

| Path | CLI | Avail | Risk / Auth | Summary |
|---|---|---|---|---|
| `status` | `zclassic23 status` | ready; native-only `zcl.core_status_brief.v1` data in a `zcl.result.v1` envelope | read / public | Compact node status and next action |
| `core` | `zclassic23 core` | ready (branch) | read / public | Consensus-bound node capabilities |
| `app` | `zclassic23 app` | ready (branch) | read / public | Capability-scoped sovereign applications |
| `dev` | `zclassic23 dev` | ready (branch) | read / public | Native edit and proof loop; runtime publication is contained |
| `ops` | `zclassic23 ops` | ready (branch) | read / public | Bounded diagnosis and node operations |
| `discover` | `zclassic23 discover` | ready (branch) | read / public | Search and describe the command registry |

### `discover.*`

| Path | CLI | Avail | Input (required) | Output schema | Summary |
|---|---|---|---|---|---|
| `discover.help` | `discover help [path]` (aliases: `help`, `dev.help`) | ready | `path` | `zcl.discovery_document.v1` | Show one branch or leaf without loading the whole tree |
| `discover.search` | `discover search <text>` (aliases: `search`, `dev.search`, `dev.diagnose.search`) | ready | `query` | `zcl.command_search.v1` | Rank ≤5 commands by local deterministic intent search |
| `discover.describe` | `discover describe <path>` | ready | **`path`** | `zcl.command_spec.v1` | Describe one leaf's schemas, safety, authority, availability |
| `discover.schema` | `discover schema <path> [--side=input\|output]` | ready | `path,side` (**`path`**) | `zcl.command_schema.v1` | Return the compact input/output schema contract for one leaf |

Missing `path` on `discover.describe`/`discover.schema` fails closed with
`error.code=UNKNOWN_PATH`, exit `INVALID` (2) — proven for both leaves by
`test_native_api_contract.c::test_missing_required_input_fails_closed_structured`.

---

## `core.*` — consensus-bound node capabilities

All `ready` leaves below dispatch via `zcl_native_bridge_command`. Lane
scope for every `core.*`/`ops.*` leaf is `local | dev | canonical | soak`
(`ZCL_COMMAND_LANE_LOCAL | ZCL_COMMAND_LANE_ALL_NODE`) unless noted.

### `core.status`, `core.chain.*`

| Path | CLI | Avail | Risk / Auth | Input (required) | Output schema | Summary |
|---|---|---|---|---|---|---|
| `core.status` | `core status` | ready | read / public | none | `zcl.core_status.v1` | Consensus node status: height, sync, health |
| `core.chain.tip` | `core chain tip` | ready | read / public | none | `zcl.chain_tip.v1` | Active chain tip in one call |
| `core.chain.block.get` | `core chain block get --height=N \| --hash=H` | ready | read / public | `height,hash,verbosity` (**`height`**) | `zcl.block.v1` | Get one block by height or hash |
| `core.chain.transaction.get` | `core chain transaction get --txid=<hex>` | ready | read / public | `txid,verbose` (**`txid`**) | `zcl.transaction.v1` | Get one transaction by id |
| `core.chain.mempool.status` | `core chain mempool status` | ready | read / public | none | `zcl.mempool_status.v1` | Mempool size, bytes, fee summary |
| `core.chain.mempool.list` | `core chain mempool list` | ready | read / public | none | `zcl.mempool_list.v1` | List mempool transaction ids |
| `core.chain.wait.height` | `core chain wait height --height=N` | **planned** | read / operator | `height,timeout_ms` (**`height`**) | `zcl.wait_result.v1` | Wait until the tip reaches a target height — *deferred to the Wave 2.2 job protocol* |
| `core.chain.wait.blocker` | `core chain wait blocker` | **planned** | read / operator | `blocker,timeout_ms` (**`blocker`**) | `zcl.wait_result.v1` | Wait until a named blocker is raised/cleared — *deferred to Wave 2.2* |
| `core.chain.wait.halt` | `core chain wait halt` | **planned** | read / operator | `timeout_ms` | `zcl.wait_result.v1` | Wait until the node halts on a named blocker — *deferred to Wave 2.2* |

### `core.sync.*`

| Path | CLI | Avail | Risk / Auth | Input | Output schema | Summary |
|---|---|---|---|---|---|---|
| `core.sync.status` | `core sync status` | ready | read / public | none | `zcl.sync_status.v1` | Sync phase and header/block gap |
| `core.sync.validation` | `core sync validation` | ready | read / public | none | `zcl.validation_status.v1` | Background validation progress |
| `core.sync.blockers` | `core sync blockers` | ready | read / public | none | `zcl.blockers.v1` | Active named sync blockers |
| `core.sync.diagnose` | `core sync diagnose` | ready | read / operator | none | `zcl.syncdiag.v1` | Diagnose why sync is not advancing (`ZCL_COMMAND_COST_MODERATE`) |

### `core.consensus.*`

| Path | CLI | Avail | Risk / Auth | Input (required) | Output schema | Summary |
|---|---|---|---|---|---|---|
| `core.consensus.report` | `core consensus report` | ready | read / public | none | `zcl.consensus_report.v1` | Consensus parity and rule report |
| `core.consensus.integrity` | `core consensus integrity` | ready | read / public | none | `zcl.data_integrity.v1` | SHA3 over consensus tables (foreground, moderate cost) |
| `core.consensus.utxo.commitment` | `core consensus utxo commitment` | ready | read / public | none | `zcl.utxo_commitment.v1` | SHA3 commitment over the UTXO set |
| `core.consensus.utxo.audit` | `core consensus utxo audit` | ready | read / operator | none | `zcl.utxo_audit.v1` | Audit the UTXO set for drift |
| `core.consensus.mmb` | `core consensus mmb` | ready | read / public | none | `zcl.mmb.v1` | Merkle Mountain Belt (FlyClient) commitment state |
| `core.consensus.block.invalidate` | `core consensus block invalidate --hash=<h>` | **planned** | destructive / core-recovery / **owner**, plan-commit | **`hash`** | `zcl.block_mutation.v1` | Mark a block invalid and reorg away from it — *Wave 2.2 confirmation handshake* |
| `core.consensus.block.reconsider` | `core consensus block reconsider --hash=<h>` | **planned** | mutate / core-recovery / **owner**, plan-commit, reversible | **`hash`** | `zcl.block_mutation.v1` | Clear an invalid mark and reconsider a block — *Wave 2.2* |

### `core.network.*`

| Path | CLI | Avail | Risk / Auth | Input (required) | Output schema | Summary |
|---|---|---|---|---|---|---|
| `core.network.status` | `core network status` | ready | read / public | none | `zcl.network_status.v1` | Network info and connections |
| `core.network.peers.list` | `core network peers list` | ready | read / public | none | `zcl.peers.v1` | List connected peers |
| `core.network.peers.incidents` | `core network peers incidents` | ready | read / operator | none | `zcl.peer_incidents.v1` | Recent peer misbehavior incidents |
| `core.network.peers.latency` | `core network peers latency` | ready | read / public | none | `zcl.peer_latency.v1` | Round-trip latency for every peer |
| `core.network.peers.add` | `core network peers add --address=<ip:port>` | **planned** | mutate / core-recovery / operator | **`address`** | `zcl.peer_add.v1` | Add an outbound peer connection — *Wave 2.2* |
| `core.network.onion.status` | `core network onion status` | ready | read / public | none | `zcl.onion_status.v1` | Onion address and bootstrap status |
| `core.network.onion.health` | `core network onion health` | ready | read / operator | none | `zcl.onion_health.v1` | Onion reachability health |

### `core.wallet.*`

| Path | CLI | Avail | Risk / Auth | Input (required) | Output schema | Summary |
|---|---|---|---|---|---|---|
| `core.wallet.status` | `core wallet status` | ready | read / operator, cap `WALLET_REQUEST` | none | `zcl.wallet_status.v1` | Wallet summary and key counts |
| `core.wallet.balance` | `core wallet balance` | ready | read / operator | none | `zcl.wallet_balance.v1` | Confirmed and total balance |
| `core.wallet.address.new` | `core wallet address new` | **planned** | mutate / wallet / **owner** | none | `zcl.wallet_address.v1` | Derive and persist a new transparent address — *Wave 2.2* |
| `core.wallet.address.list` | `core wallet address list` | ready | read / operator | none | `zcl.wallet_addresses.v1` | List transparent addresses |
| `core.wallet.address.import` | `core wallet address import --address=<addr>` | **planned** | mutate / wallet / **owner** | **`address`** | `zcl.wallet_address.v1` | Import a watch-only address — *Wave 2.2* |
| `core.wallet.address.export-key` | `core wallet address export-key --address=<addr>` | **planned** | read / **owner**, plan-commit | **`address`** | `zcl.wallet_privkey.v1` | Export the private key for an address — *owner-gated, Wave 2.2* |
| `core.wallet.utxo.list` | `core wallet utxo list` | ready | read / operator | none | `zcl.wallet_utxos.v1` | List spendable UTXOs |
| `core.wallet.transaction.list` | `core wallet transaction list` | ready | read / operator | none | `zcl.wallet_tx_list.v1` | List recent wallet transactions |
| `core.wallet.transaction.get` | `core wallet transaction get --txid=<hex>` | ready | read / operator | **`txid`** | `zcl.wallet_tx.v1` | Get one wallet transaction by id |
| `core.wallet.transaction.send` | `core wallet transaction send --input='<obj>'` | **planned** | mutate / wallet / **owner**, plan-commit | `address,amount,idempotency_key` | `zcl.wallet_send.v1` | Build, sign, and broadcast a payment — *Wave 2.2 spend handshake* |
| `core.wallet.shielded.address` | `core wallet shielded address` | **planned** | mutate / wallet / **owner** | none | `zcl.shielded_address.v1` | Derive a new shielded address — *Wave 2.2* |
| `core.wallet.shielded.balance` | `core wallet shielded balance --address=<zaddr>` | **planned** | read / operator | **`address`** | `zcl.shielded_balance.v1` | Shielded balance for one address — *Wave 2.2 arg mapping* |
| `core.wallet.shielded.notes` | `core wallet shielded notes` | **planned** | read / operator | none | `zcl.shielded_notes.v1` | List spendable shielded notes — *Wave 2.2* |
| `core.wallet.shielded.send` | `core wallet shielded send --input='<obj>'` | **planned** | mutate / wallet / **owner**, job, plan-commit | `from,to,amount,idempotency_key` | `zcl.shielded_send.v1` | Send a shielded payment (`z_sendmany`) — *Wave 2.2* |
| `core.wallet.backup.status` | `core wallet backup status` | ready | read / operator | none | `zcl.wallet_backup_status.v1` | Wallet backup freshness |
| `core.wallet.backup.now` | `core wallet backup now` | **planned** | mutate / wallet / **owner**, idempotent | none | `zcl.wallet_backup.v1` | Take a wallet backup now — *Wave 2.2* |
| `core.wallet.audit` | `core wallet audit` | ready | read / operator | none | `zcl.wallet_audit.v1` | Audit wallet key/UTXO consistency (foreground) |
| `core.wallet.rescan` | `core wallet rescan` | **planned** | mutate / wallet / **owner**, job | `start_height` | `zcl.wallet_rescan.v1` | Rescan the chain for wallet transactions — *Wave 2.2* |
| `core.wallet.replay` | `core wallet replay` | **planned** | mutate / wallet / **owner**, job | none | `zcl.wallet_replay.v1` | Replay wallet state from chain — *Wave 2.2* |

### `core.storage.*`, `core.mining.*`

| Path | CLI | Avail | Risk / Auth | Input (required) | Output schema | Summary |
|---|---|---|---|---|---|---|
| `core.storage.stats` | `core storage stats` | ready | read / operator | none | `zcl.db_stats.v1` | Database size and table stats |
| `core.storage.integrity` | `core storage integrity` | **planned** | read / operator | none | `zcl.storage_integrity.v1` | Verify raw storage integrity — *distinct handler is Wave 2.2* |
| `core.storage.query` | `core storage query --sql='SELECT ...'` | ready | read / operator | `sql,limit` (**`sql`**) | `zcl.storage_query.v1` | Run one SELECT-only query over node.db |
| `core.mining.status` | `core mining status` | ready | read / public | none | `zcl.mining_status.v1` | Mining info and difficulty |
| `core.mining.benchmark` | `core mining benchmark` | ready | read / operator | none | `zcl.mining_benchmark.v1` | Run an Equihash solver benchmark (foreground, moderate cost) |

---

## `app.*` — capability-scoped sovereign applications

| Path | CLI | Avail | Risk / Auth | Input (required) | Output schema | Summary |
|---|---|---|---|---|---|---|
| `app.list` | `app list` | ready | read / public | none | `zcl.app_index.v1` | List installed App manifests |
| `app.inspect` | `app inspect <app_id>` | ready | read / public | **`app_id`** | `zcl.app_manifest_summary.v1` | Inspect one App manifest and bindings |
| `app.protocols` | `app protocols` | compat → `zclassic23 appprotocols` (alias) | read / public | none | `zcl.app_protocols.v1` | List App protocol contracts (also native/RPC/MCP-legacy today) |
| `app.invoke` | `app invoke <id> [path...]` | ready (branch) | read / public | — | — | Invoke a manifest-owned App route; children are dynamic per `apps/<id>/app.def` (contract §6) — today only `social` is installed |

`app.list`/`app.inspect` are checkout-local: they enumerate
`apps/<id>/app.def` on disk (`zcl_native_handle_app_list` /
`zcl_native_handle_app_inspect`), the same lookup `dev.app.list` uses.

---

## `ops.*` — bounded diagnosis and node operations

| Path | CLI | Avail | Risk / Auth | Input (required) | Output schema | Summary |
|---|---|---|---|---|---|---|
| `ops.health` | `ops health` | ready | read / public | none | `zcl.health.v1` | Health rollup |
| `ops.diagnose` | `ops diagnose` | ready | read / operator | none | `zcl.ops_diagnose.v1` | Operator diagnosis (moderate cost) |
| `ops.lanes` | `ops lanes` | ready | read / operator | none | `zcl.ops_lanes.v1` | Lane status (canonical/soak/dev topology) |
| `ops.jobs.list` | `ops jobs list` | **planned** | read / operator | none | `zcl.jobs.v1` | List asynchronous jobs — *native job registry is Wave 2.2* |
| `ops.logs` | `ops logs --pattern='blocker'` | ready | read / operator | `pattern,since_secs,max_lines,level` (**`pattern`**) | `zcl.ops_logs.v1` | Server-side regex tail of node.log |
| `ops.timeline` | `ops timeline` | ready | read / operator | none | `zcl.ops_timeline.v1` | Event timeline |
| `ops.metrics` | `ops metrics` | ready | read / operator | none | `zcl.ops_metrics.v1` | Runtime metrics |
| `ops.state` | `ops state --subsystem=reducer_frontier` | ready | read / operator | `subsystem,key` (**`subsystem`**) | `zcl.ops_state.v1` | Subsystem state dump (the `zcl_state` primitive) — missing `subsystem` fails `MISSING_SUBSYSTEM`/INVALID before any node call (`test_command_registry_catalog.c::test_ops_state_requires_subsystem`) |
| `ops.selftest` | `ops selftest` | ready | read / operator | none | `zcl.ops_selftest.v1` | Registry self-test — node-free catalog sweep, the native successor of MCP `zcl_self_test mode=registry` |
| `ops.postmortem.list` | `ops postmortem list` | ready | read / operator, cap `DEV_STATE_READ` | none | `zcl.postmortem_list.v1` | List captured postmortems |
| `ops.postmortem.replay` | `ops postmortem replay <id>` | **planned** | read / operator | **`id`** | `zcl.postmortem_replay.v1` | Replay one captured postmortem — *Wave 2.2 arg mapping* |
| `ops.config.show` | `ops config show` | **planned** | read / operator | none | `zcl.ops_config.v1` | Show effective runtime configuration — *Wave 2.2* |
| `ops.config.reload` | `ops config reload` | **planned** | mutate / core-recovery / operator, idempotent | none | `zcl.ops_config_reload.v1` | Reload runtime configuration — *Wave 2.2* |
| `ops.recovery.status` | `ops recovery status` | ready | read / operator | none | `zcl.recovery_status.v1` | Refold and recovery progress |
| `ops.recovery.rebuild` | `ops recovery rebuild --depth=100` | **planned** | destructive / core-recovery / **owner**, job, plan-commit | `depth` | `zcl.recovery_rebuild.v1` | Rebuild recent chain state — *Wave 2.2* |

---

## `dev.*` — native edit and proof loop (runtime publication contained)

Phase-0 deliberately refuses every runtime-generation publication authority.
`dev.change.apply`, `dev.hotswap.apply`, `auto`/`apply` watchers, direct
activation backends, and generation-relinking revert cannot mutate a running
generation. Build, plan, simulate, verify-only watch, and `.so` construction
remain available. Resident probing is also contained because pre-admission
`dlopen` can execute constructors. This containment is removed only when an
immutable source epoch, complete proof receipts, resident expected-epoch CAS,
durable acceptance, and exact rollback form one transaction.

🔧 = dev-gated (`ZCL_COMMAND_DEV_READ`/`_COMMAND`): `ready` only in a
`ZCL_DEV_BUILD` (`zclassic23-dev`); this release binary reports `compat`
with the noted fallback. Everything else in this section is
`ZCL_COMMAND_READY_READ` and dispatches in *every* build, release included,
because it only touches the checkout on disk (App manifests, file
classification) — never process execution, watcher ownership, or
generation activation.

### `dev.status`, `dev.core.*`

| Path | CLI | Avail | Input (required) | Output schema | Summary |
|---|---|---|---|---|---|
| `dev.status` | `dev status` | ready (every build — reads a state file from disk, not gated by `ZCL_DEV_BUILD`) | none | `zcl.dev_cycle.v1` | Read the latest bounded native cycle verdict |
| `dev.core.boundary` | `dev core boundary` | ready | none | `zcl.core_app_boundary.v1` | Show the enforced Core/App ownership law |
| `dev.core.proof` | `dev core proof` | planned | `files` | `zcl.dev_core_proof.v1` | Run mandatory Core parity proof lanes — *native proof job extraction incomplete* |

### `dev.app.*`

| Path | CLI | Avail | Input (required) | Output schema | Summary |
|---|---|---|---|---|---|
| `dev.app.list` | `dev app list` | ready | none | `zcl.dev_app_index.v1` | List checkout App manifests |
| `dev.app.describe` | `dev app describe <app_id>` | ready | **`app_id`** | `zcl.dev_app.v1` | Describe an App manifest and its proofs — missing `app_id` fails `MISSING_APP_ID`/INVALID |
| `dev.app.plan` | `dev app plan <app_id> <resource>` | ready | **`app_id,resource`** | `zcl.dev_app_plan.v1` | Plan one conventional App resource slice (Rails-like) — missing either arg fails `MISSING_ARGS`/INVALID |
| `dev.app.scaffold` | `dev app scaffold <app_id> <resource>` | planned | **`app_id,resource`** | `zcl.dev_app_scaffold.v1` | Materialize a conventional App resource slice — *native file materializer not implemented* |
| `dev.app.simulate` | `dev app simulate <app_id> [--seed=]` | ready | `app_id,scenario,seed` (**`app_id`**) | `zcl.dev_app_sim.v1` | Run deterministic App network scenarios (censorship/partition/replay) |
| `dev.app.inspect` | `dev app inspect <app_id>` | planned | **`app_id`** | `zcl.dev_app_inspect.v1` | Inspect a resident App generation — *public App ABI not connected to generations yet* |
| `dev.app.publish` | `dev app publish <app_id> --idempotency-key=<k>` | planned | `app_id,idempotency_key` (**`app_id`**), job, idempotency-confirm | `zcl.dev_app_publish.v1` | Atomically publish a proven App generation — *ABI generation publication not wired* |

`dev.app.describe`/`dev.app.plan` missing-required-key failures are locked by
`test_native_api_contract.c::test_missing_required_input_fails_closed_structured`.

### `dev.change.*`, `dev.vcs.*`

| Path | CLI | Avail | Input (required) | Output schema | Summary |
|---|---|---|---|---|---|
| `dev.change.plan` | `dev change plan --input='{"files":[...]}'` | ready | `files` | `zcl.dev_plan.v1` | Classify files and select the smallest proof |
| `dev.change.apply` 🔧 | `dev change apply --input='{"files":[...]}'` (alias `dev.change.cycle`) | **contained — always refuses before publication** | `files`, job | `zcl.dev_cycle.v1` | Registered compatibility entry point; returns `publication_contained`, never activates a generation |
| `dev.vcs.revert` 🔧 | `dev vcs revert --input='{"to":"<64-hex>","relink_generation":false}'` | dev build: source-only revert ready | `to,relink_generation` (**`to`**), reversible | `zcl.dev_vcs_revert.v1` | Append a source revert when `relink_generation=false`; `true` refuses before VCS mutation while generation publication is contained. Release builds remain `DEV_BUILD_REQUIRED`/BLOCKED |
| `dev.vcs.seal.grant` 🔧 | `dev vcs seal grant --input='{"reason":"...","confirm":true}'` | compat → `zclassic23-dev dev vcs seal grant ...` | `reason,confirm` (**`reason`**) | `zcl.dev_vcs_seal_grant.v1` | Mint a one-shot ZVCS unseal token for the current worktree's sealed content — release stub fails closed `DEV_BUILD_REQUIRED`/BLOCKED regardless of `confirm` |

### `dev.hotswap.*` (Tier-1 in-process hot-swap)

| Path | CLI | Avail | Input (required) | Output schema | Summary |
|---|---|---|---|---|---|
| `dev.hotswap.apply` 🔧 | `dev hotswap apply --input='{"so_path":"...","probe_leaf":"..."}'` | **contained — always refuses before `dlopen`** | `so_path,probe_leaf` (**`so_path`**) | `zcl.dev_hotswap.v1` | Registered compatibility entry point; returns `runtime_publication_contained` and never re-points command leaves |
| `dev.hotswap.probe` 🔧 | `dev hotswap probe --input='{"so_path":"...","probe_leaf":"..."}'` | **contained — always refuses before `dlopen`** | `so_path,probe_leaf` (**`so_path`**) | `zcl.dev_hotswap.v1` | Registered compatibility entry point; returns `resident_probe_contained` pending disposable-worker and ELF admission |

See [`docs/work/HOTSWAP.md`](./work/HOTSWAP.md) for the full mechanism, ABI,
eligibility rules, and re-enable gates. The native leaves replace the former
MCP-only design; both native apply and legacy `zcl_agent_hotswap` are currently
contained. Native probing is contained too.

### `dev.loop.*` (persistent save-to-verdict watcher)

| Path | CLI | Avail | Input (required) | Output schema | Summary |
|---|---|---|---|---|---|
| `dev.loop.ensure` 🔧 | `dev loop ensure --input='{"root":".","mode":"verify"}'` (alias `dev.loop.watch`) | compat → `zclassic23-dev dev loop ensure` | optional `root,mode`; idempotent | `zcl.dev_loop_status.v1` | Ensure one verify-only watcher; `auto`/`apply` modes are rejected while publication is contained |
| `dev.loop.status` 🔧 | `dev loop status` (alias `dev.loop.heartbeat`) | compat → `zclassic23-dev dev loop heartbeat` | none | `zcl.dev_loop_status.v1` | Read watcher identity, epoch, latest verdict |
| `dev.loop.wait` 🔧 | `dev loop wait --input='{"after_epoch":41}' --view=summary` | compat → `zclassic23-dev dev loop wait` | `after_epoch,timeout_ms`; universal `--view` | `zcl.dev_cycle.v1` | Wait for one verdict after a sealed cycle epoch (persistent latency) |
| `dev.loop.events` | `dev loop events --format=jsonl` | planned | `after,heartbeat_ms`, stream | `zcl.dev_loop_event.v1` | Stream resumable source and cycle events — *resumable NDJSON journal not implemented* |
| `dev.loop.stop` 🔧 | `dev loop stop <watcher-id>` | compat → `zclassic23-dev dev loop stop` | **`watcher_id`** | `zcl.dev_loop_status.v1` | Stop one identified native watcher |

### `dev.test.*`

| Path | CLI | Avail | Input (required) | Output schema | Summary |
|---|---|---|---|---|---|
| `dev.test.plan` | `dev test plan --input='{"files":[]}'` | ready | `files` | `zcl.dev_test_plan.v1` | Map changed files to mandatory proof groups |
| `dev.test.run` 🔧 | `dev test run <group>` (alias `dev.test.focused`) | compat → `zclassic23-dev dev test focused` | **`group`** | `zcl.dev_focused_test.v1` | Run one exact prebuilt focused test group (background, high cost) |
| `dev.test.sim` 🔧 | `dev test sim` | compat → `zclassic23-dev dev test sim` | `app_id` | `zcl.dev_sim.v1` | Run the generic hot-swap network proof |
| `dev.test.replay` | `dev test replay <seed>` | planned | `seed,scenario` (**`seed`**) | `zcl.dev_test_replay.v1` | Replay one deterministic failure seed — *generic seed replay registry not implemented* |
| `dev.test.background.status` | `dev test background status` | planned | none | `zcl.dev_background_quality.v1` | Read lint/sanitizer/replay/reproducibility freshness — *native projection not implemented* |

### `dev.generation.*`, `dev.diagnose.*`

| Path | CLI | Avail | Input (required) | Output schema | Summary |
|---|---|---|---|---|---|
| `dev.generation.current` 🔧 | `dev generation current` | compat → `zclassic23-dev dev generation current` | none | `zcl.dev_generation_status.v1` | Show current and last-good generations |
| `dev.generation.history` 🔧 | `dev generation history` | compat → `zclassic23-dev dev generation history` | `cursor,max_items` | `zcl.dev_generation_history.v1` | Page accepted and rejected generations |
| `dev.generation.rollback` | `dev generation rollback --input='<intent>'` | planned | `intent_id,effect_digest`, job, plan-commit, reversible | `zcl.dev_generation_rollback.v1` | Restore verified last-good in the dev lane — *native activation engine not implemented* |
| `dev.generation.compact` | `dev generation compact --input='<intent>'` | planned | `intent_id,effect_digest`, job, plan-commit | `zcl.dev_generation_compact.v1` | Compact unleased old generations — *native lease-aware compaction not implemented* |
| `dev.diagnose.latest` 🔧 | `dev diagnose latest` | compat → `zclassic23-dev dev diagnose latest` | none | `zcl.dev_failure_latest_result.v1` | Read the most recently recorded deterministic compiler-failure summary; not current-cycle authority |
| `dev.diagnose.show` 🔧 | `dev diagnose show <failure-id> [--view=summary\|normal\|full]` | compat → `zclassic23-dev dev diagnose show <failure-id>` | **`failure_id`** | `zcl.dev_failure_show.v1` | Verify and show one workspace-scoped durable compiler-failure artifact |

---

The current `dev.status` / `dev.loop.wait` verdict is the authority for that
cycle's `failure_id`. `dev.diagnose.latest` is a workspace-local convenience
pointer; an edit or later green cycle does not erase it, so it may be stale.
A failure ID binds the source identity, proof phase, and normalized first error.
The mutation and execution IDs returned by `show` describe the first
observation, while `repeat_count` includes both executed and safely coalesced
observations. Normal output stays below 2 KiB and omits the capsule; explicit
`--view=full` is bounded by 6 KiB and adds the capsule and `retry_command`.
That command is `dev.ff`, which retries the current checkout and does not replay
the historical first-observation epoch. Per-worktree records are SHA3-sealed;
schema, inode, or digest corruption fails closed.
Every dev-state digest uses its schema domain plus labeled, NUL-delimited
field/value pairs. It is an integrity seal, not a claim of authorship.

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
| Catalog well-formed, every leaf has schemas/example, ready⇒handler, planned⇒no handler | `test_command_registry_catalog.c::test_catalog_wellformed`, `test_ready_leaves_bound`, `test_planned_fail_closed` |
| Root exposes exactly 6 choices; branch menus stay in budget (sample) | `test_command_registry_catalog.c::test_six_roots`, `test_root_menu_budget`, `test_branch_menus_shallow` |
| **Every** branch menu (all 41) lists only its own immediate children, fixed 5-field shape | `test_native_api_contract.c::test_every_branch_menu_lists_only_own_children` |
| **Every** leaf's dotted path resolves 1:1 through its space-separated CLI words | `test_native_api_contract.c::test_every_leaf_dot_path_resolves_from_cli_words` |
| Declared aliases (`agent`, `help`, `search`) resolve through the same grammar | `test_native_api_contract.c::test_root_and_discover_aliases_resolve` |
| Search returns ≤5 ranked matches | `test_command_registry_catalog.c::test_search_bounded` |
| `ops.state` and a second, disjoint set (`discover.describe`, `discover.schema`, `dev.app.describe`, `dev.app.plan`) reject missing required input with a structured `zcl.result.v1` error, not a silent pass | `test_command_registry_catalog.c::test_ops_state_requires_subsystem`; `test_native_api_contract.c::test_missing_required_input_fails_closed_structured` |
| `dev.*` leaves are release-`compat`, never falsely `ready`; `dev.vcs.revert`/`dev.vcs.seal.grant` fail closed outside a dev build | `test_command_registry_catalog.c::test_dev_branch_leaves`, `test_dev_vcs_revert_release_stub`, `test_dev_vcs_seal_grant_release_stub` |
| Every bridged `ready` leaf has exactly one MCP-free dispatch binding (body fn XOR direct RPC) | `test_command_registry_catalog.c::test_bridge_mcp_free_bindings` |
| Release binary links no dev-mutation executor symbols | `tools/lint/check_release_no_dev_symbols.sh` |
