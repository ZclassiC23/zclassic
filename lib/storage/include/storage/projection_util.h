/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * projection_util — tiny stateless helpers shared by the event-log
 * projection consumers (the *_projection.c files under lib/storage/src).
 *
 * Centralizes the small helpers shared by utxo, peers, mempool, znam, wallet,
 * contacts, hodl_history, and onion_announcements projections so those
 * consumers share one implementation.
 *
 * They are `static inline` (not plain `static`): the app/lib build runs
 * with `-Wall -Wextra -Werror`, and `bounded_strlen` is only called in
 * three of the eight including TUs. A plain `static` helper that some TU
 * does not call would trip `-Wunused-function` and break the build;
 * `static inline` suppresses that.
 *
 * NOTE on `exec_sql`: it is intentionally NOT shared. Each projection
 * keeps its own `static bool exec_sql(sqlite3*, const char*, const char*)`
 * so its diagnostic log lines stay tagged with that module's prefix
 * (e.g. "[utxo_projection] ..."). `apply_pragmas` below calls it, so
 * this header forward-declares the prototype; every including TU supplies
 * the matching definition.
 *
 * Divergent siblings that must keep their LOCAL copies and do NOT include
 * this header: block_index_projection.c (mono_now_ms, 5-pragma loop
 * apply_pragmas, 3-arg meta_get_u64/set_u64), coins_view_projection.c,
 * progress_store.c.
 */

#ifndef ZCL_STORAGE_PROJECTION_UTIL_H
#define ZCL_STORAGE_PROJECTION_UTIL_H

#include "platform/time_compat.h"

#include <inttypes.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Each projection defines its own module-tagged exec_sql; apply_pragmas
 * below calls it, so declare the prototype here. */
static bool exec_sql(sqlite3 *db, const char *sql, const char *ctx);

static inline int64_t now_ms(void)
{
    struct timespec ts;
    platform_time_monotonic_timespec(&ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline bool apply_pragmas(sqlite3 *db)
{
    return exec_sql(db, "PRAGMA journal_mode=WAL", "journal_mode") &&
           exec_sql(db, "PRAGMA synchronous=NORMAL", "synchronous") &&
           exec_sql(db, "PRAGMA busy_timeout=5000", "busy_timeout");
}

static inline uint64_t meta_get_u64(sqlite3 *db, const char *key)
{
    sqlite3_stmt *s = NULL;
    uint64_t v = 0;
    int rc = sqlite3_prepare_v2(db,
        "SELECT v FROM projection_meta WHERE k=?",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return 0;
    sqlite3_bind_text(s, 1, key, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    if (rc == SQLITE_ROW) {
        const unsigned char *txt = sqlite3_column_text(s, 0);
        if (txt) v = (uint64_t)strtoull((const char *)txt, NULL, 10);
    }
    sqlite3_finalize(s);
    return v;
}

static inline bool meta_set_u64(sqlite3 *db, const char *key, uint64_t value)
{
    sqlite3_stmt *s = NULL;
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRIu64, value);
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO projection_meta(k,v) VALUES(?,?)",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, buf, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

static inline size_t bounded_strlen(const char *s, size_t max)
{
    if (!s) return 0;
    size_t n = 0;
    while (n <= max && s[n] != '\0')
        n++;
    return n;
}

#endif /* ZCL_STORAGE_PROJECTION_UTIL_H */
