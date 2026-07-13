/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * spawn — no-shell process-launch primitives. See util/spawn.h for the
 * full contract and the SA_NOCLDWAIT / fork-in-threaded-process notes this
 * implementation depends on. */

#include "util/spawn.h"

#include "platform/time_compat.h"
#include "util/log_macros.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── Shared helpers (parent-side only — never called between fork/exec) ── */

/* Reap `pid`, tolerating ECHILD (SA_NOCLDWAIT — see util/spawn.h). Retries
 * on EINTR. Returns true if a trustworthy exit status was obtained (written
 * to *status), false otherwise (ECHILD or another wait failure). */
static bool spawn_reap(pid_t pid, int *status)
{
    for (;;) {
        pid_t r = waitpid(pid, status, 0);
        if (r == pid) return true;
        if (r < 0 && errno == EINTR) continue;
        return false;   /* ECHILD (SA_NOCLDWAIT) or another wait failure */
    }
}

/* ── zcl_spawn_detached ──────────────────────────────────────────────── */

/* Child-side only: async-signal-safe setup + exec. Never returns on
 * success. On failure, best-effort writes errno to err_fd (if >= 0) and
 * _exit(127). Only async-signal-safe calls happen in this function. */
static void spawn_grandchild_exec(const char *const argv[],
                                   const char *log_path, int err_fd)
{
    int devnull_in = open("/dev/null", O_RDONLY);
    if (devnull_in >= 0) {
        dup2(devnull_in, STDIN_FILENO);
        if (devnull_in > STDERR_FILENO) close(devnull_in);
    }

    int out_fd = log_path ? open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0600)
                          : open("/dev/null", O_WRONLY);
    if (out_fd >= 0) {
        dup2(out_fd, STDOUT_FILENO);
        dup2(out_fd, STDERR_FILENO);
        if (out_fd > STDERR_FILENO) close(out_fd);
    }

    execvp(argv[0], (char *const *)argv);

    /* execvp failed — relay errno to the parent via the CLOEXEC pipe. */
    int e = errno;
    ssize_t written = (err_fd >= 0) ? write(err_fd, &e, sizeof(e)) : 0;
    (void)written;   /* best-effort; nothing else safe to do here */
    _exit(127);
}

struct zcl_result zcl_spawn_detached(const char *const argv[],
                                      const char *log_path)
{
    if (!argv || !argv[0])
        return ZCL_ERR(-1, "zcl_spawn_detached: NULL/empty argv");

    int errpipe[2];
    if (pipe(errpipe) != 0)
        return ZCL_ERR(-errno, "zcl_spawn_detached: pipe() failed: %s",
                       strerror(errno));
    /* Write end must close-on-exec: a successful grandchild exec closes
     * it automatically (its only copy), which is how the parent learns
     * "exec succeeded" (EOF on read) vs "exec failed" (errno bytes
     * arrive first). */
    if (fcntl(errpipe[1], F_SETFD, FD_CLOEXEC) != 0) {
        int e = errno;
        close(errpipe[0]); close(errpipe[1]);
        return ZCL_ERR(-e, "zcl_spawn_detached: fcntl(FD_CLOEXEC) failed: %s",
                       strerror(e));
    }

    pid_t child1 = fork();
    if (child1 < 0) {
        int e = errno;
        close(errpipe[0]); close(errpipe[1]);
        return ZCL_ERR(-e, "zcl_spawn_detached: fork() failed: %s",
                       strerror(e));
    }

    if (child1 == 0) {
        /* First child: become session leader (detach from any controlling
         * tty), then fork the grandchild that actually execs. Only
         * async-signal-safe calls from here to _exit()/exec(). */
        close(errpipe[0]);
        setsid();

        pid_t child2 = fork();
        if (child2 < 0) {
            int e = errno;
            ssize_t written = write(errpipe[1], &e, sizeof(e));
            (void)written;
            _exit(127);
        }
        if (child2 == 0) {
            spawn_grandchild_exec(argv, log_path, errpipe[1]);
            /* unreachable */
        }
        /* Still child1: hand off immediately so the grandchild is
         * reparented to init/subreaper without delay. Do not wait for
         * it — that is the entire point of "detached". */
        close(errpipe[1]);
        _exit(0);
    }

    /* Parent. */
    close(errpipe[1]);   /* close our own copy, else read() below never sees EOF */

    int status = 0;
    spawn_reap(child1, &status);   /* reap the intermediate child; ECHILD-tolerant */

    int child_errno = 0;
    ssize_t n;
    do {
        n = read(errpipe[0], &child_errno, sizeof(child_errno));
    } while (n < 0 && errno == EINTR);
    close(errpipe[0]);

    if (n == (ssize_t)sizeof(child_errno)) {
        return ZCL_ERR(-child_errno,
                       "zcl_spawn_detached: execvp(%s) failed: %s",
                       argv[0], strerror(child_errno));
    }
    if (n < 0) {
        return ZCL_ERR(-errno,
                       "zcl_spawn_detached: read(errpipe) failed: %s",
                       strerror(errno));
    }
    /* n == 0: EOF with no error bytes -> the grandchild's exec succeeded
     * (its CLOEXEC copy of the write end closed as part of exec()). */
    return ZCL_OK;
}

/* ── zcl_spawn_capture ───────────────────────────────────────────────── */

int zcl_spawn_capture(const char *const argv[], char *buf, size_t cap,
                       int timeout_ms)
{
    if (!argv || !argv[0] || !buf || cap == 0)
        LOG_ERR("spawn", "bad args (argv=%p buf=%p cap=%zu)",
                (const void *)argv, (void *)buf, cap);
    buf[0] = '\0';

    int outpipe[2];
    if (pipe(outpipe) != 0)
        LOG_ERR("spawn", "pipe() failed: %s", strerror(errno));

    pid_t pid = fork();
    if (pid < 0) {
        close(outpipe[0]); close(outpipe[1]);
        LOG_ERR("spawn", "fork() failed: %s", strerror(errno));
    }

    if (pid == 0) {
        /* Child: only async-signal-safe calls until exec/_exit. */
        close(outpipe[0]);
        dup2(outpipe[1], STDOUT_FILENO);
        if (outpipe[1] != STDOUT_FILENO) close(outpipe[1]);

        int devnull_in = open("/dev/null", O_RDONLY);
        if (devnull_in >= 0) {
            dup2(devnull_in, STDIN_FILENO);
            if (devnull_in > STDERR_FILENO) close(devnull_in);
        }
        int devnull_err = open("/dev/null", O_WRONLY);
        if (devnull_err >= 0) {
            dup2(devnull_err, STDERR_FILENO);
            if (devnull_err > STDERR_FILENO) close(devnull_err);
        }

        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    /* Parent. */
    close(outpipe[1]);

    size_t used = 0;
    char discard[4096];
    int64_t deadline_ms = (timeout_ms > 0)
                          ? platform_time_monotonic_ms() + timeout_ms : 0;
    bool timed_out = false;

    for (;;) {
        struct pollfd pfd = { .fd = outpipe[0], .events = POLLIN };
        int poll_timeout = -1;
        if (timeout_ms > 0) {
            int64_t remain = deadline_ms - platform_time_monotonic_ms();
            if (remain <= 0) { timed_out = true; break; }
            poll_timeout = (remain > INT_MAX) ? INT_MAX : (int)remain;
        }
        int pr = poll(&pfd, 1, poll_timeout);
        if (pr < 0) {
            if (errno == EINTR) continue;
            LOG_WARN("spawn", "poll() failed: %s", strerror(errno));
            break;
        }
        if (pr == 0) { timed_out = true; break; }   /* only when timeout_ms > 0 */

        char *dst = (used < cap - 1) ? buf + used : discard;
        size_t dst_cap = (used < cap - 1) ? (cap - 1 - used) : sizeof(discard);
        ssize_t n = read(outpipe[0], dst, dst_cap);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_WARN("spawn", "read() failed: %s", strerror(errno));
            break;
        }
        if (n == 0) break;   /* EOF: child closed its stdout */
        if (used < cap - 1) used += (size_t)n;
    }
    buf[used] = '\0';
    close(outpipe[0]);

    if (timed_out)
        kill(pid, SIGKILL);

    int status = 0;
    if (!spawn_reap(pid, &status)) {
        /* ECHILD (SA_NOCLDWAIT) or another wait failure: the output
         * already captured above is still valid; exit status is simply
         * unknown — documented contract in util/spawn.h. */
        return 0;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 0;
}

/* ── zcl_argv_split ──────────────────────────────────────────────────── */

size_t zcl_argv_split(char *str, const char *argv[], size_t max)
{
    if (!argv || max == 0)
        return 0;
    size_t n = 0;
    if (str) {
        char *save = NULL;
        for (char *tok = strtok_r(str, " \t\r\n", &save);
             tok && n < max - 1;
             tok = strtok_r(NULL, " \t\r\n", &save))
            argv[n++] = tok;
    }
    argv[n] = NULL;
    return n;
}
