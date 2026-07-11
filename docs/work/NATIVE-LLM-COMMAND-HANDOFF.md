# Native LLM command/development handoff

Date: 2026-07-10
Branch: `main`
Canonical contract: [`docs/NATIVE_COMMAND_INTERFACE.md`](../NATIVE_COMMAND_INTERFACE.md)

## Outcome of this checkpoint

This checkpoint establishes the first end-to-end substrate for the AI-first C
development loop without changing consensus, canonical deployment, or the soak
lane:

- transactional immutable dev generations with `current`/`last-good`, bounded
  preflight, rollback, and quarantine;
- one durable `zcl.dev_cycle.v1` verdict per watched change;
- transactional hot-swap v2 route generations (validate/stage/self-test/atomic
  publish) plus a persistent dev-only RPC bridge;
- a native `dev` command prototype, inotify watcher, change classifier, focused
  proof runner, compilation-database generator, and loop benchmark;
- a public Core-to-App ABI skeleton at `sdk/include/zclassic23/app.h`;
- a Social App manifest and deterministic RAM-only network simulation proving
  censorship bypass, partition convergence, late joining, invalid-signature
  rejection, and identical replay;
- the frozen six-root LLM interface specification;
- a new transport-neutral typed registry engine in `lib/kernel` and initial
  declarative `root`, `apps`, and `dev` command definitions.

## Honest implementation state

The native registry migration is **in progress**, not complete.

Ready substrate:

- `lib/kernel/include/kernel/command_registry.h`
- `lib/kernel/src/command_registry.c`
- `config/commands/root.def`
- `config/commands/apps.def`
- `config/commands/dev.def`
- `config/include/config/command_catalog.h`
- `tools/command/native_command.h`

The kernel engine supplies typed metadata, validation, longest-path lookup,
registry digests, shallow bounded menus, deterministic five-result search,
leaf descriptions, common result envelopes, exit codes, lane/capability checks,
and planned-command fail-closed behavior. Its strict release object compiles.

Still missing before any claim that the new registry is active:

1. Author `config/commands/core.def` and `config/commands/ops.def`.
2. Implement `config/src/command_catalog.c` to expand the `.def` files into
   immutable metadata plus separate common/dev bindings.
3. Implement the native adapter and normalized `--input`/stdin/typed-flag
   parser under `tools/command/`.
4. Route canonical resolution before the arbitrary RPC fallback in
   `src/main.c`; a typo under a canonical branch must never become an RPC.
5. Replace `tools/dev/devloop_menu.c`'s hardcoded tree with registry wrappers.
6. Add golden registry tests: exactly six root children, shallow menus, byte
   budgets, maximum five ranked search results, unique aliases, every ready
   leaf dispatchable, every planned leaf blocked, and common envelope/exit-code
   vectors.
7. Split metadata from bindings and split release/common sources from dev-only
   executors. Today `tools/dev/*.c` is still in `ALL_SRCS`; `#ifdef` prevents
   mutation but does not prove the release binary lacks the code. Add an `nm`
   release proof before Phase B is called complete.

Do not promote the current prototype's handlerless menu nodes to `ready`.
`dev.core.proof`, generation current/history/rollback, and the old hardcoded
Social conceptual leaves had no executable handler. In the new definitions
unfinished operations are explicitly `planned`.

## Current native prototype

The currently built dev binary still uses the prototype entry point:

```bash
build/bin/zclassic23-dev dev
build/bin/zclassic23-dev dev app describe social
build/bin/zclassic23-dev dev app plan social posts
build/bin/zclassic23-dev dev app simulate social
build/bin/zclassic23-dev dev change plan FILE
```

The Social simulation proof at handoff passed in 9 microseconds for two runs:

```text
seed=0x534f4349414c0001
transcript=0x5a599450ebe72edb
deliveries=11
rejected_invalid=8
```

Some prototype cycle paths still invoke fixed-argv Make compatibility targets.
They do not interpolate a shell, but they are not the final native compile and
activation engine described by the contract.

## Dev-lane safety state — do not bypass

The dev service was deliberately left running and was **not activated** during
this work. At handoff it is active on the legacy immutable generation, with
both `current` and `last-good` pointing to:

```text
legacy-814a1bedb3f67050fd9324d51a701ef5360be74a903c94fff42d1e43f83e3dce
```

`~/.zclassic-c23-dev/auto_reindex_request` is present and contains:

```text
3175499 1
```

This marker is genuine: it was created after the current process started in
response to a progress-store failure/sticky recovery escalation. **Do not clear
it as stale.** The transactional deploy correctly refused to disturb the
running process.

A deliberate recovery activation was attempted with the documented override,
but candidate preflight timed out because the exhaustive MCP self-test called
the unhealthy predecessor. Candidate and activation preflight now use
`zcl_self_test {"mode":"registry"}` so route/schema/loadability checks remain
candidate-local. Before attempting the safe recovery bootstrap:

- retain one bounded MCP RPC-backed call after activation (for example
  `zcl_getblockcount`) in addition to the existing direct RPC/identity/readiness
  probes;
- use a deliberately long recovery activation timeout;
- never touch canonical or soak services/datadirs;
- never clear the reindex marker merely to make deployment green;
- verify exact generation identity and recovery before allowing the persistent
  watcher to own activation.

## Verification recorded for this checkpoint

Completed before the final commit:

- strict `-O3 -Werror` compile of `lib/kernel/src/command_registry.c`;
- `make t-fast ONLY=dev_platform`;
- `make t-fast ONLY=hotswap_simnet`;
- `make t-fast ONLY=dev_mcp_rpc_bridge`;
- documentation accuracy/count checks;
- `make lint`;
- `git diff --check`.

If any of these are later amended below, trust the final commit message and CI
output over this prose.

## File map

- Interface contract: `docs/NATIVE_COMMAND_INTERFACE.md`
- Dev/watch design: `docs/work/DEV-WATCH-PLAN.md`
- Hot-swap contract: `docs/work/HOTSWAP.md`
- Registry engine: `lib/kernel/{include/kernel,src}/command_registry.*`
- Registry declarations: `config/commands/`
- Native prototype: `tools/dev/devloop_*.c`
- Transactional deployment: `tools/dev/deploy-dev-lane.sh`
- Public App ABI: `sdk/include/zclassic23/app.h`
- Host ABI validator: `lib/framework/src/app_platform.c`
- Social manifest/sim: `apps/social/app.def`, `lib/sim/src/social_app_sim.c`
- Focused proofs: `lib/test/src/test_dev_platform.c`,
  `lib/test/src/test_hotswap_simnet.c`,
  `lib/test/src/test_dev_mcp_rpc_bridge.c`

## Recommended next slice

Finish Phase B only: composition-root catalog, Core/Ops definitions, native
normalization/dispatch, and golden contract tests. Do not begin native watcher
process management or remove compatibility scripts until registry dispatch and
release/dev binding separation are proven. Then proceed to `dev loop
ensure/wait/events` as Phase C.
