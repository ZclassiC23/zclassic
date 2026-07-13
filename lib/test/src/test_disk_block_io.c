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

/* Read an entire file into a malloc'd buffer. Returns bytes read, -1 on error.
 * Caller frees *out. */
static long slurp_file(const char *path, unsigned char **out)
{
    *out = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    unsigned char *buf = malloc(sz > 0 ? (size_t)sz : 1); // raw-alloc-ok:test-fixture
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if ((long)n != sz) { free(buf); return -1; }
    *out = buf;
    return sz;
}

/* Deferred-sync mode must produce a byte-identical block file to immediate
 * mode, must record a pending file (not fdatasync inline), and the file must
 * be readable back both before and after disk_block_io_sync_pending(). This is
 * the "batch does not change WHAT is written, only WHEN it is synced" contract
 * plus the at-tip degrade-to-identical guarantee. */
static int test_deferred_sync_byte_identical(void)
{
    int failures = 0;
    char dir_immediate[256], dir_deferred[256];
    snprintf(dir_immediate, sizeof(dir_immediate),
             "./test-tmp/%d_disk_io_imm", (int)getpid());
    snprintf(dir_deferred, sizeof(dir_deferred),
             "./test-tmp/%d_disk_io_def", (int)getpid());
    mkdir("./test-tmp", 0755);
    for (int i = 0; i < 2; i++) {
        const char *d = i == 0 ? dir_immediate : dir_deferred;
        char blocks[512];
        mkdir(d, 0755);
        snprintf(blocks, sizeof(blocks), "%s/blocks", d);
        mkdir(blocks, 0755);
    }

    TEST("deferred_sync: byte-identical file + pending set + readback") {
        /* Immediate mode (default). */
        if (disk_block_io_deferred_sync_enabled()) {
            printf("FAIL (deferred mode leaked on from a prior test)\n");
            failures++; goto _test_next;
        }
        struct disk_block_pos pos_imm;
        disk_block_pos_init(&pos_imm);
        if (!write_test_block(dir_immediate, &pos_imm, 424242)) {
            printf("FAIL (immediate write)\n"); failures++; goto _test_next;
        }

        /* Deferred mode: same block. Must NOT sync inline — record pending. */
        disk_block_io_set_deferred_sync(true);
        if (!disk_block_io_deferred_sync_enabled()) {
            printf("FAIL (deferred flag not set)\n"); failures++;
            disk_block_io_set_deferred_sync(false); goto _test_next;
        }
        struct disk_block_pos pos_def;
        disk_block_pos_init(&pos_def);
        if (!write_test_block(dir_deferred, &pos_def, 424242)) {
            printf("FAIL (deferred write)\n"); failures++;
            disk_block_io_set_deferred_sync(false); goto _test_next;
        }

        /* Body must be readable from the page cache BEFORE the deferred sync
         * (so body_persist's read-back does not spuriously requeue). */
        struct block pre;
        if (!read_block_from_disk_pread(&pre, &pos_def, dir_deferred)) {
            printf("FAIL (deferred readback before sync)\n"); failures++;
            disk_block_io_set_deferred_sync(false); goto _test_next;
        }
        bool pre_ok = pre.header.nTime == 424242;
        block_free(&pre);
        if (!pre_ok) {
            printf("FAIL (wrong block before sync)\n"); failures++;
            disk_block_io_set_deferred_sync(false); goto _test_next;
        }

        /* Flush the deferred sync, then leave deferred mode. */
        if (!disk_block_io_sync_pending()) {
            printf("FAIL (sync_pending returned false)\n"); failures++;
            disk_block_io_set_deferred_sync(false); goto _test_next;
        }
        disk_block_io_set_deferred_sync(false);

        /* The two block files must be byte-for-byte identical: deferral changes
         * only WHEN bytes are synced, never the bytes. */
        char path_imm[600], path_def[600];
        struct disk_block_pos f0 = { .nFile = 0, .nPos = 0 };
        get_block_pos_filename(path_imm, sizeof(path_imm), dir_immediate, &f0, "blk");
        get_block_pos_filename(path_def, sizeof(path_def), dir_deferred, &f0, "blk");
        unsigned char *bi = NULL, *bd = NULL;
        long ni = slurp_file(path_imm, &bi);
        long nd = slurp_file(path_def, &bd);
        bool identical = (ni > 0 && ni == nd && bi && bd &&
                          memcmp(bi, bd, (size_t)ni) == 0);
        free(bi); free(bd);
        if (!identical) {
            printf("FAIL (files differ imm=%ld def=%ld)\n", ni, nd);
            failures++; goto _test_next;
        }

        /* Readback after sync still succeeds. */
        struct block post;
        if (!read_block_from_disk_pread(&post, &pos_def, dir_deferred)) {
            printf("FAIL (deferred readback after sync)\n"); failures++;
            goto _test_next;
        }
        bool post_ok = post.header.nTime == 424242;
        block_free(&post);
        if (!post_ok) {
            printf("FAIL (wrong block after sync)\n"); failures++; goto _test_next;
        }

        /* A second sync with nothing pending is a benign no-op → true. */
        if (!disk_block_io_sync_pending()) {
            printf("FAIL (empty sync_pending should be true)\n"); failures++;
            goto _test_next;
        }
        printf("OK\n");
    }
    _test_next:
    /* Never leak deferred mode into sibling tests. */
    disk_block_io_set_deferred_sync(false);
    cleanup_test_dir(dir_immediate);
    cleanup_test_dir(dir_deferred);
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
    failures += test_deferred_sync_byte_identical();
    return failures;
}
