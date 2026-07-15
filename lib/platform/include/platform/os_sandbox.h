/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * os_sandbox — composable process-confinement builders (Linux).
 *
 * Why:
 *   The multi-user-server program needs to run an untrusted per-session
 *   child, and Rung 2 of docs/adr/0003-os-substrate-verdict.md wants the
 *   steady-state node to sandbox itself too. Both want the SAME primitives:
 *   drop privileges, scope the filesystem, forbid dangerous syscalls, cap
 *   resources. This header is the single blessed home for those primitives —
 *   lib/platform is the raw-OS layer (the sibling of platform/clock.h and
 *   platform/rng.h), so the direct prctl/landlock/seccomp/setrlimit/clone
 *   syscalls that back these builders belong HERE, not scattered across the
 *   tree (mirrors the check_no_raw_clock_outside_platform.sh doctrine).
 *
 *   Every builder is INDEPENDENTLY callable and testable — you can drop
 *   no_new_privs alone, or Landlock alone — and os_sandbox_enter() composes
 *   a named `struct os_sandbox_profile` in the one correct order.
 *
 * Empirical grounding (docs/work/session-substrate-probes.md, run on the
 * target box: Ubuntu 24.04.3, kernel 6.8.0-111, uid 1000, rootless):
 *   - unprivileged user namespaces WORK (unshare/clone stack CLONE_NEWUSER|…)
 *   - a hand-rolled seccomp-bpf deny-list (no libseccomp) KILLs execve
 *   - Landlock ABI v4, outside-grant open→EACCES, pre-opened fd survives
 *   - prctl(PR_SET_NO_NEW_PRIVS) succeeds
 *
 * Portability / kernel assumptions (all DEGRADE at RUNTIME, never fail to
 * compile — the .c feature-tests headers via __has_include and falls back):
 *   - Landlock: kernel >= 5.13; the ABI version is PROBED at runtime, and an
 *     unavailable Landlock returns a typed zcl_result, never aborts.
 *   - seccomp: PR_SET_SECCOMP filter mode (kernel >= 3.5); SECCOMP_RET_
 *     KILL_PROCESS is preferred (>= 4.14) and falls back where the header
 *     lacks it.
 *   - user namespaces: rootless (CONFIG_USER_NS + unprivileged_userns_clone,
 *     AND the calling process AppArmor-unconfined — see the probes doc). A
 *     capability probe lets callers degrade to the Landlock+seccomp+rlimits
 *     profile on a host that answers EPERM.
 *   - Target of record: kernel 6.8.
 *
 * Thread-safety: these builders MUTATE the calling process/thread state and
 * are one-way (no_new_privs, Landlock restrict_self, seccomp are irreversible
 * by design). Call them from a single-threaded context (typically a freshly
 * forked/cloned child right before it starts its real work), never
 * concurrently.
 */

#ifndef ZCL_PLATFORM_OS_SANDBOX_H
#define ZCL_PLATFORM_OS_SANDBOX_H

#include "util/result.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>  /* pid_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Numeric codes carried in a non-ok struct zcl_result from this module.
 * Callers switch on r.code to decide whether to degrade (e.g. Landlock
 * unavailable) or treat the failure as fatal. */
enum os_sandbox_err {
    OS_SANDBOX_ERR_NONE                 = 0,
    OS_SANDBOX_ERR_LANDLOCK_UNAVAILABLE = -1,  /* kernel too old / disabled */
    OS_SANDBOX_ERR_LANDLOCK_SYSCALL     = -2,  /* create/add/restrict failed */
    OS_SANDBOX_ERR_SECCOMP              = -3,  /* prctl(PR_SET_SECCOMP) failed */
    OS_SANDBOX_ERR_SECCOMP_UNAVAILABLE  = -4,  /* built without seccomp headers */
    OS_SANDBOX_ERR_RLIMIT               = -5,  /* setrlimit failed */
    OS_SANDBOX_ERR_NO_NEW_PRIVS         = -6,  /* prctl(PR_SET_NO_NEW_PRIVS) */
    OS_SANDBOX_ERR_USERNS_MAP           = -7,  /* uid/gid map write failed */
    OS_SANDBOX_ERR_INVALID_ARG          = -8,
    OS_SANDBOX_ERR_TOO_MANY_RULES       = -9,  /* filter would overflow bound */
};

/* A single path grant for the Landlock builder: everything BENEATH `path`
 * (the path is opened O_PATH and used as a path-beneath anchor) gets the
 * requested access. Directories should set allow_read to also permit
 * READ_DIR/listing. */
struct os_sandbox_path_rule {
    const char *path;
    bool        allow_read;
    bool        allow_write;
};

/* Sentinel for "leave this resource limit untouched". A real limit of 0
 * (e.g. RLIMIT_CORE = 0 to disable core dumps) is set explicitly; only this
 * sentinel means keep. */
#define OS_SANDBOX_RLIMIT_KEEP UINT64_MAX

/* Resource caps. Any field == OS_SANDBOX_RLIMIT_KEEP is left as-is; every
 * other field lowers the soft (and, where lowered, hard) limit. */
struct os_sandbox_rlimits {
    uint64_t as_bytes;     /* RLIMIT_AS    — total virtual memory           */
    uint64_t cpu_seconds;  /* RLIMIT_CPU   — CPU-seconds (SIGXCPU then KILL) */
    uint64_t nproc;        /* RLIMIT_NPROC — processes for the real uid      */
    uint64_t fsize_bytes;  /* RLIMIT_FSIZE — max file size (SIGXFSZ)         */
    uint64_t nofile;       /* RLIMIT_NOFILE— open fd ceiling                 */
    uint64_t core_bytes;   /* RLIMIT_CORE  — core dump size (0 = disabled)   */
};

/* A named, composable confinement recipe. os_sandbox_enter() applies the
 * enabled builders in the ONE correct order (see the ordering invariant on
 * os_sandbox_enter). Namespaces are NOT part of this struct: they must be
 * entered by the CALLER's clone()/unshare() BEFORE os_sandbox_enter(),
 * because they change the pid/mount/net view the later builders operate in. */
struct os_sandbox_profile {
    const char *name;

    bool no_new_privs;   /* PR_SET_NO_NEW_PRIVS + PR_SET_DUMPABLE(0) */

    bool apply_rlimits;
    struct os_sandbox_rlimits rlimits;

    bool landlock;                              /* apply the fs grants below */
    const struct os_sandbox_path_rule *fs_rules;
    size_t n_fs_rules;

    bool seccomp;                               /* install the deny-list     */
    const int *denied_syscalls;                 /* array of __NR_* values    */
    size_t n_denied;
    bool seccomp_deny_exec_mmap;                /* also W^X: PROT_EXEC mmap/mprotect */
};

/* Runtime capability report. Lets a caller pick the full-namespace profile or
 * degrade to Landlock+seccomp+rlimits without a userns. Never aborts. */
struct os_sandbox_caps {
    bool userns;        /* unshare(CLONE_NEWUSER) succeeds rootless          */
    int  landlock_abi;  /* Landlock ABI version, or <= 0 if unavailable      */
    bool seccomp;       /* seccomp filter mode compiled in + reachable       */
};

/* The session namespace set as a compile-time named constant — available
 * only where <sched.h> (with _GNU_SOURCE) is already included by the caller;
 * otherwise use os_sandbox_session_ns_flags(), which returns the same value.
 * Kept as a macro (not forcing sched.h into this header) so a caller that
 * does not need clone flags is not dragged into _GNU_SOURCE. */
#if defined(CLONE_NEWUSER) && defined(CLONE_NEWNET) && defined(CLONE_NEWPID) \
    && defined(CLONE_NEWNS) && defined(CLONE_NEWIPC) && defined(CLONE_NEWUTS)
#define SANDBOX_SESSION_NS_FLAGS \
    (CLONE_NEWUSER | CLONE_NEWNET | CLONE_NEWPID | CLONE_NEWNS | \
     CLONE_NEWIPC | CLONE_NEWUTS)
#endif

/* ── Individual builders (each independently callable/testable) ─────────── */

/* prctl(PR_SET_NO_NEW_PRIVS, 1) + prctl(PR_SET_DUMPABLE, 0). Required before
 * a rootless Landlock restrict_self or seccomp filter install. Returns true
 * on success; logs and returns false otherwise. */
bool os_sandbox_no_new_privs(void);

/* Probe the Landlock ABI version (landlock_create_ruleset(NULL,0,VERSION)).
 * Returns the ABI (>= 1) or -1 if Landlock is unavailable on this kernel or
 * this build lacks the headers. Non-mutating. */
int os_sandbox_landlock_abi(void);

/* Build a Landlock ruleset from `rules` and call landlock_restrict_self —
 * ONE-WAY. Only the accesses this kernel's ABI actually handles are enabled
 * (forward-compatible). ORDERING: the caller must open every fd the child
 * will need (PTY master/slave, log fd, control socket) BEFORE calling this —
 * pre-opened fds survive enforcement (probe 4), so they need no path grant.
 * On a kernel without Landlock this returns a non-ok result with code
 * OS_SANDBOX_ERR_LANDLOCK_UNAVAILABLE so the caller can DEGRADE (it does not
 * abort). Requires no_new_privs (or CAP_SYS_ADMIN) to have run first. */
struct zcl_result os_sandbox_landlock_restrict(
    const struct os_sandbox_path_rule *rules, size_t n_rules);

/* The session child's denied-syscall set (execve/execveat, the clone/fork
 * family, the socket family, ptrace/process_vm_*, mount family, bpf, kexec,
 * key management, setns/unshare, module ops, open_by_handle_at,
 * perf_event_open, …). Returns a pointer to a static const array; *count_out
 * receives its length. This is the named SESSION deny-set constant. */
const int *os_sandbox_session_denied_syscalls(size_t *count_out);

/* The RESIDENT NODE's deny-set: execution + namespace/mount/kernel escape only
 * (execve/execveat, ptrace/process_vm_*, mount family, setns/unshare/pivot_root,
 * bpf/kexec/module ops, keyrings, open_by_handle_at, perf_event_open). Unlike
 * the SESSION set it does NOT deny socket/connect/clone — the node legitimately
 * opens peer sockets and spawns threads over its lifetime, so denying those
 * would kill a running node. Returns a static const array; *count_out receives
 * its length. Named the NODE STEADY-STATE deny-set constant. */
const int *os_sandbox_node_steady_denied_syscalls(size_t *count_out);

/* Install a hand-rolled seccomp-bpf DENY-list (no libseccomp): every syscall
 * in `denied` returns SECCOMP_RET_KILL_PROCESS; everything else defaults to
 * SECCOMP_RET_ALLOW. A deny-list (not an allow-list) is deliberate — even a
 * do-nothing glibc program touches ~20 syscall names (probe 3). When
 * `deny_exec_mmap` is true, an arg-filter also KILLs mmap/mprotect/
 * pkey_mprotect calls that request PROT_EXEC (W^X). ONE-WAY. Apply AFTER
 * no_new_privs and AFTER every other one-time setup (Landlock, PTY, clone),
 * since those need syscalls the deny-list would otherwise have to special-
 * case. Returns non-ok (never aborts) on failure or if built without the
 * seccomp headers. */
struct zcl_result os_sandbox_seccomp_deny(const int *denied, size_t n_denied,
                                          bool deny_exec_mmap);

/* The session child's default resource caps: AS≈256 MiB, NPROC=1,
 * FSIZE≈64 MiB, NOFILE=16, CORE=0, CPU=KEEP (caller sets a wall/CPU budget
 * per session). */
struct os_sandbox_rlimits os_sandbox_session_rlimits(void);

/* Apply the resource caps in `lim` (fields == OS_SANDBOX_RLIMIT_KEEP are
 * skipped). Returns non-ok on the first setrlimit failure. */
struct zcl_result os_sandbox_set_rlimits(const struct os_sandbox_rlimits *lim);

/* The CLONE_* flag set a session child is cloned with (== SANDBOX_SESSION_NS_
 * FLAGS). Provided as an accessor so callers need not pull in <sched.h>. */
int os_sandbox_session_ns_flags(void);

/* Write the uid/gid maps for a userns child (typically the PARENT calls this
 * for its just-cloned child: writes /proc/<pid>/uid_map + gid_map and
 * /proc/<pid>/setgroups = "deny", which is required before gid_map on modern
 * kernels). `pid` == 0 targets /proc/self. Maps a single id: inside_uid ->
 * the caller's real uid range of length 1 (pass inside_uid = real uid for an
 * identity map, or 65534 for nobody). Returns non-ok on failure. */
struct zcl_result os_sandbox_write_userns_maps(pid_t pid, unsigned inside_uid,
                                               unsigned inside_gid);

/* Probe what confinement this host actually supports, rootless, right now.
 * Forks a throwaway child for the userns probe so the calling process is not
 * mutated. Never aborts. */
struct os_sandbox_caps os_sandbox_probe_caps(void);

/* Convenience: true iff unprivileged user namespaces work here (the Probe-1
 * gate). Thin wrapper over os_sandbox_probe_caps().userns. */
bool os_sandbox_userns_available(void);

/* ── Named profiles (serve both users of the façade) ───────────────────── */

/* The full session-child confinement: no_new_privs + session rlimits +
 * Landlock scoped to `fs_rules` (the caller's per-session directory grants) +
 * the seccomp session deny-list with W^X. Namespaces (SANDBOX_SESSION_NS_
 * FLAGS) are assumed already entered by the caller's clone(). */
struct os_sandbox_profile os_sandbox_session_child_profile(
    const struct os_sandbox_path_rule *fs_rules, size_t n_fs_rules);

/* The lighter node-self profile (Rung 2, os-substrate-plan §3): no_new_privs
 * + Landlock (datadir grant) + seccomp deny-list, but NO rlimit clamp and NO
 * nproc=1 (the node legitimately runs many threads). Currently a scaffold —
 * not yet wired into boot; here so the façade serves the node too. */
struct os_sandbox_profile os_sandbox_node_steady_state_profile(
    const struct os_sandbox_path_rule *fs_rules, size_t n_fs_rules);

/* Apply a profile's enabled builders in the ONE correct order:
 *     1. no_new_privs   (unlocks rootless Landlock + seccomp)
 *     2. rlimits        (cheap, and cannot be re-raised after seccomp)
 *     3. Landlock       (fds the child needs are already pre-opened by caller)
 *     4. seccomp        (LAST — the setup above needs syscalls a deny-list
 *                        would have to special-case)
 * Namespaces are NOT applied here — the caller must have entered them via
 * clone()/unshare() first. ONE-WAY: there is no os_sandbox_exit(). On success
 * os_sandbox_active() reads true thereafter. Returns the first failing
 * builder's non-ok result (with the seccomp step, "failure" can mean the
 * process is already dead — a denied syscall kills it). */
struct zcl_result os_sandbox_enter(const struct os_sandbox_profile *p);

/* True after a successful os_sandbox_enter() in this process. */
bool os_sandbox_active(void);

/* The `name` of the profile that entered, or NULL if none has. Introspection
 * only (backs the `sandbox` dumpstate subsystem). */
const char *os_sandbox_active_profile_name(void);

/* True iff this build was compiled with the seccomp headers (so a seccomp
 * filter install is reachable). Cheap, non-forking — unlike os_sandbox_probe_
 * caps() it does not fork a child. */
bool os_sandbox_seccomp_supported(void);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_PLATFORM_OS_SANDBOX_H */
