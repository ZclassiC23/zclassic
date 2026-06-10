/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_ldb_snapshot: exercises the snapshot-dir helper.
 *
 * Run conditions:
 *   - If $HOME/.zclassic/blocks/index/ exists, the test builds a
 *     snapshot, opens it with leveldb, iterates some keys, and
 *     verifies teardown. This tests the live "open while another
 *     process holds the source LOCK" path that motivated this
 *     helper.
 *   - Otherwise the test exercises only the NULL/error paths to
 *     keep CI green on hosts without a legacy zclassic datadir.
 */

#include "test/test_helpers.h"
#include "storage/ldb_snapshot.h"

#include <leveldb/c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool dir_exists(const char *p)
{
    struct stat st;
    if (stat(p, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

int test_ldb_snapshot(void)
{
    int failures = 0;

    /* NULL-arg path. */
    printf("ldb_snapshot: NULL args... ");
    {
        char err[64] = {0};
        bool ok = ldb_snapshot_make(NULL, NULL, err, sizeof(err));
        if (ok) { printf("FAIL (returned true)\n"); failures++; }
        else printf("OK (err=%s)\n", err);
    }

    /* Missing-src path. */
    printf("ldb_snapshot: missing src... ");
    {
        char err[128] = {0};
        bool ok = ldb_snapshot_make(
            "/no/such/path/zcl_test_missing_src",
            "/tmp/zcl_test_snap_missing", err, sizeof(err));
        if (ok) { printf("FAIL (returned true)\n"); failures++; }
        else printf("OK (err=%s)\n", err);
        /* Should not have created dst. */
        rmdir("/tmp/zcl_test_snap_missing");
    }

    /* Destroy of non-existent is safe. */
    printf("ldb_snapshot: destroy nonexistent... ");
    ldb_snapshot_destroy("/tmp/zcl_test_snap_nope");
    printf("OK\n");

    /* Live snapshot test: only runs if a real LDB is reachable. */
    const char *home = getenv("HOME");
    char src[1024];
    snprintf(src, sizeof(src), "%s/.zclassic/blocks/index",
             home ? home : "/nonexistent");
    if (!dir_exists(src)) {
        printf("ldb_snapshot: live test skipped "
               "(no %s)\n", src);
        return failures;
    }
    printf("ldb_snapshot: using live src=%s\n", src);

    const char *dst = "/tmp/zcl_test_ldb_snap";
    /* Clean any prior run. */
    ldb_snapshot_destroy(dst);

    char err[256] = {0};
    bool ok = false;
    for (int try = 0; try < 3 && !ok; try++) {
        ok = ldb_snapshot_make(src, dst, err, sizeof(err));
        if (!ok && strcmp(err, "manifest_changed") != 0) break;
    }
    printf("ldb_snapshot: make... ");
    if (!ok) { printf("FAIL (%s)\n", err); return failures + 1; }
    printf("OK\n");

    printf("ldb_snapshot: leveldb_open against snapshot... ");
    leveldb_options_t *opts = leveldb_options_create();
    leveldb_options_set_create_if_missing(opts, 0);
    leveldb_options_set_max_open_files(opts, 64);
    char *lerr = NULL;
    leveldb_t *db = leveldb_open(opts, dst, &lerr);
    if (lerr) {
        printf("FAIL (%s)\n", lerr);
        leveldb_free(lerr);
        leveldb_options_destroy(opts);
        ldb_snapshot_destroy(dst);
        return failures + 1;
    }
    printf("OK\n");

    printf("ldb_snapshot: iterate (cap 50k)... ");
    leveldb_readoptions_t *ro = leveldb_readoptions_create();
    leveldb_iterator_t *it = leveldb_create_iterator(db, ro);
    leveldb_iter_seek_to_first(it);
    int n = 0, b_keys = 0;
    while (leveldb_iter_valid(it) && n < 50000) {
        size_t klen = 0;
        const char *k = leveldb_iter_key(it, &klen);
        if (klen >= 1 && k[0] == 'b') b_keys++;
        leveldb_iter_next(it);
        n++;
    }
    leveldb_iter_destroy(it);
    leveldb_readoptions_destroy(ro);
    leveldb_close(db);
    leveldb_options_destroy(opts);
    printf("OK n=%d b_keys=%d\n", n, b_keys);
    if (n < 100) { printf("FAIL: too few records\n"); failures++; }
    if (b_keys < 50) { printf("FAIL: too few 'b'-keys\n"); failures++; }

    /* Teardown. */
    ldb_snapshot_destroy(dst);
    struct stat st;
    if (stat(dst, &st) == 0) {
        printf("ldb_snapshot: teardown FAILED (dir still exists)\n");
        failures++;
    } else {
        printf("ldb_snapshot: teardown OK\n");
    }

    return failures;
}
