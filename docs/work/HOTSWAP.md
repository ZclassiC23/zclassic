# Tier-1 in-process hot-swap (DEV-ONLY)

An AI agent edits a manifest-eligible stateless MCP controller `.c`, compiles it
into a content-identified generation `.so`, and loads it into a **running dev
node's resident MCP router** without a restart. Generation v2 stages every route, validates the
complete set and ABI manifest, runs its self-test, then publishes one immutable
router snapshot with one atomic store. Any failure publishes zero routes.

**This is dev-only. The release binary is 100% static with zero `dlopen`.**
Every dynamic-loading line lives behind `#ifdef ZCL_DEV_BUILD`; a release build
compiles the whole load path down to a stub that returns
`hotswap unavailable in release build`. Enforced by the `check-hotswap-dev-only`
lint gate (no `dlopen`/`dlsym`/`dlclose` outside `lib/hotswap/`, and inside
`lib/hotswap/` only within a `ZCL_DEV_BUILD` region).

## ⚠️ Ephemerality

A hot-swap lives only for the lifetime of the process it was loaded into, and
only in that process. **It reverts on restart.** To make a change permanent,
rebuild normally (`make dev-bin` / `make deploy-dev`) and commit the source.
Generations are never `dlclose`'d (a deliberate leak) so an in-flight call that
already entered old code stays valid. Each accepted generation also retains
the descriptor for the exact inode loaded through `/proc/self/fd/N`. Reusing
that descriptor number while an older handle remains mapped can make `dlopen`
return the older cached object for a different path; the pinned descriptor is
therefore part of generation identity, not an accidental file leak.

`dev-watch auto` starts an asynchronous immutable binary build and preflight
stage after a successful swap. That removes compilation from the foreground
latency path, but the staged binary is not silently promoted: the next
transactional reload still verifies it and preserves rollback semantics.

## Moving parts

| Piece | Where |
|-------|-------|
| Loader (`hotswap_load`, registry, `zcl_state` dumper) | `lib/hotswap/` |
| Immutable router snapshots + transactional batch commit | `tools/mcp/router.{c,h}` |
| Driving MCP tool `zcl_agent_hotswap` | `tools/mcp/controllers/dev_hotswap_controller.c` |
| v2 manifest + `ZCL_HOTSWAP_EXPORT_ROUTES` macro | `lib/hotswap/include/hotswap/hotswap.h` |
| Eligible controller TUs (each exports its own `k_routes[]`) | `tools/mcp/controllers/{app,meta,chain,net,wallet}_controller.c` |
| Runtime eligibility allowlist | `config/hotswap_eligible.def` |
| Persistent dev RPC bridge | `tools/mcp/dev_rpc_bridge.{c,h}` (`dev_hotswap`, read-only `dev_mcp_call`) |
| Build/verification targets | `make hotswap-so`, `make hotswap`, `make hotswap-sim` |
| `zcl_state subsystem=hotswap` | `zcl.hotswap_generation.v2` provenance/status |

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

# Build + commit inside the already-running isolated dev node. This never
# starts/stops a service and fails when that resident RPC is unavailable.
make hotswap FILES="tools/mcp/controllers/app_controller.c" PROBE=zcl_name_list

# Or drive the persistent RPC directly (exact dev datadir; .so under /tmp or
# build/hotswap):
build/bin/zclassic23-dev -datadir="$HOME/.zclassic-c23-dev" -rpcport=18252 \
  dev_hotswap /abs/path/gen-<sha256>.so zcl_name_list

# Inspect the resident generation through the non-destructive bridge:
build/bin/zclassic23-dev -datadir="$HOME/.zclassic-c23-dev" -rpcport=18252 \
  dev_mcp_call zcl_state '{"subsystem":"hotswap"}'

# Fast deterministic three-node generation/network replay:
make hotswap-sim
```

`zcl_agent_hotswap` returns `{ok, gen, replaced:[names], probe:{…}}`. It is
flagged destructive (rate-gated, skipped by `zcl_self_test`). The loader refuses
everything except the exact dev datadir (`~/.zclassic-c23-dev`), including
canonical, legacy, soak, copied/test, empty, and arbitrary datadirs.

### Which process gets swapped

`make hotswap` sends the authenticated `dev_hotswap` JSON-RPC request to the
already-running node at the exact dev datadir/port. That handler dispatches
`zcl_agent_hotswap` against a resident MCP router in the same process, so later
`dev_mcp_call` requests observe the committed generation. The generic bridge
refuses destructive MCP tools; only the narrow `dev_hotswap` method may mutate
the resident generation. Release, canonical, soak, test, copy, and arbitrary
datadirs do not register either RPC method.

The separate `mcpcall zcl_agent_hotswap ...` spelling still runs a one-shot
helper process and is useful only as a node-free loader smoke test; it must not
be described as a persistent swap.

## End-to-end demo (no running node required)

```sh
sh tools/scripts/hotswap_demo.sh
```

It builds `zclassic23-dev` once, then for EVERY eligible controller in
`config/hotswap_eligible.def` compiles it into a v2 generation, dispatches
`zcl_agent_hotswap` in one exact-dev-datadir `mcpcall` process, and asserts the
transaction commits with source/input/artifact provenance and re-points a
read-only route that TU owns. A harness sync-check fails loud if an eligible TU
has no proof entry (or vice-versa). It does not alter the running service.
Expected tail:

```
PASS: tools/mcp/controllers/app_controller.c committed a v2 generation re-pointing zcl_name_list
...
== hotswap_demo: 5 passed, 0 failed over eligible TUs ==
```

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
- `dev-watch auto` uses the resident dev RPC by default. Only transport
  unavailability (exit 69, normally an older dev generation) permits automatic
  reload fallback; generation rejection does not. After a successful swap the
  watcher asynchronously builds and preflights an immutable binary generation
  so source and process state can converge at the next transactional reload.

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
writes, so it is never allowed to delay an edit verdict. The first green cycle
queues a singleton detached, generation-neutral baseline and returns
`"vcs_deferred":true` plus an explicit `vcs_error` saying that cycle is
unanchored. Once the baseline completes, subsequent cycles synchronously bind
their exact generation as above. Generated build trees, local agent worktrees,
and caches are pruned from the tracked walk, and stale stat-cache rows are
deleted during the background baseline so steady-state cost follows the real
source change set.

**Generation binding, per cycle type:**
- Hot-swap (`resident_commit`): the `artifact_sha256` field already returned
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
integration point the refusal is **advisory only** — by the time
`finish_cycle` runs, the cycle's own hot-swap/reload publish has already
happened, so a refused anchor means only "this cycle's source snapshot did
not land," not "the running binary was blocked." This ZVCS-side check remains
as a defense-in-depth backstop; the **pre-publish core refusal** below (Wave
2.4) is the one that structurally blocks the running binary.

## Core refusal — the fast loop cannot auto-publish sealed consensus (Wave 2.4)

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
