/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ActiveRecord models for ZCL Names (ZNAM).
 *
 * Three sibling models — one per table:
 *   znam_names         (struct znam_entry)
 *   znam_text_records  (struct znam_text_record)
 *   znam_addr_records  (struct znam_addr_record)
 *
 * This file owns the integrity and persistence contract for the three
 * on-chain-derived tables. A malformed znam row at rest means a malformed
 * OP_RETURN was accepted earlier in the pipeline — these validators are the
 * last checkpoint before the row is written. The OP_RETURN parser/builder
 * stays in lib/znam/src/znam.c. */

#include "models/znam.h"
#include "platform/clock.h"
#include "storage/znam_projection.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

DEFINE_MODEL_CALLBACKS(znam_entry)
DEFINE_MODEL_CALLBACKS(znam_text)
DEFINE_MODEL_CALLBACKS(znam_addr)

static void znam_entry_after_save(void *record, void *ctx)
{
    const struct znam_entry *entry = record;

    (void)ctx;
    if (!entry || !znam_projection_event_log())
        return;

    /* Projection event emit. Always emit REGISTER — the projection uses
     * INSERT OR REPLACE so re-registers are idempotent and the primary-target
     * fields stay in sync without a separate UPDATE. */
    if (!znam_projection_emit_register(
            entry->name, entry->owner_address, entry->target_type,
            entry->target_value, entry->reg_txid, entry->reg_height,
            (uint32_t)(clock_now_wall_ms() / 1000), 0)) {
        fprintf(stderr,  // obs-ok:znam-projection-emit
                "znam projection emit failed for register\n");
    }
}

static void znam_text_after_save(void *record, void *ctx)
{
    const struct znam_text_record *rec = record;
    static const uint8_t zero_txid[32] = {0};

    (void)ctx;
    if (!rec || !znam_projection_event_log())
        return;

    /* update_txid unknown at this layer; the legacy caller didn't track it,
     * so pass zeros — consumers tolerate it because it only bumps
     * last_update_txid for audit. */
    if (!znam_projection_emit_update_text(rec->name, rec->key,
                                          rec->value, zero_txid)) {
        fprintf(stderr,  // obs-ok:znam-projection-emit
                "znam projection emit failed for text update\n");
    }
}

static void znam_addr_after_save(void *record, void *ctx)
{
    const struct znam_addr_record *rec = record;
    static const uint8_t zero_txid[32] = {0};

    (void)ctx;
    if (!rec || !znam_projection_event_log())
        return;

    if (!znam_projection_emit_update_addr(rec->name, rec->coin_type,
                                          rec->address, zero_txid)) {
        fprintf(stderr,  // obs-ok:znam-projection-emit
                "znam projection emit failed for addr update\n");
    }
}

static struct ar_callbacks *znam_entry_callbacks_ready(void)
{
    struct ar_callbacks *cbs = db_znam_entry_callbacks();
    static bool hooks_done = false;
    if (!hooks_done) {
        ar_register_after_save(cbs, znam_entry_after_save);
        hooks_done = true;
    }
    return cbs;
}

static struct ar_callbacks *znam_text_callbacks_ready(void)
{
    struct ar_callbacks *cbs = db_znam_text_callbacks();
    static bool hooks_done = false;
    if (!hooks_done) {
        ar_register_after_save(cbs, znam_text_after_save);
        hooks_done = true;
    }
    return cbs;
}

static struct ar_callbacks *znam_addr_callbacks_ready(void)
{
    struct ar_callbacks *cbs = db_znam_addr_callbacks();
    static bool hooks_done = false;
    if (!hooks_done) {
        ar_register_after_save(cbs, znam_addr_after_save);
        hooks_done = true;
    }
    return cbs;
}

bool db_znam_entry_validate(const struct znam_entry *entry,
                            struct ar_errors *errors)
{
    ar_errors_clear(errors);
    if (!entry) {
        ar_errors_add(errors, "entry", "is NULL");
        return false;
    }

    static const uint8_t zero32[32] = {0};

    validates_custom(errors,
        znam_validate_name(entry->name),
        "name", "is not a valid ZNAM name");
    validates_presence_of(errors, entry, owner_address);
    validates_range(errors, entry, target_type,
                    ZNAM_TYPE_ONION, ZNAM_TYPE_CONTENT);
    validates_presence_of(errors, entry, target_value);
    validates_custom(errors,
        strnlen(entry->target_value, ZNAM_VALUE_MAX + 1) <= ZNAM_VALUE_MAX,
        "target_value", "exceeds ZNAM_VALUE_MAX");
    validates_custom(errors,
        memcmp(entry->reg_txid, zero32, 32) != 0,
        "reg_txid", "can't be all zero");
    validates_non_negative(errors, entry, reg_height);

    return !ar_errors_any(errors);
}

bool db_znam_text_validate(const struct znam_text_record *rec,
                           struct ar_errors *errors)
{
    ar_errors_clear(errors);
    if (!rec) {
        ar_errors_add(errors, "rec", "is NULL");
        return false;
    }

    validates_custom(errors,
        znam_validate_name(rec->name),
        "name", "is not a valid ZNAM name");
    validates_presence_of(errors, rec, key);
    validates_custom(errors,
        strnlen(rec->key, ZNAM_TEXT_KEY_MAX + 1) <= ZNAM_TEXT_KEY_MAX,
        "key", "exceeds ZNAM_TEXT_KEY_MAX");
    /* value may be empty (deletion via empty string) */
    validates_custom(errors,
        strnlen(rec->value, ZNAM_TEXT_VAL_MAX + 1) <= ZNAM_TEXT_VAL_MAX,
        "value", "exceeds ZNAM_TEXT_VAL_MAX");

    return !ar_errors_any(errors);
}

bool db_znam_addr_validate(const struct znam_addr_record *rec,
                           struct ar_errors *errors)
{
    ar_errors_clear(errors);
    if (!rec) {
        ar_errors_add(errors, "rec", "is NULL");
        return false;
    }

    validates_custom(errors,
        znam_validate_name(rec->name),
        "name", "is not a valid ZNAM name");
    validates_range(errors, rec, coin_type,
                    ZNAM_TYPE_ONION, ZNAM_TYPE_CONTENT);
    validates_presence_of(errors, rec, address);
    validates_custom(errors,
        strnlen(rec->address, ZNAM_VALUE_MAX + 1) <= ZNAM_VALUE_MAX,
        "address", "exceeds ZNAM_VALUE_MAX");

    return !ar_errors_any(errors);
}

bool db_znam_save(struct node_db *ndb, const struct znam_entry *entry)
{
    if (!ndb || !ndb->open) LOG_FAIL("znam", "db_znam_save: db not open");
    if (!entry) LOG_FAIL("znam", "db_znam_save: entry is NULL");

    struct ar_callbacks *cbs = znam_entry_callbacks_ready();
    sqlite3_stmt *s = NULL;
    AR_BEGIN_SAVE(cbs, "znam_entry", entry, db_znam_entry_validate);
    AR_PREPARE_BOOL(ndb, s,
        "INSERT OR REPLACE INTO znam_names"
        "(name,owner_address,target_type,target_value,"
        "reg_txid,reg_height,last_update_txid)"
        " VALUES(?,?,?,?,?,?,?)");
    AR_BIND_TEXT(s, 1, entry->name);
    AR_BIND_TEXT(s, 2, entry->owner_address);
    AR_BIND_INT(s, 3, entry->target_type);
    AR_BIND_TEXT(s, 4, entry->target_value);
    AR_BIND_BLOB(s, 5, entry->reg_txid, 32);
    AR_BIND_INT(s, 6, entry->reg_height);
    AR_BIND_BLOB(s, 7, entry->last_update_txid, 32);

    bool ok = false;
    AR_FINALIZE_STEP_DONE(s, ok);
    AR_FINISH_SAVE(cbs, entry, ok);
}

static void row_to_znam(sqlite3_stmt *s, struct znam_entry *out)
{
    memset(out, 0, sizeof(*out));
    const char *name = (const char *)sqlite3_column_text(s, 0);
    if (name) snprintf(out->name, sizeof(out->name), "%s", name);

    const char *owner = (const char *)sqlite3_column_text(s, 1);
    if (owner) snprintf(out->owner_address, sizeof(out->owner_address),
                        "%s", owner);

    out->target_type = (uint8_t)sqlite3_column_int(s, 2);

    const char *val = (const char *)sqlite3_column_text(s, 3);
    if (val) snprintf(out->target_value, sizeof(out->target_value),
                      "%s", val);

    AR_READ_BLOB(s, 4, out->reg_txid, 32);

    out->reg_height = (int32_t)sqlite3_column_int(s, 5);

    AR_READ_BLOB(s, 6, out->last_update_txid, 32);
}

bool db_znam_find(struct node_db *ndb, const char *name,
                  struct znam_entry *out)
{
    if (!ndb || !ndb->open) return false;
    if (!name || !out) return false;

    sqlite3_stmt *s = NULL;
    AR_QUERY_ONE_BOOL(ndb, s,
        "SELECT name,owner_address,target_type,target_value,"
        "reg_txid,reg_height,last_update_txid"
        " FROM znam_names WHERE name=?",
        AR_BIND_TEXT(s, 1, name),
        row_to_znam(s, out));
}

int db_znam_list(struct node_db *ndb, struct znam_entry *out, size_t max)
{
    if (!ndb || !ndb->open) return 0;
    if (!out && max > 0)
        LOG_RETURN(0, "znam", "db_znam_list: out is NULL");

    sqlite3_stmt *s = NULL;
    AR_QUERY_LIST(ndb, s,
        "SELECT name,owner_address,target_type,target_value,"
        "reg_txid,reg_height,last_update_txid"
        " FROM znam_names ORDER BY reg_height DESC LIMIT ?",
        out, max,
        AR_BIND_INT(s, 1, (int)max),
        row_to_znam(s, &out[count]));
}

int db_znam_list_by_owner(struct node_db *ndb, const char *owner,
                          struct znam_entry *out, size_t max)
{
    if (!ndb || !ndb->open) return 0;
    if (!owner) return 0;
    if (!out && max > 0)
        LOG_RETURN(0, "znam", "db_znam_list_by_owner: out is NULL");

    sqlite3_stmt *s = NULL;
    AR_QUERY_LIST(ndb, s,
        "SELECT name,owner_address,target_type,target_value,"
        "reg_txid,reg_height,last_update_txid"
        " FROM znam_names WHERE owner_address=? ORDER BY name LIMIT ?",
        out, max,
        AR_BIND_TEXT(s, 1, owner);
        AR_BIND_INT(s, 2, (int)max),
        row_to_znam(s, &out[count]));
}

bool db_znam_text_save(struct node_db *ndb, const char *name,
                       const char *key, const char *value)
{
    if (!ndb || !ndb->open) LOG_FAIL("znam", "db_znam_text_save: db not open");
    if (!name || !key) LOG_FAIL("znam", "db_znam_text_save: name/key NULL");

    struct znam_text_record rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.name, sizeof(rec.name), "%s", name);
    snprintf(rec.key, sizeof(rec.key), "%s", key);
    if (value) snprintf(rec.value, sizeof(rec.value), "%s", value);

    struct ar_callbacks *cbs = znam_text_callbacks_ready();
    sqlite3_stmt *s = NULL;
    AR_BEGIN_SAVE(cbs, "znam_text", &rec, db_znam_text_validate);
    AR_PREPARE_BOOL(ndb, s,
        "INSERT OR REPLACE INTO znam_text_records(name,key,value)"
        " VALUES(?,?,?)");
    AR_BIND_TEXT(s, 1, rec.name);
    AR_BIND_TEXT(s, 2, rec.key);
    AR_BIND_TEXT(s, 3, rec.value);

    bool ok = false;
    AR_FINALIZE_STEP_DONE(s, ok);
    AR_FINISH_SAVE(cbs, &rec, ok);
}

bool db_znam_text_get(struct node_db *ndb, const char *name,
                      const char *key, char *value_out, size_t max)
{
    if (!ndb || !ndb->open) return false;
    if (!name || !key || !value_out || max == 0) return false;

    sqlite3_stmt *s = NULL;
    AR_QUERY_ONE_BOOL(ndb, s,
        "SELECT value FROM znam_text_records WHERE name=? AND key=?",
        AR_BIND_TEXT(s, 1, name);
        AR_BIND_TEXT(s, 2, key),
        const char *v = (const char *)sqlite3_column_text(s, 0);
        if (v) snprintf(value_out, max, "%s", v));
}

static void row_to_znam_text(sqlite3_stmt *s, struct znam_text_record *out)
{
    memset(out, 0, sizeof(*out));
    const char *n = (const char *)sqlite3_column_text(s, 0);
    if (n) snprintf(out->name, sizeof(out->name), "%s", n);
    const char *k = (const char *)sqlite3_column_text(s, 1);
    if (k) snprintf(out->key, sizeof(out->key), "%s", k);
    const char *v = (const char *)sqlite3_column_text(s, 2);
    if (v) snprintf(out->value, sizeof(out->value), "%s", v);
}

int db_znam_text_list(struct node_db *ndb, const char *name,
                      struct znam_text_record *out, size_t max)
{
    if (!ndb || !ndb->open) return 0;
    if (!name) return 0;
    if (!out && max > 0)
        LOG_RETURN(0, "znam", "db_znam_text_list: out is NULL");

    sqlite3_stmt *s = NULL;
    AR_QUERY_LIST(ndb, s,
        "SELECT name,key,value FROM znam_text_records"
        " WHERE name=? ORDER BY key LIMIT ?",
        out, max,
        AR_BIND_TEXT(s, 1, name);
        AR_BIND_INT(s, 2, (int)max),
        row_to_znam_text(s, &out[count]));
}

bool db_znam_addr_save(struct node_db *ndb, const char *name,
                       uint8_t coin_type, const char *address)
{
    if (!ndb || !ndb->open) LOG_FAIL("znam", "db_znam_addr_save: db not open");
    if (!name || !address)
        LOG_FAIL("znam", "db_znam_addr_save: name/address NULL");

    struct znam_addr_record rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.name, sizeof(rec.name), "%s", name);
    rec.coin_type = coin_type;
    snprintf(rec.address, sizeof(rec.address), "%s", address);

    struct ar_callbacks *cbs = znam_addr_callbacks_ready();
    sqlite3_stmt *s = NULL;
    AR_BEGIN_SAVE(cbs, "znam_addr", &rec, db_znam_addr_validate);
    AR_PREPARE_BOOL(ndb, s,
        "INSERT OR REPLACE INTO znam_addr_records(name,coin_type,address)"
        " VALUES(?,?,?)");
    AR_BIND_TEXT(s, 1, rec.name);
    AR_BIND_INT(s, 2, rec.coin_type);
    AR_BIND_TEXT(s, 3, rec.address);

    bool ok = false;
    AR_FINALIZE_STEP_DONE(s, ok);
    AR_FINISH_SAVE(cbs, &rec, ok);
}

bool db_znam_addr_get(struct node_db *ndb, const char *name,
                      uint8_t coin_type, char *addr_out, size_t max)
{
    if (!ndb || !ndb->open) return false;
    if (!name || !addr_out || max == 0) return false;

    sqlite3_stmt *s = NULL;
    AR_QUERY_ONE_BOOL(ndb, s,
        "SELECT address FROM znam_addr_records WHERE name=? AND coin_type=?",
        AR_BIND_TEXT(s, 1, name);
        AR_BIND_INT(s, 2, coin_type),
        const char *a = (const char *)sqlite3_column_text(s, 0);
        if (a) snprintf(addr_out, max, "%s", a));
}

static void row_to_znam_addr(sqlite3_stmt *s, struct znam_addr_record *out)
{
    memset(out, 0, sizeof(*out));
    const char *n = (const char *)sqlite3_column_text(s, 0);
    if (n) snprintf(out->name, sizeof(out->name), "%s", n);
    out->coin_type = (uint8_t)sqlite3_column_int(s, 1);
    const char *a = (const char *)sqlite3_column_text(s, 2);
    if (a) snprintf(out->address, sizeof(out->address), "%s", a);
}

int db_znam_addr_list(struct node_db *ndb, const char *name,
                      struct znam_addr_record *out, size_t max)
{
    if (!ndb || !ndb->open) return 0;
    if (!name) return 0;
    if (!out && max > 0)
        LOG_RETURN(0, "znam", "db_znam_addr_list: out is NULL");

    sqlite3_stmt *s = NULL;
    AR_QUERY_LIST(ndb, s,
        "SELECT name,coin_type,address FROM znam_addr_records"
        " WHERE name=? ORDER BY coin_type LIMIT ?",
        out, max,
        AR_BIND_TEXT(s, 1, name);
        AR_BIND_INT(s, 2, (int)max),
        row_to_znam_addr(s, &out[count]));
}
