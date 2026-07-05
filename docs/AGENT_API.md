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

## Command Center

The first-call operator view is `zcl_operator_summary` through MCP, or
`zclassic23 agent` through the native binary. It returns the stable status,
height/gap, peer summary, active blockers, next action, and recommended
drill-down tools. Use `zcl_status` for the larger health packet, `zcl_state`
for subsystem internals, `zcl_node_log` for bounded log search, `zcl_sql`
for SELECT-only database inspection, and `zcl_events` for recent structured
events.

Every new subsystem that has runtime state should expose it through the
diagnostics registry and become reachable through `zcl_state`. Expensive
development proof state belongs in a named background quality lane with a JSON
verdict, not in an untracked terminal scrollback.

## Operator Lane

`zclassic23 agent`, REST `GET /api/v1/agent`, and MCP
`zcl_operator_summary` include `operator_lane`
(`zcl.operator_lane.v1`). The lane is declared by the node's own boot context
(`-operator-lane=canonical|soak|dev|test|copy`, or `ZCL_OPERATOR_LANE`) and
reports the lane name, runtime profile, datadir, ports, and machine-readable
restart policy.

Use it to distinguish the long-running canonical node from the pinned soak lane
and the restartable development lane. Tooling should branch on the booleans
`canonical`, `soak_evidence`, `development`, and `ephemeral`, not on systemd
unit names or comments.

The lane object also includes `deployment_safety`
(`zcl.operator_deployment_safety.v1`). Automation must read this nested contract
before any restart or binary deployment:

- `automation_restart_ok` / `automation_deploy_ok` — whether an agent loop may
  do the action without an operator confirmation.
- `requires_operator_confirmation` and `guard_env` — the explicit stop sign for
  canonical and soak lanes.
- `protects_public_endpoint`, `counts_for_soak_hours`, and
  `isolated_from_canonical_datadir` — why the lane exists.
- `preferred_deploy_target` and `safe_default_action` — where fresh code should
  go when the current lane must not be disturbed.

Canonical defaults to observe-only, soak defaults to preserving the evidence
window, and dev defaults to `deploy_dev_lane`.

`make deploy` is guarded by `tools/deploy_guard.sh canonical-deploy`. The guard
queries the running node's native `deployment_safety` object when RPC is
reachable and falls back to the systemd `-operator-lane=` flag for older
running binaries. An active canonical lane is refused by default; set
`ZCL_DEPLOY_ALLOW_CANONICAL=1` only for a deliberate canonical restart window.
Development builds should normally use `make deploy-dev`.

## Bootstrap Service Status

Use `zcl_bootstrapstatus` (or raw RPC `bootstrapstatus`) before claiming a
zclassic23 node is helping fresh peers bootstrap. The response is versioned as
`zcl.bootstrap_status.v1` and separates two surfaces:

- `legacy_p2p_bootstrap`: ordinary full-node serving over `version`,
  `getheaders`, `getdata`, `getaddr`, and related P2P messages. This is the
  path used by zclassicd when its beta6 snapshot bootstrap is disabled with
  `-bootstrap=0`, or after its snapshot stage has completed.
- `beta6_snapshot_bootstrap`: the zclassicd v2.1.2-beta6 fast-bootstrap
  snapshot protocol. A compatible server must advertise `NODE_BOOTSTRAP`
  (`1 << 24`) and answer `getbsman/bsman`, `getbschk/bschk`,
  `getbspman/bspman`, and `getbspchk/bspchk`. zclassic23 must not advertise
  that bit until the matching C service is implemented.

The key booleans are `serving_p2p_bootstrap`,
`serving_addr_bootstrap`, `serving_snapshot_bootstrap`,
`zclassicd_beta6_p2p_compatible`, and
`zclassicd_beta6_fast_bootstrap_compatible`. `blockers[]` names missing
requirements such as `not_listening`, `provable_tip_not_published`, or
`beta6_NODE_BOOTSTRAP_not_advertised`.

The same response also includes `snapshot_loader` (`zcl.snapshot_loader.v1`),
the binary-owned recovery contract for the node's own fast-start bundle:
datadir, highest `utxo-seed-<h>.snapshot`, seed height, matching
`block_index.bin`, failed marker, active `-load-snapshot-at-own-height` path,
and `recovery_hint` (`loader_active`,
`restart_with_load_snapshot_at_own_height`, `install_tip_seed_snapshot`, etc.).
Operational scripts should consume this versioned C API instead of scraping
systemd command lines whenever the node RPC is reachable.

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

Before pushing `main`, the tracked pre-push hook computes the exact
`origin/main..HEAD` changed-file set, rejects non-`main` remote refs, and runs
`make pre-push-ci`. That command uses cached `make t-fast ONLY=<group>` tests
selected by `tools/agent_fast_ci.sh`, plus `make build-only` for compiler and
`-Werror` coverage; it does not rerun the full suite when the changed files only
require narrower coverage. It also sets `ZCL_FAST_LIVE=0`, so an already-running
node condition is visible through telemetry but does not block a code push. Set
`ZCL_FAST_STRICT_TESTS=1` when a change needs strict whole-harness focused
tests. Full-suite, fuzz, and coverage evidence belongs to the background quality lanes: install them with
`make install-quality-linger` and inspect them with `make quality-linger-status`.
Status JSON is written under `~/.local/state/zclassic23-quality`.

## Background proof lanes

The background lanes keep expensive proof work running without blocking every
push or AI edit loop.

- `zclassic23-fuzz.timer` runs `tools/scripts/background_quality_lane.sh fuzz`
  hourly. Default duration is 900 seconds per fuzzer; override with
  `ZCL_FUZZ_DURATION`.
- `zclassic23-coverage.timer` runs
  `tools/scripts/background_quality_lane.sh coverage` weekly.
- `zclassic23-test-suite.timer` runs
  `tools/scripts/background_quality_lane.sh tests` hourly.
- `make quality-linger-status` prints timer status plus the latest
  `zcl.background_quality_status.v1` JSON verdict.

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
