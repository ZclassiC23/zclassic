# Session-substrate gating probes (P0.0, multi-user-server program)

Empirical results for the six gating probes that determine what per-session
isolation is achievable on this box. Every claim below was produced by
compiling and **running** a throwaway C23 probe (`cc -std=c23 -O0`) and
capturing its real stdout/exit status — no claim here is inferred from a man
page or from `docs/adr/0003-os-substrate-verdict.md` (read for context, cited
where it corroborates or is corroborated by these probes, but every number
below was independently re-measured). The node, live datadirs, and the live
service were not touched — every probe ran against throwaway scratch files.

Probe sources: `probe{1..5}_*.c`, run from the scratchpad (not checked in —
throwaway; re-run to reproduce). Probe 6 is a code read, not a runtime probe.

## Box

- `uname -a`: `Linux rhett.dev 6.8.0-111-generic #111-Ubuntu SMP PREEMPT_DYNAMIC Sat Apr 11 23:16:02 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux`
- Distro: Ubuntu 24.04.3 LTS (noble), `ID_LIKE=debian`
- `nproc`: 32, RAM: 93 GiB total / 50 GiB available
- uid: 1000 (`rhett`), rootless throughout — no `sudo` used in any probe
- AppArmor: module loaded, current shell process is **`unconfined`**
  (`/proc/self/attr/current` = `unconfined`) — load-bearing for Probe 1, see
  below
- Repo fork point: `2678b7e51` ("docs: ADR-0003 os-substrate verdict + three-
  rung working plan") — this doc is the empirical follow-on to that ADR's
  Rung-2 design section, for the *separate* multi-user-server program (per-
  session process isolation), not the ADR's single-process self-sandbox.

## PROBE 1 (THE GATE) — unprivileged user namespaces

```
uid=1000 euid=1000

-- /proc/sys knobs --
  /proc/sys/kernel/apparmor_restrict_unprivileged_userns = "1"
  /proc/sys/user/max_user_namespaces = "382290"
  /proc/sys/kernel/unprivileged_userns_clone = "1"

-- unshare(CLONE_NEWUSER) in a forked child --
  unshare(CLONE_NEWUSER) = 0 (SUCCESS)
  post-unshare getuid()=65534
  child exit status: 0

-- clone() with CLONE_NEWUSER|CLONE_NEWNET|CLONE_NEWPID|CLONE_NEWNS as uid 1000 --
  clone(...) = 259960 (SUCCESS, child pid in parent ns)
  [clone child] alive, getuid()=65534 (namespace-mapped)
  child exit status: 0
```

- `unshare(CLONE_NEWUSER)` — **returns 0**, no `EPERM`.
- `clone(CLONE_NEWUSER|CLONE_NEWNET|CLONE_NEWPID|CLONE_NEWNS|SIGCHLD, ...)` as
  uid 1000 — **returns a valid child pid**, no `EPERM`. The child observes
  `getuid()==65534` (`nobody`), the standard unmapped-uid view inside a
  userns with no `/proc/<pid>/uid_map` written yet — expected, not an error.
- `apparmor_restrict_unprivileged_userns=1` is set on this box (Ubuntu's
  default hardening since 23.10) yet did **not** block either call.

**VERDICT — THE GATE IS OPEN, with one sharp caveat.** Full-namespace
isolation (`CLONE_NEWUSER` stacked with `NEWNET`/`NEWPID`/`NEWNS`) is
available rootless today. The caveat: Ubuntu's `apparmor_restrict_unprivileged_userns`
knob only blocks unprivileged userns creation for processes that are
**AppArmor-confined** by a profile lacking the `userns,` rule; our probe
process (and the current shell) is `unconfined`, which is why it worked. **Any
future AppArmor hardening pass on the zclassic23 binary or a session-child
binary must add an explicit `userns,` rule**, or this exact code path
silently reverts to `EPERM` on that host. This is a deployment-time
regression risk, not a code bug — record it as a standing constraint on any
AppArmor profile ever attached to the node or a session-child process.

Do **not** degrade to Landlock+seccomp+rlimits-only on this box — full
namespace stacking works. See the degraded-fallback profile at the bottom
for the case where a target host answers this probe differently (see also
Probe 4's Landlock ABI check, unaffected by userns availability either way).

## PROBE 2 — rootless PTY

```
posix_openpt = 3 (SUCCESS)
grantpt = 0 (SUCCESS)
unlockpt = 0 (SUCCESS)
ptsname_r = "/dev/pts/9" (SUCCESS)
  [child] ioctl(TIOCSCTTY) = 0 (SUCCESS, slave is now controlling tty)
read(master) = 28 bytes: "hello from pty slave child\n"
child exit status: 0
```

Full cycle — `posix_openpt(O_RDWR|O_NOCTTY)` → `grantpt` → `unlockpt` →
`ptsname_r` → child `setsid()` + `open(slave)` + `ioctl(TIOCSCTTY)` + write →
parent read from master — succeeds end to end with **zero** setuid helper
(`grantpt`'s classic `pt_chown` path was never invoked; modern glibc/kernel
handle devpts permission bits directly). `TIOCSCTTY` on the freshly-opened
slave succeeds because the child is a fresh session leader with no
controlling tty yet (the standard precondition).

**VERDICT:** a per-session controlling PTY (job control, `^C`/`^Z` signals,
window resize via `TIOCSWINSZ`) is achievable rootless with no privileged
component.

## PROBE 3 — seccomp-bpf against a real forked child (hand-rolled, no libseccomp)

```
  [child] prctl(PR_SET_NO_NEW_PRIVS,1) = 0 (SUCCESS)
  [child] prctl(PR_SET_SECCOMP, FILTER) = 0 (SUCCESS, filter installed)
  [child] post-filter glibc ops OK: sum=704982704 malloc+memset OK ptr=0x6553cf9ce2a0
  [child] now calling execve("/bin/true", ...) -- expect no further output
parent: child killed by signal 31 (Bad system call) -- CONFIRMS SIGSYS/kill on execve
```

Filter: hand-written `struct sock_fprog` via `<linux/filter.h>`/`<linux/seccomp.h>`
BPF macros (arch check → `AUDIT_ARCH_X86_64` else kill; syscall nr compared
against `__NR_execve`/`__NR_execveat` → `SECCOMP_RET_KILL_PROCESS`; default
`SECCOMP_RET_ALLOW`). Confirmed: the child runs a normal loop, `malloc(4096)`,
`memset`, and two `printf`s **after** the filter is installed with no
disruption, then `execve("/bin/true", ...)` terminates it — `waitpid`
reports `WIFSIGNALED`, `WTERMSIG == 31 == SIGSYS`.

**Tuning note (the "any OTHER syscall" ask):** `strace -f -c` over the same
binary (a trivial glibc program: one loop, one malloc, a few printfs) shows
**~50 syscalls across 20 distinct syscall names** before/around the single
deliberate `execve`: `read write close fstat mmap mprotect munmap brk
pread64 access clone execve wait4 prctl arch_prctl set_tid_address openat
set_robust_list prlimit64 getrandom rseq`. Most of these (`mmap`, `mprotect`,
`openat`, `arch_prctl`, `set_tid_address`, `set_robust_list`, `rseq`,
`prlimit64`, `getrandom`) fire during **dynamic-loader/libc startup alone**,
before any application code runs. This empirically re-confirms
`docs/adr/0003-os-substrate-verdict.md` §Rung-2's design choice of a
**deny-list** (block `execve`/`ptrace`/`process_vm_*`/`mount`*/`bpf`/`kexec`*/
`add_key`* etc.) over an allow-list: even a do-nothing glibc program already
touches ~20 syscall names, so a hand-picked allowlist under the node's real
dependency stack (OpenSSL, SQLite, embedded Tor, pthreads) would be exactly
the "slow-drip crash generator" the ADR predicted.

**VERDICT:** a hand-rolled seccomp-bpf deny-list, no libseccomp dependency,
correctly kills `execve`/`execveat` while leaving normal glibc operation
untouched — confirmed by direct execution, not inference.

## PROBE 4 — Landlock

```
landlock_create_ruleset(VERSION) = 4  (Landlock ABI version)
pre-restrict open(preopen_file) = 3 (fd opened BEFORE landlock_restrict_self)
landlock_create_ruleset(attr) = 4 (SUCCESS)
landlock_add_rule(granted_dir) = 0 (SUCCESS)
landlock_restrict_self = 0 (SUCCESS -- process is now sandboxed)
open(outside_file) AFTER restrict = -1 errno=13 (Permission denied)  -- EXPECTED (denied)
open(granted_file) AFTER restrict = 4 -- EXPECTED (allowed)
  write(fd_inside) = 21 (errno=0 if -1)
write(preopened_fd) AFTER restrict = 26 bytes -- PRE-OPENED FD SURVIVES ENFORCEMENT
```

Raw syscalls only (`syscall(__NR_landlock_create_ruleset/_add_rule/_restrict_self, ...)`
via `<linux/landlock.h>`, no liblandlock). **ABI version 4** — this kernel
supports network rules (`LANDLOCK_ACCESS_NET_BIND_TCP`/`CONNECT_TCP`, ABI v4)
in addition to the filesystem rules this probe exercised; the network side
was not empirically tested here (out of scope for this probe set) but is
available per the header for a future probe if the program needs it.

All four sub-checks matched prediction exactly:
1. Ruleset created granting `READ_FILE|WRITE_FILE|READ_DIR` under one scratch
   dir only.
2. `open()` on a file **outside** that dir, after `landlock_restrict_self`,
   returns `-1 EACCES` — denied.
3. `open()`/`write()` on a file **inside** the granted dir, after restrict,
   succeeds.
4. An fd opened to a file **outside** the granted dir, but opened **before**
   `landlock_restrict_self` was called, remains fully writable afterward
   (26-byte write succeeds) — confirms `linux/landlock.h`'s own doc comment
   ("Files or directories opened before the sandboxing are not subject to
   these restrictions").

**VERDICT:** Landlock (ABI v4) is fully functional rootless for path-scoped
FS confinement, and the pre-opened-fd-survives-enforcement property holds —
useful for handing a session child exactly the fds it needs (e.g. a PTY
master/slave pair, a log fd) *before* calling `landlock_restrict_self`,
rather than needing a path grant for everything.

## PROBE 5 — socketpair round-trip latency

```
socketpair(AF_UNIX, SOCK_STREAM) = 0 (SUCCESS), fds=3,4
round-trip stats over 10000 iterations:
  min    = 4.53 us
  p10    = 5.65 us
  median = 6.11 us
  mean   = 6.22 us
  p90    = 6.87 us
  max    = 22.72 us
VERDICT: median IS UNDER ~1ms interactive-feel threshold (6.11 us < 1000 us)
```

`AF_UNIX`/`SOCK_STREAM` `socketpair`, fork, N=10000 round trips of a small
`{path,json}`-shaped request (~90 bytes) and a bounded JSON reply (~70
bytes), `CLOCK_MONOTONIC` timed per-iteration in the parent, sorted for
percentiles.

**VERDICT:** median 6.11 us, p90 6.87 us, worst-case 22.72 us — roughly
**160x** of headroom under the ~1 ms interactive-feel budget. A
"child renders, parent executes" split over `socketpair` is unconditionally
fast enough; latency is a non-issue for this design.

## PROBE 6 — Tor stream API (code read, not a runtime probe)

Read `lib/net/include/net/tor_integration.h` (117 lines) and
`lib/net/include/net/onion_service.h` (60 lines), then the implementation at
`lib/net/src/tor_integration.c:404-473` for the outbound side.

- **Inbound:** `onion_service_handle_request(method, path, body, body_len,
  response, response_max)` — a synchronous callback dynhost invokes per
  request, returns bytes written to `response`. One call in, one response
  out. Not a stream.
- **Outbound:** `tor_integration_fetch_onion(onion_address, path, callback,
  ctx, timeout_secs)` (`tor_integration.c:412-426`) and its blocking wrapper
  `tor_integration_fetch_onion_blocking` (`:445-473`) — both are a **single
  GET-shaped fetch** of one path against one `.onion` address, backed by a
  weak-linked `dynhost_client_fetch(addr, port=80, path, callback, ctx,
  timeout)`. The callback fires exactly once with a status + complete body
  (`blocking_fetch_cb`, `:429-443`, sets `result->complete` once). There is
  no handle returned to the caller for a second write, no persistent
  circuit/session object exposed across calls, and no raw-socket-equivalent
  API anywhere in either header.
- No `.c`/`.h` in `lib/net/` or `vendor/tor_stub.c` exposes a bidirectional,
  long-lived onion stream (i.e. nothing like "open a stream, write N times,
  read N times, close").

**VERDICT — code-confirmed:** zclassic23's embedded-Tor surface is
**request/response only, both directions** (inbound: one dynhost callback
per request; outbound: one `fetch_onion` call per GET). This matches and
independently re-confirms `docs/adr/0003-os-substrate-verdict.md`'s framing
("there is no outbound-stream API in that header"). **Grounds the P1
boundary as stated: P1 session traffic must use raw TCP (direct clearnet, or
the existing one-shot onion request/response surfaces where applicable) —
a persistent onion-tunneled session stream is new scope, correctly deferred
past P1.**

---

## CAPABILITY MATRIX

| # | Capability | Rootless on this box? | Evidence |
|---|---|---|---|
| 1 | `unshare(CLONE_NEWUSER)` | **YES** (0, no EPERM) | Probe 1 |
| 1 | `clone()` stacking `NEWUSER\|NEWNET\|NEWPID\|NEWNS` | **YES** (valid child pid) | Probe 1 |
| 1 | ...but only while AppArmor-**unconfined** | caveat, not a failure today | Probe 1 + `/proc/self/attr/current` |
| 2 | Rootless PTY (`posix_openpt`→`TIOCSCTTY`), no setuid helper | **YES**, full cycle | Probe 2 |
| 3 | Hand-rolled seccomp-bpf (no libseccomp), kills `execve`/`execveat` | **YES**, confirmed SIGSYS (31) | Probe 3 |
| 3 | Normal glibc code (malloc/printf/loop) survives the same filter | **YES** | Probe 3 |
| 4 | Landlock LSM, raw syscalls, path-scoped FS grants | **YES**, ABI v4 | Probe 4 |
| 4 | Landlock: outside-grant `open()` → EACCES | **YES** | Probe 4 |
| 4 | Landlock: pre-opened fd survives `restrict_self` | **YES** | Probe 4 |
| 5 | `socketpair` round trip fast enough for interactive feel (<~1ms) | **YES**, median 6.11 us (~160x headroom) | Probe 5 |
| 6 | Persistent/bidirectional outbound or inbound Tor **stream** API | **NO** — request/response only, both directions | Probe 6 (code read) |

## BOTTOM LINE

**#1 verdict: user namespaces ARE available rootless on this box (uid 1000,
Ubuntu 24.04.3, kernel 6.8.0-111-generic) — the gate is open, not degraded.**
`unshare(CLONE_NEWUSER)` and `clone(CLONE_NEWUSER|CLONE_NEWNET|CLONE_NEWPID|CLONE_NEWNS)`
both succeed cleanly as long as the calling process is AppArmor-unconfined
(true today; must stay true, or be given an explicit `userns,` grant, for
any hardened profile applied later).

### Achievable P1 isolation profile — full-namespace (this box, today)

Per session:
1. `clone(CLONE_NEWUSER|CLONE_NEWNET|CLONE_NEWPID|CLONE_NEWNS|SIGCHLD, ...)`
   — private user/net/pid/mount namespace per session child. `NEWNET` gives
   an isolated network stack (loopback-only until deliberately bridged);
   `NEWPID` means the session can't see or signal host/other-session
   processes; `NEWNS` allows a private mount view if the session ever needs
   one (e.g. a scoped bind-mount of its own scratch dir).
2. A dedicated controlling PTY per session (Probe 2) for full interactive
   terminal semantics — no setuid helper needed.
3. Landlock path-beneath grants (Probe 4, ABI v4) scoping the session
   process to its own session directory; open any fds the child needs (PTY
   slave, log file) **before** `landlock_restrict_self` so they survive
   enforcement without needing a path grant.
4. A seccomp-bpf **deny-list** (Probe 3's confirmed pattern, matching
   ADR-0003 Rung 2's already-decided design) — `execve`/`execveat`,
   `ptrace`, `process_vm_readv`/`writev`, the `mount` family, `bpf`,
   `kexec_load`/`kexec_file_load`, `add_key`/`request_key`/`keyctl` — applied
   *after* the session child's own one-time setup (namespace entry, PTY
   open, Landlock restrict) is complete, since Landlock/PTY/clone setup
   itself needs syscalls a deny-list would otherwise have to special-case.
5. `socketpair(AF_UNIX, SOCK_STREAM)` (Probe 5) for the control-plane
   channel between the session-owning parent and the sandboxed child —
   confirmed ~6 us median round trip, no latency concern for a
   render-in-child/execute-in-parent (or vice versa) split.
6. Network: **raw TCP only** at P1 (direct clearnet connections, or the
   existing one-shot onion request/response surfaces where a session needs
   to reach an onion peer). A persistent onion-tunneled session stream
   (Probe 6) does not exist in-tree today and is out of scope for P1 by
   design, not by oversight.

### Degraded fallback — if a target host answers Probe 1 differently

If a future deployment host reports `unshare(CLONE_NEWUSER) == -1 (EPERM)`
(e.g. `unprivileged_userns_clone=0`, a kernel built without
`CONFIG_USER_NS`, or — the sharp caveat above — an AppArmor profile applied
to the session-substrate binary without a `userns,` rule), **do this
instead**, in order:

1. **Drop `CLONE_NEWUSER`/`NEWNET`/`NEWPID`/`NEWNS` entirely.** None of
   Probes 2-5 depend on user namespaces — PTY allocation, seccomp-bpf, and
   Landlock are all available to any process regardless of userns support
   (confirmed independently: Probes 2-4 never call `unshare`/`clone` with
   namespace flags, only Probe 1 does).
2. **Landlock (Probe 4) becomes the sole filesystem confinement layer** —
   still ABI-version-gated (query via
   `landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION)` per
   Probe 4; if that itself returns `< 0`, Landlock is unavailable too and
   filesystem confinement degrades further to path validation in
   application code, which is not a kernel-enforced boundary).
3. **seccomp-bpf (Probe 3) becomes the sole exec/introspection confinement
   layer** — unaffected by userns availability; the deny-list still blocks
   `execve`/`ptrace`/etc.
4. **POSIX rlimits close the resource-isolation gap left by no `NEWPID`/
   `NEWNET`:** `setrlimit(RLIMIT_NOFILE, RLIMIT_AS, RLIMIT_CPU, RLIMIT_NPROC)`
   per session child — available unconditionally, no capability required.
5. **PID isolation without `CLONE_NEWPID`:** track the session by process
   group (`setpgid` + `killpg` on session end) instead of namespace
   containment — the session child can still see host PIDs via `/proc`, so
   this is strictly weaker than Probe 1's namespace result, but it bounds
   signal/kill blast radius to the session's own process tree.
6. **Network isolation without `CLONE_NEWNET`:** no cheap unprivileged
   substitute exists. Either (a) rely on the seccomp deny-list to block
   `socket(AF_PACKET, ...)`/raw sockets/netlink (denies an attacker's
   easiest pivot primitives) while otherwise sharing the host network
   namespace, or (b) if network confinement is a hard requirement on that
   host, it becomes an owner-gated decision to run session children under a
   delegated network namespace pre-created by a privileged one-time setup
   step (outside the per-session unprivileged fast path) — explicitly a
   fallback, not the default design.

This fallback matches `docs/adr/0003-os-substrate-verdict.md`'s Rung 2
scope (Landlock + seccomp deny-list, no user namespaces mentioned there at
all) — meaning even in the degraded case, the multi-user-server program
inherits a design the ADR already committed to for the single-process
self-sandbox, rather than needing a new primitive class.

---

*Reproduction: probe sources are throwaway scratch files (not checked in),
compiled with `cc -std=c23 -O0 -o probeN probeN.c` and run directly — no
`sudo`, no node/datadir/service touched. Re-run against any candidate
deployment host before trusting this doc's verdict for that host; Probe 1's
result in particular is host- and AppArmor-profile-specific, not a Linux-wide
constant.*
