/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"
#include "services/node_db_catchup_service.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define NDC_CHECK(name, expr) do { \
    printf("node_db_catchup_service: %s... ", (name)); \
    if (expr) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

int test_node_db_catchup_service(void)
{
    int failures = 0;
    printf("\n=== node_db_catchup_service tests ===\n");

    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "node_db_catchup", "mmap");
    char blocks[512];
    snprintf(blocks, sizeof(blocks), "%s/blocks", dir);
    mkdir(blocks, 0755);

    size_t sz = 99;
    int err = 0;
    uint8_t *data = node_db_catchup_test_mmap_block_file_quiet(
        dir, 7, &sz, &err);
    NDC_CHECK("missing block file is quiet ENOENT",
              data == NULL && sz == 0 && err == ENOENT);

    char path[512];
    snprintf(path, sizeof(path), "%s/blk00008.dat", blocks);
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
    sz = 99;
    err = 0;
    data = node_db_catchup_test_mmap_block_file_quiet(dir, 8, &sz, &err);
    NDC_CHECK("empty block file is quiet EINVAL",
              data == NULL && sz == 0 && err == EINVAL);

    snprintf(path, sizeof(path), "%s/blk00009.dat", blocks);
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    const uint8_t bytes[] = {0xde, 0xad, 0xbe, 0xef};
    bool wrote = fd >= 0 &&
        write(fd, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes);
    if (fd >= 0) close(fd);
    sz = 0;
    err = 0;
    data = node_db_catchup_test_mmap_block_file_quiet(dir, 9, &sz, &err);
    bool mapped = wrote && data != NULL && sz == sizeof(bytes) && err == 0 &&
                  memcmp(data, bytes, sizeof(bytes)) == 0;
    if (data) munmap(data, sz);
    NDC_CHECK("valid block file maps", mapped);

    NDC_CHECK("sparse proven prefix may advance projection cursor",
              node_db_catchup_test_sparse_prefix_can_advance(
                  0, 3, 3, 0, 0, 2, 0, 0, 0, true, 2));
    NDC_CHECK("sparse prefix requires proven coins authority",
              !node_db_catchup_test_sparse_prefix_can_advance(
                  0, 3, 3, 0, 0, 2, 0, 0, 0, false, 2));
    NDC_CHECK("sparse prefix requires authority covering tip",
              !node_db_catchup_test_sparse_prefix_can_advance(
                  0, 3, 3, 0, 0, 2, 0, 0, 0, true, 1));
    NDC_CHECK("sparse prefix allows quiet missing files",
              node_db_catchup_test_sparse_prefix_can_advance(
                  0, 3, 3, 0, 0, 2, 0, 1, 0, true, 2));
    NDC_CHECK("sparse prefix refuses suspicious holes",
              !node_db_catchup_test_sparse_prefix_can_advance(
                  0, 3, 3, 0, 0, 2, 1, 1, 0, true, 2));
    NDC_CHECK("sparse prefix refuses missing active-chain indexes",
              !node_db_catchup_test_sparse_prefix_can_advance(
                  0, 3, 3, 0, 0, 2, 0, 0, 1, true, 2));
    NDC_CHECK("sparse prefix must cover the whole range",
              !node_db_catchup_test_sparse_prefix_can_advance(
                  0, 3, 2, 0, 0, 2, 0, 0, 0, true, 2));

    test_cleanup_tmpdir(dir);
    printf("node_db_catchup_service: %d failures\n", failures);
    return failures;
}
