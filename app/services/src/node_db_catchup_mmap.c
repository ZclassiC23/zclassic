/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Quiet block-file mmap helper for node_db catchup. */
// one-result-type-ok:internal-mmap-helper-pointer-errno

#include "node_db_catchup_internal.h"

#include "services/node_db_catchup_service.h"
#include "util/log_macros.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

uint8_t *node_db_catchup_mmap_block_file_quiet(const char *datadir,
                                               int file_num,
                                               size_t *out_size,
                                               int *out_errno)
{
    if (out_size) *out_size = 0;
    if (out_errno) *out_errno = 0;

    char path[512];
    snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
             datadir, file_num);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (out_errno) *out_errno = errno;
        return NULL;
    }

    struct stat fst;
    if (fstat(fd, &fst) != 0) {
        int e = errno;
        close(fd);
        if (out_errno) *out_errno = e;
        LOG_NULL("sync", "catchup mmap: fstat failed for %s", path);
    }
    if (fst.st_size <= 0) {
        close(fd);
        if (out_errno) *out_errno = EINVAL;
        return NULL;
    }

    uint8_t *data = mmap(NULL, (size_t)fst.st_size,
                         PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) {
        if (out_errno) *out_errno = errno;
        LOG_NULL("sync", "catchup mmap: mmap failed for file %d", file_num);
    }
    if (out_size) *out_size = (size_t)fst.st_size;
    posix_madvise(data, (size_t)fst.st_size, POSIX_MADV_SEQUENTIAL);
    posix_madvise(data, (size_t)fst.st_size, POSIX_MADV_WILLNEED);
    return data;
}

#ifdef ZCL_TESTING
uint8_t *node_db_catchup_test_mmap_block_file_quiet(const char *datadir,
                                                    int file_num,
                                                    size_t *out_size,
                                                    int *out_errno)
{
    return node_db_catchup_mmap_block_file_quiet(datadir, file_num,
                                                 out_size, out_errno);
}
#endif
