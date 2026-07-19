/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "config/boot_datadir_lock.h"

#include "util/hw_bench.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int g_pidfile_fd = -1;

static bool write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

static long lock_holder_pid(int fd)
{
    char buf[32] = {0};
    ssize_t n;
    do {
        n = pread(fd, buf, sizeof(buf) - 1, 0);
    } while (n < 0 && errno == EINTR);
    if (n <= 0)
        return 0;

    char *end = NULL;
    errno = 0;
    long pid = strtol(buf, &end, 10);
    if (errno != 0 || end == buf || pid <= 0)
        return 0;
    return pid;
}

bool boot_datadir_lock_acquire(const char *datadir)
{
    if (!datadir || !*datadir) {
        fprintf(stderr, "[boot] Cannot acquire datadir lock: empty datadir\n");
        return false;
    }
    if (g_pidfile_fd >= 0) {
        fprintf(stderr,
                "[boot] Cannot acquire datadir lock: this process already "
                "holds one\n");
        return false;
    }

    int dir_fd = open(datadir,
                      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir_fd < 0) {
        fprintf(stderr, "[boot] Cannot open data directory %s: %s\n",
                datadir, strerror(errno));
        return false;
    }

    int fd = openat(dir_fd, "zclassic23.pid",
                    O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
                    0600);
    if (fd < 0) {
        int saved_errno = errno;
        close(dir_fd);
        fprintf(stderr, "[boot] Cannot open datadir lock in %s: %s\n",
                datadir, strerror(saved_errno));
        return false;
    }

    struct stat st;
    int stat_rc = fstat(fd, &st);
    if (stat_rc != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
        int saved_errno = stat_rc != 0 ? errno : EINVAL;
        close(fd);
        close(dir_fd);
        fprintf(stderr,
                "[boot] Datadir lock in %s is not a private regular file: "
                "%s\n", datadir, strerror(saved_errno));
        return false;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int saved_errno = errno;
        long holder = lock_holder_pid(fd);
        close(fd);
        close(dir_fd);
        if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN) {
            if (holder > 0) {
                fprintf(stderr,
                        "[boot] Data directory locked by PID %ld. Cannot "
                        "start.\n", holder);
            } else {
                fprintf(stderr,
                        "[boot] Data directory is locked by another process. "
                        "Cannot start.\n");
            }
        } else {
            fprintf(stderr, "[boot] Cannot lock data directory %s: %s\n",
                    datadir, strerror(saved_errno));
        }
        return false;
    }

    char pid_text[32];
    int pid_len = snprintf(pid_text, sizeof(pid_text), "%ld\n",
                           (long)getpid());
    bool pid_fits = pid_len > 0 && (size_t)pid_len < sizeof(pid_text);
    if (!pid_fits)
        errno = EOVERFLOW;
    bool recorded = pid_fits && ftruncate(fd, 0) == 0 &&
                    lseek(fd, 0, SEEK_SET) == 0 &&
                    write_all(fd, pid_text, (size_t)pid_len) && fsync(fd) == 0 &&
                    fsync(dir_fd) == 0;
    if (!recorded) {
        int saved_errno = errno;
        (void)flock(fd, LOCK_UN);
        close(fd);
        close(dir_fd);
        fprintf(stderr, "[boot] Cannot persist datadir lock in %s: %s\n",
                datadir, strerror(saved_errno ? saved_errno : EIO));
        return false;
    }

    close(dir_fd);
    g_pidfile_fd = fd;

    /* This is the earliest point boot has exclusive, safe possession of the
     * datadir — before crypto/wallet/progress-store setup and well before
     * the block-ingest reducer can ever run. Kick the hardware-bench
     * organ's one-time (possibly synchronous, up to ~300ms) fsync/pread
     * probe HERE so hw_bench_batch_size() (reducer_drain.c, called on
     * every reducer_kick under ctl->mutex) never has to run it lazily on
     * that hot path — see hw_bench.h's CALLER CONTRACT. Idempotent: a
     * concurrent/earlier hw_bench_init() (e.g. bg_validation_init) just
     * observes the cached result. */
    hw_bench_init(datadir);

    return true;
}

void boot_datadir_lock_release(void)
{
    if (g_pidfile_fd < 0)
        return;

    int fd = g_pidfile_fd;
    g_pidfile_fd = -1;
    if (flock(fd, LOCK_UN) != 0) {
        fprintf(stderr, "[boot] Cannot explicitly unlock data directory: %s\n",
                strerror(errno));
    }
    if (close(fd) != 0) {
        fprintf(stderr, "[boot] Cannot close datadir lock descriptor: %s\n",
                strerror(errno));
    }
}
