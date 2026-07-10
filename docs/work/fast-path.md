# The Fast Path — our information algorithm for getting to correct C

Mantra: **code fearlessly; immutable history is the oracle.** ZClassic's
canonical history is not at risk from local experiments. Spend that advantage:
test against real historic blocks and throwaway datadir copies, rebuild derived
state instead of preserving bad local artifacts, and let real-chain canaries
answer consensus questions quickly. The hard line remains: never prove a repair
by mutating the live serving datadir first.

The C diff that fixes a problem is usually small (the reducer un-wedge was ~40
lines; the service_state driver ~180) — the **information algorithm** is the
work: turning a vague live symptom into the one correct small change, with
confidence, without expensive wrong turns. Each stage below makes one historical
failure class structurally impossible: the bodies-vs-coins misdiagnosis (a
design cycle on the wrong cause), and the 3,130,701 → 47,279 chain reset (a fix
that deleted tip_finalize_log rows, shipped without a reset-safe test).

## The stages (scale down for trivial changes — don't 9-agent a 3-line edit)

1. **GROUND in live truth — before any hypothesis.**
   `make diagnose-gap SLUG=<x>` dumps the three orthogonal views that must
   agree at tip — active public tip (A), best header tip (H), applied coins
   tip (C), and HAVE_DATA at A+1 (D) — plus the operational mode and active
   Conditions, and prints a root-cause verdict. STOP rule: **no hypothesis
   until the triple is captured.** This kills the bodies-vs-coins class:
   `C << A` is coins-application lag (reconcile), not a body gap; `A < H` with
   `D=true` is bodies-present-not-connected, not body-fetch.

2. **DESIGN + adversarially critique — before code.** For consensus-critical
   changes (anything touching `tip_finalize`, `*_log`, `coins_best`,
   `connect_block`, `active_chain_tip`, the boot reconcile span, or import /
   cold-import), write the design first and have it refuted. The load-bearing
   stamp: prove `can_reset_tip=false` and `weakens_gate=false`.

3. **RESET-SAFE UNIT TEST — before live.** Mirror
   `lib/test/src/test_stage_reducer_unwedge.c`: synthesize the broken state,
   assert the invariant the fix must hold (e.g. the public tip never drops
   below `coins_best`; no `*_log` rows deleted). Run it with
   `make t ONLY=<group>` (seconds).

4. **REPRODUCE ON A COPY — before touching the live chain.**
   `make repro-on-copy SLUG=<x> ARGS='...'` snapshots the live datadir to a
   throwaway copy and runs the node against it on an isolated port. It is a
   tip-regression detector: it FAILS LOUD if the public tip collapses — so the
   catastrophic tail (the 47,279 reset; the import-reset to ~199) is caught on
   a copy, never on the live node. For recovery work, make it a real H* climb
   gate too: `make repro-on-copy SLUG=<x> CLIMB_PAST=<height> ARGS='...'`
   FAILS if the copy only boots/holds flat and never serves a provable tip
   strictly above `<height>`. `-refold-from-anchor` proofs must use
   `REPRO_FULL=1` because the fold reads block bodies, and the harness now
   refuses that flag unless an anchor snapshot candidate is reachable at
   `$ZCL_MINT_ANCHOR_OUT` or `<src>/utxo-anchor.snapshot`. A refold proof also
   has to prove the boot loaded the SHA3-verified MINTED snapshot and has to
   observe H* at/below the gate before crossing it; starting above the gate is
   not a climb proof. The live datadir is never written.

5. **VERIFY + commit.** `make t ONLY=<group>` (inner loop) → `make build-only`
   / `make syntax-check` (does it compile) → `make lint` (full gates) → commit.

## The fast inner loop (use these, never `build/bin/test_zcl` in the loop)

| command | what |
|---|---|
| `make t ONLY=<group>` | run ONE test group, rebuilding the harness first (closes the stale-`test_parallel` rebuild trap). The harness is now a cached per-TU build (`build/test-rel-obj/`, strict `-O3 -Werror -pedantic -DZCL_TESTING`, non-LTO), so this rebuild is one changed-TU recompile + one link (~2 s), not a ~1,300-TU whole-program recompile (~90 s) — and header/`.def` edits are depfile-tracked, so they can no longer false-green |
| `make t-fast ONLY=<group>` | hot-path ONE test group via cached per-file test objects and a non-LTO, non-`-Werror`, `-O1` harness (`build/test-obj/`); loosest/fastest loop |
| `make test_parallel_wpo` | rebuild the original whole-program LTO test binary at `build/bin/test_parallel_wpo` — only to debug a suspected per-TU-vs-LTO divergence |
| `make fast-changed-compile` | cheapest guarded compile check: direct changed `.c` dev-object compile, direct `.h`/`.def` depfile dependents after warm-up, safe fallback for graph-wide edits |
| `make fast-compile` | fastest no-link dev compile check using cached non-LTO `build/dev-obj` objects |
| `make build-only` | strict release-flag incremental compile-check of the whole node (no link) |
| `make dev-bin` | incremental non-LTO node executable at `build/bin/zclassic23-dev`; local AI/operator iteration only, not for release/deploy |
| `make agent-doctor` | no-build combined build/dev-lane/recent-test-failure status with one next safe command |
| `make agent-dev-status` / `zclassic23 agentdevstatus` / `zcl_agent_dev_status` | no-build read-only dev-lane status: service, RPC/pre-RPC recovery, staged binary, saved deploy state, auto-reindex marker, deploy blocker/reason, stale-marker candidate, next action |
| `make agent-clear-stale-dev-reindex` | archive a proven-stale dev-lane `auto_reindex_request` after RPC height is at/above the marker anchor; no restart, no canonical/soak mutation |
| `make agent-stage-dev` | build and atomically stage `~/.local/bin/zclassic23-dev` for the next dev-lane restart without stopping the running service |
| `make syntax-check` | full no-link syntax check across every TU |
| `make lint-fast` | the 5 highest-signal lint gates (full `make lint` before commit) |
| `make agent-plan` | no-build JSON decision packet: changed files, selected focused tests, changed-compile plan, fast-cache hit/miss, dev-lane stage/deploy commands, and MCP shortcuts |
| `make agent-loop` | one-command agent loop: fast-ci checks by default; `ZCL_AGENT_LOOP_BIN=1` also links the dev binary; `ZCL_AGENT_LOOP_DEPLOY=dev` hot-swaps the dev lane |
| `make fast-ci` | cache-aware agent loop: `lint-fast` + changed compile gate + focused tests inferred from changed files + native linger-service probe; identical green inputs skip repeated lint/build/focused tests |
| `make immutable-history-canaries` | fast real-chain consensus KATs: h=478544 oversized canonical transaction plus consensus parity pins |
| `make agent-mcp-call-hot TOOL=<tool>` | no-build typed MCP read through the existing source-tree dev binary |
| `make agent-mcp-call-dev TOOL=<tool>` | no-build typed MCP read through the installed `zcl23-dev` linger-lane binary |
| `make pre-push-ci` | bounded push gate: cached focused fast-ci for changed files with `ZCL_FAST_COMPILE=strict` |
| `make install-quality-linger` | install background full-test, fuzz, and coverage user timers |
| `make quality-linger-status` | show latest background tests/fuzz/coverage JSON verdicts |
| `make test` | the fast fork-based parallel suite (~1 min), now built from the cached per-TU `test_parallel` (incremental after the first build); `make test-full` is the slow single-process binary |
| `make ci-reproducible` | build-twice byte-identity proof in isolated build dirs |

`make agent-plan` is the read-only preview of the loop: it emits
`zcl.agent_fast_plan.v1` with the changed-file set, focused test groups,
changed-compile decision, green-input cache verdict, dev-lane stage/deploy
commands, and the no-build MCP shortcuts. `make agent-doctor` embeds that same
plan alongside dev-lane health and recent focused-test failures.

`make immutable-history-canaries` is the fast consensus-risk lane for the
immutable ZClassic chain. It runs the pinned h=478544 125,811-byte transaction
fixture in `domain_consensus_tx_structural` and the golden
`consensus_parity` group. It is the first gate for bounded consensus predicate
changes; the heavier real-chain replay gates remain `make replay-canary-anchor`
and `make replay-canary-genesis`.

`make agent-loop` is the default edit-loop command for agents and operators. It
delegates to `make fast-ci` for the safe checks, then optionally links the
runnable dev binary with `ZCL_AGENT_LOOP_BIN=1`, stages the dev-lane binary
without restarting with `ZCL_AGENT_LOOP_DEPLOY=stage`, or hot-swaps the dev lane
with `ZCL_AGENT_LOOP_DEPLOY=dev`. `make fast-ci` remains the underlying cache-aware
gate and auto-selects `sccache cc` or `ccache cc` when present; override with
`ZCL_FAST_CC='ccache cc'`. It runs focused tests through `make t-fast`, which
uses `build/bin/test_parallel_fast`: a cached per-file, non-LTO test harness
that rebuilds only changed test/node objects after the first warm-up. It is
non-`-Werror`; compile warning enforcement stays in `make build-only`, strict
`make t`, and full CI. By default it uses `ZCL_FAST_COMPILE=changed`, which
runs `make fast-changed-compile`: changed node `.c` files compile directly into
`build/dev-obj/`, and once `build/dev-obj/.complete` exists, narrow `.h`/`.def`
edits compile only the dev objects whose depfiles mention that dependency.
Template/Makefile/removed-file/unwarmed-depfile/broad edits fall back to
`make fast-compile`. Use `ZCL_FAST_COMPILE=dev` to force the full
dev-object compile, or `ZCL_FAST_COMPILE=strict` when you want `make build-only`; `make pre-push-ci`
sets that automatically. Force focused test groups with
`ZCL_FAST_TESTS=make_lint_gates,mcp_controllers`; use `ZCL_FAST_STRICT_TESTS=1`
to pay the strict `make t` whole-harness rebuild. Set parallelism with
`ZCL_FAST_JOBS=N` (default caps at 16). Set
`ZCL_FAST_CHANGED_COMPILE_LIMIT=N` to control when many direct dev objects fall
back to `fast-compile` (`0` disables the limit). Set
`ZCL_FAST_CHANGED_FILES_ONLY=1` when `ZCL_FAST_CHANGED_FILES[_FILE]` is already
the exact semantic input, as the pre-push hook does. Successful runs write a content
fingerprint under `.cache/zcl-agent-fast-ci/` covering changed-file contents,
selected test groups, compiler/cache choice, strict/live knobs, core scripts,
the Makefile, and the native probe binary mtime. A repeat with the same
fingerprint logs `fast result cache hit` and skips `lint-fast`, the selected
compile gate, and focused tests; it still refreshes the live service probe unless
`ZCL_FAST_LIVE=0` is set. Disable this with `ZCL_FAST_CACHE=0`, reset it with
`ZCL_FAST_CACHE_RESET=1`, or move it with `ZCL_FAST_CACHE_DIR=...`. The live
check uses the C binary first (`build/bin/zclassic23 agent` +
`build/bin/zclassic23 healthcheck`) against the linger service; override the
binary with `ZCL_FAST_NODE_BIN=...` or skip the live check with
`ZCL_FAST_LIVE=0` for isolated/offline work. The shell gate trusts the native
`zcl.public_status.v1` status/serving/operator-needed contract rather than
re-encoding height-gap policy, and emits compact JSON summaries when a probe
fails. There is no `tools/z` fallback in
the agent fast path; if the native binary JSON interface is unavailable, rebuild
the binary or skip the live probe explicitly. Unmapped C/header/source-tree
changes fail closed until you either add a focused-test mapping or pass
`ZCL_FAST_TESTS=...`. The focused-test map is shared with native
`zclassic23 agentimpact` in
`app/controllers/include/controllers/agent_impact_rules.def`; keep new mappings
there so the CLI, MCP tool, and fast-CI shell lane do not drift.

Use `make dev-bin` when you need to run a changed node/agent CLI locally without
paying the release build's whole-program LTO pass. It emits
`make fast-rebuild` builds `build/bin/zclassic23-dev` from cached per-file objects, with default
`ZCL_DEV_OPT=-Og`, hot consensus/crypto/script/validation buckets at
`ZCL_DEV_HOT_OPT=-O2`, no LTO, no strip, and optional fast-linker selection via
`ZCL_DEV_LINKER` (auto-detects `mold` or `ld.lld` when present; override with
`ZCL_DEV_LINKER=` to force the platform default). When `sccache` or `ccache` is
installed, the Makefile auto-wraps `CC` with it unless `ZCL_USE_CCACHE=0` is set. This is
the right binary for
local `agentbuild`, `agentimpact`, parser, API, and diagnostics iteration; it is
not a deploy or release artifact.

The native build contract is discoverable with `build/bin/zclassic23 agentbuild`
or MCP `zcl_agent_build`; it advertises `make agent-plan`, the stage-without-restart
path, and the same MCP shortcuts.

Typed MCP tools are also callable through the same binary for terminal agents.
In the source tree, prefer `make agent-mcp-call TOOL=zcl_status` and
`make agent-mcp-call TOOL=zcl_state ARGS='{"subsystem":"supervisor"}'` for
fresh-code smoke checks; the target refreshes `build/bin/zclassic23-dev` before
dispatch. For routine read-only status/API checks, use
`make agent-mcp-call-hot TOOL=<tool>` to skip rebuilding and reuse the existing
source-tree dev binary, or `make agent-mcp-call-dev TOOL=<tool>` to query the
installed `zcl23-dev` linger lane with its dev datadir and RPC port. Direct
`build/bin/zclassic23 mcpcall zcl_status` is the underlying release-binary path
after `make zclassic23` or deploy. Do not add Python, shell, or helper-binary
wrappers for new agent workflows.

Canonical operator APIs, in priority order:

1. `build/bin/zclassic23 agentmap`, `agentlanes`, `agentliveness`, `agentimpact`,
   `agentbuild`, `agent`, `healthcheck`, `mcpcall <tool> [json]`, and raw RPC
   methods — native C binary client to the running linger service.
2. MCP tools (`zcl_agent_map`, `zcl_agent_lanes`, `zcl_agent_liveness`,
   `zcl_agent_impact`, `zcl_agent_build`, `zcl_agent`, `zcl_status`,
   `zcl_state`, `zcl_node_log`, `zcl_sql`) — typed agent interface over the
   same node RPC truth.
3. REST (`/api/v1/agent`, `/api/v1/openapi`) — public web/API surface.
4. `tools/z` — deprecated shell compatibility for older terminals and scripts,
   not an agent interface. Keep it working; do not add new operator logic there.

`make agent-loop` is the normal AI/operator edit gate. Before pushing `main`, the
tracked pre-push hook computes the exact `origin/main..HEAD` changed-file set,
passes it to `make pre-push-ci`, and rejects remote refs other than
`refs/heads/main`. `make pre-push-ci` runs cached focused fast-ci for that file
set (`build-only` plus mapped `t-fast` groups); it does not rerun the full suite
on every push, and it forces `ZCL_FAST_LIVE=0` so a live node condition remains
telemetry rather than a push blocker. Set `ZCL_FAST_STRICT_TESTS=1` for a
deliberate strict focused run. Full `make ci` still exists for release-grade manual runs, but the
expensive proof lanes are kept fresh by `zclassic23-test-suite.timer`,
`zclassic23-fuzz.timer`, and `zclassic23-coverage.timer`. Install them with
`make install-quality-linger`; inspect their latest
`zcl.background_quality_status.v1` verdict with `make quality-linger-status`.

## Invariants that hold across every stage

- **Live truth before design.** Pull `make diagnose-gap` first; never reason
  from a guessed cause.
- **Reproduce on a copy before any live chain/datadir mutation.** No exceptions
  for consensus-critical experiments.
- **Never weaken a consensus gate; never delete `tip_finalize_log` rows; never
  lower the public tip below `coins_best`.** Recovery only ever raises or holds
  the tip.
