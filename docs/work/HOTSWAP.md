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
already entered old code stays valid.

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
| v2 manifest + pilot controller (`ZCL_HOTSWAP_EXPORT_ROUTES`) | `lib/hotswap/include/hotswap/hotswap.h`, `tools/mcp/controllers/app_controller.c` |
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

It builds `zclassic23-dev`, compiles the allowlisted app controller into a v2
generation, dispatches `zcl_agent_hotswap` in one exact-dev-datadir `mcpcall`
process, and asserts the transaction commits with source/input/artifact
provenance. It does not alter the running service.
Expected tail:

```
{"ok":true,"gen":1,...,"provider_id":"mcp.routes",...,"artifact_sha256":"..."}
PASS: manifest v2 validated and the complete app route generation committed
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

## Known limits (v2 stateless pilot, fail-closed)

- Only the stateless MCP route provider is admitted. REST and diagnostics were
  removed from eligibility because their independent atomic slots cannot join
  the router transaction. Services, models, storage, events, conditions,
  supervisors, wallet/key state, networking, reducer/consensus, and bootstrap
  ownership are `reload_required`.
- One eligible controller per `.so`; a generated multi-provider aggregator is
  still needed before multiple controller TUs can form one generation.
- There are no state migrations or background callback leases in this pilot.
  That is safe because admitted code is stateless, declares no quiescence, and
  old text is never unmapped. Stateful modules remain refused.
- `dev-watch auto` uses the resident dev RPC by default. Only transport
  unavailability (exit 69, normally an older dev generation) permits automatic
  reload fallback; generation rejection does not. After a successful swap the
  watcher asynchronously builds and preflights an immutable binary generation
  so source and process state can converge at the next transactional reload.

## ZVCS auto-anchor (Wave 2.3, power-station Pillar 2)

Every "passed" `zcl.dev_cycle.v1` verdict — whether the cycle resolved to a
Tier-1 hot-swap (`resident_commit`), a native fast/transactional reload
(`transactional_reload`), or a docs-only `check` — now auto-anchors: the dev
loop's `finish_cycle()` (`tools/dev/devloop_cycle.c`) calls
`vcs_devloop_anchor_cycle()` (`lib/vcs/src/vcs_devloop.c`), which opens (or,
on the very first call, creates) the ZVCS repo at `.zvcs/` beside `.git/` and
takes a snapshot. The commit binds the current source tree to this cycle's
verdict (`status`/`phase`/`elapsed_ms`), the binary generation it produced,
and (from `ZCL_AGENT_ID`/`ZCL_SESSION_ID`/`ZCL_TASK_REF`) the agent/session/
task that drove it — no "remember to commit" step.

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
on failure. The one exception that is surfaced loudly is a **sealed-path
refusal**: editing a file under a sealed glob (see `vcs/vcs_seal.h` —
default set `core/`, `domain/consensus/`, `lib/consensus/`, `lib/validation/`,
`lib/chain/`, `lib/mining/`, `app/jobs/`) without a granted one-shot unseal
token adds `"vcs_sealed_refusal":true` alongside `"vcs_error"`. At this
integration point the refusal is **advisory only** — by the time
`finish_cycle` runs, the cycle's own hot-swap/reload publish has already
happened, so a refused anchor means only "this cycle's source snapshot did
not land," not "the running binary was blocked." Wave 2.4 moves the seal
check earlier, before publish, so a sealed-path edit is refused outright.

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
