/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: persist encrypted, idempotent transaction-intent state. */

#include "models/vault_intent.h"

#include "models/database.h"
#include "models/model_text.h"
#include "util/log_macros.h"

#include <string.h>

DEFINE_MODEL_CALLBACKS(vault_intent)
DEFINE_MODEL_CALLBACKS(vault_intent_raw)

struct vault_intent_raw_row {
    const uint8_t *plan_id;
    const uint8_t *raw_tx;
    size_t raw_tx_len;
};

static bool vault_intent_raw_validate(const struct vault_intent_raw_row *r,
                                      struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_custom(errors, r && r->plan_id, "plan_id", "is absent");
    validates_custom(errors, r && r->raw_tx && r->raw_tx_len > 0 &&
                     r->raw_tx_len <= VAULT_INTENT_RAW_MAX, "raw_tx",
                     "has invalid length");
    return !ar_errors_any(errors);
}

bool vault_intent_validate(const struct vault_intent_row *r,
                           struct ar_errors *errors)
{
    ar_errors_clear(errors);
    if (!r) {
        ar_errors_add(errors, "intent", "is NULL");
        return false; // raw-return-ok:null record cannot be validated
    }
    validates_presence_of(errors, r, plan_id);
    validates_presence_of(errors, r, digest);
    validates_range(errors, r, state, VAULT_INTENT_PLANNED,
                    VAULT_INTENT_FAILED);
    validates_range(errors, r, route, VAULT_INTENT_ROUTE_PRIVATE,
                    VAULT_INTENT_ROUTE_MIXED);
    validates_non_negative(errors, r, created_at);
    validates_custom(errors, r->expires_at > r->created_at, "expires_at",
                     "must be after created_at");
    validates_non_negative(errors, r, anchor_height);
    validates_presence_of(errors, r, anchor_hash);
    validates_custom(errors, r->encrypted_payload_len >= 32 &&
                     r->encrypted_payload_len <= VAULT_INTENT_PAYLOAD_MAX,
                     "encrypted_payload", "has invalid length");
    validates_custom(errors, strlen(r->error_code) <= VAULT_INTENT_ERROR_MAX &&
                     (r->error_code[0] == '\0' ||
                      model_string_is_printable(r->error_code)), "error_code",
                     "is invalid");
    return !ar_errors_any(errors);
}

bool vault_intent_save(struct node_db *ndb, const struct vault_intent_row *r)
{
    if (!ndb || !ndb->open || !r)
        LOG_FAIL("vault_intent", "save: invalid argument");
    sqlite3_stmt *s = NULL;
    AR_ADHOC_SAVE(ndb, s,
        "INSERT OR REPLACE INTO vault_intents"
        "(plan_id,digest,state,route,created_at,expires_at,anchor_height,"
        "anchor_hash,encrypted_payload,txid,confirm_height,confirm_hash,"
        "error_code,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        db_vault_intent_callbacks(), "vault_intent", r,
        vault_intent_validate,
        AR_BIND_BLOB(s, 1, r->plan_id, 32);
        AR_BIND_BLOB(s, 2, r->digest, 32);
        AR_BIND_INT(s, 3, r->state);
        AR_BIND_INT(s, 4, r->route);
        AR_BIND_INT(s, 5, r->created_at);
        AR_BIND_INT(s, 6, r->expires_at);
        AR_BIND_INT(s, 7, r->anchor_height);
        AR_BIND_BLOB(s, 8, r->anchor_hash, 32);
        AR_BIND_BLOB(s, 9, r->encrypted_payload, r->encrypted_payload_len);
        if (r->has_txid) AR_BIND_BLOB(s, 10, r->txid, 32);
        else AR_BIND_NULL(s, 10);
        AR_BIND_INT(s, 11, r->confirm_height);
        if (r->has_confirm_hash) AR_BIND_BLOB(s, 12, r->confirm_hash, 32);
        else AR_BIND_NULL(s, 12);
        AR_BIND_TEXT(s, 13, r->error_code);
        AR_BIND_INT(s, 14, r->updated_at));
}

static void intent_read(struct vault_intent_row *r, sqlite3_stmt *s)
{
    memset(r, 0, sizeof(*r));
    AR_READ_BLOB(s, 0, r->plan_id, 32);
    AR_READ_BLOB(s, 1, r->digest, 32);
    r->state = (enum vault_intent_state)AR_COL_INT(s, 2);
    r->route = (enum vault_intent_route)AR_COL_INT(s, 3);
    r->created_at = AR_COL_INT(s, 4);
    r->expires_at = AR_COL_INT(s, 5);
    r->anchor_height = (int32_t)AR_COL_INT(s, 6);
    AR_READ_BLOB(s, 7, r->anchor_hash, 32);
    int plen = AR_COL_BYTES(s, 8);
    if (plen > 0 && plen <= VAULT_INTENT_PAYLOAD_MAX) {
        AR_READ_BLOB(s, 8, r->encrypted_payload, (size_t)plen);
        r->encrypted_payload_len = (size_t)plen;
    }
    if (AR_COL_BYTES(s, 9) == 32) {
        AR_READ_BLOB(s, 9, r->txid, 32);
        r->has_txid = true;
    }
    r->confirm_height = (int32_t)AR_COL_INT(s, 10);
    if (AR_COL_BYTES(s, 11) == 32) {
        AR_READ_BLOB(s, 11, r->confirm_hash, 32);
        r->has_confirm_hash = true;
    }
    AR_READ_STR(s, 12, r->error_code, sizeof(r->error_code));
    r->updated_at = AR_COL_INT(s, 13);
}

#define INTENT_COLUMNS "plan_id,digest,state,route,created_at,expires_at," \
    "anchor_height,anchor_hash,encrypted_payload,txid,confirm_height," \
    "confirm_hash,error_code,updated_at"

bool vault_intent_find(struct node_db *ndb, const uint8_t plan_id[32],
                       struct vault_intent_row *out)
{
    if (!ndb || !ndb->open || !plan_id || !out)
        LOG_FAIL("vault_intent", "find: invalid argument");
    sqlite3_stmt *s = NULL;
    AR_QUERY_ONE_BOOL(ndb, s,
        "SELECT " INTENT_COLUMNS " FROM vault_intents WHERE plan_id=?",
        AR_BIND_BLOB(s, 1, plan_id, 32), intent_read(out, s));
}

int vault_intent_list(struct node_db *ndb, struct vault_intent_row *out,
                      size_t max)
{
    if (!ndb || !ndb->open || (!out && max))
        LOG_ERR("vault_intent", "list: invalid argument");
    sqlite3_stmt *s = NULL;
    AR_QUERY_LIST(ndb, s,
        "SELECT " INTENT_COLUMNS " FROM vault_intents "
        "ORDER BY created_at DESC LIMIT 100", out, max, ;,
        intent_read(&out[count], s));
}

bool vault_intent_claim_commit(struct node_db *ndb,
                               const uint8_t plan_id[32], int64_t now_unix)
{
    if (!ndb || !ndb->open || !plan_id || now_unix < 0)
        LOG_FAIL("vault_intent", "claim: invalid argument");
    sqlite3_stmt *s = NULL;
    AR_EXEC_CHANGED_BOOL(ndb, s,
        "UPDATE vault_intents SET state=?,updated_at=? WHERE plan_id=? "
        "AND state=? AND expires_at>?",
        AR_BIND_INT(s, 1, VAULT_INTENT_PROVING);
        AR_BIND_INT(s, 2, now_unix);
        AR_BIND_BLOB(s, 3, plan_id, 32);
        AR_BIND_INT(s, 4, VAULT_INTENT_PLANNED);
        AR_BIND_INT(s, 5, now_unix));
}

bool vault_intent_reclaim_proving(struct node_db *ndb,
                                  const uint8_t plan_id[32],
                                  int64_t stale_before_unix,
                                  int64_t now_unix)
{
    if (!ndb || !ndb->open || !plan_id || stale_before_unix < 0 ||
        now_unix < stale_before_unix)
        LOG_FAIL("vault_intent", "reclaim: invalid argument");
    sqlite3_stmt *s = NULL;
    AR_EXEC_CHANGED_BOOL(ndb, s,
        "UPDATE vault_intents SET state=?,error_code='',updated_at=? "
        "WHERE plan_id=? AND state=? AND updated_at<=? AND NOT EXISTS "
        "(SELECT 1 FROM vault_intent_raw r WHERE r.plan_id=vault_intents.plan_id)",
        AR_BIND_INT(s, 1, VAULT_INTENT_PLANNED);
        AR_BIND_INT(s, 2, now_unix);
        AR_BIND_BLOB(s, 3, plan_id, 32);
        AR_BIND_INT(s, 4, VAULT_INTENT_PROVING);
        AR_BIND_INT(s, 5, stale_before_unix));
}

bool vault_intent_set_state(struct node_db *ndb, const uint8_t plan_id[32],
                            enum vault_intent_state state,
                            const uint8_t txid[32], const char *error_code,
                            int64_t now_unix)
{
    if (!ndb || !ndb->open || !plan_id || state < VAULT_INTENT_PLANNED ||
        state > VAULT_INTENT_FAILED || now_unix < 0)
        LOG_FAIL("vault_intent", "set_state: invalid argument");
    sqlite3_stmt *s = NULL;
    AR_EXEC_CHANGED_BOOL(ndb, s,
        "UPDATE vault_intents SET state=?,txid=?,error_code=?,updated_at=? "
        "WHERE plan_id=?",
        AR_BIND_INT(s, 1, state);
        if (txid) AR_BIND_BLOB(s, 2, txid, 32); else AR_BIND_NULL(s, 2);
        AR_BIND_TEXT(s, 3, error_code ? error_code : "");
        AR_BIND_INT(s, 4, now_unix);
        AR_BIND_BLOB(s, 5, plan_id, 32));
}

bool vault_intent_expire_due(struct node_db *ndb, int64_t now_unix)
{
    if (!ndb || !ndb->open || now_unix < 0)
        LOG_FAIL("vault_intent", "expire: invalid argument");
    sqlite3_stmt *s = NULL;
    AR_EXEC_BOOL(ndb, s,
        "UPDATE vault_intents SET state=?,error_code='PLAN_EXPIRED',"
        "updated_at=? WHERE state=? AND expires_at<=?",
        AR_BIND_INT(s, 1, VAULT_INTENT_EXPIRED);
        AR_BIND_INT(s, 2, now_unix);
        AR_BIND_INT(s, 3, VAULT_INTENT_PLANNED);
        AR_BIND_INT(s, 4, now_unix));
}

bool vault_intent_set_confirmation(
    struct node_db *ndb, const uint8_t plan_id[32],
    enum vault_intent_state state, int32_t confirm_height,
    const uint8_t confirm_hash[32], int64_t now_unix)
{
    if (!ndb || !ndb->open || !plan_id || !confirm_hash ||
        (state != VAULT_INTENT_CONFIRMED &&
         state != VAULT_INTENT_FINALIZED) || confirm_height < 0)
        LOG_FAIL("vault_intent", "set_confirmation: invalid argument");
    sqlite3_stmt *s = NULL;
    AR_EXEC_CHANGED_BOOL(ndb, s,
        "UPDATE vault_intents SET state=?,confirm_height=?,confirm_hash=?,"
        "error_code='',updated_at=? WHERE plan_id=?",
        AR_BIND_INT(s, 1, state);
        AR_BIND_INT(s, 2, confirm_height);
        AR_BIND_BLOB(s, 3, confirm_hash, 32);
        AR_BIND_INT(s, 4, now_unix);
        AR_BIND_BLOB(s, 5, plan_id, 32));
}

bool vault_intent_store_raw(struct node_db *ndb, const uint8_t plan_id[32],
                            const uint8_t *raw_tx, size_t raw_tx_len)
{
    if (!ndb || !ndb->open)
        LOG_FAIL("vault_intent", "store_raw: database unavailable");
    struct vault_intent_raw_row row = {
        .plan_id = plan_id, .raw_tx = raw_tx, .raw_tx_len = raw_tx_len
    };
    sqlite3_stmt *s = NULL;
    AR_ADHOC_SAVE(ndb, s,
        "INSERT OR REPLACE INTO vault_intent_raw(plan_id,raw_tx) VALUES(?,?)",
        db_vault_intent_raw_callbacks(), "vault_intent_raw", &row,
        vault_intent_raw_validate,
        AR_BIND_BLOB(s, 1, plan_id, 32);
        AR_BIND_BLOB(s, 2, raw_tx, raw_tx_len));
}

bool vault_intent_load_raw(struct node_db *ndb, const uint8_t plan_id[32],
                           uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!ndb || !ndb->open || !plan_id || !out || !out_len)
        LOG_FAIL("vault_intent", "load_raw: invalid argument");
    sqlite3_stmt *s = NULL;
    AR_PREPARE_BOOL(ndb, s,
        "SELECT raw_tx FROM vault_intent_raw WHERE plan_id=?");
    AR_BIND_BLOB(s, 1, plan_id, 32);
    if (!AR_STEP_ROW(s)) {
        AR_FINALIZE(s);
        return false; // raw-return-ok:no prepared transaction is a valid state
    }
    int n = AR_COL_BYTES(s, 0);
    if (n <= 0 || (size_t)n > out_cap) {
        AR_FINALIZE(s);
        LOG_FAIL("vault_intent", "load_raw: invalid size %d", n);
    }
    AR_READ_BLOB(s, 0, out, (size_t)n);
    *out_len = (size_t)n;
    AR_FINALIZE(s);
    return true;
}

bool vault_intent_has_raw(struct node_db *ndb, const uint8_t plan_id[32])
{
    if (!ndb || !ndb->open || !plan_id)
        LOG_FAIL("vault_intent", "has_raw: invalid argument");
    sqlite3_stmt *s = NULL;
    AR_QUERY_ONE_BOOL(ndb, s,
        "SELECT 1 FROM vault_intent_raw WHERE plan_id=?",
        AR_BIND_BLOB(s, 1, plan_id, 32), ;);
}

const char *vault_intent_state_name(enum vault_intent_state state)
{
    static const char *const names[] = {
        "planned", "proving", "mempool_accepted", "confirmed", "finalized",
        "reorged", "conflicted", "expired", "failed"
    };
    return state >= VAULT_INTENT_PLANNED && state <= VAULT_INTENT_FAILED
        ? names[state] : "failed";
}
