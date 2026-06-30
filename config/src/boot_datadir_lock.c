/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "config/boot_datadir_lock.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static char g_pidfile_path[1024];

bool boot_datadir_lock_acquire(const char *datadir)
{
    if (!datadir || !*datadir) {
        fprintf(stderr, "[boot] Cannot acquire datadir lock: empty datadir\n");
        return false;
    }

    int n = snprintf(g_pidfile_path, sizeof(g_pidfile_path),
                     "%s/zclassic23.pid", datadir);
    if (n < 0 || (size_t)n >= sizeof(g_pidfile_path)) {
        fprintf(stderr,
                "[boot] Cannot acquire datadir lock: pid path too long\n");
        g_pidfile_path[0] = '\0';
        return false;
    }

    FILE *f = fopen(g_pidfile_path, "r");
    if (f) {
        char buf[32] = {0};
        size_t bytes = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        if (bytes > 0) {
            long old_pid = strtol(buf, NULL, 10);
            if (old_pid > 0) {
                if (kill((pid_t)old_pid, 0) == 0) {
                    fprintf(stderr,
                            "[boot] Data directory locked by PID %ld "
                            "(running). Cannot start.\n", old_pid);
                    return false;
                }
                printf("[boot] Stale lock detected (PID %ld is not running). "
                       "Removing lock file.\n", old_pid);
            }
        }
    }

    f = fopen(g_pidfile_path, "w");
    if (!f) {
        fprintf(stderr, "[boot] Cannot create PID file %s: %s\n",
                g_pidfile_path, strerror(errno));
        return true;
    }
    fprintf(f, "%ld\n", (long)getpid());
    fclose(f);
    return true;
}

void boot_datadir_lock_release(void)
{
    if (g_pidfile_path[0])
        unlink(g_pidfile_path);
    g_pidfile_path[0] = '\0';
}
