# W2 Consumer Migration Worklist — ZERO-MCP Removal

Source: 4 sweep-agent raw findings, deduped by file+line (114 unique entries,
no exact duplicates found across the 4 sweeps). Cross-checked against
`docs/work/MCP-REMOVAL-PLAN.md` §1.3 (dev-loop/deploy consumers), §1.4
(native-parity map), §1.5 (tests), §1.6 (config/docs), §1.7 (lint
gates/impact rules) — plus §1.1/§1.2/§3 consulted for context where a raw
entry is core-delete or hot-swap-redesign material rather than a pure
"consumer".

**in-plan? legend:**
- `YES §x.y` — this exact file/line (or the named function/target/line-range)
  is explicitly cited in the plan section shown.
- `partial §x.y` — the *file* or *feature* is named in the plan section, but
  this specific line is not itemized (often because the plan's cited line
  numbers have drifted from current source, or the plan cites a range that
  doesn't cover this exact call site).
- `NEW` — not mentioned anywhere in §1.1–§1.7. A genuine gap the sweep found
  that the plan's inventory missed.

**wave legend:** W1 = hot-swap re-target (§3), W2 = consumer migration
(scripts/tests/docs rewritten to native before MCP dies), W3 = pure delete
(dies when `tools/mcp/**` + `-mcp` + `mcpcall` are removed, no native
replacement needed).

---

## Wave W1 — hot-swap re-target consumers (§1.2 / §3)

| file:line | surface | kind | replacement | wave | in-plan? |
|---|---|---|---|---|---|
| Makefile:511 | `zcl_agent_hotswap` MCP tool (comment) | doc-reference | update comment to reference `dev.hotswap.apply` native command | W1 | partial §3.5 |
| Makefile:582 | `zcl_agent_hotswap` and `dev_hotswap` (comment) | doc-reference | update comment to reference native handler snapshot + native commit RPC | W1 | partial §3.5 |
| Makefile:2437 | `agent-mcp-call-dev` target | invocation | redirect to `zclassic23-dev dev hotswap apply` (lines 2437-2441) | W1 | YES §1.3 |
| lib/test/src/test_hotswap_loader.c:109 | `provider_id = "mcp.routes"` | config | change to `provider_id = "native.leaves"` in fixture + `config/hotswap_eligible.def` | W1 | YES §1.5 + §3.2 |
| lib/test/src/test_hotswap_loader.c:297 | `mcp_dev_hotswap_probe_allowed()` calls | invocation | replace with native handler probe validation logic | W1 | partial §3.4 |
| lib/test/src/test_hotswap_loader.c:320 | `mcp_dev_hotswap_probe_body_ok()` calls | invocation | replace with native `zcl.result.v1` envelope validation | W1 | YES §3.4 (named verbatim) |
| lib/test/src/test_hotswap_loader.c:351 | `mcp_router_reset/register/find/replace` | invocation | replace with `command_registry_*` equivalents | W1 | YES §1.2 + §3.3 |
| lib/test/src/test_hotswap_simnet.c:50 | `mcp_tool_route` struct + dispatch | invocation | replace with native handler function pointers | W1 | partial §1.2/§3 |
| lib/test/src/test_hotswap_simnet.c:306 | `mcp_router_replace_batch` / `active_generation` | invocation | replace with `command_registry_replace_batch` | W1 | YES §1.2 (verbatim) + §3.3 |
| config/hotswap_eligible.def:37 | `HOTSWAP_PROBE("zcl_name_list")` from `app_controller.c` | config | delete after W0 creates native `app_native_handlers.c` | W1 | YES §1.2 (verbatim) |
| config/hotswap_eligible.def:49 | `HOTSWAP_PROBE("zcl_tools_list")` from `meta_controller.c` | config | delete after W0 creates native `meta_native_handlers.c` | W1 | YES §1.2 (verbatim) |
| config/hotswap_eligible.def:52 | `HOTSWAP_PROBE("zcl_getblockchaininfo")` from `chain_controller.c` | config | delete after W0 creates native `chain_native_handlers.c` | W1 | YES §1.2 (verbatim) |
| config/hotswap_eligible.def:54 | `HOTSWAP_PROBE("zcl_networkinfo")` from `net_controller.c` | config | delete after W0 creates native `net_native_handlers.c` | W1 | YES §1.2 (verbatim) |
| config/hotswap_eligible.def:59 | `HOTSWAP_PROBE("zcl_balance")` from `wallet_controller.c` | config | delete after W0 creates native `wallet_native_handlers.c` | W1 | YES §1.2 (verbatim) |
| config/src/boot_services.c:1224 | `register_dev_mcp_rpc_commands()` call | invocation | delete; remove resident dev-RPC MCP bootstrap | W1 | YES §1.2 (function named verbatim) |

## Wave W2 — consumer migration (scripts / tests / docs / configs → native)

| file:line | surface | kind | replacement | wave | in-plan? |
|---|---|---|---|---|---|
| tools/agent_fast_ci.sh:633 | `make agent-mcp-call TOOL=<tool>` | doc-reference | native leaf equivalents; update `mcp_shortcuts` JSON docs | W2 | YES §1.3 |
| tools/agent_fast_ci.sh:636 | `make agent-mcp-call-hot TOOL=<tool>` | doc-reference | no hot-swap equivalent without MCP; fall back to full rebuild / native leaf | W2 | YES §1.3 |
| tools/agent_fast_ci.sh:639 | `make agent-mcp-call-dev TOOL=<tool>` | doc-reference | `zclassic23-dev dev hotswap apply <artifact> <probe_leaf>` | W2 | YES §1.3 |
| lib/test/src/test_mcp_controllers.c:53 | `EXPECTED_TOTAL 138` | assertion | migrate per-domain tool counts to native leaf-path registry asserts | W2 | YES §1.5 (verbatim, `:53`) |
| lib/test/src/test_syncdiag_rpc.c:1141 | `mcp_tool` field assertion | assertion | assert `native_tool`; `zcl_syncdiag` → `core.sync.diagnose` | W2 | YES §1.5 |
| lib/test/src/test_syncdiag_rpc.c:2495 | `zcl_agent_map` tool assertion | assertion | assert `dev.agent.map` native leaf path | W2 | YES §1.5 |
| lib/test/src/test_syncdiag_rpc.c:5138 | `mcp_changed` field assertion | assertion | rewrite as `native_handlers_changed` / `handler_generation_changed` | W2 | YES §1.5 (verbatim) |
| lib/test/src/test_syncdiag_rpc.c:5147 | `mcp_controllers` group assertion | assertion | change to `native_handlers` / `command_registry` group | W2 | YES §1.5 (verbatim) |
| lib/test/src/test_syncdiag_rpc.c:5231 | `mcp_declared_count` field | assertion | rewrite as native leaf-path count assertion | W2 | YES §1.5 (verbatim, `mcp_declared_count >= 20`) |
| lib/test/src/test_make_lint_gates.c:3089 | `mcpcall` string assertion | assertion | change to `zclassic23 core` / native command path assertion | W2 | YES §1.5 (verbatim) |
| lib/test/src/test_make_lint_gates.c:3113 | `cli_run_mcp_call` assertion | assertion | delete; removed in W3 `main.c` | W2 | YES §1.5 (verbatim) |
| lib/test/src/test_make_lint_gates.c:3114 | `mcp_register_ops` assertion | assertion | delete; removed in W3 `main.c` | W2 | YES §1.5 (verbatim) |
| lib/test/src/test_make_lint_gates.c:3158 | `mcp_shortcuts` JSON assertion | assertion | delete; removed from `agent_fast_ci.sh` | W2 | YES §1.5/§1.3 |
| lib/test/src/test_make_lint_gates.c:3258 | `mcp_controllers` in `agent_impact_rules.def` | assertion | change to `native_handlers` / `command_registry` | W2 | YES §1.5 + §1.7 (verbatim) |
| lib/test/src/test_secrets_hygiene.c:157 | `mcp_router_tools_list_json()` | invocation | replace with `command_registry_list_json()` or native equivalent | W2 | **NEW** — not named in §1.5 |
| lib/test/src/test_api.c:2833 | `mcp_tool` field assertions on operations | assertion | change to `native_tool` or delete | W2 | **NEW** — `test_api.c` absent from §1.5 |
| lib/test/src/test_api.c:3024 | `mcp_callable_count` field | assertion | change to `native_callable_count` or equivalent | W2 | **NEW** — same file gap |
| lib/test/src/test_api.c:4803 | `mcp_controllers` group in API contract | assertion | change to `native_handlers` / `command_registry` group | W2 | **NEW** — same file gap |
| docs/GETTING_STARTED.md:137 | `claude mcp add zcl23 -- build/bin/zclassic23 -mcp` | config | delete entirely; native command registry replaces MCP setup | W2 | YES §1.6 (verbatim) |
| README.md:251 | `claude mcp add zcl23 -- build/bin/zclassic23 -mcp` | config | delete; replace "Claude integration (MCP)" section with native examples | W2 | **NEW** — README.md not named in §1.6's 3-doc list (BUILD.md/GETTING_STARTED.md/CLAUDE.md only) |
| CLAUDE.md:130 | `claude mcp add zcl23 -- build/bin/zclassic23 -mcp` | config | delete entire MCP Server section (~lines 123-290) | W2 | YES §1.6 (verbatim) |
| CLAUDE.md:137 | `claude mcp add ... -datadir=/path` | config | delete; covered by line-130 section deletion | W2 | YES §1.6 |
| docs/BUILD.md:43 | `make agent-mcp-call TOOL=zcl_tools_list` | build-glob | `zclassic23 discover help` | W2 | **NEW** — BUILD.md named only for `claude mcp add`, not agent-mcp-call examples |
| docs/BUILD.md:44 | `make agent-mcp-call-hot TOOL=zcl_status` | build-glob | `zclassic23 status` / `zclassic23 core status` | W2 | **NEW** |
| docs/BUILD.md:45 | `make agent-mcp-call-dev TOOL=zcl_status` | build-glob | `zclassic23-dev status` | W2 | **NEW** |
| docs/GETTING_STARTED.md:61 | `make agent-mcp-call-hot TOOL=zcl_status` | build-glob | `zclassic23 status` | W2 | **NEW** |
| docs/GETTING_STARTED.md:62 | `make agent-mcp-call-dev TOOL=zcl_status` | build-glob | `zclassic23-dev status` | W2 | **NEW** |
| docs/AGENT_ARCHITECTURE.md:80 | `make agent-mcp-call TOOL=<tool>` | doc-reference | delete; native command registry replacement | W2 | partial §1.6 (doc named, line not itemized) |
| docs/CODEBASE_MAP.md:581 | `make agent-mcp-call TOOL=<tool>` | doc-reference | delete; native command registry replacement | W2 | partial §1.6 (plan cites `:569`, drifted) |
| docs/AGENT_API.md:29 | `make agent-mcp-call TOOL=zcl_status` | build-glob | `zclassic23 status` | W2 | partial §1.6 (doc named generally) |
| docs/AGENT_API.md:30 | `make agent-mcp-call-dev TOOL=zcl_status` | build-glob | `zclassic23-dev status` | W2 | partial §1.6 |
| docs/AGENT_TRAPS.md:65 | `mcpcall zcl_agent_hotswap` | invocation | `zclassic23-dev dev hotswap apply <artifact> <probe_leaf>` (W1+) | W2 | partial §1.6 (doc named, no line) |
| docs/AGENT_ARCHITECTURE.md:83 | `zclassic23 mcpcall <tool> [json]` | invocation | `zclassic23 <leaf> [--input=json]` | W2 | partial §1.6 |
| docs/CODEBASE_MAP.md:78 | `mcp/ (MCP server, controllers)` | doc-reference | delete; `tools/mcp/` removed W3, handlers re-homed to `app/controllers/` | W2 | YES §1.6 (verbatim `:78`) |
| docs/CODEBASE_MAP.md:124 | `zclassic23 mcpcall <tool> [json]` | invocation | `zclassic23 <leaf> [--input=json]` | W2 | partial §1.6 (plan cites `:123`, off-by-one drift) |
| docs/CODEBASE_MAP.md:191 | `zcl_tools_list` | invocation | `zclassic23 discover help` / `discover search` | W2 | **NEW** — not in plan's `:78,91-92,123,150,569` list |
| docs/CODEBASE_MAP.md:334 | `typed mcpcall` | doc-reference | delete; native command registry replaces | W2 | **NEW** |
| docs/CODEBASE_MAP.md:584 | `build/bin/zclassic23 mcpcall <tool> [json]` | invocation | `zclassic23 <leaf> [--input=json]` | W2 | partial §1.6 (plan cites `:569`, drifted) |
| docs/AGENT_API.md:1153 | `mcpcall zcl_agent_hotswap` | invocation | `zclassic23-dev dev hotswap apply <artifact> <probe_leaf>` (W1+) | W2 | partial §1.6 + §3.5 |
| docs/HANDOFF.md:216 | `zclassic23 mcpcall zcl_status` | invocation | `zclassic23 status` | W2 | **NEW** — `docs/HANDOFF.md` not in §1.6 inventory at all |
| docs/work/fast-path.md:81 | `make agent-mcp-call-hot TOOL=<tool>` | build-glob | `zclassic23 <leaf>` | W2 | **NEW** — file absent from §1.6 |
| docs/work/fast-path.md:173 | `build/bin/zclassic23 mcpcall zcl_status` | invocation | `zclassic23 status` | W2 | **NEW** |
| (removed) docs/work/archive/FUTURE-CLAUDE-FIX-THIS.md:552 | `-mcp` (documented default) | doc-reference | file removed from tree — no longer live inventory; recover with `git log --follow -- docs/work/archive/FUTURE-CLAUDE-FIX-THIS.md` | closed | site no longer in tree |
| README.md:10 | "a built-in MCP server" | doc-reference | "native command registry" | W2 | **NEW** — README.md entirely absent from §1.6 |
| README.md:244 | `## Claude integration (MCP)` | doc-reference | delete entire section; native command examples | W2 | **NEW** |
| README.md:302 | `└── MCP ~100 typed tools on stdio (-mcp)` | doc-reference | "Native command registry ~100 commands" | W2 | **NEW** |
| CLAUDE.md:5 | "an MCP server. 100+ typed MCP tools" | doc-reference | "native command registry. 100+ native commands" | W2 | partial §1.6 (generic "CLAUDE.md MCP sections (large)") |
| CLAUDE.md:7 | "MCP/native surfaces" | doc-reference | delete MCP reference; keep "native surfaces" | W2 | partial §1.6 |
| CLAUDE.md:118 | "Every MCP handler must set an error body" | doc-reference | reframe as "native command handler" rule | W2 | partial §1.6 |
| CLAUDE.md:179 | `zcl_tools_list` | invocation | `zclassic23 discover help` | W2 | partial §1.6 |
| CLAUDE.md:287 | `-mcp` flag | doc-reference | delete; `-mcp` flag removed W3 | W2 | partial §1.6 |
| docs/AGENT_API.md:264 | `make agent-mcp-call*` | doc-reference | delete; native command registry replacement | W2 | partial §1.6 (doc named generally) |
| .mcp.json:1 | `zcl23` MCP server config | config | delete entire file | W2 | YES §1.1 + §1.6, wave table explicit "delete `.mcp.json`" under W2 |

## Wave W3 — pure delete (dies with `tools/mcp/**` removal, no migration)

| file:line | surface | kind | replacement | wave | in-plan? |
|---|---|---|---|---|---|
| tools/agent_fast_ci.sh:213 | `tools/mcp_server.c` | build-glob | delete with W3; remove from impact detection pattern matching | W3 | YES §1.3 |
| tools/agent_fast_ci.sh:215 | `tools/mcp/*.c tools/mcp/controllers/*.c tools/mcp/views/*.c` | build-glob | delete with W3; remove globs from impact source classification | W3 | YES §1.3 |
| Makefile:174 | `tools/mcp_server.c` in `NODE_ENTRY_SRCS` | build-glob | remove from `NODE_ENTRY_SRCS`; delete the build line | W3 | YES §1.1 (`:174` verbatim) + §1.7 |
| Makefile:282 | `docs-mcp docs-mcp-check` (.PHONY list) | config | remove from `.PHONY` declaration | W3 | YES §1.7 |
| Makefile:450 | `agent-mcp-call agent-mcp-call-hot agent-mcp-call-dev` (.PHONY list) | config | remove from `.PHONY` declaration | W3 | YES §1.3 |
| Makefile:1646 | `-mcp` binary flag (comment, near `test_mcp_e2e`) | doc-reference | remove/update comment; `test_mcp_e2e` itself deleted | W3 | YES §1.5 (`test_mcp_e2e` named) |
| Makefile:2407 | `agent-mcp-call` target (lines 2407-2432) | invocation | delete entire target | W3 | YES §1.3 |
| Makefile:2432 | `mcpcall` command invocation | invocation | deleted with target | W3 | YES §1.3 |
| Makefile:2434 | `agent-mcp-call-hot` target (lines 2434-2435) | invocation | delete entire target | W3 | YES §1.3 |
| Makefile:2711 | `docs-mcp` target def (lines 2711-2716) | invocation | delete; `MCP_REFERENCE.md` becomes obsolete | W3 | YES §1.6 (`MCP_REFERENCE.md`) + §1.7 |
| Makefile:2714 | `-mcp` flag invocation | invocation | deleted with `docs-mcp` target | W3 | YES §1.1/§1.7 |
| Makefile:2718 | `docs-mcp-check` target def (lines 2718-2731) | invocation | delete; CI gate becomes obsolete | W3 | YES §1.7 |
| Makefile:2722 | `-mcp` flag invocation | invocation | deleted with `docs-mcp-check` target | W3 | YES §1.7 |
| lib/test/src/test.c:1112 | `test_mcp_router()` | invocation | delete; entire `test_mcp_router.c` deleted | W3 | YES §1.5 (verbatim) |
| lib/test/src/test.c:1113 | `test_dev_mcp_rpc_bridge()` | invocation | delete; `test_dev_mcp_rpc_bridge.c` deleted; dev hotswap uses native RPC | W3 | YES §1.5 (verbatim) |
| lib/test/src/test.c:1118 | `test_mcp_controllers()` | invocation | delete; migrate to native command registry contract tests | W3 | YES §1.5 (verbatim) |
| lib/test/src/test.c:1119 | `test_mcp_middleware()` | invocation | delete; `test_mcp_middleware.c` deleted; native CLI has no middleware layer | W3 | YES §1.5 (verbatim) |
| lib/test/src/test.c:1120 | `test_mcp_metrics()` | invocation | delete; `test_mcp_metrics.c` deleted | W3 | YES §1.5 (verbatim) |
| lib/test/src/test.c:1121 | `test_mcp_baseline()` | invocation | delete; `test_mcp_baseline.c` deleted | W3 | YES §1.5 (verbatim) |
| lib/test/src/test_dev_mcp_rpc_bridge.c:2 | whole file | invocation | delete entire file | W3 | YES §1.5 |
| lib/test/src/test_mcp_router.c:1 | whole file | invocation | delete entire file; native `command_registry` atomic handler snapshot replaces | W3 | YES §1.5 |
| lib/test/src/test_hotswap_loader.c:109 → see W1 | (moved) | — | — | — | — |
| lib/test/src/test_parallel.c:86 | `X(mcp_router) X(mcp_controllers) X(mcp_middleware) X(mcp_metrics) X(mcp_baseline)` | invocation | remove `mcp_*` test names; keep `hotswap_loader`/`hotswap_simnet` (native-migrated) | W3 | YES §1.5 (all 5 test names cited) |
| lib/test/src/test_metric_alerts.c:17 | `#include "mcp/mcp_notify.h"` | config | delete; `mcp_notify.h` removed | W3 | **NEW** — `test_metric_alerts.c` not named anywhere in §1.1-§1.7 (only `test_mcp_notify` and `mcp_notify.c/.h` themselves are named) |
| lib/test/src/test_metric_alerts.c:104 | `mcp_notify_is_operator_event()` calls | invocation | delete; push/notify capability removed (**residual flag loss** — flagged by sweep) | W3 | **NEW** — functional regression not called out in plan |
| src/main.c:60 | 4 `#include "mcp/*.h"` lines | invocation | delete 4 MCP header includes | W3 | YES §1.1 (verbatim `main.c:60-63`) |
| src/main.c:1282 | `cli_mcp_register_all()` (calls `mcp_register_*` for 8 domains) | invocation | delete entire function body | W3 | YES §1.1 (verbatim `main.c:1282-1383`) |
| src/main.c:1330 | `cli_run_mcp_call(datadir, params, nparams)` | invocation | delete entire function | W3 | YES §1.1 (within cited range) |
| src/main.c:1529 | `mcpcall`/`mcp` CLI dispatch branch | invocation | delete conditional branch from CLI routing | W3 | YES §1.1 (verbatim `main.c:1529-1530`) |
| src/main.c:1639 | `extern int mcp_server_main_inprocess(...)` | invocation | delete declaration | W3 | YES §1.1 (verbatim `main.c:1637-1665`) |
| src/main.c:1645 | `mcp_inprocess_thread_main(arg)` | invocation | delete entire function | W3 | YES §1.1 (within cited range) |
| src/main.c:2666 | `-mcp` flag branch → `mcp_server_main(...)` | invocation | delete branch (lines 2665-2676); remove out-of-process server | W3 | partial §1.1 — feature named, but plan's cited range (`1637-1665`) does not cover this call site; **line-drift risk** |
| src/main.c:2735 | `bool mcp_inprocess = false;` | config | delete variable declaration | W3 | partial §1.1 — same drift as above |
| src/main.c:2849 | `-mcp-inprocess` flag parsing | invocation | delete else-if branch | W3 | partial §1.1 — same drift |
| src/main.c:3063 | `pthread_create(&mcp_thread, ..., mcp_inprocess_thread_main, ...)` | invocation | delete if-block; remove in-process thread launch | W3 | partial §1.1 — same drift |
| config/commands/apps.def:34 | `ZCL_COMMAND_TRANSPORT_MCP_LEGACY` flag on `app.protocols` | config | remove from transport bitmask; keep `NATIVE\|RPC` only | W3 | partial §1.7 (generic "catalog flags", not itemized by file) |
| config/commands/root.def:12 | `ZCL_COMMAND_TRANSPORT_MCP_LEGACY` on `status` root command | config | remove from transport bitmask | W3 | partial §1.7 |
| config/commands/root.def:38 | `ZCL_COMMAND_TRANSPORT_MCP_COMPACT` on `discover.help` | config | remove flag; keep `NATIVE` only | W3 | partial §1.7 |
| config/commands/root.def:53 | `ZCL_COMMAND_TRANSPORT_MCP_COMPACT` on `discover.search` | config | remove flag | W3 | partial §1.7 |
| config/commands/root.def:68 | `ZCL_COMMAND_TRANSPORT_MCP_COMPACT` on `discover.describe` | config | remove flag | W3 | partial §1.7 |
| config/commands/root.def:83 | `ZCL_COMMAND_TRANSPORT_MCP_COMPACT` on `discover.schema` | config | remove flag | W3 | partial §1.7 |
| config/src/boot_services.c:99 | `#include "mcp/dev_rpc_bridge.h"` | invocation | delete include | W3 | **NEW** — `boot_services.c` as a consumer site not named (only `dev_rpc_bridge.*` itself, in §1.1/§1.2) |
| config/src/boot_services.c:140 | `#include "mcp/metrics.h"` | invocation | delete include | W3 | **NEW** — same gap |
| config/src/boot_services.c:1246 | `mcp_metrics_init()` call | invocation | delete call; Prometheus metrics init moves to native layer | W3 | **NEW** — same gap; boot-path build-break risk if missed |
| lib/kernel/include/kernel/command_registry.h:167 | `ZCL_COMMAND_TRANSPORT_MCP_COMPACT = 1U << 2` | config | delete enum member; renumber `RPC` flag `<<3 → <<2` | W3 | YES §1.7 (wave-table bullet "drop `TRANSPORT_MCP_*` enum") |
| lib/kernel/include/kernel/command_registry.h:168 | `ZCL_COMMAND_TRANSPORT_MCP_LEGACY = 1U << 3` | config | delete enum member | W3 | YES §1.7 |
| tools/mcp_server.c:1 | entire file (196 lines): `mcp_server_main`, `mcp_server_main_inprocess` | invocation | delete entire file | W3 | YES §1.1 (verbatim) |

---

## Summary

- **Total unique consumers:** 114 (deduped by file+line across the 4 sweeps —
  no exact file+line duplicates were found; the 4 sweeps had already
  partitioned cleanly by area).
- **Wave split:** W1 (hot-swap re-target) = 15, W2 (consumer migration) = 47,
  W3 (pure delete) = 52.
- **In-plan (`YES`, exact citation):** 55
- **In-plan (`partial` — file/feature named, this line/site not itemized,
  mostly doc line-number drift):** 32
- **`NEW` (not mentioned anywhere in §1.1–§1.7):** 27

### The 27 NEW findings, by risk

**High risk (functional/build-break, not just doc drift):**
1. `lib/test/src/test_metric_alerts.c:17,104` — `mcp_notify_is_operator_event()`
   / `#include "mcp/mcp_notify.h"`. The plan lists `mcp_notify.c/.h` and
   `test_mcp_notify` as dying, but never traces the consumer inside
   `test_metric_alerts.c`. The sweep itself flags this as "residual flag
   loss" — deleting `mcp_notify.*` without touching this file either breaks
   the build or silently drops operator-notify test coverage.
2. `config/src/boot_services.c:99,140,1246` — boot-path includes
   `mcp/dev_rpc_bridge.h` and `mcp/metrics.h`, and calls `mcp_metrics_init()`.
   The plan names the dying headers/functions in §1.1/§1.2 but never cites
   `boot_services.c` as a call site. Missing this means a build break at
   node boot wiring, not just in a test or doc.
3. `lib/test/src/test_api.c:2833,3024,4803` — `mcp_tool`, `mcp_callable_count`,
   `mcp_controllers` API-contract assertions. `test_api.c` is never named in
   §1.5's test-migration list (only `test_syncdiag_rpc.c` is cited for
   `mcp_tool`-style asserts). This is a full API contract test file the plan
   would leave stranded.
4. `README.md:10,244,251,302` — the top-level README's entire "Claude
   integration (MCP)" section, architecture diagram line, and intro
   sentence are completely absent from §1.6's doc inventory (which lists
   only `BUILD.md`, `GETTING_STARTED.md`, `CLAUDE.md`,
   `CODEBASE_MAP.md`, `AGENT_TRAPS.md`, `HOTSWAP.md`, `AGENT_API.md`,
   `AGENT_ARCHITECTURE.md`, `MCP_REFERENCE.md`). Highest-visibility doc,
   fully missed.
5. `lib/test/src/test_secrets_hygiene.c:157` — `mcp_router_tools_list_json()`,
   not named in §1.5.

**Lower risk (doc-only, agent-mcp-call examples the plan's §1.6 doesn't
itemize even though it names the broader `make agent-mcp-call*` target
elsewhere): `docs/BUILD.md:43-45`, `docs/GETTING_STARTED.md:61-62`,
`docs/CODEBASE_MAP.md:191,334`, `docs/HANDOFF.md:216`,
`docs/work/fast-path.md:81,173`; the former `docs/work/archive/FUTURE-CLAUDE-FIX-THIS.md:552`
site is removed from the tree (`git log --follow -- docs/work/archive/FUTURE-CLAUDE-FIX-THIS.md`).

### Line-drift flag (not "new" but worth a plan refresh)

Several §1.1/§1.6 citations no longer match current line numbers:
`docs/CODEBASE_MAP.md` (plan cites `:78,91-92,123,150,569`; live sweep found
`:78,124,191,334,584` — `:78` matches exactly, the rest drifted by 1-15
lines), and `src/main.c` (plan cites `-mcp`/`-mcp-inprocess` run modes at
`:1637-1665`; the live sweep found the actual flag-parsing/thread-launch code
at `:2666,2735,2849,3063` — a completely different region, suggesting either
stale citation or a second, uncited code path for the same feature).

## The 5 riskiest migrations

1. **`README.md` (lines 10, 244, 251, 302)** — the single most-read doc in
   the repo has zero coverage in the plan's §1.6 inventory. Anyone executing
   the plan as written ships zero-MCP with the README still telling users to
   `claude mcp add zcl23 -- build/bin/zclassic23 -mcp`.
2. **`test_metric_alerts.c:17,104` (`mcp_notify_is_operator_event`)** — a
   genuine functional-regression risk flagged by the sweep itself
   ("residual flag loss"): operator-notify capability could be silently
   dropped because the plan never traces this consumer of `mcp_notify.h`.
3. **`src/main.c:2666-3063`** — the plan's own line citation
   (`main.c:1637-1665`) for `-mcp`/`-mcp-inprocess` doesn't match where the
   live sweep found the actual flag-handling/thread-launch code
   (`:2666,2735,2849,3063`). Whoever does the W3 delete off the plan's line
   numbers alone will miss this region and leave a dead/dangling `-mcp`
   code path.
4. **`test_api.c:2833,3024,4803`** — a whole API-contract test file with
   `mcp_tool`/`mcp_callable_count`/`mcp_controllers` assertions that §1.5
   never names (it only names `test_syncdiag_rpc.c` for this assertion
   shape). Skipping W2 migration here means `test_api` red-lines the moment
   W3 deletes the MCP surface it asserts against.
5. **`config/src/boot_services.c:99,140,1224,1246`** — boot-path consumer of
   `mcp/dev_rpc_bridge.h`, `mcp/metrics.h`, `register_dev_mcp_rpc_commands()`
   and `mcp_metrics_init()`. Unlike doc/test gaps, this one breaks the build
   at node-boot wiring if the W1/W3 deletion doesn't also touch this file —
   the plan cites the dying headers/functions but never this call site.
