/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MVP criterion #2 CI gate: Tor onion bootstrap in <60s.
 *
 * Boots the same tor_integration path the main node uses
 * (config/src/boot_services.c:1303-1316) into a temp datadir, polls
 * `tor_integration_is_ready()` at 1Hz for up to 90 seconds, and
 * asserts the ready flag flips true within 60 seconds.  Also asserts
 * the reported .onion address is a well-formed v3 hidden service
 * name (56 lowercase base32 chars + ".onion").
 *
 * Gating
 * ------
 * Skipped unless the caller sets `ZCL_STRESS_TESTS=1`.  Reasons:
 *   - Real bootstrap takes 10-40s (cold) to ~30s (warm), ~1000x the
 *     sub-second budget the default `make test` suite assumes.
 *   - Requires outbound network access to Tor directory authorities;
 *     sandboxed CI environments without outbound will always fail.
 *   - Touches the vendored Tor pthread — the rest of make test only
 *     exercises torrc generation + address propagation (test_tor.c).
 *
 * Invocation:
 *   ZCL_STRESS_TESTS=1 build/bin/test_zcl
 *   ZCL_STRESS_TESTS=1 ZCL_TEST_ONLY=onion build/bin/test_zcl  (focused run)
 *
 * MVP linkage: flips `MVP.md` criterion #2 from ☐ to ✅.  Forward-
 * looking CI gate — not RED-first (no failing branch existed when
 * it was written).
 *
 * Isolation
 * ---------
 * Uses p2p_port = 18033 → bootstrap SocksPort 29999 (127.0.0.1), so
 * a concurrently-running production node on 8033/19999 does not
 * collide.  Datadir is an `mkdtemp` template under the CWD and is
 * `rm -rf`'d on exit, pass or fail.  The tor_integration static
 * state is process-local; stopping at end restores the same initial
 * state that `test_tor_initial_state` observed at boot.
 */

#include "platform/time_compat.h"
#include "test/test_helpers.h"
#include "net/tor_integration.h"
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>

/* Recursively remove a directory tree (rm -rf).  Local copy to avoid
 * leaking a `remove_tree` symbol across translation units — test_tor.c
 * has its own static version. */
static void p11_remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
            p11_remove_tree(child);
        else
            unlink(child);
    }
    closedir(d);
    rmdir(path);
}

/* v3 hidden service names are 56 base32 chars + ".onion" = 62 total.
 * RFC 4648 base32 alphabet, lowercase-only in .onion addresses:
 *   a-z | 2-7 */
static bool is_valid_onion_v3(const char *addr)
{
    if (!addr) return false;
    size_t len = strlen(addr);
    if (len != 62) return false;
    if (strcmp(addr + 56, ".onion") != 0) return false;
    for (size_t i = 0; i < 56; i++) {
        char c = addr[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '2' && c <= '7');
        if (!ok) return false;
    }
    return true;
}

/* A no-op .onion request handler.  Tor's dynhost module only wires
 * into the app layer when a handler is registered
 * (tor_integration.c:272).  The bootstrap test doesn't care about
 * serving HTTP — it only wants the .onion address published — but
 * registering a handler matches the production call shape at
 * boot_services.c:1306 so we exercise the same code path. */
static size_t p11_noop_handler(const char *method, const char *path,
                                const uint8_t *body, size_t body_len,
                                uint8_t *response, size_t response_max,
                                void *ctx)
{
    (void)method; (void)path; (void)body; (void)body_len;
    (void)response; (void)response_max; (void)ctx;
    return 0;  /* 404 — empty response */
}

int test_onion_bootstrap(void);

int test_onion_bootstrap(void)
{
    int failures = 0;
    printf("\n=== Tor onion bootstrap (MVP #2, <60s) ===\n");
    printf("onion_bootstrap MVP #2 bootstrap_state=ready in <60s... ");

    if (!getenv("ZCL_STRESS_TESTS")) {
        printf("SKIP (set ZCL_STRESS_TESTS=1 to run — ~30s + Tor network)\n");
        return 0;
    }

    /* Defensive: if a previous test in the same process already
     * started Tor (shouldn't happen — test_tor.c never calls
     * tor_integration_start), stop it so we start from a clean
     * state machine. */
    tor_integration_stop();

    char datadir[] = "zcl_p11_onion_bootstrap_XXXXXX";
    if (!mkdtemp(datadir)) {
        printf("FAIL (mkdtemp failed for bootstrap datadir)\n");
        return 1;
    }

    /* Match the production wiring at boot_services.c:1303-1316 —
     * register a request handler before starting so dynhost's
     * external-handler branch is exercised. */
    tor_integration_set_handler(p11_noop_handler, NULL);

    /* p2p_port=18033 → bootstrap SocksPort 29999; avoids collision
     * with the systemctl-running node (default 8033 → 19999). */
    const uint16_t p2p_port = 18033;

    if (!tor_integration_start(datadir, p2p_port)) {
        printf("FAIL (tor_integration_start returned false)\n");
        p11_remove_tree(datadir);
        return 1;
    }

    /* The MVP budget: 60 seconds.  We poll to 90s so diagnostics
     * differentiate "regressed past budget" from "bootstrap broken
     * entirely" in the failure message. */
    const int budget_sec = 60;
    const int ceiling_sec = 90;
    bool ready = false;
    time_t t0 = platform_time_wall_time_t();

    for (int i = 0; i < ceiling_sec; i++) {
        if (tor_integration_is_ready()) { ready = true; break; }
        sleep(1);
    }

    int elapsed = (int)(platform_time_wall_time_t() - t0);
    const char *addr = tor_integration_get_onion_address();

    if (!ready) {
        printf("FAIL (not ready after %ds ceiling; addr=%s)\n",
               ceiling_sec, addr ? addr : "NULL");
        failures++;
    } else if (elapsed > budget_sec) {
        printf("FAIL (ready in %ds, exceeds %ds MVP budget; addr=%s)\n",
               elapsed, budget_sec, addr ? addr : "NULL");
        failures++;
    } else if (!addr) {
        printf("FAIL (ready flag set but address is NULL)\n");
        failures++;
    } else if (!is_valid_onion_v3(addr)) {
        printf("FAIL (ready in %ds but address malformed: \"%s\" "
               "(len=%zu; expected 56 base32 + .onion))\n",
               elapsed, addr, strlen(addr));
        failures++;
    } else {
        printf("OK (%ds, %s)\n", elapsed, addr);
    }

    tor_integration_stop();
    p11_remove_tree(datadir);
    return failures;
}
