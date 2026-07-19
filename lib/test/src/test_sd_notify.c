/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit tests for the direct-socket systemd notify client
 * (lib/util/src/sd_notify.c). No libsystemd — this exercises the real
 * AF_UNIX datagram wire protocol against a socket this test binds
 * itself, standing in for systemd's NOTIFY_SOCKET.
 *
 * Coverage:
 *   - silent no-op when NOTIFY_SOCKET is unset (init returns false, no
 *     send attempted, no datagram observed)
 *   - path-mode NOTIFY_SOCKET: READY=1 then WATCHDOG=1 datagrams arrive
 *     byte-for-byte on the bound socket; WATCHDOG_USEC round-trips
 *   - abstract-namespace NOTIFY_SOCKET (leading '@'): same protocol,
 *     translated to the Linux abstract-socket wire form (leading NUL)
 *   - the sd_notify_watchdog_ping() health-check gate: a fake
 *     root-health callback reporting unhealthy suppresses the
 *     WATCHDOG=1 send entirely (no datagram observed); reporting
 *     healthy again resumes it
 *
 * Each scenario calls sd_notify_reset_for_testing() first so the
 * module's process-global latch (NOTIFY_SOCKET is read once, matching
 * real systemd semantics) doesn't leak state between scenarios. */

#include "test/test_helpers.h"
#include "util/sd_notify.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SDN_CHECK(name, expr) do { \
    printf("sd_notify: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* Bind a fresh non-blocking AF_UNIX datagram socket at a unique
 * path-mode address under /tmp. Returns the bound fd (>=0) and writes
 * the path into `path_out` (size >= sizeof(struct sockaddr_un.sun_path)).
 * Caller unlinks + closes. */
static int sdn_bind_path_socket(char *path_out, size_t path_out_len)
{
    snprintf(path_out, path_out_len,
             "/tmp/zcl_test_sd_notify_%d_%ld.sock",
             (int)getpid(), (long)time(NULL)); // platform-ok: test-fixture unique path, not production timing
    unlink(path_out);

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path_out, sizeof(sa.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Bind a fresh non-blocking AF_UNIX datagram socket in the Linux
 * abstract namespace (no filesystem entry). Writes the '@'-prefixed
 * NOTIFY_SOCKET-style name (what a caller would export) into
 * `name_out`. Caller only needs to close (no unlink — abstract sockets
 * have no filesystem path to remove). */
static int sdn_bind_abstract_socket(char *name_out, size_t name_out_len)
{
    snprintf(name_out, name_out_len, "@zcl_test_sd_notify_abs_%d",
             (int)getpid());

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    /* Abstract form: leading NUL, then the name bytes (no NUL
     * terminator required on the wire). */
    size_t name_len = strlen(name_out + 1); /* skip the '@' */
    sa.sun_path[0] = '\0';
    memcpy(sa.sun_path + 1, name_out + 1, name_len);
    socklen_t sa_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
                                    + 1 + name_len);
    if (bind(fd, (struct sockaddr *)&sa, sa_len) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Poll fd for one datagram with a short bound (the sender is
 * synchronous local IPC, so this never has to wait for real network
 * latency — a few retries against EAGAIN is enough to absorb scheduler
 * jitter without a real sleep-based race). Returns the byte count read
 * (>=0) or -1 if nothing arrived. */
static ssize_t sdn_try_recv(int fd, char *buf, size_t buf_len)
{
    for (int attempt = 0; attempt < 200; attempt++) {
        ssize_t n = recv(fd, buf, buf_len - 1, 0);
        if (n >= 0) {
            buf[n] = '\0';
            return n;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return -1;
}

/* True iff nothing arrives within the same short polling bound above —
 * used to prove a suppressed send produced NO datagram, not just a
 * different one. */
static bool sdn_confirm_silence(int fd)
{
    char buf[64];
    return sdn_try_recv(fd, buf, sizeof(buf)) < 0;
}

/* ── fake root-health callback used by the gate test ────────────── */
static bool g_fake_health_healthy = true;
static bool fake_health_check(void) { return g_fake_health_healthy; }

int test_sd_notify(void)
{
    int failures = 0;

    /* ── silent no-op without NOTIFY_SOCKET ──────────────────────── */
    {
        sd_notify_reset_for_testing();
        unsetenv("NOTIFY_SOCKET");
        unsetenv("WATCHDOG_USEC");

        SDN_CHECK("init returns false with no NOTIFY_SOCKET",
            !sd_notify_init());
        SDN_CHECK("is_active false with no NOTIFY_SOCKET",
            !sd_notify_is_active());
        SDN_CHECK("ready() is a silent no-op (returns false)",
            !sd_notify_ready());
        SDN_CHECK("watchdog_ping() is a silent no-op (returns false)",
            !sd_notify_watchdog_ping());
        SDN_CHECK("status() is a silent no-op (returns false)",
            !sd_notify_status("hello"));
        SDN_CHECK("watchdog_usec is 0 with nothing configured",
            sd_notify_watchdog_usec() == 0);

        sd_notify_reset_for_testing();
    }

    /* ── path-mode NOTIFY_SOCKET: READY=1 + WATCHDOG=1 arrive ────── */
    {
        char path[108];
        int fd = sdn_bind_path_socket(path, sizeof(path));
        SDN_CHECK("path-mode socket bound", fd >= 0);

        if (fd >= 0) {
            setenv("NOTIFY_SOCKET", path, 1);
            setenv("WATCHDOG_USEC", "60000000", 1); /* 60s */
            sd_notify_reset_for_testing();

            SDN_CHECK("init returns true with NOTIFY_SOCKET set",
                sd_notify_init());
            SDN_CHECK("is_active true after init",
                sd_notify_is_active());
            SDN_CHECK("watchdog_usec round-trips WATCHDOG_USEC",
                sd_notify_watchdog_usec() == 60000000ULL);

            SDN_CHECK("ready() reports success", sd_notify_ready());
            char buf[64];
            ssize_t n = sdn_try_recv(fd, buf, sizeof(buf));
            SDN_CHECK("READY=1 datagram observed on the bound socket",
                n > 0 && strcmp(buf, "READY=1\n") == 0);

            SDN_CHECK("watchdog_ping() reports success (no gate set)",
                sd_notify_watchdog_ping());
            n = sdn_try_recv(fd, buf, sizeof(buf));
            SDN_CHECK("WATCHDOG=1 datagram observed on the bound socket",
                n > 0 && strcmp(buf, "WATCHDOG=1\n") == 0);

            SDN_CHECK("status() delivers a STATUS= line",
                sd_notify_status("h=100 peers=8"));
            n = sdn_try_recv(fd, buf, sizeof(buf));
            SDN_CHECK("STATUS= datagram observed",
                n > 0 && strcmp(buf, "STATUS=h=100 peers=8\n") == 0);

            SDN_CHECK("stopping() delivers STOPPING=1",
                sd_notify_stopping("bye"));
            n = sdn_try_recv(fd, buf, sizeof(buf));
            SDN_CHECK("STOPPING=1 datagram observed",
                n > 0 && strncmp(buf, "STOPPING=1", 10) == 0);

            close(fd);
            unlink(path);
            unsetenv("NOTIFY_SOCKET");
            unsetenv("WATCHDOG_USEC");
            sd_notify_reset_for_testing();
        }
    }

    /* ── abstract-namespace NOTIFY_SOCKET (leading '@') ──────────── */
    {
        char name[64];
        int fd = sdn_bind_abstract_socket(name, sizeof(name));
        SDN_CHECK("abstract-namespace socket bound", fd >= 0);

        if (fd >= 0) {
            setenv("NOTIFY_SOCKET", name, 1);
            sd_notify_reset_for_testing();

            SDN_CHECK("init succeeds with abstract-namespace NOTIFY_SOCKET",
                sd_notify_init());
            SDN_CHECK("watchdog_ping() reaches the abstract socket",
                sd_notify_watchdog_ping());
            char buf[64];
            ssize_t n = sdn_try_recv(fd, buf, sizeof(buf));
            SDN_CHECK("WATCHDOG=1 observed via abstract-namespace socket",
                n > 0 && strcmp(buf, "WATCHDOG=1\n") == 0);

            close(fd);
            unsetenv("NOTIFY_SOCKET");
            sd_notify_reset_for_testing();
        }
    }

    /* ── health-check gate suppresses WATCHDOG=1 when unhealthy ──── */
    {
        char path[108];
        int fd = sdn_bind_path_socket(path, sizeof(path));
        SDN_CHECK("gate-test socket bound", fd >= 0);

        if (fd >= 0) {
            setenv("NOTIFY_SOCKET", path, 1);
            sd_notify_reset_for_testing();
            SDN_CHECK("init succeeds for gate test", sd_notify_init());

            g_fake_health_healthy = false;
            sd_notify_set_health_check(fake_health_check);
            bool ping_rc = sd_notify_watchdog_ping();
            SDN_CHECK("ping() reports failure when the fake gate is unhealthy",
                !ping_rc);
            SDN_CHECK("no WATCHDOG=1 datagram arrives while unhealthy",
                sdn_confirm_silence(fd));

            g_fake_health_healthy = true;
            SDN_CHECK("ping() resumes once the fake gate reports healthy",
                sd_notify_watchdog_ping());
            char buf[64];
            ssize_t n = sdn_try_recv(fd, buf, sizeof(buf));
            SDN_CHECK("WATCHDOG=1 observed once the gate clears",
                n > 0 && strcmp(buf, "WATCHDOG=1\n") == 0);

            /* Clearing the gate (NULL) restores the pre-existing
             * always-allow behavior. */
            sd_notify_set_health_check(NULL);
            SDN_CHECK("ping() still succeeds after clearing the gate",
                sd_notify_watchdog_ping());
            n = sdn_try_recv(fd, buf, sizeof(buf));
            SDN_CHECK("WATCHDOG=1 observed with the gate cleared",
                n > 0 && strcmp(buf, "WATCHDOG=1\n") == 0);

            close(fd);
            unlink(path);
            unsetenv("NOTIFY_SOCKET");
            sd_notify_reset_for_testing();
        }
    }

    return failures;
}
