# Dev watch loop: fast feedback without weakening production

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

- `make fast-changed-compile`: changed `.c` files plus depfile dependents.
- `make fast-rebuild` / `make dev-bin`: cached non-LTO `-Og` objects under
  `build/dev-obj`, with `ccache` support.
- `make t-fast`, `make fast-ci`, and `make agent-loop`: focused test routing and
  cached green inputs through `tools/agent_fast_ci.sh`.
- `deploy/zcl23-dev.service`: isolated datadir `~/.zclassic-c23-dev` and ports
  8053/18252.
- `make agent-stage-dev`, `make agent-deploy-fast`, and
  `tools/dev/deploy-dev-lane.sh`: staging, dev-only restart, and basic probes.
- `make agent-mcp-call-dev`, `zcl_tools_list`, and `zcl_self_test`: candidate
  and running-node introspection.

The host has `ccache`, but no `mold`, `lld`, `inotifywait`, `entr`, or
`watchexec`; implement a portable polling fallback.

## P0 implementation order

## MVP status — implemented 2026-07-10

The bounded watcher is now available:

```bash
make dev-watch                         # green save -> isolated dev redeploy
MODE=stage make dev-watch              # stage without restarting
MODE=off make dev-watch                # compile/test/link only
ZCL_DEV_WATCH_ONCE_FILES=lib/net/src/msg_tx.c make dev-watch-once
make dev-watch-selftest
```

`tools/dev/watch-dev-lane.sh` uses `inotifywait` when installed and otherwise
polls a SHA-256 digest over a sorted path/mtime/size manifest. It debounces and
coalesces editor saves, forwards only the changed paths through
`ZCL_FAST_CHANGED_FILES_FILE` + `ZCL_FAST_CHANGED_FILES_ONLY=1`, runs the
existing fast focused gate and `fast-rebuild`, then calls only
`agent-deploy-fast` / `agent-stage-dev`. A failed check or rebuild never invokes
the activation command, and the watcher remains alive for the next save.
Changes that arrive during a check or link suppress activation and schedule one
coalesced follow-up from the newest tree. `--once` and injectable commands give
the shell self-test a deterministic, node-free path.

Controls: `ZCL_DEV_WATCH_POLL_MS`, `ZCL_DEV_WATCH_DEBOUNCE_MS`,
`ZCL_DEV_WATCH_BACKEND=auto|poll|inotify`,
`ZCL_DEV_WATCH_MODE=deploy|stage|off`, and `ZCL_DEV_WATCH_INITIAL=1`.

This MVP intentionally reuses the existing dev deploy script. The immutable
generation / last-known-good rollback design below remains the next hardening
step. Therefore a build/test failure is proven not to touch the running dev
service, but this watcher does not yet claim automatic rollback after a failure
inside the existing activation workflow.

### 1. Transactional activation first

Harden `tools/dev/deploy-dev-lane.sh` before automatic watching:

1. Take a nonblocking `flock`; concurrent activations reject or coalesce.
2. Build and preflight before stopping the current dev process.
3. Install at `~/.local/lib/zclassic23-dev/<generation>/zclassic23-dev`.
4. Maintain atomic `current` and `last-good` symlinks; make the dev unit execute
   `current`.
5. Preflight native `agentbuild`, the MCP catalog, and `zcl_self_test`; failure
   leaves the old process untouched.
6. Flip `current`, restart only `zclassic23-dev`, and run a bounded warm probe:
   unit active, RPC ready, native `agent`, operator snapshot, exact running
   generation, and MCP self-test.
7. On failure, restore `last-good`, restart, verify recovery, and record the
   rejected generation. On success, promote the candidate to `last-good`.

Never overwrite the sole installed binary or let `Restart=always` loop a known
bad candidate indefinitely.

### 2. Add the watcher — bounded MVP complete

The MVP added `tools/dev/watch-dev-lane.sh` and `make dev-watch` with the
following behavior:

- Prefer inotify; otherwise poll a stable path/mtime/hash list around 500 ms.
- Debounce saves. Exactly one build runs; a change during it schedules one
  immediate follow-up from the newest tree.
- Forward event paths through `ZCL_FAST_CHANGED_FILES_FILE` and
  `ZCL_FAST_CHANGED_FILES_ONLY=1` so a large dirty tree does not test everything.
- Run focused fast CI without live probes, then `fast-rebuild` and the existing
  isolated dev activation. Candidate MCP preflight and transactional rollback
  remain coupled to step 1 above.
- Keep watching after failures and print one actionable summary.

Target feedback:

```text
[check 1.2s] [link 2.8s] [preflight pass] [restart] [ready gen=<id>]
[rejected gen=<id>; previous generation still serving]
[rolled back gen=<id>; last-good healthy]
```

### 3. Split foreground and background acceptance

Give the foreground loop a 60–90 second warm-activation ceiling. Move the
multi-minute height-climb/deep-health proof to a background quality lane. Cold
first bootstrap remains explicit, never part of watch mode.

Persist `candidate_generation`, `running_generation`, `last_good_generation`,
`rollback_available`, `activation_status`, `rollback_status`, mapped tests, and
MCP self-test failure count in dev status.

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

## Explicit non-goal

Do not introduce reloadable consensus/shared-state libraries. They create a
dev/release execution split and leave stale globals across reloads. The isolated
process plus persistent dev datadir is the safe hot-reload boundary.
