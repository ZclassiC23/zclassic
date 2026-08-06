/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Read-only helpers over the frozen <datadir>/zcode layout. See
 * metaverse_priv.h; the layout contract itself is vcs/package_store.h.
 *
 * Why this file exists rather than a store handle: vcs_package_store_open()
 * deletes leftover temps, commits staged packages, and GCs orphan CAS
 * objects. That is correct for the node and wrong for a read command, so
 * the projection reaches the same canonical bytes by path. The wires stay
 * authoritative — nothing here is a second truth, exactly as
 * lib/vcs/package_index.c reads manifests/<root> directly. */

#include "metaverse_priv.h"

#include "base/hex.h"
#include "base/log_macros.h"
#include "base/safe_alloc.h"
#include "metaverse/property_adapter.h"
#include "vcs/blob_store.h"
#include "vcs/package_release.h"
#include "vcs/package_store.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MV_LOG "metaverse.read"
#define MV_PATH_MAX 4400

/* Read at most `cap` bytes of a file; a file larger than cap is a
 * rejection, not a truncated read. */
static enum mv_manifest_read_status mv_read_file(
    const char *path, size_t cap, uint8_t **out, size_t *out_len)
{
    FILE *f;
    uint8_t *buf;
    size_t len;
    int open_errno;

    *out = NULL;
    *out_len = 0;
    f = fopen(path, "rb");
    if (!f) {
        open_errno = errno;
        return open_errno == ENOENT ? MV_MANIFEST_READ_ABSENT
                                    : MV_MANIFEST_READ_IO_ERROR;
    }
    if (cap == SIZE_MAX) {
        fclose(f);
        return MV_MANIFEST_READ_INVALID;
    }
    buf = zcl_malloc(cap + 1u, "mv_read_file");
    if (!buf) {
        fclose(f);
        LOG_ERR(MV_LOG, "read buffer of %zu bytes for %s", cap + 1u, path);
        return MV_MANIFEST_READ_IO_ERROR;
    }
    len = fread(buf, 1, cap + 1u, f);
    if (ferror(f)) {
        fclose(f);
        free(buf);
        return MV_MANIFEST_READ_IO_ERROR;
    }
    fclose(f);
    if (len == 0 || len > cap) {
        free(buf);
        return MV_MANIFEST_READ_INVALID;
    }
    *out = buf;
    *out_len = len;
    return MV_MANIFEST_READ_OK;
}

bool mv_manifest_is_blob(const struct vcs_package_manifest *m)
{
    const struct vcs_package_file *f;

    if (!m || m->count != 1u || !m->files)
        return false;
    f = &m->files[0];
    return f->path && strcmp(f->path, VCS_BLOB_PATH) == 0 &&
           f->mode == VCS_PACKAGE_MODE_FILE && f->chunk_count == 1u &&
           f->size > 0 && f->size <= (uint64_t)VCS_BLOB_MAX_BYTES;
}

void mv_manifest_free(struct mv_manifest_read *m)
{
    if (!m)
        return;
    vcs_package_manifest_free(&m->manifest);
    memset(m, 0, sizeof(*m));
}

const char *mv_manifest_read_status_name(enum mv_manifest_read_status status)
{
    switch (status) {
    case MV_MANIFEST_READ_OK:       return "ok";
    case MV_MANIFEST_READ_ABSENT:   return "absent";
    case MV_MANIFEST_READ_IO_ERROR: return "io_error";
    case MV_MANIFEST_READ_INVALID:  return "invalid";
    }
    return "invalid";
}

enum mv_manifest_read_status mv_manifest_read(
    const char *zcode_dir, const char *root_hex, struct mv_manifest_read *out)
{
    char path[MV_PATH_MAX];
    uint8_t name_root[32];
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    enum mv_manifest_read_status read_status;
    int n;

    if (!out)
        return MV_MANIFEST_READ_INVALID;
    memset(out, 0, sizeof(*out));
    if (!zcode_dir || !root_hex || !zcl_hex_decode_lower(root_hex, name_root, 32))
        return MV_MANIFEST_READ_INVALID;

    n = snprintf(path, sizeof(path), "%s/manifests/%s", zcode_dir, root_hex);
    if (n < 0 || (size_t)n >= sizeof(path))
        return MV_MANIFEST_READ_INVALID;
    read_status = mv_read_file(path, VCS_PACKAGE_MANIFEST_MAX_WIRE_BYTES,
                               &wire, &wire_len);
    if (read_status != MV_MANIFEST_READ_OK)
        return read_status;
    if (!vcs_package_manifest_parse(wire, wire_len, &out->manifest)) {
        free(wire);
        memset(out, 0, sizeof(*out));
        return MV_MANIFEST_READ_INVALID;
    }
    free(wire);

    /* Re-derive the root from the wire we just parsed. This is the whole
     * basis of the local_content_hash evidence grade: the filename is not
     * evidence of anything, the recomputed root is. */
    if (!vcs_package_manifest_root(&out->manifest, out->root)) {
        vcs_package_manifest_free(&out->manifest);
        memset(out, 0, sizeof(*out));
        return MV_MANIFEST_READ_INVALID;
    }
    out->root_matches_name = memcmp(out->root, name_root, 32) == 0;

    out->file_count = (uint32_t)out->manifest.count;
    for (size_t i = 0; i < out->manifest.count; i++) {
        const struct vcs_package_file *f = &out->manifest.files[i];

        out->total_bytes += f->size;
        out->chunk_total += f->chunk_count;
        for (uint32_t c = 0; c < f->chunk_count; c++) {
            if (vcs_package_cas_present_in(zcode_dir,
                                           f->chunk_hashes + (size_t)c * 32u))
                out->chunks_present++;
        }
    }
    return MV_MANIFEST_READ_OK;
}

bool mv_store_enumerable(const char *zcode_dir, char *reason, size_t cap)
{
    char path[MV_PATH_MAX];
    DIR *dir;
    int n;

    if (reason && cap)
        reason[0] = '\0';
    if (!zcode_dir) {
        if (reason && cap)
            snprintf(reason, cap, "no content store directory was given");
        return false;
    }
    n = snprintf(path, sizeof(path), "%s/manifests", zcode_dir);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        if (reason && cap)
            snprintf(reason, cap,
                     "content store path is longer than %zu bytes",
                     sizeof(path));
        return false;
    }
    dir = opendir(path);
    if (dir) {
        closedir(dir);
        return true;
    }
    /* Nothing published yet. An empty catalog is the true answer. */
    if (errno == ENOENT)
        return true;
    if (reason && cap)
        snprintf(reason, cap,
                 "the local content store exists but could not be read "
                 "(%s: %s) — this is not an empty catalog",
                 path, strerror(errno));
    return false;
}

bool mv_zcode_store_ready(const struct metaverse_adapter_ctx *ctx,
                          char *reason, size_t reason_cap)
{
    if (!ctx) {
        if (reason && reason_cap)
            snprintf(reason, reason_cap, "no adapter context");
        return false;
    }
    return mv_store_enumerable(ctx->zcode_dir, reason, reason_cap);
}

static int mv_name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

bool mv_manifest_names(const char *zcode_dir, char (*out)[65],
                       size_t out_cap, size_t *written_out,
                       size_t *total_out, bool *truncated_out)
{
    char path[MV_PATH_MAX];
    char (*names)[65];
    size_t seen = 0;
    size_t written;
    bool truncated = false;
    DIR *dir;
    struct dirent *ent;
    int n;

    if (written_out)
        *written_out = 0;
    if (total_out)
        *total_out = 0;
    if (truncated_out)
        *truncated_out = false;
    if (!zcode_dir || !written_out || !total_out || !truncated_out ||
        (!out && out_cap > 0))
        return false;

    n = snprintf(path, sizeof(path), "%s/manifests", zcode_dir);
    if (n < 0 || (size_t)n >= sizeof(path))
        return false;
    dir = opendir(path);
    if (!dir)
        return errno == ENOENT; /* no store yet: honestly empty */

    names = zcl_malloc(MV_MANIFEST_SCAN_MAX * sizeof(*names), "mv_names");
    if (!names) {
        closedir(dir);
        LOG_ERR(MV_LOG, "manifest name buffer for %s", path);
        return false;
    }
    errno = 0;
    while ((ent = readdir(dir)) != NULL) {
        uint8_t scratch[32];

        if (!zcl_hex_decode_lower(ent->d_name, scratch, 32))
            continue;
        if (seen == MV_MANIFEST_SCAN_MAX) {
            truncated = true;
            break;
        }
        memcpy(names[seen], ent->d_name, 64);
        names[seen][64] = '\0';
        seen++;
    }
    if (errno != 0) {
        closedir(dir);
        free(names);
        return false;
    }
    closedir(dir);

    /* Ascending name order: readdir order is filesystem-dependent, and a
     * catalog page that reshuffles between identical calls is unusable. */
    qsort(names, seen, sizeof(*names), mv_name_cmp);

    written = seen < out_cap ? seen : out_cap;
    for (size_t i = 0; i < written; i++)
        memcpy(out[i], names[i], 65);
    free(names);

    *written_out = written;
    *total_out = seen;
    *truncated_out = truncated || written < seen;
    return true;
}

bool mv_release_read_verified(const char *zcode_dir,
                             const char *release_id_hex,
                             struct vcs_package_release *out)
{
    char path[MV_PATH_MAX];
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    int n;

    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!zcode_dir || !release_id_hex)
        return false;
    n = snprintf(path, sizeof(path), "%s/releases/%s", zcode_dir,
                 release_id_hex);
    if (n < 0 || (size_t)n >= sizeof(path))
        return false;
    if (mv_read_file(path, VCS_PACKAGE_RELEASE_MAX_WIRE_BYTES, &wire,
                     &wire_len) != MV_MANIFEST_READ_OK)
        return false;
    if (vcs_package_release_parse(wire, wire_len, out) !=
        VCS_PACKAGE_RELEASE_OK) {
        free(wire);
        memset(out, 0, sizeof(*out));
        return false;
    }
    free(wire);

    /* Verified NOW, in this call. package_index deliberately does not
     * verify signatures ("publication did"), so a view that inherited the
     * index's word for it could not honestly claim local_signature. */
    if (vcs_package_release_verify(out) != VCS_PACKAGE_RELEASE_OK) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}
