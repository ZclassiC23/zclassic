# Dev watch loop: fast feedback without weakening production

> **Phase-0 containment (2026-07-12):** runtime generation publication is
> disabled while the complete immutable source-epoch/proof-receipt/resident-CAS/
> acceptance/rollback transaction is unfinished. Public watchers support only
> `verify` and `check`. `auto`, `apply`, `hotswap`, `reload`, and `stage` refuse
> before mutation. `dev.change.apply`, direct deploy/stage/hot-swap backends,
> and legacy/native hot-swap apply refuse too. `dev.vcs.revert` is source-only;
> `relink_generation=true` refuses before reverting. Publication behavior below
> is target design unless explicitly marked verification-only.

## Outcome

The C equivalent of JavaScript live reload should be a supervised **process
reload**, not hot-swapped consensus libraries. The desired command is:

```bash
make dev-watch
```

On each save it should compile affected objects, run mapped focused tests,
preflight the candidate while the current dev node keeps serving, activate one
immutable binary generation, and automatically restore the last-known-good
generation if bounded probes fail. Canonical and soak lanes are never touched.

## What already exists

- `make fast-changed-compile`: source-wide exact-epoch compile proof; changed
  paths are classification hints only.
- `make fast-rebuild` / `make dev-bin`: non-LTO `-Og` objects under
  `build/dev-obj/epochs/<compile-epoch>/`, with `ccache`/`sccache` recovery and
  atomic exact-candidate publication.
- `make t-fast`, `make fast-ci`, and `make agent-loop`: focused test routing and
  cached green inputs through `tools/agent_fast_ci.sh`.
- `deploy/zcl23-dev.service`: isolated datadir `~/.zclassic-c23-dev` and ports
  8053/18252.
- `make agent-stage-dev`, `make agent-deploy-fast`, and
  `tools/dev/deploy-dev-lane.sh`: contained staging/dev activation backends;
  each currently refuses unconditionally before publication.
- `make agent-mcp-call-dev`, `zcl_tools_list`, and bounded candidate-local
  `zcl_self_test {"mode":"registry"}`: candidate
  and running-node introspection.

The host has `ccache`, but no `mold`, `lld`, `inotifywait`, `entr`, or
`watchexec`; implement a portable polling fallback.

## P0 implementation order

## Unified loop status — implemented 2026-07-10

The bounded watcher is now available:

```bash
make dev-watch                         # verify-only: prove, never activate
MODE=check make dev-watch               # compile/test feedback only
ZCL_DEV_WATCH_ONCE_FILES=lib/net/src/msg_tx.c make dev-watch-once
make dev-watch-selftest
make agent-index
make dev-loop-bench
```

`tools/dev/watch-dev-lane.sh` uses `inotifywait` when installed and otherwise
polls a SHA-256 digest over a sorted path/mtime/size manifest, including the
sealed `core/` tree. It debounces and coalesces editor saves. Wake paths are
diagnostic/focused-check hints only: because they cannot prove a complete
SHA-1-free changed set, every nonempty event is conservatively classified
`reload_required`. The watcher always runs `check-core-seal` and
`check-consensus-parity` before the mapped fast check. An initial/once run with
no explicit hint checks every watched input instead of consulting Git HEAD.

Before verification, the watcher captures one `source-identity.sh
capture-record` (exact SHA-256 source id, completeness bit, and host-local
mutation token); after checks and again at the final boundary it requires an
exact `verify-record`. History-only Git commits do not change this authority,
while byte/path/mode/inventory drift and edit/restore ABA supersede the cycle.
Assume-unchanged or skip-worktree bits fail closed in source inventory capture.
Publication modes are recognized only to return a Phase-0 refusal before
checks, build, RPC, or mutation. The self-test covers mandatory Core/parity
gates, history independence, hidden index bits, ABA refusal, and both public
and in-process containment.

Every attempted cycle atomically writes `zcl.dev_cycle.v1` below
`~/.local/state/zclassic23-dev/cycles/`, refreshes `latest-cycle.json`, and
updates a watcher heartbeat. The record owns changed files, the native impact
plan, selected path/reason, phase timings, generation identities, rollback
status, a failure capsule, and one executable `agent_next_action`.

Controls: `ZCL_DEV_WATCH_POLL_MS`, `ZCL_DEV_WATCH_DEBOUNCE_MS`,
`ZCL_DEV_WATCH_BACKEND=auto|poll|inotify`,
`ZCL_DEV_WATCH_MODE=verify|check`, and `ZCL_DEV_WATCH_INITIAL=1` are the public
controls. Publication mode names remain parseable only so they can fail with
an explicit containment reason. Test command overrides are confined to the
hermetic `--self-test` entry point.

Process activation is content-addressed and immutable under
`~/.local/lib/zclassic23-dev/`. A nonblocking activation lock serializes flips;
`current` and `last-good` are atomic links. Candidate preflight runs before the
old process is stopped. A failed warm probe restores and verifies last-good,
then quarantines the rejected generation. The systemd unit executes
`current/zclassic23-dev`, never an overwritten singleton.

`auto` and `hotswap` do not call the former authenticated `dev_hotswap` RPC.
`tools/dev/hotswap-running-dev.sh` itself refuses, and no exit code permits a
reload fallback. Use `make hotswap-so`, loader tests, and `make hotswap-sim`
for non-publishing validation. Resident probing is contained until a disposable
worker and pre-load ELF policy exist.

### 1. Transactional activation — machinery implemented, authority contained

Harden `tools/dev/deploy-dev-lane.sh` before automatic watching:

1. Take a nonblocking `flock`; concurrent activations reject or coalesce.
2. Build and preflight before stopping the current dev process.
3. Install at `~/.local/lib/zclassic23-dev/<generation>/zclassic23-dev`.
4. Maintain atomic `current` and `last-good` symlinks; make the dev unit execute
   `current`.
5. Preflight native `agentbuild`, the MCP catalog, and candidate-local
   `zcl_self_test {"mode":"registry"}`; failure
   leaves the old process untouched.
6. Flip `current`, restart only `zclassic23-dev`, and run a bounded warm probe:
   unit active, RPC ready, native `agent`, operator snapshot, exact running
   generation, and MCP self-test.
7. On failure, restore `last-good`, restart, verify recovery, and record the
   rejected generation. On success, promote the candidate to `last-good`.

Never overwrite the sole installed binary or let `Restart=always` loop a known
bad candidate indefinitely.

### 2. Add the watcher — unified modes + durable verdict complete

The MVP added `tools/dev/watch-dev-lane.sh` and `make dev-watch` with the
following behavior:

- Prefer inotify; otherwise poll a stable path/mtime/hash list around 500 ms.
- Debounce saves. Exactly one build runs; a change during it schedules one
  immediate follow-up from the newest tree.
- Forward event paths through `ZCL_FAST_CHANGED_FILES_FILE` and
  `ZCL_FAST_CHANGED_FILES_ONLY=1` only as focused-check hints; exact source
  identity plus the global Core/parity gates carry authority.
- Run focused fast CI without live probes and record the verdict. The
  activation branch now stops at the Phase-0 refusal.
- Keep watching after failures and print one actionable summary.

Future post-containment feedback:

```text
[check 1.2s] [link 2.8s] [preflight pass] [restart] [ready gen=<id>]
[rejected gen=<id>; previous generation still serving]
[rolled back gen=<id>; last-good healthy]
```

### 3. Split foreground and background acceptance

Give the foreground loop a 60–90 second warm-activation ceiling. Move the
multi-minute height-climb/deep-health proof to a background quality lane. Cold
first bootstrap remains explicit, never part of watch mode.

The activation/status contracts persist `candidate_generation`,
`running_generation`, `last_good_generation`, `rollback_available`,
`activation_status`, `rollback_status`, rejected generations, current cycle,
watcher heartbeat, index freshness, mapped tests, and MCP probe outcomes.

`make agent-index` derives `compile_commands.json` from dry-runs of the exact
dev-object recipes, preserving generated-header prerequisites and the `-Og` /
hot-bucket `-O2` split. clangd is optional. `make dev-loop-bench` writes a
machine-readable percentile artifact; activation cases remain skipped during
containment, so an SLO is never inferred from a build-only run.

## Required tests

With injectable commands and temporary directories, prove:

- build/preflight failure never stops the old process;
- activation failure restores and verifies `last-good`;
- success promotes exactly the candidate generation;
- concurrent deploy is rejected/coalesced by the lock;
- debounce forwards only changed event paths;
- activation rejects a running generation/build mismatch.

Wire the script, service, Makefile, and tests into `agent_impact_rules.def` and
`make_lint_gates`.

`make hotswap-sim` is the focused deterministic three-node RAM-network proof.
It exercises a rejected all-or-zero batch, an in-flight old-generation call,
an atomic two-provider commit, new-call visibility, and exact seed replay.
`make sim-fast` remains the broader seeded network suite.

## Explicit non-goal

Do not introduce reloadable consensus/shared-state libraries. They create a
dev/release execution split and leave stale globals across reloads. The isolated
process plus persistent dev datadir is the safe hot-reload boundary.
