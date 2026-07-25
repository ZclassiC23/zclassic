/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "config/boot_legacy_blocks.h"

#include "config/file_ops.h"
#include "util/log_macros.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool boot_legacy_blocks_dir(char *out, size_t out_n,
                                   const char *datadir)
{
    if (!out || out_n == 0 || !datadir || !*datadir)
        return false;

    int n = snprintf(out, out_n, "%s/blocks", datadir);
    return n >= 0 && (size_t)n < out_n;
}

static bool boot_legacy_file_path(char *out, size_t out_n,
                                  const char *blocks_dir,
                                  const char *prefix,
                                  int file_index)
{
    if (!out || out_n == 0 || !blocks_dir || !*blocks_dir ||
        !prefix || !*prefix || file_index < 0)
        return false;

    int n = snprintf(out, out_n, "%s/%s%05d.dat", blocks_dir, prefix,
                     file_index);
    return n >= 0 && (size_t)n < out_n;
}

static bool boot_link_or_copy_import_block_file(const char *src,
                                                const char *dst,
                                                const char *prefix,
                                                int file_index,
                                                long long bytes,
                                                bool announce)
{
    if (link(src, dst) == 0) {
        if (announce && file_index % 10 == 0)
            printf("  linked %s%05d.dat (%lld MB)\n",
                   prefix, file_index, bytes >> 20);
        return true;
    }

    int link_errno = errno;
    if (announce) {
        printf("  copying %s%05d.dat (%lld MB)...\n",
               prefix, file_index, bytes >> 20);
        fflush(stdout);
    }

    if (file_copy(src, dst))
        return true;

    int copy_errno = errno;
    LOG_WARN("boot",
             "[boot] failed to link/copy %s%05d.dat from %s to %s "
             "(link errno=%d %s, copy errno=%d %s)",
             prefix, file_index, src, dst,
             link_errno, strerror(link_errno),
             copy_errno, strerror(copy_errno));
    return false;
}

struct boot_legacy_block_file_import_result
boot_legacy_import_block_files(const char *legacy_blocks_dir,
                               const char *datadir,
                               int max_files)
{
    struct boot_legacy_block_file_import_result result = {0};

    char dst_blocks_dir[1024];
    if (!boot_legacy_blocks_dir(dst_blocks_dir, sizeof(dst_blocks_dir),
                                datadir)) {
        result.truncated_path = true;
        return result;
    }
    result.destination_ready = true;

    if (!legacy_blocks_dir || !*legacy_blocks_dir || max_files <= 0)
        return result;

    for (int fi = 0; fi < max_files; fi++) {
        char src_path[1200], dst_path[1200];
        if (!boot_legacy_file_path(src_path, sizeof(src_path),
                                   legacy_blocks_dir, "blk", fi) ||
            !boot_legacy_file_path(dst_path, sizeof(dst_path),
                                   dst_blocks_dir, "blk", fi)) {
            result.truncated_path = true;
            break;
        }

        struct stat src_st, dst_st;
        if (stat(src_path, &src_st) != 0) {
            if (fi > 2)
                break;
            continue;
        }
        result.source_available = true;

        /* Preserve the historical boot behavior: if the blk file already
         * exists with the same size, this index is complete enough for this
         * pass and rev linking is left to the later warm-boot helper. */
        if (stat(dst_path, &dst_st) == 0 &&
            dst_st.st_size == src_st.st_size)
            continue;

        int index_failures = 0;
        if (!boot_link_or_copy_import_block_file(
                src_path, dst_path, "blk", fi,
                (long long)src_st.st_size, true))
            index_failures++;

        if (!boot_legacy_file_path(src_path, sizeof(src_path),
                                   legacy_blocks_dir, "rev", fi) ||
            !boot_legacy_file_path(dst_path, sizeof(dst_path),
                                   dst_blocks_dir, "rev", fi)) {
            result.truncated_path = true;
            break;
        }
        if (stat(src_path, &src_st) == 0) {
            if (!boot_link_or_copy_import_block_file(
                    src_path, dst_path, "rev", fi,
                    (long long)src_st.st_size, false))
                index_failures++;
        }

        if (index_failures > 0) {
            result.failures += index_failures;
            LOG_WARN("boot",
                     "[boot] %d zclassicd block-file import operation(s) "
                     "failed for file index %d",
                     index_failures, fi);
        }
    }

    return result;
}

/* Hardlink src -> dst if dst is absent. Returns true only when this call
 * created the link. A failure is counted and its errno remembered (first one
 * wins) so the caller can report the whole pass in one line; EEXIST is not a
 * failure — it means another writer won the race and the file is there. */
static bool boot_legacy_link_if_missing(const char *src, const char *dst,
                                        int *failures, int *first_errno)
{
    struct stat st;
    if (stat(dst, &st) == 0)
        return false;
    if (link(src, dst) == 0)
        return true;
    if (errno == EEXIST)
        return false;
    (*failures)++;
    if (*first_errno == 0)
        *first_errno = errno;
    return false;
}

struct boot_legacy_block_file_link_result
boot_legacy_link_missing_block_files(const char *legacy_blocks_dir,
                                     const char *datadir,
                                     int max_files)
{
    struct boot_legacy_block_file_link_result result = {0};

    char dst_blocks_dir[1024];
    if (!boot_legacy_blocks_dir(dst_blocks_dir, sizeof(dst_blocks_dir),
                                datadir)) {
        result.truncated_path = true;
        return result;
    }
    result.destination_ready = true;

    if (!legacy_blocks_dir || !*legacy_blocks_dir || max_files <= 0)
        return result;

    int first_errno = 0;
    for (int fi = 0; fi < max_files; fi++) {
        char src[1200], dst[1200];
        if (!boot_legacy_file_path(src, sizeof(src), legacy_blocks_dir,
                                   "blk", fi) ||
            !boot_legacy_file_path(dst, sizeof(dst), dst_blocks_dir,
                                   "blk", fi)) {
            result.truncated_path = true;
            break;
        }

        struct stat ss;
        if (stat(src, &ss) != 0) {
            if (fi > 2)
                break;
            continue;
        }
        result.source_available = true;

        /* A missing blk file IS load-bearing: block bodies are read straight
         * out of blk%05d.dat by the refold, catchup, wallet-scan and
         * file-service paths, so a link that silently does not happen shows
         * up much later as an unexplained body-read hole. */
        if (boot_legacy_link_if_missing(src, dst, &result.failures,
                                        &first_errno))
            result.linked++;

        if (!boot_legacy_file_path(src, sizeof(src), legacy_blocks_dir,
                                   "rev", fi) ||
            !boot_legacy_file_path(dst, sizeof(dst), dst_blocks_dir,
                                   "rev", fi)) {
            result.truncated_path = true;
            break;
        }
        /* rev%05d.dat is zclassicd's undo data. No zclassic23 code path reads
         * it — the node re-derives its own state rather than replaying
         * zclassicd undo records — so a failed rev link cannot make the node
         * wrong, and it is deliberately NOT counted in `linked`. It is counted
         * in `failures` because a rev link failing for a reason that would
         * equally hit blk files (EXDEV across filesystems, ENOSPC, EPERM) is
         * exactly the early warning worth having; the copy-fallback import
         * path already reports its rev failures the same way. */
        if (stat(src, &ss) == 0)
            (void)boot_legacy_link_if_missing(src, dst, &result.failures,
                                              &first_errno);
    }

    /* One aggregated line, not one per file: a cross-filesystem legacy
     * datadir makes every one of the 256 candidates fail identically, and
     * 256 warnings would bury the boot log. */
    if (result.failures > 0)
        LOG_WARN("boot",
                 "[boot] %d legacy block-file hardlink(s) failed while "
                 "linking %s into %s (first errno=%d %s); block bodies for "
                 "those files are not present in this datadir",
                 result.failures, legacy_blocks_dir, dst_blocks_dir,
                 first_errno, strerror(first_errno));

    return result;
}
