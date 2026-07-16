/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Resolve dev workspaces and publish monotonic sealed cycle state. */

#define _GNU_SOURCE
#include "devloop.h"

#include "crypto/sha3.h"
#include "json/json.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define CYCLE_CANONICAL_MAX ZCL_DEVLOOP_CYCLE_JSON_MAX
#define CYCLE_RECORD_MAX (ZCL_DEVLOOP_CYCLE_JSON_MAX + 4096)

static void set_why(char *why, size_t why_len, const char *value)
{
    if (why && why_len)
        (void)snprintf(why, why_len, "%s", value ? value : "");
}

static void hash_field(struct sha3_256_ctx *ctx, const char *name,
                       const char *value)
{
    const unsigned char zero = 0;
    sha3_256_write(ctx, (const unsigned char *)name, strlen(name));
    sha3_256_write(ctx, &zero, 1);
    sha3_256_write(ctx, (const unsigned char *)(value ? value : ""),
                   strlen(value ? value : ""));
    sha3_256_write(ctx, &zero, 1);
}

static void digest_hex(struct sha3_256_ctx *ctx, char out[65])
{
    static const char digits[] = "0123456789abcdef";
    unsigned char digest[32];
    sha3_256_finalize(ctx, digest);
    for (size_t i = 0; i < sizeof(digest); i++) {
        out[2 * i] = digits[digest[i] >> 4];
        out[2 * i + 1] = digits[digest[i] & 15];
    }
    out[64] = 0;
}

static bool valid_hex64(const char *value)
{
    if (!value || strlen(value) != 64)
        return false;
    return strspn(value, "0123456789abcdef") == 64;
}

static bool workspace_identity(const char *repo_root, char out[65])
{
    char canonical[PATH_MAX];
    struct stat st;
    if (!repo_root || !out || !realpath(repo_root, canonical) ||
        stat(canonical, &st) != 0 || !S_ISDIR(st.st_mode))
        return false;
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    hash_field(&ctx, "domain", "zcl.dev_workspace.v1");
    hash_field(&ctx, "canonical_root", canonical);
    digest_hex(&ctx, out);
    return true;
}

bool zcl_devloop_workspace_id(const char *repo_root, char out[65])
{
    return workspace_identity(repo_root, out);
}

bool zcl_devloop_workspace_resolve(const char *repo_root, char out_id[65],
                                   char *out_dir, size_t out_dir_len)
{
    const char *home = getenv("HOME");
    if (!home || !home[0] || !out_id || !out_dir || out_dir_len == 0 ||
        !workspace_identity(repo_root, out_id))
        return false;
    int n = snprintf(out_dir, out_dir_len,
                     "%s/.local/state/zclassic23-dev/workspaces/%s",
                     home, out_id);
    return n > 0 && (size_t)n < out_dir_len;
}

bool zcl_devloop_workspace_state_dir(const char *repo_root,
                                     char *out, size_t out_len)
{
    char workspace[65];
    return zcl_devloop_workspace_resolve(repo_root, workspace, out, out_len);
}

static bool mkdirs(const char *path)
{
    char copy[PATH_MAX];
    if (!path || !path[0] || strlen(path) >= sizeof(copy))
        return false;
    (void)snprintf(copy, sizeof(copy), "%s", path);
    for (char *p = copy + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(copy, 0700) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    return mkdir(copy, 0700) == 0 || errno == EEXIST;
}

static bool private_dir_fd(int fd)
{
    struct stat st;
    return fd >= 0 && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) &&
           st.st_uid == geteuid() && (st.st_mode & 0077) == 0;
}

static bool private_regular_fd(int fd, struct stat *st_out)
{
    struct stat st;
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
              st.st_uid == geteuid() && st.st_nlink == 1 &&
              (st.st_mode & 0077) == 0;
    if (ok && st_out)
        *st_out = st;
    return ok;
}

static int cycle_lock_open(int dirfd, bool create, int operation)
{
    int flags = (create ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW;
    if (create)
        flags |= O_CREAT;
    int fd = openat(dirfd, "cycle-state.lock", flags, 0600);
    if (!private_regular_fd(fd, NULL) || flock(fd, operation) != 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    return fd;
}

static bool write_all(int fd, const char *body, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, body + off, len - off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        off += (size_t)n;
    }
    return true;
}

static bool cycle_digest(const char *workspace, int64_t epoch,
                         const char *canonical,
                         char out[65])
{
    if (!valid_hex64(workspace) || epoch <= 0 || !canonical)
        return false;
    char epoch_text[32];
    int n = snprintf(epoch_text, sizeof(epoch_text), "%lld",
                     (long long)epoch);
    if (n <= 0 || (size_t)n >= sizeof(epoch_text))
        return false;
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    hash_field(&ctx, "domain", "zcl.dev_cycle_record.v1");
    hash_field(&ctx, "workspace_id", workspace);
    hash_field(&ctx, "epoch", epoch_text);
    hash_field(&ctx, "cycle_json", canonical);
    digest_hex(&ctx, out);
    return true;
}

static bool cycle_canonicalize(const struct json_value *cycle,
                               char out[CYCLE_CANONICAL_MAX], size_t *len_out)
{
    if (!cycle || cycle->type != JSON_OBJ || cycle->num_children == 0)
        return false;
    const struct json_value *schema = json_get(cycle, "schema");
    if (!schema || schema->type != JSON_STR ||
        strcmp(json_get_str(schema), "zcl.dev_cycle.v1") != 0 ||
        json_get(cycle, "epoch") != NULL)
        return false;
    size_t len = json_write(cycle, out, CYCLE_CANONICAL_MAX);
    if (len == 0 || len >= CYCLE_CANONICAL_MAX)
        return false;
    out[len] = 0;
    *len_out = len;
    return true;
}

static enum zcl_devloop_state_lookup cycle_record_read_at(
    int dirfd, const char *workspace, char *out, size_t out_len,
    size_t *len_out, int64_t *epoch_out)
{
    int fd = openat(dirfd, "native-cycle.json",
                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT)
        return ZCL_DEVLOOP_STATE_ABSENT;
    struct stat st;
    if (!private_regular_fd(fd, &st) || st.st_size <= 0 ||
        (uint64_t)st.st_size >= CYCLE_RECORD_MAX) {
        if (fd >= 0)
            close(fd);
        return ZCL_DEVLOOP_STATE_INVALID;
    }
    char body[CYCLE_RECORD_MAX];
    size_t need = (size_t)st.st_size, off = 0;
    while (off < need) {
        ssize_t n = pread(fd, body + off, need - off, (off_t)off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    close(fd);
    if (off != need)
        return ZCL_DEVLOOP_STATE_INVALID;

    struct json_value record;
    json_init(&record);
    if (!json_read(&record, body, off) || record.type != JSON_OBJ ||
        record.num_children != 5) {
        json_free(&record);
        return ZCL_DEVLOOP_STATE_INVALID;
    }
    const struct json_value *schema = json_get(&record, "schema");
    const struct json_value *stored_workspace =
        json_get(&record, "workspace_id");
    const struct json_value *stored_epoch = json_get(&record, "epoch");
    const struct json_value *cycle = json_get(&record, "cycle");
    const struct json_value *stored_digest = json_get(&record, "cycle_sha3");
    char canonical[CYCLE_CANONICAL_MAX], recomputed[65];
    size_t canonical_len = 0;
    int64_t epoch = stored_epoch && stored_epoch->type == JSON_INT
                        ? json_get_int(stored_epoch) : 0;
    bool ok = schema && schema->type == JSON_STR &&
              strcmp(json_get_str(schema), "zcl.dev_cycle_record.v1") == 0 &&
              stored_workspace && stored_workspace->type == JSON_STR &&
              strcmp(json_get_str(stored_workspace), workspace) == 0 &&
              epoch > 0 && stored_digest && stored_digest->type == JSON_STR &&
              valid_hex64(json_get_str(stored_digest)) &&
              cycle_canonicalize(cycle, canonical, &canonical_len) &&
              cycle_digest(workspace, epoch, canonical, recomputed) &&
              strcmp(recomputed, json_get_str(stored_digest)) == 0 &&
              canonical_len < out_len;
    if (ok) {
        memcpy(out, canonical, canonical_len);
        out[canonical_len] = 0;
        *len_out = canonical_len;
        *epoch_out = epoch;
    }
    json_free(&record);
    return ok ? ZCL_DEVLOOP_STATE_FOUND : ZCL_DEVLOOP_STATE_INVALID;
}

bool zcl_devloop_cycle_state_write(const char *repo_root,
                                   const char *cycle_json, size_t cycle_len,
                                   char *why, size_t why_len)
{
    if (why && why_len)
        why[0] = 0;
    char dir[PATH_MAX], workspace[65];
    if (!cycle_json || cycle_len == 0 ||
        !zcl_devloop_workspace_resolve(repo_root, workspace, dir,
                                       sizeof(dir)) ||
        !mkdirs(dir)) {
        set_why(why, why_len, "cycle_state_workspace_unavailable");
        return false;
    }
    struct json_value cycle;
    json_init(&cycle);
    if (!json_read(&cycle, cycle_json, cycle_len) || cycle.type != JSON_OBJ) {
        json_free(&cycle);
        set_why(why, why_len, "cycle_state_input_invalid");
        return false;
    }
    char canonical[CYCLE_CANONICAL_MAX];
    size_t canonical_len = 0;
    bool ok = cycle_canonicalize(&cycle, canonical, &canonical_len);
    int dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (!private_dir_fd(dirfd)) {
        if (dirfd >= 0)
            close(dirfd);
        json_free(&cycle);
        set_why(why, why_len, "cycle_state_directory_invalid");
        return false;
    }
    int lock_fd = cycle_lock_open(dirfd, true, LOCK_EX);
    if (lock_fd < 0) {
        close(dirfd);
        json_free(&cycle);
        set_why(why, why_len, "cycle_state_lock_invalid");
        return false;
    }

    char previous[CYCLE_CANONICAL_MAX];
    size_t previous_len = 0;
    int64_t previous_epoch = 0;
    enum zcl_devloop_state_lookup current = cycle_record_read_at(
        dirfd, workspace, previous, sizeof(previous), &previous_len,
        &previous_epoch);
    if (!ok || current == ZCL_DEVLOOP_STATE_INVALID ||
        (current == ZCL_DEVLOOP_STATE_FOUND && previous_epoch == INT64_MAX)) {
        close(lock_fd);
        close(dirfd);
        json_free(&cycle);
        set_why(why, why_len,
                !ok ? "cycle_state_record_overflow"
                    : current == ZCL_DEVLOOP_STATE_INVALID
                          ? "cycle_state_current_invalid"
                          : "cycle_state_epoch_exhausted");
        return false;
    }
    int64_t epoch = current == ZCL_DEVLOOP_STATE_FOUND
                        ? previous_epoch + 1 : 1;
    char digest[65];
    ok = cycle_digest(workspace, epoch, canonical, digest);
    struct json_value record;
    json_init(&record);
    json_set_object(&record);
    ok = ok &&
         json_push_kv_str(&record, "schema", "zcl.dev_cycle_record.v1") &&
         json_push_kv_str(&record, "workspace_id", workspace) &&
         json_push_kv_int(&record, "epoch", epoch) &&
         json_push_kv(&record, "cycle", &cycle) &&
         json_push_kv_str(&record, "cycle_sha3", digest);
    json_free(&cycle);
    char body[CYCLE_RECORD_MAX];
    size_t body_len = ok ? json_write(&record, body, sizeof(body) - 2) : 0;
    json_free(&record);
    if (!ok || body_len == 0 || body_len >= sizeof(body) - 2) {
        close(lock_fd);
        close(dirfd);
        set_why(why, why_len, "cycle_state_record_overflow");
        return false;
    }
    body[body_len++] = '\n';

    char temp[96] = {0};
    int fd = -1;
    for (unsigned attempt = 0; attempt < 100; attempt++) {
        int n = snprintf(temp, sizeof(temp), ".cycle.%ld.%u.tmp",
                         (long)getpid(), attempt);
        if (n <= 0 || (size_t)n >= sizeof(temp))
            break;
        fd = openat(dirfd, temp,
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                    0600);
        if (fd >= 0 || errno != EEXIST)
            break;
    }
    ok = private_regular_fd(fd, NULL) && write_all(fd, body, body_len) &&
         fsync(fd) == 0;
    if (fd >= 0 && close(fd) != 0)
        ok = false;
    if (ok)
        ok = renameat(dirfd, temp, dirfd, "native-cycle.json") == 0 &&
             fsync(dirfd) == 0;
    if (!ok)
        (void)unlinkat(dirfd, temp, 0);
    close(lock_fd);
    close(dirfd);
    if (!ok)
        set_why(why, why_len, "cycle_state_publication_failed");
    return ok;
}

enum zcl_devloop_state_lookup zcl_devloop_cycle_state_read(
    const char *repo_root, char *out, size_t out_len, size_t *len_out,
    int64_t *epoch_out, char *why, size_t why_len)
{
    if (why && why_len)
        why[0] = 0;
    if (!out || out_len < 2 || !len_out) {
        set_why(why, why_len, "cycle_state_output_invalid");
        return ZCL_DEVLOOP_STATE_INVALID;
    }
    *len_out = 0;
    if (epoch_out)
        *epoch_out = 0;
    char dir[PATH_MAX], workspace[65];
    if (!zcl_devloop_workspace_resolve(repo_root, workspace, dir,
                                       sizeof(dir))) {
        set_why(why, why_len, "cycle_state_workspace_unavailable");
        return ZCL_DEVLOOP_STATE_INVALID;
    }
    int dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0 && errno == ENOENT)
        return ZCL_DEVLOOP_STATE_ABSENT;
    if (!private_dir_fd(dirfd)) {
        if (dirfd >= 0)
            close(dirfd);
        set_why(why, why_len, "cycle_state_directory_invalid");
        return ZCL_DEVLOOP_STATE_INVALID;
    }
    int lock_fd = cycle_lock_open(dirfd, false, LOCK_SH);
    if (lock_fd < 0) {
        char probe[CYCLE_CANONICAL_MAX];
        size_t probe_len = 0;
        int64_t probe_epoch = 0;
        enum zcl_devloop_state_lookup probe_result = cycle_record_read_at(
            dirfd, workspace, probe, sizeof(probe), &probe_len, &probe_epoch);
        if (probe_result == ZCL_DEVLOOP_STATE_ABSENT) {
            close(dirfd);
            return ZCL_DEVLOOP_STATE_ABSENT;
        }
        /* A first writer may have created the lock between the initial miss
         * and the state probe. Retry once; a record without its protocol lock
         * is otherwise untrusted. */
        lock_fd = cycle_lock_open(dirfd, false, LOCK_SH);
        if (lock_fd < 0) {
            close(dirfd);
            set_why(why, why_len, "cycle_state_lock_missing_or_invalid");
            return ZCL_DEVLOOP_STATE_INVALID;
        }
    }
    int64_t epoch = 0;
    enum zcl_devloop_state_lookup result = cycle_record_read_at(
        dirfd, workspace, out, out_len, len_out, &epoch);
    close(lock_fd);
    close(dirfd);
    if (result == ZCL_DEVLOOP_STATE_INVALID) {
        set_why(why, why_len, "cycle_state_integrity_invalid");
    } else if (result == ZCL_DEVLOOP_STATE_FOUND && epoch_out) {
        *epoch_out = epoch;
    }
    return result;
}
