# Session handoff — 2026-07-11 (zero-MCP program start + power-station close-out)

**Read this with:** `docs/work/MCP-REMOVAL-PLAN.md` (the program),
`docs/work/MCP-REMOVAL-WORKLIST.md` (the authoritative 114-site execution
inventory — waves execute from the worklist, NOT from the plan's prose).

## Where main is

All of the following is merged, gated (`make lint` + full `make test-parallel`
green at each step), and pushed to origin/main:

- **Zero-MCP W0 (both slices) — DONE.**
  - Slice 1: native `ops.state` + `ops.selftest` leaves; the dev-lane deploy
    verify (`deploy-dev-lane.sh` preflight + activation probe,
    `dev_activation_ops.c`) runs **zero `mcpcall`** (uses `discover help` +
    `ops selftest`).
  - W0-A: the 41 bridged native leaves no longer enter
    `mcp_middleware`/`mcp_router` at all — 19 re-homed transport-neutral body
    functions under `app/controllers/src/*_native_handlers.c` (+
    `status_native_helpers.c`, shared contract
    `controllers/native_handler_body.h`), 22 direct-RPC pass-throughs via
    `mcp_node_rpc` + `zcl_native_bridge_project`. MCP controllers are now thin
    wrappers over the same bodies (byte-identical surface, dual-run;
    `test_mcp_controllers` passes untouched; `ops_controller.c` 3576→2395 LOC).
    `zcl_native_bridge_body_for_path()` is the probe/dispatch seam W1 needs.
- **Zero-MCP W1-A — DONE.** Atomic leaf-handler override snapshot in
  `lib/kernel/command_registry.{c,h}`: `zcl_command_registry_replace_batch`
  (validate-entire-batch-before-publish, clone + single release-store,
  lock-free readers, generation-monotonic — mirrors
  `mcp_router_replace_batch`), read through by
  `zcl_command_registry_execute_json` with a NULL fast path. Test TU
  `test_command_handler_snapshot` incl. a 4-reader/1-writer hammer.
- **Phase 4 boot decomposition:** `config/src/boot_services.c` no longer
  defines ANY worker function bodies (lane 3 moved the catchup spawn/join/reap
  lifecycle to `app/services/src/catchup_lifecycle_service.c`; earlier seams
  moved utxo-replay + mempool-limits). Copy-proven (PASS) and deployed to the
  dev lane. Remaining boot work is orchestration-shape only (runtime-services
  spec table; connman adapter is partially test-locked).
- **Adversarial review of the day's landings: 6 confirmed findings, ALL FIXED
  + merged:** auto-reindex deploy guard + crash-recovery in-progress marker in
  native activation (major); two-phase all-or-nothing `vcs_revert` worktree
  restore (major); sparse-catchup watcher now waits on the NEXT missing index
  slot (`projection_tip+1`), killing a 5s restart-churn shape; convergence-gate
  awk brace-counting fix; `utxo_mirror_sync` dumper atomics; plus the
  activation state-file now records real `auto_reindex_pending`.

## Node state

- **Dev node** (`~/.zclassic-c23-dev`, rpcport 18252): AT TIP, healthy,
  running the latest generation (rollback available). It climbed through the
  anchor-backfill region today (~1,215 blocks/min) — the sparse-prefix +
  lock-order fixes work. Remaining named blockers are the three known
  permanent ones (anchor/nullifier backfill below the seed cursor + no refold
  artifact) — the sovereign-cure track, not regressions.
- **Live node** (rpcport 18232): untouched, at tip, healthy, still on
  3b0de63b0. **Live deploy remains owner-gated.**

## Next steps (in order)

1. **W1-B/C — hot-swap re-target** (plan §3.2-3.5): new provider class
   `native.leaves` in `lib/hotswap` (`leaf_stage` vtable capability +
   `ZCL_HOTSWAP_EXPORT_LEAVES`), commit callback →
   `zcl_command_registry_replace_batch`; re-point `config/hotswap_eligible.def`
   at the `*_native_handlers.c` TUs with leaf-path probes; `dev.hotswap.apply`
   / `dev.hotswap.probe` dev commands; keep the MCP swap leg green (dual-run).
   Note from the W1-A verifier: `replace_batch` does not yet reject
   branch-mode paths (inert today — the dispatcher bails on branches first);
   add that check when W1-B builds on it. Also wire
   `zcl_command_registry_set_active(registry)` at boot (W1-A left production
   binding to this wave).
2. **W2 — consumer migration**: execute `MCP-REMOVAL-WORKLIST.md` W2 table
   (47 sites). Watch the 5 risk items in plan §1.8 (README, test_api,
   test_metric_alerts/mcp_notify, test_secrets_hygiene, boot_services.c call
   sites at :100,:141,:1219,:1241 — the last set breaks the BUILD if missed).
   Risk 4 decision still open: destructive tools (zcl_send etc.) fall back to
   `zclassic23 rpc <method>` unless promoted to native READY leaves with the
   destructive auth tier.
3. **W3 — the delete**: `tools/mcp/**`, `tools/mcp_server.c`, `-mcp`/
   `-mcp-inprocess`/`mcpcall` in `src/main.c` (REAL sites listed in worklist,
   the plan's original line numbers had drifted), `.mcp.json`, Makefile
   targets, transport flag bits. Final `grep -ri mcp` sweep.
- **W0-A byte-compat notes for W2/W3:** `core network peers list` keeps its
  pre-existing `BAD_TOOL_BODY` on array bodies (decide in W2);
  `listaddresses` raw-forward path keeps a pre-existing missing `json_free`
  (preserved bug-for-bug); `zcl_kpi` body is re-homed but has no native leaf
  yet.

## Owner actions pending

- **ZVCS seal re-pin:** every auto-anchor still reports advisory
  `vcs_sealed_refusal` because app/jobs files changed after the 13:41 baseline
  pin (seal working as designed). Owner ritual:
  `make zvcs-unseal REASON="authorize the 2026-07-11 app/jobs reducer lock-order fix"`.
- Live-node deploy of today's main (when desired): copy-prove against a live
  datadir copy first, per doctrine.

## Traps hit today (avoid re-paying)

- `.def` command files are NOT depfile-tracked: after editing
  `config/commands/*.def`, `rm build/obj/config/src/command_catalog.o` or the
  rebuild silently no-ops.
- `core.hooksPath` flaps to an absolute path when worktree agents run
  install-hooks; fix with `git config core.hooksPath tools/githooks` before
  push.
- `test_make_lint_gates` can fail 1/556 under the full parallel suite (E1/E2
  self-checks race concurrent groups scanning the real tree); it passes solo
  and on suite re-run. Verify solo before chasing it.
- The `ops` registry branch menu has ~80 bytes of headroom under its 1600-byte
  budget after slice 1's summary tightening — the next ops leaf may need a
  sub-branch.
- Pre-push runs `make pre-push-ci`: every changed file needs an
  `agent_impact_rules.def` mapping (group names must exist), including NEW
  TEST files.
