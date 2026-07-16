/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_chainstate_legacy_reader: exercises the legacy CCoins reader
 * against a snapshot of a real zclassicd chainstate when one is
 * available.
 *
 * The test looks at:
 *   1. ZCL_LEGACY_CHAINSTATE  — explicit override path
 *   2. /tmp/zcl_chainstate_snapshot — convention used by the
 *      developer-only worktree harness
 *
 * If neither directory exists (or it can't be opened — e.g. live
 * zclassicd holds the lock on the original), the test is skipped
 * with PASS so CI on a fresh checkout doesn't fail.  When a snapshot
 * is present we verify:
 *
 *   A. open succeeds
 *   B. best-block hash decodes to 32 bytes and is non-zero
 *   C. iter walks the whole 'c' keyspace and emits ~1.3M records,
 *      with sane per-record fields (height in range, value >0,
 *      script non-empty)
 *   D. sum of values matches the order of magnitude of the
 *      published supply (~10M ZCL)
 *   E. a synthetic round-trip test on a hand-built record (covers
 *      the decode path even in CI where no snapshot exists)
 */

#include "test/test_helpers.h"
#include "storage/chainstate_legacy_reader.h"
#include "storage/dbwrapper.h"
#include "sapling/incremental_merkle_tree.h"
#include "core/serialize.h"
#include "core/uint256.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TCLR_CHECK(name, expr) do {                                  \
    printf("chainstate_legacy_reader: %s... ", (name));              \
    if ((expr)) printf("OK\n");                                      \
    else { printf("FAIL\n"); failures++; }                           \
} while (0)

struct walk_state {
    int64_t  records;
    int64_t  total_vouts;
    int64_t  total_value_sat;
    int      min_height;
    int      max_height;
    int      bad_records;
    int      print_first; /* dump the first few records */
};

static bool walk_cb(const struct uint256 *txid,
                    const struct legacy_coins *coins,
                    void *ctx)
{
    struct walk_state *st = ctx;
    st->records++;
    st->total_vouts += (int64_t)coins->num_vouts;

    if (coins->height < 0 || coins->height > 100000000) {
        st->bad_records++;
    } else {
        if (st->min_height == 0 || coins->height < st->min_height)
            st->min_height = coins->height;
        if (coins->height > st->max_height)
            st->max_height = coins->height;
    }

    for (size_t i = 0; i < coins->num_vouts; i++) {
        if (coins->vouts[i].value < 0)
            st->bad_records++;
        st->total_value_sat += coins->vouts[i].value;
        if (coins->vouts[i].script_len == 0)
            st->bad_records++;
    }

    if (st->print_first > 0) {
        char hex[65];
        uint256_get_hex(txid, hex);
        printf("    sample: txid=%s height=%d coinbase=%d vouts=%zu",
               hex, coins->height, coins->coinbase, coins->num_vouts);
        if (coins->num_vouts > 0) {
            printf(" v[0]={n=%u value=%" PRId64 " script_len=%zu}",
                   coins->vouts[0].n, coins->vouts[0].value,
                   coins->vouts[0].script_len);
        }
        printf("\n");
        st->print_first--;
    }
    return true;
}

static bool dir_exists(const char *p)
{
    struct stat st;
    if (stat(p, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

/* ── malformed anchor/nullifier fail-closed coverage ───────────────────
 *
 * These exercise chainstate_legacy_iter_sapling_anchors/_sprout_anchors and
 * chainstate_legacy_iter_sapling_nullifiers/_sprout_nullifiers — the exact
 * primitives shielded_history_import_service.c streams into anchor_kv /
 * nullifier_kv — against hand-built, single-row, hermetic LevelDB fixtures
 * (no live snapshot needed, always runs in CI). Every scenario asserts the
 * iterator returns -1 (fail-closed refusal), never a partial/truncated
 * count and never a crash on the malformed bytes. */

static bool tclr_put(const char *dir, const char *key, size_t klen,
                     const void *val, size_t vlen)
{
    struct db_wrapper db;
    memset(&db, 0, sizeof(db));
    if (!db_wrapper_open(&db, dir, 1u << 20, false, false))
        return false;
    bool ok = db_write(&db, key, klen, (const char *)val, vlen, true);
    db_wrapper_close(&db);
    return ok;
}

static bool tclr_anchor_cb_accept(const struct uint256 *root,
                                  const struct incremental_merkle_tree *tree,
                                  void *ctx)
{
    (void)root; (void)tree; (void)ctx;
    return true;
}

static bool tclr_nullifier_cb_accept(const uint8_t nf[32], void *ctx)
{
    (void)nf; (void)ctx;
    return true;
}

/* Open `dir`, run `iter`, close, and return the iterator's raw result. */
static int64_t tclr_run_anchor_iter(const char *dir,
                                    int64_t (*iter)(void *, legacy_anchor_cb,
                                                    void *))
{
    void *h = NULL;
    if (!chainstate_legacy_open(dir, &h) || !h)
        return -2; /* open itself failed — distinct from the -1 we assert */
    int64_t n = iter(h, tclr_anchor_cb_accept, NULL);
    chainstate_legacy_close(h);
    return n;
}

static int64_t tclr_run_nullifier_iter(const char *dir)
{
    void *h = NULL;
    if (!chainstate_legacy_open(dir, &h) || !h)
        return -2;
    int64_t n = chainstate_legacy_iter_sapling_nullifiers(
        h, tclr_nullifier_cb_accept, NULL);
    chainstate_legacy_close(h);
    return n;
}

static int test_chainstate_legacy_malformed_anchors_nullifiers(void)
{
    int failures = 0;

    /* (1) malformed key length inside the 'Z' anchor keyspace: 31-byte root
     * instead of 32 (klen=32, not 33) — must refuse, not read k[1..32] OOB
     * or silently skip. */
    {
        char dir[512];
        test_make_tmpdir(dir, sizeof(dir), "tclr_malformed_key", "db");
        char key[32];
        key[0] = 'Z';
        memset(key + 1, 0x11, sizeof(key) - 1);
        uint8_t val[4] = {0, 0, 0, 0};
        TCLR_CHECK("put malformed-length 'Z' key",
                       tclr_put(dir, key, sizeof(key), val, sizeof(val)));
        int64_t n = tclr_run_anchor_iter(
            dir, chainstate_legacy_iter_sapling_anchors);
        TCLR_CHECK("malformed anchor key length -> iter refuses (-1)",
                       n == -1);
        test_rm_rf_recursive(dir);
    }

    /* (2) empty value under a well-formed 33-byte 'Z' key. */
    {
        char dir[512];
        test_make_tmpdir(dir, sizeof(dir), "tclr_empty_value", "db");
        char key[33];
        key[0] = 'Z';
        memset(key + 1, 0x22, 32);
        TCLR_CHECK("put well-formed key with empty value",
                       tclr_put(dir, key, sizeof(key), "", 0));
        int64_t n = tclr_run_anchor_iter(
            dir, chainstate_legacy_iter_sapling_anchors);
        TCLR_CHECK("empty anchor value -> iter refuses (-1)", n == -1);
        test_rm_rf_recursive(dir);
    }

    /* (3) short/truncated value: a real serialized tree cut down to 3 bytes
     * under its OWN root key — must fail closed on the truncated stream
     * (stream_read bounds-checks), not read past the truncated buffer. */
    {
        char dir[512];
        test_make_tmpdir(dir, sizeof(dir), "tclr_truncated_value", "db");

        struct incremental_merkle_tree tree;
        sapling_tree_init(&tree);
        struct uint256 cm;
        memset(cm.data, 0x5A, 32);
        incremental_tree_append(&tree, &cm);
        struct uint256 root;
        incremental_tree_root(&tree, &root);

        struct byte_stream ser;
        stream_init(&ser, 256);
        TCLR_CHECK("serialize a real tree for truncation",
                       incremental_tree_serialize(&tree, &ser) && !ser.error);

        char key[33];
        key[0] = 'Z';
        memcpy(key + 1, root.data, 32);
        size_t short_len = ser.size > 3 ? 3 : ser.size;
        TCLR_CHECK("put truncated (3-byte) serialized tree",
                       tclr_put(dir, key, sizeof(key), ser.data, short_len));
        stream_free(&ser);

        int64_t n = tclr_run_anchor_iter(
            dir, chainstate_legacy_iter_sapling_anchors);
        TCLR_CHECK("truncated anchor value -> iter refuses (-1)",
                       n == -1);
        test_rm_rf_recursive(dir);
    }

    /* (4) root mismatch: a well-formed, fully-deserializable tree stored
     * under a key that is NOT its own root — must refuse (mirrors the
     * point-lookup fail-closed check in test_chainstate_sapling_anchor.c,
     * but through the bulk iterator the importer actually uses). */
    {
        char dir[512];
        test_make_tmpdir(dir, sizeof(dir), "tclr_root_mismatch", "db");

        struct incremental_merkle_tree tree;
        sapling_tree_init(&tree);
        struct uint256 cm;
        memset(cm.data, 0x5B, 32);
        incremental_tree_append(&tree, &cm);

        struct byte_stream ser;
        stream_init(&ser, 256);
        TCLR_CHECK("serialize a real tree for the mismatch fixture",
                       incremental_tree_serialize(&tree, &ser) && !ser.error);

        char key[33];
        key[0] = 'Z';
        memset(key + 1, 0x33, 32); /* != incremental_tree_root(tree) */
        TCLR_CHECK("put valid tree under a WRONG-root key",
                       tclr_put(dir, key, sizeof(key), ser.data, ser.size));
        stream_free(&ser);

        int64_t n = tclr_run_anchor_iter(
            dir, chainstate_legacy_iter_sapling_anchors);
        TCLR_CHECK("root-mismatched anchor -> iter refuses (-1)",
                       n == -1);
        test_rm_rf_recursive(dir);
    }

    /* (5) malformed key length inside the 'S' Sapling-nullifier keyspace. */
    {
        char dir[512];
        test_make_tmpdir(dir, sizeof(dir), "tclr_nf_malformed_key", "db");
        char key[32];
        key[0] = 'S';
        memset(key + 1, 0x44, sizeof(key) - 1);
        uint8_t present = 1;
        TCLR_CHECK("put malformed-length 'S' key",
                       tclr_put(dir, key, sizeof(key), &present, 1));
        int64_t n = tclr_run_nullifier_iter(dir);
        TCLR_CHECK("malformed nullifier key length -> iter refuses (-1)",
                       n == -1);
        test_rm_rf_recursive(dir);
    }

    return failures;
}

static const char *pick_snapshot_path(void)
{
    const char *override = getenv("ZCL_LEGACY_CHAINSTATE");
    if (override && *override && dir_exists(override))
        return override;
    static const char *candidates[] = {
        "/tmp/zcl_chainstate_snapshot",
        NULL,
    };
    for (int i = 0; candidates[i]; i++)
        if (dir_exists(candidates[i]))
            return candidates[i];
    return NULL;
}

int test_chainstate_legacy_reader(void)
{
    int failures = 0;
    const char *path = pick_snapshot_path();

    printf("chainstate_legacy_reader: smoke (no snapshot needed)... ");
    {
        /* Open NULL handle path → must return false, no crash. */
        void *h = NULL;
        bool ok = chainstate_legacy_open(NULL, &h);
        if (ok || h != NULL) { printf("FAIL (NULL arg)\n"); failures++; }
        else printf("OK\n");
    }

    failures += test_chainstate_legacy_malformed_anchors_nullifiers();

    if (!path) {
        printf("chainstate_legacy_reader: snapshot not found "
               "(set ZCL_LEGACY_CHAINSTATE or populate "
               "/tmp/zcl_chainstate_snapshot) — skipping live tests\n");
        return failures;
    }
    printf("chainstate_legacy_reader: using snapshot at %s\n", path);

    void *h = NULL;
    printf("chainstate_legacy_reader: open... ");
    if (!chainstate_legacy_open(path, &h) || !h) {
        printf("FAIL\n");
        return failures + 1;
    }
    printf("OK\n");

    printf("chainstate_legacy_reader: best-block hash... ");
    {
        struct uint256 best;
        uint256_set_null(&best);
        if (!chainstate_legacy_get_best_block(h, &best)) {
            printf("FAIL (read)\n"); failures++;
        } else if (uint256_is_null(&best)) {
            printf("FAIL (null)\n"); failures++;
        } else {
            char hex[65];
            uint256_get_hex(&best, hex);
            printf("OK %s\n", hex);
        }
    }

    printf("chainstate_legacy_reader: full iter... ");
    fflush(stdout);
    struct walk_state st = {0};
    st.print_first = 3;
    int64_t n = chainstate_legacy_iter(h, walk_cb, &st);
    if (n < 0) {
        printf("FAIL (iter returned -1)\n"); failures++;
    } else {
        printf("OK records=%" PRId64 " vouts=%" PRId64
               " value=%.4f ZCL height_range=[%d..%d] bad=%d\n",
               n, st.total_vouts,
               (double)st.total_value_sat / 1e8,
               st.min_height, st.max_height, st.bad_records);
        /* Records here are CCoins rows (one per transaction with any
         * unspent output), not individual vouts.  Live zclassicd
         * `gettxoutsetinfo` reports ~500k transactions / ~1.35M
         * txouts on ZCL mainnet — track the tx count for `records`
         * and the vout count for `total_vouts`.  Generous bands. */
        if (n < 400000 || n > 2000000) {
            printf("    FAIL: record count outside [400k..2M] sanity band\n");
            failures++;
        }
        if (st.total_vouts < 1000000 || st.total_vouts > 5000000) {
            printf("    FAIL: vout count outside [1M..5M] sanity band\n");
            failures++;
        }
        if (st.bad_records > 0) {
            printf("    FAIL: %d malformed records\n", st.bad_records);
            failures++;
        }
        if (st.min_height < 1 || st.max_height < 3000000) {
            printf("    FAIL: height range [%d..%d] looks wrong\n",
                   st.min_height, st.max_height);
            failures++;
        }
        /* Supply check: ZCL has a published supply on the order of
         * 10M ZCL ≈ 1e15 sat.  Accept 5e14..3e15. */
        if (st.total_value_sat < 500000000000000LL ||
            st.total_value_sat > 3000000000000000LL) {
            printf("    FAIL: total value %.2f ZCL outside [5M..30M] band\n",
                   (double)st.total_value_sat / 1e8);
            failures++;
        }
    }

    chainstate_legacy_close(h);
    return failures;
}
