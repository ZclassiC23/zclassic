/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "config/boot_datadir_lock.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
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
        BDL_CHECK("release retains reusable lock inode",
                  access(pid_path, F_OK) == 0);
        test_rm_rf_recursive(dir);
    }

    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "boot_datadir_lock", "concurrent");
        char pid_path[512];
        char buf[64];
        snprintf(pid_path, sizeof(pid_path), "%s/zclassic23.pid", dir);

        int ready_pipe[2] = {-1, -1};
        int release_pipe[2] = {-1, -1};
        bool pipes_ok = pipe(ready_pipe) == 0;
        if (pipes_ok)
            pipes_ok = pipe(release_pipe) == 0;
        pid_t child = pipes_ok ? fork() : -1;
        if (child == 0) {
            close(ready_pipe[0]);
            close(release_pipe[1]);
            bool locked = boot_datadir_lock_acquire(dir);
            char ready = locked ? '1' : '0';
            (void)!write(ready_pipe[1], &ready, 1);
            close(ready_pipe[1]);
            if (locked) {
                char release = 0;
                (void)!read(release_pipe[0], &release, 1);
                boot_datadir_lock_release();
            }
            close(release_pipe[0]);
            _exit(locked ? 0 : 1);
        }

        bool child_ready = false;
        bool parent_refused = false;
        bool child_clean = false;
        bool parent_reacquired = false;
        if (child > 0) {
            close(ready_pipe[1]);
            close(release_pipe[0]);
            char ready = 0;
            child_ready = read(ready_pipe[0], &ready, 1) == 1 && ready == '1';
            close(ready_pipe[0]);
            if (child_ready) {
                parent_refused = !boot_datadir_lock_acquire(dir);
                char release = '1';
                (void)!write(release_pipe[1], &release, 1);
            }
            close(release_pipe[1]);
            int status = 0;
            child_clean = waitpid(child, &status, 0) == child &&
                          WIFEXITED(status) && WEXITSTATUS(status) == 0;
            parent_reacquired = boot_datadir_lock_acquire(dir);
            if (parent_reacquired) {
                parent_reacquired = bdl_read_file(pid_path, buf, sizeof(buf));
                boot_datadir_lock_release();
            }
        } else {
            if (ready_pipe[0] >= 0) close(ready_pipe[0]);
            if (ready_pipe[1] >= 0) close(ready_pipe[1]);
            if (release_pipe[0] >= 0) close(release_pipe[0]);
            if (release_pipe[1] >= 0) close(release_pipe[1]);
        }
        BDL_CHECK("concurrent process excluded then release permits acquire",
                  pipes_ok && child_ready && parent_refused && child_clean &&
                  parent_reacquired);
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
        BDL_CHECK("stale lock file is reused", ok && strcmp(buf, want) == 0);

        boot_datadir_lock_release();
        test_rm_rf_recursive(dir);
    }

    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "boot_datadir_lock", "symlink");
        char pid_path[512];
        char target_path[512];
        char buf[64];
        struct stat st;
        snprintf(pid_path, sizeof(pid_path), "%s/zclassic23.pid", dir);
        snprintf(target_path, sizeof(target_path), "%s/not-a-lock", dir);

        bool ok = bdl_write_file(target_path, "do-not-touch\n");
        ok = ok && symlink(target_path, pid_path) == 0;
        ok = ok && !boot_datadir_lock_acquire(dir);
        ok = ok && bdl_read_file(target_path, buf, sizeof(buf));
        ok = ok && strcmp(buf, "do-not-touch\n") == 0;
        ok = ok && lstat(pid_path, &st) == 0 && S_ISLNK(st.st_mode);
        BDL_CHECK("symlink lock path is refused without touching target", ok);
        boot_datadir_lock_release();
        test_rm_rf_recursive(dir);
    }

    {
        char parent[256];
        test_make_tmpdir(parent, sizeof(parent), "boot_datadir_lock",
                         "datadir_symlink");
        char real_dir[512];
        char alias_dir[512];
        snprintf(real_dir, sizeof(real_dir), "%s/real", parent);
        snprintf(alias_dir, sizeof(alias_dir), "%s/alias", parent);
        bool ok = mkdir(real_dir, 0700) == 0 &&
                  symlink(real_dir, alias_dir) == 0 &&
                  !boot_datadir_lock_acquire(alias_dir);
        BDL_CHECK("symlink datadir is refused", ok);
        boot_datadir_lock_release();
        test_rm_rf_recursive(parent);
    }

    {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "boot_datadir_lock", "reacquire");
        bool first = boot_datadir_lock_acquire(dir);
        bool duplicate_refused = first && !boot_datadir_lock_acquire(dir);
        boot_datadir_lock_release();
        bool second = boot_datadir_lock_acquire(dir);
        boot_datadir_lock_release();
        BDL_CHECK("release is idempotent and permits reacquire",
                  first && duplicate_refused && second);
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
