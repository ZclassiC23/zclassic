/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Live self-backtrace surface. See self_backtrace.h for the contract and the
 * rationale (perf/ptrace are blocked on hardened hosts). The SIGRTMIN+2
 * handler is strictly async-signal-safe: it only calls backtrace(),
 * backtrace_symbols_fd(), write(2) (via util/async_safe_write.h), and
 * sem_post() — all documented async-signal-safe. No malloc, stdio, or locks
 * run inside the handler. */

#define _GNU_SOURCE

#include "util/self_backtrace.h"
#include "util/async_safe_write.h"
#include "util/thread_registry.h"
#include "util/util.h"       /* GetDataDir */
#include "util/log_macros.h"
#include "json/json.h"

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>

/* The realtime signal the dump orchestrator raises on each target thread.
 * SIGRTMIN is a glibc runtime value, so it can't be a static initializer or a
 * switch label — compute it at use. Offset +2 to sit clear of libraries that
 * grab SIGRTMIN / SIGRTMIN+1 (e.g. some timer/thread-cancel implementations). */
static int self_bt_signo(void) { return SIGRTMIN + 2; }

/* Shared handoff to the signal handler. The dump is serialized one thread at a
 * time, so a single static slot suffices. `fd` and `name` are published with
 * release/consumed with acquire so the handler observes the orchestrator's
 * stores across the signal boundary. */
static struct {
    _Atomic int          fd;    /* target log fd, or -1 when idle */
    _Atomic(const char *) name; /* registered name of the current target */
    sem_t                done;  /* posted by the handler when it finishes */
} g_bt;

static _Atomic bool g_installed = false;

/* Last-dump introspection (dumpstate). Ints are atomic; the path string is
 * guarded by a brief mutex — never touched from the signal handler. */
static pthread_mutex_t  g_last_mu = PTHREAD_MUTEX_INITIALIZER;
static char             g_last_path[4300] = {0};
static _Atomic int      g_last_thread_count = 0;
static _Atomic long     g_last_unix_ts = 0;
static _Atomic long     g_dump_count = 0;

/* ── The async-signal-safe handler ─────────────────────────────────── */
static void self_bt_handler(int sig)
{
    (void)sig;
    int fd = atomic_load_explicit(&g_bt.fd, memory_order_acquire);
    if (fd >= 0) {
        const char *name = atomic_load_explicit(&g_bt.name, memory_order_acquire);
        asw_write_str(fd, "[tid=");
        asw_write_uint(fd, (unsigned long)syscall(SYS_gettid));
        asw_write_str(fd, "] ");
        asw_write_str(fd, name ? name : "?");
        asw_write_str(fd, "\n");
        void *frames[64];
        int n = backtrace(frames, 64);
        backtrace_symbols_fd(frames, n, fd);
        asw_write_str(fd, "\n");
    }
    /* Always post — even if fd was cleared — so the orchestrator never blocks
     * on a stray signal delivery. sem_post is async-signal-safe. */
    sem_post(&g_bt.done);
}

bool self_backtrace_install(void)
{
    if (atomic_load_explicit(&g_installed, memory_order_acquire))
        return true;

    if (sem_init(&g_bt.done, 0, 0) != 0)
        LOG_FAIL("self_backtrace", "sem_init failed: %s", strerror(errno));
    atomic_store_explicit(&g_bt.fd, -1, memory_order_release);
    atomic_store_explicit(&g_bt.name, NULL, memory_order_release);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = self_bt_handler;
    /* SA_RESTART: a target thread's interrupted syscall resumes after the
     * handler, so probing it is minimally disruptive. */
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(self_bt_signo(), &sa, NULL) != 0)
        LOG_FAIL("self_backtrace", "sigaction(SIGRTMIN+2) failed: %s",
                 strerror(errno));

    /* Warm up backtrace() once so its first in-handler call cannot dlopen
     * libgcc from signal context (glibc loads the unwinder lazily). */
    void *warm[4];
    (void)backtrace(warm, 4);

    atomic_store_explicit(&g_installed, true, memory_order_release);
    return true;
}

/* Drain any stale posts left by a previously timed-out (late-waking) target so
 * the next wait only observes the current target's post. */
static void self_bt_drain(void)
{
    while (sem_trywait(&g_bt.done) == 0) { /* discard */ }
}

/* Open <datadir>/backtrace-<ts>.log with O_EXCL, retrying with a -<k> suffix
 * so two dumps in the same second still land in distinct files. Writes the
 * chosen path into path_out. Returns the fd or -1. */
static int self_bt_open_log(char *path_out, size_t cap)
{
    char datadir[4096];
    GetDataDir(false, datadir, sizeof(datadir));
    if (!datadir[0])
        return -1;

    long ts = (long)time(NULL);  // platform-ok:log-filename timestamp, no platform.clock in this diagnostic path
    for (int k = 0; k < 1000; k++) {
        char path[4300];
        if (k == 0)
            snprintf(path, sizeof(path), "%s/backtrace-%ld.log", datadir, ts);
        else
            snprintf(path, sizeof(path), "%s/backtrace-%ld-%d.log",
                     datadir, ts, k);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC,
                      0600);
        if (fd >= 0) {
            if (path_out && cap) snprintf(path_out, cap, "%s", path);
            return fd;
        }
        if (errno != EEXIST)
            return -1;
    }
    return -1;
}

int self_backtrace_dump_all(char *path_out, size_t cap)
{
    if (!atomic_load_explicit(&g_installed, memory_order_acquire))
        LOG_ERR("self_backtrace", "handler not installed; call "
                "self_backtrace_install() at boot first");

    char path[4300] = {0};
    int fd = self_bt_open_log(path, sizeof(path));
    if (fd < 0)
        LOG_ERR("self_backtrace", "could not open backtrace log: %s",
                strerror(errno));

    /* Preamble. */
    asw_write_str(fd, "=== self-backtrace ts=");
    asw_write_uint(fd, (unsigned long)time(NULL));  // platform-ok:async-signal-safe path shares the crash-handler's raw time()
    asw_write_str(fd, " pid=");
    asw_write_uint(fd, (unsigned long)getpid());
    asw_write_str(fd, " ===\n\n");

    int count = 0;

    /* The calling thread dumps itself directly — no signal to self. */
    pthread_t self = pthread_self();
    asw_write_str(fd, "[tid=");
    asw_write_uint(fd, (unsigned long)syscall(SYS_gettid));
    asw_write_str(fd, "] (caller)\n");
    void *frames[64];
    int nf = backtrace(frames, 64);
    backtrace_symbols_fd(frames, nf, fd);
    asw_write_str(fd, "\n");
    count++;

    /* Snapshot the registry, then signal each other thread one at a time. */
    struct thread_registry_view view[ZCL_THREAD_REGISTRY_CAP];
    int nthreads = thread_registry_snapshot(view, ZCL_THREAD_REGISTRY_CAP);

    atomic_store_explicit(&g_bt.fd, fd, memory_order_release);
    for (int i = 0; i < nthreads; i++) {
        if (pthread_equal(view[i].tid, self))
            continue;  /* already dumped as (caller) */

        self_bt_drain();
        atomic_store_explicit(&g_bt.name, view[i].name, memory_order_release);

        int rc = pthread_kill(view[i].tid, self_bt_signo());
        if (rc != 0) {
            /* Thread exited between snapshot and signal. Record and move on. */
            asw_write_str(fd, "[name=");
            asw_write_str(fd, view[i].name);
            asw_write_str(fd, "] <gone>\n\n");
            count++;
            continue;
        }

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);  // platform-ok:sem_timedwait requires a raw CLOCK_REALTIME absolute deadline
        ts.tv_nsec += 100L * 1000L * 1000L;  /* 100 ms budget per thread */
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }

        int w;
        do {
            w = sem_timedwait(&g_bt.done, &ts);
        } while (w != 0 && errno == EINTR);

        if (w != 0) {
            /* Blocked / signal-masked thread: it cannot hang the dump. */
            asw_write_str(fd, "[name=");
            asw_write_str(fd, view[i].name);
            asw_write_str(fd, "] <no response>\n\n");
        }
        count++;
    }
    atomic_store_explicit(&g_bt.fd, -1, memory_order_release);
    atomic_store_explicit(&g_bt.name, NULL, memory_order_release);

    fsync(fd);
    close(fd);

    /* Publish last-dump introspection. */
    pthread_mutex_lock(&g_last_mu);
    snprintf(g_last_path, sizeof(g_last_path), "%s", path);
    pthread_mutex_unlock(&g_last_mu);
    atomic_store_explicit(&g_last_thread_count, count, memory_order_release);
    atomic_store_explicit(&g_last_unix_ts, (long)time(NULL), memory_order_release);  // platform-ok:introspection wall-clock stamp
    atomic_fetch_add_explicit(&g_dump_count, 1, memory_order_relaxed);

    if (path_out && cap) snprintf(path_out, cap, "%s", path);
    return count;
}

bool self_backtrace_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    json_push_kv_bool(out, "installed",
                      atomic_load_explicit(&g_installed, memory_order_acquire));
    json_push_kv_int(out, "dump_count",
                     (int64_t)atomic_load_explicit(&g_dump_count,
                                                   memory_order_relaxed));
    json_push_kv_int(out, "last_thread_count",
                     (int64_t)atomic_load_explicit(&g_last_thread_count,
                                                   memory_order_acquire));
    json_push_kv_int(out, "last_unix_ts",
                     (int64_t)atomic_load_explicit(&g_last_unix_ts,
                                                   memory_order_acquire));
    char path[4300];
    pthread_mutex_lock(&g_last_mu);
    snprintf(path, sizeof(path), "%s", g_last_path);
    pthread_mutex_unlock(&g_last_mu);
    json_push_kv_str(out, "last_path", path);
    return true;
}
