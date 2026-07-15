/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the os_sandbox process-confinement builders
 * (lib/platform/src/os_sandbox_linux.c).
 *
 * These builders MUTATE the calling process IRREVERSIBLY (no_new_privs,
 * seccomp, Landlock, lowered rlimits are all one-way). So every destructive
 * assertion runs in a FRESHLY FORKED child and is judged by the child's exit
 * status / terminating signal — mirroring the fork-and-check pattern of the
 * substrate probes (docs/work/session-substrate-probes.md, probes 3 and 4).
 * The parent (the test-group process the harness forked) is never sandboxed.
 *
 * Coverage:
 *   - os_sandbox_no_new_privs() succeeds
 *   - os_sandbox_landlock_abi() reports a supported ABI on this kernel
 *   - os_sandbox_set_rlimits(): FSIZE cap -> SIGXFSZ; NPROC=1 -> 2nd fork EAGAIN
 *   - os_sandbox_seccomp_deny(): normal glibc code runs; execve -> SIGSYS;
 *     socket -> SIGSYS
 *   - os_sandbox_landlock_restrict(): outside open -> EACCES; inside -> ok;
 *     pre-opened fd survives restrict
 *   - os_sandbox_userns_available(): reports the box's true verdict
 *   - os_sandbox_write_userns_maps(): a userns child sees the mapped uid
 *   - os_sandbox_enter(SANDBOX_SESSION_CHILD): child is alive-but-confined
 *     (reads a granted path, cannot open /etc/passwd, cannot exec, cannot
 *     socket)
 *
 * userns-DEPENDENT assertions are gated on os_sandbox_userns_available() so
 * this suite still passes on a host that answers the userns gate differently.
 */

#define _GNU_SOURCE  /* unshare/CLONE_* — must precede every include */

#include "test/test_helpers.h"
#include "platform/os_sandbox.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define SB_CHECK(name, expr) do { \
    printf("os_sandbox: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* Run fn() in a forked child. Returns the child's exit code (>= 0) or the
 * NEGATED terminating signal (< 0) so callers can assert on WTERMSIG. */
static int sb_run_child(int (*fn)(void))
{
    pid_t pid = fork();
    if (pid < 0) return -1000;
    if (pid == 0) _exit(fn());
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1001;
    if (WIFSIGNALED(st)) return -WTERMSIG(st);
    return WEXITSTATUS(st);
}

/* ── individual builder children ───────────────────────────────────────── */

static int c_no_new_privs(void)
{
    return os_sandbox_no_new_privs() ? 0 : 1;
}

static int c_seccomp_exec(void)
{
    size_t n = 0;
    const int *d = os_sandbox_session_denied_syscalls(&n);
    if (!os_sandbox_no_new_privs()) return 70;
    struct zcl_result r = os_sandbox_seccomp_deny(d, n, true);
    if (!r.ok) return 71;
    /* Prove normal glibc work still runs under the filter (default ALLOW). */
    volatile int sum = 0;
    for (int i = 0; i < 100000; i++) sum += i;
    char *p = (char *)malloc(4096);
    if (!p) return 72;
    memset(p, 0x42, 4096);
    free(p);
    /* Now the denied syscall — must SIGSYS-kill the whole process. */
    execve("/bin/true", (char *const[]){"/bin/true", NULL}, (char *const[]){NULL});
    return 5; /* reached only if the filter failed to block execve */
}

static int c_seccomp_socket(void)
{
    size_t n = 0;
    const int *d = os_sandbox_session_denied_syscalls(&n);
    if (!os_sandbox_no_new_privs()) return 70;
    struct zcl_result r = os_sandbox_seccomp_deny(d, n, true);
    if (!r.ok) return 71;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    (void)s;
    return 6; /* reached only if socket() was not blocked */
}

static char g_fsize_path[128];

static int c_rlimit_fsize(void)
{
    struct os_sandbox_rlimits lim = {
        OS_SANDBOX_RLIMIT_KEEP, OS_SANDBOX_RLIMIT_KEEP,
        OS_SANDBOX_RLIMIT_KEEP, 4096 /*FSIZE*/,
        OS_SANDBOX_RLIMIT_KEEP, 0 /*CORE*/,
    };
    if (!os_sandbox_set_rlimits(&lim).ok) return 70;
    int fd = open(g_fsize_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) return 71;
    char buf[65536];
    memset(buf, 'x', sizeof buf);
    for (int i = 0; i < 64; i++) {
        if (write(fd, buf, sizeof buf) < 0) { close(fd); return 9; }
    }
    close(fd);
    return 8; /* reached only if FSIZE was not enforced (expect SIGXFSZ) */
}

static int c_rlimit_nproc(void)
{
    struct os_sandbox_rlimits lim = os_sandbox_session_rlimits();
    /* Isolate NPROC — leave everything else alone so this child can report. */
    lim.as_bytes = OS_SANDBOX_RLIMIT_KEEP;
    lim.fsize_bytes = OS_SANDBOX_RLIMIT_KEEP;
    lim.nofile = OS_SANDBOX_RLIMIT_KEEP;
    lim.core_bytes = OS_SANDBOX_RLIMIT_KEEP;
    if (!os_sandbox_set_rlimits(&lim).ok) return 70;
    pid_t p = fork();
    if (p == 0) _exit(0);
    if (p < 0 && errno == EAGAIN) return 0;   /* the expected NPROC=1 outcome */
    if (p > 0) { int s; waitpid(p, &s, 0); return 20; }
    return 21;
}

static char g_ll_dir[128];

static int c_landlock(void)
{
    char inside[192], preopen_probe[192];
    snprintf(inside, sizeof inside, "%s/in.txt", g_ll_dir);
    snprintf(preopen_probe, sizeof preopen_probe, "%s/pre.txt", g_ll_dir);
    int f = open(inside, O_CREAT | O_RDWR, 0600); if (f >= 0) close(f);

    /* An fd to a file OUTSIDE the grant, opened BEFORE restrict — must remain
     * usable afterwards (probe 4's pre-opened-fd-survives property). */
    int pre = open("/etc/hostname", O_RDONLY | O_CLOEXEC);
    if (pre < 0) return 70;

    struct os_sandbox_path_rule rules[] = {{ g_ll_dir, true, true }};
    if (!os_sandbox_no_new_privs()) return 71;
    struct zcl_result r = os_sandbox_landlock_restrict(rules, 1);
    if (!r.ok) return 72;

    /* inside grant: allowed */
    int in = open(inside, O_RDWR);
    if (in < 0) return 73;
    close(in);
    /* outside grant, fresh open: denied EACCES */
    int out = open("/etc/passwd", O_RDONLY);
    if (out >= 0) { close(out); return 74; }
    if (errno != EACCES) return 75;
    /* pre-opened outside fd: still usable */
    char b[8];
    ssize_t rd = read(pre, b, sizeof b);
    close(pre);
    if (rd < 0) return 76;
    return 0;
}

/* ── enter(SANDBOX_SESSION_CHILD) children ─────────────────────────────── */

static char g_sess_dir[128];

/* Build the session profile but leave AS/NPROC unclamped: this child must
 * survive to report, and it does not fork, so those two caps only add
 * flakiness (the test process already maps > 256 MiB). exec/socket/landlock
 * are what we are actually asserting. */
static struct os_sandbox_profile sess_profile(struct os_sandbox_path_rule *r)
{
    struct os_sandbox_profile p = os_sandbox_session_child_profile(r, 1);
    p.rlimits.as_bytes = OS_SANDBOX_RLIMIT_KEEP;
    p.rlimits.nproc = OS_SANDBOX_RLIMIT_KEEP;
    return p;
}

static int c_enter_alive(void)
{
    char inside[192];
    snprintf(inside, sizeof inside, "%s/f.txt", g_sess_dir);
    int f = open(inside, O_CREAT | O_RDWR, 0600); if (f >= 0) close(f);
    struct os_sandbox_path_rule rules[] = {{ g_sess_dir, true, true }};
    struct os_sandbox_profile p = sess_profile(rules);
    if (!os_sandbox_enter(&p).ok) return 70;
    if (!os_sandbox_active()) return 71;
    int in = open(inside, O_RDWR);
    if (in < 0) return 72;              /* can read/write its granted dir */
    close(in);
    int out = open("/etc/passwd", O_RDONLY);
    if (out >= 0) { close(out); return 73; }  /* cannot escape the grant */
    if (errno != EACCES) return 74;
    return 0;
}

static int c_enter_exec(void)
{
    struct os_sandbox_path_rule rules[] = {{ g_sess_dir, true, true }};
    struct os_sandbox_profile p = sess_profile(rules);
    if (!os_sandbox_enter(&p).ok) return 70;
    execve("/bin/true", (char *const[]){"/bin/true", NULL}, (char *const[]){NULL});
    return 5; /* reached only if exec was not denied */
}

static int c_enter_socket(void)
{
    struct os_sandbox_path_rule rules[] = {{ g_sess_dir, true, true }};
    struct os_sandbox_profile p = sess_profile(rules);
    if (!os_sandbox_enter(&p).ok) return 70;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    (void)s;
    return 6; /* reached only if socket was not denied */
}

/* ── node steady-state profile (the boot-wired resident-node confinement) ─
 *
 * Unlike the session child, the node profile MUST keep socket()/clone() (the
 * node opens peer sockets + spawns threads) while still denying execve and the
 * escape family. These children prove that distinction. The record-registration
 * half of the wiring lives in config/src/boot.c and is pinned by
 * tools/lint/check_sandbox_wired.sh (a full boot in a unit test is impractical;
 * multi-thread seccomp/Landlock retrofit is deferred to soak per
 * BOOT_INVARIANTS.md). */

static char g_node_dir[128];

static int c_node_enter_socket_ok(void)
{
    struct os_sandbox_path_rule rules[] = {{ g_node_dir, true, true }};
    struct os_sandbox_profile p = os_sandbox_node_steady_state_profile(rules, 1);
    if (!os_sandbox_enter(&p).ok) return 70;
    if (!os_sandbox_active()) return 71;
    /* socket() must be ALLOWED under the node deny-set (session denies it). */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 72;
    close(s);
    return 0;
}

static int c_node_enter_fork_ok(void)
{
    struct os_sandbox_path_rule rules[] = {{ g_node_dir, true, true }};
    struct os_sandbox_profile p = os_sandbox_node_steady_state_profile(rules, 1);
    if (!os_sandbox_enter(&p).ok) return 70;
    /* clone/fork must be ALLOWED (the node spawns threads). No nproc clamp. */
    pid_t c = fork();
    if (c == 0) _exit(0);
    if (c < 0) return 73;
    int s; waitpid(c, &s, 0);
    return (WIFEXITED(s) && WEXITSTATUS(s) == 0) ? 0 : 74;
}

static int c_node_enter_exec_denied(void)
{
    struct os_sandbox_path_rule rules[] = {{ g_node_dir, true, true }};
    struct os_sandbox_profile p = os_sandbox_node_steady_state_profile(rules, 1);
    if (!os_sandbox_enter(&p).ok) return 70;
    execve("/bin/true", (char *const[]){"/bin/true", NULL}, (char *const[]){NULL});
    return 5; /* reached only if exec was not denied */
}

static bool denyset_contains(const int *set, size_t n, long nr)
{
    for (size_t i = 0; i < n; i++) if (set[i] == (int)nr) return true;
    return false;
}

/* ── userns map round-trip (userns-gated) ──────────────────────────────── */

static int userns_map_roundtrip(void)
{
    int pr[2], pc[2];
    if (pipe(pr) != 0 || pipe(pc) != 0) return 90;
    pid_t pid = fork();
    if (pid == 0) {
        close(pr[0]); close(pc[1]);
        char b = (unshare(CLONE_NEWUSER) == 0) ? 'U' : 'E';
        ssize_t w = write(pr[1], &b, 1); (void)w;
        if (b == 'E') _exit(91);
        char go;
        ssize_t rd = read(pc[0], &go, 1); (void)rd;  /* wait for parent maps */
        _exit(getuid() == 1000 ? 0 : 92);
    }
    close(pr[1]); close(pc[0]);
    char b = 0;
    ssize_t rd = read(pr[0], &b, 1); (void)rd;
    if (b != 'U') { int s; waitpid(pid, &s, 0); return 93; }
    struct zcl_result r = os_sandbox_write_userns_maps(pid, 1000, 1000);
    char go = 'G';
    ssize_t w = write(pc[1], &go, 1); (void)w;
    int st = 0;
    waitpid(pid, &st, 0);
    if (!r.ok) return 94;
    if (!WIFEXITED(st)) return -WTERMSIG(st);
    return WEXITSTATUS(st);
}

int test_os_sandbox(void)
{
    printf("\n=== platform os_sandbox tests ===\n");
    int failures = 0;

    test_make_tmpdir(g_ll_dir, sizeof g_ll_dir, "os_sandbox", "ll");
    test_make_tmpdir(g_sess_dir, sizeof g_sess_dir, "os_sandbox", "sess");
    test_make_tmpdir(g_node_dir, sizeof g_node_dir, "os_sandbox", "node");
    test_fmt_tmpdir(g_fsize_path, sizeof g_fsize_path, "os_sandbox", "fsize");

    /* ── builders in isolation ─────────────────────────────────────── */
    SB_CHECK("no_new_privs succeeds in child", sb_run_child(c_no_new_privs) == 0);

    int abi = os_sandbox_landlock_abi();
    printf("os_sandbox: landlock ABI = %d\n", abi);
    SB_CHECK("landlock ABI >= 1 (>=5.13 kernel)", abi >= 1);

    /* seccomp: normal code runs, denied syscalls SIGSYS-kill the process */
    SB_CHECK("seccomp deny: execve -> SIGSYS", sb_run_child(c_seccomp_exec) == -SIGSYS);
    SB_CHECK("seccomp deny: socket -> SIGSYS", sb_run_child(c_seccomp_socket) == -SIGSYS);

    /* rlimits: real enforcement */
    SB_CHECK("rlimit FSIZE -> SIGXFSZ on oversize write",
             sb_run_child(c_rlimit_fsize) == -SIGXFSZ);
    SB_CHECK("rlimit NPROC=1 -> 2nd fork EAGAIN",
             sb_run_child(c_rlimit_nproc) == 0);

    /* Landlock: outside denied, inside allowed, pre-opened fd survives */
    SB_CHECK("landlock: deny outside / allow inside / preopened-fd survives",
             sb_run_child(c_landlock) == 0);

    /* ── the composed profile ──────────────────────────────────────── */
    SB_CHECK("enter(SESSION_CHILD): alive, reads grant, /etc/passwd EACCES",
             sb_run_child(c_enter_alive) == 0);
    SB_CHECK("enter(SESSION_CHILD): exec denied -> SIGSYS",
             sb_run_child(c_enter_exec) == -SIGSYS);
    SB_CHECK("enter(SESSION_CHILD): socket denied -> SIGSYS",
             sb_run_child(c_enter_socket) == -SIGSYS);

    /* ── node steady-state profile: construction + child probe ─────── */
    {
        struct os_sandbox_path_rule r = { g_node_dir, true, true };
        struct os_sandbox_profile np = os_sandbox_node_steady_state_profile(&r, 1);
        SB_CHECK("node profile is named node_steady_state",
                 np.name && strcmp(np.name, "node_steady_state") == 0);
        SB_CHECK("node profile: no rlimit clamp (many threads)",
                 !np.apply_rlimits);
        SB_CHECK("node profile: seccomp + landlock + no_new_privs on",
                 np.seccomp && np.landlock && np.no_new_privs);
        size_t nn = 0;
        const int *nd = os_sandbox_node_steady_denied_syscalls(&nn);
        SB_CHECK("node profile denied set == node deny-set",
                 np.denied_syscalls == nd && np.n_denied == nn);
        SB_CHECK("node deny-set includes execve", denyset_contains(nd, nn, __NR_execve));
        SB_CHECK("node deny-set includes ptrace", denyset_contains(nd, nn, __NR_ptrace));
        SB_CHECK("node deny-set EXCLUDES socket (node needs it)",
                 !denyset_contains(nd, nn, __NR_socket));
        SB_CHECK("node deny-set EXCLUDES clone (node spawns threads)",
                 !denyset_contains(nd, nn, __NR_clone));
    }
    SB_CHECK("enter(NODE): socket allowed (unlike session)",
             sb_run_child(c_node_enter_socket_ok) == 0);
    SB_CHECK("enter(NODE): fork/clone allowed",
             sb_run_child(c_node_enter_fork_ok) == 0);
    SB_CHECK("enter(NODE): exec denied -> SIGSYS",
             sb_run_child(c_node_enter_exec_denied) == -SIGSYS);

    /* ── capability probe + userns-gated assertions ────────────────── */
    struct os_sandbox_caps caps = os_sandbox_probe_caps();
    printf("os_sandbox: caps userns=%d landlock_abi=%d seccomp=%d\n",
           caps.userns, caps.landlock_abi, caps.seccomp);
    SB_CHECK("caps: seccomp reported available", caps.seccomp);
    SB_CHECK("caps: landlock_abi matches direct probe", caps.landlock_abi == abi);

    bool userns = os_sandbox_userns_available();
    if (userns) {
        /* On the target box (rootless, AppArmor-unconfined) this is the path. */
        printf("os_sandbox: userns available -> running userns-gated assertions\n");
        SB_CHECK("caps.userns agrees with userns_available", caps.userns);
        SB_CHECK("userns map round-trip: child sees mapped uid 1000",
                 userns_map_roundtrip() == 0);
        int flags = os_sandbox_session_ns_flags();
        SB_CHECK("session ns flags include NEWUSER|NEWNET|NEWPID",
                 (flags & CLONE_NEWUSER) && (flags & CLONE_NEWNET) &&
                 (flags & CLONE_NEWPID));
    } else {
        /* Degraded host: skip userns assertions so the suite still passes. */
        printf("os_sandbox: SKIP (userns unavailable on host) — "
               "degraded profile still covered by the builder tests above\n");
    }

    test_rm_rf_recursive(g_ll_dir);
    test_rm_rf_recursive(g_sess_dir);
    test_rm_rf_recursive(g_node_dir);
    unlink(g_fsize_path);

    printf("=== os_sandbox tests done: %d failure(s) ===\n", failures);
    return failures;
}
