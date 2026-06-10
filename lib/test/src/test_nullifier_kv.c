/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit tests for nullifier_kv — the reducer's consensus shielded-nullifier
 * set as a `nullifiers` table IN progress.kv (C-3). The load-bearing
 * assertions are the (nf,pool) NAMESPACE SEPARATION (zclassicd keeps
 * distinct Sprout/Sapling maps, coins.cpp:166-180 — a single-column nf key
 * would reject legal cross-pool byte-reuse, an opposite-direction fork) and
 * the exact bounds of delete_range, the rewind primitive every cursor
 * rewind relies on (see the rewind invariant in storage/nullifier_kv.h). */

#include "test/test_helpers.h"

#include "storage/nullifier_kv.h"
#include "storage/progress_store.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#define NK_CHECK(name, expr) do {                                        \
    if (expr) { printf("  nullifier_kv: %s... OK\n", (name)); }          \
    else { printf("  nullifier_kv: %s... FAIL\n", (name)); failures++; } \
} while (0)

static void nk_nf(uint8_t out[32], uint8_t tag)
{
    memset(out, 0, 32);
    out[0] = tag;
    out[1] = 0x4E;
    out[31] = 0x77;
}

static int64_t nk_count(sqlite3 *db)
{
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM nullifiers",
                           -1, &s, NULL) != SQLITE_OK)
        return -1;
    int64_t n = -1;
    if (sqlite3_step(s) == SQLITE_ROW)
        n = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return n;
}

int test_nullifier_kv(void);
int test_nullifier_kv(void)
{
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "nullifier_kv", "main");

    NK_CHECK("progress_store opens", progress_store_open(dir));
    sqlite3 *db = progress_store_db();
    NK_CHECK("db handle", db != NULL);

    /* Existence probe + ensure idempotence. */
    NK_CHECK("table absent before ensure", !nullifier_kv_table_exists(db));
    NK_CHECK("ensure_schema", nullifier_kv_ensure_schema(db));
    NK_CHECK("table present after ensure", nullifier_kv_table_exists(db));
    NK_CHECK("ensure_schema idempotent", nullifier_kv_ensure_schema(db));
    NK_CHECK("count empty == 0", nk_count(db) == 0);

    /* add/get round-trip: presence + revealing height. */
    uint8_t n1[32];
    nk_nf(n1, 0x11);
    NK_CHECK("add n1 sprout h=100",
             nullifier_kv_add(db, n1, NULLIFIER_POOL_SPROUT, 100));
    {
        bool found = false;
        int64_t h = -1;
        NK_CHECK("get n1 sprout ok",
                 nullifier_kv_get(db, n1, NULLIFIER_POOL_SPROUT, &found, &h));
        NK_CHECK("get n1 sprout found at h=100", found && h == 100);
    }

    /* Miss: a clean found=false, NOT a store error. */
    {
        uint8_t miss[32];
        nk_nf(miss, 0xEE);
        bool found = true;
        NK_CHECK("get miss ok (no error)",
                 nullifier_kv_get(db, miss, NULLIFIER_POOL_SPROUT,
                                  &found, NULL));
        NK_CHECK("get miss found=false", !found);
    }

    /* (nf,pool) NAMESPACE SEPARATION: the SAME 32 bytes live independently
     * in each pool — one row per pool, neither read sees the other. */
    NK_CHECK("add n1 sapling h=200",
             nullifier_kv_add(db, n1, NULLIFIER_POOL_SAPLING, 200));
    NK_CHECK("count == 2 (one row per pool)", nk_count(db) == 2);
    {
        bool fs = false, fz = false;
        int64_t hs = -1, hz = -1;
        NK_CHECK("get n1 sprout still ok",
                 nullifier_kv_get(db, n1, NULLIFIER_POOL_SPROUT, &fs, &hs));
        NK_CHECK("get n1 sapling ok",
                 nullifier_kv_get(db, n1, NULLIFIER_POOL_SAPLING, &fz, &hz));
        NK_CHECK("pools keep separate heights",
                 fs && hs == 100 && fz && hz == 200);
    }

    /* INSERT OR REPLACE idempotence: re-adding the same (nf,pool) keeps ONE
     * row and takes the latest height. */
    NK_CHECK("re-add n1 sprout h=150",
             nullifier_kv_add(db, n1, NULLIFIER_POOL_SPROUT, 150));
    NK_CHECK("count still 2 after re-add", nk_count(db) == 2);
    {
        bool found = false;
        int64_t h = -1;
        NK_CHECK("re-add read ok",
                 nullifier_kv_get(db, n1, NULLIFIER_POOL_SPROUT, &found, &h));
        NK_CHECK("re-add replaced height", found && h == 150);
    }

    /* delete_range EXACT BOUNDS: rows at 5/6/7; deleting [6,6] removes only
     * the middle; deleting [5,7] removes the rest. Both bounds INCLUSIVE. */
    {
        uint8_t a[32], b[32], c[32];
        nk_nf(a, 0xA5);
        nk_nf(b, 0xB6);
        nk_nf(c, 0xC7);
        NK_CHECK("seed h=5", nullifier_kv_add(db, a, NULLIFIER_POOL_SAPLING, 5));
        NK_CHECK("seed h=6", nullifier_kv_add(db, b, NULLIFIER_POOL_SAPLING, 6));
        NK_CHECK("seed h=7", nullifier_kv_add(db, c, NULLIFIER_POOL_SAPLING, 7));
        NK_CHECK("delete [6,6]", nullifier_kv_delete_range(db, 6, 6));
        bool fa = false, fb = true, fc = false;
        NK_CHECK("h=5 survives [6,6]",
                 nullifier_kv_get(db, a, NULLIFIER_POOL_SAPLING, &fa, NULL) &&
                 fa);
        NK_CHECK("h=6 deleted by [6,6]",
                 nullifier_kv_get(db, b, NULLIFIER_POOL_SAPLING, &fb, NULL) &&
                 !fb);
        NK_CHECK("h=7 survives [6,6]",
                 nullifier_kv_get(db, c, NULLIFIER_POOL_SAPLING, &fc, NULL) &&
                 fc);
        NK_CHECK("delete [5,7]", nullifier_kv_delete_range(db, 5, 7));
        fa = fc = true;
        NK_CHECK("h=5 deleted by [5,7]",
                 nullifier_kv_get(db, a, NULLIFIER_POOL_SAPLING, &fa, NULL) &&
                 !fa);
        NK_CHECK("h=7 deleted by [5,7]",
                 nullifier_kv_get(db, c, NULLIFIER_POOL_SAPLING, &fc, NULL) &&
                 !fc);
        /* The h=100/150/200 rows from earlier sections are out of range. */
        NK_CHECK("out-of-range rows untouched", nk_count(db) == 2);
    }

    progress_store_close();
    test_cleanup_tmpdir(dir);
    return failures;
}
