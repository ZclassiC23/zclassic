# Command registry definitions

This directory is the authored source for the native LLM command ontology in
[`docs/NATIVE_COMMAND_INTERFACE.md`](../../docs/NATIVE_COMMAND_INTERFACE.md).

Checkpoint status (2026-07-10):

- `root.def`, `apps.def`, and `dev.def` contain the first typed declarations.
- The transport-neutral registry engine compiles in `lib/kernel`.
- These definitions are intentionally **not wired into the binary yet**.
- `core.def` and `ops.def`, the composition-root catalog, generated/common and
  dev-only binding tables, and the native adapter remain to be implemented.

The next implementation must expand the declarations from
`config/src/command_catalog.c`; no `lib/` source may include App, controller,
service, or development handler headers. Keep metadata separate from handler
bindings so release discovery can describe the `dev` branch while the release
binary contains no development executor pointers or loader path.

Before marking any leaf `ready`, the catalog validator must prove it has a
handler, input schema, output schema, typed safety policy, allowed lanes, and a
golden dispatch test. A `planned` leaf must have no handler and must fail closed
with a `zcl.result.v1` blocker. Compatibility leaves may name the old command,
but cannot silently fall through to arbitrary RPC.
