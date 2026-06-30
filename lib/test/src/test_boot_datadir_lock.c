/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "config/boot_datadir_lock.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BDL_CHECK(name, expr) do {                                      \
    printf("boot_datadir_lock: %s... ", (name));                       \
    if (expr) printf("OK\n");                                         \
    else { printf("FAIL\n"); failures++; }                            \
} while (0)

static bool bdl_read_file(const char *path, char *out, size_t out_cap)
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

static bool bdl_write_file(const char *path, const char *text)
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

int test_boot_datadir_lock(void)
{
    int failures = 0;

    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "boot_datadir_lock", "basic");
        char pid_path[512];
        char buf[64];
        snprintf(pid_path, sizeof(pid_path), "%s/zclassic23.pid", dir);

        bool ok = boot_datadir_lock_acquire(dir);
        bool read_ok = bdl_read_file(pid_path, buf, sizeof(buf));
        char want[64];
        snprintf(want, sizeof(want), "%ld\n", (long)getpid());
        BDL_CHECK("acquire writes current pid",
                  ok && read_ok && strcmp(buf, want) == 0);

        boot_datadir_lock_release();
        BDL_CHECK("release removes pid file", access(pid_path, F_OK) != 0);
        test_rm_rf_recursive(dir);
    }

    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "boot_datadir_lock", "running");
        char pid_path[512];
        char pid_text[64];
        char buf[64];
        snprintf(pid_path, sizeof(pid_path), "%s/zclassic23.pid", dir);
        snprintf(pid_text, sizeof(pid_text), "%ld\n", (long)getpid());

        bool ok = bdl_write_file(pid_path, pid_text);
        ok = ok && !boot_datadir_lock_acquire(dir);
        ok = ok && bdl_read_file(pid_path, buf, sizeof(buf));
        BDL_CHECK("running pid refuses acquire without overwrite",
                  ok && strcmp(buf, pid_text) == 0);
        test_rm_rf_recursive(dir);
    }

    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "boot_datadir_lock", "stale");
        char pid_path[512];
        char buf[64];
        snprintf(pid_path, sizeof(pid_path), "%s/zclassic23.pid", dir);

        bool ok = bdl_write_file(pid_path, "99999999\n");
        ok = ok && boot_datadir_lock_acquire(dir);
        ok = ok && bdl_read_file(pid_path, buf, sizeof(buf));
        char want[64];
        snprintf(want, sizeof(want), "%ld\n", (long)getpid());
        BDL_CHECK("stale pid is overwritten", ok && strcmp(buf, want) == 0);

        boot_datadir_lock_release();
        test_rm_rf_recursive(dir);
    }

    {
        BDL_CHECK("null datadir fails closed",
                  !boot_datadir_lock_acquire(NULL));
        BDL_CHECK("empty datadir fails closed",
                  !boot_datadir_lock_acquire(""));
    }

    return failures;
}
