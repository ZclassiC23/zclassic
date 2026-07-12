# Session handoff — 2026-07-11 (Zero-MCP W1-B/C: hot-swap re-target)

**Read this with:** `docs/work/SESSION-HANDOFF-2026-07-11-ZERO-MCP.md` (W0+W1-A
handoff this wave builds on), `docs/work/MCP-REMOVAL-PLAN.md` §3.2-3.5 (the
W1-B/C spec this wave executes), `docs/work/MCP-REMOVAL-WORKLIST.md` (the
authoritative W2/W3 site inventory), and `docs/work/HOTSWAP.md` §"`native.leaves`
provider (Zero-MCP re-target, Wave W1-B/C)" (the mechanism doc, updated this
session).

**Status: landed in worktree `~/github/zclassic23-w1bc`
(branch `dev/zero-mcp-w1bc`), NOT YET COMMITTED, NOT YET MERGED to main.**
This handoff documents the diff sitting in that worktree so the next session
(or the orchestrator) can review, gate, commit, and merge it.

## What W1-B/C landed

Tier-1 in-process hot-swap was **re-targeted from the MCP router onto the
native command registry**, so it survives the coming MCP deletion (W3). Prior
to this wave, `zcl_agent_hotswap` (MCP tool) was the only way to hot-swap a
running dev node's handlers. Now there are two provider classes, dual-run:

- **`mcp.routes`** (unchanged, pre-existing) — re-points routes in the
  resident MCP router.
- **`native.leaves`** (new, this wave) — re-points leaf handlers in the
  kernel command registry via `zcl_command_registry_replace_batch` (atomic,
  generation-monotonic, lock-free readers) instead of the MCP router.

### New symbols and files

- **`lib/hotswap/include/hotswap/hotswap.h`** — v3 host alongside the
  existing v2 host (same manifest schema `ZCL_HOTSWAP_MANIFEST_SCHEMA_V2`):
  `ZCL_HOTSWAP_HOST_ABI_V3`, `ZCL_HOTSWAP_CAP_LEAF_STAGE`,
  `ZCL_HOTSWAP_V3_HOST_CAPABILITIES`; `struct zcl_hotswap_host` gained one
  field appended past `mcp_stage` — `leaf_stage(path, handler)`. The v2 size
  is frozen as `ZCL_HOTSWAP_HOST_STRUCT_SIZE_V2 =
  offsetof(struct zcl_hotswap_host, leaf_stage)` (not `sizeof(...)`), so an
  already-compiled `mcp.routes` generation is unaffected by the struct
  growing. New: `struct zcl_hotswap_leaf_replacement`,
  `zcl_hotswap_leaf_commit_cb`, `hotswap_load_leaves()`,
  `ZCL_HOTSWAP_EXPORT_LEAVES(leaves_arr, leaves_count)`. Both `EXPORT_ROUTES`
  and `EXPORT_LEAVES` now expand through one shared
  `ZCL_HOTSWAP_EXPORT_MANIFEST(host_abi_, host_size_, required_caps_,
  provider_id_, probe_csv_)` body so the v2/v3 manifest emitters cannot
  drift apart.
- **`lib/hotswap/src/hotswap_loader.c`** — `hotswap_load_leaves()`: same
  fail-closed precheck/dlopen/hash/manifest-validate core as `hotswap_load()`,
  diverging only in staging leaves via the v3 host and invoking a
  `zcl_hotswap_leaf_commit_cb`. Dev-only (refuses with "unavailable", no
  `dlopen`, outside `ZCL_DEV_BUILD`).
- **`lib/kernel/src/command_registry.c`** — `zcl_command_registry_replace_batch`
  gained a precheck: a batch entry naming a **branch** path
  (`mode == ZCL_COMMAND_MODE_BRANCH`, no handler) is now rejected with
  `"leaf '%s' is a branch, not swappable"`. (Carry-forward gap from the W1-A
  verifier — see "Traps" below.)
- **`tools/command/native_command.c` / `.h`** — new seam
  `zcl_native_bridge_run(request, body, reply)`: the reusable core of
  `zcl_native_bridge_command()` with an EXPLICIT `body` function parameter.
  `zcl_native_bridge_command()` is now a thin wrapper:
  `zcl_native_bridge_run(request, zcl_native_bridge_body_for_path(path),
  reply)`. A hot-swap generation's trampoline supplies its OWN
  freshly-compiled body instead.
- **Per-TU trampolines** under `#ifdef ZCL_HOTSWAP_GEN` at the bottom of five
  `app/controllers/src/*_native_handlers.c` files (status, wallet, net, meta,
  chain): each defines `static void tramp_X(request, reply) { zcl_native_bridge_run(request,
  zcl_native_X_body, reply); }` for every leaf it owns, a
  `k_leaves[]` table of `{dotted_path, tramp_X}` pairs, and
  `ZCL_HOTSWAP_EXPORT_LEAVES(k_leaves, ...)`. Expands to nothing in a
  node/release build (only compiles under a generation `-DZCL_HOTSWAP_GEN`
  build).
- **`tools/command/native_dev_hotswap.c` / `.h`** (new files, untracked) —
  the MCP-free successor of
  `tools/mcp/controllers/dev_hotswap_controller.c` +
  `tools/mcp/dev_rpc_bridge.c`'s `rpc_dev_hotswap`. Defines:
  - `zcl_native_handle_dev_hotswap_apply` /
    `zcl_native_handle_dev_hotswap_probe` — CLI handlers for the two new
    leaves (declared in `native_command.h`, bound in `dev.def`). Both are
    short-lived-process handlers that forward a JSON-RPC request
    (`dev_hotswap_native`) to the already-running resident dev node.
  - `rpc_dev_hotswap_native()` — the resident-node RPC actor; runs
    `hotswap_load_leaves()` and the commit/probe callback in-process.
  - `hotswap_commit_native_leaves()` — apply path: stages the batch, calls
    `zcl_command_registry_replace_batch()`. Has a `TODO(W1)` (see below).
  - `hotswap_probe_native_leaves()` — probe path: validates the staged batch
    shape and returns success WITHOUT ever calling `replace_batch()`.
  - `register_dev_native_hotswap_rpc()` — registers `dev_hotswap_native` on
    the resident RPC table; dev-only, lane-guarded to the exact dev datadir,
    successful no-op elsewhere.
- **`config/commands/dev.def`** — new branch `dev.hotswap` with leaves
  `dev.hotswap.apply` (mutating, `ZCL_COMMAND_RISK_DEV_MUTATION`,
  `ZCL_COMMAND_AUTH_OWNER`) and `dev.hotswap.probe` (read-only,
  `ZCL_COMMAND_TRAIT_DRY_RUN`).
- **`config/hotswap_eligible.def`** — five new `native.leaves` eligibility
  rows, one per `*_native_handlers.c` TU, each mapped to its `mcp.routes`
  sibling and given a dotted-path probe leaf:

  | `native.leaves` TU | Probe leaf |
  |---|---|
  | `status_native_handlers.c` | `core.status` |
  | `wallet_native_handlers.c` | `core.wallet.address.list` |
  | `net_native_handlers.c` | `core.network.peers.incidents` |
  | `meta_native_handlers.c` | `ops.metrics` |
  | `chain_native_handlers.c` | `core.consensus.utxo.audit` |

  `diagnostics_native_handlers.c` is **eligible-pending, not listed**: both
  its leaves (`core.storage.query`, `ops.logs`) require a non-empty argument
  and reject an empty-args self-test dispatch — see the empty-args rule
  below.
- **`tools/lint/check_hotswap_eligible_scope.sh`** — the probe-format check
  now accepts `.` (dotted command paths like `core.status`, not just mcp
  tool names like `zcl_status`); the export-macro check now accepts EITHER
  `ZCL_HOTSWAP_EXPORT_ROUTES` OR `ZCL_HOTSWAP_EXPORT_LEAVES`.
- **`Makefile`** (`hotswap-so` target) — plumbs a second macro
  `-DZCL_HOTSWAP_PROBE_LEAF=<probe>` alongside the existing
  `-DZCL_HOTSWAP_PROBE_TOOLS=<probe>` through the preprocessed-inputs digest
  and the generation compile, so `native.leaves` generations get a real
  probe-leaf default without touching the `mcp.routes` build path.
- **`tools/mcp/dev_rpc_bridge.c`** — `dev_bridge_initialize_router()` now
  calls `zcl_command_registry_set_active(zcl_command_catalog())` (the W1-A
  carry-forward gap — production binding of the active registry, previously
  left to this wave); `dev_bridge_register_impl()` calls
  `register_dev_native_hotswap_rpc()` to register the new resident RPC.

### Empty-args probe rule (load-bearing)

The resident native command router self-tests a `native.leaves` generation by
dispatching its manifest's declared probe leaf **with no input** before
committing the batch — mirroring the `mcp.routes` probe discipline. So the
probe for a native TU **must be a leaf whose body tolerates empty args**,
verified by reading the body function (and its backing RPC handler's
param-arity check), never assumed from the leaf name. This is why
`diagnostics_native_handlers.c` is not yet in `hotswap_eligible.def`: both its
leaves require a caller-supplied argument (`sql`, `pattern`) and fail closed
on empty args with a top-level RPC error, which would make the generation
self-test spuriously fail on every load.

## Test coverage

- **`test_hotswap_loader`** (`lib/test/src/test_hotswap_loader.c`) — new
  `test_hotswap_leaf_manifest_v3_contract()`: a `native.leaves` fixture
  manifest (`valid_leaf_manifest()`, mirroring the real
  `status_native_handlers.c` eligibility row) validates against the v3 host
  contract; a manifest carrying v2 abi/size/caps against the v3 provider ID
  is rejected (provider/caps mismatch, not a silent fallback); an unknown
  `provider_id` is rejected outright without checking ABI/caps.
- **`test_hotswap_simnet`** (`lib/test/src/test_hotswap_simnet.c`) — new
  `test_hotswap_native_leaf_repoint()`: builds a small fixture registry
  (mirroring `test_command_handler_snapshot`'s pattern), binds it active, and
  drives two successive `zcl_command_registry_replace_batch()` generations
  across the SAME leaf path — proves the atomic re-point, generation
  monotonicity (a `gen <= active` batch is rejected, publishing nothing), and
  new-call visibility, all WITHOUT touching `mcp/router.h`.
- **`test_command_handler_snapshot`** (`lib/test/src/test_command_handler_snapshot.c`)
  — new `test_reject_branch_leaf()`: a batch entry naming a branch path
  (`mode == ZCL_COMMAND_MODE_BRANCH`, `handler = NULL`) is rejected with
  `"branch"` + the path in the error message, and nothing is installed
  (generation stays 0, effective handler stays NULL).
- **`app/controllers/include/controllers/agent_impact_rules.def`** — new
  rule mapping `tools/command/native_dev_hotswap.{c,h}` +
  `config/commands/dev.def` + `config/hotswap_eligible.def` + `lib/hotswap/*`
  to `hotswap_loader hotswap_simnet command_handler_snapshot
  make_lint_gates`, so a pre-push edit to any of these files is required to
  run those groups.

None of the above have been run this session (no `make`/`make t` per the
task's constraints) — **run them before committing**:
`make t ONLY=hotswap_loader`, `make t ONLY=hotswap_simnet`,
`make t ONLY=command_handler_snapshot`, plus `make lint`.

## The E1 `boot_services.c` ratchet lesson

`register_dev_native_hotswap_rpc()` is called from
`tools/mcp/dev_rpc_bridge.c`'s `dev_bridge_register_impl()`, **not** from
`config/src/boot_services.c` — even though most boot-time RPC registration
lives there. Reason: `config/src/boot_services.c` is pinned at its
`check-file-size-ceiling` (E1) ratchet baseline —
`tools/lint/file_size_ceiling_baseline.txt` records it at **exactly 1634
lines**, which is its current line count today, i.e. **zero headroom**. The
E1 gate baseline may only shrink, never grow, and has no inline override, so
ANY net addition to `boot_services.c` fails `make lint` outright right now.
`dev_rpc_bridge.c` was already the natural home regardless (it registers the
sibling `dev_hotswap`/`dev_mcp_call` RPC methods), so the new registration
call landed there instead. **Generalizable lesson: before adding a
registration call to `boot_services.c`, check
`tools/lint/file_size_ceiling_baseline.txt` for its current ceiling — if
it's at/near the ceiling, find (or create) a sibling registration site
instead of trying to shrink boot_services.c as a side quest.**

## Two v1 TODOs (explicit, not hidden)

1. **Isolated precommit probe.** `hotswap_commit_native_leaves()`
   (`tools/command/native_dev_hotswap.c`) carries a `TODO(W1)`: it does not
   yet look up the probe leaf's spec from the active registry and invoke the
   candidate handler against an empty request BEFORE publish. Today's v1
   guards are the generation self-test (loader-side, dispatches the declared
   probe leaf through the resident router) plus
   `zcl_command_registry_replace_batch()`'s READY / read-only / non-branch /
   non-duplicate validation. A true isolated dry-run probe needs a new
   registry lookup+invoke API and is deferred rather than blocking the batch
   publish.
2. **CLI datadir/port resolution.** `hotswap_cli_ensure_rpc_client()`
   (`tools/command/native_dev_hotswap.c`) carries a `TODO(W1)`: the CLI
   resolves its resident-RPC target from fixed defaults
   (`$HOME/.zclassic-c23-dev`, port 18252) rather than threading the CLI's own
   resolved `-datadir`/`-rpcport` (today held in `native_command.c`'s
   `g_bridge_datadir`/`g_bridge_rpc_port` statics) through the request
   context. Safe today because the native hot-swap lane is fixed to the exact
   dev lane anyway (`hotswap_datadir_is_dev()` on the resident RPC side), but
   it means a CLI invoked against a differently-configured dev node (custom
   `-datadir`) will silently target the wrong node's RPC port instead of
   failing loud.

## Next steps (in order)

1. **Gate this diff before committing:** `make lint`,
   `make t ONLY=hotswap_loader`, `make t ONLY=hotswap_simnet`,
   `make t ONLY=command_handler_snapshot`, then a full `make ci` /
   `make test-parallel` pass. Commit + push `dev/zero-mcp-w1bc`, merge to
   main per the parallel-worktree protocol (`docs/work/agent-protocol.md`).
2. **Live dev-lane acceptance** (not yet run): build a real `native.leaves`
   generation `.so` for one eligible TU (e.g.
   `status_native_handlers.c`), run `dev.hotswap.probe` against the running
   `~/.zclassic-c23-dev` node, confirm PASS with no snapshot mutation, then
   run `dev.hotswap.apply` and confirm the leaf's response actually changes
   (e.g. a marker field the edited body adds) with no restart — the
   `native.leaves` analogue of `tools/scripts/hotswap_demo.sh`. No such
   script exists yet for `native.leaves`; consider adding one alongside the
   existing `hotswap_demo.sh` for `mcp.routes`.
3. **W2 — consumer migration** (`docs/work/MCP-REMOVAL-WORKLIST.md`, 47
   sites): re-point remaining MCP-only consumers at the native bridge/registry
   surface now that both the leaf-handler override layer (W1-A) and its
   hot-swap publication path (W1-B/C) exist end to end.
4. **W3 — the delete** (52-site delete per the worklist): `tools/mcp/**`,
   `tools/mcp_server.c`, `-mcp`/`-mcp-inprocess`/`mcpcall` in `src/main.c`,
   `.mcp.json`, Makefile targets, transport flag bits, including
   `tools/mcp/controllers/dev_hotswap_controller.c` and the `mcp.routes`
   provider class in `lib/hotswap` once `native.leaves` is the only consumer
   left. Final `grep -ri mcp` sweep.

## Traps / carry-forward notes for W2

- `replace_batch`'s branch-mode rejection (this wave) was a carry-forward gap
  flagged by the W1-A verifier — confirm no other W1-A carry-forward items
  remain before starting W2 (see
  `docs/work/SESSION-HANDOFF-2026-07-11-ZERO-MCP.md` "Next steps" item 1).
- `.def` command files are NOT depfile-tracked: after editing
  `config/commands/dev.def` or `config/hotswap_eligible.def`, remove the
  stale `.o` (e.g. `build/obj/config/src/command_catalog.o`) or a rebuild can
  silently no-op on the source change.
- Pre-push runs `make pre-push-ci`: every changed file needs an
  `agent_impact_rules.def` mapping (group names must exist) — done for this
  wave's new files, but re-verify if more files are touched before commit.
- `tools/command/native_dev_hotswap.c` / `.h` are **untracked** in the
  worktree (new files) — `git add` them explicitly; they will not show up in
  `git diff` against HEAD, only in `git status`.
