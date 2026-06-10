/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ActiveRecord model: FileOffer (ZCL Market gossip)
 *
 * Wires callbacks, validation, and SQLite persistence for the `file_offers`
 * table. The P2P gossip/cache logic lives in lib/net/src/file_market.c.
 *
 * The record type is `struct file_offer` from net/file_market.h —
 * deliberately reused (rather than a parallel `struct db_file_offer`)
 * so the gossip layer and persistence layer agree byte-for-byte on
 * the on-the-wire / at-rest representation. */

#include "models/file_offer.h"
#include "platform/time_compat.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

DEFINE_MODEL_CALLBACKS(file_offer)

bool db_file_offer_validate(const struct file_offer *offer,
                            struct ar_errors *errors)
{
    ar_errors_clear(errors);
    if (!offer) {
        ar_errors_add(errors, "offer", "is NULL");
        return false;
    }

    static const uint8_t zero32[32] = {0};
    static const uint8_t zero43[43] = {0};

    validates_custom(errors,
        memcmp(offer->root_hash, zero32, 32) != 0,
        "root_hash", "can't be all zero");
    validates_presence_of(errors, offer, filename);
    validates_positive(errors, offer, size_bytes);
    validates_positive(errors, offer, num_chunks);
    validates_non_negative(errors, offer, price_per_mb);
    validates_custom(errors,
        memcmp(offer->z_addr, zero43, 43) != 0,
        "z_addr", "can't be all zero");
    validates_not_zero(errors, offer, peer_port);
    validates_non_negative(errors, offer, last_seen);
    validates_range(errors, offer, ttl, 1, FILE_MARKET_MAX_TTL);

    return !ar_errors_any(errors);
}

bool db_file_offer_save(struct node_db *ndb,
                        const struct file_offer *offer)
{
    if (!ndb || !ndb->open) LOG_FAIL("market", "db_file_offer_save: db not open");
    if (!offer) LOG_FAIL("market", "db_file_offer_save: offer is NULL");

    struct ar_callbacks *cbs = db_file_offer_callbacks();
    sqlite3_stmt *s = NULL;
    AR_ADHOC_SAVE(ndb, s,
        "INSERT OR REPLACE INTO file_offers"
        "(root_hash,filename,size_bytes,num_chunks,price_per_mb,"
        "z_addr,peer_ip,peer_port,last_seen,ttl)"
        " VALUES(?,?,?,?,?,?,?,?,?,?)",
        cbs, "file_offer", offer, db_file_offer_validate,
        AR_BIND_BLOB(s, 1, offer->root_hash, 32);
        AR_BIND_TEXT(s, 2, offer->filename);
        AR_BIND_INT(s, 3, (int64_t)offer->size_bytes);
        AR_BIND_INT(s, 4, offer->num_chunks);
        AR_BIND_INT(s, 5, offer->price_per_mb);
        AR_BIND_BLOB(s, 6, offer->z_addr, 43);
        AR_BIND_BLOB(s, 7, offer->peer_ip, 16);
        AR_BIND_INT(s, 8, offer->peer_port);
        AR_BIND_INT(s, 9, offer->last_seen
            ? offer->last_seen : (int64_t)platform_time_wall_time_t());
        AR_BIND_INT(s, 10, offer->ttl));
}

static void row_to_file_offer(sqlite3_stmt *s, struct file_offer *out)
{
    memset(out, 0, sizeof(*out));
    const void *blob = sqlite3_column_blob(s, 0);
    if (blob) memcpy(out->root_hash, blob, 32);

    const char *name = (const char *)sqlite3_column_text(s, 1);
    if (name) snprintf(out->filename, sizeof(out->filename), "%s", name);

    out->size_bytes = (uint64_t)sqlite3_column_int64(s, 2);
    out->num_chunks = (uint32_t)sqlite3_column_int(s, 3);
    out->price_per_mb = sqlite3_column_int64(s, 4);

    blob = sqlite3_column_blob(s, 5);
    if (blob) memcpy(out->z_addr, blob, 43);

    blob = sqlite3_column_blob(s, 6);
    if (blob) memcpy(out->peer_ip, blob, 16);

    out->peer_port = (uint16_t)sqlite3_column_int(s, 7);
    out->last_seen = sqlite3_column_int64(s, 8);
    out->ttl = (uint8_t)sqlite3_column_int(s, 9);
}

int db_file_offer_list(struct node_db *ndb,
                       struct file_offer *out, size_t max)
{
    if (!ndb || !ndb->open) return 0;
    if (!out && max > 0) LOG_FAIL("market", "db_file_offer_list: out is NULL");

    sqlite3_stmt *s = NULL;
    AR_QUERY_LIST(ndb, s,
        "SELECT root_hash,filename,size_bytes,num_chunks,price_per_mb,"
        "z_addr,peer_ip,peer_port,last_seen,ttl"
        " FROM file_offers ORDER BY last_seen DESC LIMIT ?",
        out, max,
        AR_BIND_INT(s, 1, (int)max),
        row_to_file_offer(s, &out[count]));
}

bool db_file_offer_find(struct node_db *ndb,
                        const uint8_t root_hash[32],
                        struct file_offer *out)
{
    if (!ndb || !ndb->open) LOG_FAIL("market", "db_file_offer_find: db not open");
    if (!root_hash) LOG_FAIL("market", "db_file_offer_find: root_hash is NULL");
    if (!out) LOG_FAIL("market", "db_file_offer_find: out is NULL");

    sqlite3_stmt *s = NULL;
    AR_QUERY_ONE_BOOL(ndb, s,
        "SELECT root_hash,filename,size_bytes,num_chunks,price_per_mb,"
        "z_addr,peer_ip,peer_port,last_seen,ttl"
        " FROM file_offers WHERE root_hash=?",
        AR_BIND_BLOB(s, 1, root_hash, 32),
        row_to_file_offer(s, out));
}

int db_file_offer_prune(struct node_db *ndb, int64_t max_age)
{
    if (!ndb || !ndb->open) return 0;

    int64_t cutoff = (int64_t)platform_time_wall_time_t() - max_age;
    sqlite3_stmt *s = NULL;
    AR_PREPARE_RET(ndb, s, "DELETE FROM file_offers WHERE last_seen < ?", 0);
    AR_BIND_INT(s, 1, cutoff);
    bool ok = false;
    AR_FINALIZE_STEP_DONE(s, ok);
    return ok ? sqlite3_changes(ndb->db) : 0;
}
