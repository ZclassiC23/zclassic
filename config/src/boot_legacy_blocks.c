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

    for (int fi = 0; fi < max_files; fi++) {
        char src[1200], dst[1200];
        if (!boot_legacy_file_path(src, sizeof(src), legacy_blocks_dir,
                                   "blk", fi) ||
            !boot_legacy_file_path(dst, sizeof(dst), dst_blocks_dir,
                                   "blk", fi)) {
            result.truncated_path = true;
            break;
        }

        struct stat ss, ds;
        if (stat(src, &ss) != 0) {
            if (fi > 2)
                break;
            continue;
        }
        result.source_available = true;

        if (stat(dst, &ds) != 0 && link(src, dst) == 0)
            result.linked++;

        if (!boot_legacy_file_path(src, sizeof(src), legacy_blocks_dir,
                                   "rev", fi) ||
            !boot_legacy_file_path(dst, sizeof(dst), dst_blocks_dir,
                                   "rev", fi)) {
            result.truncated_path = true;
            break;
        }
        if (stat(src, &ss) == 0 && stat(dst, &ds) != 0)
            (void)link(src, dst);
    }

    return result;
}
