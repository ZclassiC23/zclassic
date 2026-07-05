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
| Preferred interface contract | `zclassic23 agentinterface` | `zcl_agent_interface` |
| Deploy/restart guard | `zclassic23 agentdeployguard [action]` | `zcl_agent_deploy_guard` |

The native RPC contracts are implemented in
`app/controllers/src/agent_controller.c`. MCP routes in
`tools/mcp/controllers/ops_controller.c` proxy those same native methods. REST
currently exposes the public status contract at `GET /api/v1/agent`.

## Preferred Interface

The best interface for an AI coding operator is MCP with typed JSON tools:
start with `zcl_agent_interface`, then use `zcl_agent`, `zcl_agent_impact`,
`zcl_agent_build`, `zcl_state`, `zcl_node_log`, and `zcl_sql` as needed. The
native binary commands (`zclassic23 agentinterface`, `zclassic23 agent`, etc.)
are the second-best interface for terminal work and scripts. REST is the
public read-only mirror.

The transport can vary, but the payload should not: AI-facing status surfaces
return stable JSON objects with a `schema` or an explicit command contract, and
they must identify the running node binary with `build_commit` whenever deploy
drift would change the interpretation of state. `getmirrorstatus` follows this
rule so an operator can distinguish a stale runtime binary from current source
or a freshly deployed dev lane.

`zclassic23 agentinterface` / `zcl_agent_interface` is the machine-readable
entry point for that rule. In addition to the human summary, it emits a
top-level `build_commit`, a `runtime_identity` block for the binary that
produced the interface contract, a `capabilities[]` matrix that names each
first-class agent operation, its schema, and its native/MCP/REST transport, plus
a `machine_contract` block declaring that payloads are JSON objects with
required `schema`, `api_version`, and `status` fields. Those nested shapes are
versioned as `zcl.agent_runtime_identity.v1`, `zcl.agent_capability.v1`, and
`zcl.agent_machine_contract.v1`. Future operator APIs should extend that matrix
before adding new wrapper behavior.

No Python is required to consume the preferred agent API. Contract assembly,
status interpretation, changed-file test mapping, and deploy safety decisions
belong in C under `app/controllers/src/agent_controller.c` and
`app/controllers/src/agent_interface_controller.c`, then get exposed through
MCP/native/REST.

## Command Center

The first-call operator view is `zcl_operator_summary` through MCP, or
`zclassic23 agent` through the native binary. It returns the stable status,
the running binary `build_commit`, height/gap, peer summary, active blockers,
next action, and recommended drill-down tools. The compact packet also includes
reducer frontier telemetry, download queue/in-flight/throughput counters,
recent error state, and precise download age fields:
`download.oldest_in_flight_age_seconds`,
`download.oldest_in_flight_height`,
`download.oldest_in_flight_peer_id`,
`download.overdue_in_flight`, and
`download.in_flight_peer_count`. It also reports
`download.queue_peer_avoid_count` and
`download.queue_peer_avoid_max_seconds` when timed-out block bodies are queued
for immediate retry by other peers while temporarily avoiding the peer that just
failed that exact hash. It reports `download.catchup_stalled` when a
lagging node has active download work but the served frontier has not advanced
for the stall window. It reports `download.dispatch_idle`,
`download.dispatch_stalled`, and `download.dispatch_idle_seconds` when queued
block work is waiting but no block request is currently in flight, so the AI
operator can tell dispatch starvation from ordinary catch-up. Assignment-loop
telemetry is in `download.assign_attempts`, `download.assign_successes`,
`download.assign_zero_results`, and the `download.last_assign_*` fields; the
string `download.last_assign_result` names the last planner result
(`assigned`, `no_queue`, `peer_window_full`, `global_window_full`, `no_slot`,
etc.). `download.dispatch_wakes` counts gap-fill wakeups of the connman message
dispatcher after queue refill or timeout requeue work. Stale in-flight requests
are swept both by the peer send loop and by the supervised gap-fill worker; when
the worker requeues timed-out blocks it wakes the C-native peer dispatcher, so a
connected-but-silent peer cannot own block requests forever. If gap-fill sees
queued body work with zero in-flight requests on a duplicate/no-op refill pass,
it also wakes the dispatcher; queued work is never allowed to sit idle because a
previous wake raced the message-handler wait. `download.message_cycles`,
`download.message_send_calls`, `download.message_process_calls`,
`download.message_recv_ready`, `download.message_idle_waits`, and
`download.message_wakes` expose the connman loop itself. The peer message cycle
sends outbound work before inbound processing, then yields from inbound
processing after a bounded batch (`ZCL_MSG_PROCESS_MAX_PER_CYCLE`) so the
outbound send/assignment phase keeps running even under a large receive backlog
or slow local reducer work. Use `zcl_status`
for the larger health packet, `zcl_state` for subsystem internals,
`zcl_node_log` for bounded log search, `zcl_sql` for SELECT-only database
inspection, and `zcl_events` for recent structured events.

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

The same public status contract includes `resources`
(`zcl.node_resources.v1`) with cheap process-level RSS, RSS warning threshold,
Linux cgroup memory usage/limits when available, memory pressure (`ok`,
`watch`, `warn`, or `unknown`), `memory_pressure_detail`, pressure basis,
uptime, and source. Cgroup/systemd pressure is preferred over raw RSS because
canonical can have a large steady RSS while still running comfortably below its
service memory guardrails. When cgroup v2 `memory.stat` is available, the node
also reports anon/file/kernel buckets, reclaimable bytes, working set, and
working-set percentages against `memory.high` / `memory.max`. With a cgroup
`memory.high` limit, `watch` starts at 85% of total cgroup usage. `warn` starts
at 95% only when the unreclaimable working set is also near the limit; a high
total caused mostly by reclaimable file cache remains `watch` with
`memory_pressure_detail="cgroup_reclaimable_cache_high"`. Without
`memory.high`, the max limit gives an 80% watch and 90% warn fallback. This is
the first-call place to notice a lane that is still serving but approaching
memory pressure, before reaching for shell-only systemd probes.

The same contract also includes `restart_watchdog`
(`zcl.restart_watchdog.v1`). It is the cheap first-call view over the chain tip
watchdog's bounded restart memory: whether the watchdog is registered, whether
an autonomous no-progress recycle happened in the current episode, the stuck
height anchoring that episode, restart count, restart budget remaining, and the
deep drill-down command (`zclassic23 dumpstate chain_tip_watchdog`). A recent
controlled liveness recycle appears as
`last_restart_autonomous=true`,
`last_restart_reason="no_progress_tip_stall"`, and
`status="restart_budget_burning"`, which lets agents distinguish a deliberate
watchdog remedy from a crash or manual soak rebaseline without scraping
`node.log`.

The top-level status always reports exact `gap`, `index_gap`, and
`target_height`, but it treats normal live-tip churn of up to 10 blocks as
serving-health-compatible. Larger gaps still become `chain_gap` /
`download_queue_idle` and can set `operator_needed`.

`make deploy` is guarded by `tools/deploy_guard.sh canonical-deploy`. The guard
calls the running node's C-native `agentdeployguard` RPC
(`zcl.agent_deploy_guard.v1`) when available and falls back to the systemd
`-operator-lane=` flag only for older running binaries. An active canonical
lane is refused by default; set `ZCL_DEPLOY_ALLOW_CANONICAL=1` only for a
deliberate canonical restart window. Development builds should normally use
`make deploy-dev`.

`make lane-health` is the read-only redundancy check for the canonical, soak,
and dev lanes. `make lane-recover LANE=dev` or `LANE=soak` plans a bounded
noncanonical repair as `zcl.lane_recovery_plan.v1`; add
`ZCL_LANE_RECOVERY_APPLY=1` only when it is acceptable to restart that
noncanonical unit. The recovery tool refuses `live`, `canonical`, and `main`,
never writes the canonical datadir, and uses the optional
`ZCL_LANE_SNAPSHOT_LOADER_FLAG` systemd hook in the dev/soak units instead of
rewriting long command lines. If the snapshot is ahead of the lane's served
height, the plan imports headers first with the documented
`--importblockindex $HOME/.zclassic` step; `--import-headers` /
`ZCL_LANE_RECOVERY_IMPORT_HEADERS=1` forces that step for a loader-equipped
noncanonical lane that is still pre-RPC. Forced import is skipped when the
selected snapshot is not newer than the lane height, unless
`ZCL_LANE_RECOVERY_ALLOW_STALE_HEADER_IMPORT=1` is set. The import is bounded by
`ZCL_LANE_RECOVERY_IMPORT_TIMEOUT` (default 1200 seconds).

When lane RPC is reachable, `make lane-health --json` also consumes the native
`agent` contract and exposes `agent_build_commit`,
`agent_contract_trusted`, `agent_status`, `agent_operator_needed`,
`agent_primary_blocker`, `agent_next`, `agent_validation_pack_ok`, and
`agent_validation_pack_detail`. A current native `agent` contract
(`agent_contract_trusted=true`, which requires `build_commit`) with `blocked`
or `operator_needed=true` makes the lane `status=fail` and clears role
readiness, even if basic peer/height/listener checks still look fine. Older
compact agent responses are still printed but do not override lane status by
themselves; lane health falls back to `condition_engine` operator-needed pages
for those runtimes. This keeps the shell lane summary subordinate to the
C-native API without letting stale wrapper-era responses hide or invent a
validation-pack hold.

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
- Focused test routing is DRY: both native `zclassic23 agentimpact` and
  `tools/agent_fast_ci.sh` read
  `app/controllers/include/controllers/agent_impact_rules.def`. Add a rule
  there first, then verify `agentimpact` reports `shared_rule_hits > 0`.
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
Do not require Python for the preferred operator API path.
