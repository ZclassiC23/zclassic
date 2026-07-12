# ADR-0003: No operating system, no central scheduler — build the missing organs in-tree

- **Status:** Accepted 2026-07-12.
- **Deciders:** Project maintainer.
- **Related:** `docs/adr/0001-personal-sovereignty-stack.md` (the one-binary
  positioning this verdict protects), `docs/adr/0002-sealed-consensus-core.md`
  (the structural-boundary pattern this ADR reuses for confinement),
  [`docs/work/os-substrate-plan.md`](../work/os-substrate-plan.md) (the
  three-rung working plan), [`docs/work/lb1-wiring-design.md`](../work/lb1-wiring-design.md)
  (the bounded verify-pool design cited in Evidence (a)).

---

## Context

ZClassic23's positioning is one self-contained C23 binary that is its own
node, wallet, explorer, and operator surface (`docs/adr/0001`). Two questions
recur whenever the node's sync rate or its runtime safety comes up: (1) is
the observed fold-rate ceiling actually a *scheduling* problem — one thread,
one queue, priorities — that only a real OS or a hand-rolled scheduler would
fix, and (2) does a "personal sovereignty stack" running third-party network
input (P2P blocks, mempool tx, Tor) need OS-grade confinement (sandboxing,
resource limits, capability dropping) that a plain POSIX process does not
give it for free? This review answers both from the measured bottleneck
history and a direct audit of the runtime surface, rather than from
first-principles OS-design instinct.

## Decision

**zclassic23 adopts no operating system and builds no central scheduler.**
Where the node is missing an OS-grade "organ," it is built in-tree,
evidence-first — porting from FreeBSD (BSD-2-Clause) or OpenBSD (ISC) where
their code fits, under the repo's existing attribution mechanism (a
file-header block naming the source project + license, an entry in `NOTICE`
for verbatim/near-verbatim ports per Apache-2.0 §4(d), and a pattern entry in
[`docs/ATTRIBUTIONS.md`](../ATTRIBUTIONS.md) — the same three places the tree
already uses for ported concepts, e.g. `lib/net/src/tor_integration.c`'s
"Derived from" header).

### Evidence (a): no measured bottleneck is scheduler-shaped

[`docs/work/refold-fold-rate-bottlenecks.md`](../work/refold-fold-rate-bottlenecks.md)
is a source-grounded bottleneck pass against a live `-refold-staged` run.
Its ranked list:

1. **#1 (dominant, LANDED):** a per-block ~3.1M-node `pprev` pointer walk
   (`app/jobs/src/tip_finalize_stage.c:586` gates the window-retraction on
   `!refold_in_progress()`) — a data-structure/cache-locality problem, not a
   scheduling one.
2. **#2 (open):** "scheduler ceiling" — but reading the actual claim, it is
   one supervisor thread on a 2-second tick period draining a batch of 100
   (`app/supervisors/src/staged_sync_supervisor.c:249`, `TIP_FINALIZE_BATCH_PER_TICK`).
   That is a config constant, not a missing scheduling primitive — the fix
   the doc proposes is "lower the period, raise the batch," not "add
   priorities" or "add an OS-grade scheduler."
3. **#3 (LANDED):** `utxo_mirror_sync_run_once` did a full wipe+reinsert
   every 5s during a fold (`app/services/src/utxo_mirror_sync_service.c:446`
   now early-returns during a refold) — an algorithmic-complexity bug
   (accidentally quadratic), not scheduling.
4. **#4 (open, known fix):** `utxo_apply_sums_through` is an O(height)
   `SUM(...)` scan per block (`tip_finalize_stage.c:451`) — needs an
   incremental running counter, not a scheduler.

The one genuine **execution-parallelism** gap this review found is real:
script/proof verification (ECDSA, Groth16) runs serially on the drive
thread while the rest of the machine's cores sit idle. The answer already
has a design:
[`docs/work/lb1-wiring-design.md`](../work/lb1-wiring-design.md) specifies a
bounded verify pool gated behind `-par=N` (default parallel, `-par=1` the
bit-for-bit serial oracle — the correctness rollback). **Correction to an
earlier framing of this evidence:** the pool's scaffold code
(`lib/validation/{thread_pool,verify_queue}.{c,h}`) was in fact built once,
additively, as Phase-1.1 (commit `9c5357930`, 2026-06-20) — but it was never
wired into the script/proof hot path, sat with zero live callers, and was
deleted as dead code in this session's cleanup pass (`verify_queue` at
`a55316466`, `thread_pool` at `b1e27ce0e`, both 2026-07-12, both ancestors of
this ADR). The design doc is still the accurate target; the scaffold needs
to be **rebuilt and wired**, not merely wired, before it exists again. Either
way, the answer to the one real execution gap is a bounded verify pool on
the existing LB-1 roadmap — not an OS-grade preemptive scheduler.

### Evidence (b): the OS organs that matter are already here, shaped for this node's liveness philosophy

The node's failure model (`docs/FRAMEWORK.md` §0) is "advance a durable
cursor or name a typed blocker — never a silent halt," which is a different
contract than a general-purpose OS scheduler's "run whatever is runnable,
fairly." The organs that contract actually needs are built:

- **Supervisor tree** — `lib/util/include/util/supervisor.h`: one
  independent time-driven thread ticking registered children's `on_tick`
  and firing `on_stall` on a missed deadline or a frozen progress marker.
  Built specifically to fix an 8.6h silent sweeper wedge (see the header's
  own incident note).
- **Condition engine** — `app/conditions/` (32 live `condition_register(...)`
  registrations, per `tools/scripts/check_doc_counts.sh`'s own measured
  count): typed
  detect/remedy/witness healers — the node's equivalent of an OS's
  self-healing daemons, but wired to the append-only log instead of ambient
  process state.
- **Durable stage cursors** — `lib/util/include/util/stage.h`: one SQLite
  table (`stage_cursor`) keyed by stage name; chain progress is a cursor on
  disk, never RAM-only authoritative state (`docs/FRAMEWORK.md` §0's Prime
  Directive).
- **Injectable clock** — `lib/platform/include/platform/clock.h`: the one
  swappable time source production and the deterministic simulator both
  read through, so time-driven bugs become 64-bit simulator seeds.
- **Workpool primitive** — `lib/util/include/util/workpool.h`: a fixed-size
  persistent thread pool over a shared work queue, already the shape LB-1's
  verify pool needs (see Evidence (a) — the gap is wiring, not a missing
  primitive class).

### Evidence (c): what is actually missing

A direct grep of the tree (excluding `vendor/`) for `seccomp`, `landlock`,
`capsicum`, `pledge(`, `setrlimit` returns **zero** matches. There is no
runtime confinement anywhere in zclassic23 today. Concretely:

- The `ZCL_DEV_BUILD` hot-swap loader (`lib/hotswap/src/hotswap_loader.c:696`,
  `dlopen(pinned_path, RTLD_NOW | RTLD_LOCAL)`) runs a manifest-gated `.so`
  with the full authority of the host process — no capability restriction
  beyond the manifest hash check.
- **~12 shell-out sites** (`system()`/`popen()`/`fork()+execlp()`) block any
  exec-filtering sandbox from being added today — the full inventory is in
  `docs/work/os-substrate-plan.md` §1.
- **~10 unshimmed `/proc/self/*` reads** (RSS, uptime, exe path, per-thread
  stat) — `lib/platform` today is clock + RNG only
  (`lib/platform/include/platform/{clock,rng}.h`), so every one of these
  reads a raw `/proc` path directly instead of through a platform seam,
  which is the only real blocker to a FreeBSD port (FreeBSD has no `/proc`
  by default) and to a future sandbox that wants to deny raw
  `/proc/self/*` access from application code.

## The three rungs

Detail, file inventories, and the ordered checklist are in
[`docs/work/os-substrate-plan.md`](../work/os-substrate-plan.md). Summary:

- **Rung 0 — no shell-outs.** Two new `lib/util` primitives
  (`file_tree_ops`: one `openat`/`fdopendir`-based walker with
  `O_NOFOLLOW` + preserve-times/update-only flags; `spawn`:
  `zcl_spawn_detached` double-fork+setsid and `zcl_spawn_capture`
  fork+pipe+execvp) plus `lib/net`'s `http_post_json`, replacing all 12
  shell-out sites. A new `check-no-shell-out` lint gate rides
  WARN → RATCHET (shrink-only baseline) → HARD.
- **Rung 1 — `os_proc` introspection shim.** `lib/platform` grows an
  `os_proc_*` API (RSS, uptime, exe path, memory limit, cgroup stats, an
  fd-pin helper) with a Linux implementation that de-duplicates the two
  copies of the `/proc/self/stat` starttime parser
  (`app/services/src/node_health_service.c:54-90` and
  `app/controllers/src/agent_resources.c:61-118` are line-for-line the same
  algorithm today) and a FreeBSD mapping documented in header comments only
  (`kinfo_getproc`, `sysctl KERN_PROC_PATHNAME`, `fdlopen` for the hot-swap
  fd-pin at `lib/hotswap/src/hotswap_loader.c:693-695`).
- **Rung 2 — sandbox façade, design only.** `os_sandbox_enter(profile)`:
  `no_new_privs` + Landlock path grants (datadir RW, including the
  late-opened `<datadir>/ssl` — HTTPS defers during IBD, so path grants,
  not pre-opened fds, are mandatory) + a seccomp-bpf **deny-list**
  (`execve`, `ptrace`, `process_vm_*`, the `mount` family, `bpf`, `kexec`,
  `add_key`) rather than an allow-list, because a strict allow-list under
  OpenSSL + SQLite + embedded Tor + pthreads is a long-tail crash generator,
  and the prize — blocking `execve`, unlocked by Rung 0 — comes from the
  deny-list, not from allow-list completeness. Implementation is
  owner-gated; this ADR authorizes the design and Rung 0/1 groundwork only.

## Explicitly deferred (with reasons)

- **epoll/kqueue reactor.** `lib/net/src/connman.c:1270` runs one `poll()`
  thread over a `≤256`-fd array (`connman.c:1230`) at a 50 ms timeout — no
  measured need for an edge-triggered reactor at today's peer counts.
- **Custom allocator.** `zcl_malloc` (`util/safe_alloc.h`) is a checked
  wrapper over the system allocator; no measured fragmentation pain.
- **Thread priorities / QoS.** Contradicts the liveness philosophy (cursor +
  named blocker, never "starve the low-priority thing quietly"); no
  measured starvation.
- **A FreeBSD port.** Not undertaken now; the Rung 1 shim keeps the door
  open without paying the cost today.
- **A unikernel.** A horizon that disciplines the ports/adapters seam
  (`docs/FRAMEWORK.md`), not a work item.

## Consequences

**Positive:**

- The node stays one process, one binary, no privilege-separated helper —
  consistent with `docs/adr/0001`'s one-binary positioning.
- The sandbox design (Rung 2) reuses the same pattern ADR-0002 already
  proved works for this codebase: a structural, build/boot-time boundary
  (there, a source-tree seal; here, a syscall deny-list) rather than a
  convention a reviewer has to remember.
- Rung 0's shell-out removal is valuable independent of sandboxing — it
  also removes ~12 sites where `system()`'s return code is already known to
  be untrustworthy tree-wide, because `alerts_init()`
  (`lib/util/src/alerts.c:287-291`) installs a process-wide `SIGCHLD`
  handler with `SA_NOCLDWAIT` so `system()`'s internal `waitpid()` reliably
  fails `ECHILD` (documented in-place at
  `app/services/src/utxo_recovery_ldb_copy.c:76-85`).

**Negative / Risk:**

- Rung 0 is nontrivial: 12 sites across `app/services`, `app/models`,
  `app/conditions`, `app/controllers`, and `src/main.c`, one of which
  (`alerts.c`'s `SA_NOCLDWAIT` install) makes every other `system()` call's
  return code unreliable until it is the *last* site migrated — sequencing
  matters, not just coverage.
- Rung 2's deny-list is honest about three breaks that need owner sign-off:
  the self-respawn `execv()` at `src/main.c:3104` (already conditioned on
  `!sd_notify_is_active()` — under systemd it never fires; off-systemd it
  needs a scoped Landlock EXECUTE grant, not a bare deny), dev/agent
  runners that are incompatible with the strict profile, and an audit of
  SQLite WAL/shm/temp file paths plus the agent-test status directory
  (`app/controllers/src/agent_test_controller.c:130-139` defaults to
  `$HOME/.zclassic-c23-agent-test-status`, outside the datadir) before any
  path-grant sandbox can be enabled by default.
- Network confinement (outbound connection filtering) is out of scope for
  Rung 2 — it targets local resource/exec confinement only.

## Verification notes (what this review could and could not confirm)

- **Confirmed by reading code and git history:** all four bottleneck items
  in Evidence (a) against `docs/work/refold-fold-rate-bottlenecks.md`'s
  cited lines; that `thread_pool.c`/`thread_pool.h` and `verify_queue.c` do
  **not** exist in the current tree (`git log --all -- lib/validation/src/thread_pool.c
  lib/validation/include/validation/thread_pool.h` shows build-then-delete
  at `9c5357930` → `b1e27ce0e`, both ancestors of `HEAD`); zero
  `seccomp|landlock|capsicum|pledge(|setrlimit` matches outside `vendor/`;
  all 12 shell-out call sites and their exact line ranges; the duplicated
  `/proc/self/stat` parser in both cited files; `connman.c`'s 256-fd cap and
  50 ms `poll()` timeout at the cited lines; the `SIGCHLD`/`SA_NOCLDWAIT`
  install and its documented consequence.
- **Not independently re-verified this session:** live behavior of a
  Landlock/seccomp profile under this node's actual OpenSSL/SQLite/Tor
  thread mix (Rung 2 is design-only, no implementation exists to test);
  whether every `/proc/self/*` read site in the tree was found by grep (the
  count is a lower bound, not an exhaustive audit).
