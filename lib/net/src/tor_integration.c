/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tor is compiled INTO zclassic23. No external binary. zclassic23 does not
 * proxy application traffic through SOCKS; dynhost handles .onion requests
 * via direct C callbacks. A localhost-only SocksPort remains as a temporary
 * Tor bootstrap workaround in tor_write_torrc(). */

#define _GNU_SOURCE  /* pthread_timedjoin_np */
#define _DEFAULT_SOURCE
#include "platform/time_compat.h"
#include "net/tor_integration.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "util/safe_alloc.h"
#include "util/log_macros.h"
#include "util/thread_registry.h"

static pthread_t g_tor_thread;
static pthread_t g_monitor_thread;
static _Atomic bool g_tor_running = false;
static _Atomic bool g_tor_ready = false;
static _Atomic bool g_tor_started = false;   /* true once tor thread spawn succeeds */
static _Atomic bool g_tor_thread_done = false; /* true once tor thread returns */
static _Atomic bool g_monitor_started = false;
static char g_onion_address[128];
static char g_tor_datadir[512];

static tor_request_handler_fn g_request_handler = NULL;
static void *g_request_handler_ctx = NULL;

static void tor_join_deadline_from_now(struct timespec *ts, int timeout_sec)
{
    platform_time_realtime_timespec(ts);
    if (timeout_sec < 0)
        timeout_sec = 0;
    ts->tv_sec += timeout_sec;
}

static void tor_join_thread_bounded(pthread_t thread,
                                    const char *name,
                                    int timeout_sec)
{
    struct timespec deadline;
    int rc;

    tor_join_deadline_from_now(&deadline, timeout_sec);
    rc = pthread_timedjoin_np(thread, NULL, &deadline);
    if (rc == 0)
        return;

    if (rc == ETIMEDOUT) {
        fprintf(stderr,  // obs-ok:shutdown-straggler-named
                "Tor: %s join timed out after %ds; detaching\n",
                name ? name : "thread", timeout_sec);
    } else {
        fprintf(stderr,  // obs-ok:shutdown-straggler-named
                "Tor: %s join failed rc=%d (%s); detaching\n",
                name ? name : "thread", rc, strerror(rc));
    }
    pthread_detach(thread);
}

static void ensure_onion_suffix(void)
{
    if (!strstr(g_onion_address, ".onion")) {
        size_t alen = strlen(g_onion_address);
        if (alen + 7 <= sizeof(g_onion_address) - 1)
            memcpy(g_onion_address + alen, ".onion", 7);
    }
}

void tor_integration_set_handler(tor_request_handler_fn handler, void *ctx)
{
    g_request_handler = handler;
    g_request_handler_ctx = ctx;
}

/* Write torrc — we do NOT use SOCKS.
 *
 * Our forked Tor (RhettCreighton/tor, dynhost branch) does NOT use SOCKS.
 * Dynhost handles .onion connections via direct C function calls inside
 * the process — no SOCKS proxy, no proxy clients, nothing.
 *
 * WORKAROUND: Tor's bootstrap code refuses to start without at least
 * one listener. The dynhost service is created AFTER bootstrap, so it
 * can't satisfy this requirement. We open a localhost-only SocksPort
 * that nothing ever connects to, purely to make Tor's startup check
 * happy. The port is derived from p2p_port so multiple instances
 * don't collide. */
bool tor_write_torrc(const char *datadir, uint16_t p2p_port)
{
    char torrc_path[1024];
    snprintf(torrc_path, sizeof(torrc_path), "%s/torrc", datadir);

    FILE *f = fopen(torrc_path, "w");
    if (!f) LOG_FAIL("tor", "failed to open torrc for writing: %s", torrc_path);

    /* Localhost-only SocksPort — NOTHING connects to this.
     * It exists only because Tor won't bootstrap without a listener.
     * Derived from p2p_port to avoid collisions (8033→19999, 8035→20001).
     * When the Tor fork supports SocksPort 0 with dynhost, replace
     * this with "SocksPort 0\n". */
    uint16_t bootstrap_port = (uint16_t)(p2p_port + 11966);
    fprintf(f,
        "SocksPort 127.0.0.1:%u\n"
        "DataDirectory %s/tor_data\n"
        "Log notice file %s/tor.log\n",
        bootstrap_port, datadir, datadir);

    fclose(f);
    return true;
}

/* Read .onion address from persistent hostname file (HiddenServiceDir).
 * Returns true if address was read successfully. */
static bool read_onion_from_hostname_file(const char *datadir)
{
    char path[1024];
    snprintf(path, sizeof(path),
             "%s/tor_data/onion_service/hostname", datadir);

    FILE *f = fopen(path, "r");
    if (!f) return false;  /* not yet available — normal during bootstrap */

    char line[128];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        LOG_FAIL("tor", "hostname file empty: %s", path);
    }
    fclose(f);

    /* Strip trailing whitespace */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'
                       || line[len - 1] == ' '))
        line[--len] = '\0';

    if (len == 0 || len >= sizeof(g_onion_address))
        LOG_FAIL("tor", "hostname file has invalid length: %zu", len);

    memcpy(g_onion_address, line, len + 1);
    ensure_onion_suffix();
    return true;
}

/* Wait for .onion address: check hostname file first (persistent key),
 * fall back to parsing Tor log (ephemeral service). */
static bool read_onion_address(const char *datadir)
{
    char log_path[1024];
    snprintf(log_path, sizeof(log_path), "%s/tor.log", datadir);

    for (int attempt = 0; attempt < 120; attempt++) {
        if (!atomic_load(&g_tor_running))
            return false;

        /* Persistent key: Tor writes hostname file after bootstrap */
        if (read_onion_from_hostname_file(datadir))
            return true;

        /* Fallback: parse ephemeral address from dynhost log */
        FILE *f = fopen(log_path, "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                char *p = strstr(line,
                    "ephemeral service created with address: ");
                if (p) {
                    p += strlen("ephemeral service created with address: ");
                    char *end = p;
                    while (*end && *end != '\n' && *end != '\r' && *end != ' ')
                        end++;
                    size_t len = (size_t)(end - p);
                    if (len > 0 && len < sizeof(g_onion_address)) {
                        memcpy(g_onion_address, p, len);
                        g_onion_address[len] = '\0';
                        ensure_onion_suffix();
                        fclose(f);
                        return true;
                    }
                }
            }
            fclose(f);
        }
        sleep(1);
    }
    LOG_FAIL("tor", "timed out waiting for .onion address after 120 attempts");
}

/* Tor embedding API */
typedef struct tor_main_configuration_t tor_main_configuration_t;
extern tor_main_configuration_t *tor_main_configuration_new(void);
extern int tor_main_configuration_set_command_line(
    tor_main_configuration_t *cfg, int argc, char *argv[]);
extern void tor_main_configuration_free(tor_main_configuration_t *cfg);
extern int tor_run_main(const tor_main_configuration_t *);
extern void tor_shutdown_event_loop_and_exit(int exitcode);

/* Dynhost external handler — routes .onion requests to our code */
typedef size_t (*dynhost_external_handler_fn)(const char *, const char *,
    const uint8_t *, size_t, uint8_t *, size_t, void *);
extern void dynhost_webserver_set_external_handler(
    dynhost_external_handler_fn handler, void *ctx);

/* Bridge: dynhost calls this → we call the registered handler */
static size_t dynhost_bridge(const char *method, const char *path,
                              const uint8_t *body, size_t body_len,
                              uint8_t *response, size_t response_max,
                              void *ctx)
{
    (void)ctx;
    if (!g_request_handler) return 0;
    return g_request_handler(method, path, body, body_len,
                              response, response_max,
                              g_request_handler_ctx);
}

static void *tor_onion_monitor(void *arg);

static void *tor_thread_fn(void *arg)
{
    (void)arg;
    char torrc_path[1024];
    snprintf(torrc_path, sizeof(torrc_path), "%s/torrc", g_tor_datadir);

    printf("Tor: starting embedded (no ports, no SOCKS, dynhost only)\n");
    fflush(stdout);

    /* Monitor for .onion address in parallel when the helper thread starts. */
    if (thread_registry_spawn_ex("zcl_tor_monitor", tor_onion_monitor, NULL,
                                  &g_monitor_thread) == 0) {
        atomic_store(&g_monitor_started, true);
    } else {
        perror("Tor: thread_registry_spawn_ex onion monitor");
        atomic_store(&g_monitor_started, false);
    }

    /* Run Tor in this thread (blocks until exit) */
    tor_main_configuration_t *cfg = tor_main_configuration_new();
    char *argv[] = {"tor", "-f", torrc_path};
    tor_main_configuration_set_command_line(cfg, 3, argv);
    int result = tor_run_main(cfg);
    tor_main_configuration_free(cfg);

    /* Signal monitor to stop, then join it if it actually started. */
    atomic_store(&g_tor_running, false);
    atomic_store(&g_tor_ready, false);
    if (atomic_exchange(&g_monitor_started, false))
        tor_join_thread_bounded(g_monitor_thread, "monitor", 5);

    printf("Tor: exited with code %d\n", result);
    atomic_store(&g_tor_thread_done, true);
    return NULL;
}

static void *tor_onion_monitor(void *arg)
{
    (void)arg;
    if (read_onion_address(g_tor_datadir)) {
        atomic_store(&g_tor_ready, true);

        /* Propagate address to onion service layer */
        extern void onion_service_set_address(const char *);
        onion_service_set_address(g_onion_address);

        printf("Tor .onion: %s\n", g_onion_address);
        fflush(stdout);
    } else {
        if (atomic_load(&g_tor_running))
            fprintf(stderr, "Tor: timed out waiting for .onion\n");
    }
    return NULL;
}

bool tor_integration_start(const char *datadir, uint16_t p2p_port)
{
    if (atomic_load(&g_tor_running))
        return true;

    snprintf(g_tor_datadir, sizeof(g_tor_datadir), "%s", datadir);
    g_onion_address[0] = '\0';

    char path[1024];
    snprintf(path, sizeof(path), "%s/tor_data", datadir);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/tor_data/onion_service", datadir);
    mkdir(path, 0700);

    /* Remove stale lock file from previous session. When the node is
     * killed (SIGTERM from systemctl), Tor may not clean up its lock.
     * Safe to remove: we're the only process that uses this tor_data. */
    snprintf(path, sizeof(path), "%s/tor_data/lock", datadir);
    unlink(path);

    if (!tor_write_torrc(datadir, p2p_port))
        LOG_FAIL("tor", "failed to write torrc to %s", datadir);

    /* Register our handler with Tor's dynhost before starting.
     * All .onion HTTP requests will route through dynhost_bridge →
     * g_request_handler → onion_service_handle_request. */
    if (g_request_handler) {
        dynhost_webserver_set_external_handler(dynhost_bridge, NULL);
        printf("Tor: external handler registered for .onion requests\n");
    }

    atomic_store(&g_tor_running, true);
    atomic_store(&g_tor_thread_done, false);
    atomic_store(&g_monitor_started, false);

    if (thread_registry_spawn_ex("zcl_tor", tor_thread_fn, NULL,
                                  &g_tor_thread) != 0) {
        atomic_store(&g_tor_running, false);
        LOG_FAIL("tor", "thread_registry_spawn_ex failed for tor thread");
    }
    atomic_store(&g_tor_started, true);
    return true;
}

void tor_integration_stop(void)
{
    if (!atomic_exchange(&g_tor_started, false))
        return; /* Never started or already stopped */

    atomic_store(&g_tor_running, false);
    atomic_store(&g_tor_ready, false);

    /* Tell Tor's event loop to exit. Retry briefly in case Tor
     * hasn't entered its main loop yet when we first call. */
    for (int i = 0; i < 50; i++) {
        tor_shutdown_event_loop_and_exit(0);
        if (atomic_load(&g_tor_thread_done))
            break;
        usleep(100000); /* 100ms, up to 5s total */
    }

    tor_join_thread_bounded(g_tor_thread, "main", 5);
    atomic_store(&g_tor_thread_done, false);
}

const char *tor_integration_get_onion_address(void)
{
    if (!atomic_load(&g_tor_ready))
        return NULL;  /* not ready yet — normal during startup */
    return g_onion_address;
}

bool tor_integration_is_ready(void)
{
    return atomic_load(&g_tor_ready);
}

bool tor_integration_is_enabled(void)
{
    return atomic_load(&g_tor_running);
}

/* ── Outbound .onion fetch ─────────────────────────────────── */

/* Weak reference to dynhost_client_fetch — resolved at link time.
 * When linked against libtor_stub.a, this is NULL. */
extern int dynhost_client_fetch(const char *, uint16_t, const char *,
    void (*)(int, const uint8_t *, size_t, void *), void *, int)
    __attribute__((weak));

int tor_integration_fetch_onion(const char *onion_address,
                                 const char *path,
                                 tor_fetch_callback_fn callback,
                                 void *ctx,
                                 int timeout_secs)
{
    if (!dynhost_client_fetch)
        LOG_ERR("tor", "dynhost_client_fetch not linked (stub build)");
    if (!atomic_load(&g_tor_running))
        LOG_ERR("tor", "fetch_onion called but Tor not running");

    return dynhost_client_fetch(onion_address, 80, path,
        (void (*)(int, const uint8_t *, size_t, void *))callback,
        ctx, timeout_secs);
}

/* Callback for blocking fetch — sets result and signals completion */
static void blocking_fetch_cb(int status, const uint8_t *body,
                                size_t body_len, void *ctx)
{
    struct onion_fetch_result *r = (struct onion_fetch_result *)ctx;
    r->status = status;
    if (body && body_len > 0) {
        r->body = zcl_malloc(body_len + 1, "onion_fetch_body");
        if (r->body) {
            memcpy(r->body, body, body_len);
            r->body[body_len] = '\0';
            r->body_len = body_len;
        }
    }
    atomic_store(&r->complete, status >= 200 ? 1 : -1);
}

int tor_integration_fetch_onion_blocking(const char *onion_address,
                                          const char *path,
                                          struct onion_fetch_result *result,
                                          int timeout_secs)
{
    if (!result) LOG_ERR("tor", "fetch_onion_blocking called with NULL result");
    memset(result, 0, sizeof(*result));

    int rc = tor_integration_fetch_onion(onion_address, path,
                                          blocking_fetch_cb, result,
                                          timeout_secs);
    if (rc < 0) {
        atomic_store(&result->complete, -1);
        LOG_ERR("tor", "fetch_onion failed for %s%s", onion_address, path);
    }

    /* Poll for completion */
    int wait_ms = (timeout_secs > 0 ? timeout_secs : 60) * 1000;
    for (int elapsed = 0; elapsed < wait_ms; elapsed += 100) {
        int c = atomic_load(&result->complete);
        if (c != 0) return (c == 1) ? 0 : -1;
        usleep(100000); /* 100ms */
    }

    /* Timeout */
    atomic_store(&result->complete, -1);
    LOG_ERR("tor", "fetch_onion_blocking timed out after %ds for %s%s",
            timeout_secs > 0 ? timeout_secs : 60, onion_address, path);
}
