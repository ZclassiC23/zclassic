/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#define _GNU_SOURCE
#include "devloop.h"

#include "platform/time_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef ZCL_DEV_BUILD
static void capture_tail(struct zcl_devloop_process_result *out,
                         const char *data, size_t len)
{
    const size_t cap = sizeof(out->output) - 1;
    if (len >= cap) {
        memcpy(out->output, data + len - cap, cap);
        out->output_len = cap;
    } else {
        size_t overflow = out->output_len + len > cap
            ? out->output_len + len - cap : 0;
        if (overflow > 0) {
            memmove(out->output, out->output + overflow,
                    out->output_len - overflow);
            out->output_len -= overflow;
        }
        memcpy(out->output + out->output_len, data, len);
        out->output_len += len;
    }
    out->output[out->output_len] = 0;
}

static void drain_output(int fd, struct zcl_devloop_process_result *out)
{
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            capture_tail(out, buf, (size_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
}
#endif

bool zcl_devloop_process_run(const char *cwd,
                             const char *const argv[],
                             int timeout_ms,
                             struct zcl_devloop_process_result *out)
{
    if (!cwd || !cwd[0] || !argv || !argv[0] || !out || timeout_ms <= 0) {
        fprintf(stderr, "[devloop] process: invalid bounded invocation\n");
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->exit_code = -1;

#ifndef ZCL_DEV_BUILD
    fprintf(stderr, "[devloop] process execution is disabled outside a dev build\n");
    return false;
#else
    int fds[2];
    if (pipe(fds) != 0) {
        fprintf(stderr, "[devloop] process: pipe failed: %s\n",
                strerror(errno));
        return false;
    }
    (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    int64_t started_us = platform_time_monotonic_us();
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[devloop] process: fork failed: %s\n",
                strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        (void)setpgid(0, 0);
        close(fds[0]);
        if (chdir(cwd) != 0)
            _exit(126);
        if (dup2(fds[1], STDOUT_FILENO) < 0 ||
            dup2(fds[1], STDERR_FILENO) < 0)
            _exit(126);
        close(fds[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    close(fds[1]);
    int flags = fcntl(fds[0], F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

    int status = 0;
    bool finished = false;
    int64_t deadline_us = started_us + (int64_t)timeout_ms * 1000;
    while (!finished) {
        drain_output(fds[0], out);
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            finished = true;
            break;
        }
        if (waited < 0 && errno != EINTR) {
            fprintf(stderr, "[devloop] process: waitpid failed for %s: %s\n",
                    argv[0], strerror(errno));
            (void)kill(-pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
            close(fds[0]);
            return false;
        }
        if (platform_time_monotonic_us() >= deadline_us) {
            out->timed_out = true;
            (void)kill(-pid, SIGTERM);
            for (int i = 0; i < 20; i++) {
                if (waitpid(pid, &status, WNOHANG) == pid) {
                    finished = true;
                    break;
                }
                usleep(10000);
            }
            if (!finished) {
                (void)kill(-pid, SIGKILL);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                finished = true;
            }
            break;
        }
        struct pollfd pfd = { .fd = fds[0], .events = POLLIN };
        (void)poll(&pfd, 1, 25);
    }
    drain_output(fds[0], out);
    close(fds[0]);

    if (WIFEXITED(status))
        out->exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        out->term_signal = WTERMSIG(status);
    out->elapsed_ms = (platform_time_monotonic_us() - started_us) / 1000;
    return true;
#endif
}
