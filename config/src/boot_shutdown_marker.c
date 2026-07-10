/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "config/boot_shutdown_marker.h"

#include "event/event.h"
#include "platform/time_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ZCL_SHUTDOWN_MARKER_NAME ".shutdown_clean"

/* ── Module-level cache ───────────────────────────────────────────────
 * detect_unclean() parses the marker's v2 binding into g_cached_binding
 * before it unlinks the on-disk file. node_db_open's probe consumes it
 * (single-use). g_schema_version is the value baked into the next marker. */
static struct shutdown_clean_binding g_cached_binding;   /* valid=false at start */
static int                           g_schema_version = -1;
static _Atomic bool                  g_quick_check_skipped;

static bool boot_shutdown_marker_path(char *out, size_t out_n,
                                      const char *datadir,
                                      const char *name)
{
    if (!out || out_n == 0 || !datadir || !*datadir || !name || !*name)
        return false;

    int n = snprintf(out, out_n, "%s/%s", datadir, name);
    return n >= 0 && (size_t)n < out_n;
}

/* ── Pure format/parse/decide helpers ─────────────────────────────── */

int boot_shutdown_marker_format(char *buf, size_t n, int64_t unix_seconds,
                                const struct shutdown_clean_binding *b)
{
    if (!buf || n == 0 || !b)
        return -1;
    if (!b->valid) {
        /* Legacy timestamp-only marker (node.db identity unavailable). */
        return snprintf(buf, n, "%lld\n", (long long)unix_seconds);
    }
    return snprintf(buf, n,
                    "%lld\n"
                    "magic=ZCLSHUT\n"
                    "version=2\n"
                    "node_db_size=%lld\n"
                    "node_db_change_counter=%u\n"
                    "node_db_version_valid_for=%u\n"
                    "schema_version=%d\n"
                    "wal_checkpointed=1\n",
                    (long long)unix_seconds,
                    (long long)b->node_db_size,
                    b->change_counter,
                    b->version_valid_for,
                    b->schema_version);
}

/* Return the value string for `key=` if present on any line of [text,text+len),
 * copied into `val` (NUL-terminated). Returns true if found. */
static bool marker_find_kv(const char *text, size_t len,
                           const char *key, char *val, size_t val_n)
{
    size_t key_len = strlen(key);
    size_t i = 0;
    while (i < len) {
        size_t line_start = i;
        while (i < len && text[i] != '\n')
            i++;
        size_t line_len = i - line_start;
        if (i < len)
            i++; /* skip '\n' */
        if (line_len > key_len &&
            strncmp(text + line_start, key, key_len) == 0 &&
            text[line_start + key_len] == '=') {
            const char *v = text + line_start + key_len + 1;
            size_t v_len = line_len - key_len - 1;
            if (v_len >= val_n)
                v_len = val_n - 1;
            memcpy(val, v, v_len);
            val[v_len] = '\0';
            return true;
        }
    }
    return false;
}

bool boot_shutdown_marker_parse(const char *text, size_t len,
                                struct shutdown_clean_binding *out)
{
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!text || len == 0)
        return false;

    char v[64];

    if (!marker_find_kv(text, len, "magic", v, sizeof(v)) ||
        strcmp(v, "ZCLSHUT") != 0)
        return false;
    if (!marker_find_kv(text, len, "version", v, sizeof(v)) ||
        strcmp(v, "2") != 0)
        return false;
    if (!marker_find_kv(text, len, "wal_checkpointed", v, sizeof(v)) ||
        strcmp(v, "1") != 0)
        return false;

    if (!marker_find_kv(text, len, "node_db_size", v, sizeof(v)))
        return false;
    errno = 0;
    char *end = NULL;
    long long sz = strtoll(v, &end, 10);
    if (errno != 0 || end == v || sz < 0)
        return false;
    out->node_db_size = (int64_t)sz;

    if (!marker_find_kv(text, len, "node_db_change_counter", v, sizeof(v)))
        return false;
    errno = 0;
    unsigned long cc = strtoul(v, &end, 10);
    if (errno != 0 || end == v)
        return false;
    out->change_counter = (uint32_t)cc;

    if (!marker_find_kv(text, len, "node_db_version_valid_for", v, sizeof(v)))
        return false;
    errno = 0;
    unsigned long vvf = strtoul(v, &end, 10);
    if (errno != 0 || end == v)
        return false;
    out->version_valid_for = (uint32_t)vvf;

    /* schema_version is advisory; default -1 if absent/garbage. */
    out->schema_version = -1;
    if (marker_find_kv(text, len, "schema_version", v, sizeof(v))) {
        errno = 0;
        long sv = strtol(v, &end, 10);
        if (errno == 0 && end != v)
            out->schema_version = (int)sv;
    }

    out->valid = true;
    return true;
}

bool boot_shutdown_marker_can_skip(const struct shutdown_clean_binding *b,
                                   const struct node_db_file_identity *cur)
{
    if (!b || !cur || !b->valid || !cur->present)
        return false;
    if (cur->wal_present)
        return false;
    if (cur->size != b->node_db_size)
        return false;
    if (cur->change_counter != b->change_counter)
        return false;
    if (cur->version_valid_for != b->version_valid_for)
        return false;
    return true;
}

/* ── File identity read (pristine, pre-open) ──────────────────────── */

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

bool node_db_file_identity_read(const char *node_db_path,
                                struct node_db_file_identity *out)
{
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!node_db_path || !*node_db_path)
        return false;

    int fd = open(node_db_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;

    struct stat st;
    uint8_t hdr[100];
    bool ok = false;
    if (fstat(fd, &st) == 0 && st.st_size >= (off_t)sizeof(hdr)) {
        ssize_t r = pread(fd, hdr, sizeof(hdr), 0);
        if (r == (ssize_t)sizeof(hdr) &&
            memcmp(hdr, "SQLite format 3\0", 16) == 0) {
            out->present = true;
            out->size = (int64_t)st.st_size;
            out->change_counter = be32(hdr + 24);
            out->version_valid_for = be32(hdr + 92);
            ok = true;
        }
    }
    close(fd);

    if (ok) {
        /* WAL present iff <node.db>-wal exists with size > 0. */
        char wal[1200];
        int n = snprintf(wal, sizeof(wal), "%s-wal", node_db_path);
        if (n > 0 && (size_t)n < sizeof(wal)) {
            struct stat wst;
            out->wal_present = (stat(wal, &wst) == 0 && wst.st_size > 0);
        }
    }
    return ok;
}

/* ── Boot lifecycle ───────────────────────────────────────────────── */

void boot_shutdown_marker_set_schema_version(int schema_version)
{
    g_schema_version = schema_version;
}

bool boot_shutdown_marker_quick_check_was_skipped(void)
{
    return atomic_load(&g_quick_check_skipped);
}

void boot_shutdown_marker_reset_for_test(void)
{
    memset(&g_cached_binding, 0, sizeof(g_cached_binding));
    g_schema_version = -1;
    atomic_store(&g_quick_check_skipped, false);
}

bool boot_shutdown_marker_quick_check_probe(const char *node_db_path)
{
    /* Single-use: consume the cached binding regardless of outcome. */
    struct shutdown_clean_binding b = g_cached_binding;
    memset(&g_cached_binding, 0, sizeof(g_cached_binding));

    if (!b.valid)
        return false;

    struct node_db_file_identity cur;
    if (!node_db_file_identity_read(node_db_path, &cur))
        return false;

    if (!boot_shutdown_marker_can_skip(&b, &cur))
        return false;

    atomic_store(&g_quick_check_skipped, true);
    return true;
}

bool boot_shutdown_marker_detect_unclean(const char *datadir)
{
    char marker_path[1024];
    char wal_path[1024];
    if (!boot_shutdown_marker_path(marker_path, sizeof(marker_path),
                                   datadir, ZCL_SHUTDOWN_MARKER_NAME) ||
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

    /* Cache the v2 content-binding (if any) BEFORE unlinking, so
     * node_db_open's quick_check-skip probe can consult it. A parse failure
     * or a WAL present → no cached binding → quick_check runs as before. */
    memset(&g_cached_binding, 0, sizeof(g_cached_binding));
    if (marker_exists && !wal_exists) {
        FILE *mf = fopen(marker_path, "r");
        if (mf) {
            char buf[512];
            size_t rd = fread(buf, 1, sizeof(buf) - 1, mf);
            fclose(mf);
            struct shutdown_clean_binding parsed;
            if (boot_shutdown_marker_parse(buf, rd, &parsed))
                g_cached_binding = parsed;
        }
    }

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
    char tmp[1088];
    char node_db_path[1088];
    if (!boot_shutdown_marker_path(path, sizeof(path),
                                   datadir, ZCL_SHUTDOWN_MARKER_NAME))
        return false;

    int tn = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int dn = snprintf(node_db_path, sizeof(node_db_path), "%s/node.db", datadir);
    if (tn < 0 || (size_t)tn >= sizeof(tmp) ||
        dn < 0 || (size_t)dn >= sizeof(node_db_path))
        return false;

    /* Bind the marker to node.db's final on-disk identity. The DB has already
     * been WAL-checkpointed and closed by the shutdown path, so the header we
     * read here is exactly what the next boot will see before it opens the
     * file. If node.db is missing/unreadable we still write a legacy
     * timestamp-only marker (no skip binding) — identical to prior behavior. */
    struct node_db_file_identity id;
    struct shutdown_clean_binding b;
    memset(&b, 0, sizeof(b));
    if (node_db_file_identity_read(node_db_path, &id) && !id.wal_present) {
        b.valid = true;
        b.node_db_size = id.size;
        b.change_counter = id.change_counter;
        b.version_valid_for = id.version_valid_for;
        b.schema_version = g_schema_version;
    }

    char content[512];
    int clen = boot_shutdown_marker_format(content, sizeof(content),
                                           (int64_t)platform_time_wall_time_t(),
                                           &b);
    if (clen < 0 || (size_t)clen >= sizeof(content))
        return false;

    /* Crash-safe write: temp file + fsync + atomic rename. */
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;
    bool ok = false;
    ssize_t w = write(fd, content, (size_t)clen);
    if (w == (ssize_t)clen && fsync(fd) == 0)
        ok = true;
    if (close(fd) != 0)
        ok = false;
    if (!ok) {
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }

    /* Best-effort directory fsync so the rename is durable. */
    int dfd = open(datadir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        (void)fsync(dfd);
        close(dfd);
    }
    return true;
}
