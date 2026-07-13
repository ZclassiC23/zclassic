/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * PURPOSE: Inspect producer containment before mutating the node datadir,
 * including committed state that exists only in a kill-9-surviving WAL. */

#define _GNU_SOURCE

#include "config/boot.h"
#include "config/mint_anchor_progress.h"

#include "event/event.h"
#include "util/log_macros.h"

#include <errno.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PREFLIGHT_SUBSYS "mint_anchor"

struct preflight_source_file {
    const char *name;
    int fd;
    bool exists;
    struct stat identity;
};

static bool identity_equal(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
        a->st_nlink == b->st_nlink && a->st_size == b->st_size &&
        a->st_mode == b->st_mode &&
        a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
        a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
        a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

static bool source_open(int dirfd, struct preflight_source_file *file)
{
    file->fd = -1;
    file->exists = false;
    struct stat named;
    errno = 0;
    if (fstatat(dirfd, file->name, &named, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT)
            return true;
        LOG_WARN(PREFLIGHT_SUBSYS, "preflight stat failed file=%s: %s",
                 file->name, strerror(errno));
        return false;
    }
    if (!S_ISREG(named.st_mode)) {
        LOG_WARN(PREFLIGHT_SUBSYS,
                 "preflight refuses non-regular file=%s", file->name);
        return false;
    }
    file->fd = openat(dirfd, file->name,
                      O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat opened;
    if (file->fd < 0 || fstat(file->fd, &opened) != 0 ||
        !identity_equal(&named, &opened)) {
        LOG_WARN(PREFLIGHT_SUBSYS,
                 "preflight source identity raced file=%s", file->name);
        if (file->fd >= 0)
            close(file->fd);
        file->fd = -1;
        return false;
    }
    file->exists = true;
    file->identity = opened;
    return true;
}

static bool source_unchanged(int dirfd,
                             const struct preflight_source_file *file)
{
    struct stat named;
    errno = 0;
    if (!file->exists)
        return fstatat(dirfd, file->name, &named,
                       AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT;
    struct stat opened;
    return file->fd >= 0 && fstat(file->fd, &opened) == 0 &&
        fstatat(dirfd, file->name, &named, AT_SYMLINK_NOFOLLOW) == 0 &&
        identity_equal(&file->identity, &opened) &&
        identity_equal(&file->identity, &named);
}

static bool copy_exact_file(const struct preflight_source_file *source,
                            const char *destination)
{
    if (!source->exists)
        return true;
    int out = open(destination,
                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                   S_IRUSR | S_IWUSR);
    if (out < 0)
        return false;
    uint8_t buffer[64u * 1024u];
    off_t offset = 0;
    bool ok = true;
    while (offset < source->identity.st_size) {
        size_t want = sizeof(buffer);
        off_t left = source->identity.st_size - offset;
        if (left < (off_t)want)
            want = (size_t)left;
        ssize_t got = pread(source->fd, buffer, want, offset);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            ok = false;
            break;
        }
        size_t written = 0;
        while (written < (size_t)got) {
            ssize_t n = write(out, buffer + written, (size_t)got - written);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                ok = false;
                break;
            }
            written += (size_t)n;
        }
        if (!ok)
            break;
        offset += got;
    }
    if (ok && fsync(out) != 0)
        ok = false;
    if (close(out) != 0)
        ok = false;
    return ok;
}

static bool remove_temp_family(const char *dir, const char *main_path)
{
    static const char *const suffixes[] = {"", "-wal", "-shm", "-journal"};
    bool ok = true;
    char path[2304];
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        int n = snprintf(path, sizeof(path), "%s%s", main_path, suffixes[i]);
        if (n <= 0 || (size_t)n >= sizeof(path) ||
            (unlink(path) != 0 && errno != ENOENT))
            ok = false;
    }
    if (rmdir(dir) != 0)
        ok = false;
    return ok;
}

static bool inspect_snapshot(const char *path, char *reason,
                             size_t reason_size)
{
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX,
                             NULL);
    bool ok = rc == SQLITE_OK &&
        sqlite3_exec(db, "PRAGMA query_only=ON", NULL, NULL, NULL) == SQLITE_OK;
    if (ok)
        ok = mint_anchor_normal_boot_allowed(db, reason, reason_size);
    if (db && sqlite3_close(db) != SQLITE_OK)
        ok = false;
    return ok;
}

bool boot_mint_anchor_normal_boot_preflight(const char *datadir)
{
    if (!datadir || !datadir[0])
        LOG_FAIL(PREFLIGHT_SUBSYS, "normal boot preflight: missing datadir");
    int dirfd = open(datadir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0)
        LOG_FAIL(PREFLIGHT_SUBSYS, "normal boot preflight cannot open datadir");

    struct preflight_source_file files[] = {
        {.name = "progress.kv", .fd = -1},
        {.name = "progress.kv-wal", .fd = -1},
        {.name = "progress.kv-shm", .fd = -1},
    };
    bool ok = true;
    for (size_t i = 0; ok && i < sizeof(files) / sizeof(files[0]); i++)
        ok = source_open(dirfd, &files[i]);
    if (ok && !files[0].exists) {
        bool empty_family = !files[1].exists && !files[2].exists;
        for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
            if (files[i].fd >= 0)
                (void)close(files[i].fd);
        (void)close(dirfd);
        if (empty_family)
            return true;
        fprintf(stderr,
                "FATAL: normal node boot refused before datadir mutation: "
                "orphan progress.kv WAL/SHM without a main database\n");
        return false;
    }

    struct stat datadir_identity;
    struct stat tmp_identity;
    if (ok && (fstat(dirfd, &datadir_identity) != 0 ||
               stat("/tmp", &tmp_identity) != 0 ||
               (datadir_identity.st_dev == tmp_identity.st_dev &&
                datadir_identity.st_ino == tmp_identity.st_ino))) {
        LOG_WARN(PREFLIGHT_SUBSYS,
                 "preflight disposable root cannot be the node datadir");
        ok = false;
    }

    char temp_dir[] = "/tmp/zcl-preflight-XXXXXX";
    if (ok && !mkdtemp(temp_dir))
        ok = false;
    char main_copy[2304];
    char wal_copy[2304];
    int n1 = snprintf(main_copy, sizeof(main_copy), "%s/progress.kv", temp_dir);
    int n2 = snprintf(wal_copy, sizeof(wal_copy), "%s/progress.kv-wal", temp_dir);
    if (ok && (n1 <= 0 || (size_t)n1 >= sizeof(main_copy) ||
               n2 <= 0 || (size_t)n2 >= sizeof(wal_copy)))
        ok = false;
    if (ok)
        ok = copy_exact_file(&files[0], main_copy) &&
             copy_exact_file(&files[1], wal_copy);
    for (size_t i = 0; ok && i < sizeof(files) / sizeof(files[0]); i++)
        ok = source_unchanged(dirfd, &files[i]);

    char reason[512] = {0};
    if (ok)
        ok = inspect_snapshot(main_copy, reason, sizeof(reason));
    for (size_t i = 0; ok && i < sizeof(files) / sizeof(files[0]); i++)
        ok = source_unchanged(dirfd, &files[i]);

    bool cleanup_ok = temp_dir[0] && access(temp_dir, F_OK) == 0
        ? remove_temp_family(temp_dir, main_copy) : true;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        if (files[i].fd >= 0 && close(files[i].fd) != 0)
            ok = false;
    if (close(dirfd) != 0)
        ok = false;
    if (!cleanup_ok)
        ok = false;

    if (!ok) {
        fprintf(stderr,
                "FATAL: normal node boot refused before datadir mutation: %s\n",
                reason[0] ? reason :
                    "producer evidence snapshot could not be proven stable");
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "producer_evidence_preflight_refused reason=%s",
                    reason[0] ? reason : "stable_snapshot_inspection_failed");
    }
    return ok;
}
