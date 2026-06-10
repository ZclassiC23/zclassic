/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * sha3_sidecar_io — see storage/sha3_sidecar_io.h for the contract.
 *
 * This is the single source of truth for the body+sidecar hashing,
 * atomic-write, header-parse, and quarantine logic shared by
 * net/addrman_integrity (peers.dat) and block_index_sidecar_integrity
 * (block_index.bin). Those modules keep their public aii_* / bii_*
 * functions as thin wrappers that pass a `struct ssio_spec`.
 *
 * The streaming hash uses a 1 MiB window so we don't need to mmap or
 * load the whole file. The body files are tiny in practice but the
 * same code path is exercised by tests that synthesise larger inputs.
 */

#include "platform/time_compat.h"
#include "storage/sha3_sidecar_io.h"

#include "crypto/sha3.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Path helpers ───────────────────────────────────────────── */

static void ssio_body_path(char *out, size_t cap,
                           const char *datadir, const struct ssio_spec *spec)
{
    snprintf(out, cap, "%s/%s", datadir, spec->body_name);
}

static void ssio_sidecar_path(char *out, size_t cap,
                              const char *datadir, const struct ssio_spec *spec)
{
    snprintf(out, cap, "%s/%s", datadir, spec->sidecar_name);
}

/* ── Streaming body hash ────────────────────────────────────── */

bool ssio_hash_body(const char *datadir, const struct ssio_spec *spec,
                    uint8_t out_hash[32], uint64_t *out_size)
{
    char body_path[1024];
    ssio_body_path(body_path, sizeof(body_path), datadir, spec);

    FILE *f = fopen(body_path, "rb");
    if (!f) return false;

    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);

    enum { BUF_SIZE = 1u << 20 };  /* 1 MiB */
    uint8_t *buf = zcl_malloc(BUF_SIZE, spec->malloc_label);
    if (!buf) { fclose(f); return false; }

    uint64_t total = 0;
    size_t n;
    while ((n = fread(buf, 1, BUF_SIZE, f)) > 0) {
        sha3_256_write(&ctx, buf, n);
        total += n;
    }
    bool io_err = ferror(f) != 0;
    free(buf);
    fclose(f);
    if (io_err) return false;

    sha3_256_finalize(&ctx, out_hash);
    if (out_size) *out_size = total;
    return true;
}

/* ── Sidecar writer ─────────────────────────────────────────── */

struct zcl_result ssio_write_sidecar(const char *datadir,
                                     const struct ssio_spec *spec)
{
    if (!datadir) return ZCL_ERR(-1, "%s: null datadir", spec->domain);

    char body_path[1024];
    char side_path[1024];
    char tmp_path[1056];
    ssio_body_path(body_path, sizeof(body_path), datadir, spec);
    ssio_sidecar_path(side_path, sizeof(side_path), datadir, spec);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", side_path);

    struct stat st;
    if (stat(body_path, &st) != 0)
        return ZCL_ERR(-2, "%s: stat %s: %s", spec->domain, body_path,
                       strerror(errno));

    struct ssio_sidecar_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, spec->magic, 4);
    hdr.version = spec->version;
    hdr.body_size = (uint64_t)st.st_size;

    uint64_t hashed_size = 0;
    if (!ssio_hash_body(datadir, spec, hdr.body_sha3, &hashed_size))
        return ZCL_ERR(-3, "%s: hash body failed", spec->domain);
    /* stat size and streamed size must agree — disagreement means
     * something is truncating the file concurrently, which is a
     * bigger problem than this function can solve. */
    if (hashed_size != hdr.body_size)
        return ZCL_ERR(-4, "%s: size drift stat=%llu hashed=%llu",
                       spec->domain,
                       (unsigned long long)hdr.body_size,
                       (unsigned long long)hashed_size);

    FILE *f = fopen(tmp_path, "wb");
    if (!f)
        return ZCL_ERR(-5, "%s: fopen %s: %s", spec->domain, tmp_path,
                       strerror(errno));
    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        unlink(tmp_path);
        return ZCL_ERR(-6, "%s: fwrite failed", spec->domain);
    }
    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) (void)fsync(fd);
    fclose(f);

    if (rename(tmp_path, side_path) != 0) {
        struct zcl_result r = ZCL_ERR(-7,
            "%s: rename %s -> %s: %s",
            spec->domain, tmp_path, side_path, strerror(errno));
        unlink(tmp_path);
        return r;
    }
    return ZCL_OK;
}

/* ── Sidecar reader ─────────────────────────────────────────── */

enum ssio_read_verdict ssio_read_sidecar(const char *datadir,
                                         const struct ssio_spec *spec,
                                         struct ssio_sidecar_header *out)
{
    char side_path[1024];
    ssio_sidecar_path(side_path, sizeof(side_path), datadir, spec);

    FILE *f = fopen(side_path, "rb");
    if (!f) {
        if (errno == ENOENT) return SSIO_READ_MISSING;
        return SSIO_READ_UNREADABLE;
    }
    size_t n = fread(out, 1, sizeof(*out), f);
    bool io_err = ferror(f) != 0;
    fclose(f);
    if (io_err || n != sizeof(*out))
        return SSIO_READ_STALE;
    if (memcmp(out->magic, spec->magic, 4) != 0)
        return SSIO_READ_BAD_MAGIC;
    if (out->version != spec->version)
        return SSIO_READ_UNSUPPORTED;
    return SSIO_READ_OK;
}

/* ── Quarantine ─────────────────────────────────────────────── */

static void ssio_rename_if_present(const char *src, int64_t ts,
                                   const char *domain, const char *label)
{
    struct stat st;
    if (stat(src, &st) != 0) return;  /* nothing to do */

    char dst[1200];
    snprintf(dst, sizeof(dst), "%s.corrupt.%lld", src, (long long)ts);
    char tag[64];
    snprintf(tag, sizeof(tag), "%s_quarantine", domain);
    if (rename(src, dst) != 0) {
        LOG_WARN(tag, "%s_quarantine: rename %s -> %s failed: %s",
                 domain, src, dst, strerror(errno));
        return;
    }
    printf("%s: quarantined %s -> %s (%s)\n", domain, src, dst, label);
}

void ssio_quarantine(const char *datadir, const struct ssio_spec *spec,
                     const char *verdict_name)
{
    if (!datadir) return;
    int64_t ts = (int64_t)platform_time_wall_time_t();

    char body_path[1024];
    char side_path[1024];
    ssio_body_path(body_path, sizeof(body_path), datadir, spec);
    ssio_sidecar_path(side_path, sizeof(side_path), datadir, spec);

    ssio_rename_if_present(body_path, ts, spec->domain, verdict_name);
    ssio_rename_if_present(side_path, ts, spec->domain, verdict_name);

    event_emitf(spec->corrupt_event, 0,
                "verdict=%s ts=%lld", verdict_name, (long long)ts);
}
