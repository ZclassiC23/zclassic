/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for disk_block_io thread safety and pread correctness. */

#include "test/test_helpers.h"
#include "storage/disk_block_io.h"
#include "primitives/block.h"
#include "core/serialize.h"
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Helpers ──────────────────────────────────────────────── */

static void make_test_dir(char *buf, size_t len)
{
    snprintf(buf, len, "./test-tmp/%d_disk_io", (int)getpid());
    mkdir("./test-tmp", 0755);
    mkdir(buf, 0755);
    char blocks[512];
    snprintf(blocks, sizeof(blocks), "%s/blocks", buf);
    mkdir(blocks, 0755);
}

static void cleanup_test_dir(const char *dir)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    (void)system(cmd);
}

/* Write a minimal block with a distinguishing nTime value. */
static bool write_test_block(const char *datadir, struct disk_block_pos *pos,
                             uint32_t ntime)
{
    struct block b;
    block_init(&b);
    b.header.nVersion = 4;
    b.header.nTime = ntime;
    b.header.nBits = 0x2000ffff;
    b.num_vtx = 1;
    b.vtx = calloc(1, sizeof(struct transaction)); // raw-alloc-ok:test-fixture
    transaction_init(&b.vtx[0]);
    transaction_alloc(&b.vtx[0], 1, 1);
    b.vtx[0].vin[0].sequence = 0xffffffff;
    b.vtx[0].vout[0].value = 10 * COIN;

    unsigned char msg_start[4] = {0x24, 0xe9, 0x27, 0x64};
    bool ok = write_block_to_disk(&b, pos, datadir, msg_start);
    block_free(&b);
    return ok;
}

/* ── Thread safety test context ──────────────────────────── */

struct reader_ctx {
    const char *datadir;
    struct disk_block_pos pos;
    uint32_t expected_ntime;
    int iterations;
    bool ok;
};

static void *reader_thread(void *arg)
{
    struct reader_ctx *ctx = arg;
    ctx->ok = true;

    for (int i = 0; i < ctx->iterations; i++) {
        struct block b;
        if (!read_block_from_disk(&b, &ctx->pos, ctx->datadir)) {
            ctx->ok = false;
            return NULL;
        }
        if (b.header.nTime != ctx->expected_ntime) {
            ctx->ok = false;
            block_free(&b);
            return NULL;
        }
        block_free(&b);
    }
    return NULL;
}

static void *pread_reader_thread(void *arg)
{
    struct reader_ctx *ctx = arg;
    ctx->ok = true;

    for (int i = 0; i < ctx->iterations; i++) {
        struct block b;
        if (!read_block_from_disk_pread(&b, &ctx->pos, ctx->datadir)) {
            ctx->ok = false;
            return NULL;
        }
        if (b.header.nTime != ctx->expected_ntime) {
            ctx->ok = false;
            block_free(&b);
            return NULL;
        }
        block_free(&b);
    }
    return NULL;
}

/* ── Tests ───────────────────────────────────────────────── */

static int test_pread_basic_read(void)
{
    int failures = 0;
    char tmpdir[256];
    make_test_dir(tmpdir, sizeof(tmpdir));

    TEST("pread: basic read matches written block") {
        struct disk_block_pos pos = { .nFile = 0, .nPos = 0 };
        if (!write_test_block(tmpdir, &pos, 12345)) {
            printf("FAIL (write)\n"); failures++; goto _test_next;
        }

        struct block b;
        if (!read_block_from_disk_pread(&b, &pos, tmpdir)) {
            printf("FAIL (pread read)\n"); failures++; goto _test_next;
        }
        if (b.header.nTime != 12345 || b.num_vtx != 1) {
            printf("FAIL (data mismatch: nTime=%u vtx=%zu)\n",
                   b.header.nTime, b.num_vtx);
            failures++;
            block_free(&b);
            goto _test_next;
        }
        block_free(&b);
        printf("OK\n");
    }
    _test_next:

    cleanup_test_dir(tmpdir);
    return failures;
}

static int test_pread_matches_fread(void)
{
    int failures = 0;
    char tmpdir[256];
    make_test_dir(tmpdir, sizeof(tmpdir));

    TEST("pread: result matches read_block_from_disk") {
        struct disk_block_pos pos = { .nFile = 0, .nPos = 0 };
        if (!write_test_block(tmpdir, &pos, 77777)) {
            printf("FAIL (write)\n"); failures++; goto _test_next;
        }

        /* read_block_from_disk now delegates to pread internally,
         * so both paths should produce identical results */
        struct block b1, b2;
        if (!read_block_from_disk(&b1, &pos, tmpdir)) {
            printf("FAIL (read)\n"); failures++; goto _test_next;
        }
        if (!read_block_from_disk_pread(&b2, &pos, tmpdir)) {
            printf("FAIL (pread)\n"); failures++;
            block_free(&b1);
            goto _test_next;
        }
        if (b1.header.nTime != b2.header.nTime ||
            b1.num_vtx != b2.num_vtx) {
            printf("FAIL (mismatch)\n"); failures++;
        } else {
            printf("OK\n");
        }
        block_free(&b1);
        block_free(&b2);
    }
    _test_next:

    cleanup_test_dir(tmpdir);
    return failures;
}

static int test_concurrent_reads(void)
{
    int failures = 0;
    char tmpdir[256];
    make_test_dir(tmpdir, sizeof(tmpdir));

    TEST("pread: 4 threads reading different blocks concurrently") {
        /* Write 4 blocks with different nTime values */
        struct disk_block_pos positions[4];
        uint32_t times[4] = {1000, 2000, 3000, 4000};

        for (int i = 0; i < 4; i++) {
            positions[i].nFile = i;
            positions[i].nPos = 0;
            if (!write_test_block(tmpdir, &positions[i], times[i])) {
                printf("FAIL (write block %d)\n", i);
                failures++;
                goto _test_next;
            }
        }

        /* Spawn 4 reader threads, each reading its own block 50 times */
        pthread_t threads[4];
        struct reader_ctx ctxs[4];
        for (int i = 0; i < 4; i++) {
            ctxs[i].datadir = tmpdir;
            ctxs[i].pos = positions[i];
            ctxs[i].expected_ntime = times[i];
            ctxs[i].iterations = 50;
            ctxs[i].ok = false;
            pthread_create(&threads[i], NULL, reader_thread, &ctxs[i]);
        }

        bool all_ok = true;
        for (int i = 0; i < 4; i++) {
            pthread_join(threads[i], NULL);
            if (!ctxs[i].ok) {
                printf("FAIL (thread %d)\n", i);
                all_ok = false;
            }
        }
        if (!all_ok) {
            failures++;
            goto _test_next;
        }
        printf("OK\n");
    }
    _test_next:

    cleanup_test_dir(tmpdir);
    return failures;
}

static int test_concurrent_pread_same_file(void)
{
    int failures = 0;
    char tmpdir[256];
    make_test_dir(tmpdir, sizeof(tmpdir));

    TEST("pread: 4 threads reading same file concurrently") {
        struct disk_block_pos pos = { .nFile = 0, .nPos = 0 };
        if (!write_test_block(tmpdir, &pos, 55555)) {
            printf("FAIL (write)\n"); failures++; goto _test_next;
        }

        pthread_t threads[4];
        struct reader_ctx ctxs[4];
        for (int i = 0; i < 4; i++) {
            ctxs[i].datadir = tmpdir;
            ctxs[i].pos = pos;
            ctxs[i].expected_ntime = 55555;
            ctxs[i].iterations = 100;
            ctxs[i].ok = false;
            pthread_create(&threads[i], NULL, pread_reader_thread, &ctxs[i]);
        }

        bool all_ok = true;
        for (int i = 0; i < 4; i++) {
            pthread_join(threads[i], NULL);
            if (!ctxs[i].ok) all_ok = false;
        }
        if (!all_ok) {
            printf("FAIL (concurrent pread)\n");
            failures++;
            goto _test_next;
        }
        printf("OK\n");
    }
    _test_next:

    cleanup_test_dir(tmpdir);
    return failures;
}

static int test_disk_block_pread_raw(void)
{
    int failures = 0;
    char tmpdir[256];
    make_test_dir(tmpdir, sizeof(tmpdir));

    TEST("disk_block_pread: raw byte read returns data") {
        struct disk_block_pos pos = { .nFile = 0, .nPos = 0 };
        if (!write_test_block(tmpdir, &pos, 99999)) {
            printf("FAIL (write)\n"); failures++; goto _test_next;
        }

        uint8_t buf[4096];
        ssize_t n = disk_block_pread(tmpdir, &pos, "blk", buf, sizeof(buf));
        if (n <= 0) {
            printf("FAIL (pread returned %zd)\n", n);
            failures++;
            goto _test_next;
        }
        printf("OK (%zd bytes)\n", n);
    }
    _test_next:

    cleanup_test_dir(tmpdir);
    return failures;
}

static int test_pread_accepts_frame_offset(void)
{
    int failures = 0;
    char tmpdir[256];
    make_test_dir(tmpdir, sizeof(tmpdir));

    TEST("pread: accepts frame offset as recoverable index shape") {
        struct disk_block_pos pos = { .nFile = 0, .nPos = 0 };
        if (!write_test_block(tmpdir, &pos, 424242)) {
            printf("FAIL (write)\n"); failures++; goto _test_next;
        }
        if (pos.nPos < 8) {
            printf("FAIL (unexpected payload pos %u)\n", pos.nPos);
            failures++;
            goto _test_next;
        }

        struct disk_block_pos frame_pos = pos;
        frame_pos.nPos -= 8;

        struct block b;
        if (!read_block_from_disk_pread(&b, &frame_pos, tmpdir)) {
            printf("FAIL (pread frame offset)\n");
            failures++;
            goto _test_next;
        }
        if (b.header.nTime != 424242 || b.num_vtx != 1) {
            printf("FAIL (data mismatch: nTime=%u vtx=%zu)\n",
                   b.header.nTime, b.num_vtx);
            failures++;
            block_free(&b);
            goto _test_next;
        }
        block_free(&b);
        printf("OK\n");
    }
    _test_next:

    cleanup_test_dir(tmpdir);
    return failures;
}

static int test_set_have_data_verified(void)
{
    int failures = 0;
    char tmpdir[256];
    make_test_dir(tmpdir, sizeof(tmpdir));

    TEST("set_have_data_verified: marks only after read-back hash match") {
        struct disk_block_pos pos = { .nFile = 0, .nPos = 0 };
        if (!write_test_block(tmpdir, &pos, 515151)) {
            printf("FAIL (write)\n"); failures++; goto _test_next;
        }

        struct block b;
        if (!read_block_from_disk_pread(&b, &pos, tmpdir)) {
            printf("FAIL (readback)\n"); failures++; goto _test_next;
        }
        struct uint256 hash;
        block_get_hash(&b, &hash);
        block_free(&b);

        struct block_index bi;
        block_index_init(&bi);
        bi.nHeight = 51;
        bi.phashBlock = &hash;
        if (!block_index_set_have_data_verified(&bi, &pos, tmpdir)) {
            printf("FAIL (verify helper)\n");
            failures++;
            goto _test_next;
        }
        if (!(bi.nStatus & BLOCK_HAVE_DATA) ||
            bi.nFile != pos.nFile || bi.nDataPos != pos.nPos) {
            printf("FAIL (index not marked correctly)\n");
            failures++;
            goto _test_next;
        }
        if (!block_index_have_data_readable(&bi, tmpdir)) {
            printf("FAIL (readability helper rejected valid data)\n");
            failures++;
            goto _test_next;
        }
        struct uint256 wrong = hash;
        wrong.data[0] ^= 0xff;
        bi.phashBlock = &wrong;
        if (block_index_have_data_readable(&bi, tmpdir)) {
            printf("FAIL (readability helper accepted mismatched data)\n");
            failures++;
            goto _test_next;
        }
        printf("OK\n");
    }
    _test_next:

    cleanup_test_dir(tmpdir);
    return failures;
}

static int test_write_allocates_append_position(void)
{
    int failures = 0;
    char tmpdir[256];
    make_test_dir(tmpdir, sizeof(tmpdir));

    TEST("write_block_to_disk: file=-1 allocates append position") {
        struct disk_block_pos pos;
        disk_block_pos_init(&pos);
        if (!write_test_block(tmpdir, &pos, 616161)) {
            printf("FAIL (write)\n"); failures++; goto _test_next;
        }
        if (pos.nFile < 0 || pos.nPos == 0) {
            printf("FAIL (position not allocated file=%d pos=%u)\n",
                   pos.nFile, pos.nPos);
            failures++;
            goto _test_next;
        }
        struct block b;
        if (!read_block_from_disk_pread(&b, &pos, tmpdir)) {
            printf("FAIL (readback)\n"); failures++; goto _test_next;
        }
        bool ok = b.header.nTime == 616161;
        block_free(&b);
        if (!ok) {
            printf("FAIL (wrong block read back)\n");
            failures++;
            goto _test_next;
        }
        printf("OK\n");
    }
    _test_next:

    cleanup_test_dir(tmpdir);
    return failures;
}

/* ── Entry point ─────────────────────────────────────────── */

int test_disk_block_io(void)
{
    printf("\n=== disk_block_io (pread thread safety) ===\n");
    int failures = 0;
    failures += test_pread_basic_read();
    failures += test_pread_matches_fread();
    failures += test_concurrent_reads();
    failures += test_concurrent_pread_same_file();
    failures += test_disk_block_pread_raw();
    failures += test_pread_accepts_frame_offset();
    failures += test_set_have_data_verified();
    failures += test_write_allocates_append_position();
    return failures;
}
