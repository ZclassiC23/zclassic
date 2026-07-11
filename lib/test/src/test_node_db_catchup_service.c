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
              node_db_catchup_test_sparse_prefix_target(
                  0, 3, 3, 0, 0, 2, 0, 0, -1, true, 2) == 2);
    NDC_CHECK("sparse prefix requires proven coins authority",
              node_db_catchup_test_sparse_prefix_target(
                  0, 3, 3, 0, 0, 2, 0, 0, -1, false, 2) == -1);
    NDC_CHECK("sparse prefix requires authority covering tip",
              node_db_catchup_test_sparse_prefix_target(
                  0, 3, 3, 0, 0, 2, 0, 0, -1, true, 1) == -1);
    NDC_CHECK("sparse prefix allows quiet missing body files",
              node_db_catchup_test_sparse_prefix_target(
                  0, 3, 3, 0, 0, 2, 0, 0, -1, true, 2) == 2);
    NDC_CHECK("sparse prefix refuses suspicious holes",
              node_db_catchup_test_sparse_prefix_target(
                  0, 3, 3, 0, 0, 2, 1, 0, -1, true, 2) == -1);
    NDC_CHECK("sparse prefix stops before an interior missing active slot",
              node_db_catchup_test_sparse_prefix_target(
                  0, 3, 3, 0, 0, 2, 0, 1, 1, true, 2) == 0);
    NDC_CHECK("sparse prefix must cover the whole range",
              node_db_catchup_test_sparse_prefix_target(
                  0, 3, 2, 0, 0, 2, 0, 0, -1, true, 2) == -1);
    NDC_CHECK("sparse prefix stops before a trailing missing active slot",
              node_db_catchup_test_sparse_prefix_target(
                  0, 3, 3, 0, 0, 2, 0, 1, 2, true, 2) == 1);
    NDC_CHECK("sparse prefix refuses a missing first active slot",
              node_db_catchup_test_sparse_prefix_target(
                  0, 3, 3, 0, 0, 2, 0, 1, 0, true, 2) == -1);
    NDC_CHECK("sparse prefix target remains bounded by proven authority",
              node_db_catchup_test_sparse_prefix_target(
                  0, 3, 3, 0, 0, 2, 0, 1, 2, true, 0) == -1);
    NDC_CHECK("sparse watcher waits for the sole missing tip slot",
              node_db_catchup_sparse_tip_slot_pending(true, 1, 2, false));
    NDC_CHECK("sparse watcher resumes once the tip slot resolves",
              !node_db_catchup_sparse_tip_slot_pending(true, 1, 2, true));
    NDC_CHECK("ordinary projections never enter sparse tip wait",
              !node_db_catchup_sparse_tip_slot_pending(false, 1, 2, false));
    /* Two-or-more missing TOP slots (chain_tip=3, projection_tip=1: heights
     * 2 AND 3 both missing active-chain indices). A fresh catchup pass would
     * start at height 2, find it missing, and publish target=1 — the same
     * projection_tip it already has, no progress — so the watcher must
     * suppress the restart just as it does for a single missing slot. This
     * is the exact defect this predicate was generalized to cover. */
    NDC_CHECK("sparse watcher waits when two-or-more top slots are missing",
              node_db_catchup_sparse_tip_slot_pending(true, 1, 3, false));
    /* Missing slot strictly interior (below projection_tip + 1) while the
     * next-needed slot IS present: a fresh catchup pass starting at
     * projection_tip + 1 makes real progress (the interior hole is a lean
     * hole handled inline by the run, not a reason to wait), so the watcher
     * must allow the restart. */
    NDC_CHECK("sparse watcher allows restart when the next slot is present "
              "despite an interior hole",
              !node_db_catchup_sparse_tip_slot_pending(true, 1, 5, true));

    test_cleanup_tmpdir(dir);
    printf("node_db_catchup_service: %d failures\n", failures);
    return failures;
}
