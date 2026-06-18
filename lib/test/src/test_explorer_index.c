/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Full per-block explorer indexer fixture test.
 *
 * Builds one synthetic block with a transparent output, a transparent
 * input, and an OP_RETURN output, runs the single per-block indexer hook
 * (explorer_index_block) against an in-memory node.db, and asserts the
 * corresponding rows land in tx_outputs / tx_inputs / op_returns /
 * view_integrity. Proves the indexer populates the projection tables and
 * is idempotent under a re-walk (INSERT OR REPLACE). node.db only. */

#include "test/test_helpers.h"
#include "models/database.h"
#include "models/explorer_index.h"
#include "primitives/transaction.h"
#include "primitives/block.h"
#include "chain/chain.h"

static int count_rows(struct node_db *ndb, const char *sql)
{
    sqlite3_stmt *s = NULL;
    int n = -1;
    if (sqlite3_prepare_v2(ndb->db, sql, -1, &s, NULL) == SQLITE_OK && s) {
        if (sqlite3_step(s) == SQLITE_ROW)  // raw-sql-ok:test-readonly-count
            n = sqlite3_column_int(s, 0);
    }
    if (s) sqlite3_finalize(s);
    return n;
}

int test_explorer_index(void)
{
    int failures = 0;
    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));

    printf("explorer_index: open in-memory node.db... ");
    if (node_db_open(&ndb, ":memory:") && ndb.open) {
        printf("OK\n");
    } else {
        printf("FAIL\n");
        return 1;   /* nothing else can run */
    }

    /* Build one tx: 1 real (non-coinbase) input, 2 outputs — a transparent
     * P2PKH output and an OP_RETURN output. */
    struct transaction tx;
    transaction_init(&tx);
    transaction_alloc(&tx, 1, 2);

    /* Input spends a known prevout. */
    uint256_set_null(&tx.vin[0].prevout.hash);
    tx.vin[0].prevout.hash.data[0] = 0xAB;
    tx.vin[0].prevout.n = 7;
    tx.vin[0].sequence = 0xFFFFFFFFu;
    tx.vin[0].script_sig.size = 0;

    /* vout[0]: P2PKH scriptPubKey (OP_DUP OP_HASH160 <20> OP_EQUALVERIFY
     * OP_CHECKSIG) so utxo_classify_script extracts an address. */
    tx.vout[0].value = 5 * COIN;
    {
        struct script *sp = &tx.vout[0].script_pub_key;
        sp->data[0] = 0x76;        /* OP_DUP */
        sp->data[1] = 0xa9;        /* OP_HASH160 */
        sp->data[2] = 0x14;        /* push 20 */
        for (int i = 0; i < 20; i++)
            sp->data[3 + i] = (unsigned char)(0x10 + i);
        sp->data[23] = 0x88;       /* OP_EQUALVERIFY */
        sp->data[24] = 0xac;       /* OP_CHECKSIG */
        sp->size = 25;
    }

    /* vout[1]: OP_RETURN with arbitrary payload. */
    tx.vout[1].value = 0;
    {
        struct script *sp = &tx.vout[1].script_pub_key;
        sp->data[0] = 0x6a;        /* OP_RETURN */
        sp->data[1] = 0x04;        /* push 4 */
        sp->data[2] = 'D'; sp->data[3] = 'A'; sp->data[4] = 'T'; sp->data[5] = 'A';
        sp->size = 6;
    }
    tx.lock_time = 0;
    transaction_compute_hash(&tx);

    /* Wrap in a one-tx block at height 1. */
    struct block blk;
    block_init(&blk);
    blk.vtx = &tx;
    blk.num_vtx = 1;
    blk.header.nTime = 1700000000;

    struct uint256 bhash;
    memset(bhash.data, 0x42, 32);
    struct block_index pindex;
    memset(&pindex, 0, sizeof(pindex));
    pindex.nHeight = 1;
    pindex.phashBlock = &bhash;

    uint8_t prev_receipt[32] = {0};
    uint8_t out_receipt[32];
    int64_t sprout_v = 0, sapling_v = 0;

    printf("explorer_index: index one synthetic block... ");
    bool indexed = explorer_index_block(&ndb, &blk, &pindex, prev_receipt,
                                        out_receipt, &sprout_v, &sapling_v);
    if (indexed) printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    printf("explorer_index: tx_outputs has 2 rows... ");
    { int n = count_rows(&ndb, "SELECT COUNT(*) FROM tx_outputs");
      if (n == 2) printf("OK\n"); else { printf("FAIL (got %d)\n", n); failures++; } }

    printf("explorer_index: P2PKH output recorded an address_hash... ");
    { int n = count_rows(&ndb,
        "SELECT COUNT(*) FROM tx_outputs WHERE address_hash IS NOT NULL");
      if (n == 1) printf("OK\n"); else { printf("FAIL (got %d)\n", n); failures++; } }

    printf("explorer_index: tx_inputs has 1 row (input recorded)... ");
    { int n = count_rows(&ndb, "SELECT COUNT(*) FROM tx_inputs");
      if (n == 1) printf("OK\n"); else { printf("FAIL (got %d)\n", n); failures++; } }

    printf("explorer_index: op_returns has 1 row... ");
    { int n = count_rows(&ndb, "SELECT COUNT(*) FROM op_returns");
      if (n == 1) printf("OK\n"); else { printf("FAIL (got %d)\n", n); failures++; } }

    printf("explorer_index: view_integrity has 1 row for the height... ");
    { int n = count_rows(&ndb, "SELECT COUNT(*) FROM view_integrity WHERE height=1");
      if (n == 1) printf("OK\n"); else { printf("FAIL (got %d)\n", n); failures++; } }

    /* Idempotency: re-index the same block; INSERT OR REPLACE must not
     * duplicate any row. */
    printf("explorer_index: re-index is idempotent (no duplicate rows)... ");
    bool reindexed = explorer_index_block(&ndb, &blk, &pindex, prev_receipt,
                                          out_receipt, &sprout_v, &sapling_v);
    int n_out = count_rows(&ndb, "SELECT COUNT(*) FROM tx_outputs");
    int n_in  = count_rows(&ndb, "SELECT COUNT(*) FROM tx_inputs");
    int n_or  = count_rows(&ndb, "SELECT COUNT(*) FROM op_returns");
    int n_vi  = count_rows(&ndb, "SELECT COUNT(*) FROM view_integrity");
    if (reindexed && n_out == 2 && n_in == 1 && n_or == 1 && n_vi == 1)
        printf("OK\n");
    else { printf("FAIL (out=%d in=%d or=%d vi=%d)\n", n_out, n_in, n_or, n_vi);
           failures++; }

    /* The integrity receipt must be deterministic + non-zero. */
    printf("explorer_index: integrity receipt is non-zero + deterministic... ");
    { uint8_t zero[32] = {0};
      uint8_t r2[32];
      explorer_index_block(&ndb, &blk, &pindex, prev_receipt, r2, NULL, NULL);
      if (memcmp(out_receipt, zero, 32) != 0 &&
          memcmp(out_receipt, r2, 32) == 0)
          printf("OK\n");
      else { printf("FAIL\n"); failures++; } }

    blk.vtx = NULL;   /* tx is stack-local; don't let block_free touch it */
    blk.num_vtx = 0;
    transaction_free(&tx);
    node_db_close(&ndb);

    return failures;
}
