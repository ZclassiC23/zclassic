# TSan triage — first sweep (2026-07-18, lane t-tsan)

First ThreadSanitizer scan of the codebase, run on the new opt-in TSan
profiles (`lane/t-tsan`, worktree `.claude/worktrees/wf_tsan`). This is the
baseline referenced by the `t-tsan` / `tsan-ci` / `dev-tsan` Makefile
targets and by `tools/tsan.supp`.

## Reproducer

```bash
# Build the instrumented harness (~31 s wall from cold ccache on 32 cores;
# own epoch tree build/test-tsan-obj, non-LTO, -O1 -g -fsanitize=thread):
make test-tsan -j$(nproc)

# One group (report-and-continue; exits 66 when a race was reported):
make t-tsan ONLY=test_supervisor

# The gate posture (halt_on_error=1, first report fails the run):
make tsan-ci                       # currently RED by construction — see below
```

Both runners wrap the harness in `setarch -R`: TSan reserves fixed shadow
address ranges and default-ASLR PIE/mmap placement collided at startup
(`FATAL: ThreadSanitizer: unexpected memory mapping`) on the very first run
here. Opt-in triage binaries only — never release artifacts.

## What was built

| Target | Artifact | Build time (this host, 32 cores, cold) |
|---|---|---|
| `make test-tsan` / `t-tsan` | `build/bin/test-tsan` (1661 TUs) | 30.6 s wall (4m56s user) |
| `make dev-tsan` | `build/bin/zclassic23-dev-tsan` (1037 TUs) | 18.2 s wall (2m2s user) |

Flags: `-fsanitize=thread -fno-omit-frame-pointer`, `-g` (`-g3` on dev),
hardening flags identical to the asan profiles (inherited from
`CACHED_CFLAGS`), `-Werror` dropped (mirrors test-asan). **LTO disabled** —
deliberate: race reports need per-TU PC/stack attribution that whole-program
LTO inlining degrades, and `-fsanitize=thread -flto=auto` is a little-traveled
gcc path. `-fsanitize=thread` is mutually exclusive with address/undefined,
so TSan is a sibling profile set (`TSAN_COMMON_SAN_FLAGS`), not an extension
of `ASAN_COMMON_SAN_FLAGS`. Vendored static archives (OpenSSL, leveldb,
libevent, rustzcash) were NOT rebuilt — TSan works against uninstrumented
code with proportionally less coverage inside it.

## Coverage of this sweep

16 thread-relevant group-substrings, the 7 `TSAN_CI_GROUPS` defaults run
3× each (race detection is timing-dependent), the rest 1× — 23 harness
invocations, ~23 s of instrumented wall per full pass:

- `test_supervisor` (4 groups: supervisor, _domains, _production_tree,
  _backstop), `test_workpool`, `test_mailbox` (+_adoption),
  `test_parallel_range_fold`, `test_validate_parallel_determinism` (15.5 s —
  the heaviest), `test_net_bootstrap`, `test_cpu_topology`,
  `test_net` (8 net groups), `test_peer_lifecycle`, `test_header_sync`,
  `test_chain_advance_atomicity`, `test_connect_node_locked`,
  `test_connman_node_count_locked`, `test_service_state_driver`,
  `test_reducer_drive_watchdog`, `test_sync_watchdog_conditions`.

This is a THIN baseline: short unit-style runs with small race windows.
A full-suite TSan pass and a `dev-tsan` boot on a scratch datadir are
follow-ups, not done here.

## Findings (deduplicated)

**1 unique race, fired 3/4 runs** of `test_supervisor_production_tree`
(also reproduced once under `make tsan-ci`'s halt_on_error=1).
**FIXED 2026-07-18 — see the R1 entry below.**

### R1 — `thread_liveness_child.id` published without synchronization (REAL data race, benign-looking on x86-64)

```
Write of size 4 by main thread:
  #0 thread_liveness_register          lib/util/src/thread_liveness.c:167
  #1 thread_liveness_register_restartable lib/util/src/thread_liveness.c:213
  #2 test_supervisor_production_tree   lib/test/src/test_supervisor_production_tree.c:563

Previous read of size 4 by thread T2 ('zcl_spt_restart'):
  #0 thread_liveness_worker_alive      lib/util/src/thread_liveness.c:238
  #1 spt_restart_worker_fn             lib/test/src/test_supervisor_production_tree.c:108
  #2 thread_registry_trampoline        lib/util/src/thread_registry.c:48

Location: global 'rc.1' (struct thread_liveness_child, 440 bytes)
SUMMARY: ThreadSanitizer: data race lib/util/src/thread_liveness.c:167 in thread_liveness_register
```

- **What:** `thread_liveness_register()` line 167 stores
  `c->id = supervisor_register(&c->contract)` — a plain (non-atomic) write —
  while the already-spawned worker thread reads `c->id` in
  `thread_liveness_worker_alive()` (line 238, plain read guarding
  `supervisor_worker_alive(c->id)`). `id` is `typedef int supervisor_child_id`
  (`lib/util/include/util/supervisor.h:61`), a non-`_Atomic` field of
  `struct thread_liveness_child` (`util/thread_liveness.h:63`).
- **Production-reachable, not test-only:** the spawn-then-register ordering
  is the documented contract ("The caller already spawned the initial worker
  into c->worker_tid", thread_liveness.c:226) and is exactly what production
  callers do — `rpc_timeout_start_watchdog` spawns at
  `lib/rpc/src/rpc_timeout.c:322` then registers at :329; same idiom at
  `lib/health/src/heartbeat.c:285` and `lib/metrics/src/metrics.c:468`.
- **Severity guess:** real C11 data race (formal UB), **benign-looking on
  x86-64**: aligned 4-byte load/store cannot tear, so the worst observed
  outcome is the worker reading the stale `SUPERVISOR_INVALID_ID` and
  skipping one liveness beat (self-heals on the next beat). On weaker-ordered
  targets the missing publication edge could additionally let the worker call
  `supervisor_worker_alive(id)` before the supervisor's registration state is
  visible to it. Not consensus-adjacent (lib/util liveness adapter).
- **Fix direction (NOT applied — out of this lane's scope):** make `id`
  `_Atomic supervisor_child_id`, or publish it under the thread-registry /
  supervisor lock so the store happens-before any worker read. Tracked here
  only; deliberately left failing so the race stays visible.
- **FIXED (2026-07-18, lane/fix-tsan-r1, commit 02c991bde):** `id` is now
  `_Atomic supervisor_child_id`. `thread_liveness_register` release-stores it
  after `supervisor_register()` completes; every reader (beat,
  worker_alive/_exited, stop_begin/_finish, retire, idempotent guards)
  acquire-loads it — a worker that observes a valid id also observes the
  completed registry insertion; one that still reads INVALID skips one beat
  and self-heals. No locks, no other fields touched. Verified:
  `make t-tsan ONLY=test_supervisor` 3× → 0 reports (was 3/4 flaky-red);
  `make tsan-ci` fully green; `build-only` / `lint` / `t-fast` clean.

## Gating decision

- `tsan-ci` is **opt-in and deliberately NOT wired into `make ci`**: with R1
  unfixed it went red by construction (verified: `make tsan-ci` →
  `Error 1` on `test_supervisor` with the R1 SUMMARY). **Since the R1 fix
  (02c991bde) it is green** (verified 2026-07-18 on lane/fix-tsan-r1).
  Posture mirrors
  asan-ci — `TSAN_OPTIONS=halt_on_error=1` makes the first report a red
  group, so with the baseline fixed a red run is a real new
  finding. Override the set with `TSAN_CI_GROUPS="..."`.
- `tools/tsan.supp` ships with **zero active suppressions** (comments only):
  R1 was documented here and fixed in code — no report was ever hidden. Only
  confirmed-benign entries with written justification may be added.

## Follow-ups (not this lane)

1. ~~Fix R1 (atomic publication of `thread_liveness_child.id`), then re-run
   `make tsan-ci` to green.~~ **DONE** (02c991bde, lane/fix-tsan-r1).
2. Full-suite TSan pass (whole `test_parallel` group list) — the subset
   above is ~23 s of threaded wall; the full suite is the real net.
3. Boot `dev-tsan` on a scratch datadir and triage boot/sync/runtime races
   (net threads, reducer stages, supervisor tree under real load).
4. Only after 1–3: consider a dedicated CI lane for `tsan-ci`.
