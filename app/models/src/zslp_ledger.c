/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ActiveRecord model for the ZSLP per-(token, outpoint) ledger
 * (zslp_ledger) — the debit-correct, chain-derived token-balance
 * projection. See models/zslp_ledger.h for the field semantics, the
 * SLP-validity divergence, and the threading contract.
 *
 * All writes go through the AR lifecycle: row creates via AR_ADHOC_SAVE
 * (locally-prepared INSERT OR IGNORE), spend marks via AR_EXEC_CHANGED_BOOL
 * (a deterministic single-outpoint UPDATE, the same shape wallet spend uses).
 * node.db ONLY; never consulted by consensus. */

#include "models/zslp_ledger.h"

#include "models/database.h"
#include "models/activerecord.h"
#include "models/explorer_index.h"
#include "primitives/transaction.h"
#include "script/standard.h"
#include "zslp/slp.h"
#include "crypto/sha3.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdint.h>
#include <string.h>

#define ZSLP_LEDGER_CURSOR_KEY "zslp_ledger_cursor_height"
#define ZSLP_LEDGER_DIGEST_KEY "zslp_ledger_digest"

DEFINE_MODEL_CALLBACKS(zslp_ledger)

/* SLP quantities are unsigned 64-bit; node.db INTEGER is signed 64-bit.
 * Clamp the handful of on-chain amounts above INT64_MAX to INT64_MAX so the
 * outpoint still appears (the column cannot represent the true magnitude
 * anyway) — identical to explorer_index_zslp.c's zslp_i64, kept local so
 * the two projections stay byte-for-byte consistent. */
static int64_t zslp_ledger_i64(uint64_t v)
{
    return v > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)v;
}

/* SLP carries token_id in wire (big-endian/display) order — the byte-reverse
 * of the node's internal txid order. GENESIS stores its token_id as that
 * tx's own (internal-order) txid; MINT/SEND reverse the wire token_id to the
 * SAME internal order. Identical to explorer_index_zslp.c so a token's
 * GENESIS row and its SEND/MINT rows collate under one byte order. */
static void zslp_ledger_token_id_internal(const uint8_t wire[32],
                                          uint8_t out[32])
{
    for (int i = 0; i < 32; i++)
        out[i] = wire[31 - i];
}

/* ── Row model ─────────────────────────────────────────────────────── */

struct zslp_ledger_row {
    uint8_t  token_id[32];
    uint8_t  txid[32];
    int32_t  vout;
    int64_t  amount;         /* clamped token base units, >= 0 */
    uint8_t  address[20];
    bool     has_address;
    int32_t  created_height;
};

static bool zslp_ledger_validate(const struct zslp_ledger_row *r,
                                 struct ar_errors *errors)
{
    ar_errors_clear(errors);
    if (!r) {
        ar_errors_add(errors, "row", "is NULL");
        return false;
    }
    validates_non_negative(errors, r, vout);
    validates_non_negative(errors, r, amount);
    validates_non_negative(errors, r, created_height);
    return !ar_errors_any(errors);
}

static bool zslp_ledger_save(struct node_db *ndb,
                             const struct zslp_ledger_row *row)
{
    if (!ndb || !ndb->open)
        LOG_FAIL("zslp_ledger", "zslp_ledger_save: db not open");
    if (!row)
        LOG_FAIL("zslp_ledger", "zslp_ledger_save: row is NULL");

    struct ar_callbacks *cbs = db_zslp_ledger_callbacks();
    sqlite3_stmt *s = NULL;
    AR_ADHOC_SAVE(ndb, s,
        "INSERT OR IGNORE INTO zslp_ledger"
        "(token_id,txid,vout,amount,address,created_height)"
        " VALUES(?,?,?,?,?,?)",
        cbs, "zslp_ledger", row, zslp_ledger_validate,
        AR_BIND_BLOB(s, 1, row->token_id, 32);
        AR_BIND_BLOB(s, 2, row->txid, 32);
        AR_BIND_INT(s, 3, row->vout);
        AR_BIND_INT(s, 4, row->amount);
        if (row->has_address)
            AR_BIND_BLOB(s, 5, row->address, 20);
        else
            AR_BIND_NULL(s, 5);
        AR_BIND_INT(s, 6, row->created_height));
}

/* Mark the ledger outpoint (txid,vout) spent by `spender` at `height`.
 * Deterministic: an outpoint is consumed by exactly one input on the
 * canonical chain, so this UPDATE is idempotent under re-derive. Returns
 * true iff a token-bearing outpoint actually matched (a non-token outpoint
 * has no row and changes()==0) — the caller uses that to decide whether the
 * spend participates in the running digest. */
static bool zslp_ledger_mark_spent(struct node_db *ndb,
                                   const uint8_t prev_txid[32],
                                   int32_t prev_vout,
                                   const uint8_t spender_txid[32],
                                   int32_t height)
{
    if (!ndb || !ndb->open) return false;
    sqlite3_stmt *s = NULL;
    AR_EXEC_CHANGED_BOOL(ndb, s,
        "UPDATE zslp_ledger SET spent_by_txid=?,spent_height=?"
        " WHERE txid=? AND vout=?",
        AR_BIND_BLOB(s, 1, spender_txid, 32);
        AR_BIND_INT(s, 2, height);
        AR_BIND_BLOB(s, 3, prev_txid, 32);
        AR_BIND_INT(s, 4, prev_vout));
}

/* ── Streaming digest helpers ──────────────────────────────────────── */

static void sha3_write_le64(struct sha3_256_ctx *ctx, uint64_t v)
{
    uint8_t le[8];
    for (int i = 0; i < 8; i++) le[i] = (uint8_t)(v >> (8 * i));
    sha3_256_write(ctx, le, 8);
}

static void sha3_write_le32(struct sha3_256_ctx *ctx, uint32_t v)
{
    uint8_t le[4];
    for (int i = 0; i < 4; i++) le[i] = (uint8_t)(v >> (8 * i));
    sha3_256_write(ctx, le, 4);
}

/* ── Live per-block hook ───────────────────────────────────────────── */

/* Insert one create row for (token_id, tx.vout[vout] = amount). Skips
 * 0-amount slots and vouts the tx lacks — same guard as
 * explorer_index_zslp.c's zslp_emit_transfer, so live and backfill agree on
 * exactly which outputs become ledger rows. */
static void zslp_ledger_live_emit(struct node_db *ndb,
                                  const struct transaction *tx, int height,
                                  const uint8_t token_id32[32],
                                  uint64_t amount, uint32_t vout)
{
    if (amount == 0 || vout >= tx->num_vout)
        return;
    const struct tx_out *o = &tx->vout[vout];
    uint8_t a20[20];
    bool has = false;
    utxo_classify_script(o->script_pub_key.data, o->script_pub_key.size,
                         a20, &has);

    struct zslp_ledger_row row;
    memset(&row, 0, sizeof(row));
    memcpy(row.token_id, token_id32, 32);
    memcpy(row.txid, tx->hash.data, 32);
    row.vout = (int32_t)vout;
    row.amount = zslp_ledger_i64(amount);
    row.has_address = has;
    if (has) memcpy(row.address, a20, 20);
    row.created_height = height;

    if (!zslp_ledger_save(ndb, &row))
        LOG_WARN("zslp_ledger", "live emit: save failed h=%d vout=%u", height,
                 vout);
}

bool zslp_ledger_apply_slp_live(struct node_db *ndb,
                                const struct transaction *tx,
                                const struct slp_message *m, int height)
{
    if (!ndb || !ndb->open || !tx || !m)
        return false;

    /* Spend side FIRST: mark every ledger outpoint this tx consumes. (A tx
     * can never spend its own outputs, so order vs. the create side below
     * is irrelevant, but marking first mirrors the on-chain "inputs before
     * outputs" reading.) Coinbase / null prevouts simply match no row. */
    for (size_t i = 0; i < tx->num_vin; i++) {
        const struct outpoint *op = &tx->vin[i].prevout;
        if (outpoint_is_null(op)) continue;
        (void)zslp_ledger_mark_spent(ndb, op->hash.data, (int32_t)op->n,
                                     tx->hash.data, height);
    }

    /* Create side: same vout/amount/token_id interpretation as
     * explorer_index_apply_slp (GENESIS/MINT -> vout 1, SEND -> vouts 1..N). */
    uint8_t tid[32];
    switch (m->type) {
    case SLP_TX_GENESIS:
        zslp_ledger_live_emit(ndb, tx, height, tx->hash.data,
                              m->initial_quantity, 1);
        break;
    case SLP_TX_MINT:
        zslp_ledger_token_id_internal(m->token_id.data, tid);
        zslp_ledger_live_emit(ndb, tx, height, tid, m->additional_quantity, 1);
        break;
    case SLP_TX_SEND:
        zslp_ledger_token_id_internal(m->token_id.data, tid);
        for (int i = 0; i < m->num_outputs; i++)
            zslp_ledger_live_emit(ndb, tx, height, tid,
                                  m->output_quantities[i], (uint32_t)(i + 1));
        break;
    default:
        break;
    }
    return true;
}

/* ── Backfill: one height from node.db projections + digest fold ───── */

bool zslp_ledger_apply_height(struct node_db *ndb, int32_t height,
                              const uint8_t prev_digest[32],
                              uint8_t out_digest[32])
{
    if (!ndb || !ndb->open || !prev_digest || !out_digest)
        LOG_FAIL("zslp_ledger", "apply_height: invalid args");

    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    sha3_256_write(&ctx, prev_digest, 32);
    sha3_write_le64(&ctx, (uint64_t)(int64_t)height);

    /* CREATE rows: every SLP transfer at this height whose declared output
     * actually EXISTS as a transparent output (JOIN tx_outputs). The join
     * both supplies the authoritative recipient address and filters phantom
     * vouts (a SEND may declare more outputs than the tx has; those extra
     * amounts have no spendable UTXO and must not enter the ledger). Ordered
     * (txid,vout) for a deterministic digest. */
    sqlite3_stmt *cs = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT t.token_id,t.txid,t.vout,t.amount,o.address_hash "
            "FROM zslp_transfers t "
            "JOIN tx_outputs o ON o.txid=t.txid AND o.vout=t.vout "
            "WHERE t.block_height=? ORDER BY t.txid ASC, t.vout ASC",
            -1, &cs, NULL) != SQLITE_OK || !cs)
        LOG_FAIL("zslp_ledger", "apply_height: create prepare failed h=%d: %s",
                 height, sqlite3_errmsg(ndb->db));
    AR_BIND_INT(cs, 1, height);

    while (AR_STEP_ROW(cs)) {
        struct zslp_ledger_row row;
        memset(&row, 0, sizeof(row));
        if (AR_COL_BYTES(cs, 0) != 32 || AR_COL_BYTES(cs, 1) != 32)
            continue;
        AR_READ_BLOB(cs, 0, row.token_id, 32);
        AR_READ_BLOB(cs, 1, row.txid, 32);
        row.vout = (int32_t)AR_COL_INT(cs, 2);
        row.amount = AR_COL_INT(cs, 3);
        if (AR_COL_BYTES(cs, 4) == 20) {
            AR_READ_BLOB(cs, 4, row.address, 20);
            row.has_address = true;
        }
        row.created_height = height;

        if (!zslp_ledger_save(ndb, &row))
            LOG_WARN("zslp_ledger", "apply_height: create save failed h=%d "
                     "vout=%d", height, row.vout);

        sha3_256_write(&ctx, (const uint8_t *)"C", 1);
        sha3_256_write(&ctx, row.token_id, 32);
        sha3_256_write(&ctx, row.txid, 32);
        sha3_write_le32(&ctx, (uint32_t)row.vout);
        sha3_write_le64(&ctx, (uint64_t)row.amount);
        uint8_t addr_flag = row.has_address ? 1 : 0;
        sha3_256_write(&ctx, &addr_flag, 1);
        if (row.has_address) sha3_256_write(&ctx, row.address, 20);
        sha3_write_le32(&ctx, (uint32_t)row.created_height);
    }
    AR_FINALIZE(cs);

    /* SPEND rows: every input at this height, in (txid,vin_index) order.
     * zslp_ledger_mark_spent tells us whether it actually consumed a token
     * outpoint (changes()>0); only those fold into the digest. */
    sqlite3_stmt *ss = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT prev_txid,prev_vout,txid FROM tx_inputs "
            "WHERE block_height=? ORDER BY txid ASC, vin_index ASC",
            -1, &ss, NULL) != SQLITE_OK || !ss)
        LOG_FAIL("zslp_ledger", "apply_height: spend prepare failed h=%d: %s",
                 height, sqlite3_errmsg(ndb->db));
    AR_BIND_INT(ss, 1, height);

    while (AR_STEP_ROW(ss)) {
        uint8_t prev_txid[32], spender[32];
        if (AR_COL_BYTES(ss, 0) != 32 || AR_COL_BYTES(ss, 2) != 32)
            continue;
        AR_READ_BLOB(ss, 0, prev_txid, 32);
        int32_t prev_vout = (int32_t)AR_COL_INT(ss, 1);
        AR_READ_BLOB(ss, 2, spender, 32);

        if (zslp_ledger_mark_spent(ndb, prev_txid, prev_vout, spender,
                                   height)) {
            sha3_256_write(&ctx, (const uint8_t *)"S", 1);
            sha3_256_write(&ctx, prev_txid, 32);
            sha3_write_le32(&ctx, (uint32_t)prev_vout);
            sha3_256_write(&ctx, spender, 32);
            sha3_write_le32(&ctx, (uint32_t)height);
        }
    }
    AR_FINALIZE(ss);

    sha3_256_finalize(&ctx, out_digest);
    return true;
}

/* ── Cursor / digest state ─────────────────────────────────────────── */

bool zslp_ledger_get_cursor(struct node_db *ndb, int32_t *out_height,
                            uint8_t out_digest[32])
{
    if (!ndb || !ndb->open) return false;
    int64_t h = -1;
    bool found = node_db_state_get_int(ndb, ZSLP_LEDGER_CURSOR_KEY, &h);
    if (out_height) *out_height = found ? (int32_t)h : -1;
    if (out_digest) {
        size_t len = 0;
        if (!found ||
            !node_db_state_get(ndb, ZSLP_LEDGER_DIGEST_KEY, out_digest, 32,
                               &len) || len != 32)
            memset(out_digest, 0, 32);
    }
    /* "nothing folded yet" is a valid state, not a failure. */
    return true;
}

bool zslp_ledger_set_cursor(struct node_db *ndb, int32_t height,
                            const uint8_t digest[32])
{
    if (!ndb || !ndb->open || !digest)
        LOG_FAIL("zslp_ledger", "set_cursor: invalid args");
    if (!node_db_state_set_int(ndb, ZSLP_LEDGER_CURSOR_KEY, (int64_t)height))
        LOG_FAIL("zslp_ledger", "set_cursor: failed to persist height=%d",
                 height);
    if (!node_db_state_set(ndb, ZSLP_LEDGER_DIGEST_KEY, digest, 32))
        LOG_FAIL("zslp_ledger", "set_cursor: failed to persist digest at h=%d",
                 height);
    return true;
}

bool zslp_ledger_truncate(struct node_db *ndb)
{
    if (!ndb || !ndb->open)
        LOG_FAIL("zslp_ledger", "truncate: db not open");
    if (!node_db_exec(ndb, "DELETE FROM zslp_ledger"))
        LOG_FAIL("zslp_ledger", "truncate: DELETE failed");
    uint8_t zero[32] = {0};
    if (!node_db_state_set_int(ndb, ZSLP_LEDGER_CURSOR_KEY, -1))
        LOG_FAIL("zslp_ledger", "truncate: failed to reset cursor");
    if (!node_db_state_set(ndb, ZSLP_LEDGER_DIGEST_KEY, zero, 32))
        LOG_FAIL("zslp_ledger", "truncate: failed to reset digest");
    return true;
}

/* ── Queries ───────────────────────────────────────────────────────── */

int64_t zslp_ledger_balance(struct node_db *ndb, const uint8_t token_id[32],
                            const uint8_t address[20])
{
    if (!ndb || !ndb->open || !token_id || !address) return 0;
    sqlite3_stmt *s = NULL;
    AR_QUERY_INT64_BOUND(ndb, s,
        "SELECT COALESCE(SUM(amount),0) FROM zslp_ledger "
        "WHERE token_id=? AND address=? AND spent_by_txid IS NULL",
        AR_BIND_BLOB(s, 1, token_id, 32);
        AR_BIND_BLOB(s, 2, address, 20));
}

int64_t zslp_ledger_count(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    AR_QUERY_INT64_BOUND(ndb, s, "SELECT COUNT(*) FROM zslp_ledger", (void)0);
}

int64_t zslp_ledger_unspent_count(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    AR_QUERY_INT64_BOUND(ndb, s,
        "SELECT COUNT(*) FROM zslp_ledger WHERE spent_by_txid IS NULL",
        (void)0);
}
