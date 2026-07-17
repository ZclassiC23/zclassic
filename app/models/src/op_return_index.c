/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ActiveRecord model for the OP_RETURN catalog projection
 * (op_return_index) — see models/op_return_index.h for the field
 * semantics and the threading contract. The extract/digest logic here is
 * pure (no IO); the SQLite plumbing follows the same
 * AR_ADHOC_SAVE/AR_QUERY_* shape as models/zanc.c. */

#include "models/op_return_index.h"

#include "crypto/sha3.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "script/op_return_push.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <string.h>

#define OP_RETURN_INDEX_CURSOR_KEY "op_return_index_cursor_height"
#define OP_RETURN_INDEX_DIGEST_KEY "op_return_index_digest"

DEFINE_MODEL_CALLBACKS(op_return_index)

/* ── Pure extraction ───────────────────────────────────────────────── */

/* Render `tag[0..tag_len)` into `out` (OP_RETURN_INDEX_TAG_TEXT_MAX bytes):
 * ASCII (trailing NUL bytes trimmed first — ZSLP's lokad is "SLP\0") when
 * every remaining byte is printable, else lowercase hex of the raw bytes. */
static void tag_to_text(const uint8_t *tag, uint8_t tag_len,
                        char out[OP_RETURN_INDEX_TAG_TEXT_MAX])
{
    uint8_t n = tag_len;
    while (n > 0 && tag[n - 1] == 0x00) n--;

    bool printable = (n > 0);
    for (uint8_t i = 0; i < n && printable; i++)
        if (tag[i] < 0x20 || tag[i] > 0x7e) printable = false;

    if (printable) {
        memcpy(out, tag, n);
        out[n] = '\0';
        return;
    }

    static const char hex[] = "0123456789abcdef";
    size_t o = 0;
    for (uint8_t i = 0; i < tag_len && o + 2 < OP_RETURN_INDEX_TAG_TEXT_MAX; i++) {
        out[o++] = hex[tag[i] >> 4];
        out[o++] = hex[tag[i] & 0x0f];
    }
    out[o] = '\0';
}

bool op_return_index_extract(const uint8_t *script, size_t script_len,
                             struct op_return_index_row *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!script || script_len == 0 || script[0] != 0x6a)
        return false;

    const uint8_t *payload = script + 1;
    size_t payload_len = script_len - 1;
    /* Scripts are far smaller than UINT32_MAX (MAX_SCRIPT_SIZE); the clamp
     * is a defensive belt, not an expected path. */
    out->payload_len = (uint32_t)(payload_len > UINT32_MAX
                                       ? UINT32_MAX : payload_len);
    zcl_sha3_256(payload, payload_len, out->payload_sha3);

    const uint8_t *end = script + script_len;
    const uint8_t *data = NULL;
    size_t len = 0;
    const uint8_t *after = read_push(payload, end, &data, &len);
    if (after && len <= OP_RETURN_INDEX_TAG_MAX) {
        /* A well-formed push, including the canonical empty push (OP_0,
         * len==0) — do NOT fall through to the raw-byte fallback below,
         * which would otherwise mistake "OP_RETURN OP_0" for a malformed
         * script and catalog a 1-byte tag instead of the true 0-byte one. */
        if (len > 0) memcpy(out->tag, data, len);
        out->tag_len = (uint8_t)len;
    } else {
        size_t n = payload_len < OP_RETURN_INDEX_TAG_MAX
                       ? payload_len : OP_RETURN_INDEX_TAG_MAX;
        memcpy(out->tag, payload, n);
        out->tag_len = (uint8_t)n;
    }

    tag_to_text(out->tag, out->tag_len, out->tag_text);
    return true;
}

void op_return_index_fold_block_digest(const uint8_t prev_digest[32],
                                       int32_t height,
                                       const uint8_t block_hash[32],
                                       const struct op_return_index_row *rows,
                                       size_t n_rows, uint8_t out_digest[32])
{
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    sha3_256_write(&ctx, prev_digest, 32);

    uint8_t le8[8];
    uint64_t h64 = (uint64_t)(int64_t)height;
    for (int i = 0; i < 8; i++) le8[i] = (uint8_t)(h64 >> (8 * i));
    sha3_256_write(&ctx, le8, 8);
    sha3_256_write(&ctx, block_hash, 32);

    uint8_t le4[4];
    uint32_t nrows32 = (uint32_t)n_rows;
    for (int i = 0; i < 4; i++) le4[i] = (uint8_t)(nrows32 >> (8 * i));
    sha3_256_write(&ctx, le4, 4);

    for (size_t i = 0; i < n_rows; i++) {
        const struct op_return_index_row *r = &rows[i];
        sha3_256_write(&ctx, r->txid, 32);
        uint8_t vle[4];
        for (int b = 0; b < 4; b++) vle[b] = (uint8_t)(r->vout_n >> (8 * b));
        sha3_256_write(&ctx, vle, 4);
        sha3_256_write(&ctx, &r->tag_len, 1);
        sha3_256_write(&ctx, r->tag, r->tag_len);
        uint8_t ple[4];
        for (int b = 0; b < 4; b++)
            ple[b] = (uint8_t)(r->payload_len >> (8 * b));
        sha3_256_write(&ctx, ple, 4);
        sha3_256_write(&ctx, r->payload_sha3, 32);
    }
    sha3_256_finalize(&ctx, out_digest);
}

/* ── AR plumbing ───────────────────────────────────────────────────── */

bool db_op_return_index_validate(const struct op_return_index_row *r,
                                 struct ar_errors *errors)
{
    ar_errors_clear(errors);
    if (!r) {
        ar_errors_add(errors, "row", "is NULL");
        return false;
    }
    validates_non_negative(errors, r, height);
    validates_custom(errors, r->tag_len <= OP_RETURN_INDEX_TAG_MAX,
                     "tag_len", "exceeds OP_RETURN_INDEX_TAG_MAX");
    return !ar_errors_any(errors);
}

bool db_op_return_index_save(struct node_db *ndb,
                             const struct op_return_index_row *row)
{
    if (!ndb || !ndb->open)
        LOG_FAIL("op_return_index", "db_op_return_index_save: db not open");
    if (!row)
        LOG_FAIL("op_return_index", "db_op_return_index_save: row is NULL");

    struct ar_callbacks *cbs = db_op_return_index_callbacks();
    sqlite3_stmt *s = NULL;
    AR_ADHOC_SAVE(ndb, s,
        "INSERT OR IGNORE INTO op_return_index"
        "(txid,vout_n,height,tag,tag_text,payload_len,payload_sha3)"
        " VALUES(?,?,?,?,?,?,?)",
        cbs, "op_return_index", row, db_op_return_index_validate,
        AR_BIND_BLOB(s, 1, row->txid, 32);
        AR_BIND_INT(s, 2, (int)row->vout_n);
        AR_BIND_INT(s, 3, row->height);
        AR_BIND_BLOB(s, 4, row->tag, row->tag_len);
        AR_BIND_TEXT(s, 5, row->tag_text);
        AR_BIND_INT(s, 6, (int)row->payload_len);
        AR_BIND_BLOB(s, 7, row->payload_sha3, 32));
}

bool op_return_index_apply_block_rows(struct node_db *ndb,
                                      const struct block *blk, int32_t height,
                                      struct op_return_index_row *rows_out,
                                      size_t rows_cap,
                                      size_t *rows_count_out)
{
    if (!ndb || !ndb->open || !blk)
        LOG_FAIL("op_return_index",
                 "apply_block_rows: invalid args (ndb=%p blk=%p)",
                 (void *)ndb, (const void *)blk);

    size_t n = 0;
    for (size_t ti = 0; ti < blk->num_vtx; ti++) {
        const struct transaction *tx = &blk->vtx[ti];
        for (size_t vo = 0; vo < tx->num_vout; vo++) {
            const struct tx_out *o = &tx->vout[vo];
            const uint8_t *script = o->script_pub_key.data;
            size_t slen = o->script_pub_key.size;
            if (slen == 0 || script[0] != 0x6a)
                continue;

            struct op_return_index_row row;
            if (!op_return_index_extract(script, slen, &row))
                continue;
            memcpy(row.txid, tx->hash.data, 32);
            row.vout_n = (uint32_t)vo;
            row.height = height;

            if (!db_op_return_index_save(ndb, &row))
                LOG_WARN("op_return_index",
                         "apply_block_rows: save failed h=%d tx=%zu vout=%zu",
                         height, ti, vo);

            if (rows_out && n < rows_cap)
                rows_out[n] = row;
            n++;
        }
    }
    if (rows_count_out) *rows_count_out = n;
    return true;
}

/* ── Cursor / digest state ────────────────────────────────────────── */

bool op_return_index_get_cursor(struct node_db *ndb, int32_t *out_height,
                                uint8_t out_digest[32])
{
    if (!ndb || !ndb->open) return false;
    int64_t h = -1;
    bool found = node_db_state_get_int(ndb, OP_RETURN_INDEX_CURSOR_KEY, &h);
    if (out_height) *out_height = found ? (int32_t)h : -1;
    if (out_digest) {
        size_t len = 0;
        if (!found ||
            !node_db_state_get(ndb, OP_RETURN_INDEX_DIGEST_KEY, out_digest,
                               32, &len) || len != 32)
            memset(out_digest, 0, 32);
    }
    /* "nothing folded yet" is a valid state, not a failure. */
    return true;
}

bool op_return_index_set_cursor(struct node_db *ndb, int32_t height,
                                const uint8_t digest[32])
{
    if (!ndb || !ndb->open || !digest)
        LOG_FAIL("op_return_index", "set_cursor: invalid args");
    if (!node_db_state_set_int(ndb, OP_RETURN_INDEX_CURSOR_KEY,
                               (int64_t)height))
        LOG_FAIL("op_return_index",
                 "set_cursor: failed to persist height=%d", height);
    if (!node_db_state_set(ndb, OP_RETURN_INDEX_DIGEST_KEY, digest, 32))
        LOG_FAIL("op_return_index",
                 "set_cursor: failed to persist digest at height=%d", height);
    return true;
}

bool op_return_index_truncate(struct node_db *ndb)
{
    if (!ndb || !ndb->open)
        LOG_FAIL("op_return_index", "truncate: db not open");
    if (!node_db_exec(ndb, "DELETE FROM op_return_index"))
        LOG_FAIL("op_return_index", "truncate: DELETE failed");
    uint8_t zero[32] = {0};
    if (!node_db_state_set_int(ndb, OP_RETURN_INDEX_CURSOR_KEY, -1))
        LOG_FAIL("op_return_index", "truncate: failed to reset cursor");
    if (!node_db_state_set(ndb, OP_RETURN_INDEX_DIGEST_KEY, zero, 32))
        LOG_FAIL("op_return_index", "truncate: failed to reset digest");
    return true;
}

/* ── Queries ───────────────────────────────────────────────────────── */

int64_t op_return_index_count(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    AR_QUERY_INT64_BOUND(ndb, s, "SELECT COUNT(*) FROM op_return_index",
                         (void)0);
}

int64_t op_return_index_count_by_tag_text(struct node_db *ndb,
                                          const char *tag_text)
{
    if (!ndb || !ndb->open || !tag_text) return 0;
    sqlite3_stmt *s = NULL;
    AR_QUERY_INT64_BOUND(ndb, s,
        "SELECT COUNT(*) FROM op_return_index WHERE tag_text=?",
        AR_BIND_TEXT(s, 1, tag_text));
}

static void row_from_stmt(sqlite3_stmt *s, struct op_return_index_row *out)
{
    memset(out, 0, sizeof(*out));
    const void *txid = sqlite3_column_blob(s, 0);
    int txid_len = sqlite3_column_bytes(s, 0);
    if (txid && txid_len == 32) memcpy(out->txid, txid, 32);
    out->vout_n = (uint32_t)sqlite3_column_int(s, 1);
    out->height = sqlite3_column_int(s, 2);
    const void *tag = sqlite3_column_blob(s, 3);
    int tag_len = sqlite3_column_bytes(s, 3);
    if (tag && tag_len > 0 && tag_len <= OP_RETURN_INDEX_TAG_MAX) {
        memcpy(out->tag, tag, (size_t)tag_len);
        out->tag_len = (uint8_t)tag_len;
    }
    const char *tt = (const char *)sqlite3_column_text(s, 4);
    if (tt) snprintf(out->tag_text, sizeof(out->tag_text), "%s", tt);
    out->payload_len = (uint32_t)sqlite3_column_int(s, 5);
    const void *ps = sqlite3_column_blob(s, 6);
    int ps_len = sqlite3_column_bytes(s, 6);
    if (ps && ps_len == 32) memcpy(out->payload_sha3, ps, 32);
}

int op_return_index_query(struct node_db *ndb, int32_t h_min, int32_t h_max,
                          const char *tag_text_filter,
                          struct op_return_index_row *out, size_t max)
{
    if (!ndb || !ndb->open) return 0;
    if (!out && max > 0)
        LOG_RETURN(0, "op_return_index", "query: out is NULL");

    sqlite3_stmt *s = NULL;
    if (tag_text_filter && tag_text_filter[0]) {
        AR_QUERY_LIST(ndb, s,
            "SELECT txid,vout_n,height,tag,tag_text,payload_len,payload_sha3"
            " FROM op_return_index WHERE height>=? AND height<=?"
            " AND tag_text=?"
            " ORDER BY height DESC, txid ASC, vout_n ASC LIMIT ?",
            out, max,
            AR_BIND_INT(s, 1, h_min);
            AR_BIND_INT(s, 2, h_max);
            AR_BIND_TEXT(s, 3, tag_text_filter);
            AR_BIND_INT(s, 4, (int)max),
            row_from_stmt(s, &out[count]));
    } else {
        AR_QUERY_LIST(ndb, s,
            "SELECT txid,vout_n,height,tag,tag_text,payload_len,payload_sha3"
            " FROM op_return_index WHERE height>=? AND height<=?"
            " ORDER BY height DESC, txid ASC, vout_n ASC LIMIT ?",
            out, max,
            AR_BIND_INT(s, 1, h_min);
            AR_BIND_INT(s, 2, h_max);
            AR_BIND_INT(s, 3, (int)max),
            row_from_stmt(s, &out[count]));
    }
}
