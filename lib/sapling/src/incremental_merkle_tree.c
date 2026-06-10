/* Copyright (c) 2016 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Incremental Merkle tree — pure C23 implementation. */

#include "sapling/incremental_merkle_tree.h"
#include "sapling/pedersen_hash.h"
#include "crypto/sha256.h"
#include "crypto/sha3.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pthread.h>

#ifdef ZCL_TESTING
#include <stdatomic.h>
#endif

/* Local shorthand for the (de)serialize/wfcheck error paths below. Each use
 * logs function/file/line plus a field name so a corrupted tree on disk
 * leaves a breadcrumb instead of a bare `false`. */
#define IMT_FAIL(field) \
    LOG_FAIL("incremental_merkle_tree", \
             "%s: %s (truncated stream or malformed tree?)", __func__, (field))

void sha256_compress_combine(const struct uint256 *a,
                              const struct uint256 *b,
                              size_t depth,
                              struct uint256 *out)
{
    (void)depth;
    struct sha256_ctx hasher;
    sha256_init(&hasher);
    sha256_write(&hasher, a->data, 32);
    sha256_write(&hasher, b->data, 32);
    sha256_finalize_no_padding(&hasher, out->data, 0);
}

void sha256_compress_uncommitted(struct uint256 *out)
{
    memset(out->data, 0, 32);
}

static void tree_init(struct incremental_merkle_tree *t, size_t depth,
                       void (*combine)(const struct uint256 *, const struct uint256 *,
                                       size_t, struct uint256 *),
                       void (*uncommitted)(struct uint256 *))
{
    assert(depth <= MAX_TREE_DEPTH);
    t->depth = depth;
    t->has_left = false;
    t->has_right = false;
    memset(&t->left, 0, sizeof(struct uint256));
    memset(&t->right, 0, sizeof(struct uint256));
    memset(t->has_parent, 0, sizeof(t->has_parent));
    memset(t->parents, 0, sizeof(t->parents));
    t->num_parents = 0;
    t->combine = combine;
    t->uncommitted = uncommitted;
}

void sprout_tree_init(struct incremental_merkle_tree *t)
{
    tree_init(t, INCREMENTAL_MERKLE_TREE_DEPTH,
              sha256_compress_combine, sha256_compress_uncommitted);
}

static void pedersen_combine(const struct uint256 *a,
                              const struct uint256 *b,
                              size_t depth,
                              struct uint256 *out)
{
    pedersen_merkle_hash(depth, a->data, b->data, out->data);
}

static void pedersen_uncommitted(struct uint256 *out)
{
    sapling_uncommitted(out->data);
}

void sapling_testing_tree_init(struct incremental_merkle_tree *t)
{
    tree_init(t, INCREMENTAL_MERKLE_TREE_DEPTH_TESTING,
              pedersen_combine, pedersen_uncommitted);
}

void sapling_tree_init(struct incremental_merkle_tree *t)
{
    tree_init(t, SAPLING_INCREMENTAL_MERKLE_TREE_DEPTH,
              pedersen_combine, pedersen_uncommitted);
}

/* Cached empty roots for Sapling Pedersen tree (depth 0..32).
 * Computed once on first use, reused for all subsequent calls. */
static struct uint256 s_sapling_empty_roots[MAX_TREE_DEPTH + 1];
static pthread_once_t s_sapling_empty_roots_once = PTHREAD_ONCE_INIT;

#ifdef ZCL_TESTING
/* See comment in pedersen_hash.c — race observability. */
_Atomic int zcl_sapling_empty_roots_body_runs_for_test = 0;

void zcl_sapling_empty_roots_reset_for_test(void)
{
    /* Reassigning a pthread_once_t is not specified by POSIX but is
     * the canonical test-only trick on glibc. Only safe when no other
     * thread is racing — the race tests join all workers first. */
    s_sapling_empty_roots_once = (pthread_once_t)PTHREAD_ONCE_INIT;
    memset(s_sapling_empty_roots, 0, sizeof(s_sapling_empty_roots));
    atomic_store(&zcl_sapling_empty_roots_body_runs_for_test, 0);
}
#endif

static void load_sapling_empty_roots(void)
{
#ifdef ZCL_TESTING
    atomic_fetch_add(&zcl_sapling_empty_roots_body_runs_for_test, 1);
#endif
    pedersen_uncommitted(&s_sapling_empty_roots[0]);
    for (size_t d = 0; d < MAX_TREE_DEPTH; d++)
        pedersen_combine(&s_sapling_empty_roots[d], &s_sapling_empty_roots[d],
                          d, &s_sapling_empty_roots[d + 1]);
}

static void ensure_sapling_empty_roots(void)
{
    pthread_once(&s_sapling_empty_roots_once, load_sapling_empty_roots);
}

/* Compute empty root at given depth. Uses cache for Pedersen trees. */
static void empty_root_at_depth(const struct incremental_merkle_tree *t,
                                 size_t depth, struct uint256 *out)
{
    if (t->combine == pedersen_combine) {
        ensure_sapling_empty_roots();
        *out = s_sapling_empty_roots[depth];
        return;
    }
    struct uint256 current;
    t->uncommitted(&current);
    for (size_t d = 0; d < depth; d++) {
        struct uint256 next;
        t->combine(&current, &current, d, &next);
        current = next;
    }
    *out = current;
}

void incremental_tree_empty_root(const struct incremental_merkle_tree *t,
                                  struct uint256 *out)
{
    empty_root_at_depth(t, t->depth, out);
}

static void filler_next(const struct incremental_merkle_tree *t,
                         const struct uint256 *filler, size_t *filler_idx,
                         size_t filler_count, size_t depth,
                         struct uint256 *out)
{
    if (*filler_idx < filler_count) {
        *out = filler[*filler_idx];
        (*filler_idx)++;
    } else {
        empty_root_at_depth(t, depth, out);
    }
}

void incremental_tree_append(struct incremental_merkle_tree *t,
                              const struct uint256 *obj)
{
    if (!t->has_left) {
        t->left = *obj;
        t->has_left = true;
    } else if (!t->has_right) {
        t->right = *obj;
        t->has_right = true;
    } else {
        struct uint256 combined;
        t->combine(&t->left, &t->right, 0, &combined);

        t->left = *obj;
        t->has_right = false;

        for (size_t i = 0; i < t->depth; i++) {
            if (i < t->num_parents) {
                if (t->has_parent[i]) {
                    struct uint256 next;
                    t->combine(&t->parents[i], &combined, i + 1, &next);
                    combined = next;
                    t->has_parent[i] = false;
                } else {
                    t->parents[i] = combined;
                    t->has_parent[i] = true;
                    return;
                }
            } else {
                t->parents[i] = combined;
                t->has_parent[i] = true;
                t->num_parents = i + 1;
                return;
            }
        }
    }
}

void incremental_tree_root(const struct incremental_merkle_tree *t,
                            struct uint256 *out)
{
    size_t filler_idx = 0;
    struct uint256 combine_left;
    if (t->has_left) {
        combine_left = t->left;
    } else {
        filler_next(t, NULL, &filler_idx, 0, 0, &combine_left);
    }

    struct uint256 combine_right;
    if (t->has_right) {
        combine_right = t->right;
    } else {
        filler_next(t, NULL, &filler_idx, 0, 0, &combine_right);
    }

    struct uint256 root;
    t->combine(&combine_left, &combine_right, 0, &root);

    size_t d = 1;
    for (size_t i = 0; i < t->num_parents; i++) {
        struct uint256 next;
        if (t->has_parent[i]) {
            t->combine(&t->parents[i], &root, d, &next);
        } else {
            struct uint256 empty;
            empty_root_at_depth(t, d, &empty);
            t->combine(&root, &empty, d, &next);
        }
        root = next;
        d++;
    }

    while (d < t->depth) {
        struct uint256 next;
        struct uint256 empty;
        empty_root_at_depth(t, d, &empty);
        t->combine(&root, &empty, d, &next);
        root = next;
        d++;
    }

    *out = root;
}

size_t incremental_tree_size(const struct incremental_merkle_tree *t)
{
    size_t ret = 0;
    if (t->has_left) ret++;
    if (t->has_right) ret++;
    for (size_t i = 0; i < t->num_parents; i++) {
        if (t->has_parent[i])
            ret += ((size_t)1 << (i + 1));
    }
    return ret;
}

bool incremental_tree_is_complete(const struct incremental_merkle_tree *t)
{
    /* These three return-false paths are the "tree not yet complete" signal
     * used by the witness append loop. They are NOT errors — logging each
     * would spam at every append. Left bare intentionally. */
    if (!t->has_left || !t->has_right)
        return false;
    if (t->num_parents != t->depth - 1)
        return false;
    for (size_t i = 0; i < t->num_parents; i++) {
        if (!t->has_parent[i])
            return false;
    }
    return true;
}

/* Wire format: optional<hash> left, optional<hash> right, vector<optional<hash>> parents */
bool incremental_tree_serialize(const struct incremental_merkle_tree *t,
                                 struct byte_stream *s)
{
    /* left: discriminant + hash */
    if (!stream_write_u8(s, t->has_left ? 1 : 0))
        IMT_FAIL("write left discriminant");
    if (t->has_left && !stream_write(s, t->left.data, 32))
        IMT_FAIL("write left hash");

    /* right: discriminant + hash */
    if (!stream_write_u8(s, t->has_right ? 1 : 0))
        IMT_FAIL("write right discriminant");
    if (t->has_right && !stream_write(s, t->right.data, 32))
        IMT_FAIL("write right hash");

    /* parents: compact_size + array of optional<hash> */
    if (!stream_write_compact_size(s, t->num_parents))
        IMT_FAIL("write num_parents compact_size");
    for (size_t i = 0; i < t->num_parents; i++) {
        if (!stream_write_u8(s, t->has_parent[i] ? 1 : 0))
            IMT_FAIL("write parent discriminant");
        if (t->has_parent[i] && !stream_write(s, t->parents[i].data, 32))
            IMT_FAIL("write parent hash");
    }
    return true;
}

static bool wfcheck(const struct incremental_merkle_tree *t)
{
    if (t->num_parents >= t->depth)
        LOG_FAIL("incremental_merkle_tree",
                 "wfcheck: num_parents=%zu >= depth=%zu",
                 t->num_parents, t->depth);
    if (t->num_parents > 0 && !t->has_parent[t->num_parents - 1])
        LOG_FAIL("incremental_merkle_tree",
                 "wfcheck: parent[num_parents-1] is not set (truncated or corrupt)");
    if (!t->has_left && t->has_right)
        LOG_FAIL("incremental_merkle_tree",
                 "wfcheck: has_right without has_left (invariant violated)");
    if (!t->has_left && t->num_parents > 0)
        LOG_FAIL("incremental_merkle_tree",
                 "wfcheck: num_parents>0 without has_left (invariant violated)");
    return true;
}

bool incremental_tree_deserialize(struct incremental_merkle_tree *t,
                                   struct byte_stream *s)
{
    uint8_t disc;

    /* left */
    if (!stream_read(s, &disc, 1))
        IMT_FAIL("read left discriminant");
    t->has_left = (disc != 0);
    if (t->has_left) {
        if (!stream_read(s, t->left.data, 32))
            IMT_FAIL("read left hash");
    } else {
        memset(&t->left, 0, sizeof(struct uint256));
    }

    /* right */
    if (!stream_read(s, &disc, 1))
        IMT_FAIL("read right discriminant");
    t->has_right = (disc != 0);
    if (t->has_right) {
        if (!stream_read(s, t->right.data, 32))
            IMT_FAIL("read right hash");
    } else {
        memset(&t->right, 0, sizeof(struct uint256));
    }

    /* parents */
    uint64_t num;
    if (!stream_read_compact_size(s, &num))
        IMT_FAIL("read num_parents compact_size");
    if (num > MAX_TREE_DEPTH)
        LOG_FAIL("incremental_merkle_tree",
                 "deserialize: num_parents=%" PRIu64 " exceeds MAX_TREE_DEPTH=%d",
                 num, MAX_TREE_DEPTH);
    t->num_parents = (size_t)num;
    memset(t->has_parent, 0, sizeof(t->has_parent));
    memset(t->parents, 0, sizeof(t->parents));
    for (size_t i = 0; i < t->num_parents; i++) {
        if (!stream_read(s, &disc, 1))
            IMT_FAIL("read parent discriminant");
        t->has_parent[i] = (disc != 0);
        if (t->has_parent[i]) {
            if (!stream_read(s, t->parents[i].data, 32))
                IMT_FAIL("read parent hash");
        }
    }

    return wfcheck(t);
}

/* --- Flat-file checkpoint ───────────────────────────
 *
 * Replaces the 2.6M-block sapling tree replay on crash recovery
 * with a sub-second load from a dedicated on-disk checkpoint.
 * Lives outside the SQLite-backed node_state table so it is
 * immune to P14-class savepoint contention. See header for the
 * file format. */

#define SAPLING_CKPT_MAGIC     "SPLT"
#define SAPLING_CKPT_VERSION   1
#define SAPLING_CKPT_HEADER_SZ (4 + 4 + 8 + 32 + 4 + 4)
#define SAPLING_CKPT_TRAILER_SZ 32
#define SAPLING_CKPT_MAX_BLOB  (64u * 1024u)

static bool ckpt_write_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
    return true;
}

static bool ckpt_write_u64_le(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)((v >> (i * 8)) & 0xff);
    return true;
}

static uint32_t ckpt_read_u32_le(const uint8_t *p)
{
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t ckpt_read_u64_le(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= ((uint64_t)p[i]) << (i * 8);
    return v;
}

bool sapling_tree_flush_checkpoint(const struct incremental_merkle_tree *t,
                                   int64_t height,
                                   const char *path)
{
    if (!t || !path)
        LOG_FAIL("sapling_tree",
                 "flush_checkpoint: NULL arg (t=%p path=%p)",
                 (const void *)t, (const void *)path);

    /* 1. Serialize the tree blob. */
    struct byte_stream blob;
    stream_init(&blob, 4096);
    if (!incremental_tree_serialize(t, &blob)) {
        stream_free(&blob);
        LOG_FAIL("sapling_tree",
                 "flush_checkpoint: serialize failed");
    }
    if (blob.size > SAPLING_CKPT_MAX_BLOB) {
        stream_free(&blob);
        LOG_FAIL("sapling_tree",
                 "flush_checkpoint: blob size %zu exceeds max %u",
                 blob.size, SAPLING_CKPT_MAX_BLOB);
    }

    /* 2. Compute current root. */
    struct uint256 root;
    incremental_tree_root(t, &root);

    /* 3. Build the header + body in a contiguous buffer so we can
     *    hash it in one shot for the trailer. */
    size_t body_sz = SAPLING_CKPT_HEADER_SZ + blob.size;
    uint8_t *body = (uint8_t *)zcl_malloc(body_sz, "sapling_ckpt_body");
    if (!body) {
        stream_free(&blob);
        LOG_FAIL("sapling_tree",
                 "flush_checkpoint: alloc body %zu failed", body_sz);
    }
    memcpy(body + 0, SAPLING_CKPT_MAGIC, 4);
    ckpt_write_u32_le(body + 4, SAPLING_CKPT_VERSION);
    ckpt_write_u64_le(body + 8, (uint64_t)height);
    memcpy(body + 16, root.data, 32);
    ckpt_write_u32_le(body + 48, (uint32_t)incremental_tree_size(t));
    ckpt_write_u32_le(body + 52, (uint32_t)blob.size);
    memcpy(body + SAPLING_CKPT_HEADER_SZ, blob.data, blob.size);
    stream_free(&blob);

    uint8_t trailer[SAPLING_CKPT_TRAILER_SZ];
    zcl_sha3_256(body, body_sz, trailer);

    /* 4. Atomic write: .tmp + fsync + rename. */
    size_t path_len = strlen(path);
    char *tmp_path = (char *)zcl_malloc(path_len + 8, "sapling_ckpt_tmp_path");
    if (!tmp_path) {
        free(body);
        LOG_FAIL("sapling_tree",
                 "flush_checkpoint: alloc tmp_path failed");
    }
    snprintf(tmp_path, path_len + 8, "%s.tmp", path);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        int saved_errno = errno;
        fprintf(stderr,  // obs-ok:helper-context-logged
                "[sapling_tree] %s:%d %s(): flush_checkpoint: "
                "open(%s) failed: %s\n",
                __FILE__, __LINE__, __func__, tmp_path,
                strerror(saved_errno));
        free(body);
        free(tmp_path);
        return false;
    }

    bool ok = true;
    ssize_t w1 = write(fd, body, body_sz);
    if (w1 < 0 || (size_t)w1 != body_sz) ok = false;
    ssize_t w2 = write(fd, trailer, SAPLING_CKPT_TRAILER_SZ);
    if (w2 < 0 || (size_t)w2 != SAPLING_CKPT_TRAILER_SZ) ok = false;

    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;

    if (ok) {
        if (rename(tmp_path, path) != 0) ok = false;
    } else {
        unlink(tmp_path);
    }

    int saved_errno = errno;
    free(body);
    free(tmp_path);

    if (!ok) {
        fprintf(stderr,  // obs-ok:helper-context-logged
                "[sapling_tree] %s:%d %s(): flush_checkpoint: "
                "write/rename to %s failed: %s\n",
                __FILE__, __LINE__, __func__, path,
                strerror(saved_errno));
        return false;
    }
    return true;
}

bool sapling_tree_load_checkpoint(struct incremental_merkle_tree *t,
                                  int64_t *height_out,
                                  const char *path)
{
    if (!t || !path)
        LOG_FAIL("sapling_tree",
                 "load_checkpoint: NULL arg (t=%p path=%p)",
                 (const void *)t, (const void *)path);

    struct stat st;
    if (stat(path, &st) != 0)
        return false; /* missing file is a silent not-found */

    if (st.st_size < (off_t)(SAPLING_CKPT_HEADER_SZ + SAPLING_CKPT_TRAILER_SZ))
        return false; /* too small — treat as corrupt */
    if ((size_t)st.st_size >
        SAPLING_CKPT_HEADER_SZ + SAPLING_CKPT_MAX_BLOB + SAPLING_CKPT_TRAILER_SZ)
        return false; /* too large — refuse */

    uint8_t *buf = (uint8_t *)zcl_malloc((size_t)st.st_size,
                                         "sapling_ckpt_load");
    if (!buf)
        return false;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        free(buf);
        return false;
    }
    ssize_t r = read(fd, buf, (size_t)st.st_size);
    close(fd);
    if (r != st.st_size) {
        free(buf);
        return false;
    }

    size_t body_sz = (size_t)st.st_size - SAPLING_CKPT_TRAILER_SZ;
    uint8_t expected_trailer[SAPLING_CKPT_TRAILER_SZ];
    zcl_sha3_256(buf, body_sz, expected_trailer);

    /* Integrity check must run before any field-level parsing so a
     * tampered magic byte can never steer the loader. */
    if (memcmp(buf + body_sz, expected_trailer,
               SAPLING_CKPT_TRAILER_SZ) != 0) {
        free(buf);
        return false;
    }

    if (memcmp(buf, SAPLING_CKPT_MAGIC, 4) != 0) {
        free(buf);
        return false;
    }
    uint32_t version = ckpt_read_u32_le(buf + 4);
    if (version != SAPLING_CKPT_VERSION) {
        free(buf);
        return false;
    }
    int64_t height = (int64_t)ckpt_read_u64_le(buf + 8);
    uint32_t blob_len = ckpt_read_u32_le(buf + 52);
    if (blob_len > SAPLING_CKPT_MAX_BLOB ||
        SAPLING_CKPT_HEADER_SZ + (size_t)blob_len != body_sz) {
        free(buf);
        return false;
    }

    /* Deserialize the tree blob into a scratch tree (preserves the
     * caller's tree on any failure below). */
    struct incremental_merkle_tree scratch;
    sapling_tree_init(&scratch);
    struct byte_stream s;
    stream_init_from_data(&s, buf + SAPLING_CKPT_HEADER_SZ, blob_len);
    if (!incremental_tree_deserialize(&scratch, &s)) {
        free(buf);
        return false;
    }

    /* Root from the loaded tree must match the root field in the file.
     * The SHA3 trailer already guarantees bit-for-bit integrity, but
     * the root check also catches pointer-mismatch bugs — e.g. a
     * future format change that ships a stale root alongside a valid
     * blob. */
    struct uint256 computed_root;
    incremental_tree_root(&scratch, &computed_root);
    if (memcmp(computed_root.data, buf + 16, 32) != 0) {
        free(buf);
        return false;
    }

    free(buf);

    *t = scratch;
    if (height_out)
        *height_out = height;
    return true;
}

/* --- Incremental Witness --- */

static size_t next_depth(const struct incremental_merkle_tree *t, size_t skip)
{
    size_t d = 0;
    size_t s = skip;
    if (!t->has_right) {
        if (s == 0) return 0;
        s--;
    }
    for (size_t i = 0; i < t->num_parents; i++) {
        if (!t->has_parent[i]) {
            if (s == 0) return d + 1;
            s--;
        }
        d++;
    }
    /* Above all existing parents */
    return d + 1 + s;
}

void incremental_witness_init(struct incremental_witness *w,
                               const struct incremental_merkle_tree *tree)
{
    w->tree = *tree;
    w->num_filled = 0;
    w->has_cursor = false;
    w->cursor_depth = next_depth(tree, 0);
}

void incremental_witness_append(struct incremental_witness *w,
                                 const struct uint256 *obj)
{
    if (w->has_cursor) {
        incremental_tree_append(&w->cursor, obj);
        if (incremental_tree_is_complete(&w->cursor)) {
            struct uint256 root;
            incremental_tree_root(&w->cursor, &root);
            if (w->num_filled < MAX_WITNESS_FILLS) {
                w->filled[w->num_filled++] = root;
            }
            w->has_cursor = false;
            w->cursor_depth = next_depth(&w->tree, w->num_filled);
        }
    } else {
        w->cursor_depth = next_depth(&w->tree, w->num_filled);
        if (w->cursor_depth == 0) {
            if (w->num_filled < MAX_WITNESS_FILLS) {
                w->filled[w->num_filled++] = *obj;
            }
            w->cursor_depth = next_depth(&w->tree, w->num_filled);
        } else {
            /* Initialize cursor subtree at cursor_depth */
            tree_init(&w->cursor, w->cursor_depth,
                      w->tree.combine, w->tree.uncommitted);
            incremental_tree_append(&w->cursor, obj);
            w->has_cursor = true;
        }
    }
}

void incremental_witness_root(const struct incremental_witness *w,
                               struct uint256 *out)
{
    /* Partial fill: combine tree's root computation with filled + cursor */
    const struct incremental_merkle_tree *t = &w->tree;

    struct uint256 combine_left;
    if (t->has_left) {
        combine_left = t->left;
    } else {
        t->uncommitted(&combine_left);
    }

    struct uint256 combine_right;
    if (t->has_right) {
        combine_right = t->right;
    } else {
        /* Use first filled or uncommitted */
        if (w->num_filled > 0 || w->has_cursor) {
            size_t fi = 0;
            if (fi < w->num_filled) {
                combine_right = w->filled[fi];
                fi++;
            } else {
                t->uncommitted(&combine_right);
            }
        } else {
            t->uncommitted(&combine_right);
        }
    }

    struct uint256 root;
    t->combine(&combine_left, &combine_right, 0, &root);

    size_t d = 1;
    size_t filled_idx = t->has_right ? 0 : (w->num_filled > 0 ? 1 : 0);

    for (size_t i = 0; i < t->num_parents || d < t->depth; i++) {
        struct uint256 next_val;
        if (i < t->num_parents && t->has_parent[i]) {
            t->combine(&t->parents[i], &root, d, &next_val);
        } else {
            struct uint256 filler;
            if (filled_idx < w->num_filled) {
                filler = w->filled[filled_idx++];
            } else if (w->has_cursor && filled_idx == w->num_filled) {
                incremental_tree_root(&w->cursor, &filler);
                filled_idx++;
            } else {
                empty_root_at_depth(t, d, &filler);
            }
            t->combine(&root, &filler, d, &next_val);
        }
        root = next_val;
        d++;
        if (d >= t->depth) break;
    }

    *out = root;
}

bool incremental_witness_serialize(const struct incremental_witness *w,
                                    struct byte_stream *s)
{
    if (!incremental_tree_serialize(&w->tree, s))
        IMT_FAIL("write underlying tree");

    /* filled: vector<hash> */
    if (!stream_write_compact_size(s, w->num_filled))
        IMT_FAIL("write num_filled compact_size");
    for (size_t i = 0; i < w->num_filled; i++) {
        if (!stream_write(s, w->filled[i].data, 32))
            IMT_FAIL("write filled[i]");
    }

    /* cursor: optional<tree> */
    if (!stream_write_u8(s, w->has_cursor ? 1 : 0))
        IMT_FAIL("write cursor discriminant");
    if (w->has_cursor) {
        if (!incremental_tree_serialize(&w->cursor, s))
            IMT_FAIL("write cursor tree");
    }

    return true;
}

bool incremental_witness_merkle_path(const struct incremental_witness *w,
                                      uint8_t *path_out, size_t *path_len)
{
    const struct incremental_merkle_tree *t = &w->tree;
    size_t depth = t->depth;

    /* Compute leaf position = tree_size - 1 */
    size_t tree_sz = incremental_tree_size(t);
    if (tree_sz == 0)
        LOG_FAIL("incremental_merkle_tree",
                 "merkle_path: tree is empty (no leaves to authenticate)");
    uint64_t position = tree_sz - 1;

    /* Build the authentication path by collecting siblings at each level.
     * Walk the same structure as incremental_witness_root but collect
     * the "other side" at each combination step. */

    struct uint256 auth_path[MAX_TREE_DEPTH];
    uint8_t path_bits[MAX_TREE_DEPTH]; /* 0 = leaf is left child, 1 = right child */
    size_t num_levels = 0;

    /* Level 0: left/right of the tree base */
    (void)position;
    size_t filled_idx = 0;

    if (t->has_right) {
        /* Leaf was left child at level 0 → sibling is right */
        auth_path[0] = t->right;
        path_bits[0] = 0; /* leaf on left side */
    } else {
        /* Leaf is the right child → sibling is left */
        if (t->has_left) {
            auth_path[0] = t->left;
            path_bits[0] = 1;
        } else {
            t->uncommitted(&auth_path[0]);
            path_bits[0] = 0;
        }
    }
    num_levels = 1;

    /* Levels 1..depth-1: walk parents + filled + cursor */
    for (size_t i = 0; i < depth - 1 && num_levels < depth; i++) {
        size_t level = i; /* parent index */
        if (level < t->num_parents && t->has_parent[level]) {
            /* Parent exists → we came from the right side, sibling is parent */
            auth_path[num_levels] = t->parents[level];
            path_bits[num_levels] = 1;
        } else {
            /* No parent → get from filled or cursor or empty */
            if (filled_idx < w->num_filled) {
                auth_path[num_levels] = w->filled[filled_idx++];
            } else if (w->has_cursor && filled_idx == w->num_filled) {
                incremental_tree_root(&w->cursor, &auth_path[num_levels]);
                filled_idx++;
            } else {
                empty_root_at_depth(t, num_levels, &auth_path[num_levels]);
            }
            path_bits[num_levels] = 0;
        }
        num_levels++;
    }

    /* Pad remaining levels with empty roots */
    while (num_levels < depth) {
        empty_root_at_depth(t, num_levels, &auth_path[num_levels]);
        path_bits[num_levels] = 0;
        num_levels++;
    }

    /* Serialize in Sapling wire format:
     * compact_size(depth) || depth × (32-byte sibling || 1-byte position_bit) */
    size_t off = 0;
    /* compact_size for depth (always fits in 1 byte for depth <= 32) */
    path_out[off++] = (uint8_t)depth;
    for (size_t i = 0; i < depth; i++) {
        memcpy(path_out + off, auth_path[i].data, 32);
        off += 32;
        path_out[off++] = path_bits[i];
    }
    *path_len = off;
    return true;
}

bool incremental_witness_deserialize(struct incremental_witness *w,
                                      struct byte_stream *s,
                                      size_t depth,
                                      void (*combine)(const struct uint256 *,
                                                      const struct uint256 *,
                                                      size_t, struct uint256 *),
                                      void (*uncommitted)(struct uint256 *))
{
    /* Initialize function pointers first */
    w->tree.depth = depth;
    w->tree.combine = combine;
    w->tree.uncommitted = uncommitted;

    if (!incremental_tree_deserialize(&w->tree, s))
        IMT_FAIL("read underlying tree");

    /* filled */
    uint64_t num;
    if (!stream_read_compact_size(s, &num))
        IMT_FAIL("read num_filled compact_size");
    if (num > MAX_WITNESS_FILLS)
        LOG_FAIL("incremental_merkle_tree",
                 "witness_deserialize: num_filled=%" PRIu64 " exceeds MAX_WITNESS_FILLS=%d",
                 num, MAX_WITNESS_FILLS);
    w->num_filled = (size_t)num;
    for (size_t i = 0; i < w->num_filled; i++) {
        if (!stream_read(s, w->filled[i].data, 32))
            IMT_FAIL("read filled[i]");
    }

    /* cursor */
    uint8_t disc;
    if (!stream_read(s, &disc, 1))
        IMT_FAIL("read cursor discriminant");
    w->has_cursor = (disc != 0);
    if (w->has_cursor) {
        w->cursor.depth = depth;
        w->cursor.combine = combine;
        w->cursor.uncommitted = uncommitted;
        if (!incremental_tree_deserialize(&w->cursor, s))
            IMT_FAIL("read cursor tree");
    }

    w->cursor_depth = next_depth(&w->tree, w->num_filled);

    /* cursor.depth must match cursor_depth, NOT full tree depth.
     * The cursor is a subtree at a specific level — its root computation
     * pads empty hashes up to cursor.depth. Using the full tree depth (32)
     * instead of cursor_depth produces a wrong root with 27+ extra levels. */
    if (w->has_cursor) {
        w->cursor.depth = w->cursor_depth;
    }

    return true;
}
