/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Durable local key and delegation files for the ZCODE DHT. */

#include "vcs/zcode_dht_identity.h"

#include "crypto/ed25519.h"
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

static _Atomic uint64_t g_dht_identity_temp_sequence;

static bool io_error(char *out, size_t cap, const char *message)
{
    if (out && cap > 0)
        (void)snprintf(out, cap, "%s", message ? message : "identity I/O");
    return false;
}

static bool identity_paths(const char *datadir, char dir[1200],
                           const char *leaf, char path[1400],
                           char *err, size_t err_cap)
{
    if (!datadir || !datadir[0] || !leaf)
        return io_error(err, err_cap, "identity datadir is missing");
    char zcode[1150];
    int n = snprintf(zcode, sizeof(zcode), "%s/zcode", datadir);
    if (n <= 0 || (size_t)n >= sizeof(zcode) ||
        (mkdir(zcode, 0700) != 0 && errno != EEXIST))
        return io_error(err, err_cap, "cannot create zcode directory");
    n = snprintf(dir, 1200, "%s/%s", datadir, VCS_ZCODE_DHT_IDENTITY_DIR);
    if (n <= 0 || n >= 1200 ||
        (mkdir(dir, 0700) != 0 && errno != EEXIST))
        return io_error(err, err_cap, "cannot create DHT identity directory");
    n = snprintf(path, 1400, "%s/%s", dir, leaf);
    if (n <= 0 || n >= 1400)
        return io_error(err, err_cap, "DHT identity path too long");
    return true;
}

static bool exact_read_0600(const char *path, uint8_t *out, size_t bytes,
                            char *err, size_t err_cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return io_error(err, err_cap, "cannot open DHT identity file");
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size != (off_t)bytes || (st.st_mode & 0777) != 0600) {
        close(fd);
        return io_error(err, err_cap,
                        "DHT identity file has wrong size or permissions");
    }
    size_t off = 0;
    while (off < bytes) {
        ssize_t n = read(fd, out + off, bytes - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        off += (size_t)n;
    }
    uint8_t extra;
    ssize_t tail = read(fd, &extra, 1);
    close(fd);
    if (off != bytes || tail != 0) {
        memory_cleanse(out, bytes);
        return io_error(err, err_cap, "DHT identity read was not exact");
    }
    return true;
}

static bool atomic_write_0600(const char *dir, const char *path,
                              const uint8_t *bytes, size_t len,
                              char *err, size_t err_cap)
{
    uint64_t seq = atomic_fetch_add(&g_dht_identity_temp_sequence, 1);
    char temp[1500];
    int n = snprintf(temp, sizeof(temp), "%s.tmp.%ld.%llu", path,
                     (long)getpid(), (unsigned long long)seq);
    if (n <= 0 || (size_t)n >= sizeof(temp))
        return io_error(err, err_cap, "DHT identity temp path too long");
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return io_error(err, err_cap, "cannot create DHT identity temp");
    bool ok = zcl_write_all(fd, bytes, len) && fsync(fd) == 0 && close(fd) == 0;
    if (!ok) {
        (void)close(fd); (void)unlink(temp);
        return io_error(err, err_cap, "cannot persist DHT identity temp");
    }
    if (rename(temp, path) != 0) {
        (void)unlink(temp);
        return io_error(err, err_cap, "cannot publish DHT identity file");
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dfd < 0 || fsync(dfd) != 0) {
        if (dfd >= 0) close(dfd);
        return io_error(err, err_cap, "cannot fsync DHT identity directory");
    }
    close(dfd);
    return true;
}

bool vcs_zcode_dht_online_key_load_or_create(
    const char *datadir, uint8_t seed_out[32], uint8_t pubkey_out[32],
    char *err, size_t err_cap)
{
    if (!seed_out || !pubkey_out)
        return io_error(err, err_cap, "online key output is missing");
    memset(seed_out, 0, 32); memset(pubkey_out, 0, 32);
    char dir[1200], path[1400];
    if (!identity_paths(datadir, dir, VCS_ZCODE_DHT_ONLINE_KEY_FILE,
                        path, err, err_cap))
        return false;
    if (access(path, F_OK) == 0) {
        if (!exact_read_0600(path, seed_out, 32, err, err_cap)) return false;
    } else if (errno == ENOENT) {
        uint8_t fresh[32];
        if (!zcl_random_secret_bytes(fresh, 32, "zcode_dht_online") ||
            !atomic_write_0600(dir, path, fresh, 32, err, err_cap)) {
            memory_cleanse(fresh, 32); return false;
        }
        memcpy(seed_out, fresh, 32); memory_cleanse(fresh, 32);
    } else {
        return io_error(err, err_cap, "cannot inspect DHT online key");
    }
    uint8_t secret_copy[32];
    zcl_ed25519_keypair(pubkey_out, secret_copy, seed_out);
    memory_cleanse(secret_copy, sizeof(secret_copy));
    return true;
}

bool vcs_zcode_dht_online_key_load(
    const char *datadir, uint8_t seed_out[32], uint8_t pubkey_out[32],
    char *err, size_t err_cap)
{
    if (!seed_out || !pubkey_out)
        return io_error(err, err_cap, "online key output is missing");
    memset(seed_out, 0, 32);
    memset(pubkey_out, 0, 32);
    char dir[1200], path[1400];
    if (!identity_paths(datadir, dir, VCS_ZCODE_DHT_ONLINE_KEY_FILE,
                        path, err, err_cap) ||
        !exact_read_0600(path, seed_out, 32, err, err_cap))
        return false;
    uint8_t secret_copy[32];
    zcl_ed25519_keypair(pubkey_out, secret_copy, seed_out);
    memory_cleanse(secret_copy, sizeof(secret_copy));
    return true;
}

bool vcs_zcode_dht_delegation_save(
    const char *datadir, const struct vcs_zcode_dht_delegation *delegation,
    char *err, size_t err_cap)
{
    if (!delegation)
        return io_error(err, err_cap, "delegation is missing");
    uint8_t wire[VCS_ZCODE_DHT_DELEGATION_WIRE_BYTES];
    if (vcs_zcode_dht_delegation_encode(delegation, wire) !=
        VCS_ZCODE_DHT_DELEGATION_OK)
        return io_error(err, err_cap, "delegation does not encode");
    char dir[1200], path[1400];
    if (!identity_paths(datadir, dir, VCS_ZCODE_DHT_DELEGATION_FILE,
                        path, err, err_cap))
        return false;
    return atomic_write_0600(dir, path, wire, sizeof(wire), err, err_cap);
}

bool vcs_zcode_dht_delegation_load(
    const char *datadir, struct vcs_zcode_dht_delegation *out,
    char *err, size_t err_cap)
{
    if (!out) return io_error(err, err_cap, "delegation output is missing");
    uint8_t wire[VCS_ZCODE_DHT_DELEGATION_WIRE_BYTES];
    char dir[1200], path[1400];
    if (!identity_paths(datadir, dir, VCS_ZCODE_DHT_DELEGATION_FILE,
                        path, err, err_cap) ||
        !exact_read_0600(path, wire, sizeof(wire), err, err_cap))
        return false;
    enum vcs_zcode_dht_delegation_error parsed =
        vcs_zcode_dht_delegation_decode(out, wire, sizeof(wire));
    memory_cleanse(wire, sizeof(wire));
    if (parsed != VCS_ZCODE_DHT_DELEGATION_OK)
        return io_error(err, err_cap,
                        vcs_zcode_dht_delegation_error_string(parsed));
    return true;
}
