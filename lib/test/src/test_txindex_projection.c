/* Unit tests for the txindex transaction-location projection — the rebuildable
 * catalog that answers "where does transaction T live" (height, block hash,
 * index-in-block) without a full-chain scan, so a getrawtransaction-class
 * lookup can jump straight to the block. Mirrors test_address_index.c. */

#include "test/test_helpers.h"

#include "core/uint256.h"
#include "encoding/utilstrencodings.h"
#include "jobs/txindex_projection.h"
#include "json/json.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "services/txindex_projection_service.h"
#include "storage/progress_store.h"
#include "util/safe_alloc.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TI_CHECK(name, expr) do {                                     \
    if (expr) { printf("  txindex: %s... OK\n", (name)); }            \
    else { printf("  txindex: %s... FAIL\n", (name)); failures++; }   \
} while (0)

/* Set tx->hash to a deterministic 32-byte value from `tag`. */
static void ti_set_txid(struct transaction *tx, uint8_t tag)
{
    memset(tx->hash.data, 0, 32);
    tx->hash.data[0] = tag;
    tx->hash.data[31] = 0xAB;
}

int test_txindex_projection(void);
int test_txindex_projection(void)
{
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "txindex_projection", "main");

    TI_CHECK("progress_store opens", progress_store_open(dir));
    sqlite3 *db = progress_store_db();
    TI_CHECK("db handle", db != NULL);

    /* Opt-in default: with no -txindex arg the projection service is OFF and the
     * boot registration + a service tick are no-ops — a default boot pays zero
     * cost and writes no projection rows. */
    TI_CHECK("disabled by default", !txindex_projection_enabled());
    TI_CHECK("register no-op when disabled",
             !txindex_projection_service_register() &&
             !txindex_projection_service_registered());
    TI_CHECK("tick no-op when disabled",
             txindex_projection_service_tick_once() == 0);
    {
        uint8_t any[32]; memset(any, 0x11, 32);
        int64_t h = 0, n = 0, c = 0; uint8_t bh[32];
        TI_CHECK("read_locate DISABLED when off",
                 txindex_projection_read_locate(any, &h, bh, &n, &c) ==
                 TXINDEX_READ_DISABLED);
    }

    /* Cursor read before schema exists is a clean cursor=-1 (not an error). */
    {
        int64_t c = 999;
        TI_CHECK("cursor before schema -> -1",
                 txindex_projection_get_cursor(db, &c) && c == -1);
    }

    TI_CHECK("ensure_schema", txindex_projection_ensure_schema(db));
    TI_CHECK("ensure_schema idempotent", txindex_projection_ensure_schema(db));

    /* ── fixture chain ────────────────────────────────────────────── */
    /* block 100: coinbase(CB) + tx(P);  block 101: tx(S). Only the txid and
     * count matter to the projection — vin/vout are untouched. */
    struct block blk100; block_init(&blk100);
    blk100.num_vtx = 2;
    blk100.vtx = zcl_calloc(2, sizeof(struct transaction), "b100");
    transaction_init(&blk100.vtx[0]);
    transaction_init(&blk100.vtx[1]);
    ti_set_txid(&blk100.vtx[0], 0xC0); /* coinbase */
    ti_set_txid(&blk100.vtx[1], 0x50); /* P */

    struct block blk101; block_init(&blk101);
    blk101.num_vtx = 1;
    blk101.vtx = zcl_calloc(1, sizeof(struct transaction), "b101");
    transaction_init(&blk101.vtx[0]);
    ti_set_txid(&blk101.vtx[0], 0x51); /* S */

    uint8_t bh100[32]; memset(bh100, 0, 32); bh100[0] = 0xB1; bh100[31] = 0x64;
    uint8_t bh101[32]; memset(bh101, 0, 32); bh101[0] = 0xB1; bh101[31] = 0x65;

    /* Fold ascending, maintaining the running digest. */
    uint8_t digest[32]; memset(digest, 0, 32);
    int added100 = 0, added101 = 0;
    TI_CHECK("put block 100",
             txindex_projection_put_block(db, &blk100, 100, bh100, digest,
                                          &added100));
    TI_CHECK("block 100 added 2 rows", added100 == 2);
    uint8_t digest_after_100[32]; memcpy(digest_after_100, digest, 32);
    TI_CHECK("digest advanced after block 100",
             memcmp(digest_after_100, (uint8_t[32]){0}, 32) != 0);
    TI_CHECK("put block 101",
             txindex_projection_put_block(db, &blk101, 101, bh101, digest,
                                          &added101));
    TI_CHECK("block 101 added 1 row", added101 == 1);
    TI_CHECK("digest advanced after block 101",
             memcmp(digest, digest_after_100, 32) != 0);
    uint8_t digest_full[32]; memcpy(digest_full, digest, 32);

    /* ── lookups hit with exact location ─────────────────────────── */
    {
        int64_t h = -1, n = -1; uint8_t gotbh[32];
        TI_CHECK("lookup CB -> found",
                 txindex_projection_lookup(db, blk100.vtx[0].hash.data,
                                           &h, gotbh, &n) == 1);
        TI_CHECK("CB: height 100 tx_n 0", h == 100 && n == 0);
        TI_CHECK("CB: block_hash matches", memcmp(gotbh, bh100, 32) == 0);
    }
    {
        int64_t h = -1, n = -1; uint8_t gotbh[32];
        TI_CHECK("lookup P -> found",
                 txindex_projection_lookup(db, blk100.vtx[1].hash.data,
                                           &h, gotbh, &n) == 1);
        TI_CHECK("P: height 100 tx_n 1", h == 100 && n == 1);
    }
    {
        int64_t h = -1, n = -1; uint8_t gotbh[32];
        TI_CHECK("lookup S -> found",
                 txindex_projection_lookup(db, blk101.vtx[0].hash.data,
                                           &h, gotbh, &n) == 1);
        TI_CHECK("S: height 101 tx_n 0 block_hash bh101",
                 h == 101 && n == 0 && memcmp(gotbh, bh101, 32) == 0);
    }
    {
        uint8_t absent[32]; memset(absent, 0xEE, 32);
        int64_t h = 7, n = 7; uint8_t gotbh[32];
        TI_CHECK("lookup absent txid -> 0",
                 txindex_projection_lookup(db, absent, &h, gotbh, &n) == 0 &&
                 h == -1 && n == -1);
    }

    TI_CHECK("row_count == 3", txindex_projection_row_count(db) == 3);

    /* ── idempotent re-fold: INSERT OR IGNORE adds no rows ────────── */
    {
        uint8_t d2[32]; memcpy(d2, digest, 32);
        int added = -1;
        TI_CHECK("re-put block 100 idempotent",
                 txindex_projection_put_block(db, &blk100, 100, bh100, d2,
                                              &added));
        TI_CHECK("re-put added 0 rows (IGNORE)", added == 0);
        TI_CHECK("row_count still 3", txindex_projection_row_count(db) == 3);
    }

    /* ── cursor + digest persist round-trip ──────────────────────── */
    TI_CHECK("set_cursor",
             txindex_projection_set_cursor(db, 101, digest_full));
    {
        int64_t c = -1;
        TI_CHECK("get_cursor == 101",
                 txindex_projection_get_cursor(db, &c) && c == 101);
        uint8_t dg[32]; bool found = false;
        TI_CHECK("get_digest matches",
                 txindex_projection_get_digest(db, dg, &found) && found &&
                 memcmp(dg, digest_full, 32) == 0);
    }

    /* ── drop-and-rederive is deterministic (rebuildable) ────────── */
    TI_CHECK("drop", txindex_projection_drop(db));
    TI_CHECK("row_count 0 after drop", txindex_projection_row_count(db) == 0);
    {
        int64_t c = 999;
        TI_CHECK("cursor -1 after drop",
                 txindex_projection_get_cursor(db, &c) && c == -1);
    }
    TI_CHECK("re-ensure_schema", txindex_projection_ensure_schema(db));
    {
        uint8_t d[32]; memset(d, 0, 32);
        int a1 = 0, a2 = 0;
        TI_CHECK("refold block 100",
                 txindex_projection_put_block(db, &blk100, 100, bh100, d, &a1));
        TI_CHECK("refold block 101",
                 txindex_projection_put_block(db, &blk101, 101, bh101, d, &a2));
        TI_CHECK("rederived digest identical",
                 memcmp(d, digest_full, 32) == 0);
        TI_CHECK("rederived row_count == 3",
                 txindex_projection_row_count(db) == 3);
    }

    /* ── read classification (pure) — the "fail soft when behind" rule ── */
    TI_CHECK("classify found -> FOUND",
             txindex_projection_classify(true, 50, 100) == TXINDEX_READ_FOUND);
    TI_CHECK("classify miss below tip -> BEHIND (fail soft)",
             txindex_projection_classify(false, 50, 100) ==
             TXINDEX_READ_BEHIND);
    TI_CHECK("classify miss at tip -> ABSENT",
             txindex_projection_classify(false, 100, 100) ==
             TXINDEX_READ_ABSENT);
    TI_CHECK("classify miss above tip -> ABSENT",
             txindex_projection_classify(false, 101, 100) ==
             TXINDEX_READ_ABSENT);

    /* ── dumpstate `txindex` ─────────────────────────────────────── */
    {
        struct json_value out;

        /* status (no key): atomics only, side-effect-free. */
        json_init(&out);
        TI_CHECK("dump status returns true",
                 txindex_dump_state_json(&out, NULL));
        TI_CHECK("dump status has rows key", json_get(&out, "rows") != NULL);
        TI_CHECK("dump status enabled=false (no -txindex)",
                 !json_get_bool(json_get(&out, "enabled")));
        json_free(&out);

        /* keyed hit: CB is folded at height 100 (cursor 101 >= H*=0 in test). */
        char cbhex[65];
        HexStr(blk100.vtx[0].hash.data, 32, false, cbhex, sizeof(cbhex));
        txindex_projection_set_cursor(db, 101, digest_full);
        json_init(&out);
        TI_CHECK("dump keyed hit returns true",
                 txindex_dump_state_json(&out, cbhex));
        TI_CHECK("dump keyed: found true",
                 json_get_bool(json_get(&out, "found")));
        TI_CHECK("dump keyed: height 100",
                 json_get_int(json_get(&out, "height")) == 100);
        TI_CHECK("dump keyed: tx_n 0",
                 json_get_int(json_get(&out, "tx_n")) == 0);
        json_free(&out);

        /* keyed miss on a FRESH (unfolded) projection -> soft "behind". */
        TI_CHECK("drop for behind test", txindex_projection_drop(db));
        TI_CHECK("re-ensure for behind test",
                 txindex_projection_ensure_schema(db));
        uint8_t miss[32]; memset(miss, 0x33, 32);
        char misshex[65];
        HexStr(miss, 32, false, misshex, sizeof(misshex));
        json_init(&out);
        TI_CHECK("dump keyed behind returns true",
                 txindex_dump_state_json(&out, misshex));
        TI_CHECK("dump keyed behind: found false",
                 !json_get_bool(json_get(&out, "found")));
        {
            const char *err = json_get_str(json_get(&out, "error"));
            TI_CHECK("dump keyed behind: 'txindex behind: cursor=' message",
                     err && strncmp(err, "txindex behind: cursor=", 23) == 0);
        }
        json_free(&out);

        /* bad key -> error, no crash. */
        json_init(&out);
        TI_CHECK("dump bad key -> error, no crash",
                 txindex_dump_state_json(&out, "not-a-txid") &&
                 json_get(&out, "error") != NULL);
        json_free(&out);
    }

    /* cleanup */
    for (size_t i = 0; i < blk100.num_vtx; i++)
        transaction_free(&blk100.vtx[i]);
    free(blk100.vtx);
    transaction_free(&blk101.vtx[0]);
    free(blk101.vtx);
    progress_store_close();
    test_cleanup_tmpdir(dir);
    return failures;
}
