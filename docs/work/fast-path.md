# The Fast Path — our information algorithm for getting to correct C

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
| `make t ONLY=<group>` | run ONE test group, rebuilding the harness first (closes the stale-`test_parallel` rebuild trap) |
| `make t-fast ONLY=<group>` | hot-path ONE test group via cached per-file test objects and a non-LTO harness |
| `make build-only` | incremental compile-check of the whole node (no link) |
| `make syntax-check` | full no-link syntax check across every TU |
| `make lint-fast` | the 5 highest-signal lint gates (full `make lint` before commit) |
| `make fast-ci` | cache-aware agent loop: `lint-fast` + `build-only` + focused tests inferred from changed files + native linger-service probe; identical green inputs skip repeated lint/build/focused tests |
| `make pre-push-ci` | bounded push gate: cached focused fast-ci for changed files |
| `make install-quality-linger` | install background full-test, fuzz, and coverage user timers |
| `make quality-linger-status` | show latest background tests/fuzz/coverage JSON verdicts |
| `make test` | the fast fork-based parallel suite (~1 min); `make test-full` is the slow single-process binary |
| `make ci-reproducible` | build-twice byte-identity proof in isolated build dirs |

`make fast-ci` is the default edit-loop command for agents and operators. It
auto-selects `sccache cc` or `ccache cc` when present; override with
`ZCL_FAST_CC='ccache cc'`. It runs focused tests through `make t-fast`, which
uses `build/bin/test_parallel_fast`: a cached per-file, non-LTO test harness
that rebuilds only changed test/node objects after the first warm-up. It is
non-`-Werror`; compile warning enforcement stays in `make build-only`, strict
`make t`, and full CI. Force focused test groups with
`ZCL_FAST_TESTS=make_lint_gates,mcp_controllers`; use `ZCL_FAST_STRICT_TESTS=1`
to pay the strict `make t` whole-harness rebuild. Set parallelism with
`ZCL_FAST_JOBS=N` (default caps at 16). Successful runs write a content
fingerprint under `.cache/zcl-agent-fast-ci/` covering changed-file contents,
selected test groups, compiler/cache choice, strict/live knobs, core scripts,
the Makefile, and the native probe binary mtime. A repeat with the same
fingerprint logs `fast result cache hit` and skips `lint-fast`, `build-only`,
and focused tests; it still refreshes the live service probe unless
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

The native build contract is discoverable with `build/bin/zclassic23 agentbuild`
or MCP `zcl_agent_build`.

Canonical operator APIs, in priority order:

1. `build/bin/zclassic23 agentmap`, `agentlanes`, `agentliveness`, `agentimpact`,
   `agentbuild`, `agent`, `healthcheck`, and raw RPC methods — native C binary
   client to the running linger service.
2. MCP tools (`zcl_agent_map`, `zcl_agent_lanes`, `zcl_agent_liveness`,
   `zcl_agent_impact`, `zcl_agent_build`, `zcl_agent`, `zcl_status`,
   `zcl_state`, `zcl_node_log`, `zcl_sql`) — typed agent interface over the
   same node RPC truth.
3. REST (`/api/v1/agent`, `/api/v1/openapi`) — public web/API surface.
4. `tools/z` — deprecated shell compatibility for older terminals and scripts,
   not an agent interface. Keep it working; do not add new operator logic there.

`make fast-ci` is the normal AI/operator edit gate. Before pushing `main`, the
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
