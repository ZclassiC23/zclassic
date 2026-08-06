/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Canonical persistent Noise X25519 static-key loader. */

#include "net/v2_identity.h"

#include "crypto/curve25519.h"
#include "crypto/random_secret.h"
#include "support/cleanse.h"
#include "util/write_all.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static _Atomic uint64_t g_v2_identity_temp_sequence;

static bool identity_error(char *out, size_t cap, const char *what)
{
    if (out && cap > 0)
        (void)snprintf(out, cap, "%s", what ? what : "identity failure");
    return false;
}

static bool identity_read(const char *path, uint8_t private_out[32],
                          uint8_t public_out[32], char *err, size_t err_cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != 32 ||
        (st.st_mode & 0777) != 0600) {
        close(fd);
        return identity_error(err, err_cap,
                              "v2 identity must be a 32-byte mode-0600 file");
    }
    uint8_t extra = 0;
    ssize_t n = read(fd, private_out, 32);
    ssize_t tail = read(fd, &extra, 1);
    int saved = errno;
    close(fd);
    if (n != 32 || tail != 0) {
        memory_cleanse(private_out, 32);
        errno = saved;
        return identity_error(err, err_cap, "v2 identity read was not exact");
    }
    if (!curve25519_scalarmult_base(public_out, private_out)) {
        memory_cleanse(private_out, 32);
        return identity_error(err, err_cap, "v2 identity key is invalid");
    }
    return true;
}

static bool identity_write(const char *dir, const char *path,
                           const uint8_t private_key[32], char *err,
                           size_t err_cap)
{
    uint64_t seq = atomic_fetch_add(&g_v2_identity_temp_sequence, 1);
    char temp[1280];
    int n = snprintf(temp, sizeof(temp), "%s.tmp.%ld.%llu", path,
                     (long)getpid(), (unsigned long long)seq);
    if (n <= 0 || (size_t)n >= sizeof(temp))
        return identity_error(err, err_cap, "v2 identity temp path too long");
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return identity_error(err, err_cap, "cannot create v2 identity temp");
    bool ok = zcl_write_all(fd, private_key, 32) && fsync(fd) == 0 &&
              close(fd) == 0;
    if (!ok) {
        (void)close(fd);
        (void)unlink(temp);
        return identity_error(err, err_cap, "cannot persist v2 identity temp");
    }
    if (rename(temp, path) != 0) {
        (void)unlink(temp);
        return identity_error(err, err_cap, "cannot publish v2 identity");
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dfd < 0 || fsync(dfd) != 0) {
        if (dfd >= 0) close(dfd);
        return identity_error(err, err_cap,
                              "cannot fsync v2 identity directory");
    }
    close(dfd);
    return true;
}

bool v2_identity_load_or_create(const char *datadir,
                                uint8_t private_out[32],
                                uint8_t public_out[32],
                                char *err, size_t err_cap)
{
    if (!datadir || !datadir[0] || !private_out || !public_out)
        return identity_error(err, err_cap, "v2 identity argument missing");
    memset(private_out, 0, 32);
    memset(public_out, 0, 32);
    char path[1152];
    int n = snprintf(path, sizeof(path), "%s/v2_identity.key", datadir);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return identity_error(err, err_cap, "v2 identity path too long");

    if (access(path, F_OK) == 0)
        return identity_read(path, private_out, public_out, err, err_cap);
    if (errno != ENOENT)
        return identity_error(err, err_cap, "cannot inspect v2 identity");

    uint8_t fresh[32];
    if (!zcl_random_secret_bytes(fresh, sizeof(fresh), "v2_identity") ||
        !curve25519_scalarmult_base(public_out, fresh)) {
        memory_cleanse(fresh, sizeof(fresh));
        return identity_error(err, err_cap, "v2 identity generation failed");
    }
    if (!identity_write(datadir, path, fresh, err, err_cap)) {
        memory_cleanse(fresh, sizeof(fresh));
        memset(public_out, 0, 32);
        return false;
    }
    memcpy(private_out, fresh, 32);
    memory_cleanse(fresh, sizeof(fresh));
    return true;
}
