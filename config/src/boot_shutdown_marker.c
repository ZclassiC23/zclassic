/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "config/boot_shutdown_marker.h"

#include "event/event.h"
#include "platform/time_compat.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static bool boot_shutdown_marker_path(char *out, size_t out_n,
                                      const char *datadir,
                                      const char *name)
{
    if (!out || out_n == 0 || !datadir || !*datadir || !name || !*name)
        return false;

    int n = snprintf(out, out_n, "%s/%s", datadir, name);
    return n >= 0 && (size_t)n < out_n;
}

bool boot_shutdown_marker_detect_unclean(const char *datadir)
{
    char marker_path[1024];
    char wal_path[1024];
    if (!boot_shutdown_marker_path(marker_path, sizeof(marker_path),
                                   datadir, ".shutdown_clean") ||
        !boot_shutdown_marker_path(wal_path, sizeof(wal_path),
                                   datadir, "node.db-wal")) {
        fprintf(stderr,
                "[boot] Cannot inspect clean shutdown marker: invalid datadir\n");
        return false;
    }

    struct stat wal_st;
    bool wal_exists = (stat(wal_path, &wal_st) == 0 && wal_st.st_size > 0);
    bool marker_exists = (access(marker_path, F_OK) == 0);
    bool unclean = !marker_exists && wal_exists;

    if (unclean) {
        printf("[boot] Unclean shutdown detected (WAL=%lldB, "
               "clean marker missing)\n",
               (long long)wal_st.st_size);
        event_emitf(EV_CRASH_RECOVERY_START, 0,
                    "wal_size=%lld clean_marker=missing",
                    (long long)wal_st.st_size);
    } else if (!marker_exists) {
        printf("[boot] First boot or marker absent (no WAL)\n");
    } else {
        printf("[boot] Clean shutdown marker present\n");
    }

    unlink(marker_path);
    return unclean;
}

bool boot_shutdown_marker_write_clean(const char *datadir)
{
    char path[1024];
    if (!boot_shutdown_marker_path(path, sizeof(path),
                                   datadir, ".shutdown_clean"))
        return false;

    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    fprintf(f, "%ld\n", (long)platform_time_wall_time_t());
    return fclose(f) == 0;
}
