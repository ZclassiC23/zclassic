/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ActiveRecord model: ZswapAd — the persisted half of the zswap YARDSALE
 * (signed "for sale by owner" ZSLP/ZCL gossip ads).
 *
 * Wires callbacks, validation, and SQLite persistence for the `zswap_ads`
 * table (migration v48). The gossip/ingress/cache logic lives in
 * lib/zswap/src/zswap_yardsale.c; this table is its rebuildable
 * projection — the "verify evidence valid when created" record: rows are
 * written only after the ad verified at ingress, queries filter by now,
 * and db_zswap_ad_prune_expired() reclaims closed windows.
 *
 * The record type is `struct zswap_yardsale_ad` from
 * zswap/zswap_yardsale.h — deliberately reused (rather than a parallel
 * db struct) so the gossip layer and persistence layer agree on the
 * at-rest representation. */

#include "models/zswap_ad.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

DEFINE_MODEL_CALLBACKS(zswap_ad)

static bool bytes_nonzero_local(const uint8_t *bytes, size_t len)
{
    uint8_t any = 0;
    for (size_t i = 0; i < len; i++) any |= bytes[i];
    return any != 0;
}

bool db_zswap_ad_validate(const struct zswap_yardsale_ad *ad,
                          struct ar_errors *errors)
{
    ar_errors_clear(errors);
    if (!ad) {
        ar_errors_add(errors, "ad", "is NULL");
        return false;
    }

    validates_custom(errors,
        bytes_nonzero_local(ad->quote_root, 32),
        "quote_root", "can't be all zero");
    validates_custom(errors,
        bytes_nonzero_local(ad->quote.seller_pubkey, 32),
        "seller_pubkey", "can't be all zero");
    validates_custom(errors,
        bytes_nonzero_local(ad->quote.token_id, 32),
        "token_id", "can't be all zero");
    validates_positive(errors, ad, quote.token_amount);
    validates_positive(errors, ad, quote.zcl_amount);
    validates_custom(errors,
        ad->quote.issued_unix > 0 &&
            ad->quote.expires_unix > ad->quote.issued_unix,
        "expires_unix", "must be after issued_unix");
    validates_custom(errors,
        ad->quote.expires_unix - ad->quote.issued_unix <=
            ZSWAP_QUOTE_MAX_LIFETIME_SECS,
        "expires_unix", "lifetime exceeds the structural cap");
    validates_positive(errors, ad, seen_count);
    validates_custom(errors,
        ad->first_seen_unix <= ad->last_seen_unix,
        "first_seen_unix", "must not be after last_seen_unix");

    return !ar_errors_any(errors);
}

bool db_zswap_ad_save(struct node_db *ndb,
                      const struct zswap_yardsale_ad *ad)
{
    if (!ndb || !ndb->open) LOG_FAIL("zswap", "db_zswap_ad_save: db not open");
    if (!ad) LOG_FAIL("zswap", "db_zswap_ad_save: ad is NULL");

    /* The stored wire is the exact 210 bytes the seller signed — re-encoded
     * from the verified struct (Ed25519 sealing + the codec are byte
     * deterministic, so this reproduces the gossiped wire exactly). */
    uint8_t wire[ZSWAP_QUOTE_WIRE_BYTES];
    if (zswap_quote_encode(&ad->quote, wire) != ZSWAP_QUOTE_OK)
        LOG_FAIL("zswap", "db_zswap_ad_save: quote re-encode failed");

    struct ar_callbacks *cbs = db_zswap_ad_callbacks();
    sqlite3_stmt *s = NULL;
    /* Upsert on the dedup id: a fresh ad inserts; a byte-identical
     * re-gossip bumps last_seen_unix/seen_count in place and keeps
     * first_seen_unix — the same dedup-on-root rule as the live cache. */
    AR_ADHOC_SAVE(ndb, s,
        "INSERT INTO zswap_ads"
        "(quote_root,wire,seller_pubkey,token_id,token_amount,zcl_amount,"
        "issued_unix,expires_unix,first_seen_unix,last_seen_unix,seen_count)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(quote_root) DO UPDATE SET"
        " last_seen_unix=excluded.last_seen_unix,"
        " seen_count=zswap_ads.seen_count+1",
        cbs, "zswap_ad", ad, db_zswap_ad_validate,
        AR_BIND_BLOB(s, 1, ad->quote_root, 32);
        AR_BIND_BLOB(s, 2, wire, ZSWAP_QUOTE_WIRE_BYTES);
        AR_BIND_BLOB(s, 3, ad->quote.seller_pubkey, 32);
        AR_BIND_BLOB(s, 4, ad->quote.token_id, 32);
        AR_BIND_INT(s, 5, (int64_t)ad->quote.token_amount);
        AR_BIND_INT(s, 6, (int64_t)ad->quote.zcl_amount);
        AR_BIND_INT(s, 7, ad->quote.issued_unix);
        AR_BIND_INT(s, 8, ad->quote.expires_unix);
        AR_BIND_INT(s, 9, ad->first_seen_unix);
        AR_BIND_INT(s, 10, ad->last_seen_unix);
        AR_BIND_INT(s, 11, (int64_t)ad->seen_count));
}

/* Rebuild the record from the stored wire (single source of truth for the
 * signed fields) plus the local bookkeeping columns. */
static bool row_to_zswap_ad(sqlite3_stmt *s, struct zswap_yardsale_ad *out)
{
    memset(out, 0, sizeof(*out));
    AR_READ_BLOB(s, 0, out->quote_root, 32);

    int wire_len = sqlite3_column_bytes(s, 1);
    const void *wire = sqlite3_column_blob(s, 1);
    if (!wire || wire_len != (int)ZSWAP_QUOTE_WIRE_BYTES)
        LOG_FAIL("zswap", "zswap_ads.wire length mismatch: got=%d expected=%d",
                 wire_len, (int)ZSWAP_QUOTE_WIRE_BYTES);
    if (zswap_quote_decode(wire, (size_t)wire_len, &out->quote) !=
        ZSWAP_QUOTE_OK)
        LOG_FAIL("zswap", "zswap_ads.wire failed to decode");

    out->first_seen_unix = sqlite3_column_int64(s, 2);
    out->last_seen_unix = sqlite3_column_int64(s, 3);
    out->seen_count = (uint64_t)sqlite3_column_int64(s, 4);
    return true;
}

bool db_zswap_ad_find(struct node_db *ndb,
                      const uint8_t quote_root[32],
                      struct zswap_yardsale_ad *out)
{
    if (!ndb || !ndb->open) LOG_FAIL("zswap", "db_zswap_ad_find: db not open");
    if (!quote_root) LOG_FAIL("zswap", "db_zswap_ad_find: quote_root is NULL");
    if (!out) LOG_FAIL("zswap", "db_zswap_ad_find: out is NULL");

    sqlite3_stmt *s = NULL;
    AR_QUERY_ONE_BOOL(ndb, s,
        "SELECT quote_root,wire,first_seen_unix,last_seen_unix,seen_count"
        " FROM zswap_ads WHERE quote_root=?",
        AR_BIND_BLOB(s, 1, quote_root, 32),
        if (!row_to_zswap_ad(s, out)) { AR_FINALIZE(s); return false; });
}

int db_zswap_ad_prune_expired(struct node_db *ndb, int64_t now_unix)
{
    if (!ndb || !ndb->open) return 0;

    sqlite3_stmt *s = NULL;
    AR_PREPARE_RET(ndb, s,
        "DELETE FROM zswap_ads WHERE expires_unix <= ?", 0);
    AR_BIND_INT(s, 1, now_unix);
    bool ok = false;
    AR_FINALIZE_STEP_DONE(s, ok);
    return ok ? sqlite3_changes(ndb->db) : 0;
}

/* qsort comparator: ascending unit price, then zcl_amount, then root —
 * identical ordering to the live yardsale cache's browse query. */
static int ad_price_qsort_cmp(const void *pa, const void *pb)
{
    const struct zswap_yardsale_ad *a = pa;
    const struct zswap_yardsale_ad *b = pb;
    int c = zswap_quote_unit_price_cmp(a->quote.zcl_amount,
                                       a->quote.token_amount,
                                       b->quote.zcl_amount,
                                       b->quote.token_amount);
    if (c != 0) return c;
    if (a->quote.zcl_amount < b->quote.zcl_amount) return -1;
    if (a->quote.zcl_amount > b->quote.zcl_amount) return 1;
    return memcmp(a->quote_root, b->quote_root, 32);
}

int db_zswap_ad_best_for_token(struct node_db *ndb,
                               const uint8_t token_id[32],
                               int64_t now_unix,
                               struct zswap_yardsale_ad *out, size_t max)
{
    if (!ndb || !ndb->open) return 0;
    if (!token_id || (!out && max > 0))
        LOG_RETURN(0, "zswap",
                   "db_zswap_ad_best_for_token: NULL token=%d out=%d",
                   !token_id, !out);

    /* Expired rows are filtered by the WHERE clause, not deleted — storage
     * keeps what was valid at ingress; db_zswap_ad_prune_expired() is the
     * explicit reclamation path. Unit-price ordering is integer-only, so it
     * runs in C (SQLite integers are 64-bit and a cross product overflows). */
    struct zswap_yardsale_ad match[ZSWAP_YARDSALE_QUERY_CAP];
    sqlite3_stmt *s = NULL;
    AR_PREPARE_RET(ndb, s,
        "SELECT quote_root,wire,first_seen_unix,last_seen_unix,seen_count"
        " FROM zswap_ads WHERE token_id=? AND expires_unix>?"
        " ORDER BY last_seen_unix DESC LIMIT ?",
        0);
    AR_BIND_BLOB(s, 1, token_id, 32);
    AR_BIND_INT(s, 2, now_unix);
    AR_BIND_INT(s, 3, (int)ZSWAP_YARDSALE_QUERY_CAP);
    size_t n = 0;
    while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW &&
           n < ZSWAP_YARDSALE_QUERY_CAP) {
        if (row_to_zswap_ad(s, &match[n])) n++;
    }
    AR_FINALIZE(s);

    qsort(match, n, sizeof(match[0]), ad_price_qsort_cmp);
    if (n > max) n = max;
    memcpy(out, match, n * sizeof(match[0]));
    return (int)n;
}

int db_zswap_ad_list_live(struct node_db *ndb,
                          int64_t now_unix,
                          struct zswap_yardsale_ad *out, size_t max)
{
    if (!ndb || !ndb->open) return 0;
    if (!out && max > 0)
        LOG_RETURN(0, "zswap", "db_zswap_ad_list_live: NULL out");

    /* Same filtering/ordering contract as db_zswap_ad_best_for_token,
     * minus the token filter — the whole yard on one page. */
    struct zswap_yardsale_ad match[ZSWAP_YARDSALE_QUERY_CAP];
    sqlite3_stmt *s = NULL;
    AR_PREPARE_RET(ndb, s,
        "SELECT quote_root,wire,first_seen_unix,last_seen_unix,seen_count"
        " FROM zswap_ads WHERE expires_unix>?"
        " ORDER BY last_seen_unix DESC LIMIT ?",
        0);
    AR_BIND_INT(s, 1, now_unix);
    AR_BIND_INT(s, 2, (int)ZSWAP_YARDSALE_QUERY_CAP);
    size_t n = 0;
    while (AR_STEP_ROW_READONLY(s) == SQLITE_ROW &&
           n < ZSWAP_YARDSALE_QUERY_CAP) {
        if (row_to_zswap_ad(s, &match[n])) n++;
    }
    AR_FINALIZE(s);

    qsort(match, n, sizeof(match[0]), ad_price_qsort_cmp);
    if (n > max) n = max;
    memcpy(out, match, n * sizeof(match[0]));
    return (int)n;
}
