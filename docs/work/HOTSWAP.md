# Tier-1 in-process hot-swap (DEV-ONLY)

> **Two tracks now exist. Read this first.**
>
> 1. **The manifest/staging pilot** (`mcp.routes` + `native.leaves`, everything
>    below "Moving parts") stays **Phase-0 contained (2026-07-12)**: `make
>    hotswap`, resident `dev_hotswap`, legacy `zcl_agent_hotswap`, watcher
>    publication modes, and direct scripts all refuse before `dlopen` or registry
>    replacement. `make hotswap-so`, `make hotswap-sim`, loader tests, and state
>    inspection remain available.
>
> 2. **The REAL (activatable) module ABI** (`struct zcl_hotswap_module`, added
>    `wf/hotswap-finish`) is the finished machinery — see
>    "Real module ABI (activatable, gated)" immediately below. It is **default
>    verify-only** and a live swap is **gated OFF** behind BOTH the
>    `-hotswap-activate` flag AND `ZCL_HOTSWAP_ACTIVATE=1`, and REFUSES the
>    canonical datadir. Canonical activation stays behind the owner's Phase-3
>    ritual (NOT implemented here).

## Real module ABI (activatable, gated)

The "finished" single-handler hot-swap path. Unlike the manifest pilot (which
re-points a whole controller's route/leaf table via a host vtable and NEVER
dlcloses), a module `.so` carries ONE swappable leaf and the superseded `.so` is
dlclose'd after in-flight dispatch drains.

| Piece | Where |
|-------|-------|
| ABI struct + emitter + activation API | `lib/hotswap/include/hotswap/hotswap_module.h` |
| Loader (dlopen/admit/self_test/commit/retire) + gate + telemetry | `lib/hotswap/src/hotswap_activate.c` |
| Epoch/refcount drain + `zcl_command_registry_all_retired_quiesced()` | `lib/kernel/src/command_registry.c` |
| Swappable allowlist (THE HARD LINE) | `config/hotswap_swappable.def` |
| Hard-line lint gate + selftest | `tools/lint/check_hotswap_swappable_shape.sh`, `lib/test/src/test_make_lint_gates.c` |
| Per-handler `.so` build path | `make hotswap-module-so HANDLER=core.status` → `build/hotswap/<handler>-<gitsha>.so` |
| Resident RPC + CLI verify/apply | `tools/command/native_dev_hotswap.c` (`dev.hotswap.probe`/`apply`, `dev_hotswap_native`) |
| Activation flag `-hotswap-activate` | `src/main.c` |
| Tests | `lib/test/src/test_hotswap_module.c` |

**The ABI.** Each swappable `.so` exports ONE data symbol `zcl_hotswap_module`:

```c
struct zcl_hotswap_module {
    uint32_t abi_version;                     /* == ZCL_HOTSWAP_MODULE_ABI_V1 */
    const char *handler_name;                 /* canonical READY read-only leaf */
    zcl_hotswap_handler_fn fn;                /* replacement handler */
    bool (*self_test)(char *err, size_t cap); /* module health hook */
};
```

A missing symbol or a mismatched `abi_version` is refused **loudly** (logged +
typed `stage`/`error`), and the handler is never called. `hotswap_module_admit()`
is the pure, always-compiled gauntlet (ABI + fields + allowlist + self_test),
unit-tested directly with fabricated structs.

**Epoch/refcount quiesce.** Command dispatch acquires the active override
snapshot with an optimistic refcount + revalidate (UAF-safe because snapshots
are never freed) and holds it across the override handler call. To reclaim a
superseded `.so`, the loader waits (bounded) until
`zcl_command_registry_all_retired_quiesced()` reports every RETIRED snapshot has
drained to a zero refcount, THEN dlcloses it. If drain cannot be confirmed the
`.so` is kept mapped forever — the pilot's never-close fallback, always
memory-safe. The no-override fast path stays zero-RMW.

**THE HARD LINE.** Only shape-leaf handlers (controllers / views / conditions)
may be swapped. `config/hotswap_swappable.def` is the allowlist;
`check-hotswap-swappable-shape` (in `make lint`, self-tested) fails if any
swappable `source_tu` resolves under a reducer stage, consensus validation, the
storage engine, a supervisor, or any state root. The reducer stages, consensus,
storage, and supervisors are NEVER swappable.

**Activation gating (do NOT weaken).** Default is verify-only: `dev.hotswap.probe`
(and the resident RPC with `activate=false`) dlopen + ABI-validate + self_test
and NEVER commit. A live swap requires ALL of: the `-hotswap-activate` flag on
the resident node, `ZCL_HOTSWAP_ACTIVATE=1` in its environment, and the exact
`~/.zclassic-c23-dev` datadir — the canonical `~/.zclassic-c23` datadir is
refused with a typed reason. `hotswap_activation_authorized()` is the single
gate; verify-only results are always labeled `verify_only`. Telemetry:
`zcl_state subsystem=hotswap` → `activation` object (flag/env/containment,
counts, active slots, last activation/rejection).

**Known v1 TODOs.** The module `self_test` is a structural OK; an isolated
behavioral precommit probe (dispatch the candidate against an empty request and
compare an expected schema before publish) is future work. The CLI→resident
`apply` forward assumes the default dev datadir/port resolution.

The manifest/staging pilot documentation follows unchanged.

> **Phase-0 containment (2026-07-12) — manifest/staging pilot only:** the
> pilot has no runtime publication path. `dev.hotswap.apply` now forwards to the
> resident node's module-ABI path (above); `make hotswap`, resident
> `dev_hotswap`, legacy `zcl_agent_hotswap`, watcher publication modes, and
> direct hot-swap scripts still refuse before `dlopen` or registry replacement.
> `make hotswap-so`, `make hotswap-sim`, loader tests, and state inspection
> remain available. The mechanism below documents the pilot's in-tree
> implementation and target transaction; it is not publication authority.

The in-tree mechanism can compile a manifest-eligible stateless controller `.c`
into a content-identified generation `.so`. Two provider classes exist:

- **`mcp.routes`** (original pilot — everything below through "Known limits"
  describes it) — re-points routes in the resident **MCP router**.
- **`native.leaves`** (Wave W1-B/C, Zero-MCP re-target — see the section
  below) — re-points leaf handlers in the **kernel command registry**, so
  Tier-1 hot-swap keeps working after the MCP server is deleted (W3). See
  `docs/work/project_zero_mcp_directive_2026-07-11.md` for the deletion plan.

Generation v2 can stage every route/leaf, validate the complete set and ABI
manifest, and run its self-test. Public entry points stop before its commit
callback. Any probe failure publishes zero routes/leaves.

**This is dev-only. The release binary is 100% static with zero `dlopen`.**
Every dynamic-loading line lives behind `#ifdef ZCL_DEV_BUILD`; a release build
compiles the whole load path down to a stub that returns
`hotswap unavailable in release build`. Enforced by the `check-hotswap-dev-only`
lint gate (no `dlopen`/`dlsym`/`dlclose` outside `lib/hotswap/`, and inside
`lib/hotswap/` only within a `ZCL_DEV_BUILD` region).

## ⚠️ Ephemerality

When publication is eventually re-enabled, a hot-swap will live only for the
lifetime of the process it was loaded into and revert on restart. `make
dev-bin` may build a converged binary today, but `make deploy-dev` is contained.
Generations are never `dlclose`'d (a deliberate leak) so an in-flight call that
already entered old code stays valid. Each accepted generation also retains
the descriptor for the exact inode loaded through `/proc/self/fd/N`. Reusing
that descriptor number while an older handle remains mapped can make `dlopen`
return the older cached object for a different path; the pinned descriptor is
therefore part of generation identity, not an accidental file leak.

The target `dev-watch auto` design starts an asynchronous immutable binary
build and preflight stage after a successful swap. No watcher currently runs
that publication path. Once re-enabled, the next
transactional reload still verifies it and preserves rollback semantics.

## Moving parts

| Piece | Where |
|-------|-------|
| Loader (`hotswap_load`, registry, `zcl_state` dumper) | `lib/hotswap/` |
| Immutable router snapshots + transactional batch commit | `tools/mcp/router.{c,h}` |
| Contained legacy MCP apply `zcl_agent_hotswap` | `tools/mcp/controllers/dev_hotswap_controller.c` |
| v2 manifest + `ZCL_HOTSWAP_EXPORT_ROUTES` macro | `lib/hotswap/include/hotswap/hotswap.h` |
| Eligible controller TUs (each exports its own `k_routes[]`) | `tools/mcp/controllers/{app,meta,chain,net,wallet}_controller.c` |
| Runtime eligibility allowlist | `config/hotswap_eligible.def` |
| Persistent dev RPC bridge | `tools/mcp/dev_rpc_bridge.{c,h}` (`dev_hotswap`, read-only `dev_mcp_call`) |
| Build/verification targets | `make hotswap-so`, contained `make hotswap`, `make hotswap-sim` |
| `zcl_state subsystem=hotswap` | `zcl.hotswap_generation.v2` provenance/status |
| **`native.leaves` (W1-B/C)** loader entrypoint `hotswap_load_leaves` | `lib/hotswap/` (`hotswap_loader.c`) |
| Atomic leaf-handler override layer + `zcl_command_registry_replace_batch` | `lib/kernel/src/command_registry.c` |
| v3 host + `ZCL_HOTSWAP_EXPORT_LEAVES` macro | `lib/hotswap/include/hotswap/hotswap.h` |
| Eligible native TUs (each exports its own `k_leaves[]` trampoline table) | `app/controllers/src/{status,wallet,net,meta,chain}_native_handlers.c` |
| Native seam a trampoline calls into its own body | `zcl_native_bridge_run()` in `tools/command/native_command.c` |
| Native commands: contained `dev.hotswap.apply` and `dev.hotswap.probe` | `config/commands/dev.def`, `tools/command/native_dev_hotswap.{c,h}` |
| Resident RPC the CLI forwards to | `dev_hotswap_native` in `tools/mcp/dev_rpc_bridge.c` |

Zero new external dependencies: plain `$(CC) -shared` + libc `dlopen` (`-ldl`,
already linked). No mold/lld/inotify/etc. required.

## How a generation .so is built

A swap-eligible controller invokes `ZCL_HOTSWAP_EXPORT_ROUTES(k_routes,
PARAM_COUNT(k_routes))` once at file scope (no trailing semicolon). Under a
generation build (`-DZCL_HOTSWAP_GEN`) that macro emits both
`zcl_hotswap_manifest_v2` and `zcl_hotswap_gen_init`. The manifest binds schema
and host ABI sizes, capabilities, source/build identity, an exact input digest,
stateless state schema, mapped tests, probe tools, self-test, and quiescence.
The loader validates it before calling generation code.

`gen_init` calls the host's staging callback. Staging cannot mutate the live
router. Only after every route and the generation self-test pass does the
resident router build and release-publish one new snapshot. Failed generation
mappings are closed; successful mappings remain mapped for the process lifetime
so calls that already entered old code finish safely.

At load time the `.so` is opened `RTLD_NOW | RTLD_LOCAL`; its unresolved kernel
symbols (`mcp_node_rpc`, `json_*`, …) bind against the `-rdynamic` executable's
global scope, so the new handler runs against the live node's state.

## Commands

```sh
# Build a generation .so from one edited, manifest-admitted controller TU.
# Prints the .so path as its last line.
make hotswap-so FILES="tools/mcp/controllers/app_controller.c"

# Verify loader policy without entering a resident node.
make t ONLY=hotswap_loader

# Inspect the resident generation through the non-destructive bridge:
build/bin/zclassic23-dev -datadir="$HOME/.zclassic-c23-dev" -rpcport=18252 \
  dev_mcp_call zcl_state '{"subsystem":"hotswap"}'

# Fast deterministic three-node generation/network replay:
make hotswap-sim
```

`make hotswap`, resident `dev_hotswap`, and `zcl_agent_hotswap` return a
containment refusal. They do not use exact-dev-lane eligibility as authority and
never reach the loader.

### Which process gets swapped

No process gets swapped during containment. All native, resident-RPC, MCP, Make,
and direct-script apply paths refuse before loading a candidate. The read-only
`dev_mcp_call` and hot-swap state dump remain diagnostic surfaces.

## End-to-end proof without publication

Use `make hotswap-sim` and the focused loader tests below. The former
commit-oriented `hotswap_demo.sh` and resident probe are not public proof paths
while loading/publication are contained.

## Tests

- `make hotswap-sim` — three real RAM-only simnet nodes, rejected-batch zero
  publication, in-flight old generation, atomic two-provider commit, new-call
  visibility, and exact seed replay. Override/replay with
  `ZCL_HOTSWAP_SIM_SEED=0x... make hotswap-sim`.
- `make t ONLY=mcp_router` — immutable snapshot/batch + schema tests.
- `make t ONLY=hotswap_loader` — path/datadir predicates, registry, dumper,
  and the release-stub refusal (the test binary is not a `ZCL_DEV_BUILD`).
- `make t ONLY=dev_mcp_rpc_bridge` — release omission, exact-lane registration,
  CLI typing, and persistent generation visibility across resident RPC calls.

## Eligibility (which TUs Tier-1 can swap)

This section covers `mcp.routes` eligibility. For the `native.leaves`
provider class eligibility table, see "`native.leaves` provider (Zero-MCP
re-target, Wave W1-B/C)" below.

Eligible = (a) all routes go through the router snapshot mechanism
(`ZCL_HOTSWAP_EXPORT_ROUTES` over one `k_routes[]`), (b) no mutable file-scope
static (`check-hotswap-static-state`), (c) app-layer, outside every consensus/
state root (`check-hotswap-eligible-scope`, which now also rejects a listed TU
that fails to invoke the export macro). The scope gate's forbidden roots are
`core`, `lib/consensus`, `lib/validation`, `lib/storage`, `lib/net`,
`lib/coins`, `app/jobs`.

| TU | Eligible | Why |
|----|----------|-----|
| `tools/mcp/controllers/app_controller.c` | ✅ | pilot; stateless `k_routes[]` (ZNAM/ZMSG/market/swap) |
| `tools/mcp/controllers/meta_controller.c` | ✅ | stateless; status/kpi/tools_list/self_test |
| `tools/mcp/controllers/chain_controller.c` | ✅ | stateless read-only chain/replay (legacy-dir scratch hoisted to stack) |
| `tools/mcp/controllers/net_controller.c` | ✅ | stateless network introspection |
| `tools/mcp/controllers/wallet_controller.c` | ✅ | stateless; includes wallet-mutating routes, same footing as the pilot's destructive routes |
| `tools/mcp/controllers/ops_controller.c` | ❌ | two route tables (`g_agent_mcp_routes` + `k_routes`) — one macro exports one `gen_init`; needs an aggregator |
| `tools/mcp/controllers/diagnostics_controller.c` | ❌ | mutable file-scope statics (`g_state_subsystems_csv`, non-const `p_state[]`) |
| `tools/mcp/controllers/dev_hotswap_controller.c` | ❌ | is the swap driver; swapping the swapper is circular |
| `app/controllers/src/api_controller_routes.c`, `diagnostics_registry.c` | ❌ | `ZCL_HOTSWAP_EXPORT_PROVIDER` direct-provider slots can't join the router transaction (fail closed on `CAP_DIRECT_PROVIDER`) |

Each eligible TU is built into its OWN single-provider `.so` and swapped
independently. Every eligible TU is proven end-to-end by
`tools/scripts/hotswap_demo.sh` (build gen `.so` → dlopen via one exact-dev
`mcpcall` → assert the atomic commit re-points a route it owns).

## Known limits (v2 stateless pilot, fail-closed)

- Only the stateless MCP route provider class (`mcp.routes`) is admitted. REST
  and diagnostics were removed from eligibility because their independent atomic
  slots cannot join the router transaction. Services, models, storage, events,
  conditions, supervisors, wallet/key *state*, networking, reducer/consensus,
  and bootstrap ownership are `reload_required`.
- One eligible controller per `.so`; a generated multi-provider aggregator is
  still needed before multiple controller TUs can form ONE generation. The
  Wave 3.1 expansion widens the *set* of independently swappable TUs (app +
  meta + chain + net + wallet), it does NOT put multiple providers in one `.so`.
- There are no state migrations or background callback leases in this pilot.
  That is safe because admitted code is stateless, declares no quiescence, and
  old text is never unmapped. Stateful modules remain refused.
- `dev-watch auto` is contained. It never calls the resident RPC, never falls
  back on exit 69, and never schedules a post-swap binary stage.

## `native.leaves` provider (Zero-MCP re-target, Wave W1-B/C)

Everything above this section describes the original `mcp.routes` provider
class, which re-points routes in the resident **MCP router**
(`tools/mcp/router.{c,h}`). The ZERO-MCP directive
(`docs/work/project_zero_mcp_directive_2026-07-11.md`) deletes the MCP router
in Wave 3, so Tier-1 hot-swap needed a second, MCP-free provider class that
re-points **native command leaves** in the **kernel command registry**
instead. Both provider classes remain in-tree for dual-run tests, but neither
has a public publication authority today. When re-enabled, publishing a
`mcp.routes` generation must not touch `native.leaves` and vice versa; a
controller pair (e.g. `tools/mcp/controllers/status_controller.c` and
`app/controllers/src/status_native_handlers.c`) may each carry their own
`ZCL_HOTSWAP_EXPORT_*` macro and be swapped independently.

### What changed at the ABI level

`lib/hotswap/include/hotswap/hotswap.h` gained a **v3 host** alongside the
existing v2 host (same manifest schema, `ZCL_HOTSWAP_MANIFEST_SCHEMA_V2`):

- `ZCL_HOTSWAP_HOST_ABI_V3` / `ZCL_HOTSWAP_CAP_LEAF_STAGE` /
  `ZCL_HOTSWAP_V3_HOST_CAPABILITIES` — the v3 capability bit and its required
  set, parallel to the v2 `MCP_STAGE`/`ATOMIC_COMMIT` pair.
- `struct zcl_hotswap_host` gained one field appended past `mcp_stage`:
  `bool (*leaf_stage)(const char *path, zcl_command_handler_fn handler);`.
  The append is deliberate and load-bearing: `ZCL_HOTSWAP_HOST_STRUCT_SIZE_V2`
  is frozen as `offsetof(struct zcl_hotswap_host, leaf_stage)` (not
  `sizeof(...)`), so an already-compiled `mcp.routes` generation keeps
  declaring the SAME `host_struct_size` it always did — the v3 field growing
  the struct cannot retroactively break v2 generations.
- `struct zcl_hotswap_leaf_replacement { const char *path;
  zcl_command_handler_fn handler; }` and `zcl_hotswap_leaf_commit_cb` are the
  leaf analogues of `zcl_hotswap_mcp_replacement` / `zcl_hotswap_commit_cb`.
- `hotswap_load_leaves(so_path, datadir, probe_leaf, commit_cb, ctx, report)`
  in `lib/hotswap/src/hotswap_loader.c` is the leaf analogue of `hotswap_load()`
  — same fail-closed precheck/dlopen/hash/manifest-validate core, diverging
  only in staging leaves via the v3 host and invoking a
  `zcl_hotswap_leaf_commit_cb`. Dev-only: without `ZCL_DEV_BUILD` it refuses
  with "unavailable" and performs no `dlopen`.
- `ZCL_HOTSWAP_EXPORT_LEAVES(leaves_arr, leaves_count)` is the leaf analogue of
  `ZCL_HOTSWAP_EXPORT_ROUTES`. Both macros now expand through one shared
  `ZCL_HOTSWAP_EXPORT_MANIFEST(host_abi_, host_size_, required_caps_,
  provider_id_, probe_csv_)` body, so the v2/v3 manifest emitters cannot drift
  apart. A TU invokes exactly one of the two `EXPORT_*` macros — never both —
  so the single `zcl_hotswap_default_self_test` / `zcl_hotswap_manifest_v2`
  symbols never clash.
- `lib/kernel/src/command_registry.c`'s `zcl_command_registry_replace_batch()`
  is the publication primitive `native.leaves` commits into: atomic,
  generation-monotonic (a batch with `gen <= active` is rejected outright,
  publishing nothing), lock-free readers. W1-B/C added one more precheck to
  it: a batch entry naming a **branch** path (`mode == ZCL_COMMAND_MODE_BRANCH`,
  no handler) is rejected with `"leaf '%s' is a branch, not swappable"` — only
  a real, `READY` leaf can be re-pointed.

### How a generation .so re-points its OWN leaf

A generation `.so` re-points a leaf to its own freshly-compiled body through a
**per-TU trampoline** compiled `#ifdef ZCL_HOTSWAP_GEN` at the bottom of the
owning `app/controllers/src/*_native_handlers.c` file. Each trampoline calls
the new seam `zcl_native_bridge_run(request, body, reply)`
(`tools/command/native_command.c`) — the same request-build /
dispatch / project pipeline `zcl_native_bridge_command()` always used, now
factored so a caller can supply an explicit `body` function instead of
resolving one from the static path table. `zcl_native_bridge_command()` itself
is now a thin wrapper: `zcl_native_bridge_run(request,
zcl_native_bridge_body_for_path(request->spec->path), reply)`. Example
(`app/controllers/src/status_native_handlers.c`):

```c
#ifdef ZCL_HOTSWAP_GEN
#define ZCL_HOTSWAP_PROBE_LEAF "core.status"
#include "hotswap/hotswap.h"
#include "kernel/command_registry.h"
#include "command/native_command.h"

static void tramp_status(const struct zcl_command_request *request,
                         struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_status_body, reply);
}
/* ...five more trampolines... */

static const struct zcl_hotswap_leaf_replacement k_leaves[] = {
    { "core.status",         tramp_status },
    { "core.sync.diagnose",  tramp_syncdiag },
    /* ... */
};

ZCL_HOTSWAP_EXPORT_LEAVES(k_leaves, sizeof(k_leaves) / sizeof(k_leaves[0]))
#endif /* ZCL_HOTSWAP_GEN */
```

`lib/hotswap` itself never includes kernel/app headers — `struct
zcl_command_request`/`zcl_command_reply` are only forward-declared in
`hotswap.h` (`struct zcl_command_request; struct zcl_command_reply; typedef
void (*zcl_command_handler_fn)(...)`); the concrete types are complete only in
the controller TU that invokes `ZCL_HOTSWAP_EXPORT_LEAVES`.

### `dev.hotswap.apply` / `dev.hotswap.probe` (native commands)

Tier-1 hot-swap over `native.leaves` is driven through two new native dev
commands (`config/commands/dev.def`, handlers in
`tools/command/native_dev_hotswap.c` — the MCP-free successor of
`tools/mcp/controllers/dev_hotswap_controller.c` +
`tools/mcp/dev_rpc_bridge.c`'s `rpc_dev_hotswap`, kept entirely off
`tools/mcp/` so it survives the W3 MCP delete):

```sh
zclassic23-dev dev hotswap probe --input='{"so_path":"/tmp/gen.so","probe_leaf":"core.status"}'
```

The two command IDs remain registered as typed compatibility refusals during
containment:

- **`dev.hotswap.apply`** returns `runtime_publication_contained` before
  `dlopen`, resident forwarding, or `zcl_command_registry_replace_batch()`.
- **`dev.hotswap.probe`** returns `resident_probe_contained` before resident
  forwarding or `dlopen`. A discard-only in-process probe is still unsafe:
  ELF constructors execute before manifest validation and destructors execute
  at `dlclose`.
- The resident `dev_hotswap_native` RPC returns the same refusal even on the
  exact dev datadir. Re-enable only with a disposable worker, pre-load
  sidecar/ELF/import policy, immutable artifact receipt, and bounded fixtures.

### Eligibility for `native.leaves`

Eligibility mirrors the `mcp.routes` rule set (`config/hotswap_eligible.def`,
enforced by `tools/lint/check_hotswap_eligible_scope.sh`, which now accepts
`.` in a canonical probe — a dotted command path like `core.status`, not just
an mcp tool name like `zcl_status` — and requires an eligible TU to invoke
EITHER `ZCL_HOTSWAP_EXPORT_ROUTES` OR `ZCL_HOTSWAP_EXPORT_LEAVES`). Five
native TUs are eligible today, each mapped to its `mcp.routes` sibling:

| `native.leaves` TU | Probe leaf | `mcp.routes` sibling |
|---|---|---|
| `app/controllers/src/status_native_handlers.c` | `core.status` | `app_controller.c`/`meta_controller.c` |
| `app/controllers/src/wallet_native_handlers.c` | `core.wallet.address.list` | `wallet_controller.c` |
| `app/controllers/src/net_native_handlers.c` | `core.network.peers.incidents` | `net_controller.c` |
| `app/controllers/src/meta_native_handlers.c` | `ops.metrics` | `meta_controller.c` |
| `app/controllers/src/chain_native_handlers.c` | `core.consensus.utxo.audit` | `chain_controller.c` |

`app/controllers/src/diagnostics_native_handlers.c` is **eligible-pending, NOT
listed**. A `native.leaves` generation self-test dispatches the manifest's
declared probe leaf with **no input** before committing (mirroring the
`mcp.routes` probe discipline), so a probe leaf must tolerate empty args. Both
leaves this controller owns reject that: `core.storage.query` →
`zcl_native_sql_body` requires a non-empty `sql` and `ops.logs` →
`zcl_native_node_log_body` requires a non-empty `pattern`; both fail with a
top-level RPC error on empty args, which would make the self-test spuriously
fail on every load. **Rule for any future eligible TU: verify the probe leaf's
body tolerates empty args by reading the body function (and its backing RPC
handler's param-arity check) — never assume it from the leaf name.**

### Tests

- `make t ONLY=hotswap_loader` — `test_hotswap_leaf_manifest_v3_contract`:
  the v3 manifest/host contract (ABI/size/caps mismatch rejection, unknown
  `provider_id` rejection), fixtured against the real
  `status_native_handlers.c` eligibility row.
- `make t ONLY=hotswap_simnet` — `test_hotswap_native_leaf_repoint`: two
  successive `zcl_command_registry_replace_batch()` generations atomically
  re-point the SAME fixture leaf path, proving generation monotonicity (a
  `gen <= active` batch is rejected, publishing nothing) the same way the
  MCP-router simnet above proves it for `mcp.routes` — without touching
  `mcp/router.h` at all.
- `make t ONLY=command_handler_snapshot` — `test_reject_branch_leaf`: a batch
  entry naming a branch path (`mode == ZCL_COMMAND_MODE_BRANCH`, no handler)
  is rejected with nothing installed.

### Known v1 TODOs

- **Isolated precommit behavioral probe.** Before publication may be
  re-enabled, `hotswap_commit_native_leaves()`
  (`tools/command/native_dev_hotswap.c`) has a `TODO(W1)`: it does not yet
  look up the probe leaf's spec and invoke the candidate handler against an
  empty request before publish. Today's v1 guards are the generation
  self-test (loader-side) plus `zcl_command_registry_replace_batch()`'s
  READY / read-only / non-branch / non-duplicate validation — a dry
  precommit probe needs a typed registry lookup+invoke API. Containment must
  remain until that probe and its expected output schema are enforced.
- **CLI datadir/port resolution.** `hotswap_cli_ensure_rpc_client()`
  (`tools/command/native_dev_hotswap.c`) has a `TODO(W1)`: the CLI resolves
  the dev datadir/port from fixed defaults
  (`$HOME/.zclassic-c23-dev`, port 18252) instead of threading the CLI's own
  resolved `-datadir`/`-rpcport` (today held in `native_command.c`'s
  `g_bridge_datadir`/`g_bridge_rpc_port` statics) through the request
  context.

### Why the RPC registration lives in `dev_rpc_bridge.c`, not `boot_services.c`

`register_dev_native_hotswap_rpc()` is called from
`tools/mcp/dev_rpc_bridge.c`'s `dev_bridge_register_impl()`, not from
`config/src/boot_services.c` (where most boot-time RPC registration lives).
`config/src/boot_services.c` is pinned at its `check-file-size-ceiling` (E1)
ratchet baseline — `tools/lint/file_size_ceiling_baseline.txt` records it at
exactly 1634 lines, its current line count, i.e. **zero headroom**. Any net
addition to that file fails the E1 gate outright. `dev_rpc_bridge.c` was
already the natural home (it registers the sibling `dev_hotswap`/`dev_mcp_call`
methods), so the new registration call landed there instead.

## ZVCS auto-anchor (Wave 2.3, power-station Pillar 2)

Every warm "passed" `zcl.dev_cycle.v1` verdict — whether the cycle resolved to a
Tier-1 hot-swap (`resident_commit`), a native fast/transactional reload
(`transactional_reload`), or a docs-only `check` — now auto-anchors: the dev
loop's `finish_cycle()` (`tools/dev/devloop_cycle.c`) calls
`vcs_devloop_anchor_cycle()` (`lib/vcs/src/vcs_devloop.c`), which opens (or,
on the very first call, creates) the ZVCS repo at `.zvcs/` beside `.git/` and
takes a snapshot. The commit binds the current source tree to this cycle's
verdict (`status`/`phase`/`elapsed_ms`), the binary generation it produced,
and (from `ZCL_AGENT_ID`/`ZCL_SESSION_ID`/`ZCL_TASK_REF`) the agent/session/
task that drove it — no "remember to commit" step.

The first-ever snapshot is different: it may need thousands of durable object
writes, so it is never allowed to delay an edit verdict. `vcs_devloop_anchor_cycle()`
(`lib/vcs/src/vcs_devloop.c`) itself never spawns anything to run that
baseline — lib/vcs is release-linkable and must stay process-spawn free
(`check-vcs-no-git`, the ZVCS-sovereignty gate: no `git`, `fork()`, `exec*`,
`system()`, or `popen()` anywhere under `lib/vcs/`). It just detects that no
durable history exists yet and returns `"vcs_deferred":true` plus an explicit
`vcs_error` saying that cycle is unanchored, with `out->baseline_needed` true
for whichever cycle is the first to notice. The caller decides how to run the
baseline: `vcs_devloop_run_initial_baseline(repo_root, out)` runs it
synchronously (takes the `.zvcs/bootstrap.lock` flock singleton, snapshots,
releases the lock — no process spawned), and the interactive dev loop instead
wants it off its foreground path, so `finish_cycle()`
(`tools/dev/devloop_cycle.c`) detaches it via
`zcl_devloop_baseline_launch()` (`tools/dev/devloop_baseline.c`, dev-only —
linked only into the dev binary, `check-release-no-dev-symbols`-proven absent
from release): a double fork + `setsid()` so the grandchild worker calls
`vcs_devloop_run_initial_baseline()` with stdio redirected to
`.zvcs/bootstrap.log`, while the launcher reaps its short-lived child so a
persistent dev-loop watcher never accumulates a zombie. Once the baseline
completes, subsequent cycles synchronously bind their exact generation as
above. Generated build trees, local agent worktrees, and caches are pruned
from the tracked walk, and stale stat-cache rows are deleted during the
background baseline so steady-state cost follows the real source change set.

**Historical/pre-containment generation binding, per cycle type:** no current
cycle can produce the hot-swap or reload cases below.
- Hot-swap (`resident_commit`): the `artifact_sha256` field returned
  by `dev_hotswap` / `zcl_agent_hotswap` (see
  `tools/mcp/controllers/dev_hotswap_controller.c`) — the sha256 of the exact
  `.so` this cycle `dlopen`'d.
- Reload (`transactional_reload`): `candidate_sha256` (falling back to the
  hex suffix of `running_generation`) read back from the
  `zcl.agent_dev_deploy.v1` state file `tools/dev/deploy-dev-lane.sh` just
  wrote to `$HOME/.zclassic-c23-dev/agent-deploy.json`.
- `check` (docs-only, no build): no generation is known, so the commit binds
  an all-zero `generation_sha256` — the source-tree/verdict binding still
  lands.

**Fail-open, with one loud exception.** A ZVCS problem (a missing `HOME`, a
blocked `.zvcs/`, a corrupt index, ...) never fails the dev cycle: `finish_cycle`
still returns `passed`/`rejected` exactly as it would without ZVCS, and the
`zcl.dev_cycle.v1` verdict JSON gains one extra field —
`"vcs_commit":"<64-hex commit id>"` on success, or `"vcs_error":"<message>"`
on failure/deferred bootstrap (with `"vcs_deferred":true` for the latter). The
one exception that is surfaced loudly is a **sealed-path
refusal**: editing a file under a sealed glob (see `vcs/vcs_seal.h` —
default set `core/`, `domain/consensus/`, `lib/consensus/`, `lib/validation/`,
`lib/chain/`, `lib/mining/`, `app/jobs/`) without a granted one-shot unseal
token adds `"vcs_sealed_refusal":true` alongside `"vcs_error"`. At this
integration point the refusal remains a source-integrity signal. During
Phase-0 no cycle publishes before `finish_cycle`: runtime publication is
already contained at the entry point. Sealed-path anchoring is defense in
depth, not authority to activate code.

## Transactional reload: native engine (implemented machinery, contained)

Neither backend below is reachable from a public publication entry point.
`ZCL_DEV_NATIVE_ACTIVATION`, `ZCL_DEV_SOURCE_ID`, or a direct Make/script call
cannot opt into activation. The detail is retained as implementation to finish
behind the unified source/proof/CAS/accept/rollback transaction.

The `transactional_reload` implementation has two backends. When re-enabled
behind that transaction, both stop the running
`zcl23-dev.service`, atomically flip the `current` generation symlink,
restart, and verify the exact `/proc` executable identity before declaring
success — rolling back to the previous generation on any preflight/verify
failure. What differs is HOW:

- **Shell path (pre-containment design).**
  `devloop_cycle.c`'s `transactional_reload` label shells out to `make
  agent-deploy-fast`, which runs `make fast-rebuild` then drives
  `tools/dev/deploy-dev-lane.sh` — the proven bash transaction. This is what
  was selected when `ZCL_DEV_NATIVE_ACTIVATION` was unset.
- **Native engine (hermetic/test seam during containment).** The former
  selection used `ZCL_DEV_NATIVE_ACTIVATION=1`. Public watcher and one-shot
  entry points now refuse before consulting it. The reload implementation runs
  `make fast-rebuild` (a build step
  common to both backends — the engine never builds), then calls
  `dev_activation_run()` (`tools/dev/dev_activation.{h,c}`) directly with a
  request built from the exact dev-lane constants
  `tools/dev/deploy-dev-lane.sh` hard-codes (`GEN_ROOT`, `DEV_DATADIR`,
  `UNIT`, `DEV_RPCPORT`) — no `deploy-dev-lane.sh` invocation, no
  `agent-deploy.json` re-read: the engine's own `result.candidate_sha256`
  feeds the ZVCS generation binding directly. `build_commit` is recomputed
  via a fixed-argv `git rev-parse --short HEAD` (+ `-dirty` suffix), the
  same algorithm `deploy-dev-lane.sh:build_candidate()` uses, honoring
  `ZCL_DEV_BUILD_COMMIT_OVERRIDE` too. If a precondition only the shell path
  can currently satisfy is missing (e.g. `HOME` unset, `git` unavailable),
  the reload site logs a warning and falls back to the shell path for that
  one cycle.

  The retained `dev.vcs.revert` relink seam can exercise
  `dev_activation_activate_generation()` in hermetic tests. The public command
  refuses `relink_generation=true` before the source revert; source-only
  `relink_generation=false` remains available.

**Retained runtime selection seam.** The whole native engine
already compiles only into `ZCL_DEV_BUILD`/`ZCL_TESTING` binaries (see
`dev_activation.h`'s own header comment and `check-release-no-dev-symbols`);
a second compile-time gate on top would only let one flag value exist per
already-built dev binary. `ZCL_DEV_NATIVE_ACTIVATION` followed the same idiom
`deploy-dev-lane.sh` already uses for its own adjacent switches
(`ZCL_DEV_USE_PREBUILT`, `ZCL_DEV_SKIP_BUILD`). It remains useful to hermetic
engine tests, but no environment value authorizes public activation — see
`dev_activation_native_enabled()` in `tools/dev/devloop.h`.

**What is and isn't hermetically tested.** Both call sites' actual reload
bodies exec real processes (`make`, `systemctl`, `git`) and are therefore
`ZCL_DEV_BUILD`-only, invisible to the `-DZCL_TESTING` harness `make t`
drives. The engine itself (`dev_activation_run` /
`dev_activation_activate_generation`, driven against a fake in-memory ops
vtable) is fully covered by `test_dev_activation`. The glue both call sites
share to build the engine's request and interpret its result — pure,
no-process-exec — is factored into `dev_activation_native_enabled()` /
`dev_activation_request_from_cycle()` / `dev_activation_map_result()`
(`tools/dev/devloop.h` / `devloop_cycle.c`) and covered by `test_dev_platform`.

**Proof bar before any activation entry point is re-enabled:** complete
immutable source epochs, proof receipts, signed authority, a resident
expected-epoch compare-and-swap, durable prepared/accepted records, exact
post-publication probes, and deterministic rollback first. Then keep
`test_dev_activation`, `test_dev_platform`, and `test_vcs_devloop` green and
copy-prove a real isolated dev-lane activation. No environment opt-in can
substitute for those prerequisites.

## Core refusal — the fast loop cannot auto-publish sealed consensus (Wave 2.4)

The generic Phase-0 containment now blocks every runtime publication before
this narrower defense matters. A Core unseal token authorizes source handling
only; it does not authorize hot-swap, reload, stage, or generation relinking.

**What an agent sees when it edits `core/`.** The top-level `core/` tree is the
sealed consensus core (the predicates + static, height-keyed parameter tables
that decide block/tx validity — the exact surface `core/MANIFEST.sha3` pins).
When a dev cycle's changed-file set touches any file under `core/`, the fast
loop **stops before any hot-swap or reload publish** and emits a structured
refusal envelope on stdout (also persisted as the `zcl.dev_cycle.v1` verdict at
`$HOME/.local/state/zclassic23-dev/native-cycle.json`), exiting **3**
(blocked-by-precondition):

```json
{"schema":"zcl.dev_cycle.v1","producer":"native","status":"refused",
 "reason":"sealed_consensus_core",
 "paths":["core/consensus/src/pow.c"],
 "manifest":"core/MANIFEST.sha3",
 "law":"docs/CONSENSUS_PARITY_DOCTRINE.md",
 "unseal":"make core-unseal REASON=... (owner-gated; see core/UNSEAL.md)",
 "elevated_procedure":"full make ci + copy-prove + owner-gated deploy",
 "agent_next_action":"edit outside core/, or run the owner-gated unseal ritual (make core-unseal) for a consensus-parity fix"}
```

`"paths"` lists only the sealed members that triggered the refusal (a doc or
app file in the same edit is not named). The refusal fires on **both** the
one-shot `dev change cycle <files>` path and the persistent `dev loop watch`
path — both funnel through `zcl_devloop_run_cycle()`
(`tools/dev/devloop_cycle.c`), and the check precedes even the dev-build gate,
so a release binary refuses identically.

**Sealed ≠ frozen.** The refusal is not a wall — it always names the elevated
procedure. Consensus-parity fixes still ship routinely; they just leave the
autonomous fast path for the owner-gated route: `make core-unseal REASON=…`
records the reason in the append-only `core/UNSEAL.md` and mints a one-shot
`.core-unseal-token` at the repo root, then a full `make ci` + copy-prove +
owner-gated deploy, then `make core-seal` re-freezes the manifest.

**Token interaction (one unseal = one landed commit, not one dev-cycle).** The
fast loop checks for `.core-unseal-token` **read-only** and never mints or
consumes it. `make core-seal` is the sole consumer (it re-freezes the manifest
when the sealed edit lands). So a single `make core-unseal` authorizes exactly
one *landed commit* — which may span several iterative dev-cycles while the
author converges the fix — rather than a single dev-cycle. While the token is
present, a `core/` cycle **proceeds** (it is not refused) and routes to the
heaviest proof path (`consensus_parity`), because `zcl_devloop_plan_files()`
marks any sealed file `consensus_risk` — exactly as a `lib/validation/…` edit
is proven today; the loop logs to stderr that the seal was lifted for that
cycle. If the loop consumed the token itself, one owner unseal would cover only
the first of the author's iteration cycles, breaking the "one unseal = one
landed commit" contract — hence read-only.

Tests: `make t ONLY=dev_platform` — sealed-core classification (incl. `core/math`,
which the legacy `consensus_risk` prefix list never named), the refusal
envelope fields (status/reason/manifest/law/unseal/elevated_procedure and the
`paths` filtering), a real `zcl_devloop_run_cycle()` over a `core/` file
returning exit 3 with the persisted refusal verdict and no publish, and a
minted-token cycle proceeding (not refused).

**Reading the anchor log today.** There is no `dev vcs log` CLI yet (that is
Registry Phase B / Wave 3 CLI wiring, `dev vcs snapshot|status|log|diff|revert`
per the power-station plan) — the durable record is the `.zvcs/` directory in
the repo root you ran the dev loop from: `.zvcs/commits.log` (self-verifying,
newest commits at the tail; walk it with `vcs_log()`) and
`.zvcs/objects/<2-hex>/<62-hex>` (content-addressed blobs/manifests, one per
distinct file version). `lib/test/src/test_vcs_devloop.c` shows the exact
`vcs_open()` + `vcs_log()` call shape used to read a commit back out.

Tests: `make t ONLY=vcs_devloop` — a finish_cycle-shaped anchor call lands a
commit with the verdict/generation binding intact, fail-open on an unwritable
`.zvcs/`, and the sealed-refusal path. `make t ONLY=vcs_core` covers the ZVCS
foundation (object store, manifest, index, seal) this glue sits on.
