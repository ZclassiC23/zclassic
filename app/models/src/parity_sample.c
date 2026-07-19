/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * parity_sample model — see models/parity_sample.h. Retained, bounded
 * consensus-parity comparison history written by legacy_mirror_sync. */

#include "models/parity_sample.h"
#include "platform/time_compat.h"
#include "util/log_macros.h"

DEFINE_MODEL_CALLBACKS(parity_sample)

bool db_parity_sample_validate(const struct db_parity_sample *s,
                               struct ar_errors *errors)
{
    ar_errors_clear(errors);
    if (!s) {
        validates_custom(errors, false, "record", "null sample");
        return !ar_errors_any(errors);
    }
    validates_non_negative(errors, s, ts);
    validates_custom(errors, s->hash_equal == 0 || s->hash_equal == 1,
                     "hash_equal", "must be 0 or 1");
    validates_custom(errors, s->oracle_reachable == 0 || s->oracle_reachable == 1,
                     "oracle_reachable", "must be 0 or 1");
    return !ar_errors_any(errors);
}

bool db_parity_sample_save(struct node_db *ndb,
                           const struct db_parity_sample *s)
{
    sqlite3_stmt *stmt = NULL;

    if (!ndb || !ndb->open || !s) {
        LOG_FAIL("model", "db_parity_sample_save: bad args");
    }
    if (s->ts == 0)
        ((struct db_parity_sample *)s)->ts =
            (int64_t)platform_time_wall_unix();

    AR_ADHOC_SAVE(ndb, stmt,
        "INSERT INTO parity_samples "
        "(ts,our_height,oracle_height,heights_equal_at,hash_equal,"
        "oracle_reachable) VALUES (?,?,?,?,?,?)",
        db_parity_sample_callbacks(), "parity_sample", s,
        db_parity_sample_validate,
        AR_BIND_INT(stmt, 1, s->ts);
        AR_BIND_INT(stmt, 2, s->our_height);
        AR_BIND_INT(stmt, 3, s->oracle_height);
        AR_BIND_INT(stmt, 4, s->heights_equal_at);
        AR_BIND_INT(stmt, 5, s->hash_equal);
        AR_BIND_INT(stmt, 6, s->oracle_reachable));
}

bool db_parity_sample_prune(struct node_db *ndb, int keep_rows)
{
    sqlite3_stmt *stmt = NULL;
    if (!ndb || !ndb->open) {
        LOG_FAIL("model", "db_parity_sample_prune: bad args");
    }
    if (keep_rows < 0)
        keep_rows = 0;
    AR_EXEC_BOOL(ndb, stmt,
        "DELETE FROM parity_samples WHERE id NOT IN "
        "(SELECT id FROM parity_samples ORDER BY id DESC LIMIT ?)",
        AR_BIND_INT(stmt, 1, keep_rows));
}

int db_parity_sample_count(struct node_db *ndb)
{
    if (!ndb || !ndb->open)
        return 0;
    AR_QUERY_COUNT_SQL(ndb, "SELECT COUNT(*) FROM parity_samples");
}

int db_parity_sample_recent(struct node_db *ndb,
                            struct db_parity_sample *out, size_t max)
{
    sqlite3_stmt *stmt = NULL;
    if (!ndb || !ndb->open || !out || max == 0)
        return 0;
    AR_QUERY_LIST(ndb, stmt,
        "SELECT ts,our_height,oracle_height,heights_equal_at,hash_equal,"
        "oracle_reachable FROM parity_samples ORDER BY id DESC LIMIT ?",
        out, max,
        AR_BIND_INT(stmt, 1, (int)max),
        out[count].ts = AR_COL_INT(stmt, 0);
        out[count].our_height = AR_COL_INT(stmt, 1);
        out[count].oracle_height = AR_COL_INT(stmt, 2);
        out[count].heights_equal_at = AR_COL_INT(stmt, 3);
        out[count].hash_equal = (int)AR_COL_INT(stmt, 4);
        out[count].oracle_reachable = (int)AR_COL_INT(stmt, 5));
}
