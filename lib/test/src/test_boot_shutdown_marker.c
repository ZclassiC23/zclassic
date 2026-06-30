/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "config/boot_shutdown_marker.h"
#include "event/event.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BSM_CHECK(name, expr) do {                                      \
    printf("boot_shutdown_marker: %s... ", (name));                   \
    if (expr) printf("OK\n");                                         \
    else { printf("FAIL\n"); failures++; }                            \
} while (0)

static _Atomic int g_bsm_crash_events;

static void bsm_crash_observer(enum event_type type, uint32_t peer_id,
                               const void *payload, uint32_t len, void *ctx)
{
    (void)peer_id;
    (void)payload;
    (void)len;
    (void)ctx;
    if (type == EV_CRASH_RECOVERY_START)
        atomic_fetch_add(&g_bsm_crash_events, 1);
}

static bool bsm_write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    if (text && fputs(text, f) < 0) {
        fclose(f);
        return false;
    }
    return fclose(f) == 0;
}

static bool bsm_read_file(const char *path, char *out, size_t out_cap)
{
    if (!path || !out || out_cap == 0)
        return false;
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    size_t n = fread(out, 1, out_cap - 1, f);
    fclose(f);
    out[n] = '\0';
    return true;
}

int test_boot_shutdown_marker(void)
{
    int failures = 0;

    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "boot_shutdown_marker", "unclean");
        char marker[512];
        char wal[512];
        snprintf(marker, sizeof(marker), "%s/.shutdown_clean", dir);
        snprintf(wal, sizeof(wal), "%s/node.db-wal", dir);
        bool ok = bsm_write_file(wal, "wal data");

        atomic_store(&g_bsm_crash_events, 0);
        event_clear_observers(EV_CRASH_RECOVERY_START);
        ok = ok && event_observe(EV_CRASH_RECOVERY_START,
                                 bsm_crash_observer, NULL);
        ok = ok && boot_shutdown_marker_detect_unclean(dir);
        BSM_CHECK("wal without marker emits crash recovery",
                  ok && atomic_load(&g_bsm_crash_events) == 1 &&
                  access(marker, F_OK) != 0 &&
                  access(wal, F_OK) == 0);
        event_clear_observers(EV_CRASH_RECOVERY_START);
        test_rm_rf_recursive(dir);
    }

    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "boot_shutdown_marker", "clean");
        char marker[512];
        char wal[512];
        snprintf(marker, sizeof(marker), "%s/.shutdown_clean", dir);
        snprintf(wal, sizeof(wal), "%s/node.db-wal", dir);
        bool ok = bsm_write_file(marker, "1713100000\n");
        ok = ok && bsm_write_file(wal, "wal data");

        atomic_store(&g_bsm_crash_events, 0);
        event_clear_observers(EV_CRASH_RECOVERY_START);
        ok = ok && event_observe(EV_CRASH_RECOVERY_START,
                                 bsm_crash_observer, NULL);
        bool unclean = boot_shutdown_marker_detect_unclean(dir);
        BSM_CHECK("clean marker suppresses event and is consumed",
                  ok && !unclean &&
                  atomic_load(&g_bsm_crash_events) == 0 &&
                  access(marker, F_OK) != 0 &&
                  access(wal, F_OK) == 0);
        event_clear_observers(EV_CRASH_RECOVERY_START);
        test_rm_rf_recursive(dir);
    }

    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "boot_shutdown_marker", "first");

        atomic_store(&g_bsm_crash_events, 0);
        event_clear_observers(EV_CRASH_RECOVERY_START);
        bool ok = event_observe(EV_CRASH_RECOVERY_START,
                                bsm_crash_observer, NULL);
        bool unclean = boot_shutdown_marker_detect_unclean(dir);
        BSM_CHECK("first boot without wal stays quiet",
                  ok && !unclean &&
                  atomic_load(&g_bsm_crash_events) == 0);
        event_clear_observers(EV_CRASH_RECOVERY_START);
        test_rm_rf_recursive(dir);
    }

    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "boot_shutdown_marker", "write");
        char marker[512];
        char buf[64];
        snprintf(marker, sizeof(marker), "%s/.shutdown_clean", dir);

        bool ok = boot_shutdown_marker_write_clean(dir);
        ok = ok && bsm_read_file(marker, buf, sizeof(buf));
        char *end = NULL;
        long stamp = strtol(buf, &end, 10);
        BSM_CHECK("write clean marker with timestamp",
                  ok && stamp > 0 && end && *end == '\n');
        test_rm_rf_recursive(dir);
    }

    BSM_CHECK("invalid datadir fails closed",
              !boot_shutdown_marker_detect_unclean(NULL) &&
              !boot_shutdown_marker_write_clean(""));

    return failures;
}
