/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_consensus_state_install_runtime — focused unit test for the boot-wiring
 * of the sovereign consensus-state install (config/src/
 * consensus_state_install_runtime.c). It proves the CODE WIRING + logic; the
 * full end-to-end copy-prove (fresh datadir → install a real produced bundle →
 * H* climbs to tip) needs a produced artifact and is a SEPARATE integration
 * step the orchestrator runs.
 *
 * Asserts:
 *   (a) boot_autodetect_consensus_bundle CHOOSES a <datadir>/bundles/<name>.sqlite
 *       bundle when present, skips when absent, when the sovereign-install
 *       marker is set, or when a sibling <name>.failed marker is present, and
 *       picks the lexicographically-greatest candidate deterministically.
 *   (b) consensus_state_install_from_bundle is callable and RETURNS a typed
 *       result (does NOT _exit()) — a bogus bundle path fails closed at
 *       admission with state_installed=false and a non-empty reason.
 *   (c) the durable install-on-next-boot request round-trips: arm → pending →
 *       consume (path matches, budget bumps) → clear → not pending, and the
 *       bounded budget marks TERMINAL after BOOT_INSTALL_BUNDLE_MAX attempts and
 *       is never re-armed.
 */

#include "test/test_helpers.h"

#include "config/boot_consensus_bundle_marker.h"
#include "config/consensus_state_install_runtime.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CSIR_CHECK(desc, cond)                                               \
    do {                                                                     \
        printf("consensus_state_install_runtime: %s... ", (desc));           \
        if (cond) printf("OK\n");                                            \
        else { printf("FAIL\n"); failures++; }                              \
    } while (0)

/* Best-effort touch of an empty file at <dir>/<name>. */
static bool csir_touch(const char *dir, const char *name)
{
    char path[512];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= sizeof(path))
        return false;
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    fclose(f);
    return true;
}

/* (a) Autodetect discovery + gating. */
static int case_autodetect(void)
{
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "csir_autodetect", "ok");

    /* No bundles/ directory at all → NULL. */
    char *p = boot_autodetect_consensus_bundle(dir);
    CSIR_CHECK("no bundles/ dir -> NULL", p == NULL);
    free(p);

    /* Empty bundles/ dir (no *.sqlite) → NULL. */
    char bundles[320];
    snprintf(bundles, sizeof(bundles), "%s/bundles", dir);
    CSIR_CHECK("mkdir bundles/", mkdir(bundles, 0700) == 0);
    p = boot_autodetect_consensus_bundle(dir);
    CSIR_CHECK("empty bundles/ -> NULL", p == NULL);
    free(p);

    /* A non-.sqlite file is ignored. */
    CSIR_CHECK("touch bundles/readme.txt", csir_touch(bundles, "readme.txt"));
    p = boot_autodetect_consensus_bundle(dir);
    CSIR_CHECK("only non-sqlite -> NULL", p == NULL);
    free(p);

    /* One *.sqlite present → chosen, absolute path ends in that name. */
    CSIR_CHECK("touch bundles/a-100.sqlite", csir_touch(bundles, "a-100.sqlite"));
    p = boot_autodetect_consensus_bundle(dir);
    CSIR_CHECK("one *.sqlite -> chosen",
               p != NULL &&
                   strcmp(p + strlen(p) - strlen("/bundles/a-100.sqlite"),
                          "/bundles/a-100.sqlite") == 0);
    free(p);

    /* A lexicographically-greater candidate wins (stable, deterministic). */
    CSIR_CHECK("touch bundles/z-200.sqlite", csir_touch(bundles, "z-200.sqlite"));
    p = boot_autodetect_consensus_bundle(dir);
    CSIR_CHECK("greatest name wins (z-200)",
               p != NULL && strlen(p) >= strlen("z-200.sqlite") &&
                   strcmp(p + strlen(p) - strlen("z-200.sqlite"),
                          "z-200.sqlite") == 0);
    free(p);

    /* A sibling .failed marker on the winner skips it → falls back to the other. */
    CSIR_CHECK("touch bundles/z-200.sqlite.failed",
               csir_touch(bundles, "z-200.sqlite.failed"));
    p = boot_autodetect_consensus_bundle(dir);
    CSIR_CHECK("failed winner skipped -> a-100 chosen",
               p != NULL &&
                   strcmp(p + strlen(p) - strlen("a-100.sqlite"),
                          "a-100.sqlite") == 0);
    free(p);

    /* The sovereign-install marker present → never re-install (NULL even with a
     * live bundle present). */
    uint8_t digest[32];
    memset(digest, 0x5a, sizeof(digest));
    CSIR_CHECK("write sovereign-install marker",
               boot_consensus_bundle_marker_write(dir, 100, digest));
    p = boot_autodetect_consensus_bundle(dir);
    CSIR_CHECK("marker set -> NULL (never re-install)", p == NULL);
    free(p);

    test_rm_rf_recursive(dir);
    return failures;
}

/* (b) The runtime entry is callable and RETURNS (no _exit) on a bogus bundle. */
static int case_runtime_returns(void)
{
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "csir_runtime", "ok");

    char bogus[320];
    snprintf(bogus, sizeof(bogus), "%s/bundles/does-not-exist.sqlite", dir);

    struct consensus_state_install_runtime_result rr;
    /* ndb/ms are unused on this fail-closed early-return path (admission fails
     * before any state is touched); a valid temp datadir keeps the classify step
     * in the COPY_PROOF lane so no canonical gate fires. */
    struct zcl_result r =
        consensus_state_install_from_bundle(NULL, NULL, bogus, dir, &rr);

    CSIR_CHECK("bogus bundle -> not ok (returned, did not _exit)", !r.ok);
    CSIR_CHECK("bogus bundle -> state NOT installed", !rr.state_installed);
    CSIR_CHECK("bogus bundle -> marker NOT written", !rr.marker_written);
    CSIR_CHECK("bogus bundle -> non-empty reason", rr.reason[0] != '\0');
    CSIR_CHECK("bogus bundle -> reason names admission",
               strstr(rr.reason, "admission") != NULL ||
                   strstr(rr.reason, "bundle") != NULL);

    /* Empty path/datadir also fail closed without dereferencing anything. */
    struct consensus_state_install_runtime_result rr2;
    struct zcl_result r2 =
        consensus_state_install_from_bundle(NULL, NULL, "", dir, &rr2);
    CSIR_CHECK("empty bundle path -> not ok", !r2.ok && !rr2.state_installed);

    /* out==NULL is tolerated (the core uses a local). */
    struct zcl_result r3 =
        consensus_state_install_from_bundle(NULL, NULL, bogus, dir, NULL);
    CSIR_CHECK("out==NULL tolerated -> not ok", !r3.ok);

    test_rm_rf_recursive(dir);
    return failures;
}

/* (c) The durable install-on-next-boot request round-trips + bounded budget. */
static int case_durable_request(void)
{
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "csir_request", "ok");

    const char *bundle = "/some/where/produced-bundle-3056758.sqlite";

    CSIR_CHECK("fresh: not pending", !boot_install_bundle_pending(dir));

    /* Arm. */
    CSIR_CHECK("arm -> 1 (freshly armed)",
               boot_install_bundle_request(dir, bundle) == 1);
    CSIR_CHECK("armed -> pending", boot_install_bundle_pending(dir));

    /* Re-arm is idempotent while pending (attempts bump only at consume). */
    CSIR_CHECK("re-arm idempotent -> current count (1)",
               boot_install_bundle_request(dir, bundle) == 1);

    /* Consume returns the exact armed path and stays pending (budget not spent). */
    char got[512];
    CSIR_CHECK("consume #1 -> true",
               boot_install_bundle_consume(dir, got, sizeof(got)));
    CSIR_CHECK("consume #1 -> path round-trips", strcmp(got, bundle) == 0);
    CSIR_CHECK("still pending after consume #1", boot_install_bundle_pending(dir));

    /* Clear once committed → no longer pending, and consume is a no-op. */
    boot_install_bundle_clear(dir);
    CSIR_CHECK("cleared -> not pending", !boot_install_bundle_pending(dir));
    char got2[512];
    CSIR_CHECK("consume after clear -> false",
               !boot_install_bundle_consume(dir, got2, sizeof(got2)) &&
                   got2[0] == '\0');

    /* Bounded budget: re-arm, then consume MAX times, the next consume marks
     * TERMINAL and refuses; the request is then present-but-not-pending and can
     * never be re-armed. */
    CSIR_CHECK("re-arm after clear -> 1", boot_install_bundle_request(dir, bundle) == 1);
    for (int i = 0; i < BOOT_INSTALL_BUNDLE_MAX; i++) {
        char b[512];
        char label[64];
        snprintf(label, sizeof(label), "budget consume #%d -> true", i + 1);
        CSIR_CHECK(label, boot_install_bundle_consume(dir, b, sizeof(b)) &&
                              strcmp(b, bundle) == 0);
    }
    char bx[512];
    CSIR_CHECK("consume past budget -> false (TERMINAL)",
               !boot_install_bundle_consume(dir, bx, sizeof(bx)));
    CSIR_CHECK("terminal -> present-but-not-pending",
               !boot_install_bundle_pending(dir));
    CSIR_CHECK("re-arm over TERMINAL -> TERMINAL (never re-armed)",
               boot_install_bundle_request(dir, bundle) ==
                   BOOT_INSTALL_BUNDLE_TERMINAL);

    /* Rejects an embedded-newline path (line-oriented codec). */
    CSIR_CHECK("newline path rejected",
               boot_install_bundle_request(dir, "/a\n/b") == 0);

    test_rm_rf_recursive(dir);
    return failures;
}

int test_consensus_state_install_runtime(void)
{
    printf("\n=== consensus_state_install_runtime ===\n");
    int failures = 0;
    failures += case_autodetect();
    failures += case_runtime_returns();
    failures += case_durable_request();
    printf("=== consensus_state_install_runtime: %d failure(s) ===\n", failures);
    return failures;
}
