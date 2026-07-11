# Command registry definitions

This directory is the authored source for the native LLM command ontology in
[`docs/NATIVE_COMMAND_INTERFACE.md`](../../docs/NATIVE_COMMAND_INTERFACE.md).

Checkpoint status (2026-07-11):

- `root.def`, `core.def`, `apps.def`, and `ops.def` are declared here AND
  wired into the binary: `config/src/command_catalog.c` `#include`s all
  four, expands them into one immutable `g_catalog_commands[]` table via the
  `ZCL_COMMAND_*` X-macros, and binds native handler pointers for this
  build. `zcl_command_catalog()` (declared in `config/command_catalog.h`) is
  called from `tools/command/native_command.c`, which `src/main.c` reaches
  through `zcl_native_command_main()` for any method
  `zcl_native_command_is_root()` recognizes — this is a live dispatch path,
  not a declaration-only exercise.
- `dev.def` is declared here (a branch under `root.def`) but its leaves are
  **not yet bound in `command_catalog.c`** — `command_catalog.c` says so
  directly: *"the `dev` subtree is declared as a branch in root.def but its
  leaves stay owned by the checkout-local devloop dispatcher
  (`tools/dev/devloop_cli.c`)... Do NOT include dev.def before that binding
  exists — its READY leaves name handlers this catalog does not define."*
  Replacing that hardcoded devloop menu with registry wrappers is tracked as
  a follow-on wave; check `config/src/command_catalog.c`'s own header
  comment for the current wave label before assuming it has landed.
- The transport-neutral registry engine compiles in `lib/kernel`
  (`lib/kernel/src/command_registry.c`).

The next implementation work is: bind `dev.def`'s leaves in
`config/src/command_catalog.c` once the registry-wrapper replacement for
`devloop_cli.c` exists; no `lib/` source may include App, controller,
service, or development handler headers. Keep metadata separate from handler
bindings so release discovery can describe the `dev` branch while the release
binary contains no development executor pointers or loader path.

Before marking any leaf `ready`, the catalog validator must prove it has a
handler, input schema, output schema, typed safety policy, allowed lanes, and a
golden dispatch test. A `planned` leaf must have no handler and must fail closed
with a `zcl.result.v1` blocker. Compatibility leaves may name the old command,
but cannot silently fall through to arbitrary RPC.
