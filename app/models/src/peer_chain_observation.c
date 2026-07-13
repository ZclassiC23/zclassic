/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * peer_chain_observation model — see models/peer_chain_observation.h. Retained,
 * bounded per-peer chain-intelligence history written by the network monitor.
 * Observational only; never read by consensus. */

#include "models/peer_chain_observation.h"
#include "models/model_text.h"
#include "platform/time_compat.h"
#include "util/log_macros.h"

#include <string.h>

DEFINE_MODEL_CALLBACKS(peer_chain_observation)

static bool peer_chain_observation_before_validate(void *record, void *ctx)
{
    struct db_peer_chain_observation *o = record;
    (void)ctx;
    if (!o)
        return false;
    model_trim_ascii(o->addr);
    model_trim_ascii(o->user_agent);
    model_trim_ascii(o->tip_hash);
    model_ascii_downcase(o->tip_hash);
    return true;
}

static struct ar_callbacks *peer_chain_observation_callbacks_ready(void)
{
    struct ar_callbacks *cbs = db_peer_chain_observation_callbacks();
    static bool hooks_done = false;
    if (!hooks_done) {
        ar_register_before_validate(cbs, peer_chain_observation_before_validate);
        hooks_done = true;
    }
    return cbs;
}

bool db_peer_chain_observation_validate(const struct db_peer_chain_observation *o,
                                        struct ar_errors *errors)
{
    ar_errors_clear(errors);
    if (!o) {
        validates_custom(errors, false, "record", "null observation");
        return !ar_errors_any(errors);
    }
    validates_non_negative(errors, o, observed_at);
    validates_custom(errors, strlen(o->addr) <= PEER_OBS_ADDR_MAX,
                     "addr", "exceeds max length");
    validates_custom(errors, strlen(o->user_agent) <= PEER_OBS_UA_MAX,
                     "user_agent", "exceeds max length");
    validates_custom(errors, strlen(o->tip_hash) <= PEER_OBS_TIP_HEX,
                     "tip_hash", "exceeds max length");
    validates_custom(errors,
                     model_string_is_printable(o->addr),
                     "addr", "contains non-printable characters");
    validates_custom(errors,
                     model_string_is_printable(o->user_agent),
                     "user_agent", "contains non-printable characters");
    validates_custom(errors,
                     o->tip_hash[0] == '\0' || model_string_is_printable(o->tip_hash),
                     "tip_hash", "contains non-printable characters");
    return !ar_errors_any(errors);
}

bool db_peer_chain_observation_save(struct node_db *ndb,
                                    const struct db_peer_chain_observation *o)
{
    sqlite3_stmt *s = NULL;
    struct ar_callbacks *cbs;

    if (!ndb || !ndb->open || !o) {
        LOG_FAIL("model", "db_peer_chain_observation_save: bad args");
    }
    if (o->observed_at == 0)
        ((struct db_peer_chain_observation *)o)->observed_at =
            (int64_t)platform_time_wall_time_t();

    cbs = peer_chain_observation_callbacks_ready();
    AR_ADHOC_SAVE(ndb, s,
        "INSERT INTO peer_chain_observations "
        "(peer_id,addr,user_agent,version,best_height,tip_hash,"
        "latency_us,inbound,first_seen,last_seen,observed_at) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?)",
        cbs, "peer_chain_observation", o, db_peer_chain_observation_validate,
        AR_BIND_INT(s, 1, o->peer_id);
        AR_BIND_TEXT(s, 2, o->addr);
        AR_BIND_TEXT(s, 3, o->user_agent);
        AR_BIND_INT(s, 4, o->version);
        AR_BIND_INT(s, 5, o->best_height);
        AR_BIND_TEXT(s, 6, o->tip_hash);
        AR_BIND_INT(s, 7, o->latency_us);
        AR_BIND_INT(s, 8, o->inbound);
        AR_BIND_INT(s, 9, o->first_seen);
        AR_BIND_INT(s, 10, o->last_seen);
        AR_BIND_INT(s, 11, o->observed_at));
}

bool db_peer_chain_observation_prune(struct node_db *ndb, int keep_rows)
{
    sqlite3_stmt *s = NULL;
    if (!ndb || !ndb->open) {
        LOG_FAIL("model", "db_peer_chain_observation_prune: bad args");
    }
    if (keep_rows < 0)
        keep_rows = 0;
    AR_EXEC_BOOL(ndb, s,
        "DELETE FROM peer_chain_observations WHERE id NOT IN "
        "(SELECT id FROM peer_chain_observations ORDER BY id DESC LIMIT ?)",
        AR_BIND_INT(s, 1, keep_rows));
}

int db_peer_chain_observation_count(struct node_db *ndb)
{
    if (!ndb || !ndb->open)
        return 0;
    AR_QUERY_COUNT_SQL(ndb, "SELECT COUNT(*) FROM peer_chain_observations");
}

int db_peer_chain_observation_recent(struct node_db *ndb,
                                     struct db_peer_chain_observation *out,
                                     size_t max)
{
    sqlite3_stmt *s = NULL;
    if (!ndb || !ndb->open || !out || max == 0)
        return 0;
    AR_QUERY_LIST(ndb, s,
        "SELECT peer_id,addr,user_agent,version,best_height,tip_hash,"
        "latency_us,inbound,first_seen,last_seen,observed_at "
        "FROM peer_chain_observations ORDER BY id DESC LIMIT ?",
        out, max,
        AR_BIND_INT(s, 1, (int)max),
        out[count].peer_id = AR_COL_INT(s, 0);
        AR_READ_STR(s, 1, out[count].addr, sizeof(out[count].addr));
        AR_READ_STR(s, 2, out[count].user_agent, sizeof(out[count].user_agent));
        out[count].version = (int)AR_COL_INT(s, 3);
        out[count].best_height = AR_COL_INT(s, 4);
        AR_READ_STR(s, 5, out[count].tip_hash, sizeof(out[count].tip_hash));
        out[count].latency_us = AR_COL_INT(s, 6);
        out[count].inbound = (int)AR_COL_INT(s, 7);
        out[count].first_seen = AR_COL_INT(s, 8);
        out[count].last_seen = AR_COL_INT(s, 9);
        out[count].observed_at = AR_COL_INT(s, 10));
}
