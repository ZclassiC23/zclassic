/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ActiveRecord model: Zmsg (encrypted P2P messages)
 *
 * Wires callbacks, validation, and SQLite persistence for the
 * `zmsg_messages` table. The P2P serialization and in-memory delivery
 * cache live in lib/net/src/zmsg.c.
 *
 * Record type is `struct zmsg_message` from net/zmsg.h — reused
 * rather than duplicated so wire / runtime / at-rest representations
 * stay byte-aligned. */

#include "models/zmsg.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

DEFINE_MODEL_CALLBACKS(zmsg)

bool db_zmsg_validate(const struct zmsg_message *msg,
                      struct ar_errors *errors)
{
    ar_errors_clear(errors);
    if (!msg) {
        ar_errors_add(errors, "msg", "is NULL");
        return false;
    }

    static const uint8_t zero32[32] = {0};

    validates_custom(errors,
        memcmp(msg->msg_id, zero32, 32) != 0,
        "msg_id", "can't be all zero (pre-compute via zmsg_compute_id)");
    validates_presence_of(errors, msg, sender);
    validates_presence_of(errors, msg, recipient);
    validates_presence_of(errors, msg, body);
    validates_inclusion_of(errors, msg, direction,
        ((int[]){ZMSG_INBOUND, ZMSG_OUTBOUND}), 2);
    validates_inclusion_of(errors, msg, channel,
        ((int[]){ZMSG_CHANNEL_ONCHAIN, ZMSG_CHANNEL_P2P}), 2);
    validates_non_negative(errors, msg, timestamp);
    validates_custom(errors,
        strnlen(msg->body, ZMSG_MAX_BODY + 1) <= ZMSG_MAX_BODY,
        "body", "exceeds ZMSG_MAX_BODY");

    return !ar_errors_any(errors);
}

bool db_zmsg_save(struct node_db *ndb, const struct zmsg_message *msg)
{
    if (!ndb || !ndb->open) LOG_FAIL("zmsg", "db_zmsg_save: db not open");
    if (!msg) LOG_FAIL("zmsg", "db_zmsg_save: msg is NULL");

    struct ar_callbacks *cbs = db_zmsg_callbacks();
    sqlite3_stmt *s = NULL;
    static const uint8_t zero[32] = {0};
    AR_ADHOC_SAVE(ndb, s,
        "INSERT OR IGNORE INTO zmsg_messages"
        "(msg_id,direction,channel,sender,recipient,body,"
        "timestamp,txid,read)"
        " VALUES(?,?,?,?,?,?,?,?,?)",
        cbs, "zmsg", msg, db_zmsg_validate,
        AR_BIND_BLOB(s, 1, msg->msg_id, 32);
        AR_BIND_INT(s, 2, msg->direction);
        AR_BIND_INT(s, 3, msg->channel);
        AR_BIND_TEXT(s, 4, msg->sender);
        AR_BIND_TEXT(s, 5, msg->recipient);
        AR_BIND_TEXT(s, 6, msg->body);
        AR_BIND_INT(s, 7, msg->timestamp);
        if (memcmp(msg->txid, zero, 32) != 0)
            AR_BIND_BLOB(s, 8, msg->txid, 32);
        else
            AR_BIND_NULL(s, 8);
        AR_BIND_INT(s, 9, msg->read ? 1 : 0));
}

static void row_to_zmsg(sqlite3_stmt *s, struct zmsg_message *out)
{
    memset(out, 0, sizeof(*out));
    AR_READ_BLOB(s, 0, out->msg_id, 32);

    out->direction = sqlite3_column_int(s, 1);
    out->channel = sqlite3_column_int(s, 2);

    const char *str = (const char *)sqlite3_column_text(s, 3);
    if (str) snprintf(out->sender, sizeof(out->sender), "%s", str);

    str = (const char *)sqlite3_column_text(s, 4);
    if (str) snprintf(out->recipient, sizeof(out->recipient), "%s", str);

    str = (const char *)sqlite3_column_text(s, 5);
    if (str) snprintf(out->body, sizeof(out->body), "%s", str);

    out->timestamp = sqlite3_column_int64(s, 6);

    AR_READ_BLOB(s, 7, out->txid, 32);

    out->read = sqlite3_column_int(s, 8) != 0;
}

int db_zmsg_list(struct node_db *ndb, struct zmsg_message *out,
                 size_t max, bool unread_only)
{
    if (!ndb || !ndb->open) return 0;
    if (!out && max > 0)
        LOG_RETURN(0, "zmsg", "db_zmsg_list: out is NULL");

    const char *sql = unread_only
        ? "SELECT msg_id,direction,channel,sender,recipient,body,"
          "timestamp,txid,read FROM zmsg_messages "
          "WHERE read=0 ORDER BY timestamp DESC LIMIT ?"
        : "SELECT msg_id,direction,channel,sender,recipient,body,"
          "timestamp,txid,read FROM zmsg_messages "
          "ORDER BY timestamp DESC LIMIT ?";

    sqlite3_stmt *s = NULL;
    AR_QUERY_LIST(ndb, s, sql, out, max,
        AR_BIND_INT(s, 1, (int)max),
        row_to_zmsg(s, &out[count]));
}

bool db_zmsg_mark_read(struct node_db *ndb, const uint8_t msg_id[32])
{
    if (!ndb || !ndb->open) LOG_FAIL("zmsg", "db_zmsg_mark_read: db not open");
    if (!msg_id) LOG_FAIL("zmsg", "db_zmsg_mark_read: msg_id is NULL");

    sqlite3_stmt *s = NULL;
    AR_EXEC_BOOL(ndb, s,
        "UPDATE zmsg_messages SET read=1 WHERE msg_id=?",
        AR_BIND_BLOB(s, 1, msg_id, 32));
}
