/* Unit tests for the forward creation index (P0 §2.1) — the prevout source
 * that lets script_validate resolve transparent spends without -txindex. */

#include "test/test_helpers.h"

#include "core/uint256.h"
#include "jobs/created_outputs_index.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "storage/progress_store.h"
#include "util/safe_alloc.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CO_CHECK(name, expr) do {                                  \
    if (expr) { printf("  created_outputs: %s... OK\n", (name)); }  \
    else { printf("  created_outputs: %s... FAIL\n", (name)); failures++; } \
} while (0)

/* Build a tx with `nout` outputs; output i carries a distinct value and a
 * scriptPubKey of length (3+i) filled with byte (0xA0+i). txid is set
 * deterministically from `tag` (we control it; the index keys on tx->hash). */
static void co_make_tx(struct transaction *tx, uint8_t tag, size_t nout)
{
    transaction_init(tx);
    tx->hash.data[0] = tag;
    tx->hash.data[1] = 0xC0;
    tx->hash.data[2] = (uint8_t)nout;
    tx->num_vout = nout;
    tx->vout = zcl_calloc(nout, sizeof(struct tx_out), "co_tx_vout");
    for (size_t i = 0; i < nout; i++) {
        tx->vout[i].value = (int64_t)(1000 + (int)tag * 10 + (int)i);
        unsigned char sb[16];
        size_t sl = 3 + i;
        if (sl > sizeof(sb)) sl = sizeof(sb);
        for (size_t k = 0; k < sl; k++)
            sb[k] = (unsigned char)(0xA0 + i);
        script_set(&tx->vout[i].script_pub_key, sb, sl);
    }
}

static bool co_get(sqlite3 *db, const struct uint256 *txid, uint32_t vout,
                   int64_t *value, unsigned char *sc, size_t cap, size_t *slen)
{
    return created_outputs_index_get(db, txid->data, vout, value, sc, cap, slen);
}

int test_created_outputs_index(void);
int test_created_outputs_index(void)
{
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "created_outputs", "main");

    CO_CHECK("progress_store opens", progress_store_open(dir));
    sqlite3 *db = progress_store_db();
    CO_CHECK("db handle", db != NULL);

    /* get before schema exists -> graceful false (no crash, no throw). */
    {
        int64_t v = 0; unsigned char sc[MAX_SCRIPT_SIZE]; size_t sl = 0;
        struct uint256 z; uint256_set_null(&z);
        CO_CHECK("get before schema -> false",
                 !co_get(db, &z, 0, &v, sc, sizeof(sc), &sl));
    }

    CO_CHECK("ensure_schema", created_outputs_index_ensure_schema(db));
    CO_CHECK("ensure_schema idempotent",
             created_outputs_index_ensure_schema(db));

    /* Block at height 100: tx0 (2 vouts), tx1 (1 vout). */
    struct block blk; block_init(&blk);
    blk.num_vtx = 2;
    blk.vtx = zcl_calloc(2, sizeof(struct transaction), "co_blk_vtx");
    co_make_tx(&blk.vtx[0], 0x11, 2);
    co_make_tx(&blk.vtx[1], 0x22, 1);

    CO_CHECK("put_block height=100",
             created_outputs_index_put_block(db, &blk, 100));

    {
        int64_t v = 0; unsigned char sc[MAX_SCRIPT_SIZE]; size_t sl = 0;
        int created_h = -1;
        CO_CHECK("bounded get includes creator height",
                 created_outputs_index_get_bounded(
                     db, blk.vtx[0].hash.data, 0, 90, 100, &v, sc,
                     sizeof(sc), &sl, &created_h) &&
                 created_h == 100 && v == blk.vtx[0].vout[0].value);
        CO_CHECK("bounded get rejects below window",
                 !created_outputs_index_get_bounded(
                     db, blk.vtx[0].hash.data, 0, 101, 110, &v, sc,
                     sizeof(sc), &sl, &created_h));
        CO_CHECK("bounded get rejects future window",
                 !created_outputs_index_get_bounded(
                     db, blk.vtx[0].hash.data, 0, 1, 99, &v, sc,
                     sizeof(sc), &sl, &created_h));
    }

    /* Round-trip every output: value + scriptPubKey exact. */
    for (size_t ti = 0; ti < blk.num_vtx; ti++) {
        struct transaction *tx = &blk.vtx[ti];
        for (uint32_t vo = 0; vo < (uint32_t)tx->num_vout; vo++) {
            int64_t v = 0; unsigned char sc[MAX_SCRIPT_SIZE]; size_t sl = 0;
            bool got = co_get(db, &tx->hash, vo, &v, sc, sizeof(sc), &sl);
            bool match = got && v == tx->vout[vo].value
                && sl == tx->vout[vo].script_pub_key.size
                && memcmp(sc, tx->vout[vo].script_pub_key.data, sl) == 0;
            char nm[64];
            snprintf(nm, sizeof(nm), "round-trip tx%zu vout%u", ti, vo);
            CO_CHECK(nm, match);
        }
    }

    /* Absent txid and absent vout both miss. */
    {
        int64_t v = 0; unsigned char sc[MAX_SCRIPT_SIZE]; size_t sl = 0;
        struct uint256 absent; uint256_set_null(&absent); absent.data[0] = 0xFE;
        CO_CHECK("absent txid -> false",
                 !co_get(db, &absent, 0, &v, sc, sizeof(sc), &sl));
        CO_CHECK("absent vout -> false",
                 !co_get(db, &blk.vtx[0].hash, 99, &v, sc, sizeof(sc), &sl));
    }

    /* Idempotent re-put (rewind/re-persist simulation). */
    CO_CHECK("re-put idempotent",
             created_outputs_index_put_block(db, &blk, 100));
    {
        int64_t v = 0; unsigned char sc[MAX_SCRIPT_SIZE]; size_t sl = 0;
        CO_CHECK("resolves after re-put",
                 co_get(db, &blk.vtx[0].hash, 0, &v, sc, sizeof(sc), &sl)
                 && v == blk.vtx[0].vout[0].value);
    }

    /* Prune below finality: a low-height block is removed, the high one kept. */
    struct block low; block_init(&low);
    low.num_vtx = 1;
    low.vtx = zcl_calloc(1, sizeof(struct transaction), "co_low_vtx");
    co_make_tx(&low.vtx[0], 0x33, 1);
    CO_CHECK("put low height=50",
             created_outputs_index_put_block(db, &low, 50));
    CO_CHECK("prune_below(100)", created_outputs_index_prune_below(db, 100));
    {
        int64_t v = 0; unsigned char sc[MAX_SCRIPT_SIZE]; size_t sl = 0;
        CO_CHECK("prune removed height<100",
                 !co_get(db, &low.vtx[0].hash, 0, &v, sc, sizeof(sc), &sl));
        CO_CHECK("prune kept height>=100",
                 co_get(db, &blk.vtx[0].hash, 0, &v, sc, sizeof(sc), &sl));
    }

    for (size_t ti = 0; ti < blk.num_vtx; ti++)
        transaction_free(&blk.vtx[ti]);
    free(blk.vtx);
    transaction_free(&low.vtx[0]);
    free(low.vtx);
    progress_store_close();
    test_cleanup_tmpdir(dir);
    return failures;
}
