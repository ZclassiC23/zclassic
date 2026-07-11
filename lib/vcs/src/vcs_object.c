/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * vcs_object — implementation. See vcs/vcs_object.h.
 *
 * Layout: <repo_root>/.zvcs/objects/<hh>/<the-other-62-hex>, where the full
 * 64-hex string is the object id. For blobs and commits the id is
 * SHA3-256(tag || content) and reads recompute + verify it. Manifests are
 * addressed by their structural tree_hash (see vcs_manifest_tree_hash) rather
 * than the raw-byte hash, so they use the *_addressed / *_load_raw helpers and
 * the manifest layer re-derives + verifies the tree_hash on read (still
 * recompute-never-trust, just against the structural commitment).
 *
 * A put writes to objects/tmp/.<pid>.<seq> then fsyncs and renames into place,
 * so a reader never observes a half-written object; an existence check up front
 * skips re-writing an object that is already present (content-addressed => same
 * name means same bytes). */

#include "vcs/vcs_object.h"

#include "vcs_priv.h"

#include "platform/time_compat.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define VCS_OBJECT_PATH_MAX 4096
/* Defensive cap on a single object read into RAM (256 MiB), mirroring the
 * event log's payload ceiling. A larger file is treated as corruption. */
#define VCS_OBJECT_MAX_BYTES ((size_t)(256u * 1024u * 1024u))

/* Build "<repo_root>/.zvcs/<suffix>" into out. suffix "" => the .zvcs dir. */
static bool zvcs_path(const char *repo_root, const char *suffix,
                      char *out, size_t cap)
{
    int n;
    if (suffix && suffix[0])
        n = snprintf(out, cap, "%s/.zvcs/%s", repo_root, suffix);
    else
        n = snprintf(out, cap, "%s/.zvcs", repo_root);
    return n > 0 && (size_t)n < cap;
}

static bool ensure_dir(const char *path)
{
    if (mkdir(path, 0700) == 0)
        return true;
    return errno == EEXIST;
}

bool vcs_object_store_init(const char *repo_root)
{
    if (!repo_root || !repo_root[0])
        LOG_FAIL("vcs", "null repo_root");
    char path[VCS_OBJECT_PATH_MAX];
    if (!zvcs_path(repo_root, "", path, sizeof(path)))
        LOG_FAIL("vcs", "path too long: %s", repo_root);
    if (!ensure_dir(path))
        LOG_FAIL("vcs", "mkdir %s: %s", path, strerror(errno));
    if (!zvcs_path(repo_root, "objects", path, sizeof(path)) || !ensure_dir(path))
        LOG_FAIL("vcs", "mkdir objects: %s", strerror(errno));
    if (!zvcs_path(repo_root, "objects/tmp", path, sizeof(path)) || !ensure_dir(path))
        LOG_FAIL("vcs", "mkdir objects/tmp: %s", strerror(errno));
    return true;
}

/* objects/<hh>/<62hex> for a 32-byte address. */
static bool object_path(const char *repo_root, const uint8_t addr[32],
                        char *out, size_t cap)
{
    char hex[65];
    vcs_hex32(addr, hex);
    char suffix[80];
    int n = snprintf(suffix, sizeof(suffix), "objects/%c%c/%s",
                     hex[0], hex[1], hex + 2);
    if (n <= 0 || (size_t)n >= sizeof(suffix))
        return false;
    return zvcs_path(repo_root, suffix, out, cap);
}

bool vcs_object_has(const char *repo_root, const uint8_t hash[32])
{
    if (!repo_root || !hash)
        return false;
    char path[VCS_OBJECT_PATH_MAX];
    if (!object_path(repo_root, hash, path, sizeof(path)))
        return false;
    return access(path, F_OK) == 0;
}

/* Write content[0..len) into the object addressed by addr, atomically and
 * idempotently. Shared by the content-addressed put and the manifest's
 * structural-address put. */
static bool object_write(const char *repo_root, const uint8_t addr[32],
                         const uint8_t *content, size_t len)
{
    char final[VCS_OBJECT_PATH_MAX];
    if (!object_path(repo_root, addr, final, sizeof(final)))
        LOG_FAIL("vcs", "object path too long");
    if (access(final, F_OK) == 0)
        return true;  /* dedup */

    char hex[65];
    vcs_hex32(addr, hex);
    char shard[80];
    int sn = snprintf(shard, sizeof(shard), "objects/%c%c", hex[0], hex[1]);
    if (sn <= 0 || (size_t)sn >= sizeof(shard))
        LOG_FAIL("vcs", "shard path too long");
    char sharddir[VCS_OBJECT_PATH_MAX];
    if (!zvcs_path(repo_root, shard, sharddir, sizeof(sharddir)) ||
        !ensure_dir(sharddir))
        LOG_FAIL("vcs", "mkdir shard %s: %s", sharddir, strerror(errno));

    static _Atomic uint64_t g_seq = 0;
    uint64_t seq = atomic_fetch_add(&g_seq, 1);
    int64_t mono_ns = platform_time_monotonic_us();
    char tmpsuffix[128];
    int tn = snprintf(tmpsuffix, sizeof(tmpsuffix),
                      "objects/tmp/.put.%ld.%llu.%lld", (long)getpid(),
                      (unsigned long long)seq, (long long)mono_ns);
    if (tn <= 0 || (size_t)tn >= sizeof(tmpsuffix))
        LOG_FAIL("vcs", "tmp path too long");
    char tmp[VCS_OBJECT_PATH_MAX];
    if (!zvcs_path(repo_root, tmpsuffix, tmp, sizeof(tmp)))
        LOG_FAIL("vcs", "tmp path too long");

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        LOG_FAIL("vcs", "open tmp %s: %s", tmp, strerror(errno));

    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, content + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd); unlink(tmp);
            LOG_FAIL("vcs", "write tmp: %s", strerror(errno));
        }
        off += (size_t)w;
    }
    if (fsync(fd) != 0) {
        close(fd); unlink(tmp);
        LOG_FAIL("vcs", "fsync tmp: %s", strerror(errno));
    }
    if (close(fd) != 0) {
        unlink(tmp);
        LOG_FAIL("vcs", "close tmp: %s", strerror(errno));
    }
    if (rename(tmp, final) != 0) {
        if (access(final, F_OK) == 0) { unlink(tmp); return true; }
        unlink(tmp);
        LOG_FAIL("vcs", "rename %s -> %s: %s", tmp, final, strerror(errno));
    }
    return true;
}

bool vcs_object_put(const char *repo_root, const uint8_t *content, size_t len,
                    uint8_t tag, uint8_t out_hash[32])
{
    if (!repo_root || !out_hash || (len > 0 && !content))
        LOG_FAIL("vcs", "null arg to object_put");
    uint8_t hash[32];
    vcs_sha3_tag(tag, content, len, hash);
    memcpy(out_hash, hash, 32);
    return object_write(repo_root, hash, content, len);
}

bool vcs_object_put_addressed(const char *repo_root, const uint8_t address[32],
                              const uint8_t *content, size_t len)
{
    if (!repo_root || !address || (len > 0 && !content))
        LOG_FAIL("vcs", "null arg to object_put_addressed");
    return object_write(repo_root, address, content, len);
}

/* Read the whole object file at addr into *out (caller frees). No hash
 * verification here — the caller owns it. */
static int object_read(const char *repo_root, const uint8_t addr[32],
                       uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    char path[VCS_OBJECT_PATH_MAX];
    if (!object_path(repo_root, addr, path, sizeof(path)))
        LOG_ERR("vcs", "object path too long");
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        LOG_ERR("vcs", "open %s: %s", path, strerror(errno));
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        LOG_ERR("vcs", "fstat %s: %s", path, strerror(errno));
    }
    if (st.st_size < 0 || (size_t)st.st_size > VCS_OBJECT_MAX_BYTES) {
        close(fd);
        LOG_ERR("vcs", "object too large: %lld", (long long)st.st_size);
    }
    size_t len = (size_t)st.st_size;
    uint8_t *buf = NULL;
    if (len > 0) {
        buf = zcl_malloc(len, "vcs_object_read");
        if (!buf) { close(fd); LOG_ERR("vcs", "malloc %zu", len); }
        size_t off = 0;
        while (off < len) {
            ssize_t r = read(fd, buf + off, len - off);
            if (r < 0) {
                if (errno == EINTR) continue;
                free(buf); close(fd);
                LOG_ERR("vcs", "read %s: %s", path, strerror(errno));
            }
            if (r == 0) break;
            off += (size_t)r;
        }
        if (off != len) {
            free(buf); close(fd);
            LOG_ERR("vcs", "short read %zu/%zu on %s", off, len, path);
        }
    }
    close(fd);
    *out = buf;
    *out_len = len;
    return 0;
}

int vcs_object_get(const char *repo_root, const uint8_t hash[32], uint8_t tag,
                   uint8_t **out_content, size_t *out_len)
{
    if (out_content) *out_content = NULL;
    if (out_len) *out_len = 0;
    if (!repo_root || !hash || !out_content || !out_len)
        LOG_ERR("vcs", "null arg to object_get");
    uint8_t *buf = NULL;
    size_t len = 0;
    if (object_read(repo_root, hash, &buf, &len) != 0)
        return -1;
    uint8_t recomputed[32];
    vcs_sha3_tag(tag, buf, len, recomputed);
    if (memcmp(recomputed, hash, 32) != 0) {
        free(buf);
        LOG_ERR("vcs", "hash mismatch reading object (corruption)");
    }
    *out_content = buf;
    *out_len = len;
    return 0;
}

int vcs_object_load_raw(const char *repo_root, const uint8_t address[32],
                        uint8_t **out_content, size_t *out_len)
{
    if (out_content) *out_content = NULL;
    if (out_len) *out_len = 0;
    if (!repo_root || !address || !out_content || !out_len)
        LOG_ERR("vcs", "null arg to object_load_raw");
    return object_read(repo_root, address, out_content, out_len);
}
