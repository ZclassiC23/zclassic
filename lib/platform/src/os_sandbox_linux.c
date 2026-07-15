/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * os_sandbox — Linux backing for the process-confinement builders.
 * See platform/os_sandbox.h for the design, the empirical grounding
 * (docs/work/session-substrate-probes.md), and the ordering invariant.
 *
 * Raw syscalls (prctl, landlock_*, seccomp via prctl, setrlimit, unshare)
 * live here because lib/platform is the blessed raw-OS layer. The Landlock
 * and seccomp header groups are feature-tested with __has_include so a build
 * on an older kernel still COMPILES — the builders degrade at RUNTIME
 * (returning a typed zcl_result), never at compile time. */

#define _GNU_SOURCE

#include "platform/os_sandbox.h"

#include "util/log_macros.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

/* ── Feature detection (degrade at runtime, not compile-time) ──────────── */

#if defined(__has_include)
#  if __has_include(<linux/landlock.h>)
#    define ZCL_HAVE_LANDLOCK 1
#    include <linux/landlock.h>
#  endif
#  if __has_include(<linux/seccomp.h>) && __has_include(<linux/filter.h>) \
       && __has_include(<linux/audit.h>)
#    define ZCL_HAVE_SECCOMP 1
#    include <linux/seccomp.h>
#    include <linux/filter.h>
#    include <linux/audit.h>
#  endif
#endif

#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#endif

/* Landlock syscall numbers — supply fallbacks so the code compiles even where
 * the C library headers predate them (they are stable across arches on the
 * mainline ABI). */
#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#endif
#ifndef __NR_landlock_add_rule
#define __NR_landlock_add_rule 445
#endif
#ifndef __NR_landlock_restrict_self
#define __NR_landlock_restrict_self 446
#endif

/* SECCOMP_RET_KILL_PROCESS (kernel >= 4.14). Fall back to the raw value where
 * an older <linux/seccomp.h> only defines SECCOMP_RET_KILL (== kill thread). */
#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#endif

/* seccomp(2) mode + TSYNC flag (kernel >= 3.17). The seccomp(2) syscall with
 * SECCOMP_FILTER_FLAG_TSYNC installs the filter on EVERY thread of the calling
 * process atomically — unlike prctl(PR_SET_SECCOMP), which confines only the
 * calling thread and its later descendants. Fallbacks let this compile against
 * a libc predating the seccomp(2) wrapper/flag; on x86_64 __NR_seccomp==317. */
#ifndef __NR_seccomp
#define __NR_seccomp 317
#endif
#ifndef SECCOMP_SET_MODE_FILTER
#define SECCOMP_SET_MODE_FILTER 1
#endif
#ifndef SECCOMP_FILTER_FLAG_TSYNC
#define SECCOMP_FILTER_FLAG_TSYNC (1UL << 0)
#endif

/* Process-wide "sandbox entered" latch. Set once, never cleared (enter is
 * one-way); a plain bool is safe because enter() is single-threaded. */
static bool g_sandbox_active = false;
static const char *g_active_profile_name = NULL;

/* Which mechanism actually installed the seccomp filter, recorded so the
 * `sandbox` witness can prove thread coverage rather than intent: TSYNC means
 * the filter reached ALL threads atomically; PRCTL means only the caller +
 * later descendants. Atomic because the witness reads it from another thread. */
enum seccomp_install_method {
    SECCOMP_INSTALL_NONE = 0,
    SECCOMP_INSTALL_TSYNC,
    SECCOMP_INSTALL_PRCTL,
};
static _Atomic int g_seccomp_install_method = SECCOMP_INSTALL_NONE;

/* Count of threads that have PROVABLY entered a Landlock domain in this process
 * — incremented once per successful landlock_restrict_self. Landlock has no
 * TSYNC equivalent and is not retroactive, so this is the honest measure of
 * Landlock thread coverage (as opposed to seccomp, which TSYNC makes total).
 * Children a restricted thread later spawns inherit the domain but are NOT
 * counted here, so this is a conservative floor, never an over-count. */
static _Atomic int g_landlock_restrict_count = 0;

bool os_sandbox_active(void) { return g_sandbox_active; }

int os_sandbox_landlock_restricted_count(void)
{
    return atomic_load(&g_landlock_restrict_count);
}

const char *os_sandbox_seccomp_install_method(void)
{
    switch (atomic_load(&g_seccomp_install_method)) {
    case SECCOMP_INSTALL_TSYNC: return "tsync";
    case SECCOMP_INSTALL_PRCTL: return "prctl";
    default:                    return "";
    }
}

bool os_sandbox_seccomp_tsync_active(void)
{
    return atomic_load(&g_seccomp_install_method) == SECCOMP_INSTALL_TSYNC;
}

const char *os_sandbox_active_profile_name(void) { return g_active_profile_name; }

bool os_sandbox_seccomp_supported(void)
{
#ifdef ZCL_HAVE_SECCOMP
    return true;
#else
    return false;
#endif
}

/* ── no_new_privs ──────────────────────────────────────────────────────── */

bool os_sandbox_no_new_privs(void)
{
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        LOG_FAIL("os_sandbox", "prctl(PR_SET_NO_NEW_PRIVS) failed errno=%d (%s)",
                 errno, strerror(errno));
    /* PR_SET_DUMPABLE(0) also strips ptrace attach from a same-uid process,
     * which is part of the confinement — treat a failure as fatal too. */
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0)
        LOG_FAIL("os_sandbox", "prctl(PR_SET_DUMPABLE,0) failed errno=%d (%s)",
                 errno, strerror(errno));
    return true;
}

/* ── Landlock ──────────────────────────────────────────────────────────── */

#ifdef ZCL_HAVE_LANDLOCK
static int ll_create_ruleset(const struct landlock_ruleset_attr *attr,
                             size_t size, uint32_t flags)
{
    return (int)syscall(__NR_landlock_create_ruleset, attr, size, flags);
}
static int ll_add_rule(int ruleset_fd, enum landlock_rule_type type,
                       const void *attr, uint32_t flags)
{
    return (int)syscall(__NR_landlock_add_rule, ruleset_fd, type, attr, flags);
}
static int ll_restrict_self(int ruleset_fd, uint32_t flags)
{
    return (int)syscall(__NR_landlock_restrict_self, ruleset_fd, flags);
}
#endif /* ZCL_HAVE_LANDLOCK */

int os_sandbox_landlock_abi(void)
{
#ifdef ZCL_HAVE_LANDLOCK
    errno = 0;
    int abi = ll_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 0) return -1;
    return abi;
#else
    return -1;
#endif
}

struct zcl_result os_sandbox_landlock_restrict(
    const struct os_sandbox_path_rule *rules, size_t n_rules)
{
#ifndef ZCL_HAVE_LANDLOCK
    (void)rules;
    (void)n_rules;
    return ZCL_ERR(OS_SANDBOX_ERR_LANDLOCK_UNAVAILABLE,
                   "Landlock headers absent at build time");
#else
    int abi = os_sandbox_landlock_abi();
    if (abi < 0)
        return ZCL_ERR(OS_SANDBOX_ERR_LANDLOCK_UNAVAILABLE,
                       "Landlock unavailable on this kernel (ABI probe failed)");
    if (n_rules > 0 && rules == NULL)
        return ZCL_ERR(OS_SANDBOX_ERR_INVALID_ARG, "n_rules>0 but rules==NULL");

    /* Handle only the FS accesses this ABI knows about (forward-compatible).
     * ABI 1 introduced the file/dir bits; later ABIs add refer/truncate/net —
     * we do not enforce those here, so we do not need to handle them. */
    uint64_t handled = LANDLOCK_ACCESS_FS_READ_FILE |
                       LANDLOCK_ACCESS_FS_WRITE_FILE |
                       LANDLOCK_ACCESS_FS_READ_DIR;

    struct landlock_ruleset_attr rattr = { .handled_access_fs = handled };
    int ruleset_fd = ll_create_ruleset(&rattr, sizeof(rattr), 0);
    if (ruleset_fd < 0)
        return ZCL_ERR(OS_SANDBOX_ERR_LANDLOCK_SYSCALL,
                       "landlock_create_ruleset failed errno=%d (%s)",
                       errno, strerror(errno));

    for (size_t i = 0; i < n_rules; i++) {
        const struct os_sandbox_path_rule *r = &rules[i];
        if (r->path == NULL) {
            close(ruleset_fd);
            return ZCL_ERR(OS_SANDBOX_ERR_INVALID_ARG,
                           "path rule %zu has NULL path", i);
        }
        uint64_t allow = 0;
        if (r->allow_read)  allow |= LANDLOCK_ACCESS_FS_READ_FILE |
                                     LANDLOCK_ACCESS_FS_READ_DIR;
        if (r->allow_write) allow |= LANDLOCK_ACCESS_FS_WRITE_FILE;
        allow &= handled;

        int path_fd = open(r->path, O_PATH | O_CLOEXEC);
        if (path_fd < 0) {
            close(ruleset_fd);
            return ZCL_ERR(OS_SANDBOX_ERR_LANDLOCK_SYSCALL,
                           "open(O_PATH) %s failed errno=%d (%s)",
                           r->path, errno, strerror(errno));
        }
        struct landlock_path_beneath_attr pb = {
            .allowed_access = allow,
            .parent_fd = path_fd,
        };
        int rc = ll_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &pb, 0);
        close(path_fd);
        if (rc != 0) {
            close(ruleset_fd);
            return ZCL_ERR(OS_SANDBOX_ERR_LANDLOCK_SYSCALL,
                           "landlock_add_rule %s failed errno=%d (%s)",
                           r->path, errno, strerror(errno));
        }
    }

    if (ll_restrict_self(ruleset_fd, 0) != 0) {
        int e = errno;
        close(ruleset_fd);
        return ZCL_ERR(OS_SANDBOX_ERR_LANDLOCK_SYSCALL,
                       "landlock_restrict_self failed errno=%d (%s) "
                       "(no_new_privs run first?)", e, strerror(e));
    }
    close(ruleset_fd);
    atomic_fetch_add(&g_landlock_restrict_count, 1);
    return ZCL_OK;
#endif
}

/* ── seccomp-bpf deny-list ─────────────────────────────────────────────── */

/* The session child's deny-set. Guarded __NR_* so a syscall absent on this
 * arch (e.g. socketcall on x86_64) is simply omitted — never a build break. */
static const int g_session_denied[] = {
    /* code execution / process creation */
    __NR_execve, __NR_execveat,
    __NR_clone,
#ifdef __NR_clone3
    __NR_clone3,
#endif
#ifdef __NR_fork
    __NR_fork,
#endif
#ifdef __NR_vfork
    __NR_vfork,
#endif
    /* the socket family (NEWNET already isolates; belt-and-suspenders — a
     * pre-passed control fd is used via read/write, not a new socket) */
    __NR_socket,
#ifdef __NR_socketcall
    __NR_socketcall,
#endif
    __NR_socketpair, __NR_connect, __NR_bind, __NR_listen,
    __NR_accept, __NR_accept4, __NR_setsockopt, __NR_getsockopt,
    /* debugging / cross-process memory */
    __NR_ptrace, __NR_process_vm_readv, __NR_process_vm_writev,
    /* mount / namespace escape */
    __NR_mount, __NR_umount2, __NR_pivot_root, __NR_setns, __NR_unshare,
    /* kernel surface */
    __NR_bpf, __NR_kexec_load, __NR_kexec_file_load,
    __NR_init_module, __NR_finit_module, __NR_delete_module,
    __NR_perf_event_open,
    /* keyrings */
    __NR_add_key, __NR_request_key, __NR_keyctl,
    /* handle-based open bypass */
    __NR_open_by_handle_at,
};

const int *os_sandbox_session_denied_syscalls(size_t *count_out)
{
    if (count_out)
        *count_out = sizeof(g_session_denied) / sizeof(g_session_denied[0]);
    return g_session_denied;
}

/* The resident node's deny-set: execution + escape ONLY. Deliberately omits
 * the socket/clone/fork family the session set forbids — a running node opens
 * peer sockets and spawns threads, so those must stay allowed. */
static const int g_node_steady_denied[] = {
    /* code execution */
    __NR_execve, __NR_execveat,
    /* debugging / cross-process memory */
    __NR_ptrace, __NR_process_vm_readv, __NR_process_vm_writev,
    /* mount / namespace escape */
    __NR_mount, __NR_umount2, __NR_pivot_root, __NR_setns, __NR_unshare,
    /* kernel surface */
    __NR_bpf, __NR_kexec_load, __NR_kexec_file_load,
    __NR_init_module, __NR_finit_module, __NR_delete_module,
    __NR_perf_event_open,
    /* keyrings */
    __NR_add_key, __NR_request_key, __NR_keyctl,
    /* handle-based open bypass */
    __NR_open_by_handle_at,
};

const int *os_sandbox_node_steady_denied_syscalls(size_t *count_out)
{
    if (count_out)
        *count_out = sizeof(g_node_steady_denied) /
                     sizeof(g_node_steady_denied[0]);
    return g_node_steady_denied;
}

struct zcl_result os_sandbox_seccomp_deny(const int *denied, size_t n_denied,
                                          bool deny_exec_mmap)
{
#ifndef ZCL_HAVE_SECCOMP
    (void)denied;
    (void)n_denied;
    (void)deny_exec_mmap;
    return ZCL_ERR(OS_SANDBOX_ERR_SECCOMP_UNAVAILABLE,
                   "seccomp headers absent at build time");
#else
    if (n_denied > 0 && denied == NULL)
        return ZCL_ERR(OS_SANDBOX_ERR_INVALID_ARG, "n_denied>0 but denied==NULL");

    /* Upper bound: 4 (arch preamble) + 2 per denied syscall + 5 per W^X
     * syscall (mmap/mprotect/pkey_mprotect) + 2 tail (default allow + kill). */
    enum { WX_SYSCALLS = 3, MAX_FILTER = 512 };
    if (n_denied > (MAX_FILTER - 6 - WX_SYSCALLS * 5) / 2)
        return ZCL_ERR(OS_SANDBOX_ERR_TOO_MANY_RULES,
                       "denied set too large (%zu) for the BPF filter bound",
                       n_denied);

    struct sock_filter filt[MAX_FILTER];
    size_t k = 0;

    /* Arch check: kill on any non-x86_64 caller (a 32-bit compat syscall
     * would otherwise slip a different numbering past the nr comparisons). */
    filt[k++] = (struct sock_filter)BPF_STMT(
        BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
    filt[k++] = (struct sock_filter)BPF_JUMP(
        BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0);
    filt[k++] = (struct sock_filter)BPF_STMT(
        BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);

    /* Load the syscall number into the accumulator. */
    filt[k++] = (struct sock_filter)BPF_STMT(
        BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));

    /* W^X arg-filter: for mmap/mprotect/pkey_mprotect, KILL if the prot arg
     * (args[2]) has PROT_EXEC set. Each block reloads nr afterwards so the
     * subsequent nr comparisons still work. Emitted BEFORE the plain deny
     * comparisons so the accumulator holds nr at entry to each. */
    if (deny_exec_mmap) {
        const int wx[WX_SYSCALLS] = {
            __NR_mmap, __NR_mprotect,
#ifdef __NR_pkey_mprotect
            __NR_pkey_mprotect,
#else
            -1,
#endif
        };
        for (int i = 0; i < WX_SYSCALLS; i++) {
            if (wx[i] < 0) continue;
            /* if nr != wx -> skip the 4-instr check block (land on reload) */
            filt[k++] = (struct sock_filter)BPF_JUMP(
                BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)wx[i], 0, 4);
            /* load low 32 bits of args[2] (prot); PROT_EXEC fits the low word */
            filt[k++] = (struct sock_filter)BPF_STMT(
                BPF_LD | BPF_W | BPF_ABS,
                offsetof(struct seccomp_data, args[2]));
            /* if (prot & PROT_EXEC) -> next instr (kill); else skip it */
            filt[k++] = (struct sock_filter)BPF_JUMP(
                BPF_JMP | BPF_JSET | BPF_K, 0x4 /*PROT_EXEC*/, 0, 1);
            filt[k++] = (struct sock_filter)BPF_STMT(
                BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
            /* reload nr for the following comparisons */
            filt[k++] = (struct sock_filter)BPF_STMT(
                BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
        }
    }

    /* Plain deny comparisons: each is 2 instructions (compare, then a RET KILL
     * that is skipped on jf=1 when the syscall does not match). Length-
     * independent, so no distant label arithmetic. */
    for (size_t i = 0; i < n_denied; i++) {
        filt[k++] = (struct sock_filter)BPF_JUMP(
            BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)denied[i], 0, 1);
        filt[k++] = (struct sock_filter)BPF_STMT(
            BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    }

    /* Default: allow. */
    filt[k++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);

    struct sock_fprog prog = {
        .len = (unsigned short)k,
        .filter = filt,
    };

    /* Install on ALL threads atomically via seccomp(2)+TSYNC so the filter
     * lands on every already-running thread, not just the caller and its
     * future descendants. Return-value contract (man 2 seccomp):
     *   0        -> installed on every thread (TSYNC succeeded)
     *   > 0      -> the tid of a thread whose existing filter is incompatible;
     *              coverage is NOT total, so fail closed
     *   -1/ENOSYS-> seccomp(2) absent (kernel < 3.17) -> fall back to prctl
     *   -1/other -> genuine failure -> fail closed */
    errno = 0;
    long tsync = syscall(__NR_seccomp, (long)SECCOMP_SET_MODE_FILTER,
                         (long)SECCOMP_FILTER_FLAG_TSYNC, &prog);
    if (tsync == 0) {
        atomic_store(&g_seccomp_install_method, SECCOMP_INSTALL_TSYNC);
        return ZCL_OK;
    }
    if (tsync > 0)
        return ZCL_ERR(OS_SANDBOX_ERR_SECCOMP,
                       "seccomp(TSYNC) could not synchronise thread tid=%ld "
                       "(incompatible pre-existing filter) — refusing partial "
                       "coverage", tsync);
    if (errno != ENOSYS)
        return ZCL_ERR(OS_SANDBOX_ERR_SECCOMP,
                       "seccomp(SET_MODE_FILTER, TSYNC) failed errno=%d (%s) "
                       "(no_new_privs run first?)", errno, strerror(errno));

    /* ENOSYS: no seccomp(2) on this kernel — the prctl path still installs the
     * filter on the calling thread + its future descendants (partial coverage,
     * recorded so the witness reports it honestly). */
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0) != 0)
        return ZCL_ERR(OS_SANDBOX_ERR_SECCOMP,
                       "prctl(PR_SET_SECCOMP) failed errno=%d (%s) "
                       "(no_new_privs run first?)", errno, strerror(errno));
    atomic_store(&g_seccomp_install_method, SECCOMP_INSTALL_PRCTL);
    return ZCL_OK;
#endif
}

/* ── rlimits ───────────────────────────────────────────────────────────── */

struct os_sandbox_rlimits os_sandbox_session_rlimits(void)
{
    return (struct os_sandbox_rlimits){
        .as_bytes    = (uint64_t)256 * 1024 * 1024,
        .cpu_seconds = OS_SANDBOX_RLIMIT_KEEP,   /* caller sets a session budget */
        .nproc       = 1,
        .fsize_bytes = (uint64_t)64 * 1024 * 1024,
        .nofile      = 16,
        .core_bytes  = 0,
    };
}

static struct zcl_result set_one_rlimit(int resource, uint64_t v, const char *name)
{
    if (v == OS_SANDBOX_RLIMIT_KEEP) return ZCL_OK;
    struct rlimit rl = { .rlim_cur = (rlim_t)v, .rlim_max = (rlim_t)v };
    if (setrlimit(resource, &rl) != 0)
        return ZCL_ERR(OS_SANDBOX_ERR_RLIMIT,
                       "setrlimit(%s, %llu) failed errno=%d (%s)",
                       name, (unsigned long long)v, errno, strerror(errno));
    return ZCL_OK;
}

struct zcl_result os_sandbox_set_rlimits(const struct os_sandbox_rlimits *lim)
{
    if (!lim) return ZCL_ERR(OS_SANDBOX_ERR_INVALID_ARG, "lim==NULL");
    ZCL_CHECK(set_one_rlimit(RLIMIT_AS,    lim->as_bytes,    "RLIMIT_AS"));
    ZCL_CHECK(set_one_rlimit(RLIMIT_CPU,   lim->cpu_seconds, "RLIMIT_CPU"));
    ZCL_CHECK(set_one_rlimit(RLIMIT_NPROC, lim->nproc,       "RLIMIT_NPROC"));
    ZCL_CHECK(set_one_rlimit(RLIMIT_FSIZE, lim->fsize_bytes, "RLIMIT_FSIZE"));
    ZCL_CHECK(set_one_rlimit(RLIMIT_NOFILE,lim->nofile,      "RLIMIT_NOFILE"));
    ZCL_CHECK(set_one_rlimit(RLIMIT_CORE,  lim->core_bytes,  "RLIMIT_CORE"));
    return ZCL_OK;
}

/* ── namespaces ────────────────────────────────────────────────────────── */

int os_sandbox_session_ns_flags(void)
{
    return CLONE_NEWUSER | CLONE_NEWNET | CLONE_NEWPID |
           CLONE_NEWNS | CLONE_NEWIPC | CLONE_NEWUTS;
}

static struct zcl_result write_proc_file(pid_t pid, const char *leaf,
                                         const char *content)
{
    char path[64];
    if (pid == 0)
        snprintf(path, sizeof(path), "/proc/self/%s", leaf);
    else
        snprintf(path, sizeof(path), "/proc/%d/%s", (int)pid, leaf);

    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return ZCL_ERR(OS_SANDBOX_ERR_USERNS_MAP,
                       "open(%s) failed errno=%d (%s)", path, errno, strerror(errno));
    size_t len = strlen(content);
    ssize_t w = write(fd, content, len);
    int e = errno;
    close(fd);
    if (w < 0 || (size_t)w != len)
        return ZCL_ERR(OS_SANDBOX_ERR_USERNS_MAP,
                       "write(%s) short/failed w=%zd errno=%d (%s)",
                       path, w, e, strerror(e));
    return ZCL_OK;
}

struct zcl_result os_sandbox_write_userns_maps(pid_t pid, unsigned inside_uid,
                                               unsigned inside_gid)
{
    /* setgroups must be denied before gid_map may be written by an
     * unprivileged process (kernel >= 3.19 requirement). */
    ZCL_CHECK(write_proc_file(pid, "setgroups", "deny"));

    uid_t outer_uid = getuid();
    gid_t outer_gid = getgid();
    char buf[64];

    snprintf(buf, sizeof(buf), "%u %u 1\n", inside_uid, (unsigned)outer_uid);
    ZCL_CHECK(write_proc_file(pid, "uid_map", buf));

    snprintf(buf, sizeof(buf), "%u %u 1\n", inside_gid, (unsigned)outer_gid);
    ZCL_CHECK(write_proc_file(pid, "gid_map", buf));
    return ZCL_OK;
}

/* ── capability probe ──────────────────────────────────────────────────── */

/* Child body for the userns probe: unshare a user namespace, report via exit
 * code (0 = worked, 1 = EPERM/other). Runs in a forked child so the calling
 * process is never mutated. */
static int userns_probe_child(void)
{
    if (unshare(CLONE_NEWUSER) == 0) return 0;
    return 1;
}

struct os_sandbox_caps os_sandbox_probe_caps(void)
{
    struct os_sandbox_caps caps = {
        .userns = false,
        .landlock_abi = os_sandbox_landlock_abi(),
#ifdef ZCL_HAVE_SECCOMP
        .seccomp = true,
#else
        .seccomp = false,
#endif
    };

    pid_t pid = fork();
    if (pid == 0) {
        _exit(userns_probe_child());
    } else if (pid > 0) {
        int status = 0;
        if (waitpid(pid, &status, 0) == pid &&
            WIFEXITED(status) && WEXITSTATUS(status) == 0)
            caps.userns = true;
    } else {
        LOG_WARN("os_sandbox", "fork for userns probe failed errno=%d (%s)",
                 errno, strerror(errno));
    }
    return caps;
}

bool os_sandbox_userns_available(void)
{
    return os_sandbox_probe_caps().userns;
}

/* ── named profiles ────────────────────────────────────────────────────── */

struct os_sandbox_profile os_sandbox_session_child_profile(
    const struct os_sandbox_path_rule *fs_rules, size_t n_fs_rules)
{
    size_t n_denied = 0;
    const int *denied = os_sandbox_session_denied_syscalls(&n_denied);
    return (struct os_sandbox_profile){
        .name = "session_child",
        .no_new_privs = true,
        .apply_rlimits = true,
        .rlimits = os_sandbox_session_rlimits(),
        .landlock = true,
        .fs_rules = fs_rules,
        .n_fs_rules = n_fs_rules,
        .seccomp = true,
        .denied_syscalls = denied,
        .n_denied = n_denied,
        .seccomp_deny_exec_mmap = true,
    };
}

struct os_sandbox_profile os_sandbox_node_steady_state_profile(
    const struct os_sandbox_path_rule *fs_rules, size_t n_fs_rules)
{
    /* The node runs many threads and legitimately opens fds + peer sockets
     * over its lifetime, so NO nproc=1 and NO AS clamp here, and the NODE
     * deny-set (execution/escape only — keeps socket/clone) instead of the
     * session set. Landlock grants the datadir (path-beneath covers the
     * late-opened <datadir>/ssl + SQLite WAL/shm/tmp). Wired at the late
     * SERVICES_RUNNING boundary via a SYSINIT record in config/src/boot.c. */
    size_t n_denied = 0;
    const int *denied = os_sandbox_node_steady_denied_syscalls(&n_denied);
    return (struct os_sandbox_profile){
        .name = "node_steady_state",
        .no_new_privs = true,
        .apply_rlimits = false,
        .rlimits = {0},
        .landlock = true,
        .fs_rules = fs_rules,
        .n_fs_rules = n_fs_rules,
        .seccomp = true,
        .denied_syscalls = denied,
        .n_denied = n_denied,
        .seccomp_deny_exec_mmap = false,  /* dlopen/JIT-free but keep headroom */
    };
}

/* ── enter (the one-correct-order composer) ────────────────────────────── */

struct zcl_result os_sandbox_enter(const struct os_sandbox_profile *p)
{
    if (!p) return ZCL_ERR(OS_SANDBOX_ERR_INVALID_ARG, "profile==NULL");

    /* 1. no_new_privs — unlocks rootless Landlock + seccomp. */
    if (p->no_new_privs && !os_sandbox_no_new_privs())
        return ZCL_ERR(OS_SANDBOX_ERR_NO_NEW_PRIVS,
                       "no_new_privs step failed for profile '%s'",
                       p->name ? p->name : "?");

    /* 2. rlimits — cheap, and cannot be re-raised once seccomp is in. */
    if (p->apply_rlimits)
        ZCL_CHECK(os_sandbox_set_rlimits(&p->rlimits));

    /* 3. Landlock — fds the child needs are already pre-opened by the caller. */
    if (p->landlock)
        ZCL_CHECK(os_sandbox_landlock_restrict(p->fs_rules, p->n_fs_rules));

    /* 4. seccomp LAST — the setup above needs syscalls a deny-list would
     *    otherwise have to special-case (openat for Landlock O_PATH, etc.). */
    if (p->seccomp)
        ZCL_CHECK(os_sandbox_seccomp_deny(p->denied_syscalls, p->n_denied,
                                          p->seccomp_deny_exec_mmap));

    g_sandbox_active = true;
    g_active_profile_name = p->name;  /* profile literals are static-lifetime */
    return ZCL_OK;
}
