/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the ZSLP per-(token, outpoint) ledger (zslp_ledger) — the
 * debit-correct, chain-derived token-balance projection.
 *
 *  1. GENESIS creates an unspent outpoint row (amount + address) and its
 *     holder balance.
 *  2. A SEND moves amounts to new outpoints AND marks the consumed input
 *     spent; the balance query sums UNSPENT rows only (so a spent GENESIS
 *     output no longer counts).
 *  3. The backfill cursor advances to H*, and is idempotent once caught up.
 *  4. zslp_ledger_apply_height's running digest changes when the rows at a
 *     height change (and folds every height, so an empty height still
 *     advances the chain); truncate + a fresh re-derive reproduces the exact
 *     row count and digest ("rebuildable + integrity").
 *  5. The live per-block hook (zslp_ledger_apply_slp_live) creates rows and
 *     marks spends directly from a tx + parsed SLP message.
 *
 * The projection derives purely from already-persisted node.db tables
 * (zslp_transfers / tx_outputs / tx_inputs), so these tests seed those
 * tables directly — no on-disk block bodies required. */

#include "test/test_helpers.h"
#include "models/database.h"
#include "models/zslp_ledger.h"
#include "services/zslp_ledger_backfill_service.h"
#include "jobs/reducer_frontier.h"
#include "primitives/transaction.h"
#include "zslp/slp.h"
#include "core/amount.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

/* ── Fixture seed data ────────────────────────────────────────────── */

static uint8_t g_token[32];
static uint8_t g_txid_genesis[32];
static uint8_t g_txid_send[32];
static uint8_t g_addr_a[20];
static uint8_t g_addr_b[20];

static void fill(uint8_t *p, size_t n, uint8_t v)
{
    memset(p, v, n);
}

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

static void ins_transfer(struct node_db *ndb, const uint8_t txid[32],
                         int height, const uint8_t token_id[32], int tx_type,
                         int64_t amount, int vout, const uint8_t to_addr20[20])
{
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "INSERT OR REPLACE INTO zslp_transfers"
        "(txid,block_height,token_id,tx_type,amount,vout,to_addr)"
        " VALUES(?,?,?,?,?,?,?)", -1, &s, NULL);
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, height);
    sqlite3_bind_blob(s, 3, token_id, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 4, tx_type);
    sqlite3_bind_int64(s, 5, amount);
    sqlite3_bind_int(s, 6, vout);
    if (to_addr20) sqlite3_bind_blob(s, 7, to_addr20, 20, SQLITE_STATIC);
    else sqlite3_bind_null(s, 7);
    sqlite3_step(s);  // raw-sql-ok:test-fixture-insert
    sqlite3_finalize(s);
}

static void ins_output(struct node_db *ndb, const uint8_t txid[32], int vout,
                       int64_t value, const uint8_t addr20[20], int height)
{
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "INSERT OR REPLACE INTO tx_outputs"
        "(txid,vout,value,script_type,address_hash,block_height)"
        " VALUES(?,?,?,1,?,?)", -1, &s, NULL);
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, vout);
    sqlite3_bind_int64(s, 3, value);
    if (addr20) sqlite3_bind_blob(s, 4, addr20, 20, SQLITE_STATIC);
    else sqlite3_bind_null(s, 4);
    sqlite3_bind_int(s, 5, height);
    sqlite3_step(s);  // raw-sql-ok:test-fixture-insert
    sqlite3_finalize(s);
}

static void ins_input(struct node_db *ndb, const uint8_t txid[32],
                      int vin_index, const uint8_t prev_txid[32],
                      int prev_vout, int height)
{
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "INSERT OR REPLACE INTO tx_inputs"
        "(txid,vin_index,prev_txid,prev_vout,block_height)"
        " VALUES(?,?,?,?,?)", -1, &s, NULL);
    sqlite3_bind_blob(s, 1, txid, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, vin_index);
    sqlite3_bind_blob(s, 3, prev_txid, 32, SQLITE_STATIC);
    sqlite3_bind_int(s, 4, prev_vout);
    sqlite3_bind_int(s, 5, height);
    sqlite3_step(s);  // raw-sql-ok:test-fixture-insert
    sqlite3_finalize(s);
}

/* GENESIS at h=1: token T minted 1000 to addrA at (txid_genesis, vout 1).
 * SEND at h=2: spends (txid_genesis,1); 600 -> addrA (vout 1), 400 -> addrB
 * (vout 2), at txid_send. Seeds zslp_transfers + tx_outputs + tx_inputs. */
static void seed_fixture(struct node_db *ndb)
{
    fill(g_token, 32, 0xAA);
    fill(g_txid_genesis, 32, 0x11);
    fill(g_txid_send, 32, 0x22);
    fill(g_addr_a, 20, 0xA1);
    fill(g_addr_b, 20, 0xB2);

    /* GENESIS (h=1). token_id of a GENESIS is that tx's own txid; here we use
     * a fixed token id for both sides — the ledger just collates on it. */
    ins_transfer(ndb, g_txid_genesis, 1, g_token, SLP_TX_GENESIS, 1000, 1,
                 g_addr_a);
    ins_output(ndb, g_txid_genesis, 1, 1 * COIN, g_addr_a, 1);

    /* SEND (h=2). */
    ins_transfer(ndb, g_txid_send, 2, g_token, SLP_TX_SEND, 600, 1, g_addr_a);
    ins_transfer(ndb, g_txid_send, 2, g_token, SLP_TX_SEND, 400, 2, g_addr_b);
    ins_output(ndb, g_txid_send, 1, 1 * COIN, g_addr_a, 2);
    ins_output(ndb, g_txid_send, 2, 1 * COIN, g_addr_b, 2);
    ins_input(ndb, g_txid_send, 0, g_txid_genesis, 1, 2);
}

/* ── (1)+(2)+(3) backfill: genesis, send, spend, balance, cursor ──── */

static int test_backfill_ledger(void)
{
    int failures = 0;
    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));

    printf("zslp_ledger: open in-memory node.db (schema v%d)... ",
           NODE_DB_MAX_SCHEMA);
    if (node_db_open(&ndb, ":memory:") && ndb.open) printf("OK\n");
    else { printf("FAIL\n"); return 1; }

    printf("zslp_ledger: schema_version is %d... ", NODE_DB_MAX_SCHEMA);
    { int v = node_db_schema_version(&ndb);
      if (v == NODE_DB_MAX_SCHEMA) printf("OK\n");
      else { printf("FAIL (got %d)\n", v); failures++; } }

    seed_fixture(&ndb);

    g_zslp_ledger_backfill_test_ndb = &ndb;
    zslp_ledger_backfill_reset_for_test();

    /* Fold only through h=1 first, so we can observe the GENESIS row BEFORE
     * the SEND spends it. */
    reducer_frontier_provable_tip_set(1);

    printf("zslp_ledger: backfill folds heights up to H*=1... ");
    { int folded = zslp_ledger_backfill_run_once();
      if (folded == 2) printf("OK\n"); /* h=0 (empty) + h=1 */
      else { printf("FAIL (folded=%d)\n", folded); failures++; } }

    printf("zslp_ledger: GENESIS created 1 unspent outpoint row... ");
    { int n = count_rows(&ndb, "SELECT COUNT(*) FROM zslp_ledger");
      int u = (int)zslp_ledger_unspent_count(&ndb);
      if (n == 1 && u == 1) printf("OK\n");
      else { printf("FAIL (rows=%d unspent=%d)\n", n, u); failures++; } }

    printf("zslp_ledger: holder balance(T,addrA) == 1000 (GENESIS)... ");
    { int64_t b = zslp_ledger_balance(&ndb, g_token, g_addr_a);
      if (b == 1000) printf("OK\n"); else { printf("FAIL (%lld)\n", (long long)b); failures++; } }

    /* Now advance to H*=2: the SEND spends GENESIS and mints two outputs. */
    reducer_frontier_provable_tip_set(2);

    printf("zslp_ledger: backfill folds h=2 (SEND)... ");
    { int folded = zslp_ledger_backfill_run_once();
      if (folded == 1) printf("OK\n"); else { printf("FAIL (folded=%d)\n", folded); failures++; } }

    printf("zslp_ledger: cursor advanced to H*=2... ");
    { int32_t c = -1; uint8_t d[32];
      zslp_ledger_get_cursor(&ndb, &c, d);
      if (c == 2) printf("OK\n"); else { printf("FAIL (cursor=%d)\n", c); failures++; } }

    printf("zslp_ledger: SEND created 2 rows, GENESIS marked spent "
           "(3 total, 2 unspent)... ");
    { int n = count_rows(&ndb, "SELECT COUNT(*) FROM zslp_ledger");
      int u = (int)zslp_ledger_unspent_count(&ndb);
      int sp = count_rows(&ndb,
          "SELECT COUNT(*) FROM zslp_ledger WHERE spent_by_txid IS NOT NULL");
      if (n == 3 && u == 2 && sp == 1) printf("OK\n");
      else { printf("FAIL (rows=%d unspent=%d spent=%d)\n", n, u, sp); failures++; } }

    printf("zslp_ledger: balance sums UNSPENT only — addrA==600 "
           "(1000 GENESIS spent + 600 SEND), addrB==400... ");
    { int64_t ba = zslp_ledger_balance(&ndb, g_token, g_addr_a);
      int64_t bb = zslp_ledger_balance(&ndb, g_token, g_addr_b);
      if (ba == 600 && bb == 400) printf("OK\n");
      else { printf("FAIL (addrA=%lld addrB=%lld)\n", (long long)ba, (long long)bb); failures++; } }

    printf("zslp_ledger: GENESIS outpoint spent_by == txid_send... ");
    { sqlite3_stmt *s = NULL; int ok = 0;
      sqlite3_prepare_v2(ndb.db,
        "SELECT spent_by_txid FROM zslp_ledger WHERE txid=? AND vout=1",
        -1, &s, NULL);
      sqlite3_bind_blob(s, 1, g_txid_genesis, 32, SQLITE_STATIC);
      if (sqlite3_step(s) == SQLITE_ROW &&  // raw-sql-ok:test-readonly-check
          sqlite3_column_bytes(s, 0) == 32 &&
          memcmp(sqlite3_column_blob(s, 0), g_txid_send, 32) == 0)
          ok = 1;
      sqlite3_finalize(s);
      if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; } }

    printf("zslp_ledger: idempotent once caught up (second run folds 0)... ");
    { int folded = zslp_ledger_backfill_run_once();
      if (folded == 0) printf("OK\n"); else { printf("FAIL (folded=%d)\n", folded); failures++; } }

    g_zslp_ledger_backfill_test_ndb = NULL;
    reducer_frontier_provable_tip_reset();
    node_db_close(&ndb);
    return failures;
}

/* ── (4) digest sensitivity + rebuild == original ─────────────────── */

static int test_digest_and_rebuild(void)
{
    int failures = 0;
    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    if (!node_db_open(&ndb, ":memory:") || !ndb.open) { printf("open FAIL\n"); return 1; }
    seed_fixture(&ndb);

    uint8_t zero[32] = {0};

    printf("zslp_ledger: an empty height still advances the digest... ");
    { uint8_t d0[32];
      zslp_ledger_apply_height(&ndb, 0, zero, d0);
      if (memcmp(d0, zero, 32) != 0) printf("OK\n");
      else { printf("FAIL\n"); failures++; } }

    printf("zslp_ledger: digest changes when a height's rows change "
           "(h=1 GENESIS vs empty h=0)... ");
    { uint8_t d_empty[32], d_genesis[32];
      zslp_ledger_apply_height(&ndb, 0, zero, d_empty);   /* no rows at h=0 */
      zslp_ledger_apply_height(&ndb, 1, zero, d_genesis); /* GENESIS at h=1 */
      if (memcmp(d_empty, d_genesis, 32) != 0) printf("OK\n");
      else { printf("FAIL\n"); failures++; } }

    /* Chain the full run to capture the reference digest, then rebuild. */
    g_zslp_ledger_backfill_test_ndb = &ndb;
    zslp_ledger_backfill_reset_for_test();
    reducer_frontier_provable_tip_set(2);

    printf("zslp_ledger: full fold reaches cursor=2... ");
    { (void)zslp_ledger_backfill_run_once();
      int32_t c = -1; uint8_t d[32];
      zslp_ledger_get_cursor(&ndb, &c, d);
      if (c == 2) printf("OK\n"); else { printf("FAIL (%d)\n", c); failures++; } }

    uint8_t digest_before[32]; int32_t cursor_before = -1;
    zslp_ledger_get_cursor(&ndb, &cursor_before, digest_before);
    int rows_before = count_rows(&ndb, "SELECT COUNT(*) FROM zslp_ledger");

    printf("zslp_ledger: truncate resets rows + cursor + digest... ");
    { bool ok = zslp_ledger_truncate(&ndb);
      int32_t c = -2; uint8_t d[32];
      zslp_ledger_get_cursor(&ndb, &c, d);
      int n = count_rows(&ndb, "SELECT COUNT(*) FROM zslp_ledger");
      ok = ok && c == -1 && n == 0 && memcmp(d, zero, 32) == 0;
      if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; } }

    printf("zslp_ledger: fresh re-derive reproduces exact rows + digest... ");
    { (void)zslp_ledger_backfill_run_once();
      int32_t c = -1; uint8_t d[32];
      zslp_ledger_get_cursor(&ndb, &c, d);
      int n = count_rows(&ndb, "SELECT COUNT(*) FROM zslp_ledger");
      bool ok = c == cursor_before && n == rows_before &&
                memcmp(d, digest_before, 32) == 0;
      if (ok) printf("OK\n");
      else { printf("FAIL (rows=%d/%d cursor=%d/%d)\n", n, rows_before, c, cursor_before); failures++; } }

    g_zslp_ledger_backfill_test_ndb = NULL;
    reducer_frontier_provable_tip_reset();
    node_db_close(&ndb);
    return failures;
}

/* ── (5) live per-block hook ──────────────────────────────────────── */

static int test_live_hook(void)
{
    int failures = 0;
    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    if (!node_db_open(&ndb, ":memory:") || !ndb.open) { printf("open FAIL\n"); return 1; }

    uint8_t addr[20]; fill(addr, 20, 0xC3);

    /* A GENESIS tx: vout[0] OP_RETURN (ignored), vout[1] P2PKH holding the
     * token. */
    struct transaction tx;
    transaction_init(&tx);
    transaction_alloc(&tx, 1, 2);
    uint256_set_null(&tx.vin[0].prevout.hash);
    tx.vin[0].prevout.n = 0xFFFFFFFFu;
    tx.vin[0].sequence = 0xFFFFFFFFu;
    tx.vout[0].script_pub_key.data[0] = 0x6a; /* OP_RETURN */
    tx.vout[0].script_pub_key.size = 1;
    tx.vout[0].value = 0;
    {
        struct script *sp = &tx.vout[1].script_pub_key;
        sp->data[0] = 0x76; sp->data[1] = 0xa9; sp->data[2] = 0x14;
        memcpy(sp->data + 3, addr, 20);
        sp->data[23] = 0x88; sp->data[24] = 0xac;
        sp->size = 25;
    }
    tx.vout[1].value = 1 * COIN;
    transaction_compute_hash(&tx);

    struct slp_message m;
    memset(&m, 0, sizeof(m));
    m.type = SLP_TX_GENESIS;
    m.initial_quantity = 777;

    printf("zslp_ledger live: GENESIS hook creates the vout[1] row... ");
    { zslp_ledger_apply_slp_live(&ndb, &tx, &m, 10);
      int n = count_rows(&ndb, "SELECT COUNT(*) FROM zslp_ledger");
      int64_t b = zslp_ledger_balance(&ndb, tx.hash.data, addr);
      if (n == 1 && b == 777) printf("OK\n");
      else { printf("FAIL (rows=%d bal=%lld)\n", n, (long long)b); failures++; } }

    printf("zslp_ledger live: idempotent re-apply (still 1 row)... ");
    { zslp_ledger_apply_slp_live(&ndb, &tx, &m, 10);
      int n = count_rows(&ndb, "SELECT COUNT(*) FROM zslp_ledger");
      if (n == 1) printf("OK\n"); else { printf("FAIL (%d)\n", n); failures++; } }

    /* A spending tx consumes (tx, vout 1); the live hook marks it spent. */
    struct transaction spend;
    transaction_init(&spend);
    transaction_alloc(&spend, 1, 1);
    memcpy(spend.vin[0].prevout.hash.data, tx.hash.data, 32);
    spend.vin[0].prevout.n = 1;
    spend.vin[0].sequence = 0xFFFFFFFFu;
    spend.vout[0].script_pub_key.data[0] = 0x6a;
    spend.vout[0].script_pub_key.size = 1;
    spend.vout[0].value = 0;
    transaction_compute_hash(&spend);

    struct slp_message ms;
    memset(&ms, 0, sizeof(ms));
    ms.type = SLP_TX_INVALID; /* a plain spend carrying no SLP output side */

    printf("zslp_ledger live: spending input marks the outpoint spent... ");
    { zslp_ledger_apply_slp_live(&ndb, &spend, &ms, 11);
      int u = (int)zslp_ledger_unspent_count(&ndb);
      int64_t b = zslp_ledger_balance(&ndb, tx.hash.data, addr);
      if (u == 0 && b == 0) printf("OK\n");
      else { printf("FAIL (unspent=%d bal=%lld)\n", u, (long long)b); failures++; } }

    transaction_free(&tx);
    transaction_free(&spend);
    node_db_close(&ndb);
    return failures;
}

/* ── Entry point ──────────────────────────────────────────────────── */

int test_zslp_ledger(void)
{
    int failures = 0;
    printf("\n=== ZSLP per-(token,outpoint) Ledger Tests ===\n");
    failures += test_backfill_ledger();
    failures += test_digest_and_rebuild();
    failures += test_live_hook();
    return failures;
}
