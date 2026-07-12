/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Auth-challenge model (single-use login nonce). See auth_challenge.h. The
 * consume is one atomic UPDATE guarded by sqlite3_changes(), so a nonce can be
 * spent at most once. All writes go through the AR lifecycle. */

#include "util/log_macros.h"
#include "models/auth_challenge.h"
#include "config/runtime.h"
#include "json/json.h"
#include <string.h>

DEFINE_MODEL_CALLBACKS(auth_challenge)

static bool auth_challenge_is_hex(const char *s)
{
    if (!s || !s[0])
        return false;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

bool db_auth_challenge_validate(const struct db_auth_challenge *c,
                                struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_string_present(errors, c->nonce_hex, "nonce_hex");
    validates_string_present(errors, c->address, "address");
    validates_custom(errors, auth_challenge_is_hex(c->nonce_hex),
        "nonce_hex", "is not a hex string");
    validates_custom(errors, strlen(c->nonce_hex) <= AUTH_CHALLENGE_NONCE_HEX_MAX,
        "nonce_hex", "exceeds max length");
    validates_custom(errors, strlen(c->address) <= AUTH_CHALLENGE_ADDRESS_MAX,
        "address", "exceeds max length");
    validates_non_negative(errors, c, issued_at);
    validates_custom(errors, c->expires_at > c->issued_at,
        "expires_at", "must be after issued_at");
    return !ar_errors_any(errors);
}

bool db_auth_challenge_save(struct node_db *ndb,
                            const struct db_auth_challenge *c)
{
    sqlite3_stmt *s = NULL;
    struct ar_callbacks *cbs;
    if (!ndb || !ndb->open || !c) {
        LOG_FAIL("model", "db_auth_challenge_save: bad args");
        return false;
    }
    cbs = db_auth_challenge_callbacks();
    AR_ADHOC_SAVE(ndb, s,
        "INSERT OR REPLACE INTO auth_challenges "
        "(nonce_hex,address,issued_at,expires_at,consumed) "
        "VALUES (?,?,?,?,?)",
        cbs, "auth_challenge", c, db_auth_challenge_validate,
        AR_BIND_TEXT(s, 1, c->nonce_hex);
        AR_BIND_TEXT(s, 2, c->address);
        AR_BIND_INT(s, 3, c->issued_at);
        AR_BIND_INT(s, 4, c->expires_at);
        AR_BIND_INT(s, 5, c->consumed ? 1 : 0));
}

bool db_auth_challenge_find(struct node_db *ndb, const char *nonce_hex,
                            struct db_auth_challenge *out)
{
    sqlite3_stmt *s = NULL;
    if (!ndb || !ndb->open || !nonce_hex || !out) {
        LOG_FAIL("model", "db_auth_challenge_find: bad args");
        return false;
    }
    AR_QUERY_ONE_BOOL(ndb, s,
        "SELECT nonce_hex,address,issued_at,expires_at,consumed "
        "FROM auth_challenges WHERE nonce_hex=?",
        AR_BIND_TEXT(s, 1, nonce_hex),
        AR_READ_STR(s, 0, out->nonce_hex, sizeof(out->nonce_hex));
        AR_READ_STR(s, 1, out->address, sizeof(out->address));
        out->issued_at = AR_COL_INT(s, 2);
        out->expires_at = AR_COL_INT(s, 3);
        out->consumed = AR_COL_INT(s, 4) != 0);
}

bool db_auth_challenge_consume(struct node_db *ndb, const char *nonce_hex,
                               const char *address, int64_t now)
{
    sqlite3_stmt *s = NULL;
    if (!ndb || !ndb->open || !nonce_hex || !address) {
        LOG_FAIL("model", "db_auth_challenge_consume: bad args");
        return false;
    }
    AR_EXEC_CHANGED_BOOL(ndb, s,
        "UPDATE auth_challenges SET consumed=1 "
        "WHERE nonce_hex=? AND address=? AND consumed=0 AND expires_at>?",
        AR_BIND_TEXT(s, 1, nonce_hex);
        AR_BIND_TEXT(s, 2, address);
        AR_BIND_INT(s, 3, now));
}

int db_auth_challenge_reap(struct node_db *ndb, int64_t cutoff)
{
    sqlite3_stmt *s = NULL;
    if (!ndb || !ndb->open)
        return 0;
    if (sqlite3_prepare_v2(ndb->db,
            "DELETE FROM auth_challenges "
            "WHERE issued_at < ? OR consumed=1",
            -1, &s, NULL) != SQLITE_OK || !s) {
        LOG_WARN("model", "db_auth_challenge_reap: prepare failed");
        return 0;
    }
    AR_BIND_INT(s, 1, cutoff);
    int removed = 0;
    if (AR_STEP_DONE(s))
        removed = sqlite3_changes(ndb->db);
    AR_FINALIZE(s);
    return removed;
}

int db_auth_challenge_pending_count(struct node_db *ndb)
{
    if (!ndb || !ndb->open)
        return 0;
    AR_QUERY_COUNT_SQL(ndb,
        "SELECT COUNT(*) FROM auth_challenges WHERE consumed=0");
}

bool auth_challenge_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    struct node_db *ndb = app_runtime_node_db();
    json_push_kv_bool(out, "db_open", ndb && ndb->open);
    json_push_kv_int(out, "pending",
                     ndb && ndb->open ? db_auth_challenge_pending_count(ndb) : 0);
    return true;
}
