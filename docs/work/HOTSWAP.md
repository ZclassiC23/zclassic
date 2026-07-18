# Tier-1 in-process hot-swap (DEV-ONLY)

ZClassic23 has two native development mechanisms:

1. The activatable single-handler module ABI. It is ARMED on the dev lane
   (`zcl23-dev.service` passes `-hotswap-activate` and
   `ZCL_HOTSWAP_ACTIVATE=1`; the loader still refuses the canonical datadir).
   Verify-only probing (`dev.hotswap.probe`) skips the two arming gates but
   still requires the dev datadir, path confinement, and the admit gauntlet.
   The observable dev loop is `make hotswap-try HANDLER=<leaf> ARGS="<command>"`
   (or `ZCL_HOTSWAP_PRELOAD=<module.so>` on any one-shot CLI call): the freshly
   compiled body runs in the CLI process, seconds per edit.
2. The native-leaf manifest/staging mechanism. Its build, simulation, loader
   tests, and state inspection remain available, but every public publication
   entry point is contained before loading or registry replacement.

Neither mechanism is publication authority for the canonical node.

## Real module ABI (activatable, gated)

The finished single-handler path loads one swappable native leaf per `.so`.
After command dispatch drains every reference to the superseded generation,
the loader may unmap its module.

| Piece | Where |
|-------|-------|
| ABI struct + emitter + activation API | `lib/hotswap/include/hotswap/hotswap_module.h` |
| Loader, gate, retirement, and telemetry | `lib/hotswap/src/hotswap_activate.c` |
| Epoch/refcount drain | `lib/kernel/src/command_registry.c` |
| Swappable allowlist | `config/hotswap_swappable.def` |
| Shape lint + self-test | `tools/lint/check_hotswap_swappable_shape.sh`, `lib/test/src/test_make_lint_gates.c` |
| Per-handler build | `make hotswap-module-so HANDLER=core.status` |
| Native verify/apply commands | `tools/command/native_dev_hotswap.c` |
| Activation flag | `src/main.c` |
| Tests | `lib/test/src/test_hotswap_module.c` |

Each module exports one `zcl_hotswap_module` symbol:

```c
struct zcl_hotswap_module {
    uint32_t abi_version;
    const char *handler_name;
    zcl_hotswap_handler_fn fn;
    bool (*self_test)(char *err, size_t cap);
};
```

A missing symbol, ABI mismatch, invalid field, non-allowlisted handler, or
failed self-test produces a typed refusal and never calls the candidate.
`hotswap_module_admit()` is the pure admission gauntlet and has direct unit
coverage with fabricated module descriptors.

### Quiescent retirement

Command dispatch acquires the active override snapshot with an optimistic
refcount and revalidates it before calling the handler. A superseded module is
unmapped only after `zcl_command_registry_all_retired_quiesced()` reports that
every retired snapshot has drained to zero references. If bounded drain cannot
be proved, the module remains mapped. The no-override fast path stays zero-RMW.

### The hard line

Only shape-leaf handlers—controllers, views, and conditions—may be swapped.
`config/hotswap_swappable.def` is the allowlist. The
`check-hotswap-swappable-shape` lint gate rejects any entry whose source lives
under a reducer stage, consensus validation, the storage engine, a supervisor,
or another state root. Reducers, consensus code, storage, and supervisors are
never swappable.

The current allowlist (all read-only `app/controllers/` leaves, each with its
`ZCL_HOTSWAP_MODULE` emitter in the owning TU):

| Leaf | Owning TU (`app/controllers/src/`) |
|---|---|
| `core.status` | `status_native_handlers.c` |
| `core.network.peers.incidents` | `net_native_handlers.c` |
| `ops.metrics` | `meta_native_handlers.c` |
| `core.wallet.address.list` | `wallet_native_handlers.c` |
| `core.consensus.utxo.audit` | `chain_native_handlers.c` |
| `app.names.list` | `app_native_handlers.c` |

Adding one is: `ZCL_HOTSWAP_MODULE(...)` emitter in the TU plus one allowlist
row; the registry commit re-checks READY + EFFECT_READ, so a mutating leaf
fails closed even if listed.

### Activation gate

`dev.hotswap.probe` verifies a module without committing it. A live swap
requires all of:

- the resident process started with `-hotswap-activate`;
- `ZCL_HOTSWAP_ACTIVATE=1` is present in that process's environment; and
- the exact `~/.zclassic-c23-dev` datadir is in use.

The canonical `~/.zclassic-c23` datadir is refused. The single authority check
is `hotswap_activation_authorized()`. Verify-only results are always labeled
`verify_only`. `zcl23-dev.service` carries all three, so the dev lane is armed
by default.

The resident commit also needs a bound registry in the node process:
`register_dev_native_hotswap_rpc()` binds the native catalog
(`zcl_command_registry_set_active`) when the RPC is registered at boot —
without it the commit fails closed with `no active registry bound`.

Inspect activation state with:

```sh
build/bin/zclassic23-dev -datadir="$HOME/.zclassic-c23-dev" -rpcport=18252 \
  dumpstate hotswap
```

The state object reports the flag, environment gate, containment, counters,
active slots, and the last activation or rejection.

### The observable loop: process-local preload

A resident activation is recorded in the node's slot telemetry, but the node
never dispatches native leaves in-process — only the one-shot CLI does
(`zcl_command_registry_execute_json`). The loop that lets you SEE an edit is
therefore process-local:

```sh
make hotswap-try HANDLER=core.status ARGS="core status"
# or directly:
ZCL_HOTSWAP_PRELOAD=/abs/module.so \
  build/bin/zclassic23-dev -datadir="$HOME/.zclassic-c23-dev" -rpcport=18252 \
  core status
```

`ZCL_HOTSWAP_PRELOAD` (dev builds only) installs the module's override in the
CLI's own registry via `hotswap_activate_local()` and dispatches normally:
the freshly compiled body fetches live data from the dev lane and renders it.
Authority is probe-class (same as `dev.hotswap.probe`): the throwaway CLI is
the operator's own process and the override dies with it, so the resident
activation gate does not apply. Path confinement, the dev-datadir check, the
admit gauntlet, and the registry's READY + EFFECT_READ re-check all still
apply. The full edit→see cycle is one single-TU compile plus one CLI call.

### Module link rule: `-Wl,-Bsymbolic` is mandatory

Without it, the module trampoline's reference to its own freshly compiled body
(e.g. `zcl_native_status_body`) is interposed by the host executable's older
definition of the same symbol — the "swapped" handler silently runs the OLD
code. `-Wl,-Bsymbolic` binds intra-module references locally while leaving
host-only symbols (`json_*`, `node_rpc_call`, `zcl_native_bridge_run`, ...)
to bind against the `-rdynamic` host at dlopen. Both `make hotswap-module-so`
and `make hotswap-so` link with it.

The current module self-test proves structure. An isolated behavioral
precommit probe that dispatches the candidate against a bounded fixture and
checks its response schema remains required before broader activation.

## Native-leaf manifest and staging

The in-tree staging mechanism builds an eligible stateless native controller
translation unit into a content-identified generation `.so`. A generation can
stage every declared leaf, validate its complete manifest and ABI, and run its
self-test. Public entry points stop before commit, so a probe failure or a
containment refusal publishes no leaves.

This mechanism is dev-only. Release builds are static and have no dynamic-load
path. Every load operation is guarded by `ZCL_DEV_BUILD`; the
`check-hotswap-dev-only` gate enforces that dynamic-loader calls stay inside
`lib/hotswap/` and inside a development-build region.

### Ephemerality and module identity

Any future published override lasts only for the process lifetime and reverts
on restart. Accepted staging generations remain mapped so calls already inside
old code finish safely. Each accepted generation retains the descriptor for
the exact inode opened through `/proc/self/fd/N`; the descriptor is part of
generation identity because reusing its number while an older object is mapped
can make the dynamic loader return its cached object.

### Moving parts

| Piece | Where |
|-------|-------|
| Loader, generation registry, state dumper | `lib/hotswap/` |
| Manifest + `ZCL_HOTSWAP_EXPORT_LEAVES` | `lib/hotswap/include/hotswap/hotswap.h` |
| Eligibility allowlist | `config/hotswap_eligible.def` |
| Atomic leaf override layer | `lib/kernel/src/command_registry.c` |
| Eligible native translation units | `app/controllers/src/*_native_handlers.c` |
| Native bridge used by generated trampolines | `zcl_native_bridge_run()` in `tools/command/native_command.c` |
| Native probe/apply commands | `config/commands/dev.def`, `tools/command/native_dev_hotswap.{c,h}` |
| Build and verification | `make hotswap-so`, `make hotswap-sim`, focused loader tests |

No extra linker or watcher dependency is required: generation builds use the
system C compiler, shared-library support, and libc's dynamic loader.

### Generation build and admission

An eligible controller invokes
`ZCL_HOTSWAP_EXPORT_LEAVES(k_leaves, PARAM_COUNT(k_leaves))` once at file
scope. Under `-DZCL_HOTSWAP_GEN`, the macro emits the manifest and generation
initializer. The manifest binds:

- provider and host ABI versions and sizes;
- capabilities;
- the exact 64-hex source-tree identity;
- the complete generation-input digest;
- stateless state schema;
- mapped tests and probe leaf;
- self-test and quiescence requirements.

The loader requires byte-for-byte equality between the generation's build
identity and `zcl_build_source_id_sha256()` before entering generation code.
There is no dirty-tree compatibility exception, and a Git commit ID is never
authority for admission.

Generation initialization stages leaf replacements through the host API.
Staging cannot mutate the live command registry. After the full set and
self-test pass, the host may build and publish one immutable registry snapshot;
under current containment it stops before that commit.

The object is loaded local and with eager symbol resolution. Its unresolved
resident symbols, including `node_rpc_call` and JSON helpers, bind against the
development executable's exported global scope so handlers use live node state.

### Commands

```sh
# Build one eligible native controller generation.
make hotswap-so FILES="app/controllers/src/status_native_handlers.c"

# Verify loader policy without entering a resident node.
make t ONLY=hotswap_loader

# Inspect the resident staging state.
build/bin/zclassic23-dev -datadir="$HOME/.zclassic-c23-dev" \
  ops state --subsystem=hotswap

# Deterministic three-node generation/network replay.
make hotswap-sim
```

`make hotswap` and `dev.hotswap.apply` remain contained. They do not treat
dev-lane eligibility as publication authority and never reach the commit.

No process is swapped during containment. Use the simulation and focused
loader tests as the end-to-end proof surfaces.

### Provider and ABI contract

The provider ID is `native.leaves`. Its host API exposes a leaf-staging
callback, and each staged replacement names a canonical native command path
plus its handler. The loader rejects an unknown provider, an incompatible
manifest or host size, missing capabilities, a duplicate or branch path, and
any attempt to target a non-ready or mutating command.

Each eligible translation unit owns its handler bodies and the generation-only
trampolines that call them through `zcl_native_bridge_run()`:

```c
#ifdef ZCL_HOTSWAP_GEN
static void tramp_status(const struct zcl_command_request *request,
                         struct zcl_command_reply *reply)
{
    zcl_native_bridge_run(request, zcl_native_status_body, reply);
}

static const struct zcl_hotswap_leaf_replacement k_leaves[] = {
    { "core.status", tramp_status },
};

ZCL_HOTSWAP_EXPORT_LEAVES(k_leaves,
                          sizeof(k_leaves) / sizeof(k_leaves[0]))
#endif
```

`lib/hotswap` forward-declares the command request/reply structures. Their
concrete definitions are needed only in the controller that emits the
trampolines, keeping the loader independent of app and kernel headers.

### Native development commands

The native registry exposes `dev.hotswap.apply` and `dev.hotswap.probe` through
`config/commands/dev.def`, with handlers in
`tools/command/native_dev_hotswap.c`:

```sh
zclassic23-dev dev hotswap probe \
  --input='{"so_path":"/tmp/gen.so","probe_leaf":"core.status"}'
```

The module-ABI commands (`dev.hotswap.probe` / `dev.hotswap.apply`) are live
as documented under "Real module ABI" above. The GENERATION
(manifest/staging) path stays contained:

- `make hotswap` refuses before any load or publication; `make hotswap-so`
  builds a read-only, unpublishable candidate only.
- The resident generation loader (`hotswap_load_leaves`) has no production
  caller; staging publication stops before commit.
- `deploy-dev-lane.sh` public activation, the watcher `auto`/`apply` modes,
  and `dev.change.apply` all refuse before generation relinking.

Re-enable staging publication only with a disposable worker, pre-load
sidecar/ELF/import policy, an immutable artifact receipt, and bounded fixtures.

### Eligibility

`config/hotswap_eligible.def` and
`tools/lint/check_hotswap_eligible_scope.sh` enforce eligibility. A listed
translation unit must:

- export one native-leaf table;
- contain no mutable file-scope state;
- live in the app layer, outside every consensus and state root; and
- declare a canonical probe leaf whose body accepts an empty request.

| Native translation unit | Probe leaf |
|---|---|
| `app/controllers/src/status_native_handlers.c` | `core.status` |
| `app/controllers/src/wallet_native_handlers.c` | `core.wallet.address.list` |
| `app/controllers/src/net_native_handlers.c` | `core.network.peers.incidents` |
| `app/controllers/src/meta_native_handlers.c` | `ops.metrics` |
| `app/controllers/src/chain_native_handlers.c` | `core.consensus.utxo.audit` |

`app/controllers/src/diagnostics_native_handlers.c` is not listed. Its current
leaves require non-empty input (`sql` or `pattern`), so an empty-request
generation self-test would fail every load. Before listing any new translation
unit, read the probe body and its argument checks; do not infer empty-input
support from the command name.

### Tests

- `make hotswap-sim` covers rejected-batch zero publication, in-flight old
  generations, atomic multi-provider commit, new-call visibility, and exact
  seed replay.
- `make t ONLY=hotswap_loader` covers paths, datadir policy, the generation
  registry, state dump, release refusal, and the leaf manifest/host contract.
- `make t ONLY=hotswap_simnet` atomically repoints the same fixture leaf across
  successive generations and rejects non-monotonic generation numbers without
  publishing.
- `make t ONLY=command_handler_snapshot` rejects branch-path replacement with
  nothing installed.

### Remaining work

Before staging publication may be enabled,
`hotswap_commit_native_leaves()` must look up the declared probe leaf and invoke
the candidate against a bounded empty request before publish, then validate the
expected output schema. Current protection is the generation self-test plus
the registry's ready/read-only/non-branch/non-duplicate validation.

The CLI also needs to carry its resolved datadir and RPC port through the
request context instead of relying on the development lane's fixed defaults.

## ZVCS auto-anchor

Every warm passed development-cycle verdict—hot-swap, transactional reload, or
docs-only check—auto-anchors through `finish_cycle()` in
`tools/dev/devloop_cycle.c` and `vcs_devloop_anchor_cycle()` in
`lib/vcs/src/vcs_devloop.c`. The ZVCS commit binds the source tree, cycle
verdict, produced binary generation, and agent/session/task metadata.

The first durable snapshot can require thousands of object writes, so it is
kept off the foreground edit verdict. The library detects that a baseline is
needed and returns `vcs_deferred` plus a reason. The interactive dev loop then
launches `vcs_devloop_run_initial_baseline()` through the dev-only detached
worker in `tools/dev/devloop_baseline.c`. The library remains process-spawn
free, enforced by `check-vcs-no-git`.

Subsequent cycles synchronously bind their exact generation. Generated trees,
local agent worktrees, and caches are excluded, and stale stat-cache rows are
deleted during the baseline.

For a transactional reload, the generation is the candidate SHA-256 returned
by the activation engine. A docs-only check records an all-zero generation
hash while still binding source and verdict.

ZVCS failures are fail-open for the development verdict and are reported in
`vcs_error`. A sealed-path refusal is also labeled
`vcs_sealed_refusal:true`; source-integrity signaling never grants runtime
publication authority.

## Transactional reload (contained)

No public publication entry point reaches either retained reload backend.
Environment variables or direct script calls cannot opt into activation.

Both implementations stop the development service, atomically flip the
current-generation symlink, restart, and verify the exact `/proc` executable
identity before success, rolling back on any preflight or verification failure:

- The shell seam runs the contained development deployment script after a fast
  rebuild.
- The native engine calls `dev_activation_run()` with the exact dev-lane
  constants and carries its candidate SHA-256 directly into the ZVCS binding.

The retained selection seam exists for hermetic tests, not as publication
authority. The activation engine is covered by `test_dev_activation`; shared
request construction and result mapping are covered by `test_dev_platform`.

Before any activation entry point is enabled, require immutable source epochs,
proof receipts, signed authority, resident expected-epoch compare-and-swap,
durable prepared/accepted records, exact post-publication probes, deterministic
rollback, and an isolated copy proof.

## Sealed-consensus refusal

The fast loop cannot publish changes under `core/`. A source unseal token does
not authorize hot-swap, reload, staging, or generation relinking.

When a cycle touches sealed consensus code without a token, it exits 3 with a
structured `sealed_consensus_core` refusal that names the paths, manifest,
consensus-parity law, unseal ritual, and elevated proof procedure. Both one-shot
and persistent-watch paths pass through this check before the dev-build gate.

Consensus fixes remain possible through the owner-gated route: record a reason
with `make core-unseal REASON=...`, run full CI and copy proof, deploy through
the owner gate, then reseal with `make core-seal`. The fast loop reads but does
not mint or consume the token, so one unseal covers the iterative work for one
landed change rather than only its first local cycle.

`make t ONLY=dev_platform` covers sealed-path classification, refusal fields,
exit status, persisted verdict, and token-authorized progression.

The durable ZVCS record lives under `.zvcs/`: `commits.log` is the append-only
self-verifying history and `objects/` holds content-addressed data. Read paths
are demonstrated in `lib/test/src/test_vcs_devloop.c`.
