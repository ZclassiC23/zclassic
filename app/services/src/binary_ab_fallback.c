/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Binary A/B fallback — node side. See header for the full rationale and the
 * launcher/node split. This file is pure filesystem + blocker plumbing: no
 * threads, no background service; the two env wrappers are called once each
 * from the boot path (raise-blocker early in observability init, promote at
 * the activation-ready watchdog start).
 */
// one-result-type-ok:small-fs-helpers — these are best-effort filesystem
// steps whose only meaningful signal is success/failure (bool); there is no
// multi-reason surface a zcl_result would carry that the LOG_FAIL context
// line does not already record. Matches the sibling binary_staleness_service.c
// convention for the same class of one-shot IO helpers.

#include "services/binary_ab_fallback.h"

#include "util/blocker.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util/log_macros.h"

#define BINARY_AB_COPY_CHUNK (64 * 1024)

/* ── fsync helpers ──────────────────────────────────────────────────── */

/* fsync the directory that contains `path` so a rename into it is durable.
 * Best-effort: a filesystem that rejects O_DIRECTORY fsync (rare) must not
 * fail the whole promotion, so this logs and returns rather than aborting. */
static void binary_ab_fsync_parent_dir(const char *path)
{
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) {
        dir[0] = '.';
        dir[1] = '\0';
    } else if (slash == dir) {
        dir[1] = '\0'; /* path was "/x" → parent is "/" */
    } else {
        *slash = '\0';
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd < 0) {
        LOG_WARN("binary_ab", "open(%s) for dir fsync failed: %s",
                 dir, strerror(errno));
        return;
    }
    if (fsync(dfd) != 0)
        LOG_WARN("binary_ab", "fsync(%s) failed: %s", dir, strerror(errno));
    close(dfd);
}

/* ── Streak reset ───────────────────────────────────────────────────── */

bool binary_ab_reset_streak(const char *streak_file)
{
    if (!streak_file || streak_file[0] == '\0')
        LOG_FAIL("binary_ab", "reset_streak: empty streak_file path");

    char tmp[1088];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", streak_file, (long)getpid());

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        LOG_FAIL("binary_ab", "reset_streak: open(%s) failed: %s",
                 tmp, strerror(errno));

    static const char zero[] = "0\n";
    ssize_t w = write(fd, zero, sizeof(zero) - 1);
    if (w != (ssize_t)(sizeof(zero) - 1)) {
        LOG_WARN("binary_ab", "reset_streak: short write to %s: %s",
                 tmp, strerror(errno));
        close(fd);
        unlink(tmp);
        return false;
    }
    if (fsync(fd) != 0)
        LOG_WARN("binary_ab", "reset_streak: fsync(%s) failed: %s",
                 tmp, strerror(errno));
    close(fd);

    if (rename(tmp, streak_file) != 0) {
        LOG_WARN("binary_ab", "reset_streak: rename(%s->%s) failed: %s",
                 tmp, streak_file, strerror(errno));
        unlink(tmp);
        return false;
    }
    binary_ab_fsync_parent_dir(streak_file);
    LOG_INFO("binary_ab", "boot-failure streak reset to 0 (%s)", streak_file);
    return true;
}

/* ── Streak increment (self-respawn exits) ─────────────────────────── */

bool binary_ab_note_self_respawn_exit(const char *streak_file)
{
    if (!streak_file || streak_file[0] == '\0')
        LOG_FAIL("binary_ab", "note_self_respawn_exit: empty streak_file path");

    long cur = 0;
    int in_fd = open(streak_file, O_RDONLY);
    if (in_fd >= 0) {
        char buf[32] = {0};
        ssize_t r = read(in_fd, buf, sizeof(buf) - 1);
        close(in_fd);
        if (r > 0) {
            buf[r] = '\0';
            cur = strtol(buf, NULL, 10);
        }
    }
    /* Missing/unreadable/malformed file -> streak 0, matching the launcher's
     * own `cat "$STREAK_FILE" 2>/dev/null || echo 0` fallback. */
    if (cur < 0) cur = 0;

    char tmp[1088];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", streak_file, (long)getpid());

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        LOG_FAIL("binary_ab", "note_self_respawn_exit: open(%s) failed: %s",
                 tmp, strerror(errno));

    char content[32];
    int clen = snprintf(content, sizeof(content), "%ld\n", cur + 1);
    ssize_t w = write(fd, content, (size_t)clen);
    if (w != (ssize_t)clen) {
        LOG_WARN("binary_ab", "note_self_respawn_exit: short write to %s: %s",
                 tmp, strerror(errno));
        close(fd);
        unlink(tmp);
        return false;
    }
    if (fsync(fd) != 0)
        LOG_WARN("binary_ab", "note_self_respawn_exit: fsync(%s) failed: %s",
                 tmp, strerror(errno));
    close(fd);

    if (rename(tmp, streak_file) != 0) {
        LOG_WARN("binary_ab", "note_self_respawn_exit: rename(%s->%s) failed: %s",
                 tmp, streak_file, strerror(errno));
        unlink(tmp);
        return false;
    }
    binary_ab_fsync_parent_dir(streak_file);
    LOG_WARN("binary_ab",
             "boot-failure streak incremented to %ld (self-respawn exit, %s)",
             cur + 1, streak_file);
    return true;
}

/* ── Promotion (current -> last-good) ───────────────────────────────── */

bool binary_ab_promote(const char *slots_dir, const char *current_path)
{
    if (!slots_dir || slots_dir[0] == '\0')
        LOG_FAIL("binary_ab", "promote: empty slots_dir");
    if (!current_path || current_path[0] == '\0')
        LOG_FAIL("binary_ab", "promote: empty current_path");

    char dst[1024];
    snprintf(dst, sizeof(dst), "%s/%s", slots_dir, BINARY_AB_LASTGOOD_BASENAME);
    char tmp[1088];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", dst, (long)getpid());

    int in = open(current_path, O_RDONLY);
    if (in < 0)
        LOG_FAIL("binary_ab", "promote: open(%s) failed: %s",
                 current_path, strerror(errno));

    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out < 0) {
        close(in);
        LOG_FAIL("binary_ab", "promote: open(%s) failed: %s",
                 tmp, strerror(errno));
    }

    unsigned char buf[BINARY_AB_COPY_CHUNK];
    ssize_t n;
    bool copy_ok = true;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w <= 0) {
                LOG_WARN("binary_ab", "promote: write(%s) failed: %s",
                         tmp, strerror(errno));
                copy_ok = false;
                break;
            }
            off += w;
        }
        if (!copy_ok)
            break;
    }
    if (n < 0) {
        LOG_WARN("binary_ab", "promote: read(%s) failed: %s",
                 current_path, strerror(errno));
        copy_ok = false;
    }
    close(in);

    if (copy_ok) {
        if (fchmod(out, 0755) != 0)
            LOG_WARN("binary_ab", "promote: fchmod(%s) failed: %s",
                     tmp, strerror(errno));
        if (fsync(out) != 0) {
            LOG_WARN("binary_ab", "promote: fsync(%s) failed: %s",
                     tmp, strerror(errno));
            copy_ok = false;
        }
    }
    close(out);

    if (!copy_ok) {
        unlink(tmp);
        return false;
    }

    if (rename(tmp, dst) != 0) {
        LOG_WARN("binary_ab", "promote: rename(%s->%s) failed: %s",
                 tmp, dst, strerror(errno));
        unlink(tmp);
        return false;
    }
    binary_ab_fsync_parent_dir(dst);
    LOG_INFO("binary_ab", "promoted current binary to last-good slot (%s)", dst);
    return true;
}

/* ── Ready action ───────────────────────────────────────────────────── */

bool binary_ab_on_ready(const char *slots_dir, const char *current_path,
                        bool fallback_active)
{
    if (!slots_dir || slots_dir[0] == '\0')
        return true; /* not launcher-managed */

    char streak[1024];
    snprintf(streak, sizeof(streak), "%s/%s", slots_dir,
             BINARY_AB_STREAK_BASENAME);

    bool ok = binary_ab_reset_streak(streak);

    if (fallback_active) {
        /* Running the last-good slot: promoting `current` here would overwrite
         * the good slot with the very binary we fell back away from. Reset the
         * streak (so a subsequent good deploy gets a clean 3-strike budget)
         * but never promote. */
        LOG_WARN("binary_ab",
                 "reached ready under FALLBACK slot — streak reset, last-good "
                 "left intact (operator must deploy a good binary)");
        return ok;
    }

    if (current_path && current_path[0] != '\0')
        ok = binary_ab_promote(slots_dir, current_path) && ok;

    return ok;
}

/* ── Blocker ────────────────────────────────────────────────────────── */

void binary_ab_raise_fallback_blocker(bool fallback_active)
{
    if (!fallback_active)
        return;

    struct blocker_record rec;
    char reason[BLOCKER_REASON_MAX];
    snprintf(reason, sizeof(reason),
             "launcher fell back to the last-known-good binary after the "
             "current build failed to reach activation-ready %d times in a "
             "row — node is degraded-but-alive; deploy a working binary and "
             "clear the boot-failure streak",
             3 /* the launcher's fallback threshold; informational */);
    if (!blocker_init(&rec, BINARY_FALLBACK_BLOCKER_ID,
                      BINARY_FALLBACK_BLOCKER_OWNER, BLOCKER_PERMANENT, reason))
        return; /* blocker_init already logged via LOG_FAIL */
    blocker_set(&rec);
    LOG_ERROR("binary_ab",
              "binary.fallback_active raised — running last-good slot");
}

/* ── Env wrappers ───────────────────────────────────────────────────── */

void binary_ab_promote_on_ready_env(void)
{
    const char *slots = getenv(BINARY_AB_ENV_SLOTS_DIR);
    if (!slots || slots[0] == '\0')
        return; /* launched directly, not via the launcher */
    const char *current = getenv(BINARY_AB_ENV_CURRENT);
    const char *fb = getenv(BINARY_AB_ENV_FALLBACK);
    bool fallback_active = fb && fb[0] == '1' && fb[1] == '\0';
    binary_ab_on_ready(slots, current, fallback_active);
}

void binary_ab_raise_fallback_blocker_env(void)
{
    const char *fb = getenv(BINARY_AB_ENV_FALLBACK);
    binary_ab_raise_fallback_blocker(fb && fb[0] == '1' && fb[1] == '\0');
}

void binary_ab_note_self_respawn_exit_env(void)
{
    const char *slots = getenv(BINARY_AB_ENV_SLOTS_DIR);
    if (!slots || slots[0] == '\0')
        return; /* launched directly, not via the launcher */
    char streak[1024];
    snprintf(streak, sizeof(streak), "%s/%s", slots, BINARY_AB_STREAK_BASENAME);
    binary_ab_note_self_respawn_exit(streak);
}
