/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_auto_reindex — implementation. See header for the contract.
 *
 * Crash-only recovery primitive: a bounded, fsync-durable on-disk request that
 * the next boot consumes to rebuild derived state via -reindex-chainstate
 * (rewind to the consistent reindex target + replay from blocks/). Top-level
 * file <datadir>/auto_reindex_request holding "<anchor_height> <count>",
 * NEVER part of any derived-state wipe set so the attempt budget survives
 * every rebuild tier and a crash mid-rebuild.
 */

#include "storage/boot_auto_reindex.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void ar_path(const char *datadir, char *out, size_t n)
{
    snprintf(out, n, "%s/auto_reindex_request", datadir);
}

int boot_auto_reindex_request(const char *datadir, int32_t anchor)
{
    if (!datadir)
        return 0;

    char path[512];
    ar_path(datadir, path, sizeof(path));

    /* Read the current (anchor, count); a different anchor restarts the count
     * so a healthy node that rebuilt once long ago does not carry stale budget. */
    int32_t cur_anchor = 0;
    int cur_count = 0;
    FILE *r = fopen(path, "r");
    if (r) {
        if (fscanf(r, "%d %d", &cur_anchor, &cur_count) != 2) {
            cur_anchor = 0;
            cur_count = 0;
        }
        fclose(r);
    }
    int new_count = (cur_anchor == anchor && cur_count > 0) ? cur_count + 1 : 1;

    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%d %d\n", (int)anchor, new_count);
    if (len < 0 || len >= (int)sizeof(buf))
        return 0;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fprintf(stderr,  // obs-ok:storage-primitive-error
                "[boot] boot_auto_reindex_request: open(%s) failed\n", path);
        return 0;
    }
    ssize_t w = write(fd, buf, (size_t)len);
    int sync_rc = fsync(fd);  /* the budget MUST survive a crash mid-rebuild */
    int close_rc = close(fd);
    if (w != (ssize_t)len || sync_rc != 0 || close_rc != 0) {
        fprintf(stderr,  // obs-ok:storage-primitive-error
                "[boot] boot_auto_reindex_request: write/fsync(%s) failed\n",
                path);
        return 0;
    }
    /* fsync the directory so the file's existence is durable across a crash. */
    int dfd = open(datadir, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        (void)fsync(dfd);
        close(dfd);
    }
    return new_count;
}

bool boot_auto_reindex_pending(const char *datadir)
{
    if (!datadir)
        return false;
    char path[512];
    ar_path(datadir, path, sizeof(path));
    return access(path, F_OK) == 0;
}

void boot_auto_reindex_clear(const char *datadir)
{
    if (!datadir)
        return;
    char path[512];
    ar_path(datadir, path, sizeof(path));
    (void)remove(path);
}
