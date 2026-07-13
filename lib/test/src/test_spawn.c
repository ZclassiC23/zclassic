/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for lib/util/spawn.{h,c} — no-shell process-spawn primitives
 * (zcl_spawn_detached, zcl_spawn_capture). */

#include "test/test_helpers.h"
#include "platform/time_compat.h"
#include "util/spawn.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static bool spawn_contains(const char *hay, const char *needle)
{
    return hay && needle && strstr(hay, needle) != NULL;
}

/* Case 1: detached run execs a real binary that writes to a log file, the
 * log ends up containing the expected output, and NO zombie child of this
 * process is left behind (the grandchild is reparented to init/subreaper;
 * the intermediate child was reaped inside zcl_spawn_detached). */
static int test_spawn_detached_writes_log_no_zombie(void)
{
    int failures = 0;
    TEST("spawn: detached run writes log, leaves no zombie") {
        char log_path[128];
        snprintf(log_path, sizeof(log_path),
                 "/tmp/test_spawn_detached_%d.log", (int)getpid());
        unlink(log_path);

        const char *argv[] = { "/bin/echo",
                               "hello-from-zcl-spawn-detached", NULL };
        struct zcl_result r = zcl_spawn_detached(argv, log_path);
        ASSERT(r.ok);

        /* The grandchild runs asynchronously; poll briefly for its output. */
        char buf[256] = {0};
        bool got = false;
        for (int i = 0; i < 100 && !got; i++) {   /* up to ~2s */
            FILE *f = fopen(log_path, "r");
            if (f) {
                size_t n = fread(buf, 1, sizeof(buf) - 1, f);
                buf[n] = '\0';
                fclose(f);
                if (n > 0) got = true;
            }
            if (!got) nanosleep(&(struct timespec){ .tv_nsec = 20 * 1000 * 1000 }, NULL);
        }
        ASSERT(got);
        ASSERT(spawn_contains(buf, "hello-from-zcl-spawn-detached"));

        /* No child of THIS process should remain: the intermediate child
         * was reaped internally, and the grandchild is not our child. */
        int st = 0;
        pid_t w = waitpid(-1, &st, WNOHANG);
        ASSERT(w == -1 && errno == ECHILD);

        unlink(log_path);
        PASS();
    } _test_next:;
    return failures;
}

/* Case 2: capture returns the child's stdout. */
static int test_spawn_capture_echo(void)
{
    int failures = 0;
    TEST("spawn: capture returns argv echo output") {
        const char *argv[] = { "/bin/echo", "hello-spawn-capture", NULL };
        char buf[256] = {0};
        int rc = zcl_spawn_capture(argv, buf, sizeof(buf), 3000);
        ASSERT(rc == 0);
        ASSERT(spawn_contains(buf, "hello-spawn-capture"));
        PASS();
    } _test_next:;
    return failures;
}

/* Case 3: capture timeout kills a sleeping child within the budget. */
static int test_spawn_capture_timeout_kills(void)
{
    int failures = 0;
    TEST("spawn: capture timeout SIGKILLs a sleeping child") {
        const char *argv[] = { "/bin/sleep", "5", NULL };
        char buf[64] = {0};
        int64_t t0 = platform_time_monotonic_ms();
        int rc = zcl_spawn_capture(argv, buf, sizeof(buf), 200);
        int64_t elapsed = platform_time_monotonic_ms() - t0;
        ASSERT(elapsed < 1500);          /* generous over the 200ms budget */
        ASSERT(rc == 128 + SIGKILL);     /* killed, not a clean exit */
        PASS();
    } _test_next:;
    return failures;
}

/* Case 4: capture is ECHILD-tolerant — under a process-wide SA_NOCLDWAIT
 * SIGCHLD disposition (exactly as alerts.c:287-291 installs), waitpid()
 * fails ECHILD internally, yet the captured output is still valid and the
 * documented "exit status unknown -> 0" contract holds. The SIGCHLD
 * disposition is restored BEFORE any ASSERT (ASSERT does goto _test_next on
 * failure), so a failing assert cannot leave the disposition altered. */
static int test_spawn_capture_echild_tolerant(void)
{
    int failures = 0;
    TEST("spawn: capture tolerates ECHILD under SA_NOCLDWAIT") {
        struct sigaction old;
        memset(&old, 0, sizeof(old));
        sigaction(SIGCHLD, NULL, &old);

        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        sa.sa_flags = SA_NOCLDWAIT;
        sigaction(SIGCHLD, &sa, NULL);

        const char *argv[] = { "/bin/echo", "echild-marker", NULL };
        char buf[256] = {0};
        int rc = zcl_spawn_capture(argv, buf, sizeof(buf), 3000);

        /* Restore BEFORE asserting — see comment above. */
        sigaction(SIGCHLD, &old, NULL);

        ASSERT(rc == 0);
        ASSERT(spawn_contains(buf, "echild-marker"));
        PASS();
    } _test_next:;
    return failures;
}

/* Case 5: oversized output truncates cleanly at cap without deadlocking.
 * The child writes ~100 KB (well past a 64 KB pipe buffer), so the parent's
 * drain-and-discard path is exercised; only cap-1 bytes are retained and the
 * buffer stays NUL-terminated. Completion is via EOF-drain (well under the
 * timeout), not the kill path. (100 KB stays under Linux's per-argv-element
 * MAX_ARG_STRLEN limit of 128 KB, while still exceeding the pipe buffer.) */
static int test_spawn_capture_truncates_oversized(void)
{
    int failures = 0;
    TEST("spawn: capture truncates oversized output cleanly") {
        static char big[100000];
        memset(big, 'A', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';

        const char *argv[] = { "/bin/echo", big, NULL };
        char buf[100] = {0};
        int64_t t0 = platform_time_monotonic_ms();
        int rc = zcl_spawn_capture(argv, buf, sizeof(buf), 5000);
        int64_t elapsed = platform_time_monotonic_ms() - t0;

        ASSERT(rc == 0);
        ASSERT(strlen(buf) == sizeof(buf) - 1);   /* exactly 99 bytes */
        ASSERT(buf[sizeof(buf) - 1] == '\0');
        bool all_a = true;
        for (size_t i = 0; i < sizeof(buf) - 1; i++)
            if (buf[i] != 'A') { all_a = false; break; }
        ASSERT(all_a);
        ASSERT(elapsed < 2000);   /* drained via EOF, not the 5s timeout */
        PASS();
    } _test_next:;
    return failures;
}

/* Case 6: with the DEFAULT SIGCHLD disposition (no SA_NOCLDWAIT — exactly the
 * process state after alerts.c stopped installing it as the LAST step of
 * os-substrate Rung 0), zcl_spawn_capture()'s internal waitpid() returns a
 * REAL exit code: /bin/false exits 1, /bin/true exits 0. This is the proof
 * that removing the process-wide SA_NOCLDWAIT install made child exit statuses
 * trustworthy again (under SA_NOCLDWAIT both would collapse to the documented
 * "unknown -> 0"). SIGCHLD is saved/restored around the test. */
static int test_spawn_capture_real_exit_code(void)
{
    int failures = 0;
    TEST("spawn: capture returns real exit codes without SA_NOCLDWAIT") {
        struct sigaction old;
        memset(&old, 0, sizeof(old));
        sigaction(SIGCHLD, NULL, &old);

        struct sigaction dfl;
        memset(&dfl, 0, sizeof(dfl));
        dfl.sa_handler = SIG_DFL;      /* NO SA_NOCLDWAIT */
        sigaction(SIGCHLD, &dfl, NULL);

        char buf[64] = {0};
        const char *false_argv[] = { "/bin/false", NULL };
        int rc_false = zcl_spawn_capture(false_argv, buf, sizeof(buf), 3000);
        const char *true_argv[] = { "/bin/true", NULL };
        int rc_true = zcl_spawn_capture(true_argv, buf, sizeof(buf), 3000);

        sigaction(SIGCHLD, &old, NULL);   /* restore BEFORE asserting */

        ASSERT(rc_false == 1);   /* real non-zero exit, not ECHILD -> 0 */
        ASSERT(rc_true == 0);
        PASS();
    } _test_next:;
    return failures;
}

int test_spawn(void);

int test_spawn(void)
{
    int failures = 0;
    printf("\n=== Spawn Tests ===\n");

    failures += test_spawn_detached_writes_log_no_zombie();
    failures += test_spawn_capture_echo();
    failures += test_spawn_capture_timeout_kills();
    failures += test_spawn_capture_echild_tolerant();
    failures += test_spawn_capture_truncates_oversized();
    failures += test_spawn_capture_real_exit_code();

    printf("Spawn: %d failures\n", failures);
    return failures;
}
