/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * zclassic23-package-verify — the EXTERNAL ZCODE package verifier (slice 6).
 * This is a SEPARATE program from the node: the ZClassic23 node itself NEVER * compiles or executes downloaded code. Given a package root, this program
 * loads the signed release envelope, the manifest, the CAS chunks, and the
 * slice-5 declarative recipe from an EXPLICIT package store directory, then
 * builds and tests the package under confinement and writes ONE signed
 * attestation (lib/vcs/package_attest.*) into the store's attestations/
 * directory. Quorum (>=2 approved independent keys) is evaluated elsewhere
 * (the node's `zcode package verify` command); this program only ever
 * produces one attestation signed by one verifier key.
 *
 *   zclassic23-package-verify <package-root-hex> --store=<dir> --key=<file>
 *                             [--work=<dir>] [--require-full-isolation]
 *
 * Isolation contract (per spawned child — every compile, link, and test
 * run):
 *   - no network: a seccomp deny-list kills the whole socket family
 *     (socket/connect/bind/listen/accept, send, recv, socketpair and
 *     their variants), plus ptrace/process_vm_*, mount/namespace escape,
 *     kernel modules, keyrings, bpf, perf, and open_by_handle_at. execve
 *     and fork/clone stay allowed:
 *     gcc is a driver that must exec cc1/as/ld, and pthread tests need
 *     clone — every exec'd image inherits no_new_privs + seccomp +
 *     Landlock + rlimits, so this is not an escape.
 *   - filesystem scoping: a Landlock domain grants ONLY the materialized
 *     source tree (read), the temp build dir (read/write/create/execute),
 *     the host toolchain (/usr, /lib, /lib64, /etc read+execute),
 *     /dev/null + /dev/urandom, and the child's OWN /proc/self (read —
 *     compiler-rt re-reads /proc/self/environ for its runtime flags; deny
 *     it and ASan falls back to DEFAULT flags, resurrecting LeakSanitizer,
 *     which then dies stop-the-world). Wallet paths, node datadirs, SSH
 *     keys, and credentials are simply never granted — denial is the
 *     default.
 *   - environment hygiene: every child runs with a SCRUBBED environment
 *     (clearenv + PATH/LC_ALL + the explicit TMPDIR/ASAN_OPTIONS/
 *     UBSAN_OPTIONS pairs) — an operator shell carries credentials
 *     (API keys, tokens) that untrusted package code must never see.
 *     LeakSanitizer is deliberately OFF (detect_leaks=0): its
 *     stop-the-world needs ptrace + cross-process /proc access, both
 *     denied by this confinement; ASan memory-error and UBSan checks are
 *     unaffected.
 *   - resource limits: RLIMIT_AS = the recipe's maximum_memory_bytes,
 *     RLIMIT_CPU = the recipe's maximum_test_seconds, plus
 *     NPROC/FSIZE/NOFILE/CORE caps (NPROC as a margin over the uid's
 *     current task count — RLIMIT_NPROC is session-wide per uid), and a
 *     parent-enforced wall-clock deadline (SIGKILL) on every child. Sanitizer (ASan/UBSan) runs are
 *     the ONE exception: ASan's multi-terabyte shadow address space makes
 *     RLIMIT_AS meaningless, so the AS limit is lifted for those runs only
 *     (CPU/wall/proc/file limits still bind; the plain run is the
 *     resource-bound one — documented in the attestation's detail text
 *     when it matters).
 *   - the recipe is the ONLY build input: package Makefiles, configure
 *     scripts, and every other downloaded file are never executed. Only
 *     recipe.sources / recipe.test_sources are compiled, with exactly the
 *     recipe's include dirs, defines, and allowed libraries (v1:
 *     libc/libm/pthread).
 *   - the materialized source tree is read-only (files 0444, dirs 0555);
 *     builds write only to the separate build dir; every produced binary
 *     and object is DELETED with the temp tree after the attestation is
 *     written (success or failure).
 *
 * Degraded mode: where the kernel offers no Landlock, children still run
 * under no_new_privs + seccomp + rlimits (network and resources stay
 * bound) but the filesystem is NOT scoped — the attestation then carries
 * isolation=degraded and this program prints a loud warning. Operators who
 * refuse degraded attestations pass --require-full-isolation, which fails
 * closed (no attestation is written). The quorum policy and the `zcode
 * package verify` report surface the isolation level of every attestation.
 *
 * Key file: 64 lowercase/uppercase hex chars (one secp256k1 secret), in a
 * regular file with no group/other permission bits. The key NEVER leaves
 * this process; signing uses the libsecp256k1 RFC6979 nonce function and
 * the compact low-S form the attestation codec enforces.
 *
 * Exit codes: 0 attestation written (whatever its verdict — a build-fail
 * attestation is still a successful verification run); 2 usage; 3 the
 * store/release/manifest/recipe/chunks could not be loaded or the package
 * is incomplete; 4 --require-full-isolation on a no-Landlock kernel; 5 an
 * internal/sandbox failure (nothing is signed). */

/* clearenv(3) for the child environment scrub (glibc; the project builds
 * with strict -D_POSIX_C_SOURCE=200809L, so opt in like lib/test does). */
#define _DEFAULT_SOURCE

#include "vcs/package_attest.h"
#include "vcs/package_build.h"
#include "vcs/package_manifest.h"
#include "vcs/package_recipe.h"
#include "vcs/package_release.h"

#include "base/hex.h"
#include "crypto/sha3.h"
#include "platform/clock.h"
#include "platform/os_proc.h"
#include "platform/os_sandbox.h"
#include "support/cleanse.h"
#include "util/safe_alloc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <secp256k1.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PV_LOG "package-verify"

/* The node-wide shutdown flag lives in src/main.c, which is not linked
 * into this standalone binary; the app TUs in ALL_SRCS reference it
 * extern, so every tool main defines it (tools/bot.c precedent). */
volatile sig_atomic_t g_shutdown_requested = 0;

/* Child wall-clock budgets. The test-run budget comes from the recipe;
 * compiles and links get fixed caps (the recipe bounds tests, not the
 * compiler's own speed). */
#define PV_COMPILE_TIMEOUT_MS 120000
#define PV_LINK_TIMEOUT_MS 120000
/* Fixed compile-time resource caps (the recipe bounds the TEST run). */
#define PV_COMPILE_AS_BYTES (UINT64_C(4) * 1024u * 1024u * 1024u)
#define PV_COMPILE_FSIZE_BYTES (UINT64_C(256) * 1024u * 1024u)
/* RLIMIT_NPROC is per-REAL-UID and counts every task the uid already runs
 * session-wide, so an absolute value breaks on any busy operator host
 * (fork → EAGAIN). These constants are MARGINS: the child limit is set to
 * (uid's current task count) + margin, which bounds what the child tree can
 * add without depending on the host's background load. */
#define PV_COMPILE_NPROC 256u
#define PV_COMPILE_NOFILE 1024u
/* Test-run fixed caps beyond the recipe's AS/CPU. */
#define PV_TEST_FSIZE_BYTES (UINT64_C(64) * 1024u * 1024u)
#define PV_TEST_NPROC 64u
#define PV_TEST_NOFILE 64u
/* Capture bounds. */
#define PV_STDOUT_CAP 2048u
#define PV_STDERR_CAP 4096u
/* ASan/UBSan marker exit codes (set via *_OPTIONS in the child env). */
#define PV_ASAN_EXIT 99
#define PV_UBSAN_EXIT 98
/* Child internal failures (never a verdict). */
#define PV_CHILD_SANDBOX_FAIL 125
#define PV_CHILD_SECCOMP_FAIL 126
#define PV_CHILD_EXEC_FAIL 127

/* Emit-mode bounds. The archive/header install carries the same
 * canonical-path grammar as the manifest, so an emitted path can never
 * become a filesystem escape. */
#define PV_EMIT_MAX_DEPS 64u

/* One locked dependency the target builds against: its package root (which
 * the build receipt commits) and its already-installed directory, whose
 * include/ is granted READ-ONLY to the compile children and whose static
 * archives under lib/ are appended to the link line. */
struct pv_emit_dep {
    uint8_t root[32];
    char install_dir[2048];
    char include_dir[2100];
};

static void pv_usage(FILE *out)
{
    fprintf(out,
        "usage: zclassic23-package-verify <package-root-hex> --store=<dir>\n"
        "           --key=<file> [--work=<dir>] [--require-full-isolation]\n"
        "   or: zclassic23-package-verify <package-root-hex> --store=<dir>\n"
        "           --emit=<dir> --lock-root=<64hex> [--dep=<64hex>,<dir>]...\n"
        "           [--work=<dir>] [--require-full-isolation]\n"
        "\n"
        "--emit is INSTALL-BUILD mode (the ZCODE add lifecycle): the same\n"
        "confined build+test runs, but instead of signing an attestation the\n"
        "run archives the recipe's non-test objects into <dir>/lib/lib<pkg>.a,\n"
        "copies the recipe's public headers under <dir>/include/, and writes\n"
        "the canonical build receipt (vcs/package_build.h) to\n"
        "<dir>/build-report. NOTHING is signed in this mode and --key is\n"
        "refused: the caller is the node's own lifecycle service, which\n"
        "independently RE-HASHES every emitted file against the receipt\n"
        "before installing it. --lock-root pins the dependency lock the\n"
        "receipt commits; each --dep names a locked dependency root and its\n"
        "install dir, whose include/ is granted read-only to the compile\n"
        "children and whose lib/*.a archives join the link line.\n"
        "\n"
        "Builds and tests one ZCODE package under confinement (seccomp +\n"
        "rlimits + Landlock where the kernel offers it) following ONLY the\n"
        "slice-5 declarative recipe, then writes one secp256k1-signed\n"
        "attestation into <store>/attestations/<attestation-id-hex>.\n"
        "The node never compiles downloaded code; this separate program is\n"
        "the only place compilation happens. Produced binaries are deleted\n"
        "after the attestation is written. --key names a 0600/0400 file\n"
        "holding one 64-hex secp256k1 secret. --work chooses the parent of\n"
        "the temp build tree (default: $TMPDIR or /tmp). On a kernel\n"
        "without Landlock the run is DEGRADED (no filesystem scoping; the\n"
        "attestation says isolation=degraded) unless\n"
        "--require-full-isolation is given, which fails closed.\n");
}

/* ── small utilities ────────────────────────────────────────────────── */

static bool pv_mkdir_p(const char *path, mode_t mode)
{
    char buf[4096];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buf))
        return false;
    memcpy(buf, path, len + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buf, mode) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    return mkdir(buf, mode) == 0 || errno == EEXIST;
}

static bool pv_rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0;
    DIR *dir = opendir(path);
    if (!dir)
        return false;
    bool ok = true;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char child[4096];
        int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (n <= 0 || (size_t)n >= sizeof(child)) {
            ok = false;
            continue;
        }
        if (!pv_rm_rf(child))
            ok = false;
    }
    closedir(dir);
    if (rmdir(path) != 0)
        ok = false;
    return ok;
}

/* Read a whole file bounded by cap (NULL on any failure/oversize). */
static uint8_t *pv_read_file(const char *path, size_t cap, size_t *out_len)
{
    *out_len = 0;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size > cap)
        return NULL;
    size_t len = (size_t)st.st_size;
    uint8_t *buf = zcl_malloc(len, "pv_read_file");
    if (!buf)
        return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) {
        free(buf);
        return NULL;
    }
    if (fread(buf, 1, len, f) != len) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *out_len = len;
    return buf;
}

/* Durable write beside the destination: tmp, fsync, atomic rename (the
 * store's own discipline — a torn attestation is never a valid object). */
static bool pv_atomic_write(const char *path, const uint8_t *data,
                            size_t data_len)
{
    char tmp[4096];
    int tn = snprintf(tmp, sizeof(tmp), "%s.zvtmp.%ld", path,
                      (long)getpid());
    if (tn <= 0 || (size_t)tn >= sizeof(tmp))
        return false;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    size_t off = 0;
    while (off < data_len) {
        ssize_t w = write(fd, data + off, data_len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            unlink(tmp);
            return false;
        }
        off += (size_t)w;
    }
    if (fsync(fd) != 0 || close(fd) != 0 || rename(tmp, path) != 0) {
        close(fd);
        unlink(tmp);
        return false;
    }
    return true;
}

/* ── the sandboxed child runner ─────────────────────────────────────── */

/* The verifier child deny-set: the whole socket family (network denial),
 * cross-process memory, mount/namespace escape, kernel modules, keyrings,
 * bpf, perf, and handle-based opens. execve/fork/clone stay ALLOWED —
 * gcc must exec cc1/as/ld and pthread tests need clone; every exec'd
 * image inherits the full confinement. Guarded __NR_* like the
 * os_sandbox session set. */
static const int g_pv_child_denied[] = {
    __NR_socket,
#ifdef __NR_socketcall
    __NR_socketcall,
#endif
    __NR_socketpair, __NR_connect, __NR_bind, __NR_listen,
    __NR_accept, __NR_accept4,
    __NR_sendto, __NR_recvfrom, __NR_sendmsg, __NR_recvmsg,
    __NR_shutdown, __NR_setsockopt, __NR_getsockopt,
    __NR_getsockname, __NR_getpeername,
    __NR_ptrace, __NR_process_vm_readv, __NR_process_vm_writev,
    __NR_mount, __NR_umount2, __NR_pivot_root, __NR_setns, __NR_unshare,
    __NR_bpf, __NR_kexec_load, __NR_kexec_file_load,
    __NR_init_module, __NR_finit_module, __NR_delete_module,
    __NR_perf_event_open,
    __NR_add_key, __NR_request_key, __NR_keyctl,
    __NR_open_by_handle_at,
};

struct pv_run {
    bool launched;     /* fork/pipe machinery worked */
    bool exited;       /* child exited (vs signaled/timed out) */
    int exit_code;
    int term_signal;
    bool timed_out;
    bool sandbox_fail; /* the child could not arm its own confinement */
    char stdout_buf[PV_STDOUT_CAP];
    char stderr_buf[PV_STDERR_CAP];
};

/* Landlock grant set for every verifier child: the materialized source
 * (read), the build dir (full), the host toolchain (read+execute), and
 * the two harmless /dev nodes. Anything else — wallet, datadir, SSH — is
 * denied by default. */
static size_t pv_child_grants(const char *src_dir, const char *build_dir,
                              const struct pv_emit_dep *deps, size_t dep_count,
                              struct os_sandbox_path_rule *rules,
                              size_t cap)
{
    size_t n = 0;
    if (cap < 10u + dep_count)
        return 0;
    rules[n++] = (struct os_sandbox_path_rule){
        .path = src_dir, .allow_read = true };
    rules[n++] = (struct os_sandbox_path_rule){
        .path = build_dir, .allow_read = true, .allow_write = true,
        .allow_execute = true, .allow_create = true };
    rules[n++] = (struct os_sandbox_path_rule){
        .path = "/usr", .allow_read = true, .allow_execute = true };
    rules[n++] = (struct os_sandbox_path_rule){
        .path = "/lib", .allow_read = true, .allow_execute = true };
    struct stat st;
    if (stat("/lib64", &st) == 0)
        rules[n++] = (struct os_sandbox_path_rule){
            .path = "/lib64", .allow_read = true, .allow_execute = true };
    rules[n++] = (struct os_sandbox_path_rule){
        .path = "/etc", .allow_read = true };
    /* The child's OWN /proc/self only: compiler-rt (ASan/UBSan) re-reads
     * /proc/self/environ for its runtime flags and /proc/self/maps for the
     * shadow layout; denying it makes the runtimes fall back to DEFAULT
     * flags (LeakSanitizer on), which then dies stop-the-world. The child
     * environment is scrubbed (see pv_run_child), so this exposes nothing
     * beyond the child's own process. */
    rules[n++] = (struct os_sandbox_path_rule){
        .path = OS_SANDBOX_PROC_SELF_PATH, .allow_read = true };
    rules[n++] = (struct os_sandbox_path_rule){
        .path = "/dev/null", .allow_read = true, .allow_write = true };
    rules[n++] = (struct os_sandbox_path_rule){
        .path = "/dev/urandom", .allow_read = true };
    /* Locked dependencies: READ ONLY, and only the install dirs the caller
     * named (headers to compile against, archives to link). Never write,
     * never execute — a dependency is data to this build, not a program. */
    for (size_t i = 0; i < dep_count; i++)
        rules[n++] = (struct os_sandbox_path_rule){
            .path = deps[i].install_dir, .allow_read = true };
    return n;
}

/* This process's own current mapped size (RLIMIT_AS's accounting unit).
 * fork() gives the child the same mapping, so a limit set to the recipe's
 * raw byte budget alone starves the not-yet-exec'd child of the address
 * space this verifier binary's own text/data/bss already occupy — the
 * child dies with SIGSEGV growing its stack during rlimit/Landlock/seccomp
 * setup, before it ever reaches execve(). Best-effort: an unreadable
 * VmSize yields 0, same fallback as pv_uid_task_count below. */
static uint64_t pv_process_vsize_bytes(void)
{
    struct os_proc_mem mem;
    if (!os_proc_mem_read(&mem) || mem.vsize_bytes < 0)
        return 0;
    return (uint64_t)mem.vsize_bytes;
}

/* Current task count of the real uid (RLIMIT_NPROC's accounting unit).
 * Best-effort: a /proc scan; unreadable entries are skipped, so the result
 * can be an undercount — the margin in PV_*_NPROC absorbs that. */
static uint64_t pv_uid_task_count(void)
{
    DIR *proc = opendir("/proc");
    if (!proc)
        return 0;
    const uid_t me = getuid();
    uint64_t total = 0;
    struct dirent *ent;
    while ((ent = readdir(proc)) != NULL) {
        char *end = NULL;
        long pid = strtol(ent->d_name, &end, 10);
        if (!end || *end != '\0' || pid <= 0)
            continue;
        char tpath[64];
        snprintf(tpath, sizeof(tpath), "/proc/%ld/task", pid);
        struct stat st;
        if (stat(tpath, &st) != 0 || st.st_uid != me)
            continue;
        DIR *tasks = opendir(tpath);
        if (!tasks)
            continue;
        struct dirent *te;
        while ((te = readdir(tasks)) != NULL)
            if (te->d_name[0] >= '0' && te->d_name[0] <= '9')
                total++;
        closedir(tasks);
    }
    closedir(proc);
    return total;
}

/* Fork and run argv[0] confined. The child: stderr/stdout captured,
 * optional chdir, rlimits, no_new_privs, Landlock (when landlock=true),
 * the seccomp deny-set, then execvp. The parent enforces the wall-clock
 * deadline with SIGKILL. env_pairs is a NULL-terminated flat array of
 * "NAME=value" strings applied in the child (or NULL). */
static struct pv_run pv_run_child(const char *const argv[],
                                  const char *cwd,
                                  const struct os_sandbox_rlimits *limits,
                                  bool landlock,
                                  const struct os_sandbox_path_rule *rules,
                                  size_t n_rules,
                                  const char *const env_pairs[],
                                  int timeout_ms)
{
    struct pv_run r;
    memset(&r, 0, sizeof(r));
    /* Rebase absolute caps into "current usage + the caller's budget": NPROC
     * is session-wide per uid (see the PV_COMPILE_NPROC comment), and AS is
     * this very process's own mapping (see pv_process_vsize_bytes) — a fork
     * child starts from that same mapping, so the recipe's byte budget must
     * land on top of it, not replace it. */
    struct os_sandbox_rlimits rebased;
    if (limits && (limits->nproc != OS_SANDBOX_RLIMIT_KEEP ||
                   limits->as_bytes != OS_SANDBOX_RLIMIT_KEEP)) {
        rebased = *limits;
        if (limits->nproc != OS_SANDBOX_RLIMIT_KEEP)
            rebased.nproc = pv_uid_task_count() + limits->nproc;
        if (limits->as_bytes != OS_SANDBOX_RLIMIT_KEEP)
            rebased.as_bytes = pv_process_vsize_bytes() + limits->as_bytes;
        limits = &rebased;
    }
    int out_pipe[2] = { -1, -1 };
    int err_pipe[2] = { -1, -1 };
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0)
        return r;
    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        return r;
    }
    if (pid == 0) {
        /* Child: single-threaded standalone CLI, so the pre-exec calls
         * below are safe here (no other thread holds a lock). */
        close(out_pipe[0]);
        close(err_pipe[0]);
        (void)dup2(out_pipe[1], STDOUT_FILENO);
        (void)dup2(err_pipe[1], STDERR_FILENO);
        if (cwd && chdir(cwd) != 0)
            _exit(PV_CHILD_EXEC_FAIL);
        /* Scrub the inherited environment: the operator's shell env can
         * carry credentials (API keys, tokens) that untrusted package code
         * must never see. The child gets a minimal base plus the caller's
         * explicit pairs only. clearenv(3) keeps glibc's internal environ
         * state consistent — a raw `environ = ...` reassignment segfaults
         * execvp in this whole-program LTO build. */
        if (clearenv() != 0)
            _exit(PV_CHILD_EXEC_FAIL);
        (void)setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);
        (void)setenv("LC_ALL", "C", 1);
        if (env_pairs)
            for (size_t i = 0; env_pairs[i]; i++) {
                const char *eq = strchr(env_pairs[i], '=');
                if (!eq)
                    continue;
                char name[64];
                size_t nl = (size_t)(eq - env_pairs[i]);
                if (nl == 0 || nl >= sizeof(name))
                    continue;
                memcpy(name, env_pairs[i], nl);
                name[nl] = '\0';
                (void)setenv(name, eq + 1, 1);
            }
        /* The parent forwards this pipe (see pr.stderr_buf) into the
         * "failed to launch or arm its sandbox" messages below — without
         * it, a rlimit/Landlock arm failure is a bare exit code with no way
         * to tell a resource-budget miss from a bad grant path. */
        if (limits) {
            struct zcl_result rr = os_sandbox_set_rlimits(limits);
            if (!zcl_result_is_ok(rr)) {
                fprintf(stderr, "rlimits: %s\n", rr.message);
                _exit(PV_CHILD_SANDBOX_FAIL);
            }
        }
        if (!os_sandbox_no_new_privs())
            _exit(PV_CHILD_SANDBOX_FAIL);
        if (landlock) {
            struct zcl_result lr =
                os_sandbox_landlock_restrict(rules, n_rules);
            if (!zcl_result_is_ok(lr)) {
                fprintf(stderr, "landlock: %s\n", lr.message);
                _exit(PV_CHILD_SANDBOX_FAIL);
            }
        }
        struct zcl_result sr = os_sandbox_seccomp_deny(
            g_pv_child_denied,
            sizeof(g_pv_child_denied) / sizeof(g_pv_child_denied[0]),
            false);
        if (!zcl_result_is_ok(sr))
            _exit(PV_CHILD_SECCOMP_FAIL);
        execvp(argv[0], (char *const *)argv);
        _exit(PV_CHILD_EXEC_FAIL);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    /* Nonblocking read ends: the poll loop drains without hanging. */
    (void)fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);
    r.launched = true;

    int64_t deadline =
        clock_now_monotonic_ns() + (int64_t)timeout_ms * INT64_C(1000000);
    size_t out_len = 0, err_len = 0;
    int status = 0;
    bool reaped = false;
    for (;;) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            reaped = true;
            break;
        }
        /* Drain whatever the child has written so far (bounded). */
        uint8_t chunk[512];
        ssize_t got;
        if (out_len + 1 < sizeof(r.stdout_buf)) {
            got = read(out_pipe[0], chunk,
                       sizeof(chunk) < sizeof(r.stdout_buf) - 1 - out_len
                           ? sizeof(chunk)
                           : sizeof(r.stdout_buf) - 1 - out_len);
            if (got > 0) {
                memcpy(r.stdout_buf + out_len, chunk, (size_t)got);
                out_len += (size_t)got;
            }
        }
        if (err_len + 1 < sizeof(r.stderr_buf)) {
            got = read(err_pipe[0], chunk,
                       sizeof(chunk) < sizeof(r.stderr_buf) - 1 - err_len
                           ? sizeof(chunk)
                           : sizeof(r.stderr_buf) - 1 - err_len);
            if (got > 0) {
                memcpy(r.stderr_buf + err_len, chunk, (size_t)got);
                err_len += (size_t)got;
            }
        }
        if (clock_now_monotonic_ns() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            reaped = true;
            r.timed_out = true;
            break;
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
        nanosleep(&ts, NULL);
    }
    /* Final drain after the child is gone (pipe holds the last bytes). */
    for (;;) {
        uint8_t chunk[512];
        ssize_t got = read(out_pipe[0], chunk, sizeof(chunk));
        if (got <= 0)
            break;
        if (out_len + 1 < sizeof(r.stdout_buf)) {
            size_t room = sizeof(r.stdout_buf) - 1 - out_len;
            size_t take = (size_t)got < room ? (size_t)got : room;
            memcpy(r.stdout_buf + out_len, chunk, take);
            out_len += take;
        }
    }
    for (;;) {
        uint8_t chunk[512];
        ssize_t got = read(err_pipe[0], chunk, sizeof(chunk));
        if (got <= 0)
            break;
        if (err_len + 1 < sizeof(r.stderr_buf)) {
            size_t room = sizeof(r.stderr_buf) - 1 - err_len;
            size_t take = (size_t)got < room ? (size_t)got : room;
            memcpy(r.stderr_buf + err_len, chunk, take);
            err_len += take;
        }
    }
    close(out_pipe[0]);
    close(err_pipe[0]);
    r.stdout_buf[out_len] = '\0';
    r.stderr_buf[err_len] = '\0';
    (void)reaped;
    if (!r.timed_out) {
        if (WIFEXITED(status)) {
            r.exited = true;
            r.exit_code = WEXITSTATUS(status);
            if (r.exit_code == PV_CHILD_SANDBOX_FAIL ||
                r.exit_code == PV_CHILD_SECCOMP_FAIL)
                r.sandbox_fail = true;
        } else if (WIFSIGNALED(status)) {
            r.term_signal = WTERMSIG(status);
        }
    }
    return r;
}

/* ── store loading ──────────────────────────────────────────────────── */

/* Find the verified release envelope naming package_root: the lowest
 * release id wins when several match (deterministic). False when none. */
static bool pv_load_release(const char *store_dir,
                            const uint8_t package_root[32],
                            struct vcs_package_release *out,
                            uint8_t release_id_out[32])
{
    char dir[4096];
    int n = snprintf(dir, sizeof(dir), "%s/releases", store_dir);
    if (n < 0 || (size_t)n >= sizeof(dir))
        return false;
    DIR *d = opendir(dir);
    if (!d)
        return false;
    bool found = false;
    uint8_t best_id[32];
    memset(best_id, 0xff, 32);
    struct dirent *ent;
    size_t scanned = 0;
    while ((ent = readdir(d)) != NULL && scanned < 4096) {
        uint8_t scratch[32];
        if (!zcl_hex_decode_lower(ent->d_name, scratch, 32))
            continue;
        scanned++;
        char path[4096];
        int pn = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (pn < 0 || (size_t)pn >= sizeof(path))
            continue;
        size_t wire_len = 0;
        uint8_t *wire =
            pv_read_file(path, VCS_PACKAGE_RELEASE_MAX_WIRE_BYTES,
                         &wire_len);
        if (!wire)
            continue;
        struct vcs_package_release rel;
        enum vcs_package_release_error perr =
            vcs_package_release_parse(wire, wire_len, &rel);
        free(wire);
        if (perr != VCS_PACKAGE_RELEASE_OK)
            continue;
        if (memcmp(rel.package_root, package_root, 32) != 0)
            continue;
        if (vcs_package_release_verify(&rel) != VCS_PACKAGE_RELEASE_OK)
            continue;
        uint8_t id[VCS_PACKAGE_RELEASE_ID_BYTES];
        if (vcs_package_release_id(&rel, id) != VCS_PACKAGE_RELEASE_OK)
            continue;
        if (!found || memcmp(id, best_id, 32) < 0) {
            memcpy(best_id, id, 32);
            *out = rel;
            memcpy(release_id_out, id, 32);
            found = true;
        }
    }
    closedir(d);
    return found;
}

/* ── compiler/test invocation plumbing ──────────────────────────────── */

struct pv_compiler {
    const char *id; /* "gcc" / "clang" (attestation tokens) */
    char version[VCS_PACKAGE_ATTEST_COMPILER_VERSION_MAX + 1u];
    bool available;
    uint8_t outcome; /* enum vcs_package_attest_outcome (build verdict) */
};

/* Sanitize one bounded printable detail line from captured stderr: the
 * first line containing ": error:" when present, else the first non-empty
 * line; non-printables become '?'. */
static void pv_detail_from_stderr(const char *prefix, const char *stderr_buf,
                                  char *out, size_t out_cap)
{
    char line[200];
    line[0] = '\0';
    const char *p = stderr_buf;
    char first[200];
    first[0] = '\0';
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof(line))
            len = sizeof(line) - 1;
        char cur[200];
        memcpy(cur, p, len);
        cur[len] = '\0';
        if (cur[0] && !first[0])
            snprintf(first, sizeof(first), "%s", cur);
        if (strstr(cur, ": error:") != NULL) {
            snprintf(line, sizeof(line), "%s", cur);
            break;
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    if (!line[0])
        snprintf(line, sizeof(line), "%s", first[0] ? first : "no diagnostics captured");
    size_t o = snprintf(out, out_cap, "%s: ", prefix);
    for (size_t i = 0; line[i] && o + 1 < out_cap && o < 150; i++) {
        unsigned char c = (unsigned char)line[i];
        out[o++] = (c >= 0x20 && c <= 0x7e) ? (char)c : '?';
    }
    out[o] = '\0';
}

/* Extract the sanitizer's own report line (ASan "SUMMARY:" / UBSan
 * "runtime error:") from captured stderr into the attestation detail, so
 * a findings attestation carries WHAT was found, not just the exit code. */
static void pv_san_detail_from_stderr(const char *prefix,
                                      const char *stderr_buf,
                                      char *out, size_t out_cap)
{
    char line[200];
    line[0] = '\0';
    char first[200];
    first[0] = '\0';
    const char *p = stderr_buf;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof(line))
            len = sizeof(line) - 1;
        char cur[200];
        memcpy(cur, p, len);
        cur[len] = '\0';
        if (cur[0] && !first[0])
            snprintf(first, sizeof(first), "%s", cur);
        if (strstr(cur, "SUMMARY:") != NULL ||
            strstr(cur, "runtime error:") != NULL) {
            snprintf(line, sizeof(line), "%s", cur);
            break;
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    if (!line[0])
        snprintf(line, sizeof(line), "%s",
                 first[0] ? first : "sanitizer findings (report truncated)");
    size_t o = snprintf(out, out_cap, "%s: ", prefix);
    for (size_t i = 0; line[i] && o + 1 < out_cap && o < 150; i++) {
        unsigned char c = (unsigned char)line[i];
        out[o++] = (c >= 0x20 && c <= 0x7e) ? (char)c : '?';
    }
    out[o] = '\0';
}

/* Build the argv for one compile: cc -std=c23 -O1 [san] -I... -D...
 * -c <srcfile> -o <obj>. argv storage must outlive the call. */
struct pv_compile_args {
    const char *argv[192];
    char inc[8][4200];   /* -I args */
    char dep[PV_EMIT_MAX_DEPS][2100]; /* -I args for locked dependencies */
    char def[64][80];    /* -D args */
    char san[1][64];
};

static size_t pv_compile_argv(struct pv_compile_args *store,
                              const char *cc, bool sanitize,
                              const struct vcs_package_recipe *recipe,
                              const char *src_root,
                              const struct pv_emit_dep *deps, size_t dep_count,
                              const char *src_file, const char *obj_file)
{
    size_t n = 0;
    store->argv[n++] = cc;
    store->argv[n++] = "-std=c23";
    store->argv[n++] = "-O1";
    store->argv[n++] = "-fno-omit-frame-pointer";
    if (sanitize) {
        snprintf(store->san[0], sizeof(store->san[0]),
                 "-fsanitize=address,undefined");
        store->argv[n++] = store->san[0];
    }
    for (size_t i = 0; i < recipe->include_dirs.count &&
                        i < sizeof(store->inc) / sizeof(store->inc[0]); i++) {
        snprintf(store->inc[i], sizeof(store->inc[i]), "-I%s/%s", src_root,
                 recipe->include_dirs.items[i]);
        store->argv[n++] = store->inc[i];
    }
    for (size_t i = 0; i < dep_count && i < PV_EMIT_MAX_DEPS; i++) {
        snprintf(store->dep[i], sizeof(store->dep[i]), "-I%s",
                 deps[i].include_dir);
        store->argv[n++] = store->dep[i];
    }
    for (size_t i = 0; i < recipe->defines.count &&
                        i < sizeof(store->def) / sizeof(store->def[0]); i++) {
        snprintf(store->def[i], sizeof(store->def[i]), "-D%s",
                 recipe->defines.items[i]);
        store->argv[n++] = store->def[i];
    }
    store->argv[n++] = "-c";
    store->argv[n++] = src_file;
    store->argv[n++] = "-o";
    store->argv[n++] = obj_file;
    store->argv[n] = NULL;
    return n;
}

/* ── emit-mode helpers (install-build outputs) ──────────────────────── */

/* Absolute paths of every `lib*.a` in each locked dependency's lib/ dir, in
 * REVERSE --dep order: --dep arrives in lock BUILD order (dependencies
 * first), and a static link line wants dependents before their providers. */
struct pv_dep_archives {
    char path[PV_EMIT_MAX_DEPS * 4u][2200];
    size_t count;
};

static void pv_collect_dep_archives(const struct pv_emit_dep *deps,
                                    size_t dep_count,
                                    struct pv_dep_archives *out)
{
    out->count = 0;
    for (size_t k = dep_count; k > 0; k--) {
        const struct pv_emit_dep *d = &deps[k - 1];
        char lib_dir[2100];
        int n = snprintf(lib_dir, sizeof(lib_dir), "%s/lib", d->install_dir);
        if (n <= 0 || (size_t)n >= sizeof(lib_dir))
            continue;
        DIR *dir = opendir(lib_dir);
        if (!dir)
            continue;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            size_t nl = strlen(ent->d_name);
            if (nl < 4u || strncmp(ent->d_name, "lib", 3) != 0 ||
                strcmp(ent->d_name + nl - 2u, ".a") != 0)
                continue;
            if (out->count >= sizeof(out->path) / sizeof(out->path[0]))
                break;
            int pn = snprintf(out->path[out->count],
                              sizeof(out->path[out->count]), "%s/%s", lib_dir,
                              ent->d_name);
            if (pn > 0 && (size_t)pn < sizeof(out->path[out->count]))
                out->count++;
        }
        closedir(dir);
    }
}

/* SHA3-256 + byte count of one file, streamed in bounded chunks. */
static bool pv_sha3_file(const char *path, uint8_t out[32], uint64_t *bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    uint8_t buf[65536];
    uint64_t total = 0;
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        sha3_256_write(&ctx, buf, got);
        total += got;
    }
    bool ok = ferror(f) == 0;
    fclose(f);
    if (!ok)
        return false;
    sha3_256_finalize(&ctx, out);
    *bytes = total;
    return true;
}

static bool pv_copy_file(const char *src, const char *dst, mode_t mode)
{
    FILE *in = fopen(src, "rb");
    if (!in)
        return false;
    char parent[4200];
    (void)snprintf(parent, sizeof(parent), "%s", dst);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = '\0';
        if (!pv_mkdir_p(parent, 0700)) {
            fclose(in);
            return false;
        }
    }
    int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) {
        fclose(in);
        return false;
    }
    uint8_t buf[65536];
    size_t got;
    bool ok = true;
    while (ok && (got = fread(buf, 1, sizeof(buf), in)) > 0) {
        size_t off = 0;
        while (off < got) {
            ssize_t w = write(fd, buf + off, got - off);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                ok = false;
                break;
            }
            off += (size_t)w;
        }
    }
    if (ferror(in))
        ok = false;
    fclose(in);
    if (fsync(fd) != 0 || close(fd) != 0)
        ok = false;
    if (!ok)
        unlink(dst);
    return ok;
}

/* Install-relative header destination: strip the LONGEST recipe include-dir
 * prefix so `#include <ringbuffer.h>` keeps working after install; a header
 * under no include dir keeps its full package-relative path. */
static void pv_header_install_path(const struct vcs_package_recipe *recipe,
                                   const char *header, char *out,
                                   size_t out_cap)
{
    const char *best = header;
    size_t best_len = 0;
    for (size_t i = 0; i < recipe->include_dirs.count; i++) {
        const char *dir = recipe->include_dirs.items[i];
        size_t dl = strlen(dir);
        if (dl > best_len && strncmp(header, dir, dl) == 0 &&
            header[dl] == '/' && header[dl + 1] != '\0') {
            best = header + dl + 1u;
            best_len = dl;
        }
    }
    (void)snprintf(out, out_cap, "include/%s", best);
}

/* ── main flow ──────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    const char *root_hex = NULL;
    const char *store_dir = NULL;
    const char *key_path = NULL;
    const char *work_parent = NULL;
    const char *emit_dir = NULL;
    const char *lock_root_hex = NULL;
    bool require_full_isolation = false;
    struct pv_emit_dep emit_deps[PV_EMIT_MAX_DEPS];
    size_t emit_dep_count = 0;
    memset(emit_deps, 0, sizeof(emit_deps));
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--store=", 8) == 0)
            store_dir = argv[i] + 8;
        else if (strncmp(argv[i], "--key=", 6) == 0)
            key_path = argv[i] + 6;
        else if (strncmp(argv[i], "--work=", 7) == 0)
            work_parent = argv[i] + 7;
        else if (strncmp(argv[i], "--emit=", 7) == 0)
            emit_dir = argv[i] + 7;
        else if (strncmp(argv[i], "--lock-root=", 12) == 0)
            lock_root_hex = argv[i] + 12;
        else if (strncmp(argv[i], "--dep=", 6) == 0) {
            /* <64 hex>,<install dir> — the hex is fixed-width, so the comma
             * position is unambiguous even if the path contains commas. */
            const char *spec = argv[i] + 6;
            if (emit_dep_count >= PV_EMIT_MAX_DEPS) {
                fprintf(stderr, "%s: more than %u --dep entries\n", PV_LOG,
                        PV_EMIT_MAX_DEPS);
                return 2;
            }
            struct pv_emit_dep *d = &emit_deps[emit_dep_count];
            char hex[65];
            if (strlen(spec) < 66u || spec[64] != ',') {
                fprintf(stderr, "%s: --dep wants <64hex>,<install-dir>\n",
                        PV_LOG);
                return 2;
            }
            memcpy(hex, spec, 64);
            hex[64] = '\0';
            if (!zcl_hex_decode(hex, d->root, 32)) {
                fprintf(stderr, "%s: --dep root is not 64 hex chars\n",
                        PV_LOG);
                return 2;
            }
            int dn = snprintf(d->install_dir, sizeof(d->install_dir), "%s",
                              spec + 65);
            if (dn <= 0 || (size_t)dn >= sizeof(d->install_dir)) {
                fprintf(stderr, "%s: --dep install dir too long\n", PV_LOG);
                return 2;
            }
            /* Canonicalize to an ABSOLUTE path, same reason as --work above:
             * build children chdir() into build_root before their Landlock
             * grants (which now include every --dep install dir) are opened,
             * so a relative --dep path resolves against the wrong cwd and
             * the grant silently 404s (ENOENT), never against the caller's
             * actual directory. */
            char dep_resolved[4096]; /* realpath(3) requires >= PATH_MAX */
            if (!realpath(d->install_dir, dep_resolved)) {
                fprintf(stderr, "%s: cannot resolve --dep install dir %s: %s\n",
                        PV_LOG, d->install_dir, strerror(errno));
                return 2;
            }
            snprintf(d->install_dir, sizeof(d->install_dir), "%s",
                     dep_resolved);
            int in = snprintf(d->include_dir, sizeof(d->include_dir),
                              "%s/include", d->install_dir);
            if (in <= 0 || (size_t)in >= sizeof(d->include_dir)) {
                fprintf(stderr, "%s: --dep install dir too long\n", PV_LOG);
                return 2;
            }
            emit_dep_count++;
        } else if (strcmp(argv[i], "--require-full-isolation") == 0)
            require_full_isolation = true;
        else if (strcmp(argv[i], "--help") == 0 ||
                 strcmp(argv[i], "-h") == 0) {
            pv_usage(stdout);
            return 0;
        } else if (argv[i][0] != '-' && !root_hex)
            root_hex = argv[i];
        else {
            pv_usage(stderr);
            return 2;
        }
    }
    /* The two modes are mutually exclusive: an emit run signs nothing, so a
     * key would only invite the belief that its output was attested. */
    if (!root_hex || !store_dir || (!key_path && !emit_dir) ||
        (key_path && emit_dir) || (emit_dir && !lock_root_hex) ||
        (!emit_dir && (lock_root_hex || emit_dep_count > 0))) {
        pv_usage(stderr);
        return 2;
    }
    uint8_t emit_lock_root[32] = { 0 };
    if (emit_dir && !zcl_hex_decode(lock_root_hex, emit_lock_root, 32)) {
        fprintf(stderr, "%s: --lock-root wants 64 hex chars\n", PV_LOG);
        return 2;
    }
    uint8_t package_root[32];
    if (!zcl_hex_decode(root_hex, package_root, 32)) {
        fprintf(stderr, "%s: bad package root (want 64 hex)\n", PV_LOG);
        return 2;
    }

    /* Isolation probe FIRST: it decides full vs degraded before any work. */
    const bool landlock = os_sandbox_landlock_abi() >= 1;
    if (!landlock) {
        if (require_full_isolation) {
            fprintf(stderr,
                    "%s: FATAL: --require-full-isolation and this kernel "
                    "offers no Landlock — failing closed, nothing signed\n",
                    PV_LOG);
            return 4;
        }
        fprintf(stderr,
                "%s: WARNING: Landlock unavailable on this kernel — running "
                "DEGRADED (no filesystem scoping; seccomp + rlimits still "
                "deny network and bound resources). The attestation will "
                "carry isolation=degraded.\n", PV_LOG);
    }

    /* Store layout sanity. */
    char probe[4096];
    struct stat st;
    static const char *const k_need[] = {
        "/manifests", "/releases", "/recipes", "/cas/sha3",
    };
    for (size_t i = 0; i < sizeof(k_need) / sizeof(k_need[0]); i++) {
        int n = snprintf(probe, sizeof(probe), "%s%s", store_dir, k_need[i]);
        if (n < 0 || (size_t)n >= sizeof(probe) ||
            stat(probe, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "%s: %s%s: not a package store directory\n",
                    PV_LOG, store_dir, k_need[i]);
            return 3;
        }
    }
    if (!emit_dir) {
        int n = snprintf(probe, sizeof(probe), "%s/attestations", store_dir);
        if (n < 0 || (size_t)n >= sizeof(probe) || !pv_mkdir_p(probe, 0700)) {
            fprintf(stderr, "%s: cannot create %s/attestations\n", PV_LOG,
                    store_dir);
            return 3;
        }
    }

    /* Verifier key — attestation mode only. An emit run signs nothing, so it
     * never reads a secret; the caller re-hashes the outputs instead. */
    secp256k1_context *sign_ctx = NULL;
    uint8_t secret[32] = { 0 };
    uint8_t verifier_pubkey[33] = { 0 };
    if (!emit_dir) {
        struct stat kst;
        if (stat(key_path, &kst) != 0 || !S_ISREG(kst.st_mode) ||
            (kst.st_mode & 077) != 0) {
            fprintf(stderr,
                    "%s: key file %s must be a regular file with no "
                    "group/other permission bits (0600 or 0400)\n",
                    PV_LOG, key_path);
            return 3;
        }
        size_t key_len = 0;
        uint8_t *key_text = pv_read_file(key_path, 128, &key_len);
        if (!key_text) {
            fprintf(stderr, "%s: cannot read key file %s\n", PV_LOG,
                    key_path);
            return 3;
        }
        while (key_len > 0 &&
               (key_text[key_len - 1] == '\n' ||
                key_text[key_len - 1] == '\r' ||
                key_text[key_len - 1] == ' ' ||
                key_text[key_len - 1] == '\t'))
            key_len--;
        char key_hex[65];
        if (key_len != 64) {
            fprintf(stderr, "%s: key file must hold exactly 64 hex chars\n",
                    PV_LOG);
            free(key_text);
            return 3;
        }
        memcpy(key_hex, key_text, 64);
        key_hex[64] = '\0';
        free(key_text);
        if (!zcl_hex_decode(key_hex, secret, 32)) {
            fprintf(stderr, "%s: key file is not 64 hex chars\n", PV_LOG);
            return 3;
        }
        sign_ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
        if (!sign_ctx || !secp256k1_ec_seckey_verify(sign_ctx, secret)) {
            fprintf(stderr, "%s: key is not a valid secp256k1 secret\n",
                    PV_LOG);
            return 3;
        }
        secp256k1_pubkey vpk;
        size_t vpk_len = sizeof(verifier_pubkey);
        if (!secp256k1_ec_pubkey_create(sign_ctx, &vpk, secret) ||
            !secp256k1_ec_pubkey_serialize(sign_ctx, verifier_pubkey,
                                           &vpk_len, &vpk,
                                           SECP256K1_EC_COMPRESSED) ||
            vpk_len != sizeof(verifier_pubkey)) {
            fprintf(stderr, "%s: cannot derive the verifier pubkey\n",
                    PV_LOG);
            return 3;
        }
    }

    /* Release + manifest + recipe. */
    struct vcs_package_release release;
    uint8_t release_id[32] = { 0 };
    if (!pv_load_release(store_dir, package_root, &release, release_id)) {
        fprintf(stderr,
                "%s: no verified release envelope in %s/releases names "
                "this package root\n", PV_LOG, store_dir);
        return 3;
    }
    char root_hex64[65];
    zcl_hex_encode(package_root, 32, root_hex64);
    int mn = snprintf(probe, sizeof(probe), "%s/manifests/%s", store_dir,
                      root_hex64);
    size_t manifest_wire_len = 0;
    uint8_t *manifest_wire =
        mn > 0 && (size_t)mn < sizeof(probe)
            ? pv_read_file(probe, VCS_PACKAGE_MANIFEST_MAX_WIRE_BYTES,
                           &manifest_wire_len)
            : NULL;
    if (!manifest_wire) {
        fprintf(stderr, "%s: manifest for %s not hosted\n", PV_LOG,
                root_hex64);
        return 3;
    }
    struct vcs_package_manifest manifest;
    vcs_package_manifest_init(&manifest);
    if (!vcs_package_manifest_parse(manifest_wire, manifest_wire_len,
                                    &manifest)) {
        fprintf(stderr, "%s: manifest %s does not parse\n", PV_LOG,
                root_hex64);
        free(manifest_wire);
        return 3;
    }
    free(manifest_wire);
    uint8_t manifest_root[32] = { 0 };
    if (!vcs_package_manifest_root(&manifest, manifest_root) ||
        memcmp(manifest_root, package_root, 32) != 0) {
        fprintf(stderr, "%s: manifest root mismatch for %s\n", PV_LOG,
                root_hex64);
        vcs_package_manifest_free(&manifest);
        return 3;
    }
    char recipe_root_hex[65];
    zcl_hex_encode(release.recipe_root, 32, recipe_root_hex);
    int rn = snprintf(probe, sizeof(probe), "%s/recipes/%s", store_dir,
                      recipe_root_hex);
    size_t recipe_wire_len = 0;
    uint8_t *recipe_wire =
        rn > 0 && (size_t)rn < sizeof(probe)
            ? pv_read_file(probe, VCS_PACKAGE_RECIPE_MAX_WIRE_BYTES,
                           &recipe_wire_len)
            : NULL;
    if (!recipe_wire) {
        fprintf(stderr, "%s: recipe %s not hosted\n", PV_LOG,
                recipe_root_hex);
        vcs_package_manifest_free(&manifest);
        return 3;
    }
    struct vcs_package_recipe recipe;
    vcs_package_recipe_init(&recipe);
    enum vcs_package_recipe_error rerr =
        vcs_package_recipe_parse(recipe_wire, recipe_wire_len, &recipe);
    free(recipe_wire);
    if (rerr != VCS_PACKAGE_RECIPE_OK) {
        fprintf(stderr, "%s: recipe %s does not parse: %s\n", PV_LOG,
                recipe_root_hex, vcs_package_recipe_error_string(rerr));
        vcs_package_manifest_free(&manifest);
        return 3;
    }
    uint8_t recipe_root[32] = { 0 };
    if (vcs_package_recipe_root(&recipe, recipe_root) !=
            VCS_PACKAGE_RECIPE_OK ||
        memcmp(recipe_root, release.recipe_root, 32) != 0) {
        fprintf(stderr, "%s: recipe root mismatch against the envelope\n",
                PV_LOG);
        vcs_package_recipe_free(&recipe);
        vcs_package_manifest_free(&manifest);
        return 3;
    }
    char membership[160];
    if (!vcs_package_recipe_files_in_manifest(&recipe, &manifest,
                                              membership,
                                              sizeof(membership))) {
        fprintf(stderr, "%s: recipe path not in manifest: %s\n", PV_LOG,
                membership);
        vcs_package_recipe_free(&recipe);
        vcs_package_manifest_free(&manifest);
        return 3;
    }

    /* Completeness: every chunk must be in the CAS. */
    for (size_t i = 0; i < manifest.count; i++) {
        const struct vcs_package_file *f = &manifest.files[i];
        for (uint32_t c = 0; c < f->chunk_count; c++) {
            char hex[65];
            zcl_hex_encode(f->chunk_hashes + (size_t)c * 32u, 32, hex);
            int cn = snprintf(probe, sizeof(probe), "%s/cas/sha3/%.2s/%s",
                              store_dir, hex, hex);
            if (cn < 0 || (size_t)cn >= sizeof(probe) ||
                stat(probe, &st) != 0) {
                fprintf(stderr,
                        "%s: package incomplete: chunk %s#%u absent\n",
                        PV_LOG, f->path, c);
                vcs_package_recipe_free(&recipe);
                vcs_package_manifest_free(&manifest);
                return 3;
            }
        }
    }

    /* Temp work tree: <work>/zclverify.XXXXXX/{src,build}. */
    if (!work_parent) {
        work_parent = getenv("TMPDIR");
        if (!work_parent || !work_parent[0])
            work_parent = "/tmp";
    }
    char work[4096];
    int wn = snprintf(work, sizeof(work), "%s/zclverify.XXXXXX",
                      work_parent);
    if (wn < 0 || (size_t)wn >= sizeof(work) || !mkdtemp(work)) {
        fprintf(stderr, "%s: cannot create temp dir under %s: %s\n", PV_LOG,
                work_parent, strerror(errno));
        vcs_package_recipe_free(&recipe);
        vcs_package_manifest_free(&manifest);
        return 5;
    }
    /* Canonicalize to an ABSOLUTE path: build children chdir() into the
     * build dir before their Landlock grants (which carry src/build paths)
     * are opened, and compiler argv carries src/build paths too — a
     * relative --work would break both. */
    {
        char resolved[4096];
        if (!realpath(work, resolved)) {
            fprintf(stderr, "%s: cannot resolve %s: %s\n", PV_LOG, work,
                    strerror(errno));
            pv_rm_rf(work);
            vcs_package_recipe_free(&recipe);
            vcs_package_manifest_free(&manifest);
            return 5;
        }
        snprintf(work, sizeof(work), "%s", resolved);
    }
    char src_root[4200];
    char build_root[4200];
    snprintf(src_root, sizeof(src_root), "%s/src", work);
    snprintf(build_root, sizeof(build_root), "%s/build", work);
    if (!pv_mkdir_p(src_root, 0755) || !pv_mkdir_p(build_root, 0755)) {
        fprintf(stderr, "%s: cannot create %s/{src,build}\n", PV_LOG, work);
        pv_rm_rf(work);
        vcs_package_recipe_free(&recipe);
        vcs_package_manifest_free(&manifest);
        return 5;
    }

    /* Materialize the package read-only from the CAS. */
    bool materialized = true;
    for (size_t i = 0; i < manifest.count && materialized; i++) {
        const struct vcs_package_file *f = &manifest.files[i];
        char dest[4200];
        int dn = snprintf(dest, sizeof(dest), "%s/%s", src_root, f->path);
        if (dn < 0 || (size_t)dn >= sizeof(dest)) {
            materialized = false;
            break;
        }
        char parent[4200];
        snprintf(parent, sizeof(parent), "%s", dest);
        char *slash = strrchr(parent, '/');
        if (slash) {
            *slash = '\0';
            if (!pv_mkdir_p(parent, 0755)) {
                materialized = false;
                break;
            }
        }
        int fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0444);
        if (fd < 0) {
            materialized = false;
            break;
        }
        for (uint32_t c = 0; c < f->chunk_count; c++) {
            char hex[65];
            zcl_hex_encode(f->chunk_hashes + (size_t)c * 32u, 32, hex);
            char chunk_path[4200];
            snprintf(chunk_path, sizeof(chunk_path), "%s/cas/sha3/%.2s/%s",
                     store_dir, hex, hex);
            size_t chunk_len = 0;
            uint8_t *chunk = pv_read_file(chunk_path,
                                          VCS_PACKAGE_CHUNK_BYTES,
                                          &chunk_len);
            if (!chunk) {
                close(fd);
                materialized = false;
                break;
            }
            size_t off = 0;
            while (off < chunk_len) {
                ssize_t w = write(fd, chunk + off, chunk_len - off);
                if (w < 0) {
                    if (errno == EINTR)
                        continue;
                    free(chunk);
                    close(fd);
                    materialized = false;
                    break;
                }
                off += (size_t)w;
            }
            free(chunk);
            if (!materialized)
                break;
        }
        if (close(fd) != 0)
            materialized = false;
    }
    if (!materialized) {
        fprintf(stderr, "%s: failed to materialize the package tree\n",
                PV_LOG);
        pv_rm_rf(work);
        vcs_package_recipe_free(&recipe);
        vcs_package_manifest_free(&manifest);
        return 5;
    }

    struct os_sandbox_path_rule rules[10u + PV_EMIT_MAX_DEPS];
    size_t n_rules = pv_child_grants(src_root, build_root, emit_deps,
                                     emit_dep_count, rules,
                                     sizeof(rules) / sizeof(rules[0]));

    /* Compiler probes (version strings are recorded in the attestation). */
    struct pv_compiler compilers[2] = {
        { .id = "clang", .available = false,
          .outcome = VCS_PACKAGE_ATTEST_OUTCOME_UNAVAILABLE },
        { .id = "gcc", .available = false,
          .outcome = VCS_PACKAGE_ATTEST_OUTCOME_UNAVAILABLE },
    };
    for (size_t i = 0; i < 2; i++) {
        const char *vargv[] = { compilers[i].id, "--version", NULL };
        struct pv_run pr = pv_run_child(vargv, NULL, NULL, landlock, rules,
                                        n_rules, NULL, 10000);
        if (pr.launched && pr.exited && pr.exit_code == 0 &&
            pr.stdout_buf[0]) {
            compilers[i].available = true;
            size_t vl = strcspn(pr.stdout_buf, "\r\n");
            if (vl >= sizeof(compilers[i].version))
                vl = sizeof(compilers[i].version) - 1;
            for (size_t k = 0; k < vl; k++) {
                unsigned char c = (unsigned char)pr.stdout_buf[k];
                compilers[i].version[k] =
                    (c >= 0x20 && c <= 0x7e) ? (char)c : '?';
            }
            compilers[i].version[vl] = '\0';
        } else {
            snprintf(compilers[i].version, sizeof(compilers[i].version),
                     "unavailable");
        }
    }

    struct pv_dep_archives dep_archives;
    memset(&dep_archives, 0, sizeof(dep_archives));
    pv_collect_dep_archives(emit_deps, emit_dep_count, &dep_archives);

    /* The build+test matrix. */
    struct vcs_package_attest att;
    memset(&att, 0, sizeof(att));
    att.schema_version = VCS_PACKAGE_ATTEST_VERSION;
    memcpy(att.package_root, package_root, 32);
    memcpy(att.release_id, release_id, 32);
    memcpy(att.recipe_root, recipe_root, 32);
    att.isolation = landlock ? VCS_PACKAGE_ATTEST_ISOLATION_FULL
                             : VCS_PACKAGE_ATTEST_ISOLATION_DEGRADED;
    memcpy(att.verifier_pubkey, verifier_pubkey, 33);

    const bool have_tests = recipe.test_sources.count > 0;
    bool build_ok = true;
    uint8_t build_fail_code = VCS_PACKAGE_ATTEST_DETAIL_COMPILE_ERROR;
    char build_fail_detail[VCS_PACKAGE_ATTEST_DETAIL_MAX + 1u];
    build_fail_detail[0] = '\0';

    char env_tmpdir[4200];
    snprintf(env_tmpdir, sizeof(env_tmpdir), "TMPDIR=%s", build_root);
    const char *const compile_env[] = { env_tmpdir, NULL };
    char san_asan[96];
    char san_ubsan[96];
    /* detect_leaks=0 is deliberate: LeakSanitizer's stop-the-world needs
     * ptrace + /proc/self access, both denied by the child confinement
     * (seccomp deny-set, Landlock grants) — leak checking is fundamentally
     * incompatible with the sandbox, so ASan runs its memory-error detector
     * only. UBSan is unaffected. */
    snprintf(san_asan, sizeof(san_asan),
             "ASAN_OPTIONS=exitcode=%d:detect_leaks=0", PV_ASAN_EXIT);
    snprintf(san_ubsan, sizeof(san_ubsan),
             "UBSAN_OPTIONS=halt_on_error=1:exitcode=%d:print_stacktrace=1",
             PV_UBSAN_EXIT);
    const char *const san_env[] = { env_tmpdir, san_asan, san_ubsan, NULL };

    const struct os_sandbox_rlimits compile_limits = {
        .as_bytes = PV_COMPILE_AS_BYTES,
        .cpu_seconds = PV_COMPILE_TIMEOUT_MS / 1000 + 5,
        .nproc = PV_COMPILE_NPROC,
        .fsize_bytes = PV_COMPILE_FSIZE_BYTES,
        .nofile = PV_COMPILE_NOFILE,
        .core_bytes = 0,
    };
    const struct os_sandbox_rlimits test_limits = {
        .as_bytes = recipe.maximum_memory_bytes,
        .cpu_seconds = recipe.maximum_test_seconds,
        .nproc = PV_TEST_NPROC,
        .fsize_bytes = PV_TEST_FSIZE_BYTES,
        .nofile = PV_TEST_NOFILE,
        .core_bytes = 0,
    };
    const struct os_sandbox_rlimits san_test_limits = {
        /* ASan's shadow address space makes RLIMIT_AS meaningless — the
         * ONE documented rlimit exception (see the file header). */
        .as_bytes = OS_SANDBOX_RLIMIT_KEEP,
        .cpu_seconds = recipe.maximum_test_seconds,
        .nproc = PV_TEST_NPROC,
        .fsize_bytes = PV_TEST_FSIZE_BYTES,
        .nofile = PV_COMPILE_NOFILE,
        .core_bytes = 0,
    };

    for (size_t ci = 0; ci < 2 && build_ok; ci++) {
        if (!compilers[ci].available)
            continue;
        const char *cc = compilers[ci].id;
        bool cc_ok = true;
        for (int variant = 0; variant < 2 && cc_ok; variant++) {
            const bool sanitize = variant == 1;
            /* Sanitizer binaries are only needed when tests will run. */
            if (sanitize && !have_tests)
                continue;
            char fail_prefix[32];
            snprintf(fail_prefix, sizeof(fail_prefix), "%s%s", cc,
                     sanitize ? "+san" : "");
            const size_t total_sources =
                recipe.sources.count +
                (have_tests ? recipe.test_sources.count : 0);
            for (size_t si = 0; si < total_sources && cc_ok; si++) {
                const char *rel = si < recipe.sources.count
                    ? recipe.sources.items[si]
                    : recipe.test_sources
                          .items[si - recipe.sources.count];
                char src_file[4200];
                char obj_file[4200];
                snprintf(src_file, sizeof(src_file), "%s/%s", src_root, rel);
                snprintf(obj_file, sizeof(obj_file),
                         "%s/%s_%d_%zu.o", build_root, cc, variant, si);
                struct pv_compile_args args;
                memset(&args, 0, sizeof(args));
                pv_compile_argv(&args, cc, sanitize, &recipe, src_root,
                                emit_deps, emit_dep_count, src_file,
                                obj_file);
                struct pv_run pr = pv_run_child(
                    args.argv, build_root, &compile_limits, landlock, rules,
                    n_rules, compile_env, PV_COMPILE_TIMEOUT_MS);
                if (!pr.launched || pr.sandbox_fail) {
                    fprintf(stderr,
                            "%s: internal: compile child failed to launch "
                            "or arm its sandbox (%s)\n", PV_LOG,
                            pr.stderr_buf);
                    pv_rm_rf(work);
                    vcs_package_recipe_free(&recipe);
                    vcs_package_manifest_free(&manifest);
                    return 5;
                }
                if (pr.timed_out) {
                    cc_ok = false;
                    build_fail_code =
                        VCS_PACKAGE_ATTEST_DETAIL_COMPILE_TIMEOUT;
                    snprintf(build_fail_detail, sizeof(build_fail_detail),
                             "%s: compile timed out: %s", fail_prefix, rel);
                } else if (!pr.exited || pr.exit_code != 0) {
                    cc_ok = false;
                    build_fail_code =
                        VCS_PACKAGE_ATTEST_DETAIL_COMPILE_ERROR;
                    pv_detail_from_stderr(fail_prefix, pr.stderr_buf,
                                          build_fail_detail,
                                          sizeof(build_fail_detail));
                }
            }
            /* Link the test binary (plain and sanitizer variants). Object
             * names are deterministic — regenerated, never stored. */
            if (cc_ok && have_tests) {
                char bin_file[4200];
                snprintf(bin_file, sizeof(bin_file), "%s/%s_%d_test",
                         build_root, cc, variant);
                const char *largv[560];
                char lobjs[512][96];
                size_t ln = 0;
                largv[ln++] = cc;
                if (sanitize)
                    largv[ln++] = "-fsanitize=address,undefined";
                for (size_t o = 0; o < total_sources &&
                                   o < sizeof(lobjs) / sizeof(lobjs[0]);
                     o++) {
                    /* cwd is build_root: basenames resolve there. */
                    snprintf(lobjs[o], sizeof(lobjs[o]), "%s_%d_%zu.o", cc,
                             variant, o);
                    largv[ln++] = lobjs[o];
                }
                largv[ln++] = "-o";
                largv[ln++] = bin_file;
                /* Locked dependency archives come after the objects that
                 * reference them (static-link order). */
                for (size_t da = 0; da < dep_archives.count &&
                                    ln + 8u < sizeof(largv) / sizeof(largv[0]);
                     da++)
                    largv[ln++] = dep_archives.path[da];
                for (size_t li = 0; li < recipe.library_count; li++) {
                    if (recipe.libraries[li] == VCS_PACKAGE_RECIPE_LIB_LIBM)
                        largv[ln++] = "-lm";
                    else if (recipe.libraries[li] ==
                             VCS_PACKAGE_RECIPE_LIB_PTHREAD)
                        largv[ln++] = "-lpthread";
                }
                largv[ln] = NULL;
                struct pv_run pr = pv_run_child(
                    largv, build_root, &compile_limits, landlock, rules,
                    n_rules, compile_env, PV_LINK_TIMEOUT_MS);
                if (!pr.launched || pr.sandbox_fail) {
                    fprintf(stderr,
                            "%s: internal: link child failed to launch or "
                            "arm its sandbox (%s)\n", PV_LOG, pr.stderr_buf);
                    pv_rm_rf(work);
                    vcs_package_recipe_free(&recipe);
                    vcs_package_manifest_free(&manifest);
                    return 5;
                }
                if (pr.timed_out) {
                    cc_ok = false;
                    build_fail_code =
                        VCS_PACKAGE_ATTEST_DETAIL_COMPILE_TIMEOUT;
                    snprintf(build_fail_detail, sizeof(build_fail_detail),
                             "%s: link timed out", fail_prefix);
                } else if (!pr.exited || pr.exit_code != 0) {
                    cc_ok = false;
                    build_fail_code = VCS_PACKAGE_ATTEST_DETAIL_LINK_ERROR;
                    pv_detail_from_stderr(fail_prefix, pr.stderr_buf,
                                          build_fail_detail,
                                          sizeof(build_fail_detail));
                }
            }
            if (!cc_ok) {
                compilers[ci].outcome = VCS_PACKAGE_ATTEST_OUTCOME_FAIL;
                build_ok = false;
                break;
            }
        }
        if (build_ok && compilers[ci].available)
            compilers[ci].outcome = VCS_PACKAGE_ATTEST_OUTCOME_PASS;
    }

    /* Test runs (only when every compiler built). */
    bool test_ok = true;
    bool test_ran = false;
    uint32_t test_exit = 0;
    uint8_t test_fail_code = VCS_PACKAGE_ATTEST_DETAIL_NONE;
    char test_fail_detail[VCS_PACKAGE_ATTEST_DETAIL_MAX + 1u];
    test_fail_detail[0] = '\0';
    bool sanitizer_clean = true;
    uint8_t sanitizer_fail_code = VCS_PACKAGE_ATTEST_DETAIL_NONE;
    char sanitizer_fail_detail[VCS_PACKAGE_ATTEST_DETAIL_MAX + 1u];
    sanitizer_fail_detail[0] = '\0';

    if (build_ok && have_tests) {
        for (size_t ci = 0; ci < 2; ci++) {
            if (!compilers[ci].available)
                continue;
            const char *cc = compilers[ci].id;
            /* Plain run: the resource-bound verdict. */
            char bin_file[4200];
            snprintf(bin_file, sizeof(bin_file), "%s/%s_0_test", build_root,
                     cc);
            struct pv_run pr = pv_run_child(
                (const char *const[]){ bin_file, NULL }, build_root,
                &test_limits, landlock, rules, n_rules, compile_env,
                (int)recipe.maximum_test_seconds * 1000 + 5000);
            if (!pr.launched || pr.sandbox_fail) {
                fprintf(stderr,
                        "%s: internal: test child failed to launch or arm "
                        "its sandbox\n", PV_LOG);
                pv_rm_rf(work);
                vcs_package_recipe_free(&recipe);
                vcs_package_manifest_free(&manifest);
                return 5;
            }
            test_ran = true;
            if (pr.timed_out) {
                test_ok = false;
                test_fail_code = VCS_PACKAGE_ATTEST_DETAIL_TEST_TIMEOUT;
                snprintf(test_fail_detail, sizeof(test_fail_detail),
                         "%s: test exceeded %u s", cc,
                         recipe.maximum_test_seconds);
            } else if (pr.exited) {
                test_exit = (uint32_t)pr.exit_code;
                if (pr.exit_code != recipe.expected_test_exit_code) {
                    test_ok = false;
                    test_fail_code =
                        VCS_PACKAGE_ATTEST_DETAIL_TEST_EXIT_MISMATCH;
                    snprintf(test_fail_detail, sizeof(test_fail_detail),
                             "%s: exit %d, expected %u", cc, pr.exit_code,
                             recipe.expected_test_exit_code);
                }
            } else {
                test_ok = false;
                test_fail_code = VCS_PACKAGE_ATTEST_DETAIL_TEST_SIGNAL;
                snprintf(test_fail_detail, sizeof(test_fail_detail),
                         "%s: killed by signal %d", cc, pr.term_signal);
            }
            /* Sanitizer run: the UB-detector diagnostic. */
            char san_bin[4200];
            snprintf(san_bin, sizeof(san_bin), "%s/%s_1_test", build_root,
                     cc);
            struct pv_run sr = pv_run_child(
                (const char *const[]){ san_bin, NULL }, build_root,
                &san_test_limits, landlock, rules, n_rules, san_env,
                (int)recipe.maximum_test_seconds * 1000 + 5000);
            if (!sr.launched || sr.sandbox_fail) {
                fprintf(stderr,
                        "%s: internal: sanitizer child failed to launch or "
                        "arm its sandbox\n", PV_LOG);
                pv_rm_rf(work);
                vcs_package_recipe_free(&recipe);
                vcs_package_manifest_free(&manifest);
                return 5;
            }
            /* A sanitizer run failure is a FINDING only when the run died
             * by the sanitizer's own marker exit code (a real report — the
             * text is captured into the detail). Any other death — killed
             * by the seccomp deny-set, the wall-clock deadline, a wrong
             * exit code — mirrors the (identically confined) plain run or
             * host load and says nothing about UB: the diagnostic is then
             * UNAVAILABLE, never a finding. Findings are recorded only
             * when every plain run passed: the closed grammar allows a
             * findings outcome only in the sanitizer-fail class. Precedence
             * across compilers: findings > unavailable > pass. */
            bool san_failed =
                sr.timed_out || !sr.exited ||
                sr.exit_code != recipe.expected_test_exit_code;
            if (san_failed) {
                /* A marker exit code alone is NOT proof of a report: the
                 * runtimes also abort through the same exit path when ASan
                 * cannot INITIALIZE (a fixed-shadow collision under host
                 * memory pressure prints "Shadow memory range interleaves
                 * ... ASan cannot proceed correctly. ABORTING."). A finding
                 * requires the report text itself (ASan "SUMMARY:" / UBSan
                 * "runtime error:"); anything else is environmental and
                 * makes the diagnostic UNAVAILABLE, never dirty. */
                bool reported =
                    strstr(sr.stderr_buf, "SUMMARY:") != NULL ||
                    strstr(sr.stderr_buf, "runtime error:") != NULL;
                bool finding = false;
                char sprefix[40];
                snprintf(sprefix, sizeof(sprefix), "%s+san", cc);
                if (test_ok && reported && sr.exited &&
                    sr.exit_code == PV_ASAN_EXIT) {
                    att.sanitizers[0].outcome =
                        VCS_PACKAGE_ATTEST_OUTCOME_FAIL;
                    sanitizer_fail_code =
                        VCS_PACKAGE_ATTEST_DETAIL_ASAN_FINDINGS;
                    finding = true;
                } else if (test_ok && reported && sr.exited &&
                           sr.exit_code == PV_UBSAN_EXIT) {
                    att.sanitizers[1].outcome =
                        VCS_PACKAGE_ATTEST_OUTCOME_FAIL;
                    sanitizer_fail_code =
                        VCS_PACKAGE_ATTEST_DETAIL_UBSAN_FINDINGS;
                    finding = true;
                }
                if (finding) {
                    sanitizer_clean = false;
                    pv_san_detail_from_stderr(sprefix, sr.stderr_buf,
                                              sanitizer_fail_detail,
                                              sizeof(sanitizer_fail_detail));
                    break;   /* a finding is definitive */
                }
                if (att.sanitizers[0].outcome !=
                        VCS_PACKAGE_ATTEST_OUTCOME_FAIL &&
                    att.sanitizers[1].outcome !=
                        VCS_PACKAGE_ATTEST_OUTCOME_FAIL) {
                    att.sanitizers[0].outcome =
                        VCS_PACKAGE_ATTEST_OUTCOME_UNAVAILABLE;
                    att.sanitizers[1].outcome =
                        VCS_PACKAGE_ATTEST_OUTCOME_UNAVAILABLE;
                }
                continue;   /* an unavailable diagnostic decides nothing */
            }
            /* Clean run: leave the outcome as-is (pass by default; a
             * previous compiler's unavailable is not upgraded — the
             * diagnostic is only as strong as its weakest run). */
        }
    }

    /* Assemble the verdict. */
    for (size_t i = 0; i < 2; i++) {
        snprintf(att.compilers[i].id, sizeof(att.compilers[i].id), "%s",
                 compilers[i].id);
        snprintf(att.compilers[i].version,
                 sizeof(att.compilers[i].version), "%s",
                 compilers[i].version);
        att.compilers[i].outcome = compilers[i].outcome;
    }
    att.compiler_count = 2;
    if (have_tests) {
        snprintf(att.sanitizers[0].name,
                 sizeof(att.sanitizers[0].name), "asan");
        snprintf(att.sanitizers[1].name,
                 sizeof(att.sanitizers[1].name), "ubsan");
        att.sanitizer_count = 2;
        if (!build_ok) {
            att.sanitizers[0].outcome =
                VCS_PACKAGE_ATTEST_OUTCOME_UNAVAILABLE;
            att.sanitizers[1].outcome =
                VCS_PACKAGE_ATTEST_OUTCOME_UNAVAILABLE;
        }
    }
    att.test_ran = test_ran;
    att.test_exit_code = test_ran ? test_exit : 0;

    if (!build_ok) {
        att.result_class = VCS_PACKAGE_ATTEST_RESULT_BUILD_FAIL;
        att.detail_code = build_fail_code;
        snprintf(att.detail, sizeof(att.detail), "%s", build_fail_detail);
    } else if (have_tests && !test_ok) {
        att.result_class = VCS_PACKAGE_ATTEST_RESULT_TEST_FAIL;
        att.detail_code = test_fail_code;
        snprintf(att.detail, sizeof(att.detail), "%s", test_fail_detail);
    } else if (have_tests && !sanitizer_clean) {
        att.result_class = VCS_PACKAGE_ATTEST_RESULT_SANITIZER_FAIL;
        att.detail_code = sanitizer_fail_code;
        snprintf(att.detail, sizeof(att.detail), "%s",
                 sanitizer_fail_detail);
    } else if (have_tests) {
        att.result_class = VCS_PACKAGE_ATTEST_RESULT_TEST_PASS;
    } else {
        att.result_class = VCS_PACKAGE_ATTEST_RESULT_BUILD_PASS;
    }

    /* No compiler at all: there is no honest verdict to sign. */
    if (!compilers[0].available && !compilers[1].available) {
        fprintf(stderr,
                "%s: neither gcc nor clang is available — no attestation "
                "signed\n", PV_LOG);
        pv_rm_rf(work);
        vcs_package_recipe_free(&recipe);
        vcs_package_manifest_free(&manifest);
        return 5;
    }

    /* ── emit mode: archive, install-stage, receipt (nothing is signed) ── */
    if (emit_dir) {
        struct vcs_package_build_receipt rec;
        vcs_package_build_receipt_init(&rec);
        memcpy(rec.package_root, package_root, 32);
        memcpy(rec.recipe_root, recipe_root, 32);
        memcpy(rec.lock_root, emit_lock_root, 32);
        for (size_t i = 0; i < emit_dep_count; i++) {
            enum vcs_package_build_error de =
                vcs_package_build_add_dep(&rec, emit_deps[i].root);
            if (de != VCS_PACKAGE_BUILD_OK) {
                fprintf(stderr, "%s: --dep set rejected: %s\n", PV_LOG,
                        vcs_package_build_error_string(de));
                pv_rm_rf(work);
                vcs_package_recipe_free(&recipe);
                vcs_package_manifest_free(&manifest);
                return 2;
            }
        }
        /* Deterministic compiler choice: gcc when present, else clang. The
         * verdict above already required BOTH available compilers to pass. */
        const size_t pick = compilers[1].available ? 1u : 0u;
        snprintf(rec.compiler_id, sizeof(rec.compiler_id), "%s",
                 compilers[pick].id);
        snprintf(rec.compiler_version, sizeof(rec.compiler_version), "%s",
                 compilers[pick].version);
        snprintf(rec.flags, sizeof(rec.flags), "%s",
                 "-std=c23 -O1 -fno-omit-frame-pointer -c");
        rec.isolation = landlock
                            ? (uint8_t)VCS_PACKAGE_BUILD_ISOLATION_FULL
                            : (uint8_t)VCS_PACKAGE_BUILD_ISOLATION_DEGRADED;
        rec.test_ran = have_tests && test_ran;
        rec.test_exit_code = rec.test_ran ? test_exit : 0u;
        if (!build_ok)
            rec.result_class = (uint8_t)VCS_PACKAGE_BUILD_RESULT_BUILD_FAIL;
        else if (have_tests && !test_ok)
            rec.result_class = (uint8_t)VCS_PACKAGE_BUILD_RESULT_TEST_FAIL;
        else if (have_tests)
            rec.result_class = (uint8_t)VCS_PACKAGE_BUILD_RESULT_TEST_PASS;
        else
            rec.result_class = (uint8_t)VCS_PACKAGE_BUILD_RESULT_BUILD_PASS;
        /* A sanitizer finding is not a build/test verdict in emit mode; the
         * attestation lane owns that diagnostic. The receipt records the
         * build+test verdict only, so a clean test that trips ASan still
         * installs — exactly as the attestation quorum, not the installer,
         * is the place that judges sanitizer cleanliness. */

        bool emitted = true;
        if (vcs_package_build_installable(&rec)) {
            /* Archive the recipe's NON-TEST objects. Test objects hold
             * main() and are never part of an installed library. */
            char pkg_short[VCS_PACKAGE_RELEASE_NAME_MAX + 1u];
            const char *slash = strchr(release.name, '/');
            snprintf(pkg_short, sizeof(pkg_short), "%s",
                     slash ? slash + 1 : release.name);
            char archive_name[VCS_PACKAGE_RELEASE_NAME_MAX + 8u];
            snprintf(archive_name, sizeof(archive_name), "lib%s.a", pkg_short);
            const char *aargv[8u + 512u];
            char aobjs[512][96];
            size_t an = 0;
            aargv[an++] = "ar";
            aargv[an++] = "rcs";
            aargv[an++] = archive_name;
            for (size_t o = 0; o < recipe.sources.count &&
                               o < sizeof(aobjs) / sizeof(aobjs[0]);
                 o++) {
                snprintf(aobjs[o], sizeof(aobjs[o]), "%s_0_%zu.o",
                         compilers[pick].id, o);
                aargv[an++] = aobjs[o];
            }
            aargv[an] = NULL;
            struct pv_run ar = pv_run_child(aargv, build_root, &compile_limits,
                                            landlock, rules, n_rules,
                                            compile_env, PV_LINK_TIMEOUT_MS);
            if (!ar.launched || ar.sandbox_fail || ar.timed_out ||
                !ar.exited || ar.exit_code != 0) {
                fprintf(stderr,
                        "%s: `ar rcs %s` failed (%s) — no artifact emitted\n",
                        PV_LOG, archive_name,
                        ar.timed_out ? "timed out" : ar.stderr_buf);
                emitted = false;
            }
            char src_archive[4300];
            char dst_archive[4400];
            char rel_archive[VCS_PACKAGE_BUILD_PATH_MAX + 1u];
            snprintf(src_archive, sizeof(src_archive), "%s/%s", build_root,
                     archive_name);
            snprintf(rel_archive, sizeof(rel_archive), "lib/%s", archive_name);
            snprintf(dst_archive, sizeof(dst_archive), "%s/%s", emit_dir,
                     rel_archive);
            uint8_t h[32];
            uint64_t nbytes = 0;
            if (emitted &&
                (!pv_copy_file(src_archive, dst_archive, 0644) ||
                 !pv_sha3_file(dst_archive, h, &nbytes) ||
                 vcs_package_build_add_output(&rec, rel_archive, h, nbytes) !=
                     VCS_PACKAGE_BUILD_OK)) {
                fprintf(stderr, "%s: cannot emit %s\n", PV_LOG, dst_archive);
                emitted = false;
            }
            for (size_t i = 0;
                 emitted && i < recipe.public_headers.count; i++) {
                const char *hdr = recipe.public_headers.items[i];
                char rel[VCS_PACKAGE_BUILD_PATH_MAX + 1u];
                pv_header_install_path(&recipe, hdr, rel, sizeof(rel));
                char src_hdr[4300];
                char dst_hdr[4400];
                snprintf(src_hdr, sizeof(src_hdr), "%s/%s", src_root, hdr);
                snprintf(dst_hdr, sizeof(dst_hdr), "%s/%s", emit_dir, rel);
                uint8_t hh[32];
                uint64_t hb = 0;
                if (!pv_copy_file(src_hdr, dst_hdr, 0644) ||
                    !pv_sha3_file(dst_hdr, hh, &hb) ||
                    vcs_package_build_add_output(&rec, rel, hh, hb) !=
                        VCS_PACKAGE_BUILD_OK) {
                    fprintf(stderr, "%s: cannot emit %s\n", PV_LOG, dst_hdr);
                    emitted = false;
                }
            }
            if (!emitted) {
                /* An emit failure is an INTERNAL failure, never a silent
                 * "build failed" verdict: the build genuinely passed. */
                pv_rm_rf(work);
                vcs_package_recipe_free(&recipe);
                vcs_package_manifest_free(&manifest);
                return 5;
            }
        }

        uint8_t *rwire2 = NULL;
        size_t rwire2_len = 0;
        enum vcs_package_build_error be =
            vcs_package_build_serialize(&rec, &rwire2, &rwire2_len);
        char report_path[4400];
        snprintf(report_path, sizeof(report_path), "%s/build-report", emit_dir);
        bool wrote = be == VCS_PACKAGE_BUILD_OK && pv_mkdir_p(emit_dir, 0700) &&
                     pv_atomic_write(report_path, rwire2, rwire2_len);
        free(rwire2);
        if (!wrote) {
            fprintf(stderr, "%s: cannot write %s (%s)\n", PV_LOG, report_path,
                    vcs_package_build_error_string(be));
            pv_rm_rf(work);
            vcs_package_recipe_free(&recipe);
            vcs_package_manifest_free(&manifest);
            return 5;
        }
        if (!pv_rm_rf(work))
            fprintf(stderr, "%s: WARNING: temp tree %s not fully removed\n",
                    PV_LOG, work);
        printf("emit=%s result=%s outputs=%zu isolation=%s\n", emit_dir,
               vcs_package_build_result_string(
                   (enum vcs_package_build_result)rec.result_class),
               rec.output_count,
               vcs_package_build_isolation_string(
                   (enum vcs_package_build_isolation)rec.isolation));
        vcs_package_recipe_free(&recipe);
        vcs_package_manifest_free(&manifest);
        return 0;
    }

    /* Sign + persist. */
    uint8_t attest_id[32] = { 0 };
    enum vcs_package_attest_error aerr =
        vcs_package_attest_id(&att, attest_id);
    if (aerr != VCS_PACKAGE_ATTEST_OK) {
        fprintf(stderr, "%s: internal: attestation invalid: %s\n", PV_LOG,
                vcs_package_attest_error_string(aerr));
        pv_rm_rf(work);
        vcs_package_recipe_free(&recipe);
        vcs_package_manifest_free(&manifest);
        return 5;
    }
    secp256k1_ecdsa_signature esig;
    if (!secp256k1_ecdsa_sign(sign_ctx, &esig, attest_id, secret,
                              secp256k1_nonce_function_rfc6979, NULL)) {
        fprintf(stderr, "%s: internal: ECDSA sign failed\n", PV_LOG);
        pv_rm_rf(work);
        vcs_package_recipe_free(&recipe);
        vcs_package_manifest_free(&manifest);
        return 5;
    }
    secp256k1_ecdsa_signature_serialize_compact(sign_ctx, att.signature,
                                                &esig);
    memory_cleanse(secret, sizeof(secret));
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    aerr = vcs_package_attest_serialize(&att, &wire, &wire_len);
    if (aerr != VCS_PACKAGE_ATTEST_OK) {
        fprintf(stderr, "%s: internal: attestation serialize: %s\n", PV_LOG,
                vcs_package_attest_error_string(aerr));
        pv_rm_rf(work);
        vcs_package_recipe_free(&recipe);
        vcs_package_manifest_free(&manifest);
        return 5;
    }
    char attest_id_hex[65];
    zcl_hex_encode(attest_id, 32, attest_id_hex);
    char dest[4200];
    int dn = snprintf(dest, sizeof(dest), "%s/attestations/%s", store_dir,
                      attest_id_hex);
    bool written =
        dn > 0 && (size_t)dn < sizeof(dest) &&
        pv_atomic_write(dest, wire, wire_len);
    free(wire);
    if (!written) {
        fprintf(stderr, "%s: cannot write %s/attestations/%s\n", PV_LOG,
                store_dir, attest_id_hex);
        pv_rm_rf(work);
        vcs_package_recipe_free(&recipe);
        vcs_package_manifest_free(&manifest);
        return 5;
    }

    /* Produced binaries and objects die with the temp tree — always. */
    if (!pv_rm_rf(work))
        fprintf(stderr, "%s: WARNING: temp tree %s not fully removed\n",
                PV_LOG, work);

    printf("attestation=%s result=%s detail=%s isolation=%s\n",
           attest_id_hex, vcs_package_attest_result_string(att.result_class),
           vcs_package_attest_detail_string(att.detail_code),
           vcs_package_attest_isolation_string(att.isolation));
    vcs_package_recipe_free(&recipe);
    vcs_package_manifest_free(&manifest);
    secp256k1_context_destroy(sign_ctx);
    return 0;
}
