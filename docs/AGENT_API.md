# AGENT_API.md — native API for AI coding operators

ZClassic23's agent API is the native binary and the MCP tools backed by the
same native RPC methods. Shell wrappers are compatibility shims only.

## First calls

| need | native command | MCP tool |
|---|---|---|
| Live node status | `zclassic23 agent` | `zcl_agent` |
| Code/docs/test map | `zclassic23 agentmap` | `zcl_agent_map` |
| Changed files to tests/risk | `zclassic23 agentimpact <files...>` | `zcl_agent_impact` |
| Versioned contracts | `zclassic23 agentcontracts` | `zcl_agent_contracts` |
| Fast build contract | `zclassic23 agentbuild` | `zcl_agent_build` |

The native RPC contracts are implemented in
`app/controllers/src/agent_controller.c`. MCP routes in
`tools/mcp/controllers/ops_controller.c` proxy those same native methods. REST
currently exposes the public status contract at `GET /api/v1/agent`.

## Build loop

This is a C23 project, so the edit loop should compile only what changed.

- `make build-only` compiles all node objects without linking. It uses
  `build/obj` plus header depfiles (`-MMD -MP` and included `.d` files), so
  unchanged translation units keep their existing `.o` files and changed
  headers recompile their dependents.
- `make t-fast ONLY=<group>` uses `build/test-obj` and
  `build/bin/test_parallel_fast`, a cached non-LTO test harness for hot-path
  focused tests.
- `make fast-ci` runs `git diff --check`, shell syntax checks, `lint-fast`,
  `build-only`, focused tests inferred from changed files, and a native
  linger-service probe when the service is available. Repeated identical green
  inputs hit `.cache/zcl-agent-fast-ci/` and skip repeated lint/build/focused
  tests while still refreshing the live probe.
- `make fast-ci` auto-selects `sccache cc`, then `ccache cc`, then `cc`.
  Override with `ZCL_FAST_CC='ccache cc'`. Use `ZCL_FAST_JOBS=N`,
  `ZCL_FAST_TESTS=group[,group]`, `ZCL_FAST_STRICT_TESTS=1`, and
  `ZCL_FAST_LIVE=0` as needed. Use `ZCL_FAST_CACHE=0` to force a rerun,
  `ZCL_FAST_CACHE_RESET=1` to clear the green-input cache, or
  `ZCL_FAST_CACHE_DIR=...` to move it.

Before pushing `main`, the strict gate remains `make lint`, `make build-only`,
and the relevant strict `make t ONLY=<group>` tests. The tracked pre-push hook
runs full `make ci`.

## Reproducible build proof

Use `make ci-reproducible` for byte identity. It runs
`tools/scripts/check_reproducible_build.sh`, which builds twice in isolated
`BUILD_DIR`s using `tools/scripts/repro_build_vars.sh`, then compares the
binaries with `cmp`.

The reproducible profile pins `SOURCE_DATE_EPOCH` to the HEAD commit time
unless overridden, forces portable `-march=x86-64-v3`, and disables the linker
build id with `-Wl,--build-id=none`.

## Rule

Do not add new operator logic to `tools/z`. Add native JSON once, expose it via
MCP or REST if needed, document the schema, and cover it with focused tests.
