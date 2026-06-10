/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Async-signal-safe fatal-signal handler. See signal_handler.h. */

#define _GNU_SOURCE
#include "util/signal_handler.h"

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>

static signal_handler_crash_hook_fn g_crash_hook = NULL;
static void *g_crash_hook_ctx = NULL;
static volatile sig_atomic_t g_crash_hook_running = 0;

/* Durable, append-only crash log (best-effort). Opened once the datadir is
 * known via signal_handler_set_crash_log(). Both this module's handler and
 * the event-log crash handler mirror their backtrace here and fsync, so a
 * crash leaves a forensic record even when stderr routing is lost — the gap
 * that swallowed 6 SEGV backtraces on 2026-05-30. */
static volatile int g_crash_fd = -1;

void signal_handler_set_crash_log(const char *path)
{
    if (g_crash_fd >= 0 || !path || !*path) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0) g_crash_fd = fd;
}

int signal_handler_crash_log_fd(void)
{
    return g_crash_fd;
}

void signal_handler_set_crash_hook(signal_handler_crash_hook_fn fn,
                                   void *ctx)
{
    g_crash_hook_ctx = ctx;
    g_crash_hook = fn;
}

void signal_handler_clear_crash_hook(void)
{
    g_crash_hook = NULL;
    g_crash_hook_ctx = NULL;
    g_crash_hook_running = 0;
}

void signal_handler_run_crash_hook(int sig, siginfo_t *info, void *ucontext)
{
    signal_handler_crash_hook_fn fn = g_crash_hook;
    if (!fn || g_crash_hook_running) return;
    g_crash_hook_running = 1;
    fn(sig, info, ucontext, g_crash_hook_ctx);
    g_crash_hook_running = 0;
}

/* Async-signal-safe unsigned-decimal writer. Returns bytes written. */
static int write_uint(int fd, unsigned long v)
{
    char buf[32];
    int n = 0;
    if (v == 0) { buf[n++] = '0'; }
    while (v > 0) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    /* reverse */
    for (int i = 0; i < n / 2; i++) {
        char t = buf[i]; buf[i] = buf[n - 1 - i]; buf[n - 1 - i] = t;
    }
    return (int)write(fd, buf, (size_t)n);
}

/* Async-signal-safe hex writer (lowercase, no 0x prefix). */
static int write_hex(int fd, unsigned long v)
{
    static const char H[] = "0123456789abcdef";
    char buf[18];
    int n = 0;
    if (v == 0) { buf[n++] = '0'; }
    while (v > 0) { buf[n++] = H[v & 0xF]; v >>= 4; }
    for (int i = 0; i < n / 2; i++) {
        char t = buf[i]; buf[i] = buf[n - 1 - i]; buf[n - 1 - i] = t;
    }
    return (int)write(fd, buf, (size_t)n);
}

static int write_s(int fd, const char *s)
{
    return (int)write(fd, s, strlen(s));
}

/* Emit the marker + backtrace to one fd. Async-signal-safe throughout. */
static void emit_report(int fd, int sig, siginfo_t *info,
                        void *const *frames, int nframes)
{
    /* [fatal-signal] sig=N code=M addr=0x... pid=P tid=T time=T */
    write_s(fd, "[fatal-signal] sig=");
    write_uint(fd, (unsigned long)sig);
    write_s(fd, " code=");
    write_uint(fd, (unsigned long)(info ? info->si_code : 0));
    write_s(fd, " addr=0x");
    write_hex(fd, info ? (unsigned long)(uintptr_t)info->si_addr : 0UL);
    write_s(fd, " pid=");
    write_uint(fd, (unsigned long)getpid());
    write_s(fd, " tid=");
    write_uint(fd, (unsigned long)syscall(SYS_gettid));
    write_s(fd, " time=");
    write_uint(fd, (unsigned long)time(NULL));  // platform-ok:async-signal-safe-crash-handler (platform.clock may lock)
    write_s(fd, "\n");
    backtrace_symbols_fd(frames, nframes, fd);
    write_s(fd, "[fatal-signal] end\n");
}

/* The handler itself. SA_SIGINFO style. */
static void fatal_handler(int sig, siginfo_t *info, void *ucontext)
{
    signal_handler_run_crash_hook(sig, info, ucontext);

    /* Backtrace — up to 64 frames. backtrace + backtrace_symbols_fd are
     * documented async-signal-safe (glibc allocates internal buffers
     * lazily but uses mmap, not malloc, on the hot path). Capture once,
     * emit to stderr AND the durable crash log. */
    void *frames[64];
    int n = backtrace(frames, 64);

    emit_report(STDERR_FILENO, sig, info, frames, n);
    if (g_crash_fd >= 0) {
        emit_report(g_crash_fd, sig, info, frames, n);
        fsync(g_crash_fd);  /* survive even if the process dies hard next */
    }

    /* Restore default handler and re-raise so:
     *   - systemd still reports the original status code (e.g. 134),
     *   - the kernel still produces a core file if RLIMIT_CORE permits,
     *   - any parent process / debugger sees the real signal. */
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigaction(sig, &dfl, NULL);
    raise(sig);
}

int signal_handler_install(void)
{
    /* Alternate signal stack so a stack-overflow SIGSEGV (which exhausts the
     * thread stack) can still run the handler instead of silently dying. */
    static char alt_stack[64 * 1024];
    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = alt_stack;
    ss.ss_size = sizeof(alt_stack);
    ss.ss_flags = 0;
    if (sigaltstack(&ss, NULL) != 0) return -1;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fatal_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    const int sigs[] = { SIGABRT, SIGSEGV, SIGBUS, SIGFPE };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        if (sigaction(sigs[i], &sa, NULL) != 0) return -1;
    }
    return 0;
}
