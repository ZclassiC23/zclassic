/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"
#include "services/node_db_catchup_service.h"
#include "models/database.h"
#include "controllers/sync_controller.h"
#include "validation/chainstate.h"
#include "chain/chain.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "storage/disk_block_io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
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

    /* A torn index (cp -a of a running node) can hand the catchup walk a
     * block_index carrying BLOCK_HAVE_DATA yet a NULL phashBlock. That must
     * fail-closed with a named log — never a SIGSEGV in sync_block_lean. */
    {
        struct node_db ndb;
        bool opened = node_db_open(&ndb, ":memory:");
        struct block blk;
        block_init(&blk);
        blk.header.nVersion = 4;
        blk.num_vtx = 1;
        blk.vtx = calloc(1, sizeof(struct transaction)); // raw-alloc-ok:test-fixture
        if (blk.vtx) {
            transaction_init(&blk.vtx[0]);
            transaction_alloc(&blk.vtx[0], 1, 1);
        }
        struct block_index torn;
        memset(&torn, 0, sizeof(torn));
        torn.nHeight = 1;
        torn.nStatus = BLOCK_HAVE_DATA;
        torn.phashBlock = NULL;
        bool guarded = opened && blk.vtx &&
            !node_db_catchup_test_sync_block_lean(&ndb, &blk, &torn);
        NDC_CHECK("sync_block_lean fails closed on a hash-less block index",
                  guarded);
        block_free(&blk);
        if (opened) node_db_close(&ndb);
    }

    /* Drive the full walk against a torn projection: a real decodable block
     * on disk reached through an active-chain slot whose phashBlock is NULL,
     * with a missing lower slot (h=0). The run must complete with a named
     * lean-hole outcome, never crash, and never advance the projection tip
     * onto the unidentifiable slot. */
    {
        char dir2[256];
        test_make_tmpdir(dir2, sizeof(dir2), "node_db_catchup", "torn_walk");
        char blocks2[512];
        snprintf(blocks2, sizeof(blocks2), "%s/blocks", dir2);
        mkdir(blocks2, 0755);

        struct block b;
        block_init(&b);
        b.header.nVersion = 4;
        b.header.nTime = 1700000123u;
        b.header.nBits = 0x2000ffffu;
        b.header.hashPrevBlock.data[0] = 0x01;
        b.header.hashMerkleRoot.data[0] = 0x02;
        b.num_vtx = 1;
        b.vtx = calloc(1, sizeof(struct transaction)); // raw-alloc-ok:test-fixture
        if (b.vtx) {
            transaction_init(&b.vtx[0]);
            transaction_alloc(&b.vtx[0], 1, 1);
            b.vtx[0].vin[0].sequence = 0xffffffffu;
            b.vtx[0].vout[0].value = 5000000000LL;
        }
        struct disk_block_pos pos;
        disk_block_pos_init(&pos);
        unsigned char msg_start[4] = {0x24, 0xe9, 0x27, 0x64};
        bool wrote = b.vtx && write_block_to_disk(&b, &pos, dir2, msg_start);
        struct uint256 block_hash;
        block_get_hash(&b, &block_hash);
        block_free(&b);

        struct block_index torn;
        memset(&torn, 0, sizeof(torn));
        torn.nHeight = 1;
        torn.nStatus = BLOCK_HAVE_DATA;
        torn.nFile = pos.nFile;
        torn.nDataPos = pos.nPos;
        torn.phashBlock = NULL;

        struct active_chain ac;
        active_chain_init(&ac);
        bool installed = active_chain_install_tip_slot(&ac, &torn);

        struct node_db ndb;
        char ndb_path[512];
        snprintf(ndb_path, sizeof(ndb_path), "%s/node.db", dir2);
        bool opened = node_db_open(&ndb, ndb_path);

        /* A pre-run sentinel: if the walk SIGSEGVs on the hash-less slot,
         * the process dies here and the group fails with a signal. Reaching
         * the assertions with result reassigned proves the walk returned a
         * named outcome (a catchup abort returns -1 via LOG_ERR) instead. */
        int result = -99;
        if (wrote && installed && opened)
            result = node_db_catchup_service_run(&ndb, &ac, NULL, dir2);

        int tip = opened ? node_db_sync_get_tip_height(&ndb) : -1;
        NDC_CHECK("catchup survives a torn hash-less slot without crashing",
                  wrote && installed && opened && result != -99);
        NDC_CHECK("catchup refuses to advance the projection onto a torn slot",
                  tip < 1);

        /* Repair only the torn identity and run the same on-disk block through
         * the real catchup transaction. This reaches advance_wallet_witnesses
         * with sync_in_batch=false while the catchup's plain BEGIN is open —
         * the exact post-bundle path that previously misclassified its own
         * transaction as foreign and aborted on the first block. */
        torn.phashBlock = &block_hash;
        int repaired_result = opened
            ? node_db_catchup_service_run(&ndb, &ac, NULL, dir2) : -1;
        int repaired_tip = opened ? node_db_sync_get_tip_height(&ndb) : -1;
        int64_t tree_height = -1;
        bool tree_persisted = opened &&
            node_db_state_get_int(&ndb, "sapling_tree_rebuild_height",
                                  &tree_height);
        NDC_CHECK("catchup advances through its caller-owned plain transaction",
                  repaired_result == 1 && repaired_tip == 1);
        NDC_CHECK("catchup commits the Sapling tree/height pair atomically",
                  tree_persisted && tree_height == 1);

        if (opened) node_db_close(&ndb);
        active_chain_free(&ac);
        test_cleanup_tmpdir(dir2);
    }

    test_cleanup_tmpdir(dir);
    printf("node_db_catchup_service: %d failures\n", failures);
    return failures;
}
