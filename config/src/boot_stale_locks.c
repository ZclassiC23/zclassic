/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "config/boot_stale_locks.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

static bool boot_stale_locks_path(char *out, size_t out_n,
                                  const char *datadir,
                                  const char *suffix)
{
    if (!out || out_n == 0 || !datadir || !*datadir || !suffix || !*suffix)
        return false;

    int n = snprintf(out, out_n, "%s/%s", datadir, suffix);
    return n >= 0 && (size_t)n < out_n;
}

static bool boot_stale_locks_read_pid(const char *path, long *pid_out)
{
    if (!path || !*path || !pid_out)
        return false;

    FILE *lf = fopen(path, "r");
    if (!lf)
        return false;

    char pidbuf[32] = {0};
    size_t nr = fread(pidbuf, 1, sizeof(pidbuf) - 1, lf);
    fclose(lf);
    if (nr == 0)
        return false;

    char *end = NULL;
    errno = 0;
    long pid = strtol(pidbuf, &end, 10);
    if (errno != 0 || !end || end == pidbuf || pid <= 0)
        return false;

    *pid_out = pid;
    return true;
}

static bool boot_stale_locks_pid_dead(long pid, bool *running_out)
{
    if (running_out)
        *running_out = false;

    errno = 0;
    if (kill((pid_t)pid, 0) == 0) {
        if (running_out)
            *running_out = true;
        return false;
    }

    if (errno == ESRCH)
        return true;

    /* EPERM and unexpected errors are not proof that the owner is dead. */
    if (running_out)
        *running_out = true;
    return false;
}

static void boot_stale_locks_check_pid_lock(const char *path,
                                            const char *label,
                                            bool print_running,
                                            bool *removed_out,
                                            bool *running_out)
{
    if (!path || !label || access(path, F_OK) != 0)
        return;

    long pid = 0;
    if (!boot_stale_locks_read_pid(path, &pid))
        return;

    bool running = false;
    if (boot_stale_locks_pid_dead(pid, &running)) {
        printf("Removing stale %s LOCK (pid %ld dead)\n", label, pid);
        if (unlink(path) == 0 && removed_out)
            *removed_out = true;
        return;
    }

    if (running && running_out)
        *running_out = true;
    if (running && print_running) {
        fprintf(stderr,
                "ERROR: LevelDB locked by pid %ld (still running)\n"
                "Kill the other process or use a different datadir.\n",
                pid);
    }
}

struct boot_stale_locks_result
boot_stale_locks_preflight(const char *datadir)
{
    struct boot_stale_locks_result result = {0};
    if (!datadir || !*datadir) {
        fprintf(stderr, "[boot] Cannot inspect stale locks: invalid datadir\n");
        return result;
    }

    char lock_path[1024];
    if (!boot_stale_locks_path(lock_path, sizeof(lock_path), datadir,
                               "blocks/index/LOCK")) {
        fprintf(stderr, "[boot] Cannot inspect stale locks: datadir too long\n");
        return result;
    }
    boot_stale_locks_check_pid_lock(lock_path, "LevelDB", true,
                                    &result.blocks_index_lock_removed,
                                    &result.blocks_index_lock_running);

    if (!boot_stale_locks_path(lock_path, sizeof(lock_path), datadir,
                               "chainstate/LOCK")) {
        fprintf(stderr, "[boot] Cannot inspect stale locks: datadir too long\n");
        return result;
    }
    boot_stale_locks_check_pid_lock(lock_path, "chainstate", false,
                                    &result.chainstate_lock_removed,
                                    &result.chainstate_lock_running);

    if (!boot_stale_locks_path(lock_path, sizeof(lock_path), datadir,
                               "node.db-wal")) {
        fprintf(stderr, "[boot] Cannot inspect stale locks: datadir too long\n");
        return result;
    }
    if (access(lock_path, F_OK) == 0) {
        result.sqlite_wal_present = true;
        printf("SQLite WAL file exists (normal after crash recovery)\n");
    }

    return result;
}
