/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_auto_refold — implementation. See header for the contract.
 *
 * Bounded, fsync-durable on-disk request that the next boot consumes to run
 * boot_refold_from_anchor_reset (re-seed coins_kv from the SHA3-checkpoint-bound
 * anchor snapshot + fold the anchor->tip delta). Top-level file
 * <datadir>/auto_refold_request holding "<anchor_height> <attempts>", NEVER part
 * of any derived-state wipe set so the attempt budget survives a crash / FATAL
 * mid-refold. The attempt count increments at CONSUME (boot) time so a
 * FATAL-looping anchor is bounded even though the arming rung never runs again.
 */

#include "storage/boot_auto_refold.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void arf_path(const char *datadir, char *out, size_t n)
{
    snprintf(out, n, "%s/auto_refold_request", datadir);
}

/* Read the on-disk (anchor, count). Returns true iff a well-formed request was
 * read. On any read/parse miss, *anchor=0 and *count=0. */
static bool arf_read(const char *path, int32_t *anchor, int *count)
{
    *anchor = 0;
    *count = 0;
    FILE *r = fopen(path, "r");
    if (!r)
        return false;
    bool ok = (fscanf(r, "%d %d", anchor, count) == 2);
    fclose(r);
    if (!ok) {
        *anchor = 0;
        *count = 0;
    }
    return ok;
}

/* fsync-durable write of "<anchor> <count>\n". Returns true on success. */
static bool arf_write(const char *datadir, const char *path,
                      int32_t anchor, int count)
{
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%d %d\n", (int)anchor, count);
    if (len < 0 || len >= (int)sizeof(buf))
        return false;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fprintf(stderr,  // obs-ok:storage-primitive-error
                "[boot] boot_auto_refold: open(%s) failed\n", path);
        return false;
    }
    ssize_t w = write(fd, buf, (size_t)len);
    int sync_rc = fsync(fd);  /* the budget MUST survive a crash mid-refold */
    int close_rc = close(fd);
    if (w != (ssize_t)len || sync_rc != 0 || close_rc != 0) {
        fprintf(stderr,  // obs-ok:storage-primitive-error
                "[boot] boot_auto_refold: write/fsync(%s) failed\n", path);
        return false;
    }
    /* fsync the directory so the file's existence is durable across a crash. */
    int dfd = open(datadir, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        (void)fsync(dfd);
        close(dfd);
    }
    return true;
}

int boot_auto_refold_request(const char *datadir, int32_t anchor)
{
    if (!datadir)
        return 0;

    char path[512];
    arf_path(datadir, path, sizeof(path));

    int32_t cur_anchor = 0;
    int cur_count = 0;
    bool have = arf_read(path, &cur_anchor, &cur_count);

    /* TERMINAL already written: the budget was exhausted at a stable anchor and
     * the operator was paged. Do NOT re-arm — that is exactly the unbounded
     * crash-loop this primitive exists to prevent. */
    if (have && cur_count == BOOT_AUTO_REFOLD_TERMINAL)
        return BOOT_AUTO_REFOLD_TERMINAL;

    /* Already armed (and not terminal): leave it — attempts bump at consume
     * time, never at arm time, so a re-arming rung tick cannot inflate the
     * budget. Report the current attempt count so the caller can HOLD. */
    if (have && cur_count >= 0)
        return cur_count > 0 ? cur_count : 1;

    /* Fresh arm: attempts=0 (armed, not yet attempted by any boot). */
    if (!arf_write(datadir, path, anchor, 0))
        return 0;
    return 1;
}

bool boot_auto_refold_pending(const char *datadir)
{
    if (!datadir)
        return false;
    char path[512];
    arf_path(datadir, path, sizeof(path));
    if (access(path, F_OK) != 0)
        return false;
    int32_t a = 0;
    int c = 0;
    if (arf_read(path, &a, &c) && c == BOOT_AUTO_REFOLD_TERMINAL)
        return false;  /* terminal: present-but-not-pending */
    return true;
}

bool boot_auto_refold_consume(const char *datadir)
{
    if (!datadir)
        return false;
    char path[512];
    arf_path(datadir, path, sizeof(path));

    int32_t anchor = 0;
    int count = 0;
    if (!arf_read(path, &anchor, &count))
        return false;  /* no request */
    if (count == BOOT_AUTO_REFOLD_TERMINAL)
        return false;  /* budget already spent */

    if (count >= BOOT_AUTO_REFOLD_MAX) {
        /* Budget exhausted: persist the terminal marker (do NOT delete — a
         * delete would let the next boot re-arm a fresh count and loop) and
         * refuse the refold so the node boots normally + the escalator pages. */
        (void)arf_write(datadir, path, anchor, BOOT_AUTO_REFOLD_TERMINAL);
        fprintf(stderr,  // obs-ok:storage-primitive-error
                "[boot] boot_auto_refold: anchor=%d attempts=%d exhausted the "
                "bounded budget (max=%d) — marking TERMINAL, booting normally "
                "(the escalator will page)\n",
                (int)anchor, count, BOOT_AUTO_REFOLD_MAX);
        return false;
    }

    /* Count this boot's attempt BEFORE running the refold, so a FATAL-exit mid
     * refold still burns the budget (the reset _exit()s on a mismatch). */
    if (!arf_write(datadir, path, anchor, count + 1))
        return false;
    return true;
}

bool boot_auto_refold_status(const char *datadir, int32_t *anchor, int *count)
{
    if (anchor)
        *anchor = 0;
    if (count)
        *count = 0;
    if (!datadir)
        return false;

    char path[512];
    arf_path(datadir, path, sizeof(path));
    int32_t a = 0;
    int c = 0;
    if (!arf_read(path, &a, &c))
        return false;
    if (anchor)
        *anchor = a;
    if (count)
        *count = c;
    return true;
}

bool boot_auto_refold_is_terminal(const char *datadir)
{
    if (!datadir)
        return false;
    char path[512];
    arf_path(datadir, path, sizeof(path));
    int32_t a = 0;
    int c = 0;
    if (!arf_read(path, &a, &c))
        return false;
    return c == BOOT_AUTO_REFOLD_TERMINAL;
}

void boot_auto_refold_clear(const char *datadir)
{
    if (!datadir)
        return;
    char path[512];
    arf_path(datadir, path, sizeof(path));
    (void)remove(path);
}
