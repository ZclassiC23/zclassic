/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "storage/disk_block_io.h"
#include "core/serialize.h"
#include "core/hash.h"
#include "util/log_macros.h"
#include "util/log_throttle.h"
#include "platform/time_compat.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include "util/safe_alloc.h"

/* De-storm the read-fail logs (defense-in-depth). A blocks-less starter-pack
 * bundle ships block_index with HAVE_DATA + (nFile,nDataPos) for blocks whose
 * body was never shipped, so every read-walker — and the boot-time pprev-repair
 * (block_index_integrity.c) — would open() a missing blk*.dat once per block.
 * Below the snapshot seed that is ~3.1M absent-file opens; un-throttled it
 * emitted one line per block (~3.1M lines, a >1 GB node.log in ~2 min) which on
 * a near-full disk filled it and SILENTLY CRASHED the node before it could reach
 * tip.
 *
 * This throttle bounds that log flood to at most one line per 60 s (carrying the
 * running suppressed-repeat count + a sample path), which removes the disk-full
 * crash vector regardless of any boot-time mitigation. It is always-on
 * defense-in-depth for every residual unbacked read: a blk file lost/unlinked at
 * runtime, a present-but-wrong-coord legacy mismatch, or a blocks-less bundle
 * whose below-seed bodies were never shipped. Each such read costs only an
 * open()/pread() fail, which the fold tolerates. (The deeper fix — not re-reading
 * bodies the bundle never shipped — lives on the boot / mirror-rebuild path and
 * is tracked separately.)
 *
 * Single global key (0) per throttle: interleaved walkers over many blk files
 * would re-emit on every file transition if keyed by nFile (~4k lines observed),
 * so we collapse to one identity that emits at most once per 60 s keep-alive,
 * carrying the running suppressed-repeat count and a sample path. */
static struct log_throttle g_open_fail_throttle = LOG_THROTTLE_INIT;
static struct log_throttle g_readfail_throttle  = LOG_THROTTLE_INIT;

void get_block_pos_filename(char *buf, size_t buflen,
                            const char *datadir,
                            const struct disk_block_pos *pos,
                            const char *prefix)
{
    if (pos->nFile == 255)
        snprintf(buf, buflen, "%s/blocks/%s_sync.dat", datadir, prefix);
    else
        snprintf(buf, buflen, "%s/blocks/%s%05d.dat", datadir, prefix, pos->nFile);
}

static bool ensure_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode);
    return mkdir(path, 0755) == 0;
}

static bool choose_append_block_pos(struct disk_block_pos *pos,
                                    uint32_t block_size,
                                    const char *datadir)
{
    if (!pos || !datadir)
        return false;

    char blocks_dir[512];
    snprintf(blocks_dir, sizeof(blocks_dir), "%s/blocks", datadir);
    if (!ensure_directory(blocks_dir))
        return false;

    int last_file = 0;
    unsigned int last_size = 0;
    for (int i = 0; i <= 9999; i++) {
        char path[512];
        struct disk_block_pos probe = { .nFile = i, .nPos = 0 };
        get_block_pos_filename(path, sizeof(path), datadir, &probe, "blk");
        struct stat st;
        if (stat(path, &st) != 0)
            break;
        last_file = i;
        last_size = (unsigned int)st.st_size;
    }

    if (last_size + block_size + 8u > 0x8000000u) {
        last_file++;
        last_size = 0;
    }
    if (last_file > 9999)
        return false;

    pos->nFile = last_file;
    pos->nPos = last_size;
    return true;
}

/* ── Read-only file handle cache ──────────────────────────────────
 * During sequential IBD, consecutive blocks are almost always in the
 * same blk*.dat file. Keeping the last-opened read-only FILE* avoids
 * ~99% of open/close syscalls. Write paths bypass the cache.
 * Protected by mutex for thread safety (bg_validation, P2P, RPC). */
static pthread_mutex_t g_file_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_cached_file = NULL;
static int   g_cached_nfile = -1;
static char  g_cached_prefix[8] = {0};

/* ── Deferred block-body fdatasync (see storage/disk_block_io.h) ──────
 * When g_deferred_sync is set, write_block_to_disk() records the file it
 * wrote here instead of fdatasync()ing it inline; disk_block_io_sync_pending()
 * flushes the set at the drain-batch boundary. Both the flag and the set are
 * guarded by g_file_cache_mutex (already held across the write). The cap is
 * far larger than any batch touches (128 MB per blk file * 64 = 8 GB of
 * bodies) so overflow is unreachable in practice; it still falls back to an
 * inline sync rather than dropping a file on the floor. */
#define DEFERRED_MAX_FILES 64
static bool g_deferred_sync = false;
static char g_pending_paths[DEFERRED_MAX_FILES][512];
static int  g_pending_count = 0;

/* Add `path` to the pending set (dedup). Caller holds g_file_cache_mutex.
 * Returns false only when the set is full (caller must sync inline instead). */
static bool deferred_record_pending_locked(const char *path)
{
    for (int i = 0; i < g_pending_count; i++)
        if (strcmp(g_pending_paths[i], path) == 0)
            return true; /* already pending — one sync covers all writes */
    if (g_pending_count >= DEFERRED_MAX_FILES)
        return false;
    snprintf(g_pending_paths[g_pending_count], sizeof(g_pending_paths[0]),
             "%s", path);
    g_pending_count++;
    return true;
}

void disk_block_io_set_deferred_sync(bool enabled)
{
    pthread_mutex_lock(&g_file_cache_mutex);
    g_deferred_sync = enabled;
    pthread_mutex_unlock(&g_file_cache_mutex);
}

bool disk_block_io_deferred_sync_enabled(void)
{
    pthread_mutex_lock(&g_file_cache_mutex);
    bool v = g_deferred_sync;
    pthread_mutex_unlock(&g_file_cache_mutex);
    return v;
}

bool disk_block_io_sync_pending(void)
{
    pthread_mutex_lock(&g_file_cache_mutex);
    bool all_ok = true;
    int keep = 0;
    for (int i = 0; i < g_pending_count; i++) {
        int fd = open(g_pending_paths[i], O_RDONLY);
        bool synced = (fd >= 0 && fdatasync(fd) == 0);
        if (fd >= 0)
            close(fd);
        if (!synced) {
            /* Keep the entry so a retry re-attempts the sync — clearing it
             * would let a later commit succeed while these bytes are still
             * unsynced, breaking the ordering invariant. */
            all_ok = false;
            fprintf(stderr,  // obs-ok:sync-failure-vetoes-commit-via-precommit-hook
                    "disk_block_io_sync_pending: fdatasync %s failed: %s\n",
                    g_pending_paths[i], strerror(errno));
            if (keep != i)
                memcpy(g_pending_paths[keep], g_pending_paths[i],
                       sizeof(g_pending_paths[0]));
            keep++;
        }
    }
    g_pending_count = keep;
    pthread_mutex_unlock(&g_file_cache_mutex);
    return all_ok;
}

/* Expose mutex for callers that read block/undo files directly
 * (e.g., transaction_controller, bg_validation undo reads).
 * All fread/fseek/fclose on blk*.dat and rev*.dat MUST be wrapped
 * in lock/unlock to prevent SIGSEGV from concurrent FILE* access. */
void disk_block_io_lock(void)
{
    pthread_mutex_lock(&g_file_cache_mutex);
}

void disk_block_io_unlock(void)
{
    pthread_mutex_unlock(&g_file_cache_mutex);
}

/* Caller-owned-lock variant: used from paths that already hold
 * g_file_cache_mutex (e.g. block_pruning_service during the
 * invalidate-then-unlink sequence, where re-entering the lock
 * would self-deadlock on a NORMAL mutex). */
static void disk_block_io_close_cache_locked(void)
{
    if (g_cached_file) {
        fclose(g_cached_file);
        g_cached_file = NULL;
        g_cached_nfile = -1;
    }
}

void disk_block_io_close_cache(void)
{
    pthread_mutex_lock(&g_file_cache_mutex);
    disk_block_io_close_cache_locked();
    pthread_mutex_unlock(&g_file_cache_mutex);
}

void disk_block_io_close_cache_while_locked(void)
{
    disk_block_io_close_cache_locked();
}

FILE *open_disk_file(const char *datadir,
                     const struct disk_block_pos *pos,
                     const char *prefix, bool read_only)
{
    if (pos->nFile < 0)
        return NULL;

    char blocks_dir[512];
    snprintf(blocks_dir, sizeof(blocks_dir), "%s/blocks", datadir);
    ensure_directory(blocks_dir);

    /* Try the read-only cache: same file number and prefix → reuse handle */
    if (read_only && g_cached_file &&
        g_cached_nfile == pos->nFile &&
        strcmp(g_cached_prefix, prefix) == 0) {
        if (fseek(g_cached_file, (long)pos->nPos, SEEK_SET) == 0)
            return g_cached_file;
        /* Seek failed — close and reopen */
        fclose(g_cached_file);
        g_cached_file = NULL;
        g_cached_nfile = -1;
    }

    /* Invalidate cache if opening same file for writing — prevents
     * stale stdio buffers after the write handle modifies the file. */
    if (!read_only && g_cached_file && g_cached_nfile == pos->nFile &&
        strcmp(g_cached_prefix, prefix) == 0) {
        fclose(g_cached_file);
        g_cached_file = NULL;
        g_cached_nfile = -1;
    }

    char path[512];
    get_block_pos_filename(path, sizeof(path), datadir, pos, prefix);

    FILE *file = fopen(path, "rb+");
    if (!file && !read_only)
        file = fopen(path, "wb+");
    if (!file) {
        fprintf(stderr, "open_disk_file: cannot open %s: %s\n",
                path, strerror(errno));
        return NULL;
    }

    if (pos->nPos) {
        if (fseek(file, (long)pos->nPos, SEEK_SET)) {
            fprintf(stderr, "open_disk_file: fseek to %u failed in %s: %s\n",
                    pos->nPos, path, strerror(errno));
            fclose(file);
            return NULL;
        }
    }

    /* Cache read-only handles for sequential access */
    if (read_only) {
        /* Close any previous cached handle. Reaching here means the
         * cache-hit test above missed, so the cached handle (if any) is
         * for a different (nFile, prefix) pair — note blk and rev files
         * share numbers, so comparing nFile alone would leak the old
         * handle when only the prefix differs. */
        if (g_cached_file)
            fclose(g_cached_file);
        g_cached_file = file;
        g_cached_nfile = pos->nFile;
        snprintf(g_cached_prefix, sizeof(g_cached_prefix), "%s", prefix);
    }

    return file;
}

bool write_block_to_disk(struct block *b, struct disk_block_pos *pos,
                         const char *datadir,
                         const unsigned char message_start[4])
{
    /* Serialize first (outside lock) to minimize lock hold time */
    struct byte_stream s;
    stream_init(&s, 4096);
    if (!block_serialize(b, &s)) {
        stream_free(&s);
        LOG_FAIL("disk_block_io", "write_block: block serialization failed");
    }
    uint32_t nSize = (uint32_t)s.size;

    /* Hold mutex for entire file operation to prevent concurrent
     * read_block_from_disk from seeing partial writes or getting a
     * stale cached FILE* handle. */
    pthread_mutex_lock(&g_file_cache_mutex);
    if (pos->nFile < 0 &&
        !choose_append_block_pos(pos, nSize, datadir)) {
        pthread_mutex_unlock(&g_file_cache_mutex);
        stream_free(&s);
        LOG_FAIL("disk_block_io", "write_block: append position allocation failed");
    }

    FILE *file = open_block_file(datadir, pos, false);
    if (!file) {
        pthread_mutex_unlock(&g_file_cache_mutex);
        stream_free(&s);
        LOG_FAIL("disk_block_io", "write_block: open_block_file failed for file=%d", pos->nFile);
    }

    long file_pos = ftell(file);
    if (file_pos < 0) {
        fclose(file);
        pthread_mutex_unlock(&g_file_cache_mutex);
        stream_free(&s);
        LOG_FAIL("disk_block_io", "write_block: ftell failed");
    }

    if (fwrite(message_start, 1, 4, file) != 4 || // disk-io-lock: held (internal)
        fwrite(&nSize, sizeof(nSize), 1, file) != 1) {
        fclose(file);
        pthread_mutex_unlock(&g_file_cache_mutex);
        stream_free(&s);
        LOG_FAIL("disk_block_io", "write_block: fwrite header failed for file=%d", pos->nFile);
    }

    long data_pos = ftell(file);
    if (data_pos < 0 || (unsigned long)data_pos > UINT32_MAX) {
        fclose(file);
        pthread_mutex_unlock(&g_file_cache_mutex);
        stream_free(&s);
        LOG_FAIL("disk_block_io", "write_block: data position out of range (pos=%ld)", data_pos);
    }

    if (fwrite(s.data, 1, s.size, file) != s.size) { // disk-io-lock: held (internal)
        fclose(file);
        pthread_mutex_unlock(&g_file_cache_mutex);
        stream_free(&s);
        LOG_FAIL("disk_block_io", "write_block: fwrite block data failed (size=%zu)", s.size);
    }

    /* Flush to disk before reporting success — prevents silent data loss
     * on power failure. fdatasync skips metadata update (faster).
     *
     * Deferred mode (reducer fold / catch-up drain): push the stdio buffer to
     * the kernel now (fflush) so a concurrent reader sees the bytes, but defer
     * the fdatasync to the drain-batch boundary (disk_block_io_sync_pending,
     * fired before the stage COMMIT). Record the file as pending; on set
     * overflow fall back to an inline fdatasync so a file is never left
     * unsynced. Both g_deferred_sync and the pending set are under the mutex
     * held here. */
    if (fflush(file) != 0) {
        fprintf(stderr, "write_block_to_disk: fflush failed: %s\n",
                strerror(errno));
        fclose(file);
        pthread_mutex_unlock(&g_file_cache_mutex);
        stream_free(&s);
        return false;
    }
    bool sync_now = true;
    if (g_deferred_sync) {
        char wpath[512];
        get_block_pos_filename(wpath, sizeof(wpath), datadir, pos, "blk");
        if (deferred_record_pending_locked(wpath))
            sync_now = false; /* synced later at the drain-batch boundary */
    }
    if (sync_now && fdatasync(fileno(file)) != 0) {
        fprintf(stderr, "write_block_to_disk: fdatasync failed: %s\n",
                strerror(errno));
        fclose(file);
        pthread_mutex_unlock(&g_file_cache_mutex);
        stream_free(&s);
        return false;
    }

    /* Only record position AFTER data is confirmed on disk.
     * If we crash before this, caller retries from scratch — safe. */
    pos->nPos = (unsigned int)data_pos;

    fclose(file);
    pthread_mutex_unlock(&g_file_cache_mutex);
    stream_free(&s);
    return true;
}

bool read_block_from_disk(struct block *b, const struct disk_block_pos *pos,
                          const char *datadir)
{
    /* Delegate to pread()-based implementation — thread-safe without
     * the shared FILE* cache or mutex. All callers automatically
     * benefit from concurrent-safe reads. */
    return read_block_from_disk_pread(b, pos, datadir);
}

bool read_block_from_disk_index(struct block *b,
                                const struct block_index *pindex,
                                const char *datadir)
{
    /* Delegate to pread()-based implementation — thread-safe. */
    return read_block_from_disk_index_pread(b, pindex, datadir);
}

/* ── Thread-safe pread()-based I/O ───────────────────────────────
 * No shared state, no mutex, no FILE* cache. Safe for concurrent
 * use from any number of threads simultaneously. */

ssize_t disk_block_pread(const char *datadir, const struct disk_block_pos *pos,
                         const char *prefix, uint8_t *buf, size_t len)
{
    if (!datadir || !pos || !buf || pos->nFile < 0)
        LOG_ERR("disk_block_io", "pread: invalid arguments (datadir=%p pos=%p buf=%p)",
                (const void *)datadir, (const void *)pos, (const void *)buf);

    char path[512];
    get_block_pos_filename(path, sizeof(path), datadir, pos, prefix);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        /* Throttled per nFile (see g_open_fail_throttle rationale above). The
         * boot-time pprev-repair (block_index_integrity.c) walks EVERY had-data
         * entry through here; on a blocks-less bundle that is ~3.1M absent-file
         * opens before the snapshot repair clears the stale flags. */
        uint64_t reps = 0;
        if (log_throttle_should_emit(&g_open_fail_throttle,
                                     0u,  /* single global key (see throttle rationale above) */
                                     platform_time_wall_unix(), 60, &reps))
            fprintf(stderr, "[disk_block_io] disk_block_pread: cannot open %s "
                    "(%llu suppressed repeats since last log)\n",
                    path, (unsigned long long)reps);
        return -1;
    }

    ssize_t nread = pread(fd, buf, len, (off_t)pos->nPos);
    close(fd);
    return nread;
}

static bool disk_block_frame_header_valid(const uint8_t hdr[8],
                                          uint32_t *out_size)
{
    bool magic_ok = (hdr[0] == 0x24 && hdr[1] == 0xe9 &&
                     hdr[2] == 0x27 && hdr[3] == 0x64) ||
                    (hdr[0] == 0xfa && hdr[1] == 0x1a &&
                     hdr[2] == 0xf9 && hdr[3] == 0xbf) ||
                    (hdr[0] == 0xaa && hdr[1] == 0xe8 &&
                     hdr[2] == 0x3f && hdr[3] == 0x5f);
    uint32_t block_size = 0;
    memcpy(&block_size, hdr + 4, 4);
    if (!magic_ok || block_size == 0 || block_size > 2000000u)
        return false;
    if (out_size)
        *out_size = block_size;
    return true;
}

static bool disk_block_locate_payload(int fd,
                                      const struct disk_block_pos *pos,
                                      uint32_t *out_payload_pos,
                                      size_t *out_size)
{
    if (!pos || !out_payload_pos || !out_size)
        return false;

    uint8_t hdr[8];
    uint32_t block_size = 0;

    /* Canonical block indexes store the payload offset. Check the
     * frame header immediately before it first. */
    if (pos->nPos >= 8) {
        ssize_t hr = pread(fd, hdr, sizeof(hdr), (off_t)(pos->nPos - 8));
        if (hr == (ssize_t)sizeof(hdr) &&
            disk_block_frame_header_valid(hdr, &block_size)) {
            *out_payload_pos = pos->nPos;
            *out_size = block_size;
            return true;
        }
    }

    /* Some recovery/import paths may hand us the frame offset instead
     * of the payload offset. Accept that shape too; it is cheaper and
     * safer to read the block than to strand validation behind a
     * recoverable offset convention mismatch. */
    ssize_t hr = pread(fd, hdr, sizeof(hdr), (off_t)pos->nPos);
    if (hr == (ssize_t)sizeof(hdr) &&
        disk_block_frame_header_valid(hdr, &block_size)) {
        if (pos->nPos > UINT32_MAX - 8u)
            return false;
        *out_payload_pos = pos->nPos + 8u;
        *out_size = block_size;
        return true;
    }

    *out_payload_pos = pos->nPos;
    *out_size = 2000000u;
    return true;
}

bool read_block_from_disk_pread(struct block *b,
                                const struct disk_block_pos *pos,
                                const char *datadir)
{
    block_init(b);

    if (!datadir || !pos || pos->nFile < 0)
        LOG_FAIL("disk_block_io", "read_block_pread: invalid arguments (datadir=%p pos=%p)",
                 (const void *)datadir, (const void *)pos);

    char path[512];
    get_block_pos_filename(path, sizeof(path), datadir, pos, "blk");

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        /* Throttled: a blocks-less bundle would otherwise emit this per block
         * (~3.1M lines). Still logs context (the path) on the throttled emit. */
        uint64_t reps = 0;
        if (log_throttle_should_emit(&g_open_fail_throttle,
                                     0u,  /* single global key (see throttle rationale above) */
                                     platform_time_wall_unix(), 60, &reps))
            fprintf(stderr, "[disk_block_io] read_block_pread: cannot open %s "
                    "(%llu suppressed repeats since last log)\n",
                    path, (unsigned long long)reps);
        return false;
    }

    uint32_t payload_pos = pos->nPos;
    size_t bufsize = 2000000u;
    if (!disk_block_locate_payload(fd, pos, &payload_pos, &bufsize)) {
        close(fd);
        LOG_FAIL("disk_block_io",
                 "read_block_pread: locate payload failed for file=%d pos=%u",
                 pos->nFile, pos->nPos);
    }

    unsigned char *buf = zcl_malloc(bufsize, "read_block_pread_buf");
    if (!buf) {
        close(fd);
        LOG_FAIL("disk_block_io", "read_block_pread: malloc(%zu) failed", bufsize);
    }

    ssize_t nread = pread(fd, buf, bufsize, (off_t)payload_pos);
    close(fd);

    if (nread <= 0) {
        free(buf);
        LOG_FAIL("disk_block_io", "read_block_pread: pread returned %zd for file=%d pos=%u",
                 nread, pos->nFile, payload_pos);
    }

    struct byte_stream s;
    stream_init_from_data(&s, buf, (size_t)nread);
    bool ok = block_deserialize(b, &s);
    stream_free(&s);
    free(buf);
    return ok;
}

bool read_block_from_disk_index_pread(struct block *b,
                                      const struct block_index *pindex,
                                      const char *datadir)
{
    if (!pindex)
        LOG_FAIL("disk_block_io", "read_block_from_disk_index_pread: pindex is NULL");

    struct disk_block_pos pos;
    disk_block_pos_init(&pos);
    if (!block_index_disk_pos_snapshot(pindex, &pos, NULL))
        return false;

    if (!read_block_from_disk_pread(b, &pos, datadir)) {
        /* Throttled per nFile (see g_open_fail_throttle rationale above). */
        uint64_t reps = 0;
        if (log_throttle_should_emit(&g_readfail_throttle,
                                     0u,  /* single global key (see throttle rationale above) */
                                     platform_time_wall_unix(), 60, &reps))
            fprintf(stderr, "read_block_pread_fail: h=%d file=%d pos=%u "
                    "(%llu suppressed repeats since last log)\n",
                    pindex->nHeight, pos.nFile, pos.nPos,
                    (unsigned long long)reps);
        return false;
    }

    struct uint256 block_hash;
    block_get_hash(b, &block_hash);
    if (pindex->phashBlock &&
        uint256_cmp(&block_hash, pindex->phashBlock) != 0) {
        char got[65], want[65];
        uint256_get_hex(&block_hash, got);
        uint256_get_hex(pindex->phashBlock, want);
        fprintf(stderr, "read_block_pread_hash_mismatch: h=%d got=%.16s want=%.16s\n",
                pindex->nHeight, got, want);
        block_free(b);
        return false;
    }
    return true;
}

bool block_index_have_data_readable(const struct block_index *pindex,
                                    const char *datadir)
{
    struct disk_block_pos pos;
    disk_block_pos_init(&pos);
    if (!pindex || !datadir || !pindex->phashBlock ||
        !block_index_disk_pos_snapshot(pindex, &pos, NULL))
        return false;

    struct block blk;
    block_init(&blk);
    bool ok = read_block_from_disk_index_pread(&blk, pindex, datadir);
    block_free(&blk);
    return ok;
}

bool block_index_set_have_data_verified(struct block_index *pindex,
                                        const struct disk_block_pos *pos,
                                        const char *datadir)
{
    if (!pindex || !pos || !datadir || pos->nFile < 0)
        LOG_FAIL("disk_block_io",
                 "set_have_data_verified: invalid argument (pindex=%p pos=%p)",
                 (void *)pindex, (const void *)pos);
    if (!pindex->phashBlock)
        LOG_FAIL("disk_block_io",
                 "set_have_data_verified: missing block hash at h=%d",
                 pindex->nHeight);

    struct block blk;
    block_init(&blk);
    if (!read_block_from_disk_pread(&blk, pos, datadir)) {
        block_free(&blk);
        LOG_FAIL("disk_block_io",
                 "set_have_data_verified: read-back failed h=%d file=%d pos=%u",
                 pindex->nHeight, pos->nFile, pos->nPos);
    }

    struct uint256 got;
    block_get_hash(&blk, &got);
    bool hash_ok = uint256_cmp(&got, pindex->phashBlock) == 0;
    block_free(&blk);
    if (!hash_ok) {
        char got_hex[65], want_hex[65];
        uint256_get_hex(&got, got_hex);
        uint256_get_hex(pindex->phashBlock, want_hex);
        LOG_FAIL("disk_block_io",
                 "set_have_data_verified: hash mismatch h=%d got=%.16s want=%.16s",
                 pindex->nHeight, got_hex, want_hex);
    }

    block_index_disk_pos_store(pindex, pos->nFile, pos->nPos);
    block_index_status_fetch_or(pindex, BLOCK_HAVE_DATA);
    return true;
}
