# ZERO-MCP Removal Program

**Owner directive (2026-07-11):** remove MCP from the node binary **entirely**.
The stdio MCP server (`-mcp` / `-mcp-inprocess`), the `mcpcall` one-shot, the
router, the middleware, and the ~138 bespoke `zcl_*` tools all go. This is **not**
the compact 3-tool profile sketched in `docs/NATIVE_COMMAND_INTERFACE.md` §15 —
it is **zero MCP**. Agents drive the node purely through the native command
registry CLI (`zclassic23 <root> ...`: roots `status` / `core` / `app` / `dev` /
`ops` + `discover.*` + the RPC fallback) over Bash. REST/explorer (own HTTPS) and
the JSON-RPC server **remain**.

**Rationale:** the MCP catalog cost ~68k context tokens/session across three
server configs and the bespoke ontology sprawled; the native registry is now the
frozen contract.

**Prime invariant of this program: THE DEV LOOP AND NODE OPERATION NEVER BREAK
MID-PROGRAM.** MCP stays fully alive and dual-run until the very last wave; every
wave lands with `test_parallel` green, a working `make deploy-dev`, and one
proven real dev-lane hot swap.

---

## 0. Why this is not a simple delete — the load-bearing coupling

The single fact that shapes the whole program:

> **The native command CLI's read leaves currently run *through* MCP.**

`zcl_native_bridge_command()` (`tools/command/native_command.c:320-406`) resolves
a leaf path to an MCP tool name via `g_bridge_tools[]`
(`native_command.c:60-105`, 39 READ-ONLY entries), then calls
`mcp_middleware_dispatch(&g_bridge_mw, tool, args, bearer)`
(`native_command.c:359`) → `mcp_router_dispatch()` → the controller handler
(`h_zcl_status`, `h_zcl_getblock`, …) → `mcp_node_rpc()` (the RPC server).

So today:

- `zclassic23 core status` **is** the MCP `zcl_status` tool — the identical
  `h_zcl_status()` at `tools/mcp/controllers/ops_controller.c:861`. Genuinely one
  shared C handler, reached through the same static route table registered by
  `mcp_register_ops(); mcp_register_diagnostics(); …` at `native_command.c:135-142`,
  `src/main.c:1285-1292`, and `tools/mcp_server.c:181`.
- The MCP controller handlers are **typed proxies/aggregators over the RPC
  server**. Thin ones are 1:1 RPC (`zcl_getblock` → `getblock`); fat ones compose
  many RPC calls and shape the result (`h_zcl_status` issues 11 `mcp_node_rpc()`
  calls and derives sync/peer/health rollups, `ops_controller.c:864-874`).

**Consequence:** deleting `tools/mcp/**` before relocating those handler bodies
breaks *every* `core.*` / `ops.*` read command. The composition logic in the fat
aggregators (`zcl_status`, `zcl_kpi`, `zcl_syncdiag`, `zcl_health`,
`zcl_dataintegrity`, `zcl_utxocommitment`, `zcl_blockers`, `zcl_walletaudit`,
`zcl_agent_diagnose`, `zcl_agent_lanes`, `zcl_timeline`, `zcl_metrics`,
`zcl_refold_status`, `zcl_peer_incidents`, …) is *real code that must move, not
just be re-pointed.*

One convenient carve-out already exists: the **root `zclassic23 status`** command
does **not** go through the bridge. It maps CLI `status` → RPC method `agent`
(`src/main.c:990-995`) → `rpc_agent_summary()`
(`app/controllers/src/event_agent_summary.c:530`), a separately-coded status
composition living under `app/controllers/` (also serving REST `/api/v1/agent`).
That path is MCP-free today and survives untouched.

A second structural fact that helps: the registry already models MCP as
**transport-flag bits** — `ZCL_COMMAND_TRANSPORT_MCP_LEGACY` and
`ZCL_COMMAND_TRANSPORT_MCP_COMPACT`
(`lib/kernel/include/kernel/command_registry.h:167-168`). Removing MCP is partly
"drop those two flag bits and the enum," a clean lever.

---

## 1. Full inventory (verified in code)

### 1.1 Server / router / middleware / controllers (the code that dies)

- `tools/mcp/**` — `router.c/.h`, `middleware.c/.h`, `rpc_client.c/.h`,
  `rpc_params.c/.h`, `baseline.c/.h`, `metrics.c/.h`, `replay.c/.h`,
  `mcp_notify.c/.h`, `dev_rpc_bridge.c/.h`, and
  `controllers/{app,chain,meta,net,ops,wallet,dev_hotswap,diagnostics}_controller.c`
  + `controllers.h`. ~12,175 LOC.
- `tools/mcp_server.c` (separate entry; `mcp_server_main_inprocess`).
- `src/main.c`: `-mcp` / `-mcp-inprocess` run modes (`main.c:1637-1665`),
  `mcpcall` / `mcp` one-shot (`cli_run_mcp_call`, `cli_mcp_register_all`,
  `main.c:1282-1383,1529-1530`), and the include block (`main.c:60-63`).
- `.mcp.json` (repo root) — the `zcl23` stdio server config (`-mcp`).

### 1.2 Tier-1 hot-swap (the hardest piece — its own section §3)

- Swap unit today = one stateless **MCP controller TU** per generation; provider
  class `provider_id = "mcp.routes"` (`lib/hotswap/include/hotswap/hotswap.h:39-41,
  228-244`). Generation `.so` exports `k_routes[]` via `ZCL_HOTSWAP_EXPORT_ROUTES`;
  `zcl_hotswap_gen_init` loops it into `host->mcp_stage(name, &route)`
  (`hotswap.h:65-71,246-260`).
- Loader `hotswap_load()` (`lib/hotswap/src/hotswap_loader.c:601-768`) stages via
  `hotswap_stage_thunk` (`:452-481`), runs `manifest->self_test`, then the
  `commit_cb`.
- Publish = `mcp_router_replace_batch()` (`tools/mcp/router.c:263-334`): clones the
  active `struct mcp_router_snapshot` (`router.c:31-38`), overwrites replaced
  indexes, one atomic release-store on `g_active_snapshot`.
- Commit callback = `hotswap_commit_mcp_routes()`
  (`tools/mcp/controllers/dev_hotswap_controller.c:224-249`), which first runs
  `hotswap_commit_probe_candidate()` (`:162-211`) — dispatches the candidate route
  via `mcp_router_dispatch_route(route, NULL)` **before** the release-store.
- Eligibility manifest `config/hotswap_eligible.def` — **5 MCP controller TUs**:
  `app_controller.c` (`zcl_name_list`), `meta_controller.c` (`zcl_tools_list`),
  `chain_controller.c` (`zcl_getblockchaininfo`), `net_controller.c`
  (`zcl_networkinfo`), `wallet_controller.c` (`zcl_balance`). `HOTSWAP_PROBE(tool)`
  names an MCP tool.
- `zcl_agent_hotswap` MCP tool = `h_zcl_agent_hotswap`
  (`dev_hotswap_controller.c:266-341`, `#ifdef ZCL_DEV_BUILD`). Same code path as
  the raw `dev_hotswap` JSON-RPC method (`tools/mcp/dev_rpc_bridge.c:122-169,256`,
  which calls `mcp_router_dispatch("zcl_agent_hotswap", …)`).
- Resident dev-node commit path: `register_dev_mcp_rpc_commands()`
  (`tools/mcp/dev_rpc_bridge.*`) stands up a **resident MCP router** in the running
  dev node + the `dev_hotswap` / `dev_mcp_call` RPC methods (exact-lane
  `~/.zclassic-c23-dev` only).

### 1.3 Dev-loop + deploy consumers

- `tools/dev/deploy-dev-lane.sh:520,525,626,630-631` — preflight + post-activation
  verify exec `mcpcall zcl_tools_list` and `mcpcall zcl_self_test '{"mode":"registry"}'`.
- `tools/dev/devloop_cycle.c:746-748` — precommit probe `mcpcall zcl_agent_hotswap`;
  `:759-761` — resident commit `dev_hotswap <artifact> <probe_tool>`.
- `tools/dev/devloop_plan.c:18-22,132-138` — `HOTSWAP_PROBE` X-macro; `probe_tool`
  populated from the eligibility manifest.
- `tools/dev/hotswap-running-dev.sh:87` — `mcpcall zcl_agent_hotswap` smoke gate.
- `tools/scripts/hotswap_demo.sh:69` — per-TU `mcpcall zcl_agent_hotswap`.
- `tools/dev/watch-dev-lane.sh:483` — path-selection rationale string mentioning
  `mcpcall` (no invocation).
- `tools/dev/dev-loop-bench.sh:112` — `make hotswap FILES=…/app_controller.c PROBE=zcl_name_list`.
- `tools/dev/agent-dev-status.sh:419` — prints `make agent-mcp-call-dev TOOL=zcl_status`.
- `Makefile` — `agent-mcp-call{,-hot,-dev}` targets, `hotswap` / `hotswap-so`
  targets (parse the manifest), `docs-mcp` / `docs-mcp-check`,
  `NODE_ENTRY_SRCS = src/main.c tools/mcp_server.c` (`:174`),
  `NATIVE_ADAPTER_C_SRCS` globs `tools/mcp/*` (`:142-143`).
- `tools/agent_fast_ci.sh:186,213-216,632-639` — impact globs + `mcp_shortcuts`
  JSON.

### 1.4 Native-parity map — every operationally-used tool (verified against the
39 bridged leaves + scripts + tests, not all 138 tools)

| Category | Meaning | Handling |
|---|---|---|
| **(a) existing native leaf** | already reachable through the bridge/registry | re-home the handler off MCP (W0); path unchanged |
| **(b) RPC via CLI fallback** | one RPC method exists | `zclassic23 rpc <method> [params]` |
| **(c) NEW leaf to author** | no native equivalent today | build it (list below) |
| **(d) delete-with-the-tool** | unused off the daily path | delete in W3, no port |

**Category (c) — NEW native leaves that MUST be authored:**

1. **`ops.state`** — generic subsystem state dump (the `zcl_state` primitive).
   Absent from `g_bridge_tools[]` and `ops.def` today. Backed by the `dumpstate`
   RPC (`h_zcl_state` at `diagnostics_controller.c:89` just calls `dumpstate`).
2. **`ops.selftest`** — the `zcl_self_test` equivalent: iterate the registry's
   READ leaves, invoke each with safe defaults, report pass/fail/skip. Today only
   exists as `h_zcl_self_test` (`meta_controller.c:102`, iterates the MCP router).
   The deploy-dev-lane verify (`mcpcall zcl_self_test`) depends on this.
3. **`dev.hotswap.apply`** / **`dev.hotswap.probe`** (dev-build-only) — replace the
   `zcl_agent_hotswap` MCP tool + `dev_hotswap` RPC. See §3.
4. **Re-homed fat aggregators** — the composition in the fat bridged handlers
   (`zcl_status`→`core.status`, `zcl_kpi`, `zcl_syncdiag`→`core.sync.diagnose`,
   `zcl_health`→`ops.health`, `zcl_dataintegrity`→`core.consensus.integrity`,
   `zcl_utxocommitment`, `zcl_blockers`, `zcl_walletaudit`, `zcl_agent_diagnose`,
   `zcl_agent_lanes`, `zcl_timeline`→`ops.timeline`, `zcl_metrics`→`ops.metrics`,
   `zcl_refold_status`, `zcl_peer_incidents`) must have their multi-RPC
   composition relocated to a transport-neutral native handler/service under
   `app/controllers/`. This is authoring work even though the leaf path already
   exists — the *handler body* is what moves.

Thin 1:1 proxies (`zcl_getblock`, `zcl_getrawtransaction`, `zcl_getmempoolinfo`,
`zcl_getrawmempool`, `zcl_networkinfo`, `zcl_peers`, `zcl_getwalletinfo`,
`zcl_balance`, `zcl_listunspent`, `zcl_listaddresses`, `zcl_listtransactions`,
`zcl_gettransaction`, `zcl_dbstats`, `zcl_sql`, `zcl_getmininginfo`,
`zcl_syncstate`, `zcl_validationstatus`, `zcl_chain_tip`, `zcl_onion_status`,
`zcl_onion_health`, `zcl_wallet_backup_status`, `zcl_mmb`, `zcl_utxo_audit`,
`zcl_benchmark`, `zcl_networkinfo`, `zcl_consensus_report`, `zcl_node_log`) →
category (a)/(b): the native leaf calls the RPC method directly + reuses the
existing `zcl_native_bridge_project()` projection.

**Destructive / mutating tools** (`zcl_send`, `zcl_sendtoaddress`,
`zcl_invalidateblock`, `zcl_reconsiderblock`, `zcl_importprivkey`,
`zcl_importaddress`, `zcl_rescanblockchain`, wallet shielded ops, name/msg/market
writes) — today reachable **only via MCP** (their native leaves are `PLANNED`,
`handler=NULL`, fail-closed exit 3, `core.def`). All are backed by RPC methods, so
they fall back to `zclassic23 rpc <method>`. See **Risk 4** — this loses the typed
schema + destructive auth-tier/rate-limit that MCP middleware enforced, unless
promoted to native READY leaves.

### 1.5 Tests (migrate then delete)

Dedicated: `test_mcp_router`, `test_mcp_controllers` (asserts `EXPECTED_TOTAL 138`
+ per-domain counts `:53,89,117,122,126,127,130`), `test_mcp_baseline`,
`test_dev_mcp_rpc_bridge`, `test_mcp_inproc_equiv`, `test_mcp_e2e` (forks the real
`-mcp` binary), `test_mcp_notify`, `test_mcp_fuzz`, `test_mcp_middleware`,
`test_mcp_metrics`, `test_hotswap_simnet`, `test_hotswap_loader` (fixtures pin
`provider_id="mcp.routes"`, `source_identity="…app_controller.c"`).

Cross-cutting asserts to migrate: `test_syncdiag_rpc.c` (dozens of `mcp_tool` /
`fallback_mcp_tool` / `zcl_agent_*` field asserts + `agentimpact`
`mcp_changed`/`mcp_controllers` group asserts + `agentcontracts` `mcp_declared_count
>= 20`), `test_dev_platform.c` + `test_golden_dev_cycle.c` (hot-eligible
controller→probe tables, `ZCL_DEVLOOP_HOTSWAP`), `test_make_lint_gates.c` (asserts
`main.c` wires `cli_run_mcp_call`/`mcp_register_ops`/`mcpcall`, docs cite
`make agent-mcp-call`, `agent_impact_rules.def` contains `mcp_controllers`).

### 1.6 Config / docs

- `.mcp.json`; the three `claude mcp add` invocations in `docs/BUILD.md`,
  `docs/GETTING_STARTED.md`, `CLAUDE.md`.
- `docs/NATIVE_COMMAND_INTERFACE.md` §15 (rewrite to zero-MCP), plus §16/§21
  compat prose.
- `CLAUDE.md` MCP sections (large), `docs/CODEBASE_MAP.md`
  (`:78,91-92,123,150,569`), `docs/AGENT_TRAPS.md`, `docs/work/HOTSWAP.md`,
  `docs/AGENT_API.md`, `docs/AGENT_ARCHITECTURE.md`, `MCP_REFERENCE.md` (delete).
- No `deploy/*.service` passes `-mcp` (verified — `deploy/` is clean).

### 1.7 Lint gates + impact rules referencing MCP

- `Makefile` `check-silent-errors` (`:2773-2783`, scoped to
  `tools/mcp/controllers/`), `docs-mcp` / `docs-mcp-check` (`:2705-2725`),
  `check-hotswap-eligible-scope` / `check-hotswap-static-state` (parse the manifest),
  `check-hotswap-dev-only` (dlopen scope), `check-doc-no-false-deleted` PART 3
  (bands doc-cited MCP tool count vs routing tables, `gate_doc_no_false_deleted.sh:130-160`).
- `tools/lint/check_no_raw_sqlite_in_controllers.sh:31` includes
  `tools/mcp/controllers`.
- `tools/lint/gate_zclassicd_reach_allowlist.sh:17` scope includes `tools/mcp`.
- `app/controllers/include/controllers/agent_impact_rules.def` — many rows map
  `tools/mcp/*` / `test_mcp_*` to groups `mcp_controllers mcp_baseline mcp_router
  dev_mcp_rpc_bridge hotswap_*` (`:15-18,225-226,248-250`, etc.).
- `app/controllers/src/agent_controller.c:197-203` — impact rule mapping
  `tools/mcp/` → `mcp_agent_api`, sets `acc->mcp_changed`.

---

## 2. Wave table

| Wave | Goal | Key files (disjoint lanes) | Landed proof | MCP status after |
|---|---|---|---|---|
| **W0** | Native parity + mcpcall-free deploy verify. Every operationally-used tool reachable natively **without** calling MCP. | **A:** re-home fat-aggregator composition into `app/controllers/` services; re-point `zcl_native_bridge_command` off `mcp_middleware_dispatch`. **B:** NEW `ops.state`, `ops.selftest` leaves. **C:** `deploy-dev-lane.sh` + `watch-dev-lane.sh` verify → native commands. | `test_parallel` green; every bridged leaf returns identical body via native path with the MCP router uninvoked; `make deploy-dev` verify passes with zero `mcpcall`. | **fully alive** (dual surface) |
| **W1** | Hot-swap re-target to the handler layer. | **A:** `lib/kernel/command_registry.c/.h` atomic handler snapshot + `command_registry_replace_batch`. **B:** `lib/hotswap/*` new provider class `native.leaves` (`leaf_stage` vtable, `ZCL_HOTSWAP_EXPORT_LEAVES`). **C:** new `config/hotswap_eligible.def` schema (path→probe **leaf**); `dev.hotswap.apply/probe` + resident dev-node handler snapshot + native commit RPC. | One real dev-lane hot swap through the **native** path (recompile native handler TU → probe via `zclassic23 <leaf>` → resident commit → live handler re-pointed), with the MCP swap leg still green; `hotswap_simnet` + `golden_dev_cycle` green. | **alive, dual-run** (both swap paths) |
| **W2** | Migrate all consumers (scripts / tests / docs / configs) to native. | `devloop_cycle.c` + `devloop_plan.c` → native `dev hotswap`; migrate `test_syncdiag_rpc` / `test_dev_platform` / `test_golden_dev_cycle` / `test_make_lint_gates` asserts to native; rewrite NCI §15, `CLAUDE.md`, `CODEBASE_MAP`, `AGENT_TRAPS`, `HOTSWAP.md`; delete `.mcp.json` + `claude mcp add` lines; re-point doc-count + `docs-mcp` gates. | `test_parallel` green on native asserts; `golden_dev_cycle` green through native probe/commit; docs/gates cite native, not MCP. | **present but unused** by the dev loop |
| **W3** | Delete MCP + purge; final sweep. | Delete `tools/mcp/**`, `tools/mcp_server.c`, `-mcp`/`mcpcall`/`cli_mcp_*` in `main.c`, `dev_rpc_bridge` MCP coupling; delete `test_mcp_*`; drop `TRANSPORT_MCP_*` enum + catalog flags; purge lint gates (`check-silent-errors` mcp scope, `docs-mcp*`, doc-count MCP band, `NODE_ENTRY_SRCS`/`NATIVE_ADAPTER` globs); purge `agent_impact_rules.def` mcp rows + `acc->mcp_changed` + `agent_controller.c` mapping. | Full build + `test_parallel` green; `golden_dev_cycle` green (native only); `grep -rn mcp --include=*.c --include=*.h` returns only comments/history. | **gone** |

Lane partitioning rule (per project doctrine): each lane owns a **disjoint file
set**, is proven on its own dev-lane copy, and merges in dependency order
(W0-A before W0-C; W1-A before W1-B before W1-C). File-modifying work that touches
files another merge already changed is done by the driver, not a worktree lane
(worktrees fork from session-start).

---

## 3. Hot-swap re-target design (the hardest piece)

### 3.1 The architectural law and where the real swap unit should live

The law: *all transports call the same typed C handler; business logic never
lives in an adapter.* Today the swap unit violates the spirit of this — it swaps
an **MCP adapter TU** (`k_routes[]` + handler bodies that hold composition
logic). Once W0 re-homes the composition into transport-neutral
`app/controllers/` handlers and the native registry calls them directly, the
right swap unit becomes **the native command registry's leaf-handler dispatch
table** — the `handler` function-pointer column of `struct zcl_command_spec`
for a bounded set of leaf paths. That is the layer every remaining transport
(native CLI, REST, RPC) already funnels through
(`zcl_command_registry_execute_json`, `command_registry.c:1053-1161`).

### 3.2 New eligibility unit

`config/hotswap_eligible.def` schema changes from *(MCP-controller TU → MCP tool
probe)* to *(native handler TU → canonical READ-only, param-free leaf **path**
probe)*:

```
HOTSWAP_ELIGIBLE("app/controllers/src/<domain>_native_handlers.c")
    HOTSWAP_PROBE("core.network.status")   /* a leaf path, not a zcl_* tool */
```

The eligible TUs become the re-homed native handler bundles authored in W0 (one
stateless TU per domain: chain / net / wallet / meta-introspection / app). The two
scope/static lint gates (`check-hotswap-eligible-scope`,
`check-hotswap-static-state`) keep their guarantees — still forbid
`core|lib/consensus|lib/validation|lib/storage|lib/net|lib/coins|app/jobs`, still
forbid mutable file-scope statics — only the admitted directory set moves from
`tools/mcp/controllers/` to `app/controllers/src/*_native_handlers.c`.

### 3.3 New publish/commit mechanism

Mirror the *proven* `router.c` snapshot design one layer down, in `lib/kernel`:

- Add `struct command_handler_snapshot { uint32_t generation; struct { const char
  *path; zcl_command_handler_fn handler; } slots[N]; }` held in a
  `static … _Atomic *g_active_handlers`.
- `command_registry_replace_batch(generation, {path,handler}[], n, why, sz)` —
  clone the active snapshot, overwrite the named slots, single release-store.
  All-or-nothing; in-flight readers see old-or-new, never torn (exactly the
  `mcp_router_replace_batch` contract, `router.c:263-334`).
- `zcl_command_registry_execute_json` reads `handler` through the atomic snapshot
  (falling back to the immutable catalog's static `handler` column when a path has
  never been swapped), so a hot swap re-points the live handler with no restart.

The hotswap host vtable (`struct zcl_hotswap_host`, `hotswap.h:65-71`) gains a
sibling capability `ZCL_HOTSWAP_CAP_LEAF_STAGE` and callback
`bool (*leaf_stage)(const char *path, zcl_command_handler_fn handler)`. The
generation `.so` exports a native-leaf table via a new
`ZCL_HOTSWAP_EXPORT_LEAVES(table, count)` macro; `zcl_hotswap_gen_init` loops it
into `leaf_stage`; the loader collects the transaction exactly as today; the
`commit_cb` calls `command_registry_replace_batch`. Manifest `provider_id`
becomes `"native.leaves"`. The MCP `mcp_stage` path stays for dual-run through W1.

### 3.4 Probe design (probe = a native command invocation)

- **Precommit probe** (before the release-store): the commit callback runs the
  candidate handler against its probe **leaf path** — the native analogue of
  `mcp_router_dispatch_route(candidate, NULL)`. Concretely, `dev.hotswap.probe`
  (or the precommit stage of `dev.hotswap.apply`) invokes
  `zcl_command_registry_execute_json` against a *candidate* handler snapshot for
  the probe leaf and asserts the `zcl.result.v1` envelope is not
  `error`/`failed`/`blocked` (the native shape of
  `mcp_dev_hotswap_probe_body_ok`). Destructive probe leaves are rejected as
  today.
- **Dev-loop probe** (`devloop_cycle.c`): the short-lived one-shot
  `mcpcall zcl_agent_hotswap` becomes `zclassic23-dev dev hotswap probe
  --input='{"so_path":…,"probe_leaf":…}'`; the resident commit
  `dev_hotswap <artifact> <probe_tool>` becomes `zclassic23-dev dev hotswap apply
  <artifact> <probe_leaf>`. Both check the same `"ok":true` / no-`probe_error`
  envelope contract.

### 3.5 What happens to `zcl_agent_hotswap`

It becomes the dev-build-only registry command **`dev.hotswap.apply`** (executor
`zcl_native_handle_dev_hotswap_apply`, bound only under `ZCL_DEV_BUILD` like the
existing `dev.change.apply` / `dev.loop.*` executors; release build gets an honest
`COMPAT`/BLOCKED stub). The resident commit surface (`dev_hotswap` RPC in
`dev_rpc_bridge.c`) is re-implemented to stand up a **resident command-registry
handler snapshot** in the running dev node (replacing the resident MCP router) and
commit into it — keeping the exact-lane `~/.zclassic-c23-dev` guard. Both resident
paths coexist in W1; the MCP resident router is removed only in W3.

---

## 4. Old → new command table (session-workflow migration)

| Old (MCP) | New (native CLI over Bash) |
|---|---|
| `zcl_status` | `zclassic23 status` (root; MCP-free today) or `zclassic23 core status` |
| `zcl_kpi` | `zclassic23 status` (rollup) |
| `zcl_getblockcount` / `zcl_chain_tip` | `zclassic23 core chain tip` |
| `zcl_getblock` | `zclassic23 core chain block get --hash=<h>` (or `--height=N`) |
| `zcl_getrawtransaction` | `zclassic23 core chain transaction get --hash=<txid>` |
| `zcl_getmempoolinfo` / `zcl_getrawmempool` | `zclassic23 core chain mempool status` / `... list` |
| `zcl_syncstate` | `zclassic23 core sync status` |
| `zcl_validationstatus` | `zclassic23 core sync validation` |
| `zcl_blockers` | `zclassic23 core sync blockers` |
| `zcl_syncdiag` | `zclassic23 core sync diagnose` |
| `zcl_dataintegrity` | `zclassic23 core consensus integrity` |
| `zcl_utxocommitment` | `zclassic23 core consensus utxo commitment` |
| `zcl_peers` | `zclassic23 core network peers list` |
| `zcl_networkinfo` | `zclassic23 core network status` |
| `zcl_getwalletinfo` / `zcl_balance` | `zclassic23 core wallet status` / `... balance` |
| `zcl_listunspent` / `zcl_listaddresses` | `zclassic23 core wallet utxo list` / `... address list` |
| `zcl_dbstats` | `zclassic23 core storage stats` |
| `zcl_sql` | `zclassic23 core storage query --input='{"sql":"SELECT …"}'` |
| `zcl_getmininginfo` | `zclassic23 core mining status` |
| `zcl_health` | `zclassic23 ops health` |
| `zcl_node_log` | `zclassic23 ops logs --input='{"pattern":"…","level":"…"}'` |
| `zcl_state` (primitive) | **`zclassic23 ops state --input='{"subsystem":"reducer_frontier"}'`** (NEW) or `zclassic23 rpc dumpstate '["reducer_frontier"]'` |
| `zcl_tools_list` | `zclassic23 discover help [<path>]` / `zclassic23 discover search '<intent>'` |
| `zcl_openapi` | `zclassic23 discover schema <leaf> --side=input` |
| `zcl_self_test` | **`zclassic23 ops selftest`** (NEW) |
| `zcl_rpc(method=X, params=…)` | `zclassic23 rpc X '[…]'` (RPC fallback) |
| `zcl_send` / `zcl_invalidateblock` / `zcl_importprivkey` (destructive) | `zclassic23 rpc <method> …` (see Risk 4) |
| `zcl_agent_hotswap` (dev) | **`zclassic23-dev dev hotswap apply <artifact> <probe_leaf>`** (NEW, dev-only) |

`--view=summary|normal|full`, `--max-items`, `--cursor`, `--budget-bytes`, and
`--format=json` control progressive disclosure on any leaf
(`native_command.c:212-318`).

---

## 5. Top risks

1. **Native reads depend on MCP (the whole point of §0).** Deleting `tools/mcp/**`
   before W0 re-homes the ~39 bridged handler bodies breaks every `core.*`/`ops.*`
   read command. The fat aggregators (`zcl_status`, `zcl_kpi`, `zcl_syncdiag`,
   `zcl_health`, `zcl_dataintegrity`, `zcl_blockers`, `zcl_walletaudit`,
   `zcl_agent_*`, `zcl_timeline`, `zcl_metrics`, `zcl_peer_incidents`) hold real
   multi-RPC composition that must **move**, not just be re-pointed. *Mitigation:*
   W0 relocates composition first; MCP is deleted last (W3), never before parity is
   proven leaf-by-leaf with the MCP router uninvoked.

2. **New atomic handler snapshot in a hot path.** Re-targeting the swap to
   `lib/kernel` requires a new all-or-nothing, in-flight-reader-safe handler
   snapshot with the same guarantees `router.c` provides — a concurrency-sensitive
   addition to `zcl_command_registry_execute_json`, which every transport calls. A
   bug = torn dispatch on the live node. *Mitigation:* copy `router.c`'s proven
   release-store/clone design verbatim; dual-run behind `hotswap_simnet` +
   `golden_dev_cycle` before the MCP swap leg is removed.

3. **The running dev node commits generations via a resident MCP router.** The
   live dev node must keep a working commit path throughout. If W1 removed the
   resident MCP router before the native resident snapshot worked, a live dev node
   could not hot-swap — a mid-program dev-loop break. *Mitigation:* both resident
   paths coexist in W1; the MCP resident router is cut only in W3, after the native
   commit RPC is proven on a live dev lane.

4. **Destructive/mutating ops have no native READY leaf today.** `zcl_send`,
   `zcl_invalidateblock`, `zcl_reconsiderblock`, `zcl_importprivkey`, wallet
   shielded ops are reachable **only** via MCP (native leaves are `PLANNED`,
   fail-closed). Zero-MCP drops that typed surface; they fall back to raw
   `zclassic23 rpc <method>`, losing MCP middleware's typed schema validation and —
   more importantly — the **destructive auth-tier + destructive rate-limit bucket**.
   *Decision needed:* accept RPC-fallback for destructive ops, or promote them to
   native READY leaves (extra W0 scope) that re-home the destructive gating. Do not
   silently lose the escalated-credential boundary.

5. **A large test/gate contract surface is pinned to MCP.** `test_mcp_controllers`
   `EXPECTED_TOTAL 138` + per-domain counts, `test_syncdiag_rpc`'s dozens of
   `mcp_tool`/`zcl_agent_*` field asserts, `test_make_lint_gates` asserting
   `main.c` wires `mcpcall`/`mcp_register_ops` and docs cite `make agent-mcp-call`,
   `agentcontracts` requiring `mcp_declared_count >= 20`, and `docs-mcp-check`
   diffing live `-mcp` output. Miss one and a gate goes red or hollow-green.
   *Mitigation:* W2 migrates every assert to its native equivalent **before** W3
   deletes the tools; treat the `agent_contracts` `mcp_tool` bindings and the
   `syncdiag` `mcp` fields as their own migration lane.

**Residual capability loss (flag, likely acceptable):** MCP gave a *persistent
stdio channel* with operator-event push framing (`mcp_notify.c`). The native CLI
is one-shot per invocation — `zcl.result.v1` + `--view` covers structured
request/response, and `zclassic23 dev loop events --format=jsonl` gives a
resumable dev-loop stream, but there is **no general operator-event push**
equivalent to `mcp_notify`. REST/onion still serves web clients, so this is a
narrow loss for the local-agent session tooling only.

---

## 6. Acceptance per wave

- **W0:** for each bridged leaf, `zclassic23 <leaf>` returns a body byte-identical
  to today's MCP-routed body, with an assertion that the MCP router was **not**
  entered; `zclassic23 ops selftest` is green; `make deploy-dev` verify runs with
  zero `mcpcall`; `test_parallel` green.
- **W1:** one real dev-lane hot swap end-to-end through the native path (edit a
  native handler TU → `dev hotswap apply` → live handler re-pointed, observed by a
  probe leaf) while the MCP swap path still passes; `hotswap_simnet` +
  `golden_dev_cycle` green.
- **W2:** `golden_dev_cycle` green through the native probe/commit; all migrated
  asserts green; docs + gates reference native, not MCP; `.mcp.json` gone.
- **W3:** full build + `test_parallel` green; `golden_dev_cycle` green (native
  only); `grep -rn mcp --include='*.c' --include='*.h' .` returns only comments or
  historical strings.
