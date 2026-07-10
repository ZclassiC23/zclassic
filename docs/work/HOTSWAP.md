# Tier-1 in-process hot-swap (DEV-ONLY)

An AI agent edits an app-layer MCP controller `.c`, and the change goes live in
a **running dev process** in under a second — no restart — by compiling the
edited translation unit into a "generation" `.so` and `dlopen`'ing it into the
process, then atomically re-pointing the affected MCP routes.

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

## Moving parts

| Piece | Where |
|-------|-------|
| Loader (`hotswap_load`, registry, `zcl_state` dumper) | `lib/hotswap/` |
| Atomic router slots + `mcp_router_replace` | `tools/mcp/router.{c,h}` |
| Driving MCP tool `zcl_agent_hotswap` | `tools/mcp/controllers/dev_hotswap_controller.c` |
| Pilot controller (`ZCL_HOTSWAP_EXPORT_ROUTES`) | `tools/mcp/controllers/app_controller.c` |
| Build targets | `make hotswap-so`, `make hotswap` |
| `zcl_state subsystem=hotswap` | lists loaded generations |

Zero new external dependencies: plain `$(CC) -shared` + libc `dlopen` (`-ldl`,
already linked). No mold/lld/inotify/etc. required.

## How a generation .so is built

A swap-eligible controller invokes `ZCL_HOTSWAP_EXPORT_ROUTES(k_routes,
PARAM_COUNT(k_routes))` once at file scope (no trailing semicolon). Under a
generation build (`-DZCL_HOTSWAP_GEN`) that macro emits the well-known
`zcl_hotswap_gen_init`, which re-points every route the controller owns — the
routes and their handlers resolve to the `.so`'s own freshly-compiled copies.
In the node build and in release the macro expands to nothing, so the symbol
only ever exists inside a `.so`.

At load time the `.so` is opened `RTLD_NOW | RTLD_LOCAL`; its unresolved kernel
symbols (`mcp_node_rpc`, `json_*`, …) bind against the `-rdynamic` executable's
global scope, so the new handler runs against the live node's state.

## Commands

```sh
# Build a generation .so from one (or more) edited controller TUs.
# Prints the .so path as its last line.
make hotswap-so FILES="tools/mcp/controllers/app_controller.c"

# Build + hand it to zcl_agent_hotswap. If a dev node process is up it runs the
# call; otherwise it just prints the exact command (never starts/stops a service).
make hotswap FILES="tools/mcp/controllers/app_controller.c" PROBE=zcl_name_list

# Or drive the tool directly (so_path must be ABSOLUTE, under /tmp or build/hotswap):
build/bin/zclassic23-dev -datadir=/tmp/zcl-dev \
  mcpcall zcl_agent_hotswap '{"so_path":"/abs/path/gen-<ts>.so","probe_tool":"zcl_name_list"}'

# Inspect loaded generations:
build/bin/zclassic23-dev mcpcall zcl_state '{"subsystem":"hotswap"}'
```

`zcl_agent_hotswap` returns `{ok, gen, replaced:[names], probe:{…}}`. It is
flagged destructive (rate-gated, skipped by `zcl_self_test`). The loader refuses
the canonical live datadir (`~/.zclassic-c23`) and legacy `~/.zclassic` as a
belt-and-suspenders check behind the compile gate.

### Which process gets swapped

`mcpcall` runs a **one-shot in-process dispatcher**: it dlopens into that
short-lived process, proves load + replace + probe, then exits. That is the
smoke-test / demo path. A swap that **persists** in a long-running process
happens when the tool is dispatched through *that* process's own MCP server —
e.g. the `-mcp` stdio server an agent is already talking to. Whatever process
dispatches `zcl_agent_hotswap` is the one whose routes change.

## End-to-end demo (no running node required)

```sh
sh tools/scripts/hotswap_demo.sh
```

It builds `zclassic23-dev`, compiles a generation `.so` from the demo fixture
`tools/scripts/hotswap_demo_controller.c` (which re-points `zcl_name_list` to a
handler returning a marker string), dispatches `zcl_agent_hotswap` in one
`mcpcall` process, and asserts the probe result carries the new handler's
marker. Expected tail:

```
{"ok":true,"gen":1,...,"replaced":["zcl_name_list"],...,"probe":{"hotswap_demo":"NEW-GENERATION-HANDLER","tool":"zcl_name_list"}}
PASS: zcl_name_list dispatch returned the new generation handler (NEW-GENERATION-HANDLER)
```

## Tests

- `make t ONLY=mcp_router` — includes `mcp_router_replace` swap + schema tests.
- `make t ONLY=hotswap_loader` — path/datadir predicates, registry, dumper,
  and the release-stub refusal (the test binary is not a `ZCL_DEV_BUILD`).

## Known limits (v1, deliberately deferred)

- **Non-transactional multi-route swap**: if a generation's `gen_init` fails
  partway, already-published routes stay published (they point at valid code in
  the kept-alive `.so`). A stage-then-commit-all protocol is a later refinement.
- **One controller per `.so`**: the generation entrypoint is a single
  well-known symbol, so a `.so` re-points exactly one controller's routes.
- **`TOOLS=` subset filtering** (re-point only named tools) is not wired; a
  generation re-points every route its controller owns.
